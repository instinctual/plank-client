import SwiftUI
import Network
import UIKit
import Combine

@MainActor final class PlankIPadPencilRelay: ObservableObject {
    @Published private(set) var sharing = false
    @Published private(set) var status = "Sharing is off"
    @Published private(set) var verification: String?
    @Published private(set) var width = 1920
    @Published private(set) var height = 1200
    @Published private(set) var active = false
    @Published private(set) var padSettings = PlankIPadPencilPadSettings.load()
    @Published private(set) var adjustingPad = false
    @Published private(set) var inputEpoch: UInt64 = 0
    let imageReceiver = PlankIPadPencilImageReceiver()
    private var controlKeys = PlankControlKeyOwnership()
    var canDraw: Bool { active && !adjustingPad }
    // Local viewing does not grant permission to send pen or Artist input.
    var canNavigateImage: Bool { sharing && hasConfiguration && !adjustingPad && imageReceiver.imageSessionActive }
    private var listener: NWListener?
    private var peer: PlankPencilRelayPeer?
    private var generation = UUID(), peerID = UUID()
    private var imageServiceName: String?
    private var hasConfiguration = false
    weak var pad: PlankIPadPencilPadView?
    init() {
        imageReceiver.mappingWillChange = { [weak self] in self?.retireInput() }
        imageReceiver.mappingDidChange = { [weak self] in
            self?.pad?.setNeedsLayout(); self?.pad?.layoutIfNeeded()
        }
    }
    func setPadSettings(_ value: PlankIPadPencilPadSettings) {
        let next = value.validated
        guard next != padSettings else { return }
        retireInput(); padSettings = next; next.save(); pad?.setNeedsLayout()
    }
    func setAdjustingPad(_ value: Bool) {
        guard value != adjustingPad else { return }
        retireInput(); adjustingPad = value; pad?.setNeedsLayout()
    }
    private func offerControlKeys(_ events: [PlankControlKeyEvent]) -> Bool {
        guard !events.isEmpty else { return true }
        guard let peer, peer.offer(.keys(events.map {
            PlankPencilKeyEvent(code:$0.code,pressed:$0.pressed,modifiers:$0.modifiers)
        })) else { peer?.close(); active = false; return false }
        return true
    }
    func beginControl(owner: UUID,binding: PlankControlBinding) -> Bool {
        guard canDraw, let change = controlKeys.previewBegin(owner:.control(owner),binding:binding),
              offerControlKeys(change.events) else { return false }
        return controlKeys.apply(change)
    }
    func endControl(owner: UUID) {
        guard let change = controlKeys.previewEnd(owner:.control(owner)), offerControlKeys(change.events) else { return }
        _ = controlKeys.apply(change)
    }
    func tapControl(binding: PlankControlBinding) -> Bool {
        guard canDraw, let change = controlKeys.previewTap(binding:binding), offerControlKeys(change.events) else { return false }
        return controlKeys.apply(change)
    }
    func releaseControls() {
        guard let change = controlKeys.previewRetireControls() else { return }
        if offerControlKeys(change.events) { _ = controlKeys.apply(change) }
        else { controlKeys = PlankControlKeyOwnership() }
    }
    private func retireInput(preservingView: Bool = false) {
        inputEpoch &+= 1
        pad?.retireForTransition()
        releaseControls()
        pad?.resetViewAfterTransition(preservingView:preservingView)
    }
    func start() {
        guard !sharing else { return }
        do {
            let key = try PlankPencilRelayKeys.privateKey()
            let publicKey = try PlankPencilRelayKeys.publicKey(key)
            let listener = try NWListener(using:.tcp,on:.any)
            var txt = NWTXTRecord(); txt["version"] = PlankPencilProtocol.version; txt["capability"] = PlankPencilProtocol.capability
            txt["key"] = PlankPencilRelayKeys.hex(publicKey)
            listener.service = .init(name:UIDevice.current.name + " Pencil",type:"_plank-pencil._tcp",domain:"local.",txtRecord:txt)
            listener.newConnectionLimit = 1
            self.listener = listener; sharing = true; status = "Starting Pencil sharing…"
            generation = UUID(); let current = generation
            listener.serviceRegistrationUpdateHandler = { [weak self] change in
                Task { @MainActor in
                    guard let self, self.generation == current, self.sharing else { return }
                    switch change {
                    case let .add(endpoint):
                        guard case let .service(name,_,_,_) = endpoint, name != self.imageServiceName else { return }
                        self.imageServiceName = name
                        // Match the actual registered control name, including Bonjour renames.
                        // Image readiness is automatic; failure here cannot stop the pen lane.
                        do { try self.imageReceiver.start(privateKey:key,serviceName:name) }
                        catch { self.imageReceiver.stop() }
                        self.authorizeImage()
                    case let .remove(endpoint):
                        guard case let .service(name,_,_,_) = endpoint, name == self.imageServiceName else { return }
                        self.imageServiceName = nil; self.imageReceiver.stop()
                    @unknown default:
                        self.imageServiceName = nil; self.imageReceiver.stop()
                    }
                }
            }
            listener.newConnectionHandler = { [weak self] connection in
                Task { @MainActor in self?.accept(connection,key:key,generation:current) }
            }
            listener.stateUpdateHandler = { [weak self] state in Task { @MainActor in
                guard let self, self.generation == current else { return }
                if case .ready = state { self.status = "On the PLANK client, choose this iPad in Settings → Apple Pencil." }
                if case .failed = state { self.stop(); self.status = "Pencil sharing could not start. Check local network access." }
            } }
            listener.start(queue:.main)
        } catch { stop(); status = "Pencil sharing is unavailable. Check local network access and identity storage." }
    }
    private func accept(_ connection: NWConnection,key: Data,generation current: UUID) {
        guard sharing, generation == current, peer == nil else { connection.cancel(); return }
        do {
            imageReceiver.revoke(); hasConfiguration = false
            peerID = UUID(); let id = peerID
            let next = try PlankPencilRelayPeer(connection:connection,privateKey:key) { [weak self] event in
                Task { @MainActor in
                    guard let self, self.generation == current, self.peerID == id else { return }
                    switch event {
                    case let .verification(code):
                        self.verification = code; self.status = "Compare this code with the PLANK client before approving."
                    case .ready:
                        self.verification = nil; self.status = "Client approved. Waiting for an active desktop."
                        self.authorizeImage()
                    case .messagesAvailable: self.drain()
                    case let .ended(reason):
                        self.imageReceiver.revoke(); self.hasConfiguration = false
                        self.retireInput(); self.peer = nil; self.active = false
                        self.listener?.newConnectionLimit = 1
                        self.verification = nil; self.status = reason + ". Select this iPad again in the PLANK client."
                    }
                }
            }
            peer = next; next.start()
        } catch { connection.cancel(); listener?.newConnectionLimit = 1; status = "Could not verify client identity." }
    }
    private func authorizeImage() {
        guard let peer else { return }
        let current = generation, id = peerID
        peer.sessionBinding { [weak self] binding in
            Task { @MainActor in
                guard let self, self.generation == current, self.peerID == id,
                      self.peer != nil, let binding else { return }
                self.imageReceiver.authorize(binding)
                if self.hasConfiguration {
                    self.imageReceiver.configureDimensions(width:self.width,height:self.height)
                }
            }
        }
    }
    private func drain() {
        guard let peer else { return }
        do {
            while let message = try peer.incoming.take() {
                guard case let .configuration(w,h,next) = message else { throw PlankPencilWireError.invalid }
                applyConfiguration(width:Int(w),height:Int(h),active:next)
            }
        } catch { peer.close(); status = "Pencil connection ended." }
    }
    private func applyConfiguration(width nextWidth: Int,height nextHeight: Int,active next: Bool) {
        // The approved peer's dimensions define the view. Focus only changes
        // remote input admission; it must not disable or recenter local viewing.
        let sameSourceGeometry = hasConfiguration && width == nextWidth && height == nextHeight
        retireInput(preservingView:sameSourceGeometry)
        width = nextWidth; height = nextHeight; active = next; hasConfiguration = true
        imageReceiver.configureDimensions(width:width,height:height)
        status = next ? "Connected. Draw in the outlined area." : "Connected. Bring the PLANK desktop into focus on the client."
        pad?.setNeedsLayout()
    }
    func approve() { guard verification != nil else { return }; verification = nil; peer?.approve(); status = "Verifying client…" }
    func send(_ packet: PlankNormalizedPen) {
        guard let peer else { return }
        if !peer.offer(.pen(packet)) { peer.close(); active = false }
    }
    func click(x: Float,y: Float) {
        guard canDraw, let peer else { return }
        if !peer.offer(.rightClick(x:x,y:y)) { peer.close(); active = false }
    }
    func stop() {
        imageReceiver.stop(); imageServiceName = nil; hasConfiguration = false
        retireInput(); generation = UUID(); peerID = UUID(); active = false; sharing = false
        peer?.close(); peer = nil; verification = nil
        listener?.newConnectionHandler = nil; listener?.stateUpdateHandler = nil
        listener?.serviceRegistrationUpdateHandler = nil; listener?.cancel(); listener = nil
        adjustingPad = false; status = "Sharing is off"
    }
}

/// Consent is foreground local UI in the normal client working surface.
/// It remains visible without changing the pad or artist-control canvas size.
struct PlankIPadPencilConsent: View {
    @ObservedObject var relay: PlankIPadPencilRelay
    let code: String
    let reject: () -> Void
    var body: some View {
        VStack(spacing:12) {
            Text("Compare this code with the PLANK client before approving.")
                .font(.callout).multilineTextAlignment(.center)
            Text(code).font(.title.monospaced().bold())
            HStack {
                Button("Reject",role:.cancel,action:reject)
                Button("Codes Match — Approve Client") { relay.approve() }
                    .buttonStyle(.borderedProminent)
            }
        }.padding(16).frame(maxWidth:560)
            .background(.regularMaterial,in:RoundedRectangle(cornerRadius:16))
    }
}
struct PlankIPadPencilPadOptions: View {
    @ObservedObject var relay: PlankIPadPencilRelay
    @ObservedObject var router: PlankIPadInputRouter
    var stopSharing: (() -> Void)? = nil
    @Environment(\.dismiss) private var dismiss
    private func binding<Value>(_ key: WritableKeyPath<PlankIPadPencilPadSettings,Value>) -> Binding<Value> {
        Binding(get:{ relay.padSettings[keyPath:key] },set:{ value in
            var next = relay.padSettings; next[keyPath:key] = value; relay.setPadSettings(next)
        })
    }
    private func margin(_ title: String,_ key: WritableKeyPath<PlankIPadPencilPadSettings,Double>) -> some View {
        VStack(alignment:.leading) {
            HStack {
                Text(title)
                Spacer()
                Text(relay.padSettings[keyPath:key],format:.percent.precision(.fractionLength(0)))
                    .foregroundStyle(.secondary).monospacedDigit()
            }
            Slider(value:binding(key),in:0...0.4,step:0.01)
                .accessibilityLabel("\(title) margin")
                .accessibilityValue(Text(relay.padSettings[keyPath:key],format:.percent.precision(.fractionLength(0))))
        }
    }
    var body: some View {
        NavigationStack {
            Form {
                PlankIPadTouchOptions(router:router,sharing:true)
                Section("Apple Pencil Sharing") {
                    Text(relay.status).foregroundStyle(.secondary)
                    if let code = relay.verification {
                        PlankIPadPencilConsent(relay:relay,code:code,reject:{ stopSharing?() })
                    }
                    if let stopSharing { Button("Stop Sharing",role:.destructive,action:stopSharing) }
                }
                PlankIPadPencilMappingOptions(relay:relay,receiver:relay.imageReceiver)
                Section {
                    margin("Left",\.left); margin("Right",\.right)
                    margin("Top",\.top); margin("Bottom",\.bottom)
                } header: { Text("Margins") } footer: {
                    Text("Percent of the pad on each edge. Preferences follow the iPad’s current orientation.")
                }
                Section {
                    Picker("Tone",selection:binding(\.tone)) {
                        ForEach(PlankIPadPencilPadSettings.Tone.allCases,id:\.self) { Text($0.title).tag($0) }
                    }
                    VStack(alignment:.leading) {
                        Text("Pad glow")
                        Slider(value:binding(\.glow),in:0...1)
                            .accessibilityLabel("Pad glow")
                    }
                } header: { Text("Appearance") } footer: {
                    Text("Changes the pad’s shading. Adjust display brightness in Control Center.")
                }
                PlankIPadPencilImageOptions(receiver:relay.imageReceiver)
                Section { Button("Reset Pad Options") { relay.setPadSettings(.init()) } }
            }
            .navigationTitle("Pad Options").navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement:.confirmationAction) { Button("Done") { dismiss() } } }
        }
        .preferredColorScheme(.dark)
    }
}
private struct PlankIPadPencilMappingOptions: View {
    @ObservedObject var relay: PlankIPadPencilRelay
    @ObservedObject var receiver: PlankIPadPencilImageReceiver
    private var mapping: Binding<PlankIPadPencilPadSettings.Mapping> {
        Binding(get:{ relay.padSettings.effectiveMapping(referenceImageEnabled:receiver.imageSessionActive) },set:{ value in
            guard !receiver.imageSessionActive else { return }
            var next = relay.padSettings; next.mapping = value; relay.setPadSettings(next)
        })
    }
    var body: some View {
        Section {
            Picker("Mapping",selection:mapping) {
                ForEach(PlankIPadPencilPadSettings.Mapping.allCases,id:\.self) { Text($0.title).tag($0) }
            }.pickerStyle(.segmented).disabled(receiver.imageSessionActive)
        } header: { Text("Drawing area") } footer: {
            if receiver.imageSessionActive {
                Text("While the client sends an image, the picture and Pencil use the desktop’s proportions inside your margins. Turning transmission off on the client restores “\(relay.padSettings.mapping.title)”.")
            } else {
                Text(relay.padSettings.mapping == .matchDesktop
                     ? "Keeps the desktop’s proportions inside your margins."
                     : "Maps your entire chosen area to the desktop. Horizontal and vertical movement may scale differently.")
            }
        }
    }
}
private struct PlankIPadPencilImageOptions: View {
    @ObservedObject var receiver: PlankIPadPencilImageReceiver
    var body: some View {
        Section {
            VStack(alignment:.leading) {
                Text("Image intensity")
                Slider(value:Binding(get:{ receiver.intensity },set:{ receiver.setIntensity($0) }),in:0...1)
                    .accessibilityLabel("Image intensity")
            }
            Text(receiver.status).font(.footnote).foregroundStyle(.secondary)
        } header: { Text("Reference image") } footer: {
            Text("Pencil Sharing is ready to receive automatically. Use “Share desktop image with this iPad” on the Mac or AVP to turn the image on or off. Dimming and zoom stay on this iPad.")
        }
    }
}
struct PlankIPadPencilPad: UIViewRepresentable {
    let relay: PlankIPadPencilRelay
    var zoomController: PlankIPadZoomController? = nil
    func makeUIView(context: Context) -> PlankIPadPencilPadView {
        PlankIPadPencilPadView(relay:relay,zoomController:zoomController)
    }
    func updateUIView(_ view: PlankIPadPencilPadView,context: Context) { view.setNeedsLayout() }
    static func dismantleUIView(_ view: PlankIPadPencilPadView,coordinator: ()) { view.retire() }
}
@MainActor final class PlankIPadPencilPadView: UIView, UIPencilInteractionDelegate {
    private let relay: PlankIPadPencilRelay
    private let zoomController: PlankIPadZoomController?
    private var zoomState: PlankIPadZoomState?
    private var zoomObservation: AnyCancellable?
    private var policy = PlankIPadPencilPolicy(), squeeze = PlankIPadSqueezePolicy()
    private var touch: UITouch?
    private var handTouch: UITouch?
    private var handPoint: CGPoint?
    private var previous = CGRect.zero
    private var mappingSignature: String?
    private var receivedDowns = 0, acceptedDowns = 0, acceptedMoves = 0, acceptedUps = 0
    private var lastContactLog = -Double.infinity
    private let fill = CAShapeLayer(), outline = CAShapeLayer(), cursor = CAShapeLayer()
    private let imageView = PlankIPadPencilImageView()
    private var viewport: PlankIPadViewport? {
        relay.padSettings.viewport(bounds:bounds,width:relay.width,height:relay.height,
            referenceImageEnabled:relay.imageReceiver.imageSessionActive,
            visibleSourceCrop:zoomState?.visibleSourceCrop ?? CGRect(x:0,y:0,width:1,height:1))
    }
    init(relay: PlankIPadPencilRelay,zoomController: PlankIPadZoomController? = nil) {
        self.relay = relay; self.zoomController = zoomController; zoomState = zoomController?.state
        super.init(frame:.zero); relay.pad = self
        // Track Pencil independently when a finger or palm also touches the pad.
        isMultipleTouchEnabled = true; backgroundColor = .black
        layer.addSublayer(fill)
        imageView.configure(receiver:relay.imageReceiver); addSubview(imageView)
        outline.lineWidth = 1; layer.addSublayer(outline)
        layer.addSublayer(cursor)
        let hover = UIHoverGestureRecognizer(target:self,action:#selector(hovered(_:)))
        hover.allowedTouchTypes = [NSNumber(value:UITouch.TouchType.pencil.rawValue)]
        hover.cancelsTouchesInView = false; addGestureRecognizer(hover)
        let pencil = UIPencilInteraction(); pencil.delegate = self; addInteraction(pencil)
        zoomObservation = zoomController?.$state.sink { [weak self] state in
            MainActor.assumeIsolated { self?.applyZoomState(state) }
        }
    }
    required init?(coder: NSCoder) { fatalError("init(coder:) is unsupported") }
    override func layoutSubviews() {
        super.layoutSubviews()
        let rect = viewport?.rect ?? .zero
        if rect != previous {
            // This is the Pencil viewport, not the artist-controls surface.
            // Retire its stroke; the overlay owns control geometry retirement.
            retire(); previous = rect
        }
        refreshPresentation()
    }
    private func refreshPresentation() {
        let viewport = viewport, rect = viewport?.rect ?? .zero
        CATransaction.begin(); CATransaction.setDisableActions(true)
        let settings = relay.padSettings, white = settings.fillWhite
        fill.fillColor = (settings.tone == .charcoal ? UIColor(white:white,alpha:1)
            : UIColor(red:white * 1.07,green:white,blue:white * 0.85,alpha:1)).cgColor
        outline.strokeColor = UIColor(white:0.28 + settings.glow * 0.2,alpha:1).cgColor
        outline.fillColor = nil; fill.path = UIBezierPath(rect:rect).cgPath
        fill.opacity = relay.canDraw ? 1 : 0.4
        imageView.setViewport(viewport)
        cursor.fillColor = UIColor(white:0.6 + settings.glow * 0.2,alpha:1).cgColor
        outline.path = UIBezierPath(rect:rect).cgPath
        outline.opacity = relay.canDraw ? 1 : 0.4; CATransaction.commit()
        // Geometry only, never stroke positions: distinguish local remapping
        // from a downstream workstation/application transform on the next run.
        let effective = settings.effectiveMapping(referenceImageEnabled:relay.imageReceiver.imageSessionActive)
        let signature = "mode=\(effective.rawValue) saved=\(settings.mapping.rawValue) image=\(relay.imageReceiver.imageSessionActive) bounds=\(bounds) area=\(rect) desktop=\(relay.width)x\(relay.height)"
        if signature != mappingSignature {
            mappingSignature = signature
            NSLog("PLANK Pencil pad mapping %@",signature)
        }
    }
    func retire() {
        retireForTransition()
        resetViewAfterTransition()
    }
    func retireForTransition() {
        for p in policy.retire() { relay.send(p) }
        touch = nil; handTouch = nil; handPoint = nil; cursor.path = nil
    }
    func resetViewAfterTransition(preservingView: Bool = false) {
        // Every transition discards requests deferred against the old contact.
        // Input-only admission preserves the already applied picture transform.
        if preservingView { zoomController?.retireContactPreservingView() }
        else { zoomController?.reset() }
        updateRemoteContact()
    }
    private func finishRemoteContact() {
        for p in policy.retire() { relay.send(p) }
        touch = nil; cursor.path = nil
        updateRemoteContact()
    }
    private func retireHover() {
        guard !policy.touching else { return }
        for p in policy.retire() { relay.send(p) }
        cursor.path = nil
    }
    private func applyZoomState(_ state: PlankIPadZoomState) {
        let changedCrop = zoomState?.visibleSourceCrop != state.visibleSourceCrop
        zoomState = state
        refreshPresentation()
        if changedCrop { retireHover() }
    }
    private func updateRemoteContact() {
        zoomController?.setRemoteContactActive(touch != nil || policy.touching)
    }
    private func beginHand(_ touches: Set<UITouch>) -> Bool {
        guard relay.canNavigateImage, zoomState?.handEnabled == true,
              let next = touches.first(where: {
                  ($0.type == .direct || $0.type == .pencil) && ($0.view == nil || $0.view === self)
              }) else { return false }
        guard handTouch == nil, next !== touch, let viewport,
              viewport.normalized(next.location(in:self)) != nil else { return true }
        handTouch = next; handPoint = next.location(in:self)
        retireHover()
        return true
    }
    private func moveHand(_ touches: Set<UITouch>) -> Bool {
        guard let handTouch, touches.contains(handTouch) else { return false }
        let next = handTouch.location(in:self)
        if let handPoint, let viewport {
            zoomController?.requestPan(translation:CGPoint(x:next.x-handPoint.x,y:next.y-handPoint.y),in:viewport)
        }
        handPoint = next
        return true
    }
    private func endHand(_ touches: Set<UITouch>) -> Bool {
        guard let handTouch, touches.contains(handTouch) else { return false }
        self.handTouch = nil; handPoint = nil
        return true
    }
    private func send(_ sample: UITouch,phase: PlankNormalizedPen.Phase) -> Bool {
        guard relay.canDraw, let viewport, let p = policy.sample(phase,point:sample.location(in:self),viewport:viewport,
            timestamp:sample.timestamp,force:Double(sample.force),maximumForce:Double(sample.maximumPossibleForce),
            altitude:Double(sample.altitudeAngle),azimuth:Double(sample.azimuthAngle(in:self))) else { return false }
        relay.send(p); mark(sample.location(in:self))
        if phase == .move { acceptedMoves += 1 }
        if phase == .up { acceptedUps += 1 }
        logContactIfDue(); return true
    }
    private func mark(_ point: CGPoint) {
        CATransaction.begin(); CATransaction.setDisableActions(true)
        cursor.path = UIBezierPath(ovalIn:CGRect(x:point.x-3,y:point.y-3,width:6,height:6)).cgPath; CATransaction.commit()
    }
    @objc private func hovered(_ gesture: UIHoverGestureRecognizer) {
        controlPencilHover(gesture)
    }
    func controlPencilHover(_ gesture: UIHoverGestureRecognizer) {
        guard touch == nil, handTouch == nil, relay.canDraw, let viewport else { return }
        guard gesture.state == .began || gesture.state == .changed,
              let p = policy.sample(.hover,point:gesture.location(in:self),viewport:viewport,
                timestamp:ProcessInfo.processInfo.systemUptime,force:0,maximumForce:0,
                altitude:Double(gesture.altitudeAngle),azimuth:Double(gesture.azimuthAngle(in:self)),distance:Double(gesture.zOffset)) else { retireHover(); return }
        zoomController?.recordHover(normalized:CGPoint(x:CGFloat(p.x),y:CGFloat(p.y)))
        relay.send(p); mark(gesture.location(in:self))
    }
    override func touchesBegan(_ touches: Set<UITouch>,with event: UIEvent?) {
        if beginHand(touches) { return }
        guard touch == nil, let next = touches.first(where: { $0.type == .pencil }) else { return }
        receivedDowns += 1
        guard relay.canDraw, let viewport,
              let packets = policy.beginContact(point:next.location(in:self),viewport:viewport,
                timestamp:next.timestamp,force:Double(next.force),maximumForce:Double(next.maximumPossibleForce),
                altitude:Double(next.altitudeAngle),azimuth:Double(next.azimuthAngle(in:self))) else {
            logContactIfDue(); return
        }
        // Preserve the zero-pressure hover retirement before the fresh down.
        // Subsequent motion remains ordered by actual acquisition timestamps.
        for packet in packets { relay.send(packet) }
        touch = next; updateRemoteContact()
        acceptedDowns += 1; mark(next.location(in:self)); logContactIfDue()
    }
    override func touchesMoved(_ touches: Set<UITouch>,with event: UIEvent?) {
        _ = moveHand(touches)
        guard let touch, touches.contains(touch) else { return }
        for s in (event?.coalescedTouches(for:touch) ?? [touch]).sorted(by: { $0.timestamp < $1.timestamp }) { _ = send(s,phase:.move) }
        _ = send(touch,phase:.move)
    }
    override func touchesEnded(_ touches: Set<UITouch>,with event: UIEvent?) {
        _ = endHand(touches)
        guard let touch, touches.contains(touch) else { return }
        _ = send(touch,phase:.up); finishRemoteContact()
    }
    override func touchesCancelled(_ touches: Set<UITouch>,with event: UIEvent?) {
        _ = endHand(touches)
        guard let touch, touches.contains(touch) else { return }; finishRemoteContact()
    }
    private func logContactIfDue() {
        let now = ProcessInfo.processInfo.systemUptime
        guard now - lastContactLog >= 1 else { return }
        lastContactLog = now
        // Counts only: no position, typed content, pairing codes or identities.
        NSLog("PLANK Pencil pad contacts received=%d accepted=%d moves=%d ups=%d active=%d",
            receivedDowns,acceptedDowns,acceptedMoves,acceptedUps,relay.active ? 1 : 0)
    }
    func pencilInteraction(_ interaction: UIPencilInteraction,didReceiveSqueeze value: UIPencilInteraction.Squeeze) {
        controlPencilSqueeze(value,from:interaction.view ?? self)
    }
    func controlPencilSqueeze(_ value: UIPencilInteraction.Squeeze,from source: UIView) {
        guard handTouch == nil, let location = value.hoverPose?.location else { return }
        // UIPencilHoverPose.location belongs to the interaction's view.
        let point = source.convert(location,to:self)
        guard let position = viewport?.normalized(point),
              squeeze.click(ended:value.phase == .ended,timestamp:value.timestamp,enabled:relay.canDraw,
                touching:policy.touching,heldButtons:false,hasPosition:true) else { return }
        relay.click(x:Float(position.x),y:Float(position.y))
    }
}
