// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

@main
struct ContactLedgerChecks {
    static func main() {
        var ledger = ContactLedger()
        precondition(ledger.accept(id: 1, phase: .down, timestamp: 1))
        precondition(ledger.accept(id: 1, phase: .move, timestamp: 1.01))
        precondition(!ledger.accept(id: 1, phase: .move, timestamp: 1.01))
        // Coalesced terminal samples commonly repeat the last sample timestamp.
        precondition(ledger.accept(id: 1, phase: .up, timestamp: 1.01))
        precondition(ledger.held == 0 && ledger.ups == 1 && ledger.duplicates == 1)
        // Neither out-of-order input nor an orphaned move can begin contact.
        precondition(!ledger.accept(id: 2, phase: .move, timestamp: 2))
        precondition(ledger.accept(id: 2, phase: .down, timestamp: 2))
        precondition(!ledger.accept(id: 2, phase: .move, timestamp: 1.9))
        precondition(!ledger.accept(id: 2, phase: .move, timestamp: .nan))
        precondition(ledger.accept(id: 3, phase: .down, timestamp: 2.1))
        // Inactive/geometry transitions release every contact once.
        ledger.cancelAll(reason: "inactive")
        precondition(ledger.held == 0 && ledger.cancellations == 2)
        ledger.cancelAll(reason: "background")
        precondition(ledger.cancellations == 2)
        precondition(!ledger.accept(id: 2, phase: .move, timestamp: 2.2))
        precondition(!ledger.accept(id: 2, phase: .up, timestamp: 2.3))
        // Fresh contact works after cancellation; stale callbacks cannot resume it.
        precondition(ledger.accept(id: 4, phase: .down, timestamp: 3))
        precondition(ledger.accept(id: 4, phase: .cancel, timestamp: 3))
        precondition(ledger.held == 0 && ledger.cancellations == 3)
        precondition(ledger.rejected == 5)
        print("PASS: terminal timestamp, coalesced duplicates, ordering, orphan events and lifecycle cancellation")
    }
}
