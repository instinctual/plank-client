import Foundation
import CoreGraphics
import Combine

enum PlankIPadZoomMode: Equatable, Sendable {
    case fit, double
}

/// Local view preferences, independent of media geometry generation and Host
/// input ownership. Two-times View is twice the accepted Fit scale.
struct PlankIPadZoomState: Equatable, Sendable {
    let mode: PlankIPadZoomMode
    let center: CGPoint
    let handEnabled: Bool

    static let fit = Self(mode: .fit, center: CGPoint(x: 0.5, y: 0.5), handEnabled: false)

    var crop: CGRect {
        mode == .fit ? PlankIPadViewport.fullSourceCrop :
            CGRect(x: center.x - 0.25, y: center.y - 0.25, width: 0.5, height: 0.5)
    }
    var visibleSourceCrop: CGRect { crop }
}

/// Each direct/sharing root owns its own controller. Native input reports the
/// aggregate remote contact lifetime after real down/up/cancel delivery. Local
/// navigation contacts and intentionally held keyboard modifiers are excluded.
/// This controller never releases, synthesizes or transfers remote input.
@MainActor final class PlankIPadZoomController: ObservableObject {
    @Published private(set) var state = PlankIPadZoomState.fit
    private(set) var revision: UInt64 = 0
    private(set) var remoteContactActive = false
    private var pending: PlankIPadZoomState?
    private var lastHover: CGPoint?

    func requestMode(_ mode: PlankIPadZoomMode) {
        let current = pending ?? state
        guard current.mode != mode else { return }
        let center = mode == .fit ? CGPoint(x: 0.5, y: 0.5) : bounded(lastHover ?? CGPoint(x: 0.5, y: 0.5))
        request(PlankIPadZoomState(mode: mode, center: center, handEnabled: mode == .fit ? false : current.handEnabled))
    }

    func requestHand(_ enabled: Bool) {
        let current = pending ?? state
        guard !enabled || current.mode == .double else { return }
        request(PlankIPadZoomState(mode: current.mode, center: current.center, handEnabled: enabled))
    }

    /// Navigator selections and drag requests share the same bounded center.
    /// Fit has no crop to pan. Nonfinite requests leave even pending state intact.
    func requestCenter(_ center: CGPoint) {
        guard center.x.isFinite, center.y.isFinite else { return }
        let current = pending ?? state
        guard current.mode == .double else { return }
        request(PlankIPadZoomState(mode: current.mode, center: bounded(center), handEnabled: current.handEnabled))
    }

    /// `translation` is the incremental local screen delta since the preceding
    /// callback. Dragging the picture right/down moves its source center left/up.
    /// Accumulate against the pending center while a separate remote contact holds.
    func requestPan(translation: CGPoint, in viewport: PlankIPadViewport) {
        guard translation.x.isFinite, translation.y.isFinite else { return }
        let current = pending ?? state
        guard current.mode == .double else { return }
        requestCenter(CGPoint(x: current.center.x - translation.x / viewport.rect.width * current.crop.width,
                              y: current.center.y - translation.y / viewport.rect.height * current.crop.height))
    }

    func recordHover(normalized point: CGPoint) {
        guard point.x.isFinite, point.y.isFinite,
              point.x >= 0, point.x <= 1, point.y >= 0, point.y <= 1 else { return }
        lastHover = point
    }

    func setRemoteContactActive(_ active: Bool) {
        remoteContactActive = active
        guard !active, let next = pending else { return }
        pending = nil
        apply(next)
    }

    /// An input-only admission change retires the old contact, not the local
    /// picture. Discard its pending request before a stale up can apply it.
    func retireContactPreservingView() {
        pending = nil
        lastHover = nil
        remoteContactActive = false
    }

    /// Session/source/forced geometry replacement owns contact retirement before
    /// calling this. Pending voluntary requests and old-source hover are discarded.
    /// A fresh local revision is published even when the view was already Fit.
    func reset() {
        retireContactPreservingView()
        revision += 1
        state = .fit
    }

    private func request(_ next: PlankIPadZoomState) {
        if remoteContactActive { pending = next == state ? nil : next }
        else { apply(next) }
    }

    private func apply(_ next: PlankIPadZoomState) {
        guard next != state else { return }
        revision += 1
        state = next
    }

    private func bounded(_ center: CGPoint) -> CGPoint {
        CGPoint(x: min(max(center.x, 0.25), 0.75), y: min(max(center.y, 0.25), 0.75))
    }
}
