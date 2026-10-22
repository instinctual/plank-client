import AppKit

@main @MainActor struct PlankMacPresentationTests {
    static var checks = 0
    static func check(_ value: Bool, _ message: String) { checks += 1; precondition(value, message) }
    static func position(width: Int = 100, height: Int = 100) -> PlankRemoteCursor {
        PlankRemoteCursor(x: 50, y: 50, frameWidth: width, frameHeight: height, sequence: 1)
    }
    static func main() {
        _ = NSApplication.shared
        let root = NSView(frame: CGRect(x: 0, y: 0, width: 100, height: 100))
        root.wantsLayer = true
        let video = NSView(frame: root.bounds); video.wantsLayer = true
        video.layer?.backgroundColor = NSColor.green.cgColor
        root.addSubview(video)
        let overlay = PlankMacCursorOverlay(frame: root.bounds)
        root.addSubview(overlay, positioned: .above, relativeTo: video)
        let window = NSWindow(contentRect: root.bounds, styleMask: [.titled,.closable], backing: .buffered, defer: false)
        window.contentView = root
        overlay.setDimensions(width: 100, height: 100)
        check(!overlay.replacesSystemCursor, "no Host position keeps native arrow")
        overlay.cursor(position())
        check(overlay.replacesSystemCursor, "position before shape has a visible fallback arrow")
        let sprite = overlay.layer!.sublayers!.first!
        check(!sprite.isHidden && sprite.contents != nil, "fallback has actual artwork")
        let shape = PlankRemoteCursorShape(pixels: Data([0,0,255,255, 0,0,255,255, 0,0,255,255, 0,0,255,255]),
            width: 2, height: 2, hotspotX: 0, hotspotY: 0, visible: true, generation: 1)
        overlay.cursorShape(shape)
        check(!sprite.isHidden && sprite.frame == CGRect(x: 50,y: 50,width: 2,height: 2), "Host cursor position and artwork")
        let native = overlay.nativeMouseCursor!
        check(native.image.size == NSSize(width: 2,height: 2) && native.hotSpot == .zero, "native mouse uses Host artwork and hotspot")
        overlay.setLocalMouse(true)
        check(sprite.isHidden && !overlay.replacesSystemCursor, "mouse ownership suppresses echoed Host sprite")
        overlay.cursor(PlankRemoteCursor(x: 1, y: 1, frameWidth: 100, frameHeight: 100, sequence: 2))
        check(sprite.isHidden && overlay.nativeMouseCursor === native, "late mouse acknowledgement cannot move sprite or recreate artwork")
        overlay.setLocalMouse(false)
        check(!sprite.isHidden && sprite.frame.origin == NSPoint(x: 1,y: 1), "tablet ownership resumes actual Host position")
        overlay.cursor(position())
        overlay.frame = CGRect(x: 0, y: 0, width: 200, height: 120)
        overlay.layout()
        check(!sprite.isHidden && sprite.frame.origin == CGPoint(x: 100, y: 60), "windowed resize maps live tablet sprite into new letterboxed canvas")
        overlay.setLocalMouse(true)
        check(sprite.isHidden, "mouse handoff after Space resize leaves no parked Host sprite")
        overlay.frame = root.bounds; overlay.layout(); overlay.setLocalMouse(false)
        check(!sprite.isHidden && sprite.frame.origin == CGPoint(x: 50, y: 50), "tablet handoff after fullscreen restores one live Host sprite")
        check(root.subviews.last === overlay && overlay.layer!.zPosition > video.layer!.zPosition, "cursor owns a view above video")
        // AppKit keeps backing layers of an unshown test window detached.
        // Compose its actual production cursor layer above the opaque video
        // layer to verify artwork and alpha, alongside the view-order check.
        root.layoutSubtreeIfNeeded()
        let composition = CALayer()
        composition.frame = root.bounds
        composition.addSublayer(video.layer!)
        composition.addSublayer(overlay.layer!)
        let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: 100, pixelsHigh: 100,
            bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false,
            colorSpaceName: .deviceRGB, bytesPerRow: 400, bitsPerPixel: 32)!
        let graphics = NSGraphicsContext(bitmapImageRep: bitmap)!
        composition.render(in: graphics.cgContext)
        let color = bitmap.colorAt(x: 50,y: 50)!.usingColorSpace(.deviceRGB)!
        check(color.redComponent > 0.9 && color.greenComponent < 0.1, "Host cursor pixels appear above opaque video after layout")
        overlay.cursorShape(PlankRemoteCursorShape(pixels: shape.pixels,width: 2,height: 2,hotspotX: 0,hotspotY: 0,visible: false,generation: 2))
        check(sprite.isHidden && overlay.replacesSystemCursor, "explicit Host hiding respected")
        overlay.setDimensions(width: 200,height: 100)
        check(!overlay.replacesSystemCursor && sprite.isHidden, "stale position not mapped into a new resolution")
        overlay.cursorShape(nil)
        overlay.cursor(position(width: 200))
        check(overlay.replacesSystemCursor && !sprite.isHidden, "new-resolution position recovers visible fallback")
        overlay.setDimensions(width: 0,height: 0)
        check(!overlay.replacesSystemCursor && sprite.isHidden, "disconnect releases native pointer hiding")
        window.collectionBehavior = [.fullScreenAuxiliary]
        PlankMacDesktopWindow.configure(window)
        check(window.styleMask.contains(.resizable), "desktop resizable")
        check(window.collectionBehavior.contains(.fullScreenPrimary) && !window.collectionBehavior.contains(.fullScreenAuxiliary), "desktop can be primary fullscreen window")
        check(window.standardWindowButton(.zoomButton)?.isEnabled == true, "green fullscreen control enabled")
        check(window.standardWindowButton(.zoomButton)?.target === window &&
            window.standardWindowButton(.zoomButton)?.action == #selector(NSWindow.toggleFullScreen(_:)),
            "green window button invokes fullscreen rather than zoom")
        let shortcut = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: [.control,.command], timestamp: 0,
            windowNumber: 0, context: nil, characters: "f", charactersIgnoringModifiers: "f", isARepeat: false, keyCode: 3)!
        check(PlankMacKeys.staysLocal(shortcut), "fullscreen shortcut is not forwarded to Host")
        typealias Pointer = PlankMacPointerPresentation
        check(Pointer.mode(appActive: true, onCanvas: true, mouseOwns: false, mouseArtwork: true,
            currentHostPosition: true, overlayOwns: false) == .hidden,
            "parked mouse hides when pen cursor is on the other output")
        check(Pointer.mode(appActive: true, onCanvas: true, mouseOwns: true, mouseArtwork: true,
            currentHostPosition: true, overlayOwns: true) == .mouse,
            "physical mouse handoff selects native artwork despite old tablet sprite")
        check(Pointer.mode(appActive: true, onCanvas: false, mouseOwns: false, mouseArtwork: true,
            currentHostPosition: true, overlayOwns: true) == .local,
            "local controls always keep their visible pointer")
        check(Pointer.mode(appActive: false, onCanvas: true, mouseOwns: false, mouseArtwork: true,
            currentHostPosition: true, overlayOwns: true) == .local,
            "inactive app cannot hide another app pointer")
        check(Pointer.mode(appActive: true, onCanvas: true, mouseOwns: false, mouseArtwork: true,
            currentHostPosition: false, overlayOwns: false) == .local,
            "disconnect or stale resolution restores local cursor")
        check(Pointer.mode(appActive: true, onCanvas: true, mouseOwns: true, mouseArtwork: false,
            currentHostPosition: true, overlayOwns: false) == .hidden,
            "Host hidden cursor remains hidden for mouse")
        print("Mac cursor composition and fullscreen: \(checks) checks passed")
    }
}
