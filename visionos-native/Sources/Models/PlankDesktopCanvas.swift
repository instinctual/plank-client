import CoreGraphics

/// One full decoded desktop, including both top-aligned outputs. Video, cursor
/// and pointer capture use this same aspect-fit rectangle in local points.
/// Raw Wacom reports remain device data; they do not pass through this mapping.
struct PlankDesktopCanvas {
    let dimensions: PlankFrameDimensions

    func rect(in size: CGSize) -> CGRect {
        guard dimensions.width > 0, dimensions.height > 0,
              size.width.isFinite, size.height.isFinite,
              size.width > 0, size.height > 0 else { return .zero }
        let scale = min(size.width / CGFloat(dimensions.width),
                        size.height / CGFloat(dimensions.height))
        let width = CGFloat(dimensions.width) * scale
        let height = CGFloat(dimensions.height) * scale
        return CGRect(x: (size.width - width) / 2, y: (size.height - height) / 2,
                      width: width, height: height)
    }

    func remotePoint(_ point: CGPoint, in size: CGSize) -> (x: Int, y: Int)? {
        let canvas = rect(in: size)
        guard canvas.width > 0, canvas.height > 0, point.x.isFinite, point.y.isFinite,
              point.x >= canvas.minX, point.x <= canvas.maxX,
              point.y >= canvas.minY, point.y <= canvas.maxY else { return nil }
        // Preserve the final row/column at the far edge, as in the original
        // single-display view. Spatial window size never changes Host pixels.
        return (
            min(dimensions.width - 1,
                Int(((point.x - canvas.minX) / canvas.width * CGFloat(dimensions.width - 1)).rounded())),
            min(dimensions.height - 1,
                Int(((point.y - canvas.minY) / canvas.height * CGFloat(dimensions.height - 1)).rounded()))
        )
    }
}
