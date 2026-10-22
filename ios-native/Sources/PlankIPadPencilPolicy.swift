import Foundation
import CoreGraphics

/// Actual samples only. A geometry/focus cancellation retires the contact;
/// later moves cannot resurrect it without a new UIKit touch-down.
struct PlankIPadPencilPolicy {
    private(set) var touching = false
    private var last: PlankNormalizedPen?
    private var timestamp = -Double.infinity

    /// Hover is stamped when its recognizer callback is delivered, whereas a
    /// touch carries its acquisition timestamp. A queued hover callback must
    /// not veto a fresh contact with an earlier acquisition time. Validate on
    /// a copy so an invalid/margin down cannot retire the current state.
    mutating func beginContact(point: CGPoint, viewport: PlankIPadViewport,
                               timestamp: Double, force: Double, maximumForce: Double,
                               altitude: Double, azimuth: Double) -> [PlankNormalizedPen]? {
        guard !touching else { return nil }
        var next = self
        let retirement = next.retire()
        guard let down = next.sample(.down, point: point, viewport: viewport,
            timestamp: timestamp, force: force, maximumForce: maximumForce,
            altitude: altitude, azimuth: azimuth) else { return nil }
        self = next
        return retirement + [down]
    }

    mutating func sample(_ phase: PlankNormalizedPen.Phase, point: CGPoint,
                         viewport: PlankIPadViewport, timestamp next: Double,
                         force: Double, maximumForce: Double,
                         altitude: Double, azimuth: Double, distance: Double = 0) -> PlankNormalizedPen? {
        guard next.isFinite, next >= 0, next >= timestamp else { return nil }
        switch phase {
        case .down: guard !touching else { return nil }
        case .move: guard touching, next > timestamp else { return nil }
        case .up: guard touching else { return nil }
        case .hover: guard !touching else { return nil }
        case .cancel, .leave: return nil // Only retire() owns these transitions.
        }
        guard let coordinate = viewport.normalized(point, held: touching) else { return nil }
        guard force.isFinite, maximumForce.isFinite, distance.isFinite else { return nil }
        let orientation = Self.orientation(altitude: altitude, azimuth: azimuth)
        // A zero/unknown force range carries zero pressure, never fabricated force.
        let pressure = maximumForce > 0 ? min(max(force / maximumForce, 0), 1) : 0
        let packet = PlankNormalizedPen(phase: phase, x: Float(coordinate.x), y: Float(coordinate.y),
            pressureOrDistance: Float(phase == .down || phase == .move ? pressure :
                (phase == .hover ? min(max(distance, 0), 1) : 0)),
            tilt: orientation.tilt, rotation: orientation.rotation)
        timestamp = next; last = packet
        if phase == .down { touching = true }
        if phase == .up { touching = false }
        return packet
    }

    static func orientation(altitude: Double, azimuth: Double) -> (tilt: UInt8, rotation: UInt16) {
        guard altitude.isFinite, azimuth.isFinite, altitude >= 0, altitude <= .pi / 2 else {
            return (255, 65535)
        }
        // UIKit cap direction: +X at 0, clockwise +Y at pi/2. Host tilt:
        // x = -sin(rotation)*sin(tilt), y = cos(rotation)*sin(tilt).
        let degrees = (azimuth * 180 / .pi).truncatingRemainder(dividingBy: 360)
        let rotation = (degrees + 270 + 360).truncatingRemainder(dividingBy: 360)
        return (UInt8((90 - altitude * 180 / .pi).rounded()), UInt16(rotation.rounded()) % 360)
    }

    mutating func retire() -> [PlankNormalizedPen] {
        guard var packet = last else { return [] }
        var result: [PlankNormalizedPen] = []
        packet.pressureOrDistance = 0
        if touching { packet.phase = .cancel; result.append(packet) }
        packet.phase = .leave; result.append(packet)
        touching = false; last = nil; timestamp = -Double.infinity
        return result
    }
}
