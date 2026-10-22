import Foundation
import Network

// Loopback only, injected in-memory consent store; no device, Keychain or Host.
final class PencilSocketCheck: @unchecked Sendable {
    let lock = NSLock(), done = DispatchSemaphore(value:0)
    var client: PlankPencilRelayPeer?, server: PlankPencilRelayPeer?
    var clientCode: String?, serverCode: String?
    var clientSaved = false, serverSaved = false, sent = false, finished = false
    var received: [PlankPencilMessage] = []
    let a = Data([UInt8(1)] + [UInt8](repeating:0,count:31))
    let b = Data([UInt8(2)] + [UInt8](repeating:0,count:31))
    var expected: [PlankPencilMessage] {
        let down = PlankNormalizedPen(phase:.down,x:0.2,y:0.3,pressureOrDistance:0.7,tilt:0,rotation:0)
        var move = down; move.phase = .move; move.x = 0.9
        var up = move; up.phase = .up; up.pressureOrDistance = 0
        let maximumDown = PlankControlKeyCatalog.entries.prefix(48).map { PlankPencilKeyEvent(code:$0.code,pressed:true) }
        let maximumUp = maximumDown.reversed().map { PlankPencilKeyEvent(code:$0.code,pressed:false) }
        return [.keys([.init(code:0x10,pressed:true,modifiers:1),.init(code:0x20,pressed:true,modifiers:1)]),
            .pen(down),.pen(move),.pen(up),
            .keys([.init(code:0x20,pressed:false,modifiers:1),.init(code:0x10,pressed:false)]),
            .keys([.init(code:0x11,pressed:true,modifiers:2),.init(code:0x5A,pressed:true,modifiers:2),
                .init(code:0x5A,pressed:false,modifiers:2),.init(code:0x11,pressed:false)]),
            .keys(maximumDown),.keys(maximumUp)]
    }
    func run() throws {
        let listener = try NWListener(using:.tcp,on:.any)
        listener.newConnectionHandler = { connection in
            do {
                let peer = try PlankPencilRelayPeer(connection:connection,privateKey:self.b,
                    approvalLookup: { _ in false },saveApproval: { _ in self.lock.lock(); self.serverSaved = true; self.lock.unlock() }) { event in self.handle(event,server:true) }
                self.lock.lock(); self.server = peer; self.lock.unlock(); peer.start()
            } catch { fatalError("Server creation failed") }
        }
        listener.stateUpdateHandler = { state in
            if case .ready = state, let port = listener.port {
                do {
                    let key = try PlankPencilRelayKeys.publicKey(self.b)
                    assert(PlankPencilRelayKeys.approvalAccount(key) == "peer-v3-" + PlankPencilRelayKeys.hex(key))
                    let peer = try PlankPencilRelayPeer(connection:NWConnection(host:"127.0.0.1",port:port,using:.tcp),
                        privateKey:self.a,peerKey:key,approvalLookup: { _ in false },saveApproval: { _ in self.lock.lock(); self.clientSaved = true; self.lock.unlock() }) { event in self.handle(event,server:false) }
                    self.lock.lock(); self.client = peer; self.lock.unlock(); peer.start()
                } catch { fatalError("Client creation failed") }
            }
        }
        listener.start(queue:DispatchQueue(label:"pencil.test.listener"))
        assert(done.wait(timeout:.now() + .seconds(10)) == .success,"Socket fixture timed out")
        listener.cancel(); client?.close(); server?.close()
        lock.lock(); defer { lock.unlock() }
        assert(clientSaved && serverSaved)
        assert(received == expected)
    }
    func handle(_ event: PlankPencilPeerEvent,server isServer: Bool) {
        switch event {
        case let .verification(code):
            lock.lock()
            if isServer { serverCode = code } else { clientCode = code }
            let both = serverCode != nil && clientCode != nil
            if both { assert(serverCode == clientCode); assert(!clientSaved && !serverSaved) }
            let s = server, c = client; lock.unlock()
            if both { s?.approve(); c?.approve() }
        case .ready: break
        case .messagesAvailable:
            if isServer {
                while let message = try! server?.incoming.take() {
                    guard case .configuration = message else { fatalError("Wrong direction") }
                    lock.lock(); let shouldSend = !sent; sent = true; lock.unlock()
                    if shouldSend {
                        assert(!server!.offer(.keys([])))
                        assert(!server!.offer(.keys([.init(code:0xFFFF,pressed:true)])))
                        assert(!server!.offer(.keys(Array(repeating:.init(code:0x41,pressed:true),count:49))))
                        // Invalid admission leaves the authenticated peer alive
                        // and does not enqueue a valid prefix of the batch.
                        for value in expected { assert(server!.offer(value)) }
                    }
                }
            } else {
                while let message = try! client?.incoming.take() {
                    lock.lock(); received.append(message)
                    let complete = received.count == expected.count; if complete { finished = true }
                    lock.unlock(); if complete { done.signal() }
                }
            }
        case let .ended(reason):
            lock.lock(); let okay = finished; lock.unlock()
            assert(okay,"Unexpected socket close: \(reason)")
        }
    }
}
@main struct PencilSocketMain {
    static func main() throws { try PencilSocketCheck().run(); print("Real loopback IK/physical-consent/configuration/ordered generic-key batches + modifier-held pressure stroke passed") }
}
