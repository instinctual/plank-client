import Foundation
import CoreGraphics

/// Local pad preferences only. The wire still carries normalized positions;
/// direct desktop input keeps its existing aspect-fit viewport.
struct PlankIPadPencilPadSettings: Codable, Equatable {
    enum Mapping: String, Codable, CaseIterable {
        case matchDesktop, fullPad
        var title: String { self == .matchDesktop ? "Match desktop" : "Use full pad" }
    }
    enum Tone: String, Codable, CaseIterable {
        case charcoal, warmGray
        var title: String { self == .charcoal ? "Charcoal" : "Warm gray" }
    }
    var mapping: Mapping = .matchDesktop
    var left = 0.0, right = 0.0, top = 0.0, bottom = 0.0
    var tone: Tone = .charcoal
    var glow = 0.2
    var shortcutOrder = PlankPencilModifier.allCases.map(\.rawValue)

    private enum CodingKeys: String, CodingKey {
        case mapping, left, right, top, bottom, tone, glow, shortcutOrder
    }
    init() {}
    init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy:CodingKeys.self)
        mapping = try values.decodeIfPresent(Mapping.self,forKey:.mapping) ?? .matchDesktop
        left = try values.decodeIfPresent(Double.self,forKey:.left) ?? 0
        right = try values.decodeIfPresent(Double.self,forKey:.right) ?? 0
        top = try values.decodeIfPresent(Double.self,forKey:.top) ?? 0
        bottom = try values.decodeIfPresent(Double.self,forKey:.bottom) ?? 0
        tone = try values.decodeIfPresent(Tone.self,forKey:.tone) ?? .charcoal
        glow = try values.decodeIfPresent(Double.self,forKey:.glow) ?? 0.2
        shortcutOrder = try values.decodeIfPresent([UInt16].self,forKey:.shortcutOrder)
            ?? PlankPencilModifier.allCases.map(\.rawValue)
    }
    var shortcuts: [PlankPencilModifier] {
        var seen = Set<UInt16>()
        return (shortcutOrder + PlankPencilModifier.allCases.map(\.rawValue)).compactMap {
            guard let key = PlankPencilModifier(rawValue:$0), seen.insert($0).inserted else { return nil }
            return key
        }
    }

    var validated: Self {
        var next = self
        func margin(_ value: Double) -> Double { value.isFinite ? min(max(value, 0), 0.4) : 0 }
        next.left = margin(left); next.right = margin(right)
        next.top = margin(top); next.bottom = margin(bottom)
        next.glow = glow.isFinite ? min(max(glow, 0), 1) : 0.2
        next.shortcutOrder = shortcuts.map(\.rawValue)
        return next
    }
    func viewport(bounds: CGRect, width: Int, height: Int) -> PlankIPadViewport? {
        // Validate the original canvas before insetting; negative/NaN canvases
        // must never become a seemingly valid CGRect through standardization.
        guard bounds.minX.isFinite, bounds.minY.isFinite,
              bounds.width.isFinite, bounds.height.isFinite,
              bounds.width > 32, bounds.height > 32 else { return nil }
        let canvas = bounds.insetBy(dx: 16, dy: 16)
        let settings = validated
        let area = CGRect(x: canvas.minX + canvas.width * settings.left,
                          y: canvas.minY + canvas.height * settings.top,
                          width: canvas.width * (1 - settings.left - settings.right),
                          height: canvas.height * (1 - settings.top - settings.bottom))
        return PlankIPadViewport(bounds: area, width: width, height: height,
                                preserveAspectRatio: settings.mapping == .matchDesktop)
    }
    /// Low luminance even at the upper end. This controls the app's pad fill,
    /// never the device-wide display brightness.
    var fillWhite: Double { 0.015 + validated.glow * 0.085 }
    static let storageKey = "plank.ipad.pencil.pad.v1"
    static func load(from defaults: UserDefaults = .standard) -> Self {
        guard let data = defaults.data(forKey: storageKey),
              let settings = try? JSONDecoder().decode(Self.self, from: data) else { return Self() }
        return settings.validated
    }
    func save(to defaults: UserDefaults = .standard) {
        guard let data = try? JSONEncoder().encode(validated) else { return }
        defaults.set(data, forKey: Self.storageKey)
    }
}
