import CoreGraphics
import CoreVideo
import Metal
import QuartzCore
import UIKit

// One decoded xf44 image is mapped directly to two Metal textures. The Host
// stores G in Y and B/R in Cb/Cr, so standard video presentation is incorrect.
final class PlankMetalVideoView: UIView {
    private final class FrameLease: @unchecked Sendable {
        let buffer: CVPixelBuffer
        let green: CVMetalTexture
        let blueRed: CVMetalTexture

        init(buffer: CVPixelBuffer, green: CVMetalTexture, blueRed: CVMetalTexture) {
            self.buffer = buffer
            self.green = green
            self.blueRed = blueRed
        }
    }

    override class var layerClass: AnyClass { CAMetalLayer.self }

    private let device: MTLDevice?
    private let queue: MTLCommandQueue?
    private let cache: CVMetalTextureCache?
    private let pipeline: MTLRenderPipelineState?
    private var latestBuffer: CVPixelBuffer?
    private var frameID = DispatchTime.now().uptimeNanoseconds

    private var metalLayer: CAMetalLayer { layer as! CAMetalLayer }

    override init(frame: CGRect) {
        let device = MTLCreateSystemDefaultDevice()
        self.device = device
        queue = device?.makeCommandQueue()
        var newCache: CVMetalTextureCache?
        if let device {
            CVMetalTextureCacheCreate(kCFAllocatorDefault, nil, device, nil, &newCache)
        }
        cache = newCache

        let source = """
        #include <metal_stdlib>
        using namespace metal;
        struct Vertex { float4 position [[position]]; float2 uv; };
        vertex Vertex plankVertex(const device float4 *data [[buffer(0)]], uint id [[vertex_id]]) {
            Vertex out;
            out.position = float4(data[id].xy, 0, 1);
            out.uv = data[id].zw;
            return out;
        }
        constexpr sampler pixelSampler(coord::normalized, address::clamp_to_edge, filter::linear);
        fragment float4 plankIdentityGBR(Vertex input [[stage_in]],
                texture2d<float> greenPlane [[texture(0)]],
                texture2d<float> blueRedPlane [[texture(1)]]) {
            float green = greenPlane.sample(pixelSampler, input.uv).r;
            float2 blueRed = blueRedPlane.sample(pixelSampler, input.uv).rg;
            // xf44 stores each 10-bit code left-aligned in a 16-bit word.
            float scale = 65535.0 / 65472.0;
            return float4(saturate(blueRed.y * scale),
                          saturate(green * scale),
                          saturate(blueRed.x * scale), 1.0);
        }
        """
        if let device,
           let library = try? device.makeLibrary(source: source, options: nil),
           let vertex = library.makeFunction(name: "plankVertex"),
           let fragment = library.makeFunction(name: "plankIdentityGBR") {
            let descriptor = MTLRenderPipelineDescriptor()
            descriptor.vertexFunction = vertex
            descriptor.fragmentFunction = fragment
            descriptor.colorAttachments[0].pixelFormat = .rgba16Float
            pipeline = try? device.makeRenderPipelineState(descriptor: descriptor)
        } else {
            pipeline = nil
        }
        super.init(frame: frame)
        isOpaque = true
        isUserInteractionEnabled = false
        metalLayer.device = device
        metalLayer.pixelFormat = .rgba16Float
        metalLayer.framebufferOnly = true
        metalLayer.colorspace = CGColorSpace(name: CGColorSpace.sRGB)
        metalLayer.backgroundColor = UIColor.black.cgColor
        metalLayer.contentsScale = contentScaleFactor
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) has not been implemented") }

    var canRender: Bool { queue != nil && cache != nil && pipeline != nil }

    func display(_ buffer: CVPixelBuffer) {
        latestBuffer = buffer
        frameID &+= 1
        drawLatest()
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        drawLatest()
    }

    private func drawLatest() {
        guard let buffer = latestBuffer, let queue, let cache, let pipeline,
              bounds.width > 0, bounds.height > 0 else { return }
        // A visionOS window is sized in points (1280 wide by default), while
        // the remote desktop is commonly 2560 pixels wide or more. Drawing at
        // the point size discards detail before the system scales the window.
        let size = CGSize(
            width: CVPixelBufferGetWidth(buffer),
            height: CVPixelBufferGetHeight(buffer)
        )
        if metalLayer.drawableSize != size { metalLayer.drawableSize = size }
        var green: CVMetalTexture?
        var blueRed: CVMetalTexture?
        let greenStatus = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, cache, buffer, nil, .r16Unorm,
            CVPixelBufferGetWidthOfPlane(buffer, 0),
            CVPixelBufferGetHeightOfPlane(buffer, 0), 0, &green
        )
        let blueRedStatus = CVMetalTextureCacheCreateTextureFromImage(
            kCFAllocatorDefault, cache, buffer, nil, .rg16Unorm,
            CVPixelBufferGetWidthOfPlane(buffer, 1),
            CVPixelBufferGetHeightOfPlane(buffer, 1), 1, &blueRed
        )
        guard greenStatus == kCVReturnSuccess, blueRedStatus == kCVReturnSuccess,
              let green, let blueRed,
              let greenTexture = CVMetalTextureGetTexture(green),
              let blueRedTexture = CVMetalTextureGetTexture(blueRed) else { return }
        // nextDrawable() blocks this (main) thread when no drawable is free.
        let drawableStart = DispatchTime.now().uptimeNanoseconds
        let nextDrawable = metalLayer.nextDrawable()
        PlankTimingCapture.shared.drawableAcquired(
            drawableWaitNanos: DispatchTime.now().uptimeNanoseconds - drawableStart,
            gotDrawable: nextDrawable != nil,
            background: UIApplication.shared.applicationState == .background
        )
        guard let drawable = nextDrawable,
              let command = queue.makeCommandBuffer() else { return }

        var vertices: [SIMD4<Float>] = [
            SIMD4(-1,  1, 0, 0),
            SIMD4(-1, -1, 0, 1),
            SIMD4( 1,  1, 1, 0),
            SIMD4( 1, -1, 1, 1)
        ]
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = drawable.texture
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        guard let encoder = command.makeRenderCommandEncoder(descriptor: pass) else { return }
        encoder.setRenderPipelineState(pipeline)
        encoder.setVertexBytes(&vertices, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
        encoder.setFragmentTexture(greenTexture, index: 0)
        encoder.setFragmentTexture(blueRedTexture, index: 1)
        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        encoder.endEncoding()
        let lease = FrameLease(buffer: buffer, green: green, blueRed: blueRed)
        command.addCompletedHandler { buffer in
            withExtendedLifetime(lease) {}
            PlankTimingCapture.shared.gpuCompleted(
                seconds: buffer.gpuEndTime - buffer.gpuStartTime,
                succeeded: buffer.status == .completed
            )
        }
        // The simulator Metal SDK omits presentation callbacks. Do not invent
        // scanout timing from GPU completion; retain real-device measurements.
#if !targetEnvironment(simulator)
        let submittedFrameID = frameID
        drawable.addPresentedHandler { shown in
            PlankTimingCapture.shared.presented(frameID: submittedFrameID, time: shown.presentedTime)
        }
#endif
        command.present(drawable)
        PlankTimingCapture.shared.submitted()
        command.commit()
    }
}
