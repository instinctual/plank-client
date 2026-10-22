import Foundation
import Network

// Actual loopback TCP only. No device/Host, persistent Keychain or production
// listeners. Synthetic consent bindings stand for a current approved session;
// the C fixture separately proves deriving their full final transcript.
final class PencilImageSocketCheck: @unchecked Sendable {
    private let lock=NSLock(), done=DispatchSemaphore(value:0)
    private var sender:PlankPencilImagePeer?, receiver:PlankPencilImagePeer?
    private var readyCount=0, received:[UInt64]=[], deliveryTimes:[UInt64]=[]
    private var finished=false, offeredReplacement=false
    private var sentAt:UInt64=0
    private let a=Data([UInt8(16)]+[UInt8](repeating:0,count:31))
    private let b=Data([UInt8(32)]+[UInt8](repeating:0,count:31))
    private let hash=Data((0..<64).map(UInt8.init))
    private let desktop=UUID()
    private var listener:NWListener?
    func frame(_ number:UInt64,bytes:Int)->PlankPencilImageFrame {
        .init(descriptor:.init(desktopID:desktop,geometryGeneration:1,frameNumber:number,
            sourceWidth:3840,sourceHeight:2160,encodedWidth:1920,encodedHeight:1080),
            jpegData:Data(repeating:UInt8(truncatingIfNeeded:number),count:bytes),receivedAtUptimeNanoseconds:0)
    }
    func run() throws {
        let server=try NWListener(using:.tcp,on:.any); listener=server
        server.newConnectionHandler = { [self] connection in
            do {
                let key=try PlankPencilRelayKeys.publicKey(a)
                let peer=try PlankPencilImagePeer(connection:connection,privateKey:b,
                    binding:.init(peerKey:key,transcriptHash:hash),initiator:false) { [self] in handle($0,isSender:false) }
                assert(!peer.offer(frame(1,bytes:1)),"Receiver cannot offer a reverse pixel capability")
                lock.lock(); receiver=peer; lock.unlock(); peer.start()
            } catch { fatalError("Image receiver creation failed") }
        }
        server.stateUpdateHandler = { [self] state in
            if case .ready=state, let port=server.port {
                do {
                    let key=try PlankPencilRelayKeys.publicKey(b)
                    let peer=try PlankPencilImagePeer(connection:NWConnection(host:"127.0.0.1",port:port,using:.tcp),privateKey:a,
                        binding:.init(peerKey:key,transcriptHash:hash),initiator:true) { [self] in handle($0,isSender:true) }
                    assert(!peer.offer(frame(1,bytes:1)),"Image admission must await both bound handshake directions")
                    lock.lock(); sender=peer; lock.unlock(); peer.start()
                } catch { fatalError("Image sender creation failed") }
            }
        }
        server.start(queue:DispatchQueue(label:"pencil.image.test.listener",qos:.utility))
        assert(done.wait(timeout:.now() + .seconds(10)) == .success,"Image loopback timed out")
        lock.lock(); let s=sender,r=receiver,frames=received,times=deliveryTimes; finished=true; lock.unlock()
        s?.close(); r?.close(); server.cancel(); listener=nil
        assert(!s!.offer(frame(200,bytes:1)),"Closing revokes admission before a queued transport cleanup")
        assert(readyCount == 2 && frames.first == 1 && frames.last == 101)
        assert(frames == frames.sorted() && frames.count <= 4,"Pending latest frames must not become100 queued writes")
        for i in 1..<times.count { assert(times[i]-times[i-1] >= 100_000_000,"Callbacks must stay bounded to10fps") }
    }
    private func handle(_ event:PlankPencilImagePeerEvent,isSender:Bool) {
        switch event {
        case .ready:
            lock.lock(); readyCount += 1; let peer=sender; lock.unlock()
            if isSender {
                assert(peer != nil)
                assert(!peer!.offer(frame(0,bytes:1)))
                assert(!peer!.offer(frame(1,bytes:PlankPencilImageLimits.maximumFrameBytes+1)))
                lock.lock(); sentAt=DispatchTime.now().uptimeNanoseconds; lock.unlock()
                assert(peer!.offer(frame(1,bytes:PlankPencilImageLimits.maximumFrameBytes)))
            }
        case let .frame(value):
            assert(!isSender,"Only the consented receiving role receives pixels")
            let now=DispatchTime.now().uptimeNanoseconds
            lock.lock()
            received.append(value.descriptor.frameNumber); deliveryTimes.append(now)
            let shouldOffer = !offeredReplacement; offeredReplacement=true
            let peer=sender, elapsed=now-sentAt
            let complete=value.descriptor.frameNumber == 101
            if complete { finished=true }
            lock.unlock()
            assert(value.receivedAtUptimeNanoseconds > 0 && value.receivedAtUptimeNanoseconds <= now)
            if value.descriptor.frameNumber == 1 {
                assert(value.jpegData == frame(1,bytes:PlankPencilImageLimits.maximumFrameBytes).jpegData)
                assert(elapsed >= 850_000_000,"Image cap cannot burst the entire2MiB frame")
            }
            if shouldOffer {
                for i in 2...101 { assert(peer!.offer(frame(UInt64(i),bytes:1))) }
            }
            if complete { done.signal() }
        case .ended:
            lock.lock(); let okay=finished; lock.unlock(); assert(okay,"Unexpected image close")
        }
    }
}

@main struct PencilImageSocketMain {
    static func main() throws {
        try PencilSessionBindingCheck().run()
        try PencilImageSocketCheck().run()
        print("Real TCP current-approved pen binding, bound media IK, direction/admission, max2MiB chunks, byte-rate cap, latest replacement and10fps callback bounds passed")
    }
}

final class PencilSessionBindingCheck: @unchecked Sendable {
    private let lock=NSLock(), done=DispatchSemaphore(value:0)
    private var client:PlankPencilRelayPeer?, server:PlankPencilRelayPeer?
    private var clientBinding:PlankPencilSessionBinding?, serverBinding:PlankPencilSessionBinding?
    private var readyCount=0, closing=false, checkedUnapproved=false
    private let a=Data([UInt8(16)]+[UInt8](repeating:0,count:31))
    private let b=Data([UInt8(32)]+[UInt8](repeating:0,count:31))
    func run() throws {
        let listener=try NWListener(using:.tcp,on:.any)
        listener.newConnectionHandler = { [self] connection in
            do {
                let peer=try PlankPencilRelayPeer(connection:connection,privateKey:b,
                    approvalLookup:{ _ in false },saveApproval:{ _ in }) { [self] in handle($0,isServer:true) }
                lock.lock(); server=peer; lock.unlock(); peer.start()
            } catch { fatalError("Pen binding receiver creation failed") }
        }
        listener.stateUpdateHandler = { [self] state in
            if case .ready=state, let port=listener.port {
                do {
                    let peer=try PlankPencilRelayPeer(connection:NWConnection(host:"127.0.0.1",port:port,using:.tcp),
                        privateKey:a,peerKey:PlankPencilRelayKeys.publicKey(b),approvalLookup:{ _ in true },saveApproval:{ _ in }) {
                        [self] in handle($0,isServer:false)
                    }
                    lock.lock(); client=peer; lock.unlock()
                    peer.sessionBinding { binding in assert(binding == nil,"An existing pin before ready has no image authority"); peer.start() }
                } catch { fatalError("Pen binding sender creation failed") }
            }
        }
        listener.start(queue:DispatchQueue(label:"pen.binding.test.listener"))
        assert(done.wait(timeout:.now() + .seconds(5)) == .success,"Pen binding fixture timed out")
        lock.lock()
        assert(checkedUnapproved && readyCount == 2)
        assert(clientBinding?.transcriptHash.count == 64 && clientBinding?.transcriptHash == serverBinding?.transcriptHash)
        assert(clientBinding?.peerKey == (try! PlankPencilRelayKeys.publicKey(b)))
        assert(serverBinding?.peerKey == (try! PlankPencilRelayKeys.publicKey(a)))
        closing=true; let c=client!,s=server!; lock.unlock()
        c.close(); s.close(); listener.cancel()
        let retired=DispatchSemaphore(value:0)
        c.sessionBinding { binding in assert(binding == nil,"Closed current session cannot donate image authority"); retired.signal() }
        assert(retired.wait(timeout:.now() + .seconds(2)) == .success)
    }
    private func handle(_ event:PlankPencilPeerEvent,isServer:Bool) {
        switch event {
        case .verification:
            assert(isServer)
            lock.lock(); let peer=server!; lock.unlock()
            peer.sessionBinding { [self] binding in
                assert(binding == nil,"Unapproved stage1 transcript cannot authorize images")
                lock.lock(); checkedUnapproved=true; lock.unlock(); peer.approve()
            }
        case .ready:
            lock.lock(); readyCount += 1; let c=client!,s=server!,both=readyCount == 2; lock.unlock()
            if both {
                c.sessionBinding { [self] binding in
                    lock.lock(); clientBinding=binding; let complete=clientBinding != nil && serverBinding != nil; lock.unlock()
                    if complete { done.signal() }
                }
                s.sessionBinding { [self] binding in
                    lock.lock(); serverBinding=binding; let complete=clientBinding != nil && serverBinding != nil; lock.unlock()
                    if complete { done.signal() }
                }
            }
        case .messagesAvailable: break
        case .ended:
            lock.lock(); let expected=closing; lock.unlock(); assert(expected,"Pen binding unexpectedly ended")
        }
    }
}
