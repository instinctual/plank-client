// SPDX-License-Identifier: GPL-3.0-or-later
import CoreGraphics
import CoreVideo
import Foundation
import ImageIO

/// An independently decodable, full-composite reference. This SDR JPEG is not
/// the native stream and is not suitable for judging colour-critical output.
nonisolated struct PlankPencilEncodedReferenceImage: Sendable {
    let jpegData: Data
    let sourceWidth: Int
    let sourceHeight: Int
    let encodedWidth: Int
    let encodedHeight: Int
}

/// Synchronous worker operations; the caller owns scheduling and a latest-frame
/// mailbox. Nothing here captures a UI object, persists pixels or uses CoreImage's
/// YUV conversion (the Host's xf44 samples are identity GBR, not YUV).
nonisolated enum PlankPencilReferenceImageCodec {
    enum Failure: Error {
        case invalidSourceDimensions
        case invalidSourceStorage
        case unsupportedPixelFormat
        case unavailablePixelStorage
        case imageCreationFailed
        case encodedImageTooLarge
        case invalidEncodedImage
    }

    static let maximumDimension = 1920
    static let maximumEncodedBytes = 2 * 1024 * 1024
    private static let maximumSourceDimension = 16_384
    private static let maximumSourcePixels = 33_554_432
    private static let maximumSourceStorage = 256 * 1024 * 1024

    nonisolated static func encode(_ frame: PlankRenderedFrame) throws -> PlankPencilEncodedReferenceImage {
        try validateSource(width: frame.width, height: frame.height)
        let scale = min(1.0, Double(maximumDimension) / Double(max(frame.width, frame.height)))
        let width = max(1, Int((Double(frame.width) * scale).rounded()))
        let height = max(1, Int((Double(frame.height) * scale).rounded()))
        // Allocate only the reduced output. Hardware reconstruction never makes
        // an RGBA copy of the potentially much larger original pixel buffer.
        var output = Data(count: width * height * 4)
        if let buffer = frame.pixelBuffer {
            try reconstructGBR(buffer, sourceWidth: frame.width, sourceHeight: frame.height,
                               output: &output, width: width, height: height)
        } else {
            let count = try storageSize(stride: frame.bytesPerRow, height: frame.height,
                                        minimumStride: frame.width * 4)
            guard frame.pixels.count >= count else { throw Failure.invalidSourceStorage }
            let converted = frame.pixels.withUnsafeBytes { source in
                output.withUnsafeMutableBytes { destination in
                    plank_pencil_image_convert_bgra8(
                        source.baseAddress?.assumingMemoryBound(to: UInt8.self), source.count, frame.bytesPerRow,
                        UInt32(frame.width), UInt32(frame.height),
                        destination.baseAddress?.assumingMemoryBound(to: UInt8.self), destination.count,
                        UInt32(width), UInt32(height))
                }
            }
            guard converted == 1 else { throw Failure.invalidSourceStorage }
        }
        let image = try bgraImage(output, width: width, height: height)
        // A noisy frame can exceed the wire bound. Retry at a finite number of
        // lower qualities, retaining the full composite and its geometry.
        for quality in [0.80, 0.60, 0.40] {
            let encoded = NSMutableData()
            guard let destination = CGImageDestinationCreateWithData(
                encoded, "public.jpeg" as CFString, 1, nil) else { throw Failure.imageCreationFailed }
            CGImageDestinationAddImage(destination, image, [
                kCGImageDestinationLossyCompressionQuality: quality,
                kCGImagePropertyOrientation: 1
            ] as CFDictionary)
            guard CGImageDestinationFinalize(destination) else { throw Failure.imageCreationFailed }
            if encoded.length > 0 && encoded.length <= maximumEncodedBytes {
                return PlankPencilEncodedReferenceImage(
                    jpegData: encoded as Data, sourceWidth: frame.width, sourceHeight: frame.height,
                    encodedWidth: width, encodedHeight: height)
            }
        }
        throw Failure.encodedImageTooLarge
    }

    nonisolated static func decode(_ jpegData: Data, expectedWidth: Int,
                                   expectedHeight: Int) throws -> CGImage {
        guard expectedWidth > 0, expectedHeight > 0,
              expectedWidth <= maximumDimension, expectedHeight <= maximumDimension,
              jpegData.count >= 4, jpegData.count <= maximumEncodedBytes,
              jpegData.starts(with: [0xff, 0xd8]), jpegData.suffix(2).elementsEqual([0xff, 0xd9]) else {
            throw Failure.invalidEncodedImage
        }
        let lazyOptions = [kCGImageSourceShouldCache: false,
                           kCGImageSourceShouldAllowFloat: false] as CFDictionary
        guard let source = CGImageSourceCreateWithData(jpegData as CFData, lazyOptions),
              CGImageSourceGetType(source) == "public.jpeg" as CFString,
              CGImageSourceGetCount(source) == 1,
              CGImageSourceGetStatus(source) == .statusComplete,
              let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, lazyOptions) as? [CFString: Any],
              let width = properties[kCGImagePropertyPixelWidth] as? NSNumber,
              let height = properties[kCGImagePropertyPixelHeight] as? NSNumber,
              width.intValue == expectedWidth, height.intValue == expectedHeight,
              width.doubleValue == Double(expectedWidth), height.doubleValue == Double(expectedHeight),
              (properties[kCGImagePropertyOrientation] as? NSNumber)?.intValue ?? 1 == 1 else {
            throw Failure.invalidEncodedImage
        }
        // Validate compressed dimensions before ImageIO eagerly decompresses.
        guard let decoded = CGImageSourceCreateImageAtIndex(source, 0, [
            kCGImageSourceShouldCacheImmediately: true,
            kCGImageSourceShouldAllowFloat: false
        ] as CFDictionary), CGImageSourceGetStatusAtIndex(source, 0) == .statusComplete,
              decoded.width == expectedWidth, decoded.height == expectedHeight,
              decoded.bitsPerComponent == 8,
              let colour = CGColorSpace(name: CGColorSpace.sRGB),
              let context = CGContext(data: nil, width: expectedWidth, height: expectedHeight,
                                      bitsPerComponent: 8, bytesPerRow: expectedWidth * 4,
                                      space: colour, bitmapInfo: bitmapInfo.rawValue) else {
            throw Failure.invalidEncodedImage
        }
        context.setBlendMode(.copy)
        context.draw(decoded, in: CGRect(x: 0, y: 0, width: expectedWidth, height: expectedHeight))
        guard let image = context.makeImage() else { throw Failure.imageCreationFailed }
        return image
    }

    nonisolated private static var bitmapInfo: CGBitmapInfo {
        .byteOrder32Little.union(CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue))
    }

    nonisolated private static func validateSource(width: Int, height: Int) throws {
        guard width > 0, height > 0, width <= maximumSourceDimension, height <= maximumSourceDimension else {
            throw Failure.invalidSourceDimensions
        }
        let pixels = width.multipliedReportingOverflow(by: height)
        guard !pixels.overflow, pixels.partialValue <= maximumSourcePixels else {
            throw Failure.invalidSourceDimensions
        }
    }

    nonisolated private static func storageSize(stride: Int, height: Int, minimumStride: Int) throws -> Int {
        let count = stride.multipliedReportingOverflow(by: height)
        guard stride >= minimumStride, !count.overflow, count.partialValue > 0,
              count.partialValue <= maximumSourceStorage else { throw Failure.invalidSourceStorage }
        return count.partialValue
    }

    nonisolated private static func reconstructGBR(
        _ buffer: CVPixelBuffer, sourceWidth: Int, sourceHeight: Int,
        output: inout Data, width: Int, height: Int
    ) throws {
        guard CVPixelBufferGetPixelFormatType(buffer) == kCVPixelFormatType_444YpCbCr10BiPlanarFullRange else {
            throw Failure.unsupportedPixelFormat
        }
        guard CVPixelBufferGetWidth(buffer) == sourceWidth, CVPixelBufferGetHeight(buffer) == sourceHeight,
              CVPixelBufferIsPlanar(buffer), CVPixelBufferGetPlaneCount(buffer) == 2,
              (0..<2).allSatisfy({ CVPixelBufferGetWidthOfPlane(buffer, $0) == sourceWidth &&
                                  CVPixelBufferGetHeightOfPlane(buffer, $0) == sourceHeight }) else {
            throw Failure.invalidSourceStorage
        }
        let greenStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0)
        let blueRedStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1)
        let greenCount = try storageSize(stride: greenStride, height: sourceHeight, minimumStride: sourceWidth * 2)
        let blueRedCount = try storageSize(stride: blueRedStride, height: sourceHeight, minimumStride: sourceWidth * 4)
        let total = greenCount.addingReportingOverflow(blueRedCount)
        guard !total.overflow, total.partialValue <= maximumSourceStorage,
              CVPixelBufferGetDataSize(buffer) >= total.partialValue else { throw Failure.invalidSourceStorage }
        guard CVPixelBufferLockBaseAddress(buffer, .readOnly) == kCVReturnSuccess else {
            throw Failure.unavailablePixelStorage
        }
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        guard let green = CVPixelBufferGetBaseAddressOfPlane(buffer, 0),
              let blueRed = CVPixelBufferGetBaseAddressOfPlane(buffer, 1) else {
            throw Failure.unavailablePixelStorage
        }
        let converted = output.withUnsafeMutableBytes { destination in
            plank_pencil_image_convert_gbr10(
                green.assumingMemoryBound(to: UInt8.self), greenCount, greenStride,
                blueRed.assumingMemoryBound(to: UInt8.self), blueRedCount, blueRedStride,
                UInt32(sourceWidth), UInt32(sourceHeight),
                destination.baseAddress?.assumingMemoryBound(to: UInt8.self), destination.count,
                UInt32(width), UInt32(height))
        }
        guard converted == 1 else { throw Failure.invalidSourceStorage }
    }

    nonisolated private static func bgraImage(_ data: Data, width: Int, height: Int) throws -> CGImage {
        guard let colour = CGColorSpace(name: CGColorSpace.sRGB),
              let provider = CGDataProvider(data: data as CFData),
              let image = CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32,
                                  bytesPerRow: width * 4, space: colour, bitmapInfo: bitmapInfo,
                                  provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent) else {
            throw Failure.imageCreationFailed
        }
        return image
    }
}
