import Foundation

enum PlankPencilImageLimits {
    static let maximumSourceDimension = 16_384
    static let maximumSourcePixels = 33_554_432
    static let maximumEncodedDimension = 1_920
    static let maximumEncodedPixels = 3_686_400
    static let maximumFrameBytes = 2 * 1_024 * 1_024
    static let maximumPlaintextBytes = 7_680
    static let maximumCiphertextBytes = maximumPlaintextBytes + 16
    static let maximumChunkBytes = maximumPlaintextBytes - 59
    static let maximumBytesPerSecond = 2 * 1_024 * 1_024
    static let minimumFrameIntervalNanoseconds: UInt64 = 100_000_000
    static let assemblyTimeoutNanoseconds: UInt64 = 3_000_000_000
}
enum PlankPencilImageError: Error { case invalid, overflow, crypto, timedOut }

// Identity crop is implicit and mandatory: the first protocol has no optional
// caller-selected crop/offset/transform fields to silently alter pen mapping.
struct PlankPencilImageDescriptor: Sendable, Equatable {
    let desktopID: UUID
    let geometryGeneration: UInt64
    let frameNumber: UInt64
    let sourceWidth: Int
    let sourceHeight: Int
    let encodedWidth: Int
    let encodedHeight: Int

    func validated() throws -> Self {
        guard geometryGeneration > 0, frameNumber > 0,
              (2...PlankPencilImageLimits.maximumSourceDimension).contains(sourceWidth),
              (2...PlankPencilImageLimits.maximumSourceDimension).contains(sourceHeight),
              sourceWidth * sourceHeight <= PlankPencilImageLimits.maximumSourcePixels,
              (2...PlankPencilImageLimits.maximumEncodedDimension).contains(encodedWidth),
              (2...PlankPencilImageLimits.maximumEncodedDimension).contains(encodedHeight),
              encodedWidth * encodedHeight <= PlankPencilImageLimits.maximumEncodedPixels,
              encodedWidth <= sourceWidth, encodedHeight <= sourceHeight,
              abs(sourceWidth * encodedHeight - sourceHeight * encodedWidth) <= max(sourceWidth,sourceHeight)
        else { throw PlankPencilImageError.invalid }
        return self
    }
}
struct PlankPencilImageFrame: Sendable, Equatable {
    let descriptor: PlankPencilImageDescriptor
    let jpegData: Data
    // Receiver replaces this with its own first authenticated chunk time, so
    // time spent assembling also counts toward stale-image presentation limits.
    // No sender clock is serialized, accepted or compared across machines.
    let receivedAtUptimeNanoseconds: UInt64
    func validated() throws -> Self {
        _ = try descriptor.validated()
        guard !jpegData.isEmpty, jpegData.count <= PlankPencilImageLimits.maximumFrameBytes else {
            throw PlankPencilImageError.overflow
        }
        return self
    }
}

struct PlankPencilImageChunk: Sendable, Equatable {
    let descriptor: PlankPencilImageDescriptor
    let totalBytes: Int
    let offset: Int
    let bytes: Data
    func validated() throws -> Self {
        _ = try descriptor.validated()
        guard (1...PlankPencilImageLimits.maximumFrameBytes).contains(totalBytes),
              offset >= 0, offset < totalBytes,
              !bytes.isEmpty, bytes.count <= PlankPencilImageLimits.maximumChunkBytes,
              bytes.count <= totalBytes - offset,
              offset % PlankPencilImageLimits.maximumChunkBytes == 0,
              bytes.count == min(PlankPencilImageLimits.maximumChunkBytes,totalBytes-offset)
        else { throw PlankPencilImageError.invalid }
        return self
    }
}

enum PlankPencilImageRecord: Sendable, Equatable {
    case ready, chunk(PlankPencilImageChunk), end, ping, pong
    func encoded() throws -> Data {
        switch self {
        case .ready: return Data([0])
        case .end: return Data([2])
        case .ping: return Data([3])
        case .pong: return Data([4])
        case let .chunk(c):
            _ = try c.validated()
            var out = Data([1])
            var uuid = c.descriptor.desktopID.uuid
            withUnsafeBytes(of:&uuid) { out.append(contentsOf:$0) }
            func u64(_ value:UInt64) { for i in 0..<8 { out.append(UInt8(truncatingIfNeeded:value >> (8*i))) } }
            func u32(_ value:Int) { for i in 0..<4 { out.append(UInt8(truncatingIfNeeded:value >> (8*i))) } }
            u64(c.descriptor.geometryGeneration); u64(c.descriptor.frameNumber)
            u32(c.descriptor.sourceWidth); u32(c.descriptor.sourceHeight)
            u32(c.descriptor.encodedWidth); u32(c.descriptor.encodedHeight)
            u32(c.totalBytes); u32(c.offset)
            out.append(UInt8(truncatingIfNeeded:c.bytes.count)); out.append(UInt8(c.bytes.count >> 8))
            out.append(c.bytes)
            guard out.count <= PlankPencilImageLimits.maximumPlaintextBytes else { throw PlankPencilImageError.overflow }
            return out
        }
    }
    static func decode(_ data:Data) throws -> Self {
        guard !data.isEmpty, data.count <= PlankPencilImageLimits.maximumPlaintextBytes else { throw PlankPencilImageError.invalid }
        // Only these exact record shapes are admitted; unknown kinds cannot
        // become an unbounded allocation or an alternate stream capability.
        let b = Array(data)
        if b.count == 1 {
            switch b[0] {
            case 0: return .ready
            case 2: return .end
            case 3: return .ping
            case 4: return .pong
            default: throw PlankPencilImageError.invalid
            }
        }
        guard b[0] == 1, b.count >= 60 else { throw PlankPencilImageError.invalid }
        func u64(_ i:Int)->UInt64 { (0..<8).reduce(0) { $0 | UInt64(b[i+$1]) << (8*$1) } }
        func u32(_ i:Int)->Int { (0..<4).reduce(0) { $0 | Int(b[i+$1]) << (8*$1) } }
        let uuid = UUID(uuid:(b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15],b[16]))
        let d = PlankPencilImageDescriptor(desktopID:uuid,geometryGeneration:u64(17),frameNumber:u64(25),
            sourceWidth:u32(33),sourceHeight:u32(37),encodedWidth:u32(41),encodedHeight:u32(45))
        let count = Int(b[57]) | Int(b[58]) << 8
        guard count == b.count-59 else { throw PlankPencilImageError.invalid }
        return .chunk(try .init(descriptor:d,totalBytes:u32(49),offset:u32(53),bytes:Data(b.dropFirst(59))).validated())
    }
}

// One sequential incomplete frame. Validation precedes any frame-sized growth;
// no reserveCapacity(totalBytes) on metadata supplied by a peer. Cipher records
// are NEVER dropped: replacement happens only before encryption/after assembly.
struct PlankPencilImageAssembler {
    private var descriptor: PlankPencilImageDescriptor?
    private var expectedBytes = 0
    private var bytes = Data()
    private var startedAt: UInt64 = 0
    private var lastCompletedNumber: UInt64 = 0
    var pendingBytes: Int { bytes.count }
    var hasIncompleteFrame: Bool { descriptor != nil }
    mutating func accept(_ c:PlankPencilImageChunk,now:UInt64) throws -> PlankPencilImageFrame? {
        _ = try c.validated()
        try checkDeadline(now:now)
        if let descriptor {
            guard descriptor == c.descriptor, expectedBytes == c.totalBytes, c.offset == bytes.count else {
                throw PlankPencilImageError.invalid
            }
        } else {
            guard c.offset == 0, c.descriptor.frameNumber > lastCompletedNumber else { throw PlankPencilImageError.invalid }
            descriptor = c.descriptor; expectedBytes = c.totalBytes; startedAt = now
        }
        guard bytes.count <= expectedBytes-c.bytes.count else { throw PlankPencilImageError.overflow }
        bytes.append(c.bytes)
        guard bytes.count == expectedBytes else { return nil }
        let frame = PlankPencilImageFrame(descriptor:c.descriptor,jpegData:bytes,receivedAtUptimeNanoseconds:startedAt)
        lastCompletedNumber = c.descriptor.frameNumber
        descriptor = nil; bytes = Data(); expectedBytes = 0; startedAt = 0
        return try frame.validated()
    }
    func checkDeadline(now:UInt64) throws {
        if descriptor != nil, (now < startedAt || now-startedAt > PlankPencilImageLimits.assemblyTimeoutNanoseconds) {
            throw PlankPencilImageError.timedOut
        }
    }
    mutating func clear() { self = .init() }
}

// Admission before dispatch: only one latest whole frame, at most one wake.
// Closing clears pixel bytes and rejects delayed offers from an old session.
final class PlankPencilImageMailbox: @unchecked Sendable {
    private let lock = NSLock()
    private var value: PlankPencilImageFrame?
    private var closed = false, active = false
    func offer(_ frame:PlankPencilImageFrame)->(accepted:Bool,wake:Bool) {
        guard (try? frame.validated()) != nil else { return (false,false) }
        lock.lock(); defer { lock.unlock() }
        guard !closed else { return (false,false) }
        value = frame
        let wake = !active; active = true
        return (true,wake)
    }
    func take()->PlankPencilImageFrame? {
        lock.lock(); defer { lock.unlock() }
        let result=value; value=nil
        if result == nil { active=false }
        return result
    }
    func close() { lock.lock(); closed=true; value=nil; active=false; lock.unlock() }
}
