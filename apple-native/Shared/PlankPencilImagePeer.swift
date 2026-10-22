import Foundation
import Network

enum PlankPencilImagePeerEvent: Sendable {
    case ready
    case frame(PlankPencilImageFrame)
    case ended(String)
}

// Separate utility executor, connection and bounded image mailboxes. This peer
// owns no pen peer and cannot close or delay its input socket. Each encrypted
// chunk waits for its completion; latest replacement precedes encryption.
final class PlankPencilImagePeer: @unchecked Sendable {
    private let queue = DispatchQueue(label:"la.instinctual.plank.pencil.image.peer",qos:.utility)
    private let callbackQueue = DispatchQueue(label:"la.instinctual.plank.pencil.image.delivery",qos:.utility)
    private let connection: NWConnection
    private let initiator: Bool
    private let event: @Sendable (PlankPencilImagePeerEvent)->Void
    private var crypto: OpaquePointer?
    private var stage = 0, started = false, closed = false, sending = false, pumpScheduled = false
    private var buffer = Data()
    private var writes: [PendingWrite] = []
    private var currentFrame: PlankPencilImageFrame?
    private var currentOffset = 0
    private var lastFrameNumber: UInt64 = 0
    private var nextFrameAt: UInt64 = 0, nextChunkAt: UInt64 = 0
    private var assembler = PlankPencilImageAssembler()
    private let outgoing = PlankPencilImageMailbox()
    private let callbacks = PlankPencilImageMailbox()
    // Owned only by callbackQueue; never compared with a sender timestamp.
    private var nextDeliveryAt: UInt64 = 0
    private let admissionLock = NSLock()
    private var admitting = false
    private var timer: DispatchSourceTimer?
    private var deadline: UInt64 = 0, lastReceive: UInt64 = 0, lastPing: UInt64 = 0
    private static let prefix = Data([0x50,0x4c,0x49,0x4d,1])
    private enum PendingWrite { case handshake(Data), secure(PlankPencilImageRecord) }

    init(connection:NWConnection,privateKey:Data,binding:PlankPencilSessionBinding,initiator:Bool,
         event:@escaping @Sendable (PlankPencilImagePeerEvent)->Void) throws {
        guard privateKey.count == 32, binding.peerKey.count == 32, binding.transcriptHash.count == 64 else {
            throw PlankPencilImageError.invalid
        }
        self.connection=connection; self.initiator=initiator; self.event=event
        let result=privateKey.withUnsafeBytes { key in
            binding.peerKey.withUnsafeBytes { peer in
                binding.transcriptHash.withUnsafeBytes { hash in
                    plank_pencil_image_crypto_create(initiator ? 1 : 0,
                        key.bindMemory(to:UInt8.self).baseAddress,
                        peer.bindMemory(to:UInt8.self).baseAddress,
                        hash.bindMemory(to:UInt8.self).baseAddress)
                }
            }
        }
        guard let result else { throw PlankPencilImageError.crypto }; crypto=result
    }
    deinit { if let crypto { plank_pencil_image_crypto_destroy(crypto) } }
    func start() {
        queue.async { [self] in
            guard !started, !closed else { return }; started=true
            let now=DispatchTime.now().uptimeNanoseconds
            deadline=now+10_000_000_000; lastReceive=now; lastPing=now
            connection.stateUpdateHandler = { [weak self] state in
                guard let self, !self.closed else { return }
                switch state {
                case .ready:
                    do {
                        if self.initiator {
                            self.enqueue(.handshake(Self.prefix + (try self.output {
                                plank_pencil_image_crypto_first($0,$1,$2,$3)
                            })))
                        }
                        self.read()
                    } catch { self.finish("Reference image could not be verified") }
                case .failed, .cancelled: self.finish("Reference image connection ended")
                default: break
                }
            }
            connection.start(queue:queue)
            let tick=DispatchSource.makeTimerSource(queue:queue); timer=tick
            tick.schedule(deadline:.now() + .milliseconds(250),repeating:.milliseconds(250))
            tick.setEventHandler { [weak self] in self?.tick() }; tick.resume()
        }
    }
    @discardableResult func offer(_ frame:PlankPencilImageFrame)->Bool {
        guard initiator else { return false }
        admissionLock.lock()
        guard admitting else { admissionLock.unlock(); return false }
        let result=outgoing.offer(frame)
        admissionLock.unlock()
        if result.wake { queue.async { [self] in pump() } }
        return result.accepted
    }
    func close() {
        admissionLock.lock(); admitting=false; admissionLock.unlock()
        outgoing.close(); callbacks.close()
        queue.async { [self] in finish("Reference image stopped") }
    }
    private func output(_ operation:(OpaquePointer,UnsafeMutablePointer<UInt8>,Int,UnsafeMutablePointer<Int>)->Int32) throws -> Data {
        guard let crypto else { throw PlankPencilImageError.crypto }
        var bytes=[UInt8](repeating:0,count:PlankPencilImageLimits.maximumCiphertextBytes), count=0
        guard operation(crypto,&bytes,bytes.count,&count) == 0, count > 0, count <= bytes.count else {
            throw PlankPencilImageError.crypto
        }
        return Data(bytes.prefix(count))
    }
    private func read() {
        guard !closed else { return }
        connection.receive(minimumIncompleteLength:1,maximumLength:8_192) { [weak self] data,_,complete,error in
            guard let self, !self.closed else { return }
            do {
                if let data, !data.isEmpty {
                    guard self.buffer.count + data.count <= 2 * (PlankPencilImageLimits.maximumCiphertextBytes+2) + 8_192 else {
                        throw PlankPencilImageError.overflow
                    }
                    self.buffer.append(data)
                    while self.buffer.count >= 2 {
                        let i=self.buffer.startIndex
                        let count=Int(self.buffer[i]) | Int(self.buffer[i+1]) << 8
                        guard count > 0, count <= PlankPencilImageLimits.maximumCiphertextBytes else { throw PlankPencilImageError.invalid }
                        guard self.buffer.count >= count+2 else { break }
                        let body=Data(self.buffer.dropFirst(2).prefix(count))
                        self.buffer=Data(self.buffer.dropFirst(count+2))
                        try self.accept(body)
                        if self.closed { return }
                    }
                }
                if complete || error != nil { self.finish("Reference image connection ended"); return }
                self.read()
            } catch { self.finish("Reference image could not be verified") }
        }
    }
    private func accept(_ body:Data) throws {
        guard let crypto else { throw PlankPencilImageError.crypto }
        if stage == 0 {
            guard body.starts(with:Self.prefix), body.count <= 256 else { throw PlankPencilImageError.invalid }
            let result=body.dropFirst(5).withUnsafeBytes { bytes in
                if initiator {
                    return plank_pencil_image_crypto_accept_second(crypto,bytes.bindMemory(to:UInt8.self).baseAddress,bytes.count)
                }
                return plank_pencil_image_crypto_accept_first(crypto,bytes.bindMemory(to:UInt8.self).baseAddress,bytes.count)
            }
            guard result == 0 else { throw PlankPencilImageError.crypto }
            stage=1
            if initiator { enqueue(.secure(.ready)) }
            else { enqueue(.handshake(Self.prefix + (try output { plank_pencil_image_crypto_second($0,$1,$2,$3) }))) }
            return
        }
        var plain=[UInt8](repeating:0,count:PlankPencilImageLimits.maximumPlaintextBytes), count=0
        let result=body.withUnsafeBytes { plank_pencil_image_crypto_decrypt(crypto,$0.bindMemory(to:UInt8.self).baseAddress,$0.count,&plain,plain.count,&count) }
        guard result == 0, count > 0, count <= plain.count else { throw PlankPencilImageError.crypto }
        let record=try PlankPencilImageRecord.decode(Data(plain.prefix(count)))
        let now=DispatchTime.now().uptimeNanoseconds; lastReceive=now
        if stage == 1 {
            guard record == .ready else { throw PlankPencilImageError.invalid }
            if !initiator { enqueue(.secure(.ready)) }
            stage=2
            admissionLock.lock(); admitting=true; admissionLock.unlock()
            callbackQueue.async { [event] in event(.ready) }
            pump(); return
        }
        guard stage == 2 else { throw PlankPencilImageError.invalid }
        switch record {
        case let .chunk(c):
            guard !initiator else { throw PlankPencilImageError.invalid }
            if let frame=try assembler.accept(c,now:now) { deliver(frame) }
        case .ping: enqueue(.secure(.pong))
        case .pong: break
        case .end: finish("Reference image stopped")
        case .ready: throw PlankPencilImageError.invalid
        }
    }
    private func deliver(_ frame:PlankPencilImageFrame) {
        let result=callbacks.offer(frame)
        if result.wake { callbackQueue.async { [self] in drainCallbacks() } }
    }
    private func drainCallbacks() {
        // One executing callback and one latest complete frame; deliver at most
        //10fps even if TCP batches several completed frames together. Consumers
        // retain latest-only discipline when handing to a display actor.
        let now=DispatchTime.now().uptimeNanoseconds
        if now < nextDeliveryAt {
            callbackQueue.asyncAfter(deadline:DispatchTime(uptimeNanoseconds:nextDeliveryAt)) { [self] in drainCallbacks() }
            return
        }
        guard let frame=callbacks.take() else { return }
        event(.frame(frame))
        nextDeliveryAt=DispatchTime.now().uptimeNanoseconds + PlankPencilImageLimits.minimumFrameIntervalNanoseconds
        callbackQueue.asyncAfter(deadline:DispatchTime(uptimeNanoseconds:nextDeliveryAt)) { [self] in drainCallbacks() }
    }
    private func enqueue(_ write:PendingWrite) {
        guard !closed else { return }
        guard writes.count < 4 else { finish("Reference image control limit reached"); return }
        writes.append(write); pump()
    }
    private func pump() {
        guard !closed, !sending else { return }
        do {
            if !writes.isEmpty {
                let write=writes.removeFirst()
                switch write {
                case let .handshake(body): send(body)
                case let .secure(record): try secure(record)
                }
                return
            }
            guard initiator, stage == 2 else { return }
            let now=DispatchTime.now().uptimeNanoseconds
            if currentFrame == nil {
                if now < nextFrameAt { schedulePump(at:nextFrameAt); return }
                while let candidate=outgoing.take() {
                    guard candidate.descriptor.frameNumber > lastFrameNumber else { continue }
                    currentFrame=candidate; currentOffset=0
                    lastFrameNumber=candidate.descriptor.frameNumber
                    nextFrameAt=now+PlankPencilImageLimits.minimumFrameIntervalNanoseconds
                    break
                }
            }
            guard let currentFrame else { return }
            if now < nextChunkAt { schedulePump(at:nextChunkAt); return }
            let count=min(PlankPencilImageLimits.maximumChunkBytes,currentFrame.jpegData.count-currentOffset)
            let chunk=PlankPencilImageChunk(descriptor:currentFrame.descriptor,totalBytes:currentFrame.jpegData.count,
                offset:currentOffset,bytes:Data(currentFrame.jpegData.dropFirst(currentOffset).prefix(count)))
            // No plaintext frame replacement from this point until all its
            // authenticated chunks complete in sequence.
            currentOffset += count
            if currentOffset == currentFrame.jpegData.count { self.currentFrame=nil; currentOffset=0 }
            try secure(.chunk(chunk))
        } catch { finish("Reference image queue ended") }
    }
    private func schedulePump(at deadline:UInt64) {
        guard !pumpScheduled else { return }; pumpScheduled=true
        queue.asyncAfter(deadline:DispatchTime(uptimeNanoseconds:deadline)) { [self] in
            pumpScheduled=false; pump()
        }
    }
    private func secure(_ record:PlankPencilImageRecord) throws {
        let plain=try record.encoded()
        let body=try plain.withUnsafeBytes { bytes in
            try output { plank_pencil_image_crypto_encrypt($0,bytes.bindMemory(to:UInt8.self).baseAddress,bytes.count,$1,$2,$3) }
        }
        send(body)
    }
    private func send(_ body:Data) {
        guard !closed, !sending, !body.isEmpty, body.count <= PlankPencilImageLimits.maximumCiphertextBytes else {
            finish("Reference image write limit reached"); return
        }
        var record=Data([UInt8(truncatingIfNeeded:body.count),UInt8(body.count >> 8)]); record.append(body)
        sending=true
        // Rate includes record headers/tags/control bytes. A single record may
        // burst; subsequent records wait on both socket completion and budget.
        let now=DispatchTime.now().uptimeNanoseconds
        let budget=UInt64(record.count) * 1_000_000_000 / UInt64(PlankPencilImageLimits.maximumBytesPerSecond)
        nextChunkAt=max(now,nextChunkAt)+max(1,budget)
        connection.send(content:record,completion:.contentProcessed { [weak self] error in
            guard let self, !self.closed else { return }; self.sending=false
            if error != nil { self.finish("Reference image connection ended"); return }
            self.pump()
        })
    }
    private func tick() {
        guard !closed else { return }
        let now=DispatchTime.now().uptimeNanoseconds
        if stage < 2 {
            if now >= deadline { finish("Reference image verification timed out") }
            return
        }
        do { try assembler.checkDeadline(now:now) }
        catch { finish("Reference image frame timed out"); return }
        if now-lastReceive >= 5_000_000_000 { finish("Reference image connection timed out") }
        else if now-lastPing >= 1_000_000_000 {
            lastPing=now; enqueue(.secure(.ping))
        }
    }
    private func finish(_ reason:String) {
        guard !closed else { return }; closed=true
        admissionLock.lock(); admitting=false; admissionLock.unlock()
        outgoing.close(); callbacks.close(); assembler.clear()
        timer?.cancel(); timer=nil; currentFrame=nil; writes.removeAll(); buffer.removeAll()
        connection.stateUpdateHandler=nil; connection.cancel()
        if let crypto { plank_pencil_image_crypto_destroy(crypto); self.crypto=nil }
        callbackQueue.async { [event] in event(.ended(reason)) }
    }
}
