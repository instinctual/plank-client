import Foundation
import CoreFoundation

/// A local drawing policy shared by both artist-control surfaces. Transparency
/// changes color layers only; view alpha and input ownership do not depend on it.
struct PlankIPadControlAppearance: Equatable {
    static let preferenceKey = "plank.ipad.custom-control-transparency.v1"
    static let defaultTransparency = 0.0
    let transparency: Double

    init(transparency: Double = Self.defaultTransparency) {
        self.transparency = Self.normalized(transparency)
    }
    static func normalized(_ value: Double) -> Double {
        guard value.isFinite else { return defaultTransparency }
        return min(1,max(0,value))
    }
    static func load(from defaults: UserDefaults = .standard) -> Double {
        guard let number = defaults.object(forKey:preferenceKey) as? NSNumber,
              CFGetTypeID(number) != CFBooleanGetTypeID(), number.doubleValue.isFinite
        else { return defaultTransparency }
        return normalized(number.doubleValue)
    }
    static func save(_ value: Double,to defaults: UserDefaults = .standard) {
        guard value.isFinite else { return }
        defaults.set(normalized(value),forKey:preferenceKey)
    }

    var idleFillAlpha: Double { 0.94 * (1 - transparency) }
    var idleBorderAlpha: Double { 0.8 * (1 - transparency) }
    /// A fully transparent control still exposes its label and touch target.
    var idleLabelAlpha: Double { 1 - 0.7 * transparency }
    /// Held keys must remain distinguishable even over the lightest desktop.
    var pressedFillAlpha: Double { 1 - 0.45 * transparency }
    var pressedBorderAlpha: Double { 1 - 0.25 * transparency }
    var pressedLabelAlpha: Double { 1 }
}
