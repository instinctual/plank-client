import Foundation

// The version authenticates the expanded keyboard authority as well as pen.
// Old peer-v1/v2 approvals remain stored but never approve this capability.
enum PlankPencilProtocol {
    static let version = "3"
    static let capability = "normalized-pen-keys"
    static let wireVersion: UInt8 = 3
    static let maximumKeyEdges = 48
}
struct PlankPencilKeyEvent: Sendable, Equatable {
    let code: UInt16
    let pressed: Bool
    let modifiers: UInt8
    init(code: UInt16, pressed: Bool, modifiers: UInt8 = 0) {
        self.code = code; self.pressed = pressed; self.modifiers = modifiers
    }
}
// Independent, versioned normalized source. No HID descriptor or Wacom identity.
enum PlankPencilMessage: Sendable, Equatable {
    case configuration(width: UInt32, height: UInt32, active: Bool)
    case pen(PlankNormalizedPen)
    case rightClick(x: Float, y: Float)
    case modifier(PlankPencilModifier, pressed: Bool)
    case keys([PlankPencilKeyEvent])
    case ping, pong, end

    var replaceablePhase: PlankNormalizedPen.Phase? {
        guard case let .pen(p) = self, p.phase == .hover || p.phase == .move else { return nil }
        return p.phase
    }
    func encoded() throws -> Data {
        var out = Data([0x50, 0x4c, 0x50, 0x4e, PlankPencilProtocol.wireVersion])
        func u32(_ value: UInt32) { for n in 0..<4 { out.append(UInt8(truncatingIfNeeded: value >> (8*n))) } }
        func scalar(_ value: Float) throws {
            guard value.isFinite, (0...1).contains(value) else { throw PlankPencilWireError.invalid }
            u32(value.bitPattern)
        }
        switch self {
        case let .configuration(w,h,active):
            guard (2...65536).contains(w), (2...65536).contains(h) else { throw PlankPencilWireError.invalid }
            out.append(1); u32(w); u32(h); out.append(active ? 1 : 0)
        case let .pen(p):
            guard p.tilt <= 90 || p.tilt == 255, p.rotation < 360 || p.rotation == 65535,
                  (p.tilt == 255) == (p.rotation == 65535),
                  [.down,.move,.hover].contains(p.phase) || p.pressureOrDistance == 0 else { throw PlankPencilWireError.invalid }
            out.append(2); out.append(p.phase.rawValue)
            try scalar(p.x); try scalar(p.y); try scalar(p.pressureOrDistance)
            out.append(p.tilt); out.append(UInt8(truncatingIfNeeded:p.rotation)); out.append(UInt8(p.rotation >> 8))
        case let .rightClick(x,y): out.append(3); try scalar(x); try scalar(y)
        case .ping: out.append(4)
        case .pong: out.append(5)
        case .end: out.append(6)
        case let .modifier(key,pressed):
            out.append(7); out.append(UInt8(truncatingIfNeeded:key.rawValue)); out.append(UInt8(key.rawValue >> 8)); out.append(pressed ? 1 : 0)
        case let .keys(events):
            guard !events.isEmpty, events.count <= PlankPencilProtocol.maximumKeyEdges else { throw PlankPencilWireError.invalid }
            out.append(8); out.append(UInt8(events.count))
            for event in events {
                guard PlankControlKeyCatalog.supports(event.code), event.modifiers <= 15 else { throw PlankPencilWireError.invalid }
                out.append(UInt8(truncatingIfNeeded:event.code)); out.append(UInt8(event.code >> 8))
                out.append(event.pressed ? 1 : 0); out.append(event.modifiers)
            }
        }
        return out
    }
    static func decode(_ data: Data) throws -> Self {
        let b = Array(data)
        guard b.count >= 6, b.prefix(5) == [0x50,0x4c,0x50,0x4e,PlankPencilProtocol.wireVersion] else { throw PlankPencilWireError.invalid }
        func u32(_ i: Int) -> UInt32 { (0..<4).reduce(0) { $0 | UInt32(b[i+$1]) << (8*$1) } }
        let result: Self
        switch (b[5],b.count) {
        case (1,15):
            guard b[14] <= 1 else { throw PlankPencilWireError.invalid }
            result = .configuration(width:u32(6),height:u32(10),active:b[14] == 1)
        case (2,22):
            guard let phase = PlankNormalizedPen.Phase(rawValue:b[6]) else { throw PlankPencilWireError.invalid }
            result = .pen(.init(phase:phase,x:Float(bitPattern:u32(7)),y:Float(bitPattern:u32(11)),
                pressureOrDistance:Float(bitPattern:u32(15)),tilt:b[19],rotation:UInt16(b[20]) | UInt16(b[21]) << 8))
        case (3,14): result = .rightClick(x:Float(bitPattern:u32(6)),y:Float(bitPattern:u32(10)))
        case (4,6): result = .ping
        case (5,6): result = .pong
        case (6,6): result = .end
        case (7,9):
            guard b[8] <= 1, let key = PlankPencilModifier(rawValue:UInt16(b[6]) | UInt16(b[7]) << 8) else { throw PlankPencilWireError.invalid }
            result = .modifier(key,pressed:b[8] == 1)
        case (8, _):
            guard b.count >= 7, b[6] > 0, Int(b[6]) <= PlankPencilProtocol.maximumKeyEdges,
                  b.count == 7 + Int(b[6])*4 else { throw PlankPencilWireError.invalid }
            var events: [PlankPencilKeyEvent] = []
            for i in stride(from:7,to:b.count,by:4) {
                guard b[i+2] <= 1 else { throw PlankPencilWireError.invalid }
                events.append(.init(code:UInt16(b[i]) | UInt16(b[i+1]) << 8,pressed:b[i+2] == 1,modifiers:b[i+3]))
            }
            result = .keys(events)
        default: throw PlankPencilWireError.invalid
        }
        _ = try result.encoded() // Same finite/range rules in both directions.
        return result
    }
}
enum PlankPencilWireError: Error { case invalid, overflow, storage, crypto }

// A complete batch is validated against a copy before state changes. This also
// prevents legacy modifier and expanded key frames from double-owning one key.
struct PlankPencilKeyState {
    private(set) var held = Set<UInt16>()
    mutating func accept(_ events: [PlankPencilKeyEvent]) throws {
        _ = try PlankPencilMessage.keys(events).encoded()
        var next = held
        for event in events {
            if event.pressed {
                guard next.insert(event.code).inserted else { throw PlankPencilWireError.invalid }
            } else {
                guard next.remove(event.code) != nil else { throw PlankPencilWireError.invalid }
            }
        }
        held = next
    }
    mutating func retire() -> [PlankPencilKeyEvent] {
        let ordered = held.sorted {
            let left = PlankControlKeyCatalog.modifierMask($0), right = PlankControlKeyCatalog.modifierMask($1)
            return left == right ? $0 < $1 : (left == 0 || (right != 0 && left > right))
        }
        return ordered.map { code in
            held.remove(code)
            let mask = held.reduce(UInt8(0)) { $0 | PlankControlKeyCatalog.modifierMask($1) }
            return PlankPencilKeyEvent(code:code,pressed:false,modifiers:mask)
        }
    }
}

struct PlankPencilStrokeState {
    private(set) var touching = false
    private var last: PlankNormalizedPen?
    mutating func accept(_ packet: PlankNormalizedPen) throws {
        _ = try PlankPencilMessage.pen(packet).encoded()
        switch packet.phase {
        case .down: guard !touching else { throw PlankPencilWireError.invalid }; touching = true
        case .move: guard touching else { throw PlankPencilWireError.invalid }
        case .up: guard touching else { throw PlankPencilWireError.invalid }; touching = false
        case .hover: guard !touching else { throw PlankPencilWireError.invalid }
        case .cancel: touching = false
        case .leave: guard !touching else { throw PlankPencilWireError.invalid }
        }
        last = packet.phase == .leave ? nil : packet
    }
    mutating func retire() -> [PlankNormalizedPen] {
        guard var p = last else { return [] }
        var result: [PlankNormalizedPen] = []
        p.pressureOrDistance = 0
        if touching { p.phase = .cancel; result.append(p) }
        p.phase = .leave; result.append(p)
        last = nil; touching = false
        return result
    }
}

/// Local scalar observations only. A FIFO slot retains its first offer time
/// across motion replacement; the retained value's timestamp refreshes. These
/// separate executor residence from the freshness of the motion eventually read.
struct PlankPencilMailboxDiagnostics: Sendable {
    let offered: UInt64, accepted: UInt64, rejected: UInt64, taken: UInt64, coalesced: UInt64
    let depth: Int, highWaterDepth: Int
    let oldestAgeNanos: UInt64, headValueAgeNanos: UInt64
    let maxDrainAgeNanos: UInt64, maxDrainedValueAgeNanos: UInt64
}

private func plankPencilDiagnosticIncrement(_ value: inout UInt64) {
    if value != UInt64.max { value += 1 }
}
private func plankPencilDiagnosticAge(now: UInt64, since: UInt64) -> UInt64 {
    now >= since ? now - since : 0
}

// Thread-safe admission BEFORE scheduling work: event edges cannot accumulate
// as unbounded dispatch closures. Only consecutive same-phase motion coalesces.
final class PlankPencilMailbox: @unchecked Sendable {
    private struct Entry {
        var value: PlankPencilMessage
        let queuedAt: UInt64
        var updatedAt: UInt64
    }
    private let lock = NSLock()
    private let clock: @Sendable () -> UInt64
    private var values: [Entry] = []
    private var failed = false
    private var closed = false
    private var offered: UInt64 = 0, accepted: UInt64 = 0, rejected: UInt64 = 0
    private var taken: UInt64 = 0, coalesced: UInt64 = 0
    private var highWaterDepth = 0
    private var maxDrainAgeNanos: UInt64 = 0, maxDrainedValueAgeNanos: UInt64 = 0
    let limit: Int
    init(limit: Int = 128, clock: @escaping @Sendable () -> UInt64 = {
        DispatchTime.now().uptimeNanoseconds
    }) { self.limit = limit; self.clock = clock }
    func offer(_ value: PlankPencilMessage) -> (accepted: Bool, wake: Bool) {
        lock.lock(); defer { lock.unlock() }
        plankPencilDiagnosticIncrement(&offered)
        guard !failed, !closed else {
            plankPencilDiagnosticIncrement(&rejected); return (false,false)
        }
        let now = clock()
        let wake = values.isEmpty
        if let phase = value.replaceablePhase, values.last?.value.replaceablePhase == phase {
            values[values.count-1].value = value
            values[values.count-1].updatedAt = now
            plankPencilDiagnosticIncrement(&coalesced)
        } else {
            guard values.count < limit else {
                failed = true; plankPencilDiagnosticIncrement(&rejected); return (false,true)
            }
            values.append(Entry(value:value,queuedAt:now,updatedAt:now))
        }
        plankPencilDiagnosticIncrement(&accepted)
        highWaterDepth = max(highWaterDepth,values.count)
        return (true,wake)
    }
    func take() throws -> PlankPencilMessage? {
        lock.lock(); defer { lock.unlock() }
        guard !failed else { throw PlankPencilWireError.overflow }
        guard !values.isEmpty else { return nil }
        let entry = values.removeFirst(), now = clock()
        plankPencilDiagnosticIncrement(&taken)
        maxDrainAgeNanos = max(maxDrainAgeNanos,plankPencilDiagnosticAge(now:now,since:entry.queuedAt))
        maxDrainedValueAgeNanos = max(maxDrainedValueAgeNanos,plankPencilDiagnosticAge(now:now,since:entry.updatedAt))
        return entry.value
    }
    func close() { lock.lock(); closed = true; values.removeAll(); lock.unlock() }
    var diagnostics: PlankPencilMailboxDiagnostics {
        lock.lock(); defer { lock.unlock() }
        let now = clock()
        return .init(offered:offered,accepted:accepted,rejected:rejected,taken:taken,coalesced:coalesced,
            depth:values.count,highWaterDepth:highWaterDepth,
            oldestAgeNanos:values.first.map { plankPencilDiagnosticAge(now:now,since:$0.queuedAt) } ?? 0,
            headValueAgeNanos:values.first.map { plankPencilDiagnosticAge(now:now,since:$0.updatedAt) } ?? 0,
            maxDrainAgeNanos:maxDrainAgeNanos,maxDrainedValueAgeNanos:maxDrainedValueAgeNanos)
    }
}

struct PlankPencilPeerTransportDiagnostics: Sendable {
    let sendStarted: UInt64, sendCompleted: UInt64, sendFailed: UInt64, sendCancelled: UInt64
    let inFlightAgeNanos: UInt64, lastSendCompletionNanos: UInt64, maxSendCompletionNanos: UInt64
    let tickCount: UInt64, missedTicks: UInt64
    let lastTickLatenessNanos: UInt64, maxTickLatenessNanos: UInt64
}
struct PlankPencilPeerDiagnostics: Sendable {
    let outgoing: PlankPencilMailboxDiagnostics, incoming: PlankPencilMailboxDiagnostics
    let transport: PlankPencilPeerTransportDiagnostics
}

/// One scalar lock makes diagnostic snapshots safe without waiting on the
/// transport executor or retaining events. Its clock never enters wire records.
final class PlankPencilPeerDiagnosticState: @unchecked Sendable {
    private let lock = NSLock()
    private let clock: @Sendable () -> UInt64
    private var closed = false
    private var inFlightAt: UInt64?
    private var sendStarted: UInt64 = 0, sendCompleted: UInt64 = 0
    private var sendFailed: UInt64 = 0, sendCancelled: UInt64 = 0
    private var lastSendCompletionNanos: UInt64 = 0, maxSendCompletionNanos: UInt64 = 0
    private var expectedTickAt: UInt64?
    private var tickCount: UInt64 = 0, missedTicks: UInt64 = 0
    private var lastTickLatenessNanos: UInt64 = 0, maxTickLatenessNanos: UInt64 = 0
    init(clock: @escaping @Sendable () -> UInt64 = { DispatchTime.now().uptimeNanoseconds }) {
        self.clock = clock
    }
    func beginSend() {
        lock.lock(); defer { lock.unlock() }
        guard !closed, inFlightAt == nil else { return }
        inFlightAt = clock(); plankPencilDiagnosticIncrement(&sendStarted)
    }
    func completeSend(failed: Bool) {
        lock.lock(); defer { lock.unlock() }
        guard !closed, let started = inFlightAt else { return }
        inFlightAt = nil; plankPencilDiagnosticIncrement(&sendCompleted)
        if failed { plankPencilDiagnosticIncrement(&sendFailed) }
        lastSendCompletionNanos = plankPencilDiagnosticAge(now:clock(),since:started)
        maxSendCompletionNanos = max(maxSendCompletionNanos,lastSendCompletionNanos)
    }
    func expectFirstTick(at: UInt64) {
        lock.lock(); defer { lock.unlock() }
        guard !closed else { return }; expectedTickAt = at
    }
    func recordTick(at now: UInt64) {
        lock.lock(); defer { lock.unlock() }
        guard !closed else { return }; plankPencilDiagnosticIncrement(&tickCount)
        guard let expected = expectedTickAt else { return }
        let interval: UInt64 = 1_000_000_000
        lastTickLatenessNanos = plankPencilDiagnosticAge(now:now,since:expected)
        maxTickLatenessNanos = max(maxTickLatenessNanos,lastTickLatenessNanos)
        let intervals = lastTickLatenessNanos / interval + 1
        let missed = intervals - 1
        missedTicks = missedTicks > UInt64.max - missed ? UInt64.max : missedTicks + missed
        // Diagnostics follow the timer's original grid, including coalesced
        // firings. This changes no timer deadline, leeway or timeout policy.
        let (advance,overflow) = intervals.multipliedReportingOverflow(by:interval)
        expectedTickAt = overflow || expected > UInt64.max - advance ? UInt64.max : expected + advance
    }
    func close() {
        lock.lock(); defer { lock.unlock() }
        if inFlightAt != nil { plankPencilDiagnosticIncrement(&sendCancelled) }
        inFlightAt = nil; expectedTickAt = nil; closed = true
    }
    var diagnostics: PlankPencilPeerTransportDiagnostics {
        lock.lock(); defer { lock.unlock() }
        let now = clock()
        return .init(sendStarted:sendStarted,sendCompleted:sendCompleted,sendFailed:sendFailed,
            sendCancelled:sendCancelled,
            inFlightAgeNanos:inFlightAt.map { plankPencilDiagnosticAge(now:now,since:$0) } ?? 0,
            lastSendCompletionNanos:lastSendCompletionNanos,maxSendCompletionNanos:maxSendCompletionNanos,
            tickCount:tickCount,missedTicks:missedTicks,lastTickLatenessNanos:lastTickLatenessNanos,
            maxTickLatenessNanos:maxTickLatenessNanos)
    }
}
