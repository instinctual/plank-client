#if PLANK_PENCIL_RELAY_RECEIVER
import Foundation
import Network
import Combine

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
            let next = try PlankPencilRelayPeer(connection:connection,privateKey:key,peerKey:pad.key) { [weak self] event in
                Task { @MainActor in
                    guard let self, self.generation == current else { return }
                    switch event {
                    case let .verification(code): self.verification = code; self.status = "Compare this code with the iPad, then approve on both devices."
                    case .ready: self.connected = true; self.verification = nil; self.status = "Pencil connected"; self.sync()
                    case .messagesAvailable: self.drain()
                    case let .ended(reason):
                        self.retire(); self.peer = nil; self.chosenPad = nil; self.usedDesktop = false; self.admission = false; self.connected = false
                        self.verification = nil; self.selectedName = nil; self.status = reason
                    }
                }
            }
            peer = next; selectedName = pad.name; status = "Verifying iPad…"; next.start(); sync()
        } catch { disconnect(); status = "Could not open Pencil connection." }
    }
    func approve() { guard verification != nil else { return }; verification = nil; peer?.approve(); status = "Waiting for iPad approval…" }
    func sync() {
        guard let client else { return }
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
        do {
            while let message = try peer.incoming.take() {
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
        retire(); generation = UUID(); peer?.close(); peer = nil; admission = false
        chosenPad = nil; usedDesktop = false
        connected = false; selectedName = nil; verification = nil; status = "Pencil sharing is off"
    }
}

#endif
