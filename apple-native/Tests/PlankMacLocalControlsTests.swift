import AppKit
import SwiftUI

// Only transport and video are substituted. The real AppKit input view,
// cursor overlay, and window delegate run without connecting to a Host.
@MainActor final class PlankCoreClient {
    var nativeMouseOwnsPointer = false
    var keys: [(UInt16, Bool)] = []
    var tabletChanges = 0
    var disconnects = 0
    func disconnectSession() { disconnects += 1 }
    func setTabletActive(_ active: Bool) { tabletChanges += 1 }
    func registerVideoSurface(id: UUID, frame: (PlankRenderedFrame?) -> Void,
        cursor: (PlankRemoteCursor?) -> Void, cursorShape: (PlankRemoteCursorShape?) -> Void) {}
    func unregisterVideoSurface(id: UUID) {}
    func movePointer(x: Int, y: Int, width: Int, height: Int) { nativeMouseOwnsPointer = true }
    func setMouseButton(number: UInt8, pressed: Bool) {}
    func scroll(vertical: Int16, horizontal: Int16) {}
    func sendKey(code: UInt16, pressed: Bool, modifiers: UInt8) { keys.append((code, pressed)) }
    func pressKey(code: UInt16) {}
}
final class PlankMacMetalView: NSView {
    var sourceCrop = CGRect(x: 0, y: 0, width: 1, height: 1)
    func display(_ buffer: CVPixelBuffer) {}
}
@MainActor final class OriginalDelegate: NSObject, NSWindowDelegate {
    var closes = 0
    func windowShouldClose(_ sender: NSWindow) -> Bool { closes += 1; return false }
    func window(_ window: NSWindow, willUseFullScreenPresentationOptions proposed: NSApplication.PresentationOptions) -> NSApplication.PresentationOptions {
        proposed.union([.disableProcessSwitching, .hideMenuBar, .hideDock])
    }
}
@MainActor final class FullScreenWindowDouble: NSWindow {
    var exitRequests = 0
    var simulatedFullScreen = false
    var simulatedVisible = false
    var frontRequests = 0
    override var isVisible: Bool { simulatedVisible }
    override func makeKeyAndOrderFront(_ sender: Any?) { frontRequests += 1 }
    override var styleMask: NSWindow.StyleMask {
        get { simulatedFullScreen ? super.styleMask.union(.fullScreen) : super.styleMask }
        set { super.styleMask = newValue.subtracting(.fullScreen); simulatedFullScreen = newValue.contains(.fullScreen) }
    }
    override func toggleFullScreen(_ sender: Any?) { exitRequests += 1 }
}
@main @MainActor struct PlankMacLocalControlsTests {
    static var checks = 0
    static func check(_ value: Bool, _ message: String) { checks += 1; precondition(value, message) }
    static func main() {
        _ = NSApplication.shared
        let window = NSWindow(contentRect: CGRect(x: 0, y: 0, width: 640, height: 360),
            styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        let original = OriginalDelegate()
        window.delegate = original
        let presentation = PlankMacWindowPresentation(window: window)
        check(window.delegate === presentation, "desktop delegate installed")
        var presentationChanges = 0
        presentation.changed = { presentationChanges += 1 }
        check(!presentation.preservesFocus, "stable window has no transition focus override")
        let green = window.standardWindowButton(.zoomButton)!
        check(green.target === presentation && green.action == #selector(PlankMacWindowPresentation.toggleDesktopFullScreen(_:)),
            "green button has reversible desktop fullscreen action")
        let fullscreenItem = NSMenuItem(title: "Full Screen", action: green.action, keyEquivalent: "")
        check(presentation.validateUserInterfaceItem(fullscreenItem), "green button validates independently of zoom size")
        check(presentation.validateUserInterfaceItem(NSMenuItem(title: "Other", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "")),
            "unrelated action validation remains available")
        let transition = Notification(name: NSWindow.willEnterFullScreenNotification, object: window)
        presentation.windowWillEnterFullScreen(transition)
        check(presentation.preservesFocus && presentationChanges == 1, "individual green entry starts focus grace and notifies session")
        check(presentation.validateUserInterfaceItem(fullscreenItem), "will-enter callback cannot latch exit action off")
        presentation.windowDidEnterFullScreen(Notification(name: NSWindow.didEnterFullScreenNotification, object: window))
        check(presentation.validateUserInterfaceItem(fullscreenItem), "completed fullscreen enables exit action")
        check(!presentation.preservesFocus && presentationChanges == 2, "entry completion ends grace and refreshes cursor/focus")
        presentation.windowWillExitFullScreen(Notification(name: NSWindow.willExitFullScreenNotification, object: window))
        check(presentation.preservesFocus, "individual green exit retains already-owned capture")
        presentation.windowDidFailToExitFullScreen(window)
        check(!presentation.preservesFocus && presentationChanges == 4, "failed exit clears focus grace and notifies session")
        check(presentation.validateUserInterfaceItem(fullscreenItem), "failed transition cannot leave fullscreen action disabled")
        window.styleMask.remove(.resizable)
        green.isEnabled = false
        PlankMacDesktopWindow.configure(window)
        check(window.styleMask.contains(.resizable) && green.isEnabled, "lost fullscreen capabilities repaired without transition latch")
        check(green.target === presentation && green.action == #selector(PlankMacWindowPresentation.toggleDesktopFullScreen(_:)), "repair retains shared fullscreen action")
        let options = window.delegate!.window!(window, willUseFullScreenPresentationOptions: [])
        green.isEnabled = false; green.target = window; green.action = #selector(NSWindow.performZoom(_:))
        presentation.install()
        check(green.isEnabled && green.target === presentation && green.action == #selector(PlankMacWindowPresentation.toggleDesktopFullScreen(_:)),
            "reinstall repairs revalidated green button even when delegate is unchanged")
        check(options.contains([.fullScreen, .autoHideMenuBar, .autoHideDock, .autoHideToolbar]), "fullscreen chrome auto-hides")
        check(!options.contains(.hideMenuBar) && !options.contains(.hideDock), "mutually exclusive options removed")
        check(options.contains(.disableProcessSwitching), "original delegate options preserved")
        check(window.delegate!.windowShouldClose!(window) == false && original.closes == 1, "optional lifecycle callback forwarded")
        presentation.remove()
        check(window.delegate === original, "previous window delegate restored")
        let replacement = OriginalDelegate()
        window.delegate = replacement
        presentation.install()
        check(window.delegate!.windowShouldClose!(window) == false && replacement.closes == 1, "later SwiftUI delegate preserved on reinstall")
        presentation.remove()
        check(window.delegate === replacement, "later delegate restored")

        let client = PlankCoreClient()
        let view = PlankMacInputView(client: client)
        let settings = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: [.command], timestamp: 0,
            windowNumber: 0, context: nil, characters: ",", charactersIgnoringModifiers: ",",
            isARepeat: false, keyCode: 43)!
        check(!view.performKeyEquivalent(with: settings) && client.keys.isEmpty, "Mac Settings shortcut stays local")
        let key = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: [], timestamp: 0,
            windowNumber: 0, context: nil, characters: "a", charactersIgnoringModifiers: "a",
            isARepeat: false, keyCode: 0)!
        view.keyDown(with: key)
        check(client.keys.count == 1 && client.keys[0].1, "desktop key pressed")
        NSCursor.crosshair.set()
        view.setLocalControlsPresented(true)
        check(client.keys.count == 2 && !client.keys[1].1, "opening controls releases held remote key")
        check(client.tabletChanges == 0, "opening controls leaves tablet ownership untouched")
        check(NSCursor.current === NSCursor.arrow, "local controls restore native arrow")
        check(!view.performKeyEquivalent(with: key), "local controls leave shortcuts to AppKit")
        view.flagsChanged(with: key)
        view.keyUp(with: key)
        check(client.keys.count == 2, "no remote input leaks through controls")
        view.setLocalControlsPresented(false)
        view.keyDown(with: key)
        check(client.keys.count == 3 && client.keys[2].1, "desktop keyboard resumes after controls close")
        view.releaseInput()
        check(client.keys.count == 4 && !client.keys[3].1, "resumed key releases normally")
        check(client.tabletChanges == 0, "control dismissal does not recapture tablet")

        let secondary = FullScreenWindowDouble(contentRect: CGRect(x: 0, y: 0, width: 640, height: 360),
            styleMask: [.titled, .resizable], backing: .buffered, defer: false)
        secondary.simulatedFullScreen = true
        let secondaryPresentation = PlankMacWindowPresentation(window: secondary)
        var disposed = 0
        secondaryPresentation.closeWhenWindowed { disposed += 1 }
        secondaryPresentation.closeWhenWindowed { disposed += 1 }
        check(disposed == 0 && secondary.exitRequests == 1, "secondary disposal waits for one fullscreen exit")
        secondary.styleMask.remove(.fullScreen)
        secondaryPresentation.windowDidExitFullScreen(Notification(name: NSWindow.didExitFullScreenNotification, object: secondary))
        check(disposed == 1, "secondary disposes once after fullscreen exit acknowledgement")
        secondaryPresentation.closeWhenWindowed { disposed += 1 }
        check(disposed == 2, "windowed secondary can dispose immediately")

        // A close requested during entry waits for entry to complete, then
        // requests one exit; a close during a user exit never toggles it back.
        secondaryPresentation.windowWillEnterFullScreen(Notification(name: NSWindow.willEnterFullScreenNotification, object: secondary))
        secondaryPresentation.closeWhenWindowed { disposed += 1 }
        check(disposed == 2 && secondary.exitRequests == 1, "entering fullscreen defers disposal without reversing entry")
        secondary.simulatedFullScreen = true
        secondaryPresentation.windowDidEnterFullScreen(Notification(name: NSWindow.didEnterFullScreenNotification, object: secondary))
        check(secondary.exitRequests == 2 && disposed == 2, "entry completion starts pending exit once")
        secondary.simulatedFullScreen = false
        secondaryPresentation.windowDidExitFullScreen(Notification(name: NSWindow.didExitFullScreenNotification, object: secondary))
        check(disposed == 3, "entry and exit complete before disposal")
        secondary.simulatedFullScreen = true
        secondaryPresentation.windowWillExitFullScreen(Notification(name: NSWindow.willExitFullScreenNotification, object: secondary))
        secondaryPresentation.closeWhenWindowed { disposed += 1 }
        check(secondary.exitRequests == 2 && disposed == 3, "normal exit is not reversed by disconnect")
        secondary.simulatedFullScreen = false
        secondaryPresentation.windowDidExitFullScreen(Notification(name: NSWindow.didExitFullScreenNotification, object: secondary))
        check(disposed == 4, "normal exit acknowledgement releases disposal")

        // Use the production group coordinator and window registry. Only
        // AppKit's asynchronous Space animation is substituted.
        func makeWindow() -> FullScreenWindowDouble {
            FullScreenWindowDouble(contentRect: CGRect(x: 0, y: 0, width: 640, height: 360),
                styleMask: [.titled, .resizable], backing: .buffered, defer: false)
        }
        func completed(_ window: FullScreenWindowDouble, _ full: Bool) {
            window.simulatedFullScreen = full
            let name = full ? NSWindow.didEnterFullScreenNotification : NSWindow.didExitFullScreenNotification
            let event = Notification(name: name, object: window)
            let delegate = window.delegate as! PlankMacWindowPresentation
            if full { delegate.windowDidEnterFullScreen(event) }
            else { delegate.windowDidExitFullScreen(event) }
            NotificationCenter.default.post(event)
            RunLoop.main.run(until: Date().addingTimeInterval(0.02))
        }
        let left = makeWindow(), right = makeWindow()
        let leftPresentation = PlankMacWindowPresentation(window: left)
        let rightPresentation = PlankMacWindowPresentation(window: right)
        let group = PlankMacSessionPresentation()
        group.toggle(windows: [left, right, left])
        let first = left.exitRequests == 1 ? left : right
        let second = first === left ? right : left
        check(group.busy && first.exitRequests == 1 && second.exitRequests == 0, "group enters one Space at a time and deduplicates windows")
        group.toggle(windows: [left, right])
        check(first.exitRequests == 1 && second.exitRequests == 0, "repeated toolbar press cannot reverse pending entry")
        completed(first, true)
        check(group.busy && second.exitRequests == 1, "second entry begins only after first acknowledges")
        completed(second, true)
        check(!group.busy && left.simulatedFullScreen && right.simulatedFullScreen, "toolbar enters both desktop windows")
        group.toggle(windows: [left, right])
        check(first.exitRequests == 2 && second.exitRequests == 1, "global exit serializes both Spaces")
        completed(first, false); completed(second, false)
        check(!group.busy && !left.simulatedFullScreen && !right.simulatedFullScreen, "toolbar returns both to windowed mode")
        left.simulatedFullScreen = true
        let leftCount = left.exitRequests, rightCount = right.exitRequests
        group.toggle(windows: [right, left])
        check(left.exitRequests == leftCount + 1 && right.exitRequests == rightCount, "mixed-state toolbar exits fullscreen member without entering windowed member")
        completed(left, false)
        check(!group.busy, "mixed-state exit completes")
        rightPresentation.toggleDesktopFullScreen(nil)
        check(right.exitRequests == rightCount + 1 && left.exitRequests == leftCount + 1 && !group.busy, "green button remains independent")

        leftPresentation.windowWillEnterFullScreen(Notification(name: NSWindow.willEnterFullScreenNotification, object: left))
        var closeCalls = 0
        group.windowed(windows: [left, right]) { if $0 { closeCalls += 1 } }
        check(left.exitRequests == leftCount + 1 && closeCalls == 0, "disconnect during green entry waits instead of reversing animation")
        completed(left, true)
        check(left.exitRequests == leftCount + 2 && closeCalls == 0, "entry acknowledgement starts required exit")
        completed(left, false)
        check(closeCalls == 1 && !group.busy, "close completion follows windowed acknowledgement")

        group.toggle(windows: [left, right])
        let failing = left.exitRequests > leftCount + 2 ? left : right
        (failing.delegate as! PlankMacWindowPresentation).windowDidFailToEnterFullScreen(failing)
        RunLoop.main.run(until: Date().addingTimeInterval(0.02))
        check(!group.busy, "AppKit failure releases coordinator for retry")
        left.simulatedFullScreen = true; right.simulatedFullScreen = true
        let groupClient = PlankCoreClient(), leftID = UUID(), rightID = UUID()
        PlankMacSessionWindows.attach(client: groupClient, surface: leftID, window: left)
        PlankMacSessionWindows.attach(client: groupClient, surface: rightID, window: right)
        var dismissCalls = 0
        PlankMacSessionWindows.disconnect(client: groupClient) { dismissCalls += 1 }
        PlankMacSessionWindows.disconnect(client: groupClient) { dismissCalls += 1 }
        check(groupClient.disconnects == 1 && dismissCalls == 0, "disconnect from either surface stops shared session exactly once")
        completed(first, false)
        check(dismissCalls == 0, "disconnect keeps both windows until both Spaces exit")
        completed(second, false)
        check(dismissCalls == 1, "disconnect dismisses both only after their exit acknowledgements")
        PlankMacSessionWindows.attach(client: groupClient, surface: leftID, window: nil)
        PlankMacSessionWindows.attach(client: groupClient, surface: rightID, window: nil)
        check(client.tabletChanges == 0, "presentation operations never directly recreate tablet capture")

        let unacknowledged = makeWindow()
        unacknowledged.simulatedFullScreen = true
        let unacknowledgedPresentation = PlankMacWindowPresentation(window: unacknowledged)
        let bounded = PlankMacSessionPresentation(timeoutSeconds: 0.01)
        var timeoutResult: Bool?
        bounded.windowed(windows: [unacknowledged]) { timeoutResult = $0 }
        RunLoop.main.run(until: Date().addingTimeInterval(0.04))
        check(!bounded.busy && timeoutResult == false && unacknowledged.simulatedFullScreen,
            "unacknowledged exit times out without destroying a fullscreen window")
        unacknowledgedPresentation.windowDidFailToExitFullScreen(unacknowledged)
        bounded.windowed(windows: []) { timeoutResult = $0 }
        check(!bounded.busy && timeoutResult == true, "deadline cannot latch future actions off")
        let single = makeWindow()
        let singlePresentation = PlankMacWindowPresentation(window: single)
        group.toggle(windows: [single])
        completed(single, true)
        check(!group.busy && single.simulatedFullScreen, "single-display toolbar still enters fullscreen")
        group.toggle(windows: [single]); completed(single, false)
        check(!group.busy && !single.simulatedFullScreen, "single-display toolbar still exits fullscreen")
        withExtendedLifetime(singlePresentation) {}

        check(PlankMacSessionFocus.active(appActive: true, windows: [], presentationInProgress: true, previouslyActive: true),
            "Space transition retains already-owned tablet through transient focus gap")
        check(!PlankMacSessionFocus.active(appActive: false, windows: [.init(key: true, main: true)], presentationInProgress: true, previouslyActive: true),
            "switching to another app releases tablet even during group transition")
        check(!PlankMacSessionFocus.active(appActive: true, windows: [], presentationInProgress: true, previouslyActive: false),
            "group transition cannot acquire an unowned tablet")
        check(!PlankMacSessionFocus.active(appActive: true, windows: [], presentationInProgress: false, previouslyActive: true),
            "finished transition cannot retain tablet after real focus loss")

        let reopenClient = PlankCoreClient(), primaryID = UUID(), secondaryID = UUID()
        let primaryWindow = makeWindow(), otherWindow = makeWindow()
        primaryWindow.simulatedVisible = true; otherWindow.simulatedVisible = true
        primaryWindow.simulatedFullScreen = true; otherWindow.simulatedFullScreen = false
        PlankMacSessionWindows.attach(client: reopenClient, surface: primaryID, window: primaryWindow)
        PlankMacSessionWindows.attach(client: reopenClient, surface: secondaryID, window: otherWindow)
        var reopenCalls = 0
        PlankMacSessionWindows.showSecondDisplay(client: reopenClient, excluding: primaryWindow) { reopenCalls += 1 }
        check(reopenCalls == 0 && otherWindow.frontRequests == 1, "existing second display is activated, never reopened")
        check(primaryWindow.simulatedFullScreen && !otherWindow.simulatedFullScreen && primaryWindow.exitRequests == 0 && otherWindow.exitRequests == 0,
            "show second display preserves both window presentation modes")
        PlankMacSessionWindows.attach(client: reopenClient, surface: secondaryID, window: nil)
        PlankMacSessionWindows.showSecondDisplay(client: reopenClient, excluding: primaryWindow) { reopenCalls += 1 }
        check(reopenCalls == 1 && primaryWindow.exitRequests == 0, "closed second display reopens without toggling main fullscreen")
        PlankMacSessionWindows.attach(client: reopenClient, surface: primaryID, window: nil)
        let stalledTransition = makeWindow()
        let shortGrace = PlankMacWindowPresentation(window: stalledTransition, focusGraceSeconds: 0.01)
        var graceChanges = 0
        shortGrace.changed = { graceChanges += 1 }
        shortGrace.windowWillEnterFullScreen(Notification(name: NSWindow.willEnterFullScreenNotification, object: stalledTransition))
        check(shortGrace.preservesFocus, "pending individual transition retains capture initially")
        RunLoop.main.run(until: Date().addingTimeInterval(0.04))
        check(!shortGrace.preservesFocus && graceChanges == 2, "unacknowledged individual transition releases focus override on bounded deadline")
        shortGrace.windowDidFailToEnterFullScreen(stalledTransition)

        // Full-screen toolbars overlap content without leaving its bounds.
        // Exercise the actual input view beneath a real native control.
        let root = NSView(frame: CGRect(x: 0, y: 0, width: 640, height: 360))
        view.frame = root.bounds
        root.addSubview(view)
        window.contentView = root
        view.display(PlankRenderedFrame(pixels: Data(count: 640 * 360 * 4), pixelBuffer: nil,
            width: 640, height: 360, bytesPerRow: 640 * 4, frameNumber: 1))
        view.layoutSubtreeIfNeeded()
        let center = view.convert(NSPoint(x: 320, y: 180), to: nil)
        check(view.ownsDesktopPoint(center), "uncovered video owns pointer")
        let toolbar = NSButton(frame: root.bounds.insetBy(dx: 100, dy: 100))
        root.addSubview(toolbar, positioned: .above, relativeTo: view)
        check(!view.ownsDesktopPoint(center), "overlapping native toolbar excludes hidden cursor and remote routing")
        let ownershipChanges = client.tabletChanges
        NSCursor.crosshair.set()
        let hover = NSEvent.mouseEvent(with: .mouseMoved, location: center, modifierFlags: [], timestamp: 0,
            windowNumber: window.windowNumber, context: nil, eventNumber: 1, clickCount: 0, pressure: 0)!
        view.cursorUpdate(with: hover)
        check(NSCursor.current === NSCursor.arrow, "toolbar cursor update restores visible arrow")
        check(client.tabletChanges == ownershipChanges, "toolbar hover leaves tablet capture unchanged")
        green.isEnabled = false; green.target = window; green.action = #selector(NSWindow.performZoom(_:))
        view.updatePointerAppearance(for: hover)
        check(green.isEnabled && green.target === window.delegate && green.action == #selector(PlankMacWindowPresentation.toggleDesktopFullScreen(_:)),
            "native toolbar hover repairs window-specific green button before input dispatch")
        toolbar.removeFromSuperview()
        check(view.ownsDesktopPoint(center), "desktop routing resumes after toolbar retracts")
        view.setLocalControlsPresented(true)
        check(!view.ownsDesktopPoint(center), "popover remains local even over video")
        view.setLocalControlsPresented(false)
        let movingPresentation = window.delegate as! PlankMacWindowPresentation
        movingPresentation.windowWillExitFullScreen(Notification(name: NSWindow.willExitFullScreenNotification, object: window))
        view.removeFromSuperview()
        check(PlankMacSessionWindows.owns(client: client, window: window),
            "temporary Space reparent preserves old session membership")
        root.addSubview(view)
        check(PlankMacSessionWindows.owns(client: client, window: window),
            "Space reparent rebinds surface to its desktop")
        view.detachWindowPresentation()
        PlankMacSessionWindows.attach(client: client, surface: view.surfaceID, window: nil)
        check(!PlankMacSessionWindows.owns(client: client, window: window),
            "real surface teardown still removes session membership")

        let region = PlankMacLocalPointerView(frame: NSRect(x: 0, y: 0, width: 40, height: 24))
        check(region.hitTest(NSPoint(x: 20, y: 12)) == nil, "native pointer region never intercepts buttons")
        NSCursor.crosshair.set(); region.cursorUpdate(with: hover)
        check(NSCursor.current === NSCursor.arrow, "local control owns a visible native arrow")
        PlankMacLocalPointerView.hideForDesktop()
        check(NSCursor.current === NSCursor.arrow, "desktop hiding retains real arrow for system menu-bar reveal")
        PlankMacLocalPointerView.showArrow()

        var bitrate = 50_000.0
        var finalUpdates = 0
        let control = PlankMacSlider(value: Binding(get: { bitrate }, set: { bitrate = $0 }),
            range: 10_000...150_000, step: 500, label: "Video bitrate", editingChanged: { if !$0 { finalUpdates += 1 } })
        let coordinator = PlankMacSlider.Coordinator(parent: control)
        let slider = PlankMacNativeSlider(frame: NSRect(x: 0, y: 0, width: 240, height: 24))
        slider.cell = PlankMacSliderCell(); slider.minValue = 10_000; slider.maxValue = 150_000
        slider.doubleValue = 100_260; coordinator.changed(slider)
        check(bitrate == 100_500 && slider.doubleValue == 100_500, "native slider snaps bitrate in 500 kbps steps")
        check(finalUpdates == 1, "keyboard/accessibility changes commit without a mouse release")
        slider.doubleValue = 999_999; coordinator.changed(slider)
        check(bitrate == 150_000, "native slider keeps upper bitrate bound")
        let cell = slider.cell as! PlankMacSliderCell
        for enabled in [true, false] {
            cell.isEnabled = enabled
            let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 24, pixelsHigh: 24,
                bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
                colorSpaceName: .deviceRGB, bytesPerRow: 96, bitsPerPixel: 32)!
            let graphics = NSGraphicsContext(bitmapImageRep: bitmap)!
            NSGraphicsContext.saveGraphicsState(); NSGraphicsContext.current = graphics
            NSColor.black.setFill(); NSRect(x: 0, y: 0, width: 24, height: 24).fill()
            cell.drawKnob(NSRect(x: 5, y: 5, width: 14, height: 14))
            NSGraphicsContext.restoreGraphicsState()
            let color = bitmap.colorAt(x: 12, y: 12)!.usingColorSpace(.deviceRGB)!
            check(color.redComponent > 0.5 && color.greenComponent > 0.5 && color.blueComponent > 0.5,
                "native slider handle visible in enabled and disabled states")
        }
        print("Mac local controls and fullscreen delegate: \(checks) checks passed")
    }
}
