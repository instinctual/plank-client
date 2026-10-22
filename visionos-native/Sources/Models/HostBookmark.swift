import Foundation
import CoreVideo

enum KeyboardFunctionKeyMode: String, CaseIterable, Identifiable, Sendable {
    case pc
    case appleExtended

    var id: String { rawValue }

    var title: String {
        switch self {
        case .pc: return "Windows / PC Keyboard"
        case .appleExtended: return "Apple Extended Keyboard"
        }
    }
}

enum SpatialDisplaySize: String, Codable, CaseIterable, Identifiable, Sendable {
    case portrait1024
    case portrait1280
    case fullHD
    case wuxga
    case standard
    case tall2560
    case portrait2560
    case wide
    case wide3840
    case ultraHD
    case dci4K
    case ultrawide

    var id: String { rawValue }

    var title: String { virtualMode.replacingOccurrences(of: "x", with: " × ") }

    var virtualMode: String {
        switch self {
        case .portrait1024: return "1024x2160"
        case .portrait1280: return "1280x2160"
        case .fullHD: return "1920x1080"
        case .wuxga: return "1920x1200"
        case .standard: return "2560x1440"
        case .tall2560: return "2560x1600"
        case .portrait2560: return "2560x2160"
        case .wide: return "3440x1440"
        case .wide3840: return "3840x1600"
        case .ultraHD: return "3840x2160"
        case .dci4K: return "4096x2160"
        case .ultrawide: return "5120x2160"
        }
    }

    var pixelSize: (width: Int, height: Int) {
        let components = virtualMode.split(separator: "x").compactMap { Int($0) }
        return (components[0], components[1])
    }

}

enum StreamFrameRate {
    static let defaultValue = 60
    static let presets = [24, 25, 30, 48, 50, 60, 90, 120]

    static func normalized(_ value: Int) -> Int {
        (1...240).contains(value) ? value : defaultValue
    }
}

/// Startup encoder target sent as `video.encoder_target_kbps`. The range,
/// step and HEVC 10-bit 4:4:4 default match the desktop Client's per-profile
/// bitrate (`app/settings/streamingpreferences.h`). This is a Host encoder
/// target, not a guaranteed received rate.
enum StreamBitrate {
    static let minimumKbps = 10_000
    static let maximumKbps = 150_000
    static let stepKbps = 500
    static let defaultKbps = 50_000

    /// Clamps to the supported range and rounds to the nearest step, so a
    /// saved or edited value can never leave the qualified bounds.
    static func normalized(_ kbps: Int) -> Int {
        let clamped = min(max(kbps, minimumKbps), maximumKbps)
        let steps = (clamped - minimumKbps + stepKbps / 2) / stepKbps
        return min(minimumKbps + steps * stepKbps, maximumKbps)
    }

    static func megabitsLabel(_ kbps: Int) -> String {
        kbps % 1_000 == 0 ? "\(kbps / 1_000) Mbps" : String(format: "%.1f Mbps", Double(kbps) / 1_000)
    }
}

struct HostBookmark: Identifiable, Codable, Hashable, Sendable {
    var id: UUID
    var name: String
    var address: String
    var port: UInt16
    var lastConnectedAt: Date?
    var spatialDisplaySize: SpatialDisplaySize
    var streamFrameRate: Int
    var videoBitrateKbps: Int
    /// nil preserves the existing single-display bookmark. Mac only.
    var secondDisplaySize: SpatialDisplaySize?

    init(
        id: UUID = UUID(),
        name: String,
        address: String,
        port: UInt16 = 28989,
        lastConnectedAt: Date? = nil,
        spatialDisplaySize: SpatialDisplaySize = .standard,
        streamFrameRate: Int = StreamFrameRate.defaultValue,
        videoBitrateKbps: Int = StreamBitrate.defaultKbps,
        secondDisplaySize: SpatialDisplaySize? = nil
    ) {
        self.id = id
        self.name = name
        self.address = address
        self.port = port
        self.lastConnectedAt = lastConnectedAt
        self.spatialDisplaySize = spatialDisplaySize
        self.streamFrameRate = StreamFrameRate.normalized(streamFrameRate)
        self.videoBitrateKbps = StreamBitrate.normalized(videoBitrateKbps)
        self.secondDisplaySize = secondDisplaySize
    }

    private enum CodingKeys: String, CodingKey {
        case id, name, address, port, lastConnectedAt, spatialDisplaySize, streamFrameRate
        case videoBitrateKbps, secondDisplaySize
    }

    init(from decoder: Decoder) throws {
        let values = try decoder.container(keyedBy: CodingKeys.self)
        id = try values.decode(UUID.self, forKey: .id)
        name = try values.decode(String.self, forKey: .name)
        address = try values.decode(String.self, forKey: .address)
        port = try values.decode(UInt16.self, forKey: .port)
        lastConnectedAt = try values.decodeIfPresent(Date.self, forKey: .lastConnectedAt)
        spatialDisplaySize = try values.decodeIfPresent(
            SpatialDisplaySize.self,
            forKey: .spatialDisplaySize
        ) ?? .standard
        streamFrameRate = StreamFrameRate.normalized(
            (try? values.decodeIfPresent(Int.self, forKey: .streamFrameRate)) ??
            StreamFrameRate.defaultValue
        )
        secondDisplaySize = try? values.decodeIfPresent(SpatialDisplaySize.self, forKey: .secondDisplaySize)
        // Bookmarks saved before this field existed keep the previous fixed
        // 50 Mbps target. A malformed value must not throw: HostStore decodes
        // the whole list, so one failure would drop every bookmark.
        videoBitrateKbps = StreamBitrate.normalized(
            (try? values.decodeIfPresent(Int.self, forKey: .videoBitrateKbps)) ??
            StreamBitrate.defaultKbps
        )
    }
}

struct PlankHostIdentity: Equatable, Sendable {
    let name: String
    let uniqueID: String
    let version: String
    let supportsAuthentication: Bool
}

struct PlankAuthentication: Equatable, Sendable {
    let sessionToken: String
    let desktopStage: String
}

struct PlankApplication: Equatable, Sendable {
    let id: Int
    let title: String
}

/// Bookmark edits compare against the settings retained by the authenticated
/// connection, not against an editor's possibly newer saved snapshot.
enum PlankBookmarkSessionPolicy {
    static func requiresClosure(edited: HostBookmark, connected: HostBookmark?,
                                authenticated: Bool) -> Bool {
        guard authenticated, let connected, edited.id == connected.id else { return false }
        return edited.spatialDisplaySize != connected.spatialDisplaySize ||
            edited.secondDisplaySize != connected.secondDisplaySize ||
            StreamFrameRate.normalized(edited.streamFrameRate) !=
                StreamFrameRate.normalized(connected.streamFrameRate) ||
            edited.address != connected.address || edited.port != connected.port
    }
}

struct PlankTopology: Equatable, Sendable {
    struct Layout: Equatable, Sendable {
        let kind: String
        let virtualModes: [String]
        var startupKind: String? = nil
    }

    let schemaVersion: Int
    let featureFlags: Int
    let generation: String
    let desktopWidth: Int
    let desktopHeight: Int
    let layout: Layout
    var desktopX = 0
    var desktopY = 0
    var outputs: [Output] = []
    var requestedPrimaryOutput: Int? = nil

    struct Rect: Equatable, Sendable {
        let x, y, width, height: Int
        func fits(width: Int, height: Int) -> Bool {
            x >= 0 && y >= 0 && self.width > 0 && self.height > 0 &&
                x <= width && y <= height && self.width <= width - x && self.height <= height - y
        }
    }
    struct Output: Equatable, Sendable, Identifiable {
        let id, name: String
        let x, y, width, height: Int
        let primary: Bool
        let sourceRect: Rect
    }
    var orderedOutputs: [Output] {
        outputs.sorted { ($0.x, $0.y, $0.id) < ($1.x, $1.y, $1.id) }
    }
    var splitPresentation: Bool { layout.kind == "dual-horizontal" }
    var displayMode: String { splitPresentation ? "separate-displays" : "scaled-span" }

    var supportsVirtualPrimary: Bool { featureFlags & 0x2000000 != 0 && layout.startupKind == "single" }
    var launchLayoutQuery: [URLQueryItem] {
        var query = layout.virtualModes.prefix(2).enumerated().map { URLQueryItem(name: "plankVirtualMode\($0.offset + 1)", value: $0.element) }
        if let primary = requestedPrimaryOutput, supportsVirtualPrimary,
           primary >= 0, primary < (splitPresentation ? 2 : 1) {
            query.append(.init(name: "plankPrimaryOutput", value: String(primary)))
        }
        return query
    }
    func requesting(_ displaySize: SpatialDisplaySize, second: SpatialDisplaySize? = nil,
                    primaryOutput: Int? = nil) -> PlankTopology {
        let first = displaySize.pixelSize
        let other = second?.pixelSize
        let primary = supportsVirtualPrimary && second != nil && (primaryOutput == 0 || primaryOutput == 1) ? primaryOutput : nil
        let matches = matches(displaySize, second: second, primaryOutput: primary)
        return PlankTopology(schemaVersion: schemaVersion, featureFlags: featureFlags,
            generation: generation, desktopWidth: first.width + (other?.width ?? 0),
            desktopHeight: max(first.height, other?.height ?? 0),
            layout: .init(kind: second == nil ? "single" : "dual-horizontal",
                          virtualModes: [displaySize.virtualMode] + (second.map { [$0.virtualMode] } ?? []),
                          startupKind: layout.startupKind),
            desktopX: matches ? desktopX : 0, desktopY: matches ? desktopY : 0,
            outputs: matches ? outputs : [], requestedPrimaryOutput: primary)
    }
    func matches(_ displaySize: SpatialDisplaySize, second: SpatialDisplaySize? = nil,
                 primaryOutput: Int? = nil) -> Bool {
        let other = second?.pixelSize
        if let second {
            guard orderedOutputs.count == 2,
                  orderedOutputs[0].width == displaySize.pixelSize.width,
                  orderedOutputs[0].height == displaySize.pixelSize.height,
                  orderedOutputs[1].width == second.pixelSize.width,
                  orderedOutputs[1].height == second.pixelSize.height else { return false }
        }
        if let primaryOutput {
            guard supportsVirtualPrimary, orderedOutputs.indices.contains(primaryOutput),
                  orderedOutputs[primaryOutput].primary,
                  orderedOutputs[primaryOutput].id == "x11:DP-0" else { return false }
        }
        return layout.kind == (second == nil ? "single" : "dual-horizontal") &&
            layout.virtualModes == [displaySize.virtualMode] + (second.map { [$0.virtualMode] } ?? []) &&
            desktopWidth == displaySize.pixelSize.width + (other?.width ?? 0) &&
            desktopHeight == max(displaySize.pixelSize.height, other?.height ?? 0)
    }
    static func validCanvas(first: SpatialDisplaySize, second: SpatialDisplaySize?) -> Bool {
        // Host v13 plank_topology.h limits the combined virtual canvas to 8192.
        first.pixelSize.width + (second?.pixelSize.width ?? 0) <= 8192
    }

}

/// Local logical bounds are queried once per connection. They determine
/// spatial output association and primary-side negotiation, never video crops
/// or transformations of raw Wacom reports. AppKit bounds use y upwards.
struct PlankLocalDisplayLayout: Equatable, Sendable {
    struct Display: Equatable, Sendable {
        let id: UInt32
        let bounds: PlankTopology.Rect
        let primary: Bool
    }
    let displays: [Display]
    var ordered: [Display] { displays.sorted { ($0.bounds.x, $0.bounds.y, $0.id) < ($1.bounds.x, $1.bounds.y, $1.id) } }
    var primarySpatialIndex: Int? {
        guard displays.count == 2, Set(displays.map(\.id)).count == 2,
              displays.allSatisfy({ $0.bounds.width > 0 && $0.bounds.height > 0 &&
                $0.bounds.width <= 16384 && $0.bounds.height <= 16384 &&
                abs(Double($0.bounds.x)) <= 1_000_000 && abs(Double($0.bounds.y)) <= 1_000_000 }),
              displays.filter(\.primary).count == 1 else { return nil }
        let left = ordered[0].bounds, right = ordered[1].bounds
        guard left.x + left.width <= right.x,
              left.y < right.y + right.height, right.y < left.y + left.height else { return nil }
        return ordered.firstIndex { $0.primary }
    }
    func displayID(spatialIndex: Int) -> UInt32? {
        guard primarySpatialIndex != nil, ordered.indices.contains(spatialIndex) else { return nil }
        return ordered[spatialIndex].id
    }
}

struct PlankLaunchCredentials: Equatable, Sendable {
    let transportPort: UInt16
    let certificateSHA256: String
    let transportToken: String
    let udpPayloadMTU: UInt32
    let encodingMode: String
}

struct PlankFrameProbe: Equatable, Sendable {
    let byteCount: Int
    let frameNumber: UInt64
    let isKeyFrame: Bool
    let negotiationSummary: String
}

// CVPixelBuffer is immutable after VideoToolbox returns it. The Metal renderer
// retains it until its command buffer completes.
struct PlankRenderedFrame: @unchecked Sendable {
    let pixels: Data
    let pixelBuffer: CVPixelBuffer?
    let width: Int
    let height: Int
    let bytesPerRow: Int
    let frameNumber: UInt64
}

struct PlankFrameDimensions: Equatable, Sendable {
    let width: Int
    let height: Int
}

struct PlankRemoteCursor: Equatable, Sendable {
    let x: Int
    let y: Int
    let frameWidth: Int
    let frameHeight: Int
    let sequence: UInt64
}

struct PlankRemoteCursorShape: Equatable, Sendable {
    let pixels: Data
    let width: Int
    let height: Int
    let hotspotX: Int
    let hotspotY: Int
    let visible: Bool
    let generation: UInt64
}

enum PlankCursorUpdate: Sendable {
    case position(PlankRemoteCursor)
    case shape(PlankRemoteCursorShape)
}

struct DiscoveredHost: Identifiable, Hashable, Sendable {
    let serviceName: String
    let endpointDescription: String

    var id: String { serviceName }
}
