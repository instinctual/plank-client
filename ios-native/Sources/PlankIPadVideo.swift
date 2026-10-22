import UIKit
import CoreVideo

// Extracted from the Vision fit/cursor container; keep behavior aligned until
// the upstream shared presentation move. Metal uses the original source.
final class PlankIPadVideoView: UIView {
    private var dimensions: PlankFrameDimensions?
    private var cursor: PlankRemoteCursor?
    private var cursorShape: PlankRemoteCursorShape?
    private let cursorLayer = CALayer()
    private let cursorFallback = CALayer()
    private let softwareImage = CALayer()
    private let viewportClip = UIView(frame:.zero)
    private let metalVideo = PlankMetalVideoView(frame: .zero)
    private var suppliedViewport: PlankIPadViewport?
    private var ownsViewport = false

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .black
        isOpaque = true
        isUserInteractionEnabled = false
        viewportClip.isUserInteractionEnabled = false
        viewportClip.clipsToBounds = true
        addSubview(viewportClip)
        viewportClip.addSubview(metalVideo)
        softwareImage.contentsGravity = .resize
        viewportClip.layer.addSublayer(softwareImage)
        metalVideo.isHidden = true
        cursorLayer.contentsGravity = .resize
        cursorLayer.isHidden = true
        viewportClip.layer.addSublayer(cursorLayer)
        cursorFallback.bounds = CGRect(x: 0, y: 0, width: 12, height: 12)
        cursorFallback.cornerRadius = 6
        cursorFallback.borderWidth = 2
        cursorFallback.borderColor = UIColor.cyan.cgColor
        cursorFallback.isHidden = true
        viewportClip.layer.addSublayer(cursorFallback)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    /// Input owns this immutable transform. A local crop changes neither the
    /// UIKit drawing surface nor the retained decoded source frame.
    func setViewport(_ viewport: PlankIPadViewport?) {
        ownsViewport = true
        suppliedViewport = viewport
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        layoutVideo()
        layoutCursor()
        CATransaction.commit()
    }
    private var viewport: PlankIPadViewport? {
        guard let dimensions else { return nil }
        if ownsViewport {
            guard let suppliedViewport,
                  suppliedViewport.width == dimensions.width,
                  suppliedViewport.height == dimensions.height else { return nil }
            return suppliedViewport
        }
        // Standalone presentation retains Fit; the desktop supplies exactly
        // the same viewport used by its inverse input mapping.
        return PlankIPadViewport(bounds:bounds,width:dimensions.width,height:dimensions.height)
    }

    func display(_ frame: PlankRenderedFrame?) {
        if let frame, let pixelBuffer = frame.pixelBuffer, metalVideo.canRender {
            dimensions = PlankFrameDimensions(width: frame.width, height: frame.height)
            layoutVideo()
            softwareImage.contents = nil
            metalVideo.isHidden = false
            metalVideo.display(pixelBuffer)
            layoutCursor()
            return
        }
        metalVideo.isHidden = true
        guard let frame,
              let provider = CGDataProvider(data: frame.pixels as CFData),
              let colorSpace = CGColorSpace(name: CGColorSpace.sRGB) else {
            softwareImage.contents = nil
            dimensions = nil
            cursorLayer.isHidden = true
            cursorFallback.isHidden = true
            viewportClip.isHidden = true
            return
        }
        let bitmapInfo = CGBitmapInfo.byteOrder32Little.union(
            CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue)
        )
        guard let image = CGImage(
            width: frame.width,
            height: frame.height,
            bitsPerComponent: 8,
            bitsPerPixel: 32,
            bytesPerRow: frame.bytesPerRow,
            space: colorSpace,
            bitmapInfo: bitmapInfo,
            provider: provider,
            decode: nil,
            shouldInterpolate: false,
            intent: .defaultIntent
        ) else { return }
        dimensions = PlankFrameDimensions(width: frame.width, height: frame.height)
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        softwareImage.contents = image
        layoutVideo()
        layoutCursor()
        CATransaction.commit()
    }

    func displayCursor(_ cursor: PlankRemoteCursor?) {
        self.cursor = cursor
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        layoutCursor()
        CATransaction.commit()
    }

    func displayCursorShape(_ shape: PlankRemoteCursorShape?) {
        cursorShape = shape
        cursorLayer.contents = nil
        if let shape,
           let provider = CGDataProvider(data: shape.pixels as CFData),
           let colorSpace = CGColorSpace(name: CGColorSpace.sRGB) {
            let bitmapInfo = CGBitmapInfo.byteOrder32Little.union(
                CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue)
            )
            cursorLayer.contents = CGImage(
                width: shape.width,
                height: shape.height,
                bitsPerComponent: 8,
                bitsPerPixel: 32,
                bytesPerRow: shape.width * 4,
                space: colorSpace,
                bitmapInfo: bitmapInfo,
                provider: provider,
                decode: nil,
                shouldInterpolate: false,
                intent: .defaultIntent
            )
        }
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        layoutCursor()
        CATransaction.commit()
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        layoutVideo()
        layoutCursor()
        CATransaction.commit()
    }

    private func layoutVideo() {
        guard let viewport else {
            viewportClip.isHidden = true
            return
        }
        viewportClip.isHidden = false
        viewportClip.frame = viewport.rect
        let frame = viewport.fullImageFrame.offsetBy(dx:-viewport.rect.minX,dy:-viewport.rect.minY)
        metalVideo.frame = frame
        softwareImage.frame = frame
    }

    private func layoutCursor() {
        guard let dimensions, let cursor, let viewport,
              dimensions.width == cursor.frameWidth,
              dimensions.height == cursor.frameHeight,
              let point = viewport.point(x:cursor.x,y:cursor.y) else {
            cursorLayer.isHidden = true
            cursorFallback.isHidden = true
            return
        }
        let scale = viewport.fullImageFrame.width / CGFloat(dimensions.width)
        let pointer = CGPoint(x:point.x-viewport.rect.minX,y:point.y-viewport.rect.minY)
        guard let cursorShape else {
            cursorLayer.isHidden = true
            cursorFallback.position = pointer
            cursorFallback.isHidden = false
            return
        }
        cursorFallback.isHidden = true
        guard cursorShape.visible, cursorLayer.contents != nil else {
            cursorLayer.isHidden = true
            return
        }
        // Limit the minimum size to half the Host cursor. Full-size Host
        // artwork overwhelms small controls in a spatial desktop window.
        let cursorScale = min(max(scale, 0.5), 1)
        cursorLayer.frame = CGRect(
            x: pointer.x - CGFloat(cursorShape.hotspotX) * cursorScale,
            y: pointer.y - CGFloat(cursorShape.hotspotY) * cursorScale,
            width: CGFloat(cursorShape.width) * cursorScale,
            height: CGFloat(cursorShape.height) * cursorScale
        )
        cursorLayer.isHidden = false
    }
}
