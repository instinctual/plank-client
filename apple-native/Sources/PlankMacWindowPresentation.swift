import AppKit

// Keep SwiftUI's window delegate in the chain. Only the desktop's full-screen
// presentation options are changed; normal window lifecycle stays with it.
@MainActor final class PlankMacWindowPresentation: NSObject, NSWindowDelegate, NSUserInterfaceValidations {
    private weak var window: NSWindow?
    private var afterFullScreenExit: (() -> Void)?
    private var requestingExit = false
    private var enteringFullScreen = false
    var changed: (() -> Void)?
    private var transitionStartedAt: TimeInterval?
    private let focusGraceSeconds: TimeInterval
    init(window: NSWindow, focusGraceSeconds: TimeInterval = 30) {
        self.window = window; self.focusGraceSeconds = focusGraceSeconds
        super.init()
        install()
    }
    var preservesFocus: Bool {
        guard isTransitioning, let transitionStartedAt else { return false }
        return ProcessInfo.processInfo.systemUptime - transitionStartedAt < focusGraceSeconds
    }
    // Objective-C may probe optional delegate selectors outside the UI actor.
    // This weak reference is only changed by install/remove on the main actor.
    nonisolated(unsafe) private weak var previous: (any NSWindowDelegate)?

    func install() {
        guard let window else { return }
        if window.delegate !== self {
            previous = window.delegate
            window.delegate = self
        }
        // SwiftUI/AppKit may replace or revalidate the fullscreen titlebar
        // controls without replacing this delegate. Repair those controls too.
        PlankMacDesktopWindow.configure(window)
    }
    func remove() {
        if let window, window.delegate === self {
            window.delegate = previous
            PlankMacDesktopWindow.configure(window)
        }
    }
    nonisolated override func responds(to selector: Selector!) -> Bool {
        super.responds(to: selector) || previous?.responds(to: selector) == true
    }
    nonisolated override func forwardingTarget(for selector: Selector!) -> Any? {
        if previous?.responds(to: selector) == true { return previous }
        return super.forwardingTarget(for: selector)
    }
    @objc func toggleDesktopFullScreen(_ sender: Any?) {
        if let window {
            NSLog("PLANK Mac fullscreen action: green window=%ld", window.windowNumber)
            PlankMacDesktopWindow.toggle(window, sender: sender)
        }
        else { NSLog("PLANK Mac fullscreen: green action has no desktop window") }
    }
    /// Do not dispose a secondary window while it owns a fullscreen Space.
    /// Its SwiftUI window remains alive until AppKit confirms the exit.
    func closeWhenWindowed(_ close: @escaping () -> Void) {
        guard let window, enteringFullScreen || window.styleMask.contains(.fullScreen) else { close(); return }
        afterFullScreenExit = close
        if !requestingExit && !enteringFullScreen {
            requestingExit = true
            PlankMacDesktopWindow.toggle(window)
        }
    }
    var isTransitioning: Bool { enteringFullScreen || requestingExit }
    var isFullScreenOrEntering: Bool { enteringFullScreen || window?.styleMask.contains(.fullScreen) == true }
    static let transitionFailed = Notification.Name("PLANKMacFullScreenTransitionFailed")

    func validateUserInterfaceItem(_ item: any NSValidatedUserInterfaceItem) -> Bool {
        if item.action == #selector(toggleDesktopFullScreen(_:)) { return window != nil }
        return (previous as? any NSUserInterfaceValidations)?.validateUserInterfaceItem(item) ?? true
    }
    func windowWillEnterFullScreen(_ notification: Notification) {
        enteringFullScreen = true
        beginFocusTransition()
        previous?.windowWillEnterFullScreen?(notification)
    }
    func windowWillExitFullScreen(_ notification: Notification) {
        requestingExit = true
        beginFocusTransition()
        previous?.windowWillExitFullScreen?(notification)
    }
    private func beginFocusTransition() {
        let started = ProcessInfo.processInfo.systemUptime
        transitionStartedAt = started; changed?()
        DispatchQueue.main.asyncAfter(deadline: .now() + focusGraceSeconds) { [weak self] in
            guard let self, transitionStartedAt == started, isTransitioning else { return }
            NSLog("PLANK Mac fullscreen focus: grace expired; normal focus policy resumes")
            changed?()
        }
    }
    private func repairFullScreenButton() {
        // AppKit/SwiftUI can revalidate controls after the did-change callback.
        // Rebind on the next UI turn using our reversible fullscreen action.
        DispatchQueue.main.async { [weak self] in
            guard let self, let window, window.delegate === self else { return }
            PlankMacDesktopWindow.configure(window)
        }
    }
    func windowDidEnterFullScreen(_ notification: Notification) {
        enteringFullScreen = false
        transitionStartedAt = nil; changed?()
        NSLog("PLANK Mac fullscreen: entered")
        previous?.windowDidEnterFullScreen?(notification)
        repairFullScreenButton()
        if let close = afterFullScreenExit { closeWhenWindowed(close) }
    }
    func windowDidExitFullScreen(_ notification: Notification) {
        NSLog("PLANK Mac fullscreen: exited")
        previous?.windowDidExitFullScreen?(notification)
        repairFullScreenButton()
        requestingExit = false
        transitionStartedAt = nil; changed?()
        let close = afterFullScreenExit; afterFullScreenExit = nil
        close?()
    }
    func windowDidFailToEnterFullScreen(_ window: NSWindow) {
        enteringFullScreen = false
        transitionStartedAt = nil; changed?()
        previous?.windowDidFailToEnterFullScreen?(window); repairFullScreenButton()
        if let close = afterFullScreenExit { afterFullScreenExit = nil; closeWhenWindowed(close) }
        NotificationCenter.default.post(name: Self.transitionFailed, object: window)
    }
    func windowDidFailToExitFullScreen(_ window: NSWindow) {
        previous?.windowDidFailToExitFullScreen?(window); repairFullScreenButton()
        requestingExit = false
        transitionStartedAt = nil; changed?()
        // Keep the window reachable if AppKit refuses the exit; do not leave
        // a detached Space. The operator can retry its normal exit control.
        NSLog("PLANK Mac fullscreen: disposal deferred because exit failed")
        NotificationCenter.default.post(name: Self.transitionFailed, object: window)
    }
    func window(_ window: NSWindow, willUseFullScreenPresentationOptions proposed: NSApplication.PresentationOptions) -> NSApplication.PresentationOptions {
        var options = previous?.window?(window, willUseFullScreenPresentationOptions: proposed) ?? proposed
        // AppKit requires auto-hidden menu bars to be paired with a hidden or
        // auto-hidden Dock. Auto-hide toolbar also requires fullScreen.
        options.remove([.hideMenuBar, .hideDock])
        options.formUnion([.fullScreen, .autoHideMenuBar, .autoHideDock, .autoHideToolbar])
        return options
    }
}
