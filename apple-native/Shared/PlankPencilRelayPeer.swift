import Foundation
import Network

enum PlankPencilPeerEvent: Sendable {
    case verification(String)
    case ready
    case messagesAvailable
    case ended(String)
}

// One executor owns the Noise state, socket, deadline and writes. Both directions
// have bounded mailboxes. User approval is bound to THIS ephemeral transcript.
final class PlankPencilRelayPeer: @unchecked Sendable {
    private let queue = DispatchQueue(label:"la.instinctual.plank.pencil.peer",qos:.userInteractive)
    private let connection: NWConnection
    private let initiator: Bool
    private let event: @Sendable (PlankPencilPeerEvent) -> Void
    private var crypto: OpaquePointer?
    private var peerKey: Data
    private var stage = 0 // 0 first/second, 1 approval, 2 secure-before-ready, 3 ready
    private var verified: Bool
    private var secondReceived = false
    private var closed = false, sending = false
    private var buffer = Data(), writes: [Data] = []
    private let outgoing = PlankPencilMailbox()
    let incoming = PlankPencilMailbox()
    private var timer: DispatchSourceTimer?
    private let deadline = DispatchTime.now().uptimeNanoseconds + 60_000_000_000
    private var lastReceive = DispatchTime.now().uptimeNanoseconds
    private var lastPing = DispatchTime.now().uptimeNanoseconds
    private var openingConfiguration: PlankPencilMessage?
    private let approvalLookup: @Sendable (Data) throws -> Bool
    private let saveApproval: @Sendable (Data) throws -> Void
    private var configuration: PlankPencilMessage = .configuration(width:1920,height:1200,active:false)
    private var stroke = PlankPencilStrokeState()
    private var keys = PlankPencilKeyState()
    private var active = false
    private static let prefix = Data([0x50,0x4c,0x50,0x4e,PlankPencilProtocol.wireVersion])

    init(connection: NWConnection, privateKey: Data, peerKey: Data? = nil,
         approvalLookup: @escaping @Sendable (Data) throws -> Bool = PlankPencilRelayKeys.approved,
         saveApproval: @escaping @Sendable (Data) throws -> Void = PlankPencilRelayKeys.approve,
         event: @escaping @Sendable (PlankPencilPeerEvent) -> Void) throws {
        guard privateKey.count == 32, peerKey == nil || peerKey?.count == 32 else { throw PlankPencilWireError.invalid }
        initiator = peerKey != nil; self.connection = connection; self.event = event
        self.peerKey = peerKey ?? Data()
        self.approvalLookup = approvalLookup; self.saveApproval = saveApproval
        verified = try peerKey.map { try approvalLookup($0) } ?? false
        let c = privateKey.withUnsafeBytes { key in
            (peerKey ?? Data()).withUnsafeBytes { peer in
                plank_pencil_crypto_create(initiator ? 1 : 0,key.bindMemory(to:UInt8.self).baseAddress,
                    peer.bindMemory(to:UInt8.self).baseAddress)
            }
        }
        guard let c else { throw PlankPencilWireError.crypto }; crypto = c
    }
    deinit { if let crypto { plank_pencil_crypto_destroy(crypto) } }
    func start() {
        queue.async { [self] in
            connection.stateUpdateHandler = { [weak self] state in
                guard let self, !self.closed else { return }
                switch state {
                case .ready:
                    do {
                        if self.initiator {
                            let body = try self.output { plank_pencil_crypto_first($0,$1,$2,$3) }
                            self.stage = 1; self.writeRecord(Self.prefix + body)
                            if !self.verified { self.event(.verification(try self.code())) }
                        }
                        self.read()
                    } catch { self.finish("Could not verify Pencil connection") }
                case .failed, .cancelled: self.finish("Pencil connection ended")
                default: break
                }
            }
            connection.start(queue:queue)
            let tick = DispatchSource.makeTimerSource(queue:queue); timer = tick
            tick.schedule(deadline: .now() + .seconds(1),repeating:.seconds(1))
            tick.setEventHandler { [weak self] in self?.tick() }; tick.resume()
        }
    }
    // Called only by an explicit local comparison action, or an existing pin.
    func approve() {
        queue.async { [self] in
            guard !closed, stage == 1 else { return }
            verified = true
            do {
                if initiator { try completeInitiator() }
                else {
                    let body = try output { plank_pencil_crypto_approve($0,$1,$2,$3) }
                    stage = 2; writeRecord(Self.prefix + body)
                }
            } catch { finish("Pencil approval failed") }
        }
    }
    func configure(width: Int, height: Int, active: Bool) {
        queue.async { [self] in
            guard !closed, initiator, width > 1, height > 1, width <= 65536, height <= 65536 else { return }
            let next = PlankPencilMessage.configuration(width:UInt32(width),height:UInt32(height),active:active)
            guard configuration != next else { return }; configuration = next
            if stage == 3 { _ = offer(next) }
        }
    }
    @discardableResult func offer(_ message: PlankPencilMessage) -> Bool {
        // The caller commits key ownership only after this admission. Reject
        // malformed or oversized batches before the asynchronous encoder runs.
        guard (try? message.encoded()) != nil else { return false }
        let result = outgoing.offer(message)
        if result.wake { queue.async { [self] in pump() } }
        return result.accepted
    }
    func close() { queue.async { [self] in finish("Pencil sharing stopped") } }
    private func output(_ operation: (OpaquePointer,UnsafeMutablePointer<UInt8>,Int,UnsafeMutablePointer<Int>) -> Int32) throws -> Data {
        guard let crypto else { throw PlankPencilWireError.crypto }
        var bytes = [UInt8](repeating:0,count:256), count = 0
        guard operation(crypto,&bytes,bytes.count,&count) == 0, count > 0, count <= bytes.count else { throw PlankPencilWireError.crypto }
        return Data(bytes.prefix(count))
    }
    private func code() throws -> String {
        guard let crypto else { throw PlankPencilWireError.crypto }
        var chars = [CChar](repeating:0,count:13)
        guard plank_pencil_crypto_code(crypto,&chars) == 0 else { throw PlankPencilWireError.crypto }
        let text = String(decoding:chars.prefix(12).map { UInt8(bitPattern:$0) },as:UTF8.self).uppercased()
        return stride(from:0,to:12,by:4).map { start in
            String(text.dropFirst(start).prefix(4))
        }.joined(separator:" ")
    }
    private func completeInitiator() throws {
        guard verified, secondReceived, stage == 1 else { return }
        // Pin only after the authenticated responder proved the advertised key
        // and the local user verified the first-message transcript code.
        try saveApproval(peerKey)
        openingConfiguration = configuration
        stage = 2; try secure(configuration)
    }
    private func read() {
        guard !closed else { return }
        connection.receive(minimumIncompleteLength:1,maximumLength:256) { [weak self] data,_,complete,error in
            guard let self, !self.closed else { return }
            do {
                if let data, !data.isEmpty {
                    guard self.buffer.count + data.count <= 512 else { throw PlankPencilWireError.overflow }
                    self.buffer.append(data)
                    while self.buffer.count >= 2 {
                        let length = Int(self.buffer[self.buffer.startIndex]) | Int(self.buffer[self.buffer.startIndex+1]) << 8
                        guard length > 0, length <= 256 else { throw PlankPencilWireError.invalid }
                        guard self.buffer.count >= length+2 else { break }
                        let body = Data(self.buffer.dropFirst(2).prefix(length))
                        self.buffer = Data(self.buffer.dropFirst(length+2))
                        try self.accept(body)
                        if self.closed { return }
                    }
                }
                if complete || error != nil { self.finish("Pencil connection ended"); return }
                self.read()
            } catch { self.finish("Pencil connection could not be verified") }
        }
    }
    private func accept(_ body: Data) throws {
        guard let crypto else { throw PlankPencilWireError.crypto }
        if !initiator && stage == 0 {
            guard body.starts(with:Self.prefix) else { throw PlankPencilWireError.invalid }
            let r = body.dropFirst(5).withUnsafeBytes { plank_pencil_crypto_accept_first(crypto,$0.bindMemory(to:UInt8.self).baseAddress,$0.count) }
            guard r == 0 else { throw PlankPencilWireError.crypto }
            var key = [UInt8](repeating:0,count:32)
            guard plank_pencil_crypto_peer(crypto,&key) == 0 else { throw PlankPencilWireError.crypto }
            peerKey = Data(key); stage = 1
            verified = try approvalLookup(peerKey)
            if verified { approve() } else { event(.verification(try code())) }
            return
        }
        if initiator && stage == 1 && !secondReceived {
            guard body.starts(with:Self.prefix) else { throw PlankPencilWireError.invalid }
            let r = body.dropFirst(5).withUnsafeBytes { plank_pencil_crypto_accept_second(crypto,$0.bindMemory(to:UInt8.self).baseAddress,$0.count) }
            guard r == 0 else { throw PlankPencilWireError.crypto }
            secondReceived = true; try completeInitiator(); return
        }
        guard stage >= 2 else { throw PlankPencilWireError.invalid }
        var plain = [UInt8](repeating:0,count:224), count = 0
        let r = body.withUnsafeBytes { plank_pencil_crypto_decrypt(crypto,$0.bindMemory(to:UInt8.self).baseAddress,$0.count,&plain,plain.count,&count) }
        guard r == 0 else { throw PlankPencilWireError.crypto }
        let message = try PlankPencilMessage.decode(Data(plain.prefix(count)))
        lastReceive = DispatchTime.now().uptimeNanoseconds
        if case .end = message { finish("Pencil sharing stopped"); return }
        if stage == 2 {
            guard case .configuration = message else { throw PlankPencilWireError.invalid }
            if !initiator {
                try saveApproval(peerKey)
                configuration = message; try secure(message)
            } else { guard openingConfiguration == message else { throw PlankPencilWireError.invalid } }
            stage = 3; event(.ready)
            if initiator && configuration != openingConfiguration { _ = offer(configuration) }
        }
        switch message {
        case .configuration:
            if initiator { guard message == configuration else { throw PlankPencilWireError.invalid }; return }
            configuration = message
            if case let .configuration(_,_,next) = message { active = next }
            stroke = PlankPencilStrokeState()
        case let .pen(p):
            guard initiator else { throw PlankPencilWireError.invalid }; try stroke.accept(p)
        case .rightClick:
            guard initiator, !stroke.touching else { throw PlankPencilWireError.invalid }
        case let .modifier(key,pressed):
            guard initiator else { throw PlankPencilWireError.invalid }
            try keys.accept([.init(code:key.rawValue,pressed:pressed)])
        case let .keys(events):
            guard initiator else { throw PlankPencilWireError.invalid }
            try keys.accept(events)
        case .ping: try secure(.pong); return
        case .pong: return
        case .end: return
        }
        let result = incoming.offer(message)
        guard result.accepted else { throw PlankPencilWireError.overflow }
        if result.wake { event(.messagesAvailable) }
    }
    private func secure(_ message: PlankPencilMessage) throws {
        let plain = try message.encoded()
        let body = try plain.withUnsafeBytes { input in
            try output { plank_pencil_crypto_encrypt($0,input.bindMemory(to:UInt8.self).baseAddress,input.count,$1,$2,$3) }
        }
        writeRecord(body)
    }
    private func writeRecord(_ body: Data) {
        guard !closed, body.count <= 256, writes.count < 4 else { finish("Pencil write limit reached"); return }
        var record = Data([UInt8(truncatingIfNeeded:body.count),UInt8(body.count >> 8)]); record.append(body)
        writes.append(record); sendNext()
    }
    private func sendNext() {
        guard !closed, !sending, !writes.isEmpty else { return }
        sending = true
        let record = writes.removeFirst()
        connection.send(content:record,completion:.contentProcessed { [weak self] error in
            guard let self, !self.closed else { return }
            self.sending = false
            if error != nil { self.finish("Pencil connection ended"); return }
            self.sendNext(); self.pump()
        })
    }
    private func pump() {
        guard !closed, stage == 3, !sending, writes.isEmpty else { return }
        do {
            guard let next = try outgoing.take() else { return }
            try secure(next)
        } catch { finish("Pencil input queue ended") }
    }
    private func tick() {
        guard !closed else { return }
        let now = DispatchTime.now().uptimeNanoseconds
        if stage < 3 {
            if now >= deadline { finish("Pencil approval timed out") }
        } else if now-lastReceive >= 5_000_000_000 { finish("Pencil connection timed out") }
        else if now-lastPing >= 1_000_000_000 { lastPing = now; _ = offer(.ping) }
    }
    private func finish(_ reason: String) {
        guard !closed else { return }; closed = true
        outgoing.close(); incoming.close(); timer?.cancel(); timer = nil
        connection.stateUpdateHandler = nil; connection.cancel(); buffer.removeAll(); writes.removeAll()
        if let crypto { plank_pencil_crypto_destroy(crypto); self.crypto = nil }
        event(.ended(reason))
    }
}
