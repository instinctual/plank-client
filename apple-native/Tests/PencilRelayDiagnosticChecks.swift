import Foundation

private final class PencilDiagnosticClock: @unchecked Sendable {
    private let lock = NSLock()
    private var value: UInt64 = 0
    func set(_ next: UInt64) { lock.lock(); value = next; lock.unlock() }
    func read() -> UInt64 { lock.lock(); defer { lock.unlock() }; return value }
}

@main struct PencilRelayDiagnosticChecks {
    static func main() throws {
        let clock = PencilDiagnosticClock()
        let box = PlankPencilMailbox(limit:4,clock:clock.read)
        let empty = box.diagnostics
        assert(empty.depth == 0 && empty.oldestAgeNanos == 0 && empty.headValueAgeNanos == 0)
        let emptyTake = try box.take()
        assert(emptyTake == nil && box.diagnostics.taken == 0)

        func pen(_ phase: PlankNormalizedPen.Phase,_ x: Float) -> PlankPencilMessage {
            .pen(.init(phase:phase,x:x,y:0.5,pressureOrDistance:0,tilt:0,rotation:0))
        }
        clock.set(100)
        let first = box.offer(pen(.hover,0.1))
        assert(first.accepted && first.wake)
        clock.set(160)
        let replacement = box.offer(pen(.hover,0.2))
        assert(replacement.accepted && !replacement.wake)
        clock.set(180)
        let replaced = box.diagnostics
        assert(replaced.offered == 2 && replaced.accepted == 2 && replaced.coalesced == 1)
        assert(replaced.depth == 1 && replaced.highWaterDepth == 1)
        assert(replaced.oldestAgeNanos == 80 && replaced.headValueAgeNanos == 20,
               "Replacing motion preserves slot residence and refreshes retained motion age")
        clock.set(200)
        assert(box.offer(pen(.down,0.3)).accepted)
        clock.set(210)
        assert(box.offer(pen(.move,0.4)).accepted)
        clock.set(230)
        assert(box.offer(pen(.move,0.6)).accepted)
        clock.set(240)
        assert(box.offer(pen(.up,0.7)).accepted)
        assert(box.diagnostics.depth == 4 && box.diagnostics.highWaterDepth == 4)
        clock.set(250)
        var drained: [PlankPencilMessage] = []
        while let next = try box.take() { drained.append(next) }
        assert(drained == [pen(.hover,0.2),pen(.down,0.3),pen(.move,0.6),pen(.up,0.7)],
               "Counters cannot alter edge ordering, replacement or returned payloads")
        let taken = box.diagnostics
        assert(taken.offered == 6 && taken.accepted == 6 && taken.taken == 4 && taken.coalesced == 2)
        assert(taken.depth == 0 && taken.oldestAgeNanos == 0 && taken.headValueAgeNanos == 0)
        assert(taken.maxDrainAgeNanos == 150 && taken.maxDrainedValueAgeNanos == 90)
        assert(box.offer(.ping).wake, "Empty-to-nonempty wake remains unchanged")
        box.close(); box.close()
        let closed = box.offer(.pong)
        assert(!closed.accepted && !closed.wake && box.diagnostics.rejected == 1)
        let closedTake = try box.take()
        assert(closedTake == nil && box.diagnostics.taken == 4)

        let overflow = PlankPencilMailbox(limit:1,clock:clock.read)
        assert(overflow.offer(.ping).accepted)
        let failure = overflow.offer(.pong)
        assert(!failure.accepted && failure.wake)
        let failedAgain = overflow.offer(.end)
        assert(!failedAgain.accepted && !failedAgain.wake)
        do { _ = try overflow.take(); fatalError("Overflow must remain latched") }
        catch PlankPencilWireError.overflow {}
        let failed = overflow.diagnostics
        assert(failed.offered == 3 && failed.accepted == 1 && failed.rejected == 2)
        assert(failed.depth == 1 && failed.highWaterDepth == 1 && failed.taken == 0)
        overflow.close()
        do { _ = try overflow.take(); fatalError("Closing cannot clear the overflow latch") }
        catch PlankPencilWireError.overflow {}

        let regression = PlankPencilMailbox(clock:clock.read)
        clock.set(400); _ = regression.offer(.ping)
        clock.set(350)
        assert(regression.diagnostics.oldestAgeNanos == 0 && regression.diagnostics.headValueAgeNanos == 0)
        _ = try regression.take()
        assert(regression.diagnostics.maxDrainAgeNanos == 0)

        let send = PlankPencilPeerDiagnosticState(clock:clock.read)
        clock.set(1_000); send.beginSend()
        clock.set(1_500)
        assert(send.diagnostics.inFlightAgeNanos == 500 && send.diagnostics.sendStarted == 1)
        clock.set(2_000); send.completeSend(failed:false)
        assert(send.diagnostics.sendCompleted == 1 && send.diagnostics.inFlightAgeNanos == 0)
        assert(send.diagnostics.lastSendCompletionNanos == 1_000 && send.diagnostics.maxSendCompletionNanos == 1_000)
        clock.set(3_000); send.beginSend()
        clock.set(3_100); send.completeSend(failed:true)
        let error = send.diagnostics
        assert(error.sendCompleted == 2 && error.sendFailed == 1)
        assert(error.lastSendCompletionNanos == 100 && error.maxSendCompletionNanos == 1_000)
        send.beginSend(); send.close(); send.close(); send.completeSend(failed:false)
        let cancelled = send.diagnostics
        assert(cancelled.sendStarted == 3 && cancelled.sendCompleted == 2 && cancelled.sendCancelled == 1)
        assert(cancelled.inFlightAgeNanos == 0)

        let timer = PlankPencilPeerDiagnosticState(clock:clock.read)
        timer.expectFirstTick(at:2_000_000_000)
        timer.recordTick(at:2_005_000_000)
        assert(timer.diagnostics.lastTickLatenessNanos == 5_000_000 && timer.diagnostics.missedTicks == 0)
        timer.recordTick(at:5_050_000_000)
        assert(timer.diagnostics.lastTickLatenessNanos == 2_050_000_000 && timer.diagnostics.missedTicks == 2)
        timer.recordTick(at:6_004_000_000)
        let ticks = timer.diagnostics
        assert(ticks.tickCount == 3 && ticks.missedTicks == 2)
        assert(ticks.lastTickLatenessNanos == 4_000_000 && ticks.maxTickLatenessNanos == 2_050_000_000)
        timer.close(); timer.recordTick(at:8_000_000_000)
        assert(timer.diagnostics.tickCount == 3)

        let concurrent = PlankPencilMailbox(clock:clock.read)
        let motion = pen(.hover,0.5)
        DispatchQueue.concurrentPerform(iterations:1_000) { _ in
            assert(concurrent.offer(motion).accepted)
            let value = concurrent.diagnostics
            assert(value.offered == value.accepted + value.rejected)
            assert(value.depth == 1 && value.highWaterDepth == 1)
        }
        let totals = concurrent.diagnostics
        assert(totals.accepted == 1_000 && totals.coalesced == 999 && totals.depth == 1)
        let concurrentTake = try concurrent.take()
        assert(concurrentTake == motion)
        print("Pencil scalar diagnostics passed: coalesced slot/value ages, FIFO/wake/overflow/close invariants, monotonic guards, send completion/cancellation, timer lateness and concurrent snapshots")
    }
}
