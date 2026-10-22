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
    private let metalVideo = PlankMetalVideoView(frame: .zero)

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .black
        isOpaque = true
        isUserInteractionEnabled = false
        layer.contentsGravity = .resizeAspect
        addSubview(metalVideo)
        metalVideo.isHidden = true
        cursorLayer.contentsGravity = .resize
        cursorLayer.isHidden = true
        layer.addSublayer(cursorLayer)
        cursorFallback.bounds = CGRect(x: 0, y: 0, width: 12, height: 12)
        cursorFallback.cornerRadius = 6
        cursorFallback.borderWidth = 2
        cursorFallback.borderColor = UIColor.cyan.cgColor
        cursorFallback.isHidden = true
        layer.addSublayer(cursorFallback)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    func display(_ frame: PlankRenderedFrame?) {
        if let frame, let pixelBuffer = frame.pixelBuffer, metalVideo.canRender {
            dimensions = PlankFrameDimensions(width: frame.width, height: frame.height)
            layoutVideo()
            layer.contents = nil
            metalVideo.isHidden = false
            metalVideo.display(pixelBuffer)
            layoutCursor()
            return
        }
        metalVideo.isHidden = true
        guard let frame,
              let provider = CGDataProvider(data: frame.pixels as CFData),
              let colorSpace = CGColorSpace(name: CGColorSpace.sRGB) else {
            layer.contents = nil
            dimensions = nil
            cursorLayer.isHidden = true
            cursorFallback.isHidden = true
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
        layer.contents = image
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
        layoutVideo()
        layoutCursor()
    }

    private func layoutVideo() {
        guard let dimensions, bounds.width > 0, bounds.height > 0 else {
            metalVideo.frame = bounds
            return
        }
        let scale = min(
            bounds.width / CGFloat(dimensions.width),
            bounds.height / CGFloat(dimensions.height)
        )
        let width = CGFloat(dimensions.width) * scale
        let height = CGFloat(dimensions.height) * scale
        metalVideo.frame = CGRect(
            x: (bounds.width - width) / 2,
            y: (bounds.height - height) / 2,
            width: width,
            height: height
        )
    }

    private func layoutCursor() {
        guard let dimensions, let cursor,
              dimensions.width == cursor.frameWidth,
              dimensions.height == cursor.frameHeight,
              bounds.width > 0, bounds.height > 0 else {
            cursorLayer.isHidden = true
            cursorFallback.isHidden = true
            return
        }
        let scale = min(
            bounds.width / CGFloat(dimensions.width),
            bounds.height / CGFloat(dimensions.height)
        )
        let imageWidth = CGFloat(dimensions.width) * scale
        let imageHeight = CGFloat(dimensions.height) * scale
        let pointer = CGPoint(
            x: (bounds.width - imageWidth) / 2 + CGFloat(cursor.x) * scale,
            y: (bounds.height - imageHeight) / 2 + CGFloat(cursor.y) * scale
        )
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
