import Foundation

@main struct PencilImageWireChecks {
    static func main() throws {
        let id=UUID()
        func descriptor(_ number:UInt64=1,_ generation:UInt64=1)->PlankPencilImageDescriptor {
            .init(desktopID:id,geometryGeneration:generation,frameNumber:number,
                  sourceWidth:3840,sourceHeight:2160,encodedWidth:1920,encodedHeight:1080)
        }
        func rejects(_ block:()->Void) { block() }
        func fails(_ block:() throws->Void) {
            do { try block(); assertionFailure("Invalid media admission") } catch { }
        }
        _ = try descriptor().validated()
        fails { _ = try descriptor(0).validated() }
        fails { _ = try descriptor(1,0).validated() }
        for dimensions in [(0,2160,1920,1080),(16385,2,1920,2),(16384,16384,1920,1920),
                           (3840,2160,1921,1080),(3840,2160,1920,1920),(2,2,1920,1920)] {
            fails { _ = try PlankPencilImageDescriptor(desktopID:id,geometryGeneration:1,frameNumber:1,
                sourceWidth:dimensions.0,sourceHeight:dimensions.1,encodedWidth:dimensions.2,encodedHeight:dimensions.3).validated() }
        }
        let bytes=Data(repeating:0xa5,count:PlankPencilImageLimits.maximumFrameBytes)
        let frame=PlankPencilImageFrame(descriptor:descriptor(),jpegData:bytes,receivedAtUptimeNanoseconds:0)
        _ = try frame.validated()
        fails { _ = try PlankPencilImageFrame(descriptor:descriptor(),jpegData:Data(),receivedAtUptimeNanoseconds:0).validated() }
        fails { _ = try PlankPencilImageFrame(descriptor:descriptor(),jpegData:bytes+Data([0]),receivedAtUptimeNanoseconds:0).validated() }
        var assembler=PlankPencilImageAssembler(), final:PlankPencilImageFrame?
        var first:PlankPencilImageChunk?
        for offset in stride(from:0,to:bytes.count,by:PlankPencilImageLimits.maximumChunkBytes) {
            let chunk=PlankPencilImageChunk(descriptor:descriptor(),totalBytes:bytes.count,offset:offset,
                bytes:Data(bytes.dropFirst(offset).prefix(PlankPencilImageLimits.maximumChunkBytes)))
            if first == nil { first=chunk }
            let encoded=try PlankPencilImageRecord.chunk(chunk).encoded()
            assert(encoded.count <= PlankPencilImageLimits.maximumPlaintextBytes)
            let decoded=try PlankPencilImageRecord.decode(encoded)
            assert(decoded == .chunk(chunk))
            if case let .chunk(c)=decoded { final=try assembler.accept(c,now:1_000_000_000+UInt64(offset)) }
            assert(assembler.pendingBytes <= PlankPencilImageLimits.maximumFrameBytes)
        }
        assert(final?.descriptor == descriptor() && final?.jpegData == bytes)
        assert(final!.receivedAtUptimeNanoseconds == 1_000_000_000)
        assert(!assembler.hasIncompleteFrame && assembler.pendingBytes == 0)
        fails { _ = try assembler.accept(first!,now:2_000_000_000) } // completed frame replay
        for record in [PlankPencilImageRecord.ready,.end,.ping,.pong] {
            let roundtrip=try PlankPencilImageRecord.decode(record.encoded()); assert(roundtrip == record)
        }
        fails { _ = try PlankPencilImageRecord.decode(Data([99])) }
        fails { _ = try PlankPencilImageRecord.decode(Data([3,0])) }
        fails { _ = try PlankPencilImageRecord.decode(Data(repeating:1,count:7681)) }
        var wrongLength=try PlankPencilImageRecord.chunk(first!).encoded(); wrongLength[57]=0; wrongLength[58]=0
        fails { _ = try PlankPencilImageRecord.decode(wrongLength) }
        var hugeTotal=try PlankPencilImageRecord.chunk(first!).encoded()
        for i in 49..<53 { hugeTotal[i]=255 }
        fails { _ = try PlankPencilImageRecord.decode(hugeTotal) }
        var partial=PlankPencilImageAssembler()
        let started=try partial.accept(first!,now:100); assert(started == nil)
        fails { _ = try partial.accept(first!,now:101) } // chunk duplication
        let second=PlankPencilImageChunk(descriptor:descriptor(),totalBytes:bytes.count,
            offset:PlankPencilImageLimits.maximumChunkBytes,bytes:first!.bytes)
        var empty=PlankPencilImageAssembler()
        fails { _ = try empty.accept(second,now:1) } // cannot start in middle
        var changed=second
        changed = .init(descriptor:descriptor(2),totalBytes:bytes.count,offset:second.offset,bytes:second.bytes)
        fails { _ = try partial.accept(changed,now:102) }
        fails { try partial.checkDeadline(now:PlankPencilImageLimits.assemblyTimeoutNanoseconds+101) }
        fails { try partial.checkDeadline(now:99) }
        partial.clear(); assert(!partial.hasIncompleteFrame && partial.pendingBytes == 0)
        // A valid but slow final chunk cannot make an old local frame fresh.
        // Presentation's age begins at authenticated offset0, not completion.
        var delayed=PlankPencilImageAssembler()
        let smallTotal=PlankPencilImageLimits.maximumChunkBytes+3
        let startChunk=PlankPencilImageChunk(descriptor:descriptor(2),totalBytes:smallTotal,offset:0,bytes:first!.bytes)
        let endChunk=PlankPencilImageChunk(descriptor:descriptor(2),totalBytes:smallTotal,
            offset:PlankPencilImageLimits.maximumChunkBytes,bytes:Data([7,8,9]))
        _ = try delayed.accept(startChunk,now:1_000_000_000)
        let lateFrame=try delayed.accept(endChunk,now:2_600_000_000)
        assert(lateFrame?.receivedAtUptimeNanoseconds == 1_000_000_000)
        assert(2_600_000_000-lateFrame!.receivedAtUptimeNanoseconds >= 1_500_000_000)
        let box=PlankPencilImageMailbox()
        for i in 1...10_000 {
            let replacement=PlankPencilImageFrame(descriptor:descriptor(UInt64(i)),jpegData:Data([1]),receivedAtUptimeNanoseconds:0)
            let result=box.offer(replacement)
            assert(result.accepted && result.wake == (i == 1))
        }
        assert(box.take()?.descriptor.frameNumber == 10_000)
        assert(box.take() == nil)
        assert(box.offer(frame).wake)
        box.close(); assert(box.take() == nil && !box.offer(frame).accepted)
        rejects { assert(!box.offer(.init(descriptor:descriptor(),jpegData:Data(),receivedAtUptimeNanoseconds:0)).accepted) }
        print("Image identity geometry, exact codec, max frame/chunk bounds, sequence/replay, deadline and latest-only admission passed")
    }
}
