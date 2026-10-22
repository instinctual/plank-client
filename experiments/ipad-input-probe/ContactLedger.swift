// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation

// The probe's contact accounting is independent of UIKit and remote input.
// A terminal transition must survive even when it shares the last move's timestamp.
struct ContactLedger {
    enum Phase: String { case down, move, up, cancel }
    private struct Contact { var timestamp: Double; var phase: Phase }
    private var contacts: [Int: Contact] = [:]
    private(set) var downs = 0
    private(set) var moves = 0
    private(set) var ups = 0
    private(set) var cancellations = 0
    private(set) var rejected = 0
    private(set) var duplicates = 0
    private(set) var maxSampleGapMilliseconds = 0.0
    private(set) var lastCancellationReason = "none"
    var held: Int { contacts.count }

    mutating func accept(id: Int, phase: Phase, timestamp: Double) -> Bool {
        guard timestamp.isFinite, timestamp >= 0 else { rejected += 1; return false }
        if phase == .down {
            guard contacts[id] == nil else { duplicates += 1; return false }
            contacts[id] = Contact(timestamp: timestamp, phase: phase)
            downs += 1
            return true
        }
        guard let previous = contacts[id] else { rejected += 1; return false }
        guard timestamp >= previous.timestamp else { rejected += 1; return false }
        guard timestamp != previous.timestamp || phase != previous.phase else {
            duplicates += 1; return false
        }
        maxSampleGapMilliseconds = max(maxSampleGapMilliseconds,
            (timestamp - previous.timestamp) * 1000)
        switch phase {
        case .move:
            contacts[id] = Contact(timestamp: timestamp, phase: phase); moves += 1
        case .up:
            contacts.removeValue(forKey: id); ups += 1
        case .cancel:
            contacts.removeValue(forKey: id); cancellations += 1
        case .down: break
        }
        return true
    }

    mutating func cancelAll(reason: String) {
        cancellations += contacts.count
        contacts.removeAll()
        lastCancellationReason = reason
    }
}
