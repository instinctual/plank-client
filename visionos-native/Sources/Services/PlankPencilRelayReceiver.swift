#if PLANK_PENCIL_RELAY_RECEIVER
import Foundation
import Network
import Combine

// Diagnostic storage never publishes UI state or owns input work. The peer's
// existing callback records scheduling before its existing MainActor task.
private final class PlankPencilReceiverFlowDiagnostics: @unchecked Sendable {
    struct Snapshot {
        let scheduled: UInt64, started: UInt64, finished: UInt64
        let outstanding: UInt64, highWaterOutstanding: UInt64
        let lastWaitNanos: UInt64, maxWaitNanos: UInt64
        let drains: UInt64, messages: UInt64, normalizedPens: UInt64
        let lastDrainNanos: UInt64, maxDrainNanos: UInt64, maxDrainMessages: UInt64
    }
    private let lock = NSLock()
    private var scheduled: UInt64 = 0, started: UInt64 = 0, finished: UInt64 = 0
    private var outstanding: UInt64 = 0, highWaterOutstanding: UInt64 = 0
    private var lastWaitNanos: UInt64 = 0, maxWaitNanos: UInt64 = 0
    private var drains: UInt64 = 0, messages: UInt64 = 0, normalizedPens: UInt64 = 0
    private var lastDrainNanos: UInt64 = 0, maxDrainNanos: UInt64 = 0, maxDrainMessages: UInt64 = 0
    private var lastReportAt: UInt64?
    private static func adding(_ a: UInt64, _ b: UInt64) -> UInt64 {
        let (value, overflow) = a.addingReportingOverflow(b)
        return overflow ? UInt64.max : value
    }
    func recordScheduled() {
        lock.lock(); defer { lock.unlock() }
        scheduled = Self.adding(scheduled, 1); outstanding = Self.adding(outstanding, 1)
        highWaterOutstanding = max(highWaterOutstanding, outstanding)
    }
    func recordStarted(scheduledAt: UInt64, at: UInt64) {
        lock.lock(); defer { lock.unlock() }
        started = Self.adding(started, 1)
        lastWaitNanos = at >= scheduledAt ? at - scheduledAt : 0
        maxWaitNanos = max(maxWaitNanos, lastWaitNanos)
    }
    func recordFinished() {
        lock.lock(); defer { lock.unlock() }
        finished = Self.adding(finished, 1)
        if outstanding > 0 { outstanding -= 1 }
    }
    func recordDrain(startedAt: UInt64, at: UInt64, messages count: UInt64, normalizedPens pens: UInt64) {
        lock.lock(); defer { lock.unlock() }
        drains = Self.adding(drains, 1); messages = Self.adding(messages, count)
        normalizedPens = Self.adding(normalizedPens, pens)
        lastDrainNanos = at >= startedAt ? at - startedAt : 0
        maxDrainNanos = max(maxDrainNanos, lastDrainNanos); maxDrainMessages = max(maxDrainMessages, count)
    }
    func snapshotForReport(at: UInt64) -> Snapshot? {
        lock.lock(); defer { lock.unlock() }
        if let lastReportAt, at < lastReportAt || at - lastReportAt < 2_000_000_000 { return nil }
        lastReportAt = at
        return Snapshot(scheduled: scheduled, started: started, finished: finished,
            outstanding: outstanding, highWaterOutstanding: highWaterOutstanding,
            lastWaitNanos: lastWaitNanos, maxWaitNanos: maxWaitNanos,
            drains: drains, messages: messages, normalizedPens: normalizedPens,
            lastDrainNanos: lastDrainNanos, maxDrainNanos: maxDrainNanos, maxDrainMessages: maxDrainMessages)
    }
}

struct PlankDiscoveredPencilPad: Identifiable, Sendable {
    let id: String
    let name: String
    let key: Data
    let endpoint: NWEndpoint
}
@MainActor final class PlankPencilRelayReceiver: ObservableObject {
    @Published private(set) var pads: [PlankDiscoveredPencilPad] = []
    @Published private(set) var status = "Pencil sharing is off"
    @Published private(set) var verification: String?
    @Published private(set) var selectedName: String?
    @Published private(set) var connected = false
    @Published private(set) var imageSharingEnabled = false
#if PLANK_NATIVE_MAC_WACOM
    @Published private(set) var inputDetails = "Waiting for desktop input state"
    @Published private(set) var inputCounts = "Received messages: 0 · Pencil samples queued: 0"
#endif
    let imageSender = PlankPencilImageSender()
    private var imageBinding: PlankPencilSessionBinding?
    private var imageStarted = false
    private var imageAuthority = UUID(), imageRetryID = UUID()
    private var imageAttempt: UUID?
    private var imageRetryTask: Task<Void, Never>?
    private var imageRetryDelay: UInt64 = 1
    private weak var client: PlankCoreClient?
    private var browser: NWBrowser?
    private var discoveryOwners = Set<UUID>()
    private var peer: PlankPencilRelayPeer?
    private var generation = UUID(), discoveryID = UUID()
    private var delivered = PlankPencilStrokeState()
    private var deliveredKeys = PlankPencilKeyState()
    private var admission = false
    private var chosenPad: PlankDiscoveredPencilPad?
    private var usedDesktop = false
    private var dimensions = PlankFrameDimensions(width:1920,height:1200)
    private var flowDiagnostics = PlankPencilReceiverFlowDiagnostics()
    var ownsPen: Bool { chosenPad != nil }
    var strokeActive: Bool { delivered.touching }
    init(client: PlankCoreClient) { self.client = client }
    func discover(owner: UUID) {
        discoveryOwners.insert(owner)
        guard browser == nil else { return }
        let parameters = NWParameters.tcp; parameters.includePeerToPeer = true
        let next = NWBrowser(for:.bonjourWithTXTRecord(type:"_plank-pencil._tcp",domain:"local."),using:parameters)
        discoveryID = UUID(); let id = discoveryID
        next.browseResultsChangedHandler = { [weak self] results,_ in
            // TXT is only a route/capability hint. Trust requires transcript
            // verification against the iPad's physically displayed check code.
            let found = results.compactMap { result -> PlankDiscoveredPencilPad? in
                guard case let .service(name,_,_,_) = result.endpoint,
                      case let .bonjour(txt) = result.metadata,
                      txt["version"] == PlankPencilProtocol.version, txt["capability"] == PlankPencilProtocol.capability,
                      let text = txt["key"], let key = PlankPencilRelayKeys.unhex(text) else { return nil }
                return .init(id:text,name:name,key:key,endpoint:result.endpoint)
            }.sorted { $0.name < $1.name }
            Task { @MainActor in
                guard let self, self.discoveryID == id, self.browser != nil else { return }
                var keys = Set<String>(); self.pads = Array(found.filter { keys.insert($0.id).inserted }.prefix(32))
            }
        }
        next.stateUpdateHandler = { [weak self] state in Task { @MainActor in
            guard let self, self.discoveryID == id else { return }
            if case .failed = state { self.status = "Pencil discovery unavailable. Check local network access." }
        } }
        browser = next; next.start(queue:.main)
    }
    func stopDiscovery(owner: UUID) {
        discoveryOwners.remove(owner)
        guard discoveryOwners.isEmpty else { return }
        discoveryID = UUID(); browser?.cancel(); browser = nil; pads = []
    }
    func connect(_ pad: PlankDiscoveredPencilPad) {
        guard let client else { return }
        guard client.pencilRelaySourceAllowed else { status = client.pencilRelaySourceMessage; return }
        disconnect()
        chosenPad = pad
        do {
            let key = try PlankPencilRelayKeys.privateKey()
            let connection = NWConnection(to:pad.endpoint,using:.tcp)
            generation = UUID(); let current = generation
            let diagnostics = PlankPencilReceiverFlowDiagnostics(); flowDiagnostics = diagnostics
            let next = try PlankPencilRelayPeer(connection:connection,privateKey:key,peerKey:pad.key) { [weak self] event in
                let scheduledAt: UInt64?
                if case .messagesAvailable = event {
                    scheduledAt = DispatchTime.now().uptimeNanoseconds
                    diagnostics.recordScheduled()
                } else { scheduledAt = nil }
                Task { @MainActor in
                    if let scheduledAt {
                        diagnostics.recordStarted(scheduledAt: scheduledAt, at: DispatchTime.now().uptimeNanoseconds)
                    }
                    // A superseded peer's task remains balanced in its own
                    // diagnostic generation, even when admission rejects it.
                    defer { if scheduledAt != nil { diagnostics.recordFinished() } }
                    guard let self, self.generation == current else { return }
                    switch event {
                    case let .verification(code): self.verification = code; self.status = "Compare this code with the iPad, then approve on both devices."
                    case .ready:
                        self.connected = true; self.verification = nil; self.status = "Pencil connected"; self.sync()
                        self.authorizeImage(generation:current)
                    case .messagesAvailable: self.drain()
                    case let .ended(reason):
                        self.stopImage()
                        self.retire(); self.peer = nil; self.chosenPad = nil; self.usedDesktop = false; self.admission = false; self.connected = false
                        self.verification = nil; self.selectedName = nil; self.status = reason
                    }
                }
            }
            peer = next; selectedName = pad.name; status = "Verifying iPad…"; next.start(); sync()
        } catch { disconnect(); status = "Could not open Pencil connection." }
    }
    func approve() { guard verification != nil else { return }; verification = nil; peer?.approve(); status = "Waiting for iPad approval…" }
    func setImageSharingEnabled(_ enabled: Bool) {
        guard enabled != imageSharingEnabled else { return }
        if !enabled {
            imageSharingEnabled = false; cancelImageAttempt(); return
        }
        guard connected, peer != nil else { return }
        imageSharingEnabled = true
        if imageBinding == nil { authorizeImage(generation:generation) }
        syncImage()
    }
    private func authorizeImage(generation current: UUID) {
        peer?.sessionBinding { [weak self] binding in
            Task { @MainActor in
                guard let self, self.generation == current, self.connected,
                      self.peer != nil, let binding else { return }
                if self.imageBinding != binding {
                    self.cancelImageAttempt(); self.imageBinding = binding
                }
                self.syncImage()
            }
        }
    }
    private func syncImage() {
        guard imageSharingEnabled, connected, peer != nil, let client,
              case .streaming = client.phase, let binding = imageBinding,
              let pad = chosenPad else {
            if imageStarted || imageRetryTask != nil { cancelImageAttempt() }
            return
        }
        // Frequent desktop updates cannot bypass an already scheduled retry.
        guard !imageStarted, imageRetryTask == nil else { return }
        // Image authority belongs to this approved peer and desktop lifetime.
        // A transient input-focus pause does not change image admission.
        imageStarted = true
        let authority = imageAuthority, current = generation, attempt = UUID()
        imageAttempt = attempt
        do {
            try imageSender.start(client:client,binding:binding,endpoint:pad.endpoint,
                                  privateKey:PlankPencilRelayKeys.privateKey()) { [weak self] event in
                guard let self, self.imageAuthority == authority,
                      self.generation == current, self.imageAttempt == attempt,
                      self.imageSharingEnabled, self.connected, self.peer != nil else { return }
                guard let client = self.client, case .streaming = client.phase else {
                    self.cancelImageAttempt(); return
                }
                switch event {
                case .ready: self.imageRetryDelay = 1
                case .ended:
                    self.imageStarted = false; self.imageAttempt = nil
                    self.scheduleImageRetry()
                }
            }
        } catch {
            imageStarted = false; imageAttempt = nil; imageSender.fail()
            scheduleImageRetry()
        }
    }
    private func scheduleImageRetry() {
        guard imageSharingEnabled, connected, peer != nil, imageBinding != nil,
              chosenPad != nil, let client, case .streaming = client.phase,
              !imageStarted, imageRetryTask == nil else { return }
        let authority = imageAuthority, current = generation, retry = UUID()
        imageRetryID = retry
        let delay = imageRetryDelay
        imageRetryDelay = min(8, delay * 2)
        // One weak task owns a capped delay. No client, key, endpoint or pen
        // transcript is retained by the wait; authority is re-read on wake.
        imageRetryTask = Task { @MainActor [weak self] in
            do { try await Task.sleep(nanoseconds:delay * 1_000_000_000) }
            catch { return }
            guard !Task.isCancelled, let self, self.imageAuthority == authority,
                  self.generation == current, self.imageRetryID == retry else { return }
            self.imageRetryTask = nil
            self.syncImage()
        }
    }
    private func cancelImageAttempt() {
        imageAuthority = UUID(); imageRetryID = UUID(); imageAttempt = nil
        imageRetryTask?.cancel(); imageRetryTask = nil
        imageStarted = false; imageRetryDelay = 1; imageSender.stop()
    }
    private func stopImage() {
        imageSharingEnabled = false; imageBinding = nil; cancelImageAttempt()
    }
    func sync() {
        guard let client else { return }
#if PLANK_NATIVE_MAC_WACOM
        let nextDetails = client.pencilRelayInputDetails
        if inputDetails != nextDetails { inputDetails = nextDetails }
#endif
        logFlowDiagnostics()
        syncImage()
        guard let peer else {
            if client.pencilRelayCanDraw, let chosenPad { connect(chosenPad) }
            return
        }
        let nextDimensions = client.frameDimensions ?? .init(width:1920,height:1200)
        let nextAdmission = connected && client.pencilRelayCanDraw
        guard admission != nextAdmission || dimensions != nextDimensions else { return }
        retire()
        admission = nextAdmission; dimensions = nextDimensions
        if admission { usedDesktop = true }
        peer.configure(width:dimensions.width,height:dimensions.height,active:admission)
        if connected {
            status = admission ? "Pencil connected · \(dimensions.width) × \(dimensions.height)" :
                "Pencil connected; open a supported desktop and bring it into focus."
        }
    }
    private func drain() {
        guard let peer else { return }
        let drainStartedAt = DispatchTime.now().uptimeNanoseconds
        var messageCount: UInt64 = 0, normalizedPenCount: UInt64 = 0
        defer {
            flowDiagnostics.recordDrain(startedAt: drainStartedAt, at: DispatchTime.now().uptimeNanoseconds,
                                        messages: messageCount, normalizedPens: normalizedPenCount)
        }
        do {
            while let message = try peer.incoming.take() {
                if messageCount < UInt64.max { messageCount += 1 }
                guard admission, let client, client.pencilRelayCanDraw else { continue }
                switch message {
                case let .pen(p):
                    // A move from a stroke begun while unfocused cannot start a
                    // new stroke after focus returns. Only a fresh down admits it.
                    if p.phase == .move || p.phase == .up, !delivered.touching { continue }
                    if p.phase == .down {
                        for button: UInt8 in [1,2,3] { client.setMouseButton(number:button,pressed:false) }
                    }
                    try delivered.accept(p)
                    guard client.sendPencilRelayPen(p) else { throw PlankPencilWireError.overflow }
                    if normalizedPenCount < UInt64.max { normalizedPenCount += 1 }
                case let .rightClick(x,y):
                    guard !delivered.touching else { continue }
                    retireStroke()
                    guard client.sendPencilRelayRightClick(x:x,y:y,width:dimensions.width,height:dimensions.height) else {
                        throw PlankPencilWireError.overflow
                    }
                case let .modifier(key,pressed):
                    try deliverKeys([.init(code:key.rawValue,pressed:pressed)],to:client)
                case let .keys(events):
                    try deliverKeys(events,to:client)
                default: throw PlankPencilWireError.invalid
                }
            }
        } catch { peer.close(); retire(); status = "Pencil input ended." }
    }
    private func logFlowDiagnostics() {
        guard connected, let peer,
              let flow = flowDiagnostics.snapshotForReport(at: DispatchTime.now().uptimeNanoseconds) else { return }
        let incoming = peer.diagnostics.incoming
#if PLANK_NATIVE_MAC_WACOM
        // Reuse the existing two-second report; never publish per pen sample.
        let nextCounts = "Received messages: \(flow.messages) · Pencil samples queued: \(flow.normalizedPens)"
        if inputCounts != nextCounts { inputCounts = nextCounts }
#endif
        func ms(_ nanos: UInt64) -> Double { Double(nanos) / 1_000_000 }
        // All clocks here are local to this receiver. Counts include rejected
        // admission in messages, while normalizedPens counts successful offers.
        NSLog("PLANK Pencil receiver diagnostics: scheduled=%llu started=%llu finished=%llu outstanding=%llu highWater=%llu waitLast=%.2fms waitMax=%.2fms drains=%llu messages=%llu normalizedPens=%llu drainLast=%.2fms drainMax=%.2fms drainMaxMessages=%llu incomingDepth=%ld incomingOldest=%.2fms incomingValueAge=%.2fms",
              flow.scheduled, flow.started, flow.finished, flow.outstanding, flow.highWaterOutstanding,
              ms(flow.lastWaitNanos), ms(flow.maxWaitNanos), flow.drains, flow.messages, flow.normalizedPens,
              ms(flow.lastDrainNanos), ms(flow.maxDrainNanos), flow.maxDrainMessages,
              incoming.depth, ms(incoming.oldestAgeNanos), ms(incoming.headValueAgeNanos))
    }
    private func deliverKeys(_ batch: [PlankPencilKeyEvent], to client: PlankCoreClient) throws {
        // The authenticated peer already checked balanced edges. Ups from a
        // down received while unfocused have no delivered owner to release.
        var next = deliveredKeys
        var accepted: [PlankPencilKeyEvent] = []
        for edge in batch {
            if !edge.pressed, !next.held.contains(edge.code) { continue }
            try next.accept([edge]); accepted.append(edge)
        }
        guard !accepted.isEmpty else { return }
        guard client.sendPencilRelayKeys(accepted) else { throw PlankPencilWireError.overflow }
        deliveredKeys = next
    }
    private func retireStroke() { for p in delivered.retire() { client?.retirePencilRelayPen(p) } }
    private func retire() {
        retireStroke(); _ = deliveredKeys.retire(); client?.retirePencilRelayModifiers()
    }
    func endDesktop() {
        stopImage()
        retire(); client?.resetPencilKeyOwnership(); admission = false
        guard usedDesktop else {
            peer?.configure(width:dimensions.width,height:dimensions.height,active:false)
            return
        }
        // Each desktop lifetime gets a fresh cryptographic peer generation.
        // Late events from the previous stream cannot enter the next desktop.
        generation = UUID(); peer?.close(); peer = nil; connected = false
        verification = nil; usedDesktop = false
        status = "Pencil selected; waiting for the next supported desktop."
    }
    func disconnect() {
        stopImage()
        retire(); generation = UUID(); peer?.close(); peer = nil; admission = false
        chosenPad = nil; usedDesktop = false
        connected = false; selectedName = nil; verification = nil; status = "Pencil sharing is off"
    }
}

#endif
