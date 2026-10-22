import Foundation

@main
struct PlankPencilFlowDiagnosticsChecks {
    static func main() {
        let receiver = PlankPencilReceiverFlowDiagnostics()
        assert(receiver.snapshotForReport(at: 0) != nil)
        receiver.recordScheduled(); receiver.recordScheduled()
        receiver.recordStarted(scheduledAt: 100, at: 150)
        receiver.recordDrain(startedAt: 155, at: 175, messages: 4, normalizedPens: 2)
        receiver.recordFinished()
        assert(receiver.snapshotForReport(at: 1_999_999_999) == nil)
        let first = receiver.snapshotForReport(at: 2_000_000_000)!
        assert(first.scheduled == 2 && first.started == 1 && first.finished == 1)
        assert(first.outstanding == 1 && first.highWaterOutstanding == 2)
        assert(first.lastWaitNanos == 50 && first.maxWaitNanos == 50)
        assert(first.messages == 4 && first.normalizedPens == 2)
        assert(first.drains == 1 && first.lastDrainNanos == 20 && first.maxDrainMessages == 4)
        // A stale-generation wake still finishes; backward clocks cannot wrap.
        receiver.recordStarted(scheduledAt: 250, at: 200)
        receiver.recordDrain(startedAt: 250, at: 200, messages: 0, normalizedPens: 0)
        receiver.recordFinished()
        assert(receiver.snapshotForReport(at: 2_000_000_000) == nil)
        assert(receiver.snapshotForReport(at: 1_000_000_000) == nil)
        let second = receiver.snapshotForReport(at: 4_000_000_000)!
        assert(second.outstanding == 0 && second.started == second.finished)
        assert(second.maxWaitNanos == 50 && second.lastWaitNanos == 0)
        assert(second.maxDrainNanos == 20 && second.lastDrainNanos == 0)

        // Callback scheduling and actor completion update the same scalar
        // ledger safely; concurrent reports do not reset counters or ownership.
        let concurrent = PlankPencilReceiverFlowDiagnostics()
        DispatchQueue.concurrentPerform(iterations: 4_000) { _ in
            concurrent.recordScheduled()
            concurrent.recordStarted(scheduledAt: 10, at: 30)
            concurrent.recordDrain(startedAt: 30, at: 40, messages: 1, normalizedPens: 1)
            concurrent.recordFinished()
        }
        let totals = concurrent.snapshotForReport(at: 100)!
        assert(totals.scheduled == 4_000 && totals.started == 4_000 && totals.finished == 4_000)
        assert(totals.outstanding == 0 && totals.highWaterOutstanding > 0 && totals.highWaterOutstanding <= 4_000)
        assert(totals.messages == 4_000 && totals.normalizedPens == 4_000 && totals.drains == 4_000)

        let sender = PlankInputSenderState()
        assert(PlankInputSenderState.packetCount(for: .scroll(vertical: 0, horizontal: 0)) == 0)
        assert(PlankInputSenderState.packetCount(for: .scroll(vertical: 1, horizontal: 0)) == 1)
        assert(PlankInputSenderState.packetCount(for: .scroll(vertical: 0, horizontal: 1)) == 1)
        assert(PlankInputSenderState.packetCount(for: .scroll(vertical: 1, horizontal: 1)) == 2)
        assert(PlankInputSenderState.packetCount(for: .rawHid(Data())) == nil)
        sender.recordBatchStarted(count: 3, at: 100)
        sender.recordSend(nanos: 25, succeeded: true, normalizedPen: true, packetCount: 1)
        let active = sender.diagnosticSnapshot(at: 160)
        assert(active.acceptedEvents == 1 && active.submittedPackets == 1 && active.packetCountExact)
        assert(active.normalizedPenSubmits == 1 && active.batchOutstanding == 2 && active.batchAgeNanos == 60)
        // A two-axis scroll is two packets; an empty scroll is zero packets.
        sender.recordSend(nanos: 40, succeeded: true, packetCount: 2)
        sender.recordSend(nanos: 1, succeeded: true, packetCount: 0)
        sender.recordBatchFinished(at: 500)
        let finished = sender.diagnosticSnapshot(at: 600)
        assert(finished.acceptedEvents == 3 && finished.submittedPackets == 3 && finished.packetCountExact)
        assert(finished.batchOutstanding == 0 && finished.batchAgeNanos == 0 && finished.maxBatchNanos == 400)
        assert(sender.sendSummary.accepted == 3 && sender.sendSummary.maxNanos == 40)
        assert(!sender.hasFailed)
        // A source whose bridge can emit extra reset packets is explicitly
        // inexact, rather than treating an event count as a native backlog.
        sender.recordSend(nanos: 50, succeeded: true, tabletType: 3, packetCount: nil)
        let raw = sender.diagnosticSnapshot(at: 700)
        assert(!raw.packetCountExact && raw.rawHidSubmits == 1 && raw.submittedPackets == 4)
        assert(sender.tabletSummary.reports == 1 && sender.tabletSummary.messages == 1)
        assert(sender.tabletSummary.lastReport != 0)
        let failed = PlankInputSenderState()
        failed.recordBatchStarted(count: 2, at: 700)
        failed.recordSend(nanos: 90, succeeded: false, packetCount: 2)
        let failure = failed.diagnosticSnapshot(at: 750)
        assert(failure.acceptedEvents == 0 && failure.submittedPackets == 0 && !failure.packetCountExact)
        assert(failure.failedSubmitNanos == 90 && failure.batchOutstanding == 1 && failure.batchAgeNanos == 50)
        failed.markFailed(); assert(failed.hasFailed)

        // The unchanged production input queue still preserves tip edges and
        // latest motion while repeated diagnostics leave admission/order alone.
        let queue = PlankInputQueue()
        let down = PlankNormalizedPen(phase: .down, x: 0.1, y: 0.2, pressureOrDistance: 0.5, tilt: 0, rotation: 0)
        assert(PlankInputSenderState.isNormalizedPen(.pen(down)))
        assert(!PlankInputSenderState.isNormalizedPen(.rawHid(Data())))
        assert(queue.offerPencilRelayPen(down))
        var move = down; move.phase = .move
        for index in 0..<400 {
            move.x = Float(index) / 400
            assert(queue.offerPencilRelayPen(move))
            assert(queue.diagnostics.depth == 2)
        }
        var up = move; up.phase = .up; up.pressureOrDistance = 0
        assert(queue.offerPencilRelayPen(up))
        assert(queue.diagnostics.depth == 3)
        let drained = queue.drain()
        assert(drained.count == 3)
        if case let .pen(actual) = drained[0] { assert(actual == down) } else { assertionFailure() }
        if case let .pen(actual) = drained[1] { assert(actual == move) } else { assertionFailure() }
        if case let .pen(actual) = drained[2] { assert(actual == up) } else { assertionFailure() }
        assert(queue.diagnostics.depth == 0)
        queue.stop(); assert(!queue.offerPencilRelayPen(down))
        print("PASS: balanced concurrent receiver wake/drain metrics, bounded reporting, sender packet/batch accounting, and unchanged queued tip/motion ordering")
    }
}
