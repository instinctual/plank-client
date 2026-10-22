import Foundation
import CoreFoundation
import CoreGraphics
import Combine

/// The zoom map is local chrome. These preferences have no relationship to a
/// saved Artist layout, source crop, drawing coordinates or remote input state.
@MainActor final class PlankIPadZoomNavigatorPreferences: ObservableObject {
    static let visibleKey = "plank.ipad.zoom-map.visible.v1"
    static let positionXKey = "plank.ipad.zoom-map.position-x.v1"
    static let positionYKey = "plank.ipad.zoom-map.position-y.v1"
    static let defaultPosition = CGPoint(x: 0, y: 1)

    @Published private(set) var navigatorVisible: Bool
    @Published private(set) var normalizedPosition: CGPoint
    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        if let value = defaults.object(forKey: Self.visibleKey) as? NSNumber,
           CFGetTypeID(value) == CFBooleanGetTypeID() {
            navigatorVisible = value.boolValue
        } else {
            navigatorVisible = true
        }
        if let x = Self.coordinate(defaults.object(forKey: Self.positionXKey)),
           let y = Self.coordinate(defaults.object(forKey: Self.positionYKey)) {
            normalizedPosition = CGPoint(x: x, y: y)
        } else {
            normalizedPosition = Self.defaultPosition
        }
    }

    func setVisible(_ visible: Bool) {
        guard navigatorVisible != visible else { return }
        navigatorVisible = visible
        defaults.set(visible, forKey: Self.visibleKey)
    }

    func setNormalizedPosition(_ position: CGPoint) {
        guard let position = PlankIPadZoomNavigatorPlacement.bounded(position),
              normalizedPosition != position else { return }
        normalizedPosition = position
        defaults.set(Double(position.x), forKey: Self.positionXKey)
        defaults.set(Double(position.y), forKey: Self.positionYKey)
    }

    func resetPosition() { setNormalizedPosition(Self.defaultPosition) }

    private static func coordinate(_ object: Any?) -> CGFloat? {
        guard let number = object as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID(),
              number.doubleValue.isFinite else { return nil }
        return CGFloat(min(1, max(0, number.doubleValue)))
    }
}

/// Normalized position describes the panel's top-left travel range, rather than
/// an absolute screen point. Resizing a window preserves the chosen corner or
/// relative location without scaling the map, its handle or its touch targets.
enum PlankIPadZoomNavigatorPlacement {
    static let panelSize = CGSize(width: 140, height: 152)
    static let chromeGutter: CGFloat = 4

    static func bounded(_ position: CGPoint) -> CGPoint? {
        guard position.x.isFinite, position.y.isFinite else { return nil }
        return CGPoint(x: min(1, max(0, position.x)), y: min(1, max(0, position.y)))
    }

    static func rect(position: CGPoint, usableBounds: CGRect, excluding excludedRects: [CGRect]) -> CGRect? {
        guard valid(usableBounds), usableBounds.width >= panelSize.width,
              usableBounds.height >= panelSize.height, let position = bounded(position) else { return nil }
        let minX = usableBounds.minX, maxX = usableBounds.maxX - panelSize.width
        let minY = usableBounds.minY, maxY = usableBounds.maxY - panelSize.height
        let desired = CGPoint(x: minX + position.x * (maxX - minX),
                              y: minY + position.y * (maxY - minY))
        let exclusions = excludedRects.filter(valid).map { $0.insetBy(dx: -chromeGutter, dy: -chromeGutter) }
        // The nearest legal rectangle occurs at the desired origin, a window
        // edge, or an exclusion edge. Search those finite boundary candidates;
        // never shrink the panel or move remote content to make chrome fit.
        var xs = [desired.x, minX, maxX], ys = [desired.y, minY, maxY]
        for exclusion in exclusions {
            xs += [min(maxX, max(minX, exclusion.minX - panelSize.width)),
                   min(maxX, max(minX, exclusion.maxX))]
            ys += [min(maxY, max(minY, exclusion.minY - panelSize.height)),
                   min(maxY, max(minY, exclusion.maxY))]
        }
        xs = Array(Set(xs)).sorted(); ys = Array(Set(ys)).sorted()
        var best: CGRect?, bestDistance = CGFloat.infinity
        for x in xs {
            for y in ys {
                let candidate = CGRect(origin: CGPoint(x: x, y: y), size: panelSize)
                guard !exclusions.contains(where: { overlaps(candidate, $0) }) else { continue }
                let distance = hypot(x - desired.x, y - desired.y)
                if best == nil || distance < bestDistance { best = candidate; bestDistance = distance }
            }
        }
        return best
    }

    static func normalizedPosition(for origin: CGPoint, in usableBounds: CGRect) -> CGPoint? {
        guard origin.x.isFinite, origin.y.isFinite, valid(usableBounds),
              usableBounds.width >= panelSize.width, usableBounds.height >= panelSize.height else { return nil }
        let width = usableBounds.width - panelSize.width, height = usableBounds.height - panelSize.height
        return bounded(CGPoint(x: width == 0 ? 0 : (origin.x - usableBounds.minX) / width,
                               y: height == 0 ? 0 : (origin.y - usableBounds.minY) / height))
    }

    private static func valid(_ rect: CGRect) -> Bool {
        !rect.isNull && !rect.isInfinite && rect.origin.x.isFinite && rect.origin.y.isFinite && rect.width.isFinite && rect.height.isFinite
            && rect.maxX.isFinite && rect.maxY.isFinite && rect.width > 0 && rect.height > 0
    }

    private static func overlaps(_ lhs: CGRect, _ rhs: CGRect) -> Bool {
        let intersection = lhs.intersection(rhs)
        return !intersection.isNull && intersection.width > 0 && intersection.height > 0
    }
}
