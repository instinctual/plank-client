import Foundation
import CoreGraphics
import Combine

@main enum ZoomPolicyChecks {
    static func close(_ value: CGFloat, _ expected: CGFloat, _ message: String = "Unexpected geometry") {
        precondition(abs(value - expected) < 0.0000001, message)
    }
    static func close(_ point: CGPoint, _ expected: CGPoint) {
        close(point.x, expected.x); close(point.y, expected.y)
    }
    static func normalized(_ viewport: PlankIPadViewport, _ point: CGPoint, held: Bool = false) -> CGPoint {
        let result = viewport.normalized(point, held: held)!
        return CGPoint(x: result.x, y: result.y)
    }

    static func geometryChecks() {
        // Retain the accepted Fit landmarks and contact admission unchanged.
        let fit = PlankIPadViewport(bounds: CGRect(x: 0, y: 0, width: 1000, height: 1000), width: 1920, height: 1080)!
        precondition(fit.rect == CGRect(x: 0, y: 218.75, width: 1000, height: 562.5))
        precondition(fit.visibleSourceCrop == PlankIPadViewport.fullSourceCrop && fit.fullImageFrame == fit.rect)
        precondition(fit.map(CGPoint(x: 500, y: 100)) == nil)
        precondition(fit.map(CGPoint(x: 0, y: 218.75))!.x == 0)
        precondition(fit.map(CGPoint(x: 1000, y: 781.25))!.x == 1919)
        precondition(fit.map(CGPoint(x: 1000, y: 781.25))!.y == 1079)
        let held = fit.map(CGPoint(x: 2000, y: -100), held: true)!
        precondition(held.x == 1919 && held.y == 0)
        precondition(fit.point(x: 1919, y: 1079) == CGPoint(x: 1000, y: 781.25))

        let centerCrop = CGRect(x: 0.25, y: 0.25, width: 0.5, height: 0.5)
        let doubled = PlankIPadViewport(bounds: CGRect(x: 0, y: 0, width: 1000, height: 1000), width: 1920, height: 1080,
                                       visibleSourceCrop: centerCrop)!
        precondition(doubled.rect == fit.rect, "Two-times View keeps the complete-source fitted outline")
        precondition(doubled.fullImageFrame == CGRect(x: -500, y: -62.5, width: 2000, height: 1125))
        close(normalized(doubled, CGPoint(x: 0, y: 218.75)), CGPoint(x: 0.25, y: 0.25))
        close(normalized(doubled, CGPoint(x: 1000, y: 781.25)), CGPoint(x: 0.75, y: 0.75))
        close(normalized(doubled, CGPoint(x: 500, y: 500)), CGPoint(x: 0.5, y: 0.5))
        precondition(doubled.map(CGPoint(x: 0, y: 218.75))!.x == 480)
        precondition(doubled.map(CGPoint(x: 1000, y: 781.25))!.x == 1439)
        precondition(doubled.map(CGPoint(x: 1000, y: 781.25))!.y == 809)
        precondition(doubled.normalized(CGPoint(x: 500, y: 0)) == nil)
        close(normalized(doubled, CGPoint(x: 2000, y: -100), held: true), CGPoint(x: 0.75, y: 0.25))

        // Asymmetric local canvas and crop origins have no Host desktop-origin
        // term. These expected landmarks independently pin both translation axes.
        let bounds = CGRect(x: 123, y: -47, width: 1000, height: 803)
        let crop = CGRect(x: 0.4, y: 0.1, width: 0.5, height: 0.5)
        let offset = PlankIPadViewport(bounds: bounds, width: 1920, height: 960, visibleSourceCrop: crop)!
        precondition(offset.rect == CGRect(x: 123, y: 104.5, width: 1000, height: 500))
        precondition(offset.fullImageFrame == CGRect(x: -677, y: 4.5, width: 2000, height: 1000))
        close(normalized(offset, CGPoint(x: 123, y: 104.5)), CGPoint(x: 0.4, y: 0.1))
        close(normalized(offset, CGPoint(x: 1123, y: 604.5)), CGPoint(x: 0.9, y: 0.6))
        close(normalized(offset, CGPoint(x: 623, y: 354.5)), CGPoint(x: 0.65, y: 0.35))
        precondition(offset.map(CGPoint(x: 123, y: 104.5))!.x == 768)
        precondition(offset.map(CGPoint(x: 123, y: 104.5))!.y == 96)
        precondition(offset.map(CGPoint(x: 1123, y: 604.5))!.x == 1727)
        precondition(offset.map(CGPoint(x: 1123, y: 604.5))!.y == 575)
        close(offset.point(normalized: CGPoint(x: 0, y: 0))!, CGPoint(x: -677, y: 4.5))
        close(offset.point(normalized: CGPoint(x: 1, y: 1))!, CGPoint(x: 1323, y: 1004.5))
        close(offset.point(normalized: CGPoint(x: 0.65, y: 0.35))!, CGPoint(x: 623, y: 354.5))
        precondition(offset.normalized(offset.point(x: 0, y: 0)!) == nil, "Hidden source points cannot start remote input")
        for source in [CGPoint(x: 0.4, y: 0.1), CGPoint(x: 0.61, y: 0.49), CGPoint(x: 0.9, y: 0.6)] {
            close(normalized(offset, offset.point(normalized: source)!), source)
        }
        for (x, y) in [(1000, 400), (1700, 500), (800, 100)] {
            let result = offset.map(offset.point(x: x, y: y)!)!
            precondition(result.x == x && result.y == y, "Host pixel endpoints survive both transform directions")
        }

        let edgeCrop = CGRect(x: 0.5, y: 0.5, width: 0.5, height: 0.5)
        let edge = PlankIPadViewport(bounds: bounds, width: 65536, height: 65536, visibleSourceCrop: edgeCrop)!
        precondition(edge.map(CGPoint(x: edge.rect.maxX, y: edge.rect.maxY))!.x == 65535)
        precondition(edge.map(CGPoint(x: edge.rect.maxX, y: edge.rect.maxY))!.y == 65535)
        close(edge.point(x: 65535, y: 65535)!, CGPoint(x: edge.rect.maxX, y: edge.rect.maxY))
        let stretch = PlankIPadViewport(bounds: bounds, width: 1920, height: 960, preserveAspectRatio: false,
                                       visibleSourceCrop: crop)!
        precondition(stretch.rect == bounds, "The explicit full-pad preference stretches picture and input together")
        close(normalized(stretch, CGPoint(x: bounds.minX, y: bounds.minY)), CGPoint(x: 0.4, y: 0.1))
        close(normalized(stretch, CGPoint(x: bounds.maxX, y: bounds.maxY)), CGPoint(x: 0.9, y: 0.6))
        close(stretch.fullImageFrame.width, 2000); close(stretch.fullImageFrame.height, 1606)

        let validBounds = CGRect(x: 0, y: 0, width: 1000, height: 800)
        for badBounds in [CGRect.zero, CGRect(x: 0, y: 0, width: -1, height: 800),
                          CGRect(x: CGFloat.nan, y: 0, width: 1000, height: 800),
                          CGRect(x: 0, y: CGFloat.infinity, width: 1000, height: 800),
                          CGRect(x: 0, y: 0, width: CGFloat.infinity, height: 800),
                          CGRect(x: CGFloat.greatestFiniteMagnitude, y: 0, width: CGFloat.greatestFiniteMagnitude, height: 800)] {
            precondition(PlankIPadViewport(bounds: badBounds, width: 1920, height: 1080) == nil)
        }
        for badCrop in [CGRect.zero, CGRect(x: -0.1, y: 0, width: 0.5, height: 0.5),
                        CGRect(x: 0.6, y: 0, width: 0.5, height: 0.5),
                        CGRect(x: 0, y: 0.6, width: 0.5, height: 0.5),
                        CGRect(x: CGFloat.nan, y: 0, width: 0.5, height: 0.5),
                        CGRect(x: 0, y: 0, width: CGFloat.infinity, height: 0.5),
                        CGRect(x: 0, y: 0, width: -0.5, height: 0.5),
                        CGRect(x: 0, y: 0, width: CGFloat.leastNonzeroMagnitude, height: 0.5)] {
            precondition(PlankIPadViewport(bounds: validBounds, width: 1920, height: 1080, visibleSourceCrop: badCrop) == nil)
        }
        precondition(PlankIPadViewport(bounds: validBounds, width: 1, height: 1080) == nil)
        precondition(PlankIPadViewport(bounds: validBounds, width: 1920, height: 65537) == nil)
        precondition(doubled.map(CGPoint(x: CGFloat.nan, y: 0), held: true) == nil)
        precondition(doubled.normalized(CGPoint(x: CGFloat.infinity, y: 0), held: true) == nil)
        precondition(doubled.point(normalized: CGPoint(x: -CGFloat.infinity, y: 0)) == nil)
        precondition(doubled.point(normalized: CGPoint(x: 1.001, y: 0)) == nil)
        precondition(doubled.point(x: 1920, y: 0) == nil && doubled.point(x: 0, y: -1) == nil)
    }

    @MainActor static func controllerChecks() {
        let controller = PlankIPadZoomController()
        let otherRoot = PlankIPadZoomController()
        precondition(controller.state == .fit && controller.revision == 0)
        controller.requestHand(true)
        precondition(controller.state == .fit, "Fit retains normal drawing until Two-times View is requested")
        controller.recordHover(normalized: CGPoint(x: 0.92, y: 0.08))
        controller.recordHover(normalized: CGPoint(x: CGFloat.nan, y: 0.3))
        controller.recordHover(normalized: CGPoint(x: 1.01, y: 0.3))
        controller.requestMode(.double)
        precondition(controller.state.center == CGPoint(x: 0.75, y: 0.25))
        precondition(controller.state.crop == CGRect(x: 0.5, y: 0, width: 0.5, height: 0.5))
        precondition(controller.state.visibleSourceCrop == controller.state.crop)
        precondition(otherRoot.state == .fit, "Direct and sharing roots own independent local views")
        let initialRevision = controller.revision
        controller.requestMode(.double)
        precondition(controller.revision == initialRevision, "Repeated mode request must not recenter an existing view")
        controller.requestCenter(CGPoint(x: 0.6, y: 0.4))
        let valid = controller.state, validRevision = controller.revision
        controller.requestCenter(CGPoint(x: CGFloat.nan, y: 0.5))
        controller.requestPan(translation: CGPoint(x: CGFloat.infinity, y: 0),
                              in: PlankIPadViewport(bounds: CGRect(x: 0, y: 0, width: 1000, height: 1000), width: 1920, height: 1080)!)
        precondition(controller.state == valid && controller.revision == validRevision)
        controller.requestCenter(CGPoint(x: -20, y: 20))
        precondition(controller.state.center == CGPoint(x: 0.25, y: 0.75))
        controller.reset()
        controller.requestMode(.double)
        precondition(controller.state.center == CGPoint(x: 0.5, y: 0.5), "Forced replacement clears hover from the old source")
        controller.requestHand(true)
        controller.requestMode(.fit)
        precondition(!controller.state.handEnabled, "Returning to Fit restores normal drawing")
        controller.requestCenter(CGPoint(x: 0.7, y: 0.7))
        precondition(controller.state.center == CGPoint(x: 0.5, y: 0.5) && controller.state.crop == PlankIPadViewport.fullSourceCrop)

        // Actual production Pencil policy and held-input ledger supply the
        // modeled contact lifetime. Keeping Space held cannot block local views.
        var pencil = PlankIPadPencilPolicy()
        var pointer = PlankIPadHeldInput(), finger = PlankIPadHeldInput()
        precondition(pointer.key(0x20, pressed: true, modifiers: 0))
        let fit = PlankIPadViewport(bounds: CGRect(x: 123, y: -47, width: 1000, height: 803), width: 1920, height: 960)!
        func syncContact() {
            controller.setRemoteContactActive(pencil.touching || !pointer.buttons.isEmpty || !finger.buttons.isEmpty)
        }
        func sample(_ phase: PlankNormalizedPen.Phase, time: Double) -> PlankNormalizedPen? {
            pencil.sample(phase, point: CGPoint(x: 623, y: 354.5), viewport: fit, timestamp: time,
                          force: 1, maximumForce: 2, altitude: .pi / 2, azimuth: 0)
        }
        precondition(sample(.down, time: 1) != nil && pencil.touching)
        syncContact()
        let beforeContact = controller.state, beforeRevision = controller.revision
        controller.requestMode(.double)
        controller.requestHand(true)
        controller.requestCenter(CGPoint(x: 0.62, y: 0.41))
        precondition(controller.state == beforeContact && controller.revision == beforeRevision)
        precondition(sample(.move, time: 2)!.x == 0.5 && pencil.touching, "Deferred zoom cannot change an active stroke's transform")
        precondition(pointer.button(1, pressed: true))
        precondition(finger.button(1, pressed: true))
        syncContact()
        precondition(sample(.up, time: 3) != nil && !pencil.touching)
        syncContact()
        precondition(controller.state == beforeContact, "A remaining pointer/finger contact must keep the deferred view")
        precondition(pointer.button(1, pressed: false))
        syncContact()
        precondition(controller.state == beforeContact)
        precondition(finger.button(1, pressed: false))
        syncContact()
        precondition(controller.state.mode == .double && controller.state.handEnabled)
        precondition(controller.state.center == CGPoint(x: 0.62, y: 0.41))
        precondition(controller.revision == beforeRevision + 1, "Coalesced zoom, Hand and center publish one transform after the final release")
        precondition(pointer.keys[0x20] == 0, "Local view changes leave held Artist/hardware modifiers owned")

        controller.requestCenter(CGPoint(x: 0.5, y: 0.5))
        let zoomed = PlankIPadViewport(bounds: CGRect(x: 123, y: -47, width: 1000, height: 803), width: 1920, height: 960,
                                      visibleSourceCrop: controller.state.crop)!
        // With only a local navigation contact (no setRemoteContactActive), Hand
        // movement applies immediately and obeys the magnified screen scale.
        controller.requestPan(translation: CGPoint(x: 100, y: -50), in: zoomed)
        close(controller.state.center, CGPoint(x: 0.45, y: 0.55))
        precondition(pointer.keys[0x20] == 0)
        precondition(pointer.button(3, pressed: true))
        syncContact()
        let beforePan = controller.state, panRevision = controller.revision
        controller.requestPan(translation: CGPoint(x: 100, y: -50), in: zoomed)
        controller.requestPan(translation: CGPoint(x: 100, y: -50), in: zoomed)
        controller.requestHand(false)
        controller.requestPan(translation: CGPoint(x: CGFloat.nan, y: 0), in: zoomed)
        controller.requestCenter(CGPoint(x: 0.5, y: CGFloat.infinity))
        precondition(controller.state == beforePan)
        precondition(pointer.button(3, pressed: false))
        syncContact()
        close(controller.state.center, CGPoint(x: 0.35, y: 0.65))
        precondition(!controller.state.handEnabled && controller.revision == panRevision + 1)

        // Reversing pending mode/Hand requests back to the current state cancels
        // them. A release must not publish an intermediate or duplicate view.
        precondition(pointer.button(1, pressed: true))
        syncContact()
        let coalesced = controller.state, coalescedRevision = controller.revision
        controller.requestHand(true); controller.requestHand(false)
        precondition(pointer.button(1, pressed: false))
        syncContact()
        precondition(controller.state == coalesced && controller.revision == coalescedRevision)
        controller.requestMode(.fit)
        precondition(pointer.button(1, pressed: true))
        syncContact()
        let fitRevision = controller.revision
        controller.requestMode(.double); controller.requestMode(.fit)
        precondition(pointer.button(1, pressed: false))
        syncContact()
        precondition(controller.state == .fit && controller.revision == fitRevision)

        // Forced geometry retires the real contact first, then discards the
        // deferred request. A stale later up cannot replay it into a new source.
        controller.recordHover(normalized: CGPoint(x: 0.7, y: 0.6))
        precondition(sample(.down, time: 4) != nil)
        syncContact()
        controller.requestMode(.double); controller.requestHand(true)
        precondition(pencil.retire().map(\.phase) == [.cancel, .leave])
        let resetRevision = controller.revision
        controller.reset()
        precondition(controller.state == .fit && controller.revision == resetRevision + 1)
        precondition(!controller.remoteContactActive && pointer.keys[0x20] == 0)
        syncContact()
        precondition(controller.state == .fit)
        precondition(sample(.up, time: 5) == nil)
        controller.requestMode(.double)
        precondition(controller.state.center == CGPoint(x: 0.5, y: 0.5) && !controller.state.handEnabled)
        precondition(pointer.key(0x20, pressed: false, modifiers: 0), "Only the actual modifier release clears its owner")

        var publications: [PlankIPadZoomState] = []
        let observation = controller.$state.sink { publications.append($0) }
        controller.setRemoteContactActive(true)
        controller.requestCenter(CGPoint(x: 0.6, y: 0.6)); controller.requestCenter(CGPoint(x: 0.7, y: 0.3))
        controller.requestHand(true)
        precondition(publications.count == 1)
        controller.setRemoteContactActive(false)
        precondition(publications.count == 2 && publications.last == controller.state)
        precondition(publications.last!.center == CGPoint(x: 0.7, y: 0.3) && publications.last!.handEnabled)
        observation.cancel()
    }

    @MainActor static func main() {
        geometryChecks()
        controllerChecks()
        print("iPad Fit/2x crop landmarks, forward/inverse endpoint mapping, finite geometry, remote-contact deferral, pan coalescing, independent local views and forced reset checks passed")
    }
}
