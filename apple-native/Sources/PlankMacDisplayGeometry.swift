import AppKit
import CoreGraphics

extension PlankTopology {
    /// Window roles are primary/secondary; stream rectangles stay in Host
    /// coordinates. A primary output on the right must not be recropped as left.
    var macPresentationOutputs: [Output] {
        let spatial = orderedOutputs
        return spatial.filter(\.primary) + spatial.filter { !$0.primary }
    }
    func localDisplayID(outputIndex: Int, layout: PlankLocalDisplayLayout) -> UInt32? {
        guard macPresentationOutputs.indices.contains(outputIndex),
              let spatialIndex = orderedOutputs.firstIndex(where: { $0.id == macPresentationOutputs[outputIndex].id }) else { return nil }
        return layout.displayID(spatialIndex: spatialIndex)
    }
}

enum PlankMacDisplayPriority {
    struct Display {
        let id: CGDirectDisplayID
        let frame: CGRect
    }
    static func orderedIDs(_ displays: [Display], primary: CGDirectDisplayID) -> [CGDirectDisplayID] {
        displays.sorted {
            if ($0.id == primary) != ($1.id == primary) { return $0.id == primary }
            return ($0.frame.minX, $0.frame.minY, $0.id) < ($1.frame.minX, $1.frame.minY, $1.id)
        }.map(\.id)
    }
    static func targetID(outputIndex: Int, displays: [Display], primary: CGDirectDisplayID) -> CGDirectDisplayID? {
        guard outputIndex >= 0, outputIndex < 2 else { return nil }
        let ids = orderedIDs(displays, primary: primary)
        guard !ids.isEmpty else { return nil }
        return ids[min(outputIndex, ids.count - 1)]
    }
    @MainActor static func id(_ screen: NSScreen) -> CGDirectDisplayID? {
        (screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? NSNumber)?.uint32Value
    }
    @MainActor static func screens() -> [NSScreen] {
        // NSScreen.main follows keyboard focus. CGMainDisplayID follows the
        // system primary instead; fetch current screens on each assignment.
        let current = NSScreen.screens
        let displays = current.compactMap { screen in id(screen).map { Display(id: $0, frame: screen.frame) } }
        return orderedIDs(displays, primary: CGMainDisplayID()).compactMap { target in current.first { id($0) == target } }
    }
    @MainActor static func snapshot() -> PlankLocalDisplayLayout {
        let primary = CGMainDisplayID()
        return .init(displays: NSScreen.screens.compactMap { screen in
            guard let displayID = id(screen) else { return nil }
            let bounds = screen.frame
            return .init(id: displayID, bounds: .init(x: Int(bounds.minX), y: Int(bounds.minY),
                width: Int(bounds.width), height: Int(bounds.height)), primary: displayID == primary)
        })
    }
    @MainActor static func screen(outputIndex: Int, topology: PlankTopology, layout: PlankLocalDisplayLayout?) -> NSScreen? {
        let current = NSScreen.screens
        if let layout, let target = topology.localDisplayID(outputIndex: outputIndex, layout: layout),
           let screen = current.first(where: { id($0) == target }) { return screen }
        // When a mapped display is unplugged, keep both windows reachable.
        // Unmapped/manual arrangements retain role-based window placement.
        let ordered = screens()
        guard !ordered.isEmpty else { return nil }
        return ordered[min(outputIndex, ordered.count - 1)]
    }
}

/// Source pixels, local points and Host desktop origins are different spaces.
/// Use source_rect for both the video crop and absolute input into the one
/// composite stream. Local Retina scale never participates in remote mapping.
struct PlankMacDisplayGeometry: Equatable {
    let frameWidth, frameHeight: Int
    let source: PlankTopology.Rect
    init?(frameWidth: Int, frameHeight: Int, topology: PlankTopology?, outputIndex: Int) {
        guard frameWidth > 0, frameHeight > 0 else { return nil }
        self.frameWidth = frameWidth; self.frameHeight = frameHeight
        if let topology, topology.splitPresentation {
            guard topology.desktopWidth == frameWidth, topology.desktopHeight == frameHeight,
                  topology.macPresentationOutputs.indices.contains(outputIndex) else { return nil }
            source = topology.macPresentationOutputs[outputIndex].sourceRect
        } else {
            guard outputIndex == 0 else { return nil }
            source = .init(x: 0, y: 0, width: frameWidth, height: frameHeight)
        }
        guard source.fits(width: frameWidth, height: frameHeight) else { return nil }
    }
    var normalizedCrop: CGRect {
        CGRect(x: Double(source.x) / Double(frameWidth), y: Double(source.y) / Double(frameHeight),
               width: Double(source.width) / Double(frameWidth), height: Double(source.height) / Double(frameHeight))
    }
    func canvas(in bounds: CGRect) -> CGRect {
        PlankMacCoordinates.canvas(in: bounds, width: source.width, height: source.height)
    }
    func remote(_ point: CGPoint, in bounds: CGRect, dragging: Bool) -> (Int, Int)? {
        guard let (x, y) = PlankMacCoordinates.remote(point, canvas: canvas(in: bounds),
            width: source.width, height: source.height, dragging: dragging) else { return nil }
        return (x + source.x, y + source.y)
    }
    func localCursor(_ cursor: PlankRemoteCursor) -> CGPoint? {
        guard cursor.frameWidth == frameWidth, cursor.frameHeight == frameHeight,
              cursor.x >= source.x, cursor.x < source.x + source.width,
              cursor.y >= source.y, cursor.y < source.y + source.height else { return nil }
        return CGPoint(x: cursor.x - source.x, y: cursor.y - source.y)
    }
}

enum PlankMacSessionFocus {
    struct Window { let key, main: Bool }
    static func active(appActive: Bool, windows: [Window],
                       presentationInProgress: Bool = false, previouslyActive: Bool = false) -> Bool {
        appActive && ((presentationInProgress && previouslyActive) || windows.contains { $0.key || $0.main })
    }
}
