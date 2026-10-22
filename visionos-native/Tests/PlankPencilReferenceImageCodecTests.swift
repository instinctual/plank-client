// SPDX-License-Identifier: GPL-3.0-or-later
// Compile this source and the production codec with the production
// PlankRenderedFrame model (or its exact field-only fixture for macOS checks).
import CoreGraphics
import CoreVideo
import Foundation
import ImageIO

@main
@MainActor
enum PlankPencilReferenceImageCodecTests {
    typealias Codec = PlankPencilReferenceImageCodec
    static var checks = 0

    static func check(_ condition: Bool, _ message: String, line: Int = #line) {
        checks += 1
        precondition(condition, "line \(line): \(message)")
    }

    static func rejects(_ expected: Codec.Failure, _ body: () throws -> Void) {
        do { try body(); check(false, "expected \(expected)") }
        catch let actual as Codec.Failure { check(String(describing: actual) == String(describing: expected), "\(actual) == \(expected)") }
        catch { check(false, "unexpected failure: \(error)") }
    }

    static func main() throws {
        try softwareBGRA()
        try hardwareIdentityGBR()
        try completeCompositeDownscale()
        try boundsAndMalformedInput()
        try noisyFrameStaysBounded()
        print("PlankPencilReferenceImageCodecTests: \(checks) checks passed")
    }

    // Solid patches are wider than a JPEG chroma block. Test their centres,
    // including asymmetric top/bottom colours, without claiming losslessness.
    static func colour(_ x: Int, _ y: Int, width: Int, height: Int) -> SIMD3<UInt8> {
        if y < height / 2 {
            return x < width / 2 ? SIMD3(255, 0, 0) : SIMD3(0, 255, 0)
        }
        return x < width / 2 ? SIMD3(0, 0, 255) : SIMD3(64, 128, 192)
    }

    static func software(width: Int, height: Int, padding: Int = 0) -> PlankRenderedFrame {
        let stride = width * 4 + padding
        var bytes = Data(count: stride * height)
        bytes.withUnsafeMutableBytes { raw in
            for y in 0..<height {
                for x in 0..<width {
                    let rgb = colour(x, y, width: width, height: height)
                    let offset = y * stride + x * 4
                    raw[offset] = rgb.z; raw[offset + 1] = rgb.y; raw[offset + 2] = rgb.x
                    raw[offset + 3] = 255
                }
                for x in (width * 4)..<stride { raw[y * stride + x] = 231 }
            }
        }
        return PlankRenderedFrame(pixels: bytes, pixelBuffer: nil, width: width, height: height,
                                  bytesPerRow: stride, frameNumber: 1)
    }

    static func rgb(_ image: CGImage, x: Int, y: Int) -> SIMD3<Int> {
        guard let bytes = image.dataProvider?.data, let pointer = CFDataGetBytePtr(bytes) else {
            preconditionFailure("missing decoded bytes")
        }
        check(image.bitsPerComponent == 8 && image.bitsPerPixel == 32, "explicit SDR 8-bit output")
        let offset = y * image.bytesPerRow + x * 4
        return SIMD3(Int(pointer[offset + 2]), Int(pointer[offset + 1]), Int(pointer[offset]))
    }

    static func near(_ actual: SIMD3<Int>, _ expected: SIMD3<UInt8>, tolerance: Int = 12) {
        check(abs(actual.x - Int(expected.x)) <= tolerance &&
              abs(actual.y - Int(expected.y)) <= tolerance &&
              abs(actual.z - Int(expected.z)) <= tolerance,
              "RGB \(actual) approximates \(expected); identity channel order survives JPEG")
    }

    static func patches(_ image: CGImage) {
        for y in [image.height / 4, image.height * 3 / 4] {
            for x in [image.width / 4, image.width * 3 / 4] {
                near(rgb(image, x: x, y: y), colour(x, y, width: image.width, height: image.height))
            }
        }
        check(image.colorSpace?.name == CGColorSpace.sRGB, "decoded reference uses sRGB")
    }

    static func softwareBGRA() throws {
        let frame = software(width: 96, height: 64, padding: 28)
        let encoded = try Codec.encode(frame)
        check(encoded.sourceWidth == 96 && encoded.sourceHeight == 64, "original composite dimensions retained")
        check(encoded.encodedWidth == 96 && encoded.encodedHeight == 64, "small source is not enlarged")
        check(encoded.jpegData.starts(with: [0xff, 0xd8]), "independent JPEG image")
        patches(try Codec.decode(encoded.jpegData, expectedWidth: 96, expectedHeight: 64))
        // Premultiplied-first RGB is composited over the renderer's black
        // backdrop, not divided by alpha or interpreted as straight-alpha RGBA.
        let dark = PlankRenderedFrame(pixels: Data(repeating: 64, count: 32 * 32 * 4), pixelBuffer: nil,
                                      width: 32, height: 32, bytesPerRow: 128, frameNumber: 2)
        let darkJPEG = try Codec.encode(dark)
        near(rgb(try Codec.decode(darkJPEG.jpegData, expectedWidth: 32, expectedHeight: 32), x: 16, y: 16), SIMD3(64, 64, 64))
    }

    static func pixelBuffer(width: Int, height: Int, format: OSType) -> CVPixelBuffer {
        var buffer: CVPixelBuffer?
        let result = CVPixelBufferCreate(kCFAllocatorDefault, width, height, format, nil, &buffer)
        check(result == kCVReturnSuccess && buffer != nil, "CoreVideo allocates actual \(format) storage")
        return buffer!
    }

    static func hardwareIdentityGBR() throws {
        let width = 100, height = 64
        let buffer = pixelBuffer(width: width, height: height, format: kCVPixelFormatType_444YpCbCr10BiPlanarFullRange)
        check(CVPixelBufferGetPlaneCount(buffer) == 2, "actual xf44 has two planes")
        check(CVPixelBufferLockBaseAddress(buffer, []) == kCVReturnSuccess, "hardware fixture lock")
        let green = CVPixelBufferGetBaseAddressOfPlane(buffer, 0)!
        let br = CVPixelBufferGetBaseAddressOfPlane(buffer, 1)!
        let greenStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0)
        let brStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1)
        check(greenStride >= width * 2 && brStride >= width * 4, "validated independent plane strides")
        for y in 0..<height {
            for x in 0..<width {
                let rgb = colour(x, y, width: width, height: height)
                func word(_ value: UInt8) -> UInt16 {
                    UInt16((Double(value) * 1023 / 255).rounded()) << 6
                }
                green.storeBytes(of: word(rgb.y).littleEndian, toByteOffset: y * greenStride + x * 2, as: UInt16.self)
                br.storeBytes(of: word(rgb.z).littleEndian, toByteOffset: y * brStride + x * 4, as: UInt16.self)
                br.storeBytes(of: word(rgb.x).littleEndian, toByteOffset: y * brStride + x * 4 + 2, as: UInt16.self)
            }
        }
        CVPixelBufferUnlockBaseAddress(buffer, [])
        let frame = PlankRenderedFrame(pixels: Data(), pixelBuffer: buffer, width: width, height: height,
                                      bytesPerRow: 0, frameNumber: 3)
        let encoded = try Codec.encode(frame)
        patches(try Codec.decode(encoded.jpegData, expectedWidth: width, expectedHeight: height))
        // White's left-aligned max is 65472, rather than 65535. Its round trip
        // must reach white without using a video-range/YUV matrix.
        check(CVPixelBufferLockBaseAddress(buffer, []) == kCVReturnSuccess, "white fixture lock")
        for y in 0..<height {
            for x in 0..<width {
                green.storeBytes(of: UInt16(65472).littleEndian, toByteOffset: y * greenStride + x * 2, as: UInt16.self)
                br.storeBytes(of: UInt16(65472).littleEndian, toByteOffset: y * brStride + x * 4, as: UInt16.self)
                br.storeBytes(of: UInt16(65472).littleEndian, toByteOffset: y * brStride + x * 4 + 2, as: UInt16.self)
            }
        }
        CVPixelBufferUnlockBaseAddress(buffer, [])
        let white = try Codec.encode(frame)
        near(rgb(try Codec.decode(white.jpegData, expectedWidth: width, expectedHeight: height), x: 48, y: 32), SIMD3(255, 255, 255), tolerance: 2)
        let wrongSize = PlankRenderedFrame(pixels: Data(), pixelBuffer: buffer, width: width + 1, height: height,
                                          bytesPerRow: 0, frameNumber: 4)
        rejects(.invalidSourceStorage) { _ = try Codec.encode(wrongSize) }
    }

    static func completeCompositeDownscale() throws {
        let encoded = try Codec.encode(software(width: 2560, height: 1280, padding: 32))
        check(encoded.sourceWidth == 2560 && encoded.sourceHeight == 1280, "full original composite metadata")
        check(encoded.encodedWidth == 1920 && encoded.encodedHeight == 960, "aspect retained at bounded resolution")
        let image = try Codec.decode(encoded.jpegData, expectedWidth: 1920, expectedHeight: 960)
        patches(image)
        // Both far edges remain present; this catches a crop masquerading as a
        // scale, including loss of a second display in a composite.
        near(rgb(image, x: 2, y: 2), SIMD3(255, 0, 0))
        near(rgb(image, x: 1917, y: 957), SIMD3(64, 128, 192))
    }

    static func boundsAndMalformedInput() throws {
        func frame(_ width: Int, _ height: Int, _ stride: Int, _ bytes: Data = Data()) -> PlankRenderedFrame {
            PlankRenderedFrame(pixels: bytes, pixelBuffer: nil, width: width, height: height,
                               bytesPerRow: stride, frameNumber: 0)
        }
        for dimensions in [(0, 1), (-1, 1), (16_385, 1), (1, 16_385), (8192, 4097), (Int.max, 1)] {
            rejects(.invalidSourceDimensions) { _ = try Codec.encode(frame(dimensions.0, dimensions.1, 0)) }
        }
        rejects(.invalidSourceStorage) { _ = try Codec.encode(frame(8, 8, 31, Data(count: 248))) }
        rejects(.invalidSourceStorage) { _ = try Codec.encode(frame(8, 8, 32, Data(count: 255))) }
        rejects(.invalidSourceStorage) { _ = try Codec.encode(frame(8, 8, Int.max)) }
        rejects(.invalidSourceStorage) { _ = try Codec.encode(frame(8, 8, -1)) }
        let unsupported = pixelBuffer(width: 8, height: 8, format: kCVPixelFormatType_32BGRA)
        rejects(.unsupportedPixelFormat) {
            _ = try Codec.encode(PlankRenderedFrame(pixels: Data(), pixelBuffer: unsupported, width: 8, height: 8,
                                                   bytesPerRow: 32, frameNumber: 0))
        }
        let valid = try Codec.encode(software(width: 64, height: 48))
        rejects(.invalidEncodedImage) { _ = try Codec.decode(Data(), expectedWidth: 64, expectedHeight: 48) }
        rejects(.invalidEncodedImage) { _ = try Codec.decode(Data("not a JPEG".utf8), expectedWidth: 64, expectedHeight: 48) }
        rejects(.invalidEncodedImage) { _ = try Codec.decode(valid.jpegData, expectedWidth: 63, expectedHeight: 48) }
        rejects(.invalidEncodedImage) { _ = try Codec.decode(valid.jpegData, expectedWidth: 0, expectedHeight: 48) }
        rejects(.invalidEncodedImage) { _ = try Codec.decode(valid.jpegData, expectedWidth: 1921, expectedHeight: 48) }
        rejects(.invalidEncodedImage) { _ = try Codec.decode(Data(valid.jpegData.prefix(100)), expectedWidth: 64, expectedHeight: 48) }
        rejects(.invalidEncodedImage) { _ = try Codec.decode(Data(valid.jpegData.dropLast(2)), expectedWidth: 64, expectedHeight: 48) }
        rejects(.invalidEncodedImage) { _ = try Codec.decode(Data(count: Codec.maximumEncodedBytes + 1), expectedWidth: 64, expectedHeight: 48) }
        let pngBytes = NSMutableData()
        let image = try Codec.decode(valid.jpegData, expectedWidth: 64, expectedHeight: 48)
        let destination = CGImageDestinationCreateWithData(pngBytes, "public.png" as CFString, 1, nil)!
        CGImageDestinationAddImage(destination, image, nil)
        check(CGImageDestinationFinalize(destination), "non-JPEG fixture encoded")
        rejects(.invalidEncodedImage) { _ = try Codec.decode(pngBytes as Data, expectedWidth: 64, expectedHeight: 48) }
        // Maximum dimension in a narrow source is valid and remains full width.
        let narrow = try Codec.encode(software(width: 16_384, height: 1))
        check(narrow.encodedWidth == 1920 && narrow.encodedHeight == 1, "source dimension limit inclusive")
    }

    static func noisyFrameStaysBounded() throws {
        let width = 1920, height = 1920
        var bytes = Data(count: width * height * 4)
        bytes.withUnsafeMutableBytes { raw in
            var random: UInt32 = 0x1845abcf
            for offset in stride(from: 0, to: raw.count, by: 4) {
                for channel in 0..<3 {
                    random ^= random << 13; random ^= random >> 17; random ^= random << 5
                    raw[offset + channel] = UInt8(truncatingIfNeeded: random)
                }
                raw[offset + 3] = 255
            }
        }
        let encoded = try Codec.encode(PlankRenderedFrame(pixels: bytes, pixelBuffer: nil,
            width: width, height: height, bytesPerRow: width * 4, frameNumber: 9))
        check(encoded.jpegData.count <= Codec.maximumEncodedBytes, "worst-pattern reference obeys 2 MiB wire bound")
        check(encoded.encodedWidth == width && encoded.encodedHeight == height, "quality fallback preserves geometry")
        let image = try Codec.decode(encoded.jpegData, expectedWidth: width, expectedHeight: height)
        check(image.width == width && image.height == height, "bounded noisy JPEG is independently decodable")
    }
}
