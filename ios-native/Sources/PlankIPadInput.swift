import SwiftUI
import UIKit
import GameController

@MainActor final class PlankIPadInputRouter: ObservableObject {
    weak var surface: PlankIPadInputView?
    let client: PlankCoreClient
    @Published private(set) var wheelDiagnostics = "Received 0 · sent 0 · blocked 0"
    @Published private(set) var softwareKeyboardPresented = false
    @Published private(set) var heldControlIDs = Set<UUID>()
    @Published private(set) var inputEpoch: UInt64 = 0
    @Published private(set) var inputFailure: String?
    func updateWheelDiagnostics(_ value: String) { wheelDiagnostics = value }
    var enabled = false {
        didSet {
            if !enabled {
                setSoftwareKeyboardPresented(false)
                surface?.publishWheelDiagnostics(); release()
            }
            else if !oldValue, !isHandlingInputFailure { keySession.reopen(); surface?.resumeKeyboard() }
        }
    }
    private var held = PlankIPadHeldInput()
    private var keySession = PlankControlKeySession()
    private var isHandlingInputFailure = false
    private var pointerMotion = PlankIPadPointerMotion()
    private(set) var pointerMoves = 0, stationaryPointerCallbacks = 0
    private var pencil = PlankIPadPencilPolicy()
    var pencilTouching: Bool { pencil.touching }
    var hasHeldButtons: Bool { !held.buttons.isEmpty }
    init(client: PlankCoreClient) { self.client = client }
    func clearInputFailure() { inputFailure = nil }
    private func admitKeys(_ change: PlankControlKeyOwnership.Change) -> Bool {
        guard !isHandlingInputFailure else { return false }
        let accepted = keySession.admit(change,offer:client.offerControlKeys) { [self] retirement in
            // One bounded attempt to retire existing keys, then invalidate the
            // connection. Do not continue with uncertain Host key ownership.
            isHandlingInputFailure = true
            defer { isHandlingInputFailure = false }
            inputFailure = "Keyboard input stopped responding. The desktop was disconnected; reconnect to continue."
            pointerMotion.reset()
            retirePencil()
            for button in held.releaseButtons() { client.setMouseButton(number:button,pressed:false) }
            if !retirement.isEmpty { _ = client.offerControlKeys(retirement) }
            client.reset()
            enabled = false
            surface?.clearContact()
        }
        heldControlIDs = keySession.ownership.controlOwners
        return accepted
    }
    func setSoftwareKeyboardPresented(_ presented: Bool) {
        let next = presented && enabled
        guard next != softwareKeyboardPresented else { return }
        release()
        softwareKeyboardPresented = next
        surface?.refreshSoftwareKeyboard()
    }
    func softwareText(_ text: String) {
        guard enabled, softwareKeyboardPresented else { return }
        for command in PlankIPadSoftwareKeyboard.commands(for: text) {
            guard enabled else { break }
            switch command {
            case let .key(code, modifiers): _ = tapControl(binding:.init(code:code,modifiers:modifiers))
            case let .text(value): client.sendText(value)
            }
        }
    }
    func softwareKey(_ code: UInt16) {
        guard enabled, softwareKeyboardPresented else { return }
        _ = tapControl(binding:.init(code:code))
    }
    func pointer(_ point: CGPoint, viewport: PlankIPadViewport, dragging: Bool,
                 suppressStationary: Bool = false) -> Bool {
        guard !pencil.touching else { return false }
        retirePencil()
        guard enabled, let pixel = viewport.map(point, held: dragging) else { return false }
        if pointerMotion.shouldSend(x: pixel.x, y: pixel.y, width: viewport.width,
                                    height: viewport.height, force: !suppressStationary) {
            client.movePointer(x: pixel.x, y: pixel.y, width: viewport.width, height: viewport.height)
            pointerMoves += 1
        } else { stationaryPointerCallbacks += 1 }
        return true
    }
    @discardableResult
    func pen(_ phase: PlankNormalizedPen.Phase, point: CGPoint, viewport: PlankIPadViewport,
             timestamp: Double, force: Double, maximumForce: Double,
             altitude: Double, azimuth: Double, distance: Double = 0) -> Bool {
        guard enabled, client.acceptsNormalizedPen,
              let packet = pencil.sample(phase, point: point, viewport: viewport, timestamp: timestamp,
                  force: force, maximumForce: maximumForce, altitude: altitude, azimuth: azimuth,
                  distance: distance) else { return false }
        pointerMotion.reset()
        client.sendPen(packet)
        return true
    }
    func retirePencil() {
        let packets = pencil.retire()
        if !packets.isEmpty { pointerMotion.reset() }
        for packet in packets { client.sendPen(packet) }
    }
    func preparePencilContact() {
        pointerMotion.reset()
        retirePencil()
        for number in held.releaseButtons() { client.setMouseButton(number: number, pressed: false) }
        surface?.clearPointerContact()
    }
    func button(_ number: UInt8, pressed: Bool) {
        guard (!pressed || enabled), held.button(number, pressed: pressed) else { return }
        client.setMouseButton(number: number, pressed: pressed)
    }
    func key(_ code: UInt16, pressed: Bool, modifiers: UInt8) {
        guard !isHandlingInputFailure, (!pressed || enabled),
              let change = keySession.ownership.previewHardware(code:code,pressed:pressed,modifiers:modifiers) else { return }
        _ = admitKeys(change)
    }
    @discardableResult
    func beginControl(owner: UUID, binding: PlankControlBinding) -> Bool {
        guard !isHandlingInputFailure, enabled,
              let change = keySession.ownership.previewBegin(owner:.control(owner),binding:binding) else { return false }
        return admitKeys(change)
    }
    @discardableResult
    func endControl(owner: UUID) -> Bool {
        guard !isHandlingInputFailure,
              let change = keySession.ownership.previewEnd(owner:.control(owner)) else { return false }
        return admitKeys(change)
    }
    @discardableResult
    func tapControl(binding: PlankControlBinding) -> Bool {
        guard !isHandlingInputFailure, enabled,
              let change = keySession.ownership.previewTap(binding:binding) else { return false }
        return admitKeys(change)
    }
    @discardableResult
    func releaseControls() -> Bool {
        guard !isHandlingInputFailure else { return false }
        guard let change = keySession.ownership.previewRetireControls() else { return true }
        return admitKeys(change)
    }
    func release() {
        guard !isHandlingInputFailure else { return }
        inputEpoch &+= 1
        pointerMotion.reset()
        retirePencil()
        let releases = held.release()
        for number in releases.buttons { client.setMouseButton(number: number, pressed: false) }
        if let change = keySession.ownership.previewRetireAll() { _ = admitKeys(change) }
        surface?.clearContact()
    }
}

struct PlankIPadCanvas: UIViewRepresentable {
    let client: PlankCoreClient
    let router: PlankIPadInputRouter
    let functionKeyMode: KeyboardFunctionKeyMode
    func makeUIView(context: Context) -> PlankIPadInputView {
        PlankIPadInputView(client: client, router: router)
    }
    func updateUIView(_ view: PlankIPadInputView, context: Context) {
        view.functionKeyMode = functionKeyMode
        view.updateDimensions(client.frameDimensions)
    }
    static func dismantleUIView(_ view: PlankIPadInputView, coordinator: ()) { view.stop() }
}

@MainActor final class PlankIPadInputView: UIView, UIPencilInteractionDelegate, UIKeyInput {
    private let client: PlankCoreClient
    private let router: PlankIPadInputRouter
    private let video = PlankIPadVideoView(frame: .zero)
    private let surfaceID = UUID()
    private var dimensions: PlankFrameDimensions?
    private var activeTouch: UITouch?
    private var pointerButtons = Set<UInt8>()
    private var wheel = PlankIPadWheel()
    private var discreteScroll: UIPanGestureRecognizer?
    private var wheelEvents = 0
    private var wheelReceived = 0, wheelBlocked = 0
    private var wheelVertical = 0, wheelHorizontal = 0
    private var lastWheelLog = 0.0
    private var previousVideoFrame = CGRect.zero
    private lazy var keyboardViewport = PlankIPadKeyboardViewport(canvas: self)
    private var registered = false
    private var keyboardInput: GCKeyboardInput?
    private var transferringKeyboardFocus = false
    private lazy var keyboardEntry = PlankIPadKeyboardEntry(router: router, surface: self)
    private var ownsKeyboardFocus: Bool { isFirstResponder || keyboardEntry.isFirstResponder }
    private var keyboardPolicy = PlankIPadKeyboardPolicy()
    private var squeezePolicy = PlankIPadSqueezePolicy()
    var functionKeyMode: KeyboardFunctionKeyMode = .pc {
        didSet { if oldValue != functionKeyMode { router.release() } }
    }

    override var canBecomeFirstResponder: Bool { true }
    // There is no local document. Permit Backspace against the remote text,
    // even when this view has never received a character itself.
    var hasText: Bool { true }
    var autocorrectionType: UITextAutocorrectionType = .no
    var autocapitalizationType: UITextAutocapitalizationType = .none
    var spellCheckingType: UITextSpellCheckingType = .no
    var smartQuotesType: UITextSmartQuotesType = .no
    var smartDashesType: UITextSmartDashesType = .no
    var smartInsertDeleteType: UITextSmartInsertDeleteType = .no
    private let hiddenKeyboard = UIView(frame: .zero)
    private func configureKeyboardAssistant() {
        let item = keyboardEntry.inputAssistantItem
        item.allowsHidingShortcuts = !router.softwareKeyboardPresented
        guard router.softwareKeyboardPresented else {
            item.leadingBarButtonGroups = []; item.trailingBarButtonGroups = []
            return
        }
        let escape = UIBarButtonItem(title: "Esc", style: .plain, target: self, action: #selector(softwareEscape))
        let tab = UIBarButtonItem(title: "Tab", style: .plain, target: self, action: #selector(softwareTab))
        item.leadingBarButtonGroups = [UIBarButtonItemGroup(barButtonItems: [escape, tab], representativeItem: nil)]
        let hide = UIBarButtonItem(image: UIImage(systemName: "keyboard.chevron.compact.down"),
            style: .plain, target: self, action: #selector(hideSoftwareKeyboard))
        hide.accessibilityLabel = "Hide Keyboard"
        item.trailingBarButtonGroups = [UIBarButtonItemGroup(barButtonItems: [hide], representativeItem: nil)]
    }
    override var inputView: UIView? { hiddenKeyboard }

    func insertText(_ text: String) {
        guard isFirstResponder else { return }
        router.softwareText(text)
    }
    func deleteBackward() {
        guard isFirstResponder else { return }
        router.softwareKey(0x08)
    }
    @objc private func softwareEscape() { router.softwareKey(0x1B) }
    @objc private func softwareTab() { router.softwareKey(0x09) }
    @objc private func hideSoftwareKeyboard() { router.setSoftwareKeyboardPresented(false) }
    func refreshSoftwareKeyboard() {
        guard window != nil else { return }
        configureKeyboardAssistant()
        transferringKeyboardFocus = true
        defer { transferringKeyboardFocus = false }
        // Keep UIKit's native responder and keyboard mode controls. There is
        // no app-owned preview or custom accessory to reparent on expansion.
        if router.softwareKeyboardPresented {
            keyboardEntry.becomeFirstResponder()
            keyboardEntry.reloadInputViews()
        } else {
            keyboardEntry.resignFirstResponder()
            if router.enabled { resumeKeyboard() }
            reloadInputViews()
        }
        // Presentation/focus evidence only; never record typed characters.
        NSLog("PLANK iPad keyboard requested=%d textResponder=%d keyWindow=%d hardware=%d", router.softwareKeyboardPresented, keyboardEntry.isFirstResponder, window?.isKeyWindow == true, keyboardInput != nil)
    }
    private var viewport: PlankIPadViewport? {
        guard let dimensions else { return nil }
        return PlankIPadViewport(bounds: video.frame, width: dimensions.width, height: dimensions.height)
    }
    init(client: PlankCoreClient, router: PlankIPadInputRouter) {
        self.client = client; self.router = router
        super.init(frame: .zero)
        backgroundColor = .black
        isMultipleTouchEnabled = true
        addSubview(video)
        // A normal text responder lets UIKit supply its keyboard chooser with
        // hardware attached. It stores no document and is not a visible field.
        keyboardEntry.frame = CGRect(x: 0, y: 0, width: 1, height: 1)
        addSubview(keyboardEntry)
        registerForTraitChanges([UITraitPreferredContentSizeCategory.self]) { (view: PlankIPadInputView, _: UITraitCollection) in
            if view.router.softwareKeyboardPresented { view.refreshSoftwareKeyboard() }
        }
        let hover = UIHoverGestureRecognizer(target: self, action: #selector(hovered(_:)))
        hover.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.indirectPointer.rawValue)]
        hover.cancelsTouchesInView = false
        addGestureRecognizer(hover)
        let pencilHover = UIHoverGestureRecognizer(target: self, action: #selector(pencilHovered(_:)))
        pencilHover.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.pencil.rawValue)]
        pencilHover.cancelsTouchesInView = false
        addGestureRecognizer(pencilHover)
        // Separate masks preserve the distinction between physical notches and
        // continuous trackpad motion. Neither recognizer accepts touch drags.
        for mask in [UIScrollTypeMask.discrete, .continuous] {
            let scroll = UIPanGestureRecognizer(target: self, action: #selector(scrolled(_:)))
            scroll.allowedScrollTypesMask = mask
            // UIKit wheel input uses the indirect-pointer type. An empty
            // allowedTouchTypes list filters it out. Zero touch count keeps
            // finger/Pencil/button drags out of this scroll-only recognizer.
            scroll.allowedTouchTypes = [NSNumber(value: UITouch.TouchType.indirectPointer.rawValue)]
            scroll.maximumNumberOfTouches = 0
            scroll.cancelsTouchesInView = false
            if mask == .discrete { discreteScroll = scroll }
            addGestureRecognizer(scroll)
        }
        addInteraction(UIPencilInteraction(delegate: self))
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window == nil { stop(); return }
        keyboardViewport.start()
        guard !registered else { return }
        router.surface = self
        registered = true
        NotificationCenter.default.addObserver(self, selector: #selector(keyboardChanged),
            name: .GCKeyboardDidConnect, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(keyboardChanged),
            name: .GCKeyboardDidDisconnect, object: nil)
        attachKeyboard()
        client.registerVideoSurface(id: surfaceID,
            frame: { [weak self] frame in
                self?.updateDimensions(frame.map { .init(width: $0.width, height: $0.height) })
                self?.video.display(frame)
            }, cursor: { [weak self] cursor in self?.video.displayCursor(cursor) },
            cursorShape: { [weak self] shape in self?.video.displayCursorShape(shape) })
        resumeKeyboard()
    }
    func updateDimensions(_ next: PlankFrameDimensions?) {
        if next != dimensions { router.release() }
        dimensions = next
    }
    override func layoutSubviews() {
        super.layoutSubviews()
        video.frame = keyboardViewport.videoFrame
        // The input viewport is the same resized rectangle used by video.
        // Retire held contact before another sample uses changed geometry.
        if previousVideoFrame != video.frame {
            router.release()
            previousVideoFrame = video.frame
            NSLog("PLANK iPad viewport canvas=%.0fx%.0f video=%.0fx%.0f guideTop=%.0f reportedTop=%.0f reportedWidth=%.0f",
                bounds.width, bounds.height, video.frame.width, video.frame.height,
                keyboardLayoutGuide.layoutFrame.minY,
                keyboardViewport.reportedFrame?.minY ?? -1,
                keyboardViewport.reportedFrame?.width ?? 0)
        }
    }
    func clearContact() {
        clearPointerContact(); keyboardPolicy.reset()
    }
    func clearPointerContact() { activeTouch = nil; pointerButtons.removeAll(); wheel.reset() }
    @objc private func keyboardChanged() {
        router.release()
        attachKeyboard()
        if router.softwareKeyboardPresented { refreshSoftwareKeyboard() }
    }
    private func attachKeyboard() {
        keyboardInput?.keyChangedHandler = nil
        keyboardInput = GCKeyboard.coalesced?.keyboardInput
        GCKeyboard.coalesced?.handlerQueue = .main
        keyboardInput?.keyChangedHandler = { [weak self] source, _, code, pressed in
            // GCDevice delivers on the main queue selected above, so key edges
            // remain ordered with pointer/pen events and focus changes.
            MainActor.assumeIsolated {
                guard let self, self.router.enabled, self.ownsKeyboardFocus,
                      let input = self.keyboardInput, input === source else { return }
                var modifiers: UInt8 = 0
                for (left, right, mask) in [(0xE1, 0xE5, UInt8(1)), (0xE0, 0xE4, 2),
                                           (0xE2, 0xE6, 4), (0xE3, 0xE7, 8)] {
                    if input.button(forKeyCode: GCKeyCode(rawValue: left))?.isPressed == true ||
                       input.button(forKeyCode: GCKeyCode(rawValue: right))?.isPressed == true { modifiers |= mask }
                }
                self.hardwareKey(Int(code.rawValue), pressed: pressed, modifiers: modifiers)
            }
        }
    }
    private func hardwareKey(_ usage: Int, pressed: Bool, modifiers: UInt8) {
        guard let event = keyboardPolicy.event(usage: usage, pressed: pressed,
            modifiers: modifiers, mode: functionKeyMode) else { return }
        router.key(event.code, pressed: event.pressed, modifiers: event.modifiers)
    }
    // Prevent Space from activating local SwiftUI controls while the remote
    // canvas owns keyboard focus. The raw handler supplies real down/up edges.
    override var keyCommands: [UIKeyCommand]? {
        guard keyboardInput != nil, router.enabled else { return super.keyCommands }
        let command = UIKeyCommand(input: " ", modifierFlags: [], action: #selector(reservedSpace))
        command.wantsPriorityOverSystemBehavior = true
        return [command]
    }
    @objc private func reservedSpace() {}
    func pencilInteraction(_ interaction: UIPencilInteraction, didReceiveSqueeze squeeze: UIPencilInteraction.Squeeze) {
        controlPencilSqueeze(squeeze,from:interaction.view ?? self)
    }
    func controlPencilSqueeze(_ squeeze: UIPencilInteraction.Squeeze,from source: UIView) {
        guard let viewport else { return }
        let position = squeeze.hoverPose.map { source.convert($0.location,to:self) }
        guard squeezePolicy.click(ended: squeeze.phase == .ended, timestamp: squeeze.timestamp,
            enabled: router.enabled, touching: router.pencilTouching,
            heldButtons: router.hasHeldButtons,
            hasPosition: position.flatMap { viewport.map($0) } != nil), let position else { return }
        guard router.pointer(position, viewport: viewport, dragging: false) else { return }
        router.button(3, pressed: true)
        router.button(3, pressed: false)
    }

    func resumeKeyboard() {
        guard window != nil else { return }
        if router.softwareKeyboardPresented {
            if !keyboardEntry.isFirstResponder {
                transferringKeyboardFocus = true
                keyboardEntry.becomeFirstResponder()
                transferringKeyboardFocus = false
            }
        } else if !isFirstResponder { becomeFirstResponder() }
    }
    func softwareKeyboardResigned() {
        guard !transferringKeyboardFocus else { return }
        router.setSoftwareKeyboardPresented(false)
        router.release()
    }
    func stop() {
        keyboardViewport.stop()
        router.setSoftwareKeyboardPresented(false)
        router.release()
        NotificationCenter.default.removeObserver(self)
        keyboardInput?.keyChangedHandler = nil
        keyboardInput = nil
        if registered { client.unregisterVideoSurface(id: surfaceID); registered = false }
        if router.surface === self { router.surface = nil }
    }
    override func resignFirstResponder() -> Bool {
        if !transferringKeyboardFocus {
            router.setSoftwareKeyboardPresented(false)
            router.release()
        }
        return super.resignFirstResponder()
    }
    @objc private func hovered(_ gesture: UIHoverGestureRecognizer) {
        guard gesture.state == .began || gesture.state == .changed, let viewport else { return }
        _ = router.pointer(gesture.location(in: self), viewport: viewport,
                           dragging: activeTouch != nil, suppressStationary: true)
    }
    @objc private func pencilHovered(_ gesture: UIHoverGestureRecognizer) {
        controlPencilHover(gesture)
    }
    func controlPencilHover(_ gesture: UIHoverGestureRecognizer) {
        guard activeTouch == nil, let viewport else { return }
        if gesture.state == .began || gesture.state == .changed {
            guard viewport.normalized(gesture.location(in: self)) != nil else {
                router.retirePencil(); return
            }
            _ = router.pen(.hover, point: gesture.location(in: self), viewport: viewport,
                timestamp: ProcessInfo.processInfo.systemUptime, force: 0, maximumForce: 0,
                altitude: Double(gesture.altitudeAngle), azimuth: Double(gesture.azimuthAngle(in: self)),
                distance: Double(gesture.zOffset))
        } else { router.retirePencil() }
    }
    @objc private func scrolled(_ gesture: UIPanGestureRecognizer) {
        wheelReceived += 1
        guard gesture.state == .began || gesture.state == .changed || gesture.state == .ended else {
            wheelBlocked += 1; publishWheelDiagnosticsIfDue()
            gesture.setTranslation(.zero, in: self); wheel.reset(); return
        }
        guard router.enabled, !router.pencilTouching,
              let viewport,
              viewport.map(gesture.location(in: self)) != nil else {
            wheelBlocked += 1; publishWheelDiagnosticsIfDue()
            gesture.setTranslation(.zero, in: self); wheel.reset(); return
        }
        let delta = gesture.translation(in: self)
        gesture.setTranslation(.zero, in: self)
        if gesture.state == .began { wheel.reset() }
        let value = wheel.add(x: Double(delta.x), y: Double(delta.y),
            source: gesture === discreteScroll ? .discrete : .continuous)
        guard value.vertical != 0 || value.horizontal != 0 else { publishWheelDiagnosticsIfDue(); return }
        // Hover/contact events position the Host pointer. Do not manufacture
        // absolute motion for wheel-only input: Linux alternates XTEST motion
        // with uinput scrolling, resetting GTK's scroll baseline each tick.
        // This matches the accepted Mac adapter's wheel path.
        client.scroll(vertical: value.vertical, horizontal: value.horizontal)
        wheelEvents += 1; wheelVertical += Int(value.vertical); wheelHorizontal += Int(value.horizontal)
        publishWheelDiagnosticsIfDue()
    }
    func publishWheelDiagnostics() {
        router.updateWheelDiagnostics("Received \(wheelReceived) · sent \(wheelEvents) · blocked \(wheelBlocked)\nPointer moves \(router.pointerMoves) · stationary ignored \(router.stationaryPointerCallbacks)")
    }
    private func publishWheelDiagnosticsIfDue() {
        let now = ProcessInfo.processInfo.systemUptime
        if now - lastWheelLog >= 1 {
            publishWheelDiagnostics()
            NSLog("PLANK iPad wheel received=%d forwarded=%d blocked=%d vertical=%d horizontal=%d pointerMoves=%d stationaryIgnored=%d", wheelReceived, wheelEvents, wheelBlocked, wheelVertical, wheelHorizontal, router.pointerMoves, router.stationaryPointerCallbacks)
            lastWheelLog = now
        }
    }
    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        if let touch = touches.first(where: { $0.type == .pencil }), let viewport {
            guard viewport.normalized(touch.location(in: self)) != nil else { return }
            // Retire pointer ownership without releasing held shortcuts such as
            // Space + pen drag. Keys keep their real hardware up edge.
            router.preparePencilContact()
            if sendPencil(touch, phase: .down, viewport: viewport) { activeTouch = touch }
            resumeKeyboard()
            return
        }
        guard activeTouch == nil, let viewport,
              let touch = touches.first(where: { $0.type == .direct || $0.type == .indirectPointer }),
              router.pointer(touch.location(in: self), viewport: viewport, dragging: false) else { return }
        activeTouch = touch
        if touch.type == .indirectPointer {
            updatePointerButtons(event?.buttonMask.rawValue ?? 0, fallbackPrimary: true)
        } else { router.button(1, pressed: true) }
        resumeKeyboard()
    }
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = activeTouch, touches.contains(touch), let viewport else { return }
        if touch.type == .pencil {
            // Coalesced actual samples retain acquisition order. Timestamp policy
            // excludes duplicated final samples; predicted/late estimates stay local.
            for sample in (event?.coalescedTouches(for: touch) ?? [touch]).sorted(by: { $0.timestamp < $1.timestamp }) {
                _ = sendPencil(sample, phase: .move, viewport: viewport)
            }
            _ = sendPencil(touch, phase: .move, viewport: viewport)
            return
        }
        _ = router.pointer(touch.location(in: self), viewport: viewport, dragging: true)
        if touch.type == .indirectPointer, let event, !event.buttonMask.isEmpty {
            updatePointerButtons(event.buttonMask.rawValue, fallbackPrimary: false)
        }
    }
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = activeTouch, touches.contains(touch) else { return }
        if touch.type == .pencil {
            if let viewport {
                for sample in (event?.coalescedTouches(for: touch) ?? []).sorted(by: { $0.timestamp < $1.timestamp }) {
                    _ = sendPencil(sample, phase: .move, viewport: viewport)
                }
                _ = sendPencil(touch, phase: .up, viewport: viewport)
            }
            router.retirePencil()
            activeTouch = nil
            return
        }
        if let viewport { _ = router.pointer(touch.location(in: self), viewport: viewport, dragging: true) }
        if touch.type == .indirectPointer { updatePointerButtons(0, fallbackPrimary: false) }
        else { router.button(1, pressed: false) }
        activeTouch = nil
    }
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard let touch = activeTouch, touches.contains(touch) else { return }
        // Cancellation retires this contact's source. Independently held
        // hardware or artist-control keys retain their real release edge.
        if touch.type == .pencil { router.retirePencil() }
        else if touch.type == .indirectPointer { updatePointerButtons(0, fallbackPrimary:false) }
        else { router.button(1,pressed:false) }
        clearPointerContact()
    }
    @discardableResult
    private func sendPencil(_ touch: UITouch, phase: PlankNormalizedPen.Phase,
                            viewport: PlankIPadViewport) -> Bool {
        router.pen(phase, point: touch.location(in: self), viewport: viewport, timestamp: touch.timestamp,
            force: Double(touch.force), maximumForce: Double(touch.maximumPossibleForce),
            altitude: Double(touch.altitudeAngle), azimuth: Double(touch.azimuthAngle(in: self)))
    }
    private func updatePointerButtons(_ mask: Int, fallbackPrimary: Bool) {
        let raw = mask == 0 && fallbackPrimary ? 1 : mask
        // UIKit primary/secondary/tertiary masks map to the existing wire's
        // left/right/middle numbering (1/3/2), as in the accepted Mac adapter.
        let mapping: [(Int, UInt8)] = [(1, 1), (2, 3), (4, 2)]
        let next = Set(mapping.compactMap { raw & $0.0 != 0 ? $0.1 : nil })
        for button in pointerButtons.subtracting(next).sorted() { router.button(button, pressed: false) }
        for button in next.subtracting(pointerButtons).sorted() { router.button(button, pressed: true) }
        pointerButtons = next
    }
    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        guard router.enabled else { super.pressesBegan(presses, with: event); return }
        // A connected raw keyboard is the sole owner of mapped keys. UIKit is
        // retained for keyboards unavailable through GCKeyboard and text only.
        for press in presses {
            guard let key = press.key else { continue }
            if plankIPadVirtualKey(for: Int(key.keyCode.rawValue), functionKeyMode: functionKeyMode) != nil {
                if keyboardInput == nil {
                    hardwareKey(Int(key.keyCode.rawValue), pressed: true, modifiers: plankIPadModifiers(for: key.modifierFlags))
                }
            } else if !key.characters.isEmpty { client.sendText(key.characters) }
        }
    }
    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        guard keyboardInput == nil else { return }
        for press in presses {
            guard let key = press.key else { continue }
            hardwareKey(Int(key.keyCode.rawValue), pressed: false, modifiers: plankIPadModifiers(for: key.modifierFlags))
        }
    }
    override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        if keyboardInput == nil { router.release() }
    }
}

private func plankIPadModifiers(for flags: UIKeyModifierFlags) -> UInt8 {
    var modifiers: UInt8 = 0
    if flags.contains(.shift) { modifiers |= 1 }
    if flags.contains(.control) { modifiers |= 2 }
    if flags.contains(.alternate) { modifiers |= 4 }
    if flags.contains(.command) { modifiers |= 8 }
    return modifiers
}


@MainActor private final class PlankIPadKeyboardEntry: UITextField, UITextFieldDelegate {
    private let router: PlankIPadInputRouter
    private weak var surface: PlankIPadInputView?
    init(router: PlankIPadInputRouter, surface: PlankIPadInputView) {
        self.router = router; self.surface = surface
        super.init(frame: .zero)
        delegate = self
        textColor = .clear; tintColor = .clear; backgroundColor = .clear
        borderStyle = .none
        isAccessibilityElement = false
        autocorrectionType = .no; autocapitalizationType = .none
        spellCheckingType = .no; smartQuotesType = .no; smartDashesType = .no
        smartInsertDeleteType = .no
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }
    override var hasText: Bool { true }
    override func caretRect(for position: UITextPosition) -> CGRect { .zero }
    func textField(_ textField: UITextField, shouldChangeCharactersIn range: NSRange, replacementString string: String) -> Bool {
        if string.isEmpty { router.softwareKey(0x08) }
        else { router.softwareText(string) }
        return false // Already sent live; never retain a local editable document.
    }
    func textFieldShouldReturn(_ textField: UITextField) -> Bool {
        router.softwareKey(0x0D)
        return false
    }
    override func deleteBackward() { router.softwareKey(0x08) }
    override var keyCommands: [UIKeyCommand]? { surface?.keyCommands }
    override func pressesBegan(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        surface?.pressesBegan(presses, with: event)
    }
    override func pressesEnded(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        surface?.pressesEnded(presses, with: event)
    }
    override func pressesCancelled(_ presses: Set<UIPress>, with event: UIPressesEvent?) {
        surface?.pressesCancelled(presses, with: event)
    }
    override func resignFirstResponder() -> Bool {
        let resigned = super.resignFirstResponder()
        if resigned { surface?.softwareKeyboardResigned() }
        return resigned
    }
}
