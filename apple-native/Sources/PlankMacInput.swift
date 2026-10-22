import AppKit
import SwiftUI

struct PlankMacSurface: NSViewRepresentable {
    let client: PlankCoreClient
    var windowChanged: (NSWindow?) -> Void = { _ in }
    var localControlsPresented = false
    var topology: PlankTopology?
    var outputIndex = 0
    func makeNSView(context: Context) -> PlankMacInputView {
        let view = PlankMacInputView(client: client)
        view.windowChanged = windowChanged
        view.setOutput(topology: topology, index: outputIndex)
        view.setLocalControlsPresented(localControlsPresented)
        client.registerVideoSurface(id: view.surfaceID, frame: { [weak view] in view?.display($0) },
            cursor: { [weak view] in view?.cursor($0) }, cursorShape: { [weak view] in view?.shape($0) })
        return view
    }
    func updateNSView(_ view: PlankMacInputView, context: Context) {
        view.windowChanged = windowChanged
        view.setOutput(topology: topology, index: outputIndex)
        view.setLocalControlsPresented(localControlsPresented)
    }
    static func dismantleNSView(_ view: PlankMacInputView, coordinator: ()) {
#if PLANK_PENCIL_RELAY_RECEIVER
        view.client.setPencilLocalControls(false, owner: view.surfaceID)
#endif
        view.releaseInput(); view.restoreLocalCursor(); view.detachWindowPresentation()
        PlankMacSessionWindows.attach(client: view.client, surface: view.surfaceID, window: nil)
        view.client.unregisterVideoSurface(id: view.surfaceID)
    }
}
final class PlankMacInputView: NSView {
    let client: PlankCoreClient
    let surfaceID = UUID()
    private let video = PlankMacMetalView(frame: .zero)
    private let software = CALayer()
    private let cursorOverlay = PlankMacCursorOverlay(frame: .zero)
    var windowChanged: (NSWindow?) -> Void = { _ in }
    private var width = 0, height = 0
    private var topology: PlankTopology?
    private var outputIndex = 0
    private var latestFrame: PlankRenderedFrame?
    private var latestCursor: PlankRemoteCursor?
    private var geometry: PlankMacDisplayGeometry?
    private var heldButtons = Set<UInt8>()
    private var scrollAccumulator = PlankMacScrollAccumulator()
    private var heldKeys = Set<UInt16>()
    private var modifierKeys = Set<UInt16>()
    private var observations = [NSObjectProtocol]()
    private var tracking: NSTrackingArea?
    private var replacingSystemCursor = false
    private var localControlsPresented = false
    private var windowPresentation: PlankMacWindowPresentation?
    private var cursorMonitor: Any?
    private var lastMouseMovement: NSEvent?
    private var wheelForwarded = 0
    private var wheelVertical = 0, wheelHorizontal = 0
    private var wheelLogTime = 0.0
    private var canvas: CGRect { geometry?.canvas(in: bounds) ?? .zero }
    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    init(client: PlankCoreClient) {
        self.client = client; super.init(frame: .zero); wantsLayer = true
        layer?.backgroundColor = NSColor.black.cgColor; addSubview(video)
        layer?.addSublayer(software); software.contentsGravity = .resizeAspect
        addSubview(cursorOverlay, positioned: .above, relativeTo: video)
        let center = NotificationCenter.default
        for name in [NSApplication.didResignActiveNotification, NSApplication.didBecomeActiveNotification,
                     NSWindow.didBecomeKeyNotification, NSWindow.didResignKeyNotification] {
            observations.append(center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                Task { @MainActor [weak self] in self?.updateFocus() }
            })
        }
    }
    required init?(coder: NSCoder) { fatalError("init(coder:)") }
    isolated deinit {
        observations.forEach(NotificationCenter.default.removeObserver)
        if let cursorMonitor { NSEvent.removeMonitor(cursorMonitor) }
    }
    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        // AppKit can briefly detach the surface while reparenting a Space.
        // Keep its old session membership until the replacement window arrives;
        // dismantleNSView still explicitly removes a genuinely closed surface.
        if window == nil, windowPresentation?.preservesFocus == true || PlankMacSessionWindows.isPresenting(client: client) {
            PlankMacSessionWindows.refresh(client: client)
            return
        }
        detachWindowPresentation()
        if let window {
            PlankMacDesktopWindow.configure(window)
            windowPresentation = PlankMacWindowPresentation(window: window)
            windowPresentation?.changed = { [weak self] in self?.presentationChanged() }
            // The full-screen toolbar can cover the canvas without causing
            // mouseExited. Restore the local pointer before native controls
            // receive their events; never consume or reroute those events.
            cursorMonitor = NSEvent.addLocalMonitorForEvents(matching: [.mouseMoved, .leftMouseDragged,
                .rightMouseDragged, .otherMouseDragged, .cursorUpdate, .leftMouseDown,
                .rightMouseDown, .otherMouseDown, .scrollWheel]) { [weak self] event in
                if event.type == .mouseMoved { self?.forwardMouseMovement(event) }
                self?.updatePointerAppearance(for: event)
                // Eligible wheel events are consumed once here even if Space
                // changes left the responder chain on local chrome. Controls,
                // letterboxes and other windows continue through AppKit.
                if event.type == .scrollWheel, self?.forwardScroll(event) == true { return nil }
                return event
            }
            window.acceptsMouseMovedEvents = true; window.makeFirstResponder(self)
        }
        // SwiftUI's window reference is updated after the representable has
        // completed its view update; no session action is taken here.
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            if let window { PlankMacDesktopWindow.configure(window) }
            windowPresentation?.install()
            windowChanged(window)
        }
        PlankMacSessionWindows.attach(client: client, surface: surfaceID, window: window, view: self)
        updateFocus()
    }
    private func updateFocus() {
        windowPresentation?.install()
        let owned = NSApp.isActive && (window?.isKeyWindow == true || window?.isMainWindow == true)
        if !NSApp.isActive || localControlsPresented { restoreLocalCursor() }
        else { updatePointerAtCurrentLocation() }
        if !owned { releaseInput() }
        PlankMacSessionWindows.refresh(client: client)
    }
    private func presentationChanged() {
        PlankMacSessionWindows.refresh(client: client)
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            updateTrackingAreas()
            if !localControlsPresented, window?.isKeyWindow == true { window?.makeFirstResponder(self) }
            updatePointerAtCurrentLocation()
        }
    }
    func detachWindowPresentation() {
        windowPresentation?.remove(); windowPresentation = nil
        if let cursorMonitor { NSEvent.removeMonitor(cursorMonitor); self.cursorMonitor = nil }
    }
    func setLocalControlsPresented(_ presented: Bool) {
        guard localControlsPresented != presented else { return }
        localControlsPresented = presented
#if PLANK_PENCIL_RELAY_RECEIVER
        client.setPencilLocalControls(presented, owner: surfaceID)
#endif
        if presented { releaseInput(); restoreLocalCursor() }
        else if window?.isKeyWindow == true { window?.makeFirstResponder(self); updatePointerAtCurrentLocation() }
        window?.invalidateCursorRects(for: self)
        // Local controls retain tablet ownership. This affects cursor and
        // keyboard/mouse routing only, never the raw Wacom capture lifetime.
    }
    func restoreLocalCursor() { PlankMacLocalPointerView.showArrow() }
    override func layout() { super.layout(); video.frame = canvas; software.frame = canvas; cursorOverlay.frame = bounds; refreshNativeCursor(); window?.invalidateCursorRects(for: self) }
    override func updateTrackingAreas() {
        if let tracking { removeTrackingArea(tracking) }
        let next = NSTrackingArea(rect: .zero, options: [.inVisibleRect,.activeInActiveApp,.mouseMoved,.cursorUpdate,.mouseEnteredAndExited], owner: self)
        addTrackingArea(next); tracking = next; super.updateTrackingAreas()
    }
    func setOutput(topology: PlankTopology?, index: Int) {
        guard self.topology != topology || outputIndex != index else { return }
        releaseInput()
        self.topology = topology; outputIndex = index
        geometry = nil
        if let topology, topology.splitPresentation, topology.macPresentationOutputs.indices.contains(index) {
            let output = topology.macPresentationOutputs[index]
            NSLog("PLANK Mac output: index=%d id=%@ generation=%@ source=%d,%d %dx%d primary=%d",
                index, output.id, topology.generation, output.sourceRect.x, output.sourceRect.y,
                output.sourceRect.width, output.sourceRect.height, output.primary ? 1 : 0)
        }
        display(latestFrame)
    }
    func display(_ frame: PlankRenderedFrame?) {
        latestFrame = frame
        guard let frame else { geometry = nil; width = 0; height = 0; video.isHidden = true; software.contents = nil; cursorOverlay.setDimensions(width: 0, height: 0); refreshNativeCursor(); return }
        if geometry == nil || width != frame.width || height != frame.height {
            geometry = PlankMacDisplayGeometry(frameWidth: frame.width, frameHeight: frame.height,
                topology: topology, outputIndex: outputIndex)
        }
        width = frame.width; height = frame.height; needsLayout = true
        guard let geometry else {
            video.isHidden = true; software.contents = nil
            cursorOverlay.setDimensions(width: 0, height: 0); refreshNativeCursor(); return
        }
        cursorOverlay.setGeometry(geometry)
        video.sourceCrop = geometry.normalizedCrop
        if let pixels = frame.pixelBuffer { video.isHidden = false; software.contents = nil; video.display(pixels) }
        else if let provider = CGDataProvider(data: frame.pixels as CFData) {
            video.isHidden = true
            let image = CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32,
                bytesPerRow: frame.bytesPerRow, space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: .byteOrder32Little.union(CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue)),
                provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
            software.contents = image?.cropping(to: CGRect(x: geometry.source.x, y: geometry.source.y,
                width: geometry.source.width, height: geometry.source.height))
        }
    }
    func cursor(_ position: PlankRemoteCursor?) {
        latestCursor = position
        cursorOverlay.setLocalMouse(client.nativeMouseOwnsPointer)
        cursorOverlay.cursor(position); refreshNativeCursor()
        updatePointerAtCurrentLocation()
    }
    func shape(_ shape: PlankRemoteCursorShape?) {
        cursorOverlay.cursorShape(shape); refreshNativeCursor(); updatePointerAtCurrentLocation()
    }
    func remotePointer(at screenPoint: CGPoint) -> (Int, Int, Int, Int)? {
        guard let window, window.isVisible, !localControlsPresented,
              let geometry else { return nil }
        let windowPoint = window.convertPoint(fromScreen: screenPoint)
        guard ownsDesktopPoint(windowPoint), let (x, y) = geometry.remote(
            convert(windowPoint, from: nil), in: bounds, dragging: false) else { return nil }
        return (x, y, width, height)
    }
    private func pointer(_ event: NSEvent, requireKeyWindow: Bool = true) -> Bool {
#if PLANK_PENCIL_RELAY_RECEIVER
        guard !client.pencilRelay.strokeActive else { return false }
#endif
        guard !localControlsPresented, NSApp.isActive, !requireKeyWindow || window?.isKeyWindow == true,
              let geometry else { return false }
        let dragging = !heldButtons.isEmpty
        var result: (Int, Int, Int, Int)?
        // AppKit delivers a held drag to its original view. If the mouse moves
        // into the other output window, map that window's canvas while keeping
        // button ownership here until the real release arrives.
        if dragging, let window {
            result = PlankMacSessionWindows.remotePointer(client: client,
                screenPoint: window.convertPoint(toScreen: event.locationInWindow))
        }
        if result == nil {
            guard dragging || ownsDesktopPoint(event.locationInWindow),
                  let (x, y) = geometry.remote(convert(event.locationInWindow, from: nil),
                                               in: bounds, dragging: dragging) else { return false }
            result = (x, y, width, height)
        }
        guard let (x, y, w, h) = result else { return false }
        client.movePointer(x: x, y: y, width: w, height: h)
        cursorOverlay.setLocalMouse(true)
        return true
    }
    private func forwardMouseMovement(_ event: NSEvent) {
        guard event.window === window, lastMouseMovement !== event else { return }
        lastMouseMovement = event
        // A Space transition can leave the responder chain on the titlebar.
        // Route hover by its actual canvas, without stealing keyboard focus.
        _ = pointer(event, requireKeyWindow: false)
    }
    override func mouseMoved(with event: NSEvent) { forwardMouseMovement(event); updatePointerAppearance(for: event) }
    override func mouseDragged(with event: NSEvent) { _ = pointer(event); updatePointerAppearance(for: event) }
    override func rightMouseDragged(with event: NSEvent) { _ = pointer(event); updatePointerAppearance(for: event) }
    override func otherMouseDragged(with event: NSEvent) { _ = pointer(event); updatePointerAppearance(for: event) }
    private func down(_ event: NSEvent, button: UInt8) {
        guard NSApp.isActive, ownsDesktopPoint(event.locationInWindow) else { return }
        window?.makeKey()
        guard pointer(event) else { return }; window?.makeFirstResponder(self)
        heldButtons.insert(button); client.setMouseButton(number: button, pressed: true)
    }
    private func up(_ button: UInt8) { if heldButtons.remove(button) != nil { client.setMouseButton(number: button, pressed: false) } }
    override func mouseDown(with event: NSEvent) { down(event, button: 1) }
    override func mouseUp(with event: NSEvent) { up(1) }
    override func rightMouseDown(with event: NSEvent) { down(event, button: 3) }
    override func rightMouseUp(with event: NSEvent) { up(3) }
    override func otherMouseDown(with event: NSEvent) { if let button = PlankMacKeys.mouseButton(event.buttonNumber) { down(event, button: button) } }
    override func otherMouseUp(with event: NSEvent) { if let button = PlankMacKeys.mouseButton(event.buttonNumber) { up(button) } }
    private func forwardScroll(_ event: NSEvent) -> Bool {
        guard NSApp.isActive, event.window === window, geometry != nil,
              ownsDesktopPoint(event.locationInWindow) else { return false }
        // Real mouse movement positions the Host pointer, including over a
        // non-key desktop. Do not manufacture motion for wheel-only input:
        // Linux uses XTEST for absolute motion and uinput for scrolling. That
        // alternation resets GTK's scroll baseline before each wheel tick.
        let (vertical, horizontal) = scrollAccumulator.take(event: event)
        if vertical != 0 || horizontal != 0 { client.scroll(vertical: vertical, horizontal: horizontal) }
        wheelForwarded += 1; wheelVertical += Int(vertical); wheelHorizontal += Int(horizontal)
        let now = ProcessInfo.processInfo.systemUptime
        if now - wheelLogTime >= 1 {
            NSLog("PLANK Mac wheel: output=%d events=%d vertical=%d horizontal=%d precise=%d",
                outputIndex, wheelForwarded, wheelVertical, wheelHorizontal, event.hasPreciseScrollingDeltas ? 1 : 0)
            wheelLogTime = now; wheelForwarded = 0; wheelVertical = 0; wheelHorizontal = 0
        }
        return true
    }
    override func scrollWheel(with event: NSEvent) { _ = forwardScroll(event) }
    override func keyDown(with event: NSEvent) {
        guard !localControlsPresented else { super.keyDown(with: event); return }
        guard !PlankMacKeys.staysLocal(event), let key = PlankMacKeys.table[event.keyCode] else { super.keyDown(with: event); return }
        heldKeys.insert(key); client.sendKey(code: key, pressed: true, modifiers: PlankMacKeys.modifiers(event.modifierFlags))
    }
    override func keyUp(with event: NSEvent) {
        guard let key = PlankMacKeys.table[event.keyCode], heldKeys.remove(key) != nil else { return }
        client.sendKey(code: key, pressed: false, modifiers: PlankMacKeys.modifiers(event.modifierFlags))
    }
    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        if localControlsPresented || PlankMacKeys.staysLocal(event) { return false }; keyDown(with: event); return true
    }
    override func flagsChanged(with event: NSEvent) {
        guard !localControlsPresented else { return }
        if event.keyCode == 57 { client.pressKey(code: 0x14); return }
        let next = PlankMacKeys.modifiers(event.modifierFlags)
        let keys: [(UInt8,UInt16)] = [(1,0x10),(2,0x11),(4,0x12),(8,0x5b)]
        for (bit,key) in keys {
            let pressed = next & bit != 0
            if pressed != modifierKeys.contains(key) {
                if pressed { modifierKeys.insert(key) } else { modifierKeys.remove(key) }
                client.sendKey(code: key, pressed: pressed, modifiers: next)
            }
        }
    }
    override func resignFirstResponder() -> Bool { releaseInput(); restoreLocalCursor(); return true }
    func releaseInput() {
        scrollAccumulator.reset()
        heldButtons.forEach { client.setMouseButton(number: $0, pressed: false) }
        heldKeys.union(modifierKeys).forEach { client.sendKey(code: $0, pressed: false, modifiers: 0) }
        heldButtons.removeAll(); heldKeys.removeAll(); modifierKeys.removeAll()
    }
    // Do not register a hidden cursor rectangle over the entire canvas:
    // AppKit's revealed full-screen toolbar overlaps that rectangle.
    override func resetCursorRects() {}
    func ownsDesktopPoint(_ windowPoint: NSPoint) -> Bool {
        guard !localControlsPresented, window != nil, width > 0,
              canvas.contains(convert(windowPoint, from: nil)) else { return false }
        var root: NSView = self
        while let parent = root.superview { root = parent }
        let point = root.superview?.convert(windowPoint, from: nil) ?? windowPoint
        guard let hit = root.hitTest(point) else { return false }
        return hit === self || hit.isDescendant(of: self)
    }
    func updatePointerAppearance(for event: NSEvent) {
        if event.window === window {
            // Revealed fullscreen chrome can be revalidated after the delegate's
            // did-enter callback. Rebind its native button before local hover or
            // click dispatch, including on the non-key desktop window.
            if !ownsDesktopPoint(event.locationInWindow) { windowPresentation?.install() }
            updatePointerAppearance(at: event.locationInWindow)
        }
        else if !PlankMacSessionWindows.owns(client: client, window: event.window) { restoreLocalCursor() }
        // Another session window handles its own cursor appearance.
    }
    private func updatePointerAtCurrentLocation() {
        guard let window, window.isVisible, window.frame.contains(NSEvent.mouseLocation),
              NSWindow.windowNumber(at: NSEvent.mouseLocation, belowWindowWithWindowNumber: 0) == window.windowNumber else { return }
        // A popover, local browser or another app above this desktop owns its
        // pointer. Background cursor packets must not hide that local pointer.
        // The other desktop must not restore the native arrow over this canvas.
        updatePointerAppearance(at: window.mouseLocationOutsideOfEventStream)
    }
    private func updatePointerAppearance(at point: NSPoint) {
        let mouseOwns = client.nativeMouseOwnsPointer
        cursorOverlay.setLocalMouse(mouseOwns)
        let artwork = cursorOverlay.nativeMouseCursor
        let currentHostPosition = latestCursor != nil && width > 0 && height > 0 &&
            latestCursor?.frameWidth == width && latestCursor?.frameHeight == height
        switch PlankMacPointerPresentation.mode(appActive: NSApp.isActive,
            onCanvas: ownsDesktopPoint(point), mouseOwns: mouseOwns, mouseArtwork: artwork != nil,
            currentHostPosition: currentHostPosition, overlayOwns: cursorOverlay.replacesSystemCursor) {
        case .mouse:
            // WindowServer draws at the actual local mouse position.
            NSCursor.setHiddenUntilMouseMoves(false); artwork?.set()
        case .hidden:
            // Reassert after AppKit reveals the parked pointer during a Space
            // transition. Also hide it when the pen is on the other output.
            PlankMacLocalPointerView.hideForDesktop()
        case .local:
            restoreLocalCursor()
        }
    }
    private func refreshNativeCursor() {
        guard replacingSystemCursor != cursorOverlay.replacesSystemCursor else { return }
        replacingSystemCursor = cursorOverlay.replacesSystemCursor
        if !replacingSystemCursor { updatePointerAtCurrentLocation() }
        window?.invalidateCursorRects(for: self)
    }
    override func cursorUpdate(with event: NSEvent) {
        updatePointerAppearance(for: event)
    }
    override func mouseExited(with event: NSEvent) { restoreLocalCursor() }
}
