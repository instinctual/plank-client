/// The pinned virtual-startup Host accepts only its qualified EDID modes.
/// These 16:10 modes are the closest supported landscape shape to this
/// iPad Air's 2360x1640 panel. They are NOT an exact native-aspect mode.
/// Keep a legacy bookmark selectable without silently changing its session.
enum PlankIPadDisplayOptions {
    static let defaultSize = SpatialDisplaySize.wuxga
    static let presets: [SpatialDisplaySize] = [.wuxga, .tall2560]

    static func choices(current: SpatialDisplaySize) -> [SpatialDisplaySize] {
        presets.contains(current) ? presets : presets + [current]
    }
    static func title(_ size: SpatialDisplaySize) -> String {
        switch size {
        case .wuxga: return "Balanced · \(size.title)"
        case .tall2560: return "Sharper · \(size.title)"
        default: return "Saved · \(size.title)"
        }
    }
}
