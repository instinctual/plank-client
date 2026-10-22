import AppKit

/// Session focus is the union of its windows. A secondary surface never starts,
/// stops or duplicates the session's raw Wacom worker.
@MainActor enum PlankMacSessionWindows {
    private final class WindowRef {
        weak var window: NSWindow?
        weak var view: PlankMacInputView?
        init(_ window: NSWindow, view: PlankMacInputView?) { self.window = window; self.view = view }
    }
    @MainActor private final class Session {
        weak var client: PlankCoreClient?
        var windows: [UUID: WindowRef] = [:]
        var active: Bool?
        var closing = false
        let presentation = PlankMacSessionPresentation()
        init(_ client: PlankCoreClient) { self.client = client }
    }
    private static var sessions: [ObjectIdentifier: Session] = [:]
    static func attach(client: PlankCoreClient, surface: UUID, window: NSWindow?, view: PlankMacInputView? = nil) {
        let id = ObjectIdentifier(client)
        let session = sessions[id] ?? Session(client)
        sessions[id] = session
        if let window { session.windows[surface] = WindowRef(window, view: view) }
        else { session.windows.removeValue(forKey: surface) }
        refresh(client: client)
    }
    static func isPresenting(client: PlankCoreClient) -> Bool {
        sessions[ObjectIdentifier(client)]?.presentation.busy == true
    }
    static func isClosing(client: PlankCoreClient) -> Bool {
        sessions[ObjectIdentifier(client)]?.closing == true
    }
    static func toggleFullScreen(client: PlankCoreClient) {
        guard let session = sessions[ObjectIdentifier(client)], !session.closing else { return }
        session.presentation.changed = { [weak client] in if let client { refresh(client: client) } }
        session.presentation.toggle(windows: session.windows.values.compactMap(\.window))
    }
    static func disconnect(client: PlankCoreClient, dismiss: @escaping () -> Void) {
        guard let session = sessions[ObjectIdentifier(client)] else {
            client.disconnectSession(); dismiss(); return
        }
        guard !session.closing else { return }
        session.closing = true
        client.disconnectSession()
        session.presentation.changed = { [weak client] in if let client { refresh(client: client) } }
        session.presentation.windowed(windows: session.windows.values.compactMap(\.window)) { success in
            if success { dismiss() }
            session.closing = false // Failed Spaces remain reachable for retry.
            refresh(client: client)
        }
    }
    static func owns(client: PlankCoreClient, window: NSWindow?) -> Bool {
        guard let window, let session = sessions[ObjectIdentifier(client)] else { return false }
        return session.windows.values.contains { $0.window === window }
    }
    static func otherWindow(client: PlankCoreClient, excluding window: NSWindow?) -> NSWindow? {
        guard let window, let session = sessions[ObjectIdentifier(client)] else { return nil }
        return session.windows.values.compactMap(\.window).first { $0 !== window && $0.isVisible }
    }
    static func showSecondDisplay(client: PlankCoreClient, excluding window: NSWindow?, open: () -> Void) {
        if let existing = otherWindow(client: client, excluding: window) {
            NSLog("PLANK Mac second display: activate existing; fullscreen unchanged")
            existing.makeKeyAndOrderFront(nil)
        } else {
            NSLog("PLANK Mac second display: reopen; fullscreen unchanged")
            open()
        }
    }
    static func remotePointer(client: PlankCoreClient, screenPoint: CGPoint) -> (Int, Int, Int, Int)? {
        guard let session = sessions[ObjectIdentifier(client)] else { return nil }
        for entry in session.windows.values {
            if let result = entry.view?.remotePointer(at: screenPoint) { return result }
        }
        return nil
    }
    static func refresh(client: PlankCoreClient) {
        // Key-window notifications for a transfer arrive in a pair. Evaluate
        // after the transfer, so Wacom never sees a false focus gap.
        DispatchQueue.main.async {
            let id = ObjectIdentifier(client)
            guard let session = sessions[id] else { return }
            // Spaces can briefly leave neither desktop key/main. Retain an
            // already-owned tablet for this bounded operation, never across
            // app deactivation or disconnect.
            let moving = session.presentation.busy || session.windows.values.contains {
                ($0.window?.delegate as? PlankMacWindowPresentation)?.preservesFocus == true
            }
            let transitionOwnsFocus = moving && (NSApp.keyWindow == nil || owns(client: client, window: NSApp.keyWindow))
            let active = !session.closing && PlankMacSessionFocus.active(appActive: NSApp.isActive,
                windows: session.windows.values.map {
                    .init(key: $0.window?.isKeyWindow == true, main: $0.window?.isMainWindow == true)
                }, presentationInProgress: transitionOwnsFocus, previouslyActive: session.active == true)
            if session.active != active {
                NSLog("PLANK Mac tablet focus: active=%d app=%d key=%ld transition=%d closing=%d",
                    active ? 1 : 0, NSApp.isActive ? 1 : 0, NSApp.keyWindow?.windowNumber ?? -1,
                    transitionOwnsFocus ? 1 : 0, session.closing ? 1 : 0)
                session.active = active; client.setTabletActive(active)
            }
            if session.windows.isEmpty && !session.presentation.busy { sessions.removeValue(forKey: id) }
        }
    }
}


/// Toolbar commands have one target for all session windows. AppKit Space
/// transitions are serialized; green-button transitions finish before a new
/// target is applied. No transport or HID worker is recreated here.
@MainActor final class PlankMacSessionPresentation {
    private(set) var busy = false
    var changed: (() -> Void)?
    private var windows: [NSWindow] = []
    private var target = false
    private var index = 0
    private weak var waiting: NSWindow?
    private var generation = UUID()
    private var completion: ((Bool) -> Void)?
    private var observers: [NSObjectProtocol] = []
    private var deadline: DispatchWorkItem?
    private let timeoutSeconds: TimeInterval

    init(timeoutSeconds: TimeInterval = 30) {
        self.timeoutSeconds = timeoutSeconds
        for name in [NSWindow.didEnterFullScreenNotification, NSWindow.didExitFullScreenNotification,
                     PlankMacWindowPresentation.transitionFailed] {
            observers.append(NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { [weak self] event in
                guard let window = event.object as? NSWindow else { return }
                let windowID = ObjectIdentifier(window), name = event.name
                // Delegate flags settle in the same notification turn. Evaluate
                // afterwards; do not carry Notification's untyped payload across
                // the actor boundary.
                DispatchQueue.main.async { self?.transition(name: name, windowID: windowID) }
            })
        }
    }
    isolated deinit {
        deadline?.cancel()
        observers.forEach(NotificationCenter.default.removeObserver)
    }
    func toggle(windows: [NSWindow]) {
        guard !busy else { return } // Repeated clicks cannot reverse an animation.
        let anyFullScreen = windows.contains {
            ($0.delegate as? PlankMacWindowPresentation)?.isFullScreenOrEntering ?? $0.styleMask.contains(.fullScreen)
        }
        begin(windows: windows, target: !anyFullScreen, completion: nil)
    }
    func windowed(windows: [NSWindow], completion: @escaping (Bool) -> Void) {
        // Disconnect supersedes pending group entry. Finish the current
        // AppKit transition, then exit each Space once.
        begin(windows: windows, target: false, completion: completion)
    }
    private func begin(windows: [NSWindow], target: Bool, completion: ((Bool) -> Void)?) {
        deadline?.cancel()
        generation = UUID()
        var seen = Set<ObjectIdentifier>()
        self.windows = windows.filter { seen.insert(ObjectIdentifier($0)).inserted }
            .sorted { $0.windowNumber < $1.windowNumber }
        self.target = target; self.completion = completion; index = 0; waiting = nil
        busy = true; changed?()
        let ticket = generation
        let timeout = DispatchWorkItem { [weak self] in
            MainActor.assumeIsolated {
                guard let self, self.busy, self.generation == ticket else { return }
                NSLog("PLANK Mac session presentation: transition deadline exceeded; windows retained")
                self.finish(false)
            }
        }
        deadline = timeout
        DispatchQueue.main.asyncAfter(deadline: .now() + timeoutSeconds, execute: timeout)
        advance()
    }
    private func advance() {
        // A green-button entry/exit may already be underway on either display.
        if let moving = windows.first(where: {
            ($0.delegate as? PlankMacWindowPresentation)?.isTransitioning == true
        }) { waiting = moving; return }
        while index < windows.count {
            let window = windows[index]
            if (window.delegate as? PlankMacWindowPresentation)?.isTransitioning == true {
                waiting = window; return
            }
            if window.styleMask.contains(.fullScreen) == target { index += 1; continue }
            waiting = window
            PlankMacDesktopWindow.toggle(window)
            return
        }
        finish(true)
    }
    private func transition(name: Notification.Name, windowID: ObjectIdentifier) {
        guard busy, let waiting, ObjectIdentifier(waiting) == windowID else { return }
        if name == PlankMacWindowPresentation.transitionFailed {
            NSLog("PLANK Mac session presentation: AppKit transition failed; windows retained")
            finish(false); return
        }
        self.waiting = nil
        advance()
    }
    private func finish(_ success: Bool) {
        deadline?.cancel(); deadline = nil
        let callback = completion; completion = nil
        windows.removeAll(); waiting = nil; busy = false
        changed?(); callback?(success)
    }
}
