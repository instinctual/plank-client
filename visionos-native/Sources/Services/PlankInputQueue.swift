import Foundation

/// Existing type-7 normalized pen. Orientation is tilt direction, not barrel roll.
struct PlankNormalizedPen: Sendable, Equatable {
    enum Phase: UInt8, Sendable { case hover = 0, down = 1, up = 2, move = 3, cancel = 4, leave = 6 }
    var phase: Phase
    var x: Float
    var y: Float
    var pressureOrDistance: Float
    var tilt: UInt8
    var rotation: UInt16
}

enum PlankInputEvent: Sendable {
    case pointer(x: UInt16, y: UInt16, maximumX: UInt16, maximumY: UInt16)
    case button(number: UInt8, pressed: Bool)
    case scroll(vertical: Int16, horizontal: Int16)
    case key(code: UInt16, pressed: Bool, modifiers: UInt8)
    case text(Data)
    case rawHid(Data)
    case pen(PlankNormalizedPen)
}

final class PlankInputQueue: @unchecked Sendable {
    private let lock = NSLock()
    private let wakeups: AsyncStream<Void>
    private let wakeupContinuation: AsyncStream<Void>.Continuation
    private var events: [PlankInputEvent] = []
    private var stopped = false
#if PLANK_NATIVE_MAC_WACOM
    // Presentation ownership only. This does not decode or change HID payloads,
    // their ordering, capture, or transport behavior.
    private var mouseOwnsPointer = false
    var nativeMouseOwnsPointer: Bool {
        lock.lock(); defer { lock.unlock() }
        return mouseOwnsPointer
    }
    private func observePointerSource(_ event: PlankInputEvent) {
        if case .pointer = event { mouseOwnsPointer = true }
        if case let .pen(pen) = event, pen.phase != .cancel, pen.phase != .leave {
            mouseOwnsPointer = false
        }
        if case let .rawHid(frame) = event, frame.count >= 20,
           frame[6] == 3, frame[7] == 0 { mouseOwnsPointer = false } // Existing registered-Relay presentation path; local USB uses the filtered callback.
    }
#endif
    // Bounded diagnostics only: timings and counts, never event contents.
    private var oldestEnqueueTime: UInt64 = 0
    private var highWaterDepth = 0
    private var maxDrainAge: UInt64 = 0

    init() {
        (wakeups, wakeupContinuation) = AsyncStream<Void>.makeStream(
            bufferingPolicy: .bufferingNewest(1)
        )
    }

    var signals: AsyncStream<Void> { wakeups }

    func append(_ event: PlankInputEvent) {
        lock.lock()
        guard !stopped else {
            lock.unlock()
            return
        }
#if PLANK_NATIVE_MAC_WACOM
        observePointerSource(event)
#endif
        let shouldWake = events.isEmpty
        if shouldWake { oldestEnqueueTime = DispatchTime.now().uptimeNanoseconds }
        if case .pointer = event,
           case .pointer? = events.last {
            events[events.count - 1] = event
        } else {
            events.append(event)
        }
        highWaterDepth = max(highWaterDepth, events.count)
        lock.unlock()
        if shouldWake { wakeupContinuation.yield(()) }
    }

#if PLANK_NATIVE_MAC_WACOM
    // Only the physical worker's descriptor-filtered activity signal selects
    // tablet ownership for local USB input; raw status traffic never does.
    func observeNativeTabletActivity() {
        lock.lock(); defer { lock.unlock() }
        guard !stopped else { return }
        mouseOwnsPointer = false
    }

    // Refuse a congested or stopped sender; the capture worker then releases
    // and retries rather than losing a tip/button transition silently.
    func offerNativeRawHid(_ frame: Data) -> Bool {
        lock.lock()
        guard !stopped, events.count < 256 else { lock.unlock(); return false }
        let shouldWake = events.isEmpty
        if shouldWake { oldestEnqueueTime = DispatchTime.now().uptimeNanoseconds }
        events.append(.rawHid(frame))
        highWaterDepth = max(highWaterDepth, events.count)
        lock.unlock()
        if shouldWake { wakeupContinuation.yield(()) }
        return true
    }
#endif

    // Controls submit one ordered chord, including every modifier transition.
    // Capacity is reserved for the complete batch under one lock; a refused
    // batch adds no partial key state to the sender queue.
    func offerControlKeys(_ batch: [PlankControlKeyEvent]) -> Bool {
        guard batch.count <= 256,
              batch.allSatisfy({ PlankControlKeyCatalog.supports($0.code) && $0.modifiers <= 15 }) else { return false }
        lock.lock()
        guard !stopped else { lock.unlock(); return false }
        // Adding/removing a duplicate owner may need no Host edge. Existing
        // unrelated traffic cannot reject that ownership-only transaction.
        guard !batch.isEmpty else { lock.unlock(); return true }
        guard events.count <= 256 - batch.count else { lock.unlock(); return false }
        let shouldWake = events.isEmpty
        if shouldWake { oldestEnqueueTime = DispatchTime.now().uptimeNanoseconds }
        events.append(contentsOf:batch.map { .key(code:$0.code,pressed:$0.pressed,modifiers:$0.modifiers) })
        highWaterDepth = max(highWaterDepth,events.count)
        lock.unlock()
        if shouldWake { wakeupContinuation.yield(()) }
        return true
    }

#if PLANK_PENCIL_RELAY_RECEIVER
    // Squeeze positions and clicks together, without presenting the parked
    // physical Mac mouse as the active pointer. Refuse the whole gesture if
    // there is insufficient room for both button edges.
    func offerPencilRelayRightClick(x: UInt16, y: UInt16, maximumX: UInt16, maximumY: UInt16) -> Bool {
        lock.lock()
        guard !stopped, events.count <= 253 else { lock.unlock(); return false }
        let shouldWake = events.isEmpty
        if shouldWake { oldestEnqueueTime = DispatchTime.now().uptimeNanoseconds }
        events.append(.pointer(x:x,y:y,maximumX:maximumX,maximumY:maximumY))
        events.append(.button(number:3,pressed:true))
        events.append(.button(number:3,pressed:false))
#if PLANK_NATIVE_MAC_WACOM
        mouseOwnsPointer = false
#endif
        highWaterDepth = max(highWaterDepth,events.count)
        lock.unlock()
        if shouldWake { wakeupContinuation.yield(()) }
        return true
    }
    func offerPencilRelayKey(code: UInt16, pressed: Bool, modifiers: UInt8) -> Bool {
        offerControlKeys([.init(code:code,pressed:pressed,modifiers:modifiers)])
    }
    // New source-specific admission: do not expand the shared sender queue
    // indefinitely if the workstation stops accepting input. Terminal release
    // requests remain the existing upstream sender-lifetime boundary.
    func offerPencilRelayPen(_ pen: PlankNormalizedPen) -> Bool {
        lock.lock()
        guard !stopped else { lock.unlock(); return false }
        let shouldWake = events.isEmpty
        if (pen.phase == .move || pen.phase == .hover),
           case let .pen(previous)? = events.last, previous.phase == pen.phase {
            events[events.count-1] = .pen(pen)
        } else {
            guard events.count < 256 else { lock.unlock(); return false }
            if shouldWake { oldestEnqueueTime = DispatchTime.now().uptimeNanoseconds }
            events.append(.pen(pen))
        }
#if PLANK_NATIVE_MAC_WACOM
        observePointerSource(.pen(pen))
#endif
        highWaterDepth = max(highWaterDepth,events.count)
        lock.unlock()
        if shouldWake { wakeupContinuation.yield(()) }
        return true
    }
#endif

    func drain() -> [PlankInputEvent] {
        lock.lock()
        let drained = events
        if !drained.isEmpty {
            maxDrainAge = max(maxDrainAge,
                              DispatchTime.now().uptimeNanoseconds - oldestEnqueueTime)
        }
        events.removeAll(keepingCapacity: true)
        lock.unlock()
        return drained
    }

    var diagnostics: PlankInputQueueDiagnostics {
        lock.lock()
        defer { lock.unlock() }
        return PlankInputQueueDiagnostics(
            depth: events.count,
            oldestAgeNanos: events.isEmpty ? 0 :
                DispatchTime.now().uptimeNanoseconds - oldestEnqueueTime,
            highWaterDepth: highWaterDepth,
            maxDrainAgeNanos: maxDrainAge
        )
    }

    func stop() {
        lock.lock()
        stopped = true
        events.removeAll(keepingCapacity: true)
        lock.unlock()
        wakeupContinuation.finish()
    }
}

struct PlankInputQueueDiagnostics: Sendable {
    let depth: Int
    let oldestAgeNanos: UInt64
    let highWaterDepth: Int
    let maxDrainAgeNanos: UInt64
}
