import Foundation

/// Multiple presentations retain the same immutable decoded frame. Removing
/// one surface cannot remove another surface's callbacks or own the decoder.
@MainActor final class PlankVideoSurfaces {
    struct Surface {
        let frame: (PlankRenderedFrame?) -> Void
        let cursor: (PlankRemoteCursor?) -> Void
        let shape: (PlankRemoteCursorShape?) -> Void
    }
    private var surfaces: [UUID: Surface] = [:]
    func register(id: UUID, surface: Surface) { surfaces[id] = surface }
    func unregister(id: UUID) { surfaces.removeValue(forKey: id) }
    func frame(_ value: PlankRenderedFrame?) { Array(surfaces.values).forEach { $0.frame(value) } }
    func cursor(_ value: PlankRemoteCursor?) { Array(surfaces.values).forEach { $0.cursor(value) } }
    func shape(_ value: PlankRemoteCursorShape?) { Array(surfaces.values).forEach { $0.shape(value) } }
}
