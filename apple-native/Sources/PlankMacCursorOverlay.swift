import AppKit

// A real AppKit subview owns the cursor layer. AppKit can reorder backing
// layers for video subviews; a sibling CALayer alone can end up behind video.
final class PlankMacCursorOverlay: NSView {
    private let sprite = CALayer()
    private var position: PlankRemoteCursor?
    private var shape: PlankRemoteCursorShape?
    private var shapeImage: CGImage?
    private var localMouse = false
    private var cachedNativeCursor: NSCursor?
    private var cachedCursorScale: Double?
    var nativeMouseCursor: NSCursor? {
        if let shape, !shape.visible { return nil }
        guard let shape, let shapeImage, width > 0 else { return .arrow }
        let scale = min(max(PlankMacCoordinates.canvas(in: bounds, width: width, height: height).width / Double(width), 0.5), 1)
        if cachedNativeCursor == nil || cachedCursorScale != scale {
            let size = NSSize(width: Double(shape.width) * scale, height: Double(shape.height) * scale)
            cachedNativeCursor = NSCursor(image: NSImage(cgImage: shapeImage, size: size),
                hotSpot: NSPoint(x: Double(shape.hotspotX) * scale, y: Double(shape.hotspotY) * scale))
            cachedCursorScale = scale
        }
        return cachedNativeCursor
    }
    func setLocalMouse(_ active: Bool) {
        guard localMouse != active else { return }
        localMouse = active; placeCursor()
    }
    private let fallbackImage: CGImage?
    private let fallbackSize: CGSize
    private let fallbackHotspot: CGPoint
    private var width = 0, height = 0
    private var geometry: PlankMacDisplayGeometry?
    private(set) var replacesSystemCursor = false
    override var isFlipped: Bool { true }
    override func hitTest(_ point: NSPoint) -> NSView? { nil }

    override init(frame: CGRect) {
        let arrow = NSCursor.arrow
        fallbackImage = arrow.image.cgImage(forProposedRect: nil, context: nil, hints: nil)
        fallbackSize = arrow.image.size
        fallbackHotspot = arrow.hotSpot
        super.init(frame: frame)
        wantsLayer = true
        layer?.masksToBounds = true
        layer?.zPosition = 10
        layer?.addSublayer(sprite)
        sprite.contentsGravity = .resize
        sprite.isHidden = true
    }
    required init?(coder: NSCoder) { fatalError("init(coder:)") }

    func setDimensions(width: Int, height: Int) {
        self.width = width; self.height = height
        geometry = PlankMacDisplayGeometry(frameWidth: width, frameHeight: height, topology: nil, outputIndex: 0)
        placeCursor()
    }
    func setGeometry(_ geometry: PlankMacDisplayGeometry) {
        self.geometry = geometry
        width = geometry.source.width; height = geometry.source.height; placeCursor()
    }
    func cursor(_ position: PlankRemoteCursor?) { self.position = position; placeCursor() }
    func cursorShape(_ shape: PlankRemoteCursorShape?) {
        self.shape = shape
        shapeImage = nil; cachedNativeCursor = nil; cachedCursorScale = nil
        // Artwork changes infrequently; never recreate it for every movement.
        if let shape, shape.width > 0, shape.height > 0,
           shape.pixels.count == shape.width * shape.height * 4,
           let provider = CGDataProvider(data: shape.pixels as CFData) {
            shapeImage = CGImage(width: shape.width, height: shape.height, bitsPerComponent: 8, bitsPerPixel: 32,
                bytesPerRow: shape.width * 4, space: CGColorSpaceCreateDeviceRGB(),
                bitmapInfo: .byteOrder32Little.union(CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue)),
                provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
        }
        placeCursor()
    }
    override func layout() { super.layout(); placeCursor() }

    private func placeCursor() {
        CATransaction.begin(); CATransaction.setDisableActions(true)
        defer { CATransaction.commit() }
        replacesSystemCursor = false
        sprite.isHidden = true
        guard !localMouse else { return }
        guard let position, width > 1, height > 1,
              let local = geometry?.localCursor(position),
              bounds.width > 0, bounds.height > 0 else { return }
        let canvas = PlankMacCoordinates.canvas(in: bounds, width: width, height: height)
        let scale = canvas.width / Double(width)
        let pointer = CGPoint(x: canvas.minX + local.x * scale,
                              y: canvas.minY + local.y * scale)
        if let shape, !shape.visible {
            // Honor an explicit Host request to hide its pointer.
            replacesSystemCursor = true
            return
        }
        var image: CGImage?
        var size: CGSize
        var hotspot: CGPoint
        if let shape, let shapeImage {
            image = shapeImage
            let cursorScale = min(max(scale, 0.5), 1)
            size = CGSize(width: Double(shape.width) * cursorScale, height: Double(shape.height) * cursorScale)
            hotspot = CGPoint(x: Double(shape.hotspotX) * cursorScale, y: Double(shape.hotspotY) * cursorScale)
        } else {
            // Positions can precede the first shape. Use an arrow at the actual
            // Host position so both raw Wacom and mouse remain visible.
            image = fallbackImage
            size = fallbackSize
            hotspot = fallbackHotspot
        }
        sprite.contents = image
        sprite.frame = CGRect(origin: CGPoint(x: pointer.x - hotspot.x, y: pointer.y - hotspot.y), size: size)
        sprite.isHidden = image == nil
        replacesSystemCursor = image != nil
    }
}

enum PlankMacDesktopWindow {
    @MainActor static func toggle(_ window: NSWindow, sender: Any? = nil) {
        configure(window)
        NSLog("PLANK Mac fullscreen request: window=%ld fullScreen=%d resizable=%d",
            window.windowNumber, window.styleMask.contains(.fullScreen) ? 1 : 0,
            window.styleMask.contains(.resizable) ? 1 : 0)
        window.toggleFullScreen(sender)
    }
    @MainActor static func configure(_ window: NSWindow) {
        // SwiftUI can reset capabilities as it moves the content into full
        // screen. Preserve the fullScreen flag and restore the capability to
        // leave it; avoid rebuilding chrome when the flags already match.
        if !window.styleMask.contains(.resizable) { window.styleMask.insert(.resizable) }
        if !window.collectionBehavior.contains(.fullScreenPrimary) {
            window.collectionBehavior.remove([.fullScreenAuxiliary, .fullScreenNone])
            window.collectionBehavior.insert(.fullScreenPrimary)
        }
        if let button = window.standardWindowButton(.zoomButton) {
            if !button.isEnabled { button.isEnabled = true }
            if let presentation = window.delegate as? PlankMacWindowPresentation {
                button.target = presentation
                button.action = #selector(PlankMacWindowPresentation.toggleDesktopFullScreen(_:))
            } else {
                button.target = window
                button.action = #selector(NSWindow.toggleFullScreen(_:))
            }
        }
    }
}
