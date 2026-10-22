import Foundation
import CoreGraphics

@main enum PencilChecks {
    static func main() {
        let viewport = PlankIPadViewport(bounds: CGRect(x: 0, y: 0, width: 1000, height: 1000), width: 1920, height: 1080)!
        var pen = PlankIPadPencilPolicy()
        func sample(_ phase: PlankNormalizedPen.Phase, time: Double, point: CGPoint = CGPoint(x: 500.25, y: 500), force: Double = 2, maxForce: Double = 4) -> PlankNormalizedPen? {
            pen.sample(phase, point: point, viewport: viewport, timestamp: time,
                       force: force, maximumForce: maxForce, altitude: .pi / 4, azimuth: 0, distance: 0.25)
        }
        precondition(sample(.down, time: 1, point: CGPoint(x: 0, y: 0)) == nil)
        let down = sample(.down, time: 1)!
        precondition(down.x > 0.5 && down.x < 0.501 && down.y == 0.5, "Preserve subpixel coordinates")
        precondition(down.pressureOrDistance == 0.5 && down.tilt == 45 && down.rotation == 270)
        precondition(sample(.down, time: 1) == nil)
        precondition(sample(.move, time: 0.5) == nil && sample(.move, time: 1) == nil)
        precondition(sample(.hover, time: 2) == nil, "Hover cannot interrupt contact")
        let edge = sample(.move, time: 2, point: CGPoint(x: 2000, y: 2000))!
        precondition(edge.x == 1 && edge.y == 1)
        precondition(sample(.up, time: 2)!.pressureOrDistance == 0, "Same-time terminal must release")
        precondition(sample(.up, time: 2) == nil)
        precondition(pen.retire().map(\.phase) == [.leave])
        precondition(pen.retire().isEmpty)
        precondition(sample(.move, time: 3) == nil, "Stale move cannot restart a retired contact")
        precondition(sample(.hover, time: 3)!.pressureOrDistance == 0.25)
        precondition(sample(.down, time: 4, force: 10)!.pressureOrDistance == 1)
        precondition(pen.retire().map(\.phase) == [.cancel, .leave])
        precondition(sample(.move, time: 5) == nil)
        precondition(sample(.down, time: 6, maxForce: 0)!.pressureOrDistance == 0, "Never invent measured force")
        _ = pen.retire()
        precondition(sample(.down, time: .nan) == nil)
        precondition(sample(.down, time: 7, force: .infinity) == nil)
        // Real hover callbacks use delivery time; UIKit contacts use acquisition
        // time. A new contact starts a new epoch, without admitting stale moves.
        _ = pen.retire()
        precondition(sample(.hover, time: 100.020) != nil)
        func begin(time: Double, point: CGPoint = CGPoint(x: 500, y: 500), force: Double = 2) -> [PlankNormalizedPen]? {
            pen.beginContact(point: point, viewport: viewport, timestamp: time,
                force: force, maximumForce: 4, altitude: .pi / 4, azimuth: 0)
        }
        precondition(begin(time: 100.010, point: CGPoint(x: 0, y: 0)) == nil)
        precondition(begin(time: .nan) == nil && begin(time: 100.010, force: .infinity) == nil)
        // Invalid starts leave the existing hover epoch intact.
        precondition(sample(.hover, time: 100.019) == nil)
        let restarted = begin(time: 100.010)!
        precondition(restarted.map(\.phase) == [.leave, .down])
        precondition(restarted.last!.pressureOrDistance == 0.5)
        precondition(begin(time: 100.011) == nil && pen.touching)
        precondition(sample(.move, time: 100.009) == nil)
        precondition(sample(.move, time: 100.030) != nil)
        precondition(sample(.up, time: 100.040) != nil)
        precondition(pen.retire().map(\.phase) == [.leave])
        precondition(begin(time: 101)!.map(\.phase) == [.down])
        _ = pen.retire()
        let up = PlankIPadPencilPolicy.orientation(altitude: .pi / 2, azimuth: 0)
        precondition(up.tilt == 0)
        let axes: [(Double, Double, Double)] = [(0, 1, 0), (.pi / 2, 0, 1), (.pi, -1, 0), (-.pi / 2, 0, -1)]
        for (azimuth, x, y) in axes {
            let angle = PlankIPadPencilPolicy.orientation(altitude: .pi / 4, azimuth: azimuth)
            let rotation = Double(angle.rotation) * .pi / 180
            // Independent Host backend convention, not a duplicate of the converter.
            precondition(abs(-sin(rotation) - x) < 0.001 && abs(cos(rotation) - y) < 0.001)
        }
        precondition(PlankIPadPencilPolicy.orientation(altitude: .nan, azimuth: 0).tilt == 255)
        precondition(PlankIPadPencilPolicy.orientation(altitude: .pi / 4, azimuth: -.pi * 10).rotation == 270)
        let queue = PlankInputQueue()
        queue.append(.pen(down)); queue.append(.pen(edge))
        queue.append(.pen(.init(phase: .cancel, x: 1, y: 1, pressureOrDistance: 0, tilt: 45, rotation: 270)))
        let phases = queue.drain().compactMap { event -> PlankNormalizedPen.Phase? in
            if case let .pen(packet) = event { return packet.phase }; return nil
        }
        precondition(phases == [.down, .move, .cancel], "Sender queue must preserve contact edges/order")
        print("Pencil viewport, pressure, Host tilt convention, terminal ordering and queue checks passed")
    }
}
