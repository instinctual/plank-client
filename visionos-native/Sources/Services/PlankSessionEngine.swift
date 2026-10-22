import Foundation
import CoreVideo

enum PlankHostFeature {
    static let normalizedPen: UInt32 = 0x01
    static let rawHidTablet: UInt32 = 0x04
    static let rawHidFocusSuspend: UInt32 = 0x20
    static let tabletRelayRequired = rawHidTablet | rawHidFocusSuspend
}

struct PlankVideoProgress: Sendable {
    let received: UInt64
    let decoded: UInt64
    let keyFrames: UInt64
    let receiveDrops: UInt64
    let fecUnrecovered: UInt64
    let frameGaps: UInt64
    let averageDecodeMilliseconds: Double
    let decoder: String
}

// The Rust endpoint synchronizes its input and video queues independently.
// Keep it alive until the input sender has exited before destroying it.
private final class PlankTransportHandle: @unchecked Sendable {
    let pointer: OpaquePointer

    init(_ pointer: OpaquePointer) {
        self.pointer = pointer
    }
}

private final class PlankInputSenderState: @unchecked Sendable {
    private let lock = NSLock()
    private var failed = false
    private var accepted: UInt64 = 0
    private var maxSendNanos: UInt64 = 0
    private var failedSendNanos: UInt64 = 0
    private var tabletReportsAccepted: UInt64 = 0
    private var tabletMessagesAccepted: UInt64 = 0
    private var lastTabletReportSend: UInt64 = 0

    func markFailed() {
        lock.lock()
        failed = true
        lock.unlock()
    }

    func recordSend(nanos: UInt64, succeeded: Bool, tabletType: UInt16? = nil) {
        lock.lock()
        maxSendNanos = max(maxSendNanos, nanos)
        if succeeded { accepted &+= 1 } else { failedSendNanos = nanos }
        if succeeded, let tabletType {
            tabletMessagesAccepted &+= 1
            if tabletType == 3 {
                tabletReportsAccepted &+= 1
                lastTabletReportSend = DispatchTime.now().uptimeNanoseconds
            }
        }
        lock.unlock()
    }

    var tabletSummary: (reports: UInt64, messages: UInt64, lastReport: UInt64) {
        lock.lock()
        defer { lock.unlock() }
        return (tabletReportsAccepted, tabletMessagesAccepted, lastTabletReportSend)
    }

    /// Events the native endpoint accepted, slowest submission, and the
    /// duration of the failed submission (0 when none failed).
    var sendSummary: (accepted: UInt64, maxNanos: UInt64, failedNanos: UInt64) {
        lock.lock()
        defer { lock.unlock() }
        return (accepted, maxSendNanos, failedSendNanos)
    }

    var hasFailed: Bool {
        lock.lock()
        defer { lock.unlock() }
        return failed
    }
}

enum PlankSessionError: LocalizedError, Sendable {
    case transport(String)
    case invalidNegotiation
    case noVideo

    var errorDescription: String? {
        switch self {
        case let .transport(message): return message
        case .invalidNegotiation: return "The Host returned invalid native session values."
        case .noVideo: return "The Host started the session but sent no video frame."
        }
    }
}

// Shape chunks arrive on the same reliable data channel as cursor positions.
// Only a complete, ordered generation becomes visible to the presentation layer.
private struct CursorShapeAssembly {
    private var pixels = Data()
    private var generation: UInt64 = 0
    private var width = 0
    private var height = 0
    private var hotspotX = 0
    private var hotspotY = 0
    private var visible = false

    mutating func append(
        event: PlankVisionCursorEvent,
        chunk: ArraySlice<UInt8>
    ) -> PlankRemoteCursorShape? {
        let first = event.flags & UInt32(PLANK_VISION_CURSOR_FIRST_CHUNK) != 0
        let last = event.flags & UInt32(PLANK_VISION_CURSOR_LAST_CHUNK) != 0
        let currentVisibility = event.flags & UInt32(PLANK_VISION_CURSOR_VISIBLE) != 0
        if first {
            guard event.chunk_offset == 0 else { return nil }
            pixels = Data()
            pixels.reserveCapacity(Int(event.image_size))
            generation = event.generation
            width = Int(event.width)
            height = Int(event.height)
            hotspotX = Int(event.hotspot_x)
            hotspotY = Int(event.hotspot_y)
            visible = currentVisibility
        }
        guard !pixels.isEmpty || first,
              generation == event.generation,
              width == event.width,
              height == event.height,
              hotspotX == event.hotspot_x,
              hotspotY == event.hotspot_y,
              visible == currentVisibility,
              pixels.count == event.chunk_offset else {
            pixels = Data()
            return nil
        }
        pixels.append(contentsOf: chunk)
        guard last else { return nil }
        guard pixels.count == event.image_size else {
            pixels = Data()
            return nil
        }
        let shape = PlankRemoteCursorShape(
            pixels: pixels,
            width: width,
            height: height,
            hotspotX: hotspotX,
            hotspotY: hotspotY,
            visible: visible,
            generation: generation
        )
        pixels = Data()
        return shape
    }
}


private final class PlankControlBuffer: @unchecked Sendable {
    private var chunk = [UInt8](repeating: 0, count: Int(PLANK_VISION_CURSOR_MAX_CHUNK_SIZE))
    private var assembly = CursorShapeAssembly()

    func receive(from transport: OpaquePointer,
                 liveBitrate: PlankLiveBitrate?,
                 onCursor: @Sendable (PlankCursorUpdate) -> Void,
                 onRawHid: @Sendable (Data) -> Void) -> PlankControlReceiver.Result {
        // Live bitrate requests leave from this thread, which runs at least
        // every 20 ms and is joined before the endpoint is destroyed.
        if let liveBitrate, !liveBitrate.pump(send: {
            plank_vision_transport_set_video_bitrate(transport, $0) == PLANK_VISION_TRANSPORT_OK
        }) {
            return .failed("The live bitrate request could not be sent" +
                           PlankSessionEngine.failureSuffix(transport))
        }
        var event = PlankVisionCursorEvent()
        var size = 0
        let result = plank_vision_transport_receive_data_event(
            transport, &event, &chunk, chunk.count, &size, 20
        )
        if result == PLANK_VISION_TRANSPORT_TIMEOUT { return .idle }
        if result == PLANK_VISION_TRANSPORT_DATA_IGNORED { return .packet(.ignored) }
        guard result == PLANK_VISION_TRANSPORT_OK else {
            return .failed("The remote control channel stopped unexpectedly" +
                           PlankSessionEngine.failureSuffix(transport))
        }
        switch event.type {
        case UInt16(PLANK_VISION_CURSOR_POSITION):
            onCursor(.position(PlankRemoteCursor(
                x: Int(event.x), y: Int(event.y),
                frameWidth: Int(event.frame_width), frameHeight: Int(event.frame_height),
                sequence: event.sequence
            )))
            return .packet(.position)
        case UInt16(PLANK_VISION_CURSOR_SHAPE):
            if let shape = assembly.append(event: event, chunk: chunk.prefix(size)) {
                onCursor(.shape(shape))
            }
            return .packet(.shape)
        case UInt16(PLANK_VISION_RAW_HID_EVENT):
            onRawHid(Data(chunk.prefix(size)))
            return .packet(.tablet)
        case UInt16(PLANK_VISION_BITRATE_APPLIED):
            liveBitrate?.acknowledge(requestedKbps: Int(event.bitrate_requested_kbps),
                                     appliedKbps: Int(event.bitrate_applied_kbps))
            NSLog("PLANK live bitrate acknowledged: requested=%u applied=%u peak=%u kbps",
                  event.bitrate_requested_kbps, event.bitrate_applied_kbps,
                  event.bitrate_peak_kbps)
            return .packet(.ignored)
        default: return .packet(.ignored)
        }
    }
}

struct PlankSessionEngine: Sendable {
    func stream(
        host: String,
        topology: PlankTopology,
        frameRate: Int,
        encoderTargetKbps: Int,
        launch: PlankLaunchCredentials,
        inputQueue: PlankInputQueue,
        sessionLabel: String = "-",
        relayState: @escaping @Sendable () -> String = { "not applicable" },
        onAudioProgress: @escaping @Sendable (PlankAudioProgress) -> Void = { _ in },
        shouldReportVideoProgress: @escaping @Sendable () -> Bool = { true },
        liveBitrate: PlankLiveBitrate? = nil,
        beforeTransportClose: @escaping @Sendable () -> Void = {},
        onFrame: @escaping @Sendable (PlankRenderedFrame) -> Void,
        onVideoProgress: @escaping @Sendable (PlankVideoProgress) -> Void,
        onCursor: @escaping @Sendable (PlankCursorUpdate) -> Void,
        onHostFeatures: @escaping @Sendable (UInt32) -> Void,
        onRawHid: @escaping @Sendable (Data) -> Void,
        onTabletFrameSent: @escaping @Sendable (Data) -> Void
    ) async throws {
        let sessionStart = DispatchTime.now().uptimeNanoseconds
        let timing = PlankTimingCapture.shared
        timing.beginSession(sessionLabel)
        let controlLifetime = PlankControlReceiverLifetime()
        let worker = Task.detached(priority: .userInitiated) {
            var error = [CChar](repeating: 0, count: 512)
            let transport = host.withCString { hostPointer in
                launch.certificateSHA256.withCString { certificatePointer in
                    launch.transportToken.withCString { tokenPointer in
                        plank_vision_transport_connect(
                            hostPointer,
                            launch.transportPort,
                            certificatePointer,
                            tokenPointer,
                            launch.udpPayloadMTU,
                            &error,
                            error.count
                        )
                    }
                }
            }
            guard let transport else {
                throw PlankSessionError.transport(String(cString: error))
            }
            defer { beforeTransportClose(); plank_vision_transport_disconnect(transport) }

            let request = PlankStreamRequest.negotiation(
                topology: topology,
                frameRate: frameRate,
                encoderTargetKbps: encoderTargetKbps
            )
            NSLog("PLANK session %@ requesting encoder target %d kbps at %d fps",
                  sessionLabel, StreamBitrate.normalized(encoderTargetKbps), frameRate)
            let requestData = try JSONSerialization.data(withJSONObject: request)
            var response = [UInt8](repeating: 0, count: 64 * 1024)
            var responseSize = 0
            let negotiationResult = requestData.withUnsafeBytes { bytes in
                plank_vision_transport_negotiate(
                    transport,
                    bytes.bindMemory(to: UInt8.self).baseAddress,
                    bytes.count,
                    &response,
                    response.count,
                    &responseSize,
                    &error,
                    error.count
                )
            }
            guard negotiationResult == PLANK_VISION_TRANSPORT_OK else {
                throw PlankSessionError.transport(String(cString: error))
            }
            let responseData = Data(response.prefix(responseSize))
            guard let responseObject = try JSONSerialization.jsonObject(with: responseData) as? [String: Any],
                  responseObject["video_format"] as? Int == 0x0800 else {
                throw PlankSessionError.invalidNegotiation
            }
            guard let audioFormat = PlankAudioFormat.negotiated(from: responseObject) else {
                throw PlankSessionError.transport(
                    "The Host returned unsupported native audio values."
                )
            }
            // Declared after the transport's defer, so it runs first: the
            // receive thread has exited before the endpoint is destroyed.
            let audio = try PlankAudioReceiver(
                transport: transport, format: audioFormat,
                sessionLabel: sessionLabel, onProgress: onAudioProgress
            )
            defer { audio.stop() }
            audio.start()
            let hostFeatures = UInt32(truncatingIfNeeded: responseObject["host_feature_flags"] as? Int ?? 0)
            onHostFeatures(hostFeatures)
            liveBitrate?.setSupported(
                PlankLiveBitrateState.hostSupportsChanges(hostFeatures)
            )
            let rawHidAvailable = hostFeatures & PlankHostFeature.tabletRelayRequired ==
                PlankHostFeature.tabletRelayRequired

            let endpoint = PlankTransportHandle(transport)
            // The data lane cannot depend on frame arrival, decoding, or recovery.
            // Its buffers and shape assembler belong solely to this receive thread.
            let controlBuffer = PlankControlBuffer()
            let controls = PlankControlReceiver {
                controlBuffer.receive(from: endpoint.pointer, liveBitrate: liveBitrate,
                                      onCursor: onCursor, onRawHid: onRawHid)
            }
            controlLifetime.install(controls)
            controls.start()
            defer { controls.stop() } // Join before endpoint destruction on every exit.

            guard let decoder = plank_video_decoder_create(&error, error.count) else {
                throw PlankSessionError.transport(String(cString: error))
            }
            defer { plank_video_decoder_destroy(decoder) }
            var hardwareDecoder: PlankHardwareVideoDecoder? = PlankHardwareVideoDecoder()

            let decoderPadding = Int(plank_video_decoder_input_padding())
            var payload = [UInt8](repeating: 0, count: 4 * 1024 * 1024 + decoderPadding)
            var pixels = [UInt8](
                repeating: 0,
                count: topology.desktopWidth * topology.desktopHeight * 4
            )
            var frame = PlankVisionVideoFrame()
            var receivedFrames: UInt64 = 0
            var decodedFrames: UInt64 = 0
            var keyFrames: UInt64 = 0
            var frameGaps: UInt64 = 0
            var lastReceivedFrameNumber: UInt64?
            var decodeNanos: UInt64 = 0
            var decodeCalls: UInt64 = 0
            var lastVideoReport = DispatchTime.now().uptimeNanoseconds
            var lastDecodedFrameTime = lastVideoReport
            var lastFrameTime = UInt64(0)
            var recovery = PlankVideoDecoderRecovery()
            let senderState = PlankInputSenderState()
            var lastTabletFlowLog = sessionStart
            let sender = Task.detached(priority: .userInitiated) {
                for await _ in inputQueue.signals {
                    for event in inputQueue.drain() {
                        let sendStart = DispatchTime.now().uptimeNanoseconds
                        let sent = Self.send(
                            event, to: endpoint.pointer,
                            rawHidAvailable: rawHidAvailable,
                            penAvailable: hostFeatures & PlankHostFeature.normalizedPen != 0
                        ) == PLANK_VISION_TRANSPORT_OK
                        senderState.recordSend(
                            nanos: DispatchTime.now().uptimeNanoseconds - sendStart,
                            succeeded: sent,
                            tabletType: {
                                guard case let .rawHid(frame) = event, frame.count >= 8 else { return nil }
                                return UInt16(frame[6]) | (UInt16(frame[7]) << 8)
                            }()
                        )
                        guard sent else {
                            senderState.markFailed()
                            inputQueue.stop()
                            return
                        }
                        if case let .rawHid(frame) = event {
                            onTabletFrameSent(frame)
                        }
                    }
                }
            }
            do {
                while !Task.isCancelled {
                    if let failure = controls.snapshot.failure {
                        throw PlankSessionError.transport(failure)
                    }
                    if senderState.hasFailed {
                        throw PlankSessionError.transport(
                            "The remote input channel stopped unexpectedly" + Self.failureSuffix(transport)
                        )
                    }
                    var payloadSize = 0
                    let result = plank_vision_transport_receive_video(
                        transport, &frame, &payload,
                        payload.count - decoderPadding, &payloadSize, 30
                    )
                    let reportTime = DispatchTime.now().uptimeNanoseconds
                    if reportTime - lastTabletFlowLog >= 5_000_000_000 {
                        let tablet = senderState.tabletSummary
                        let queued = inputQueue.diagnostics
                        var stats = PlankVisionVideoStats()
                        _ = plank_vision_transport_video_stats(transport, &stats)
                        let age = tablet.lastReport == 0 ? "none" : String(format: "%.0fms", Double(max(reportTime, tablet.lastReport) - tablet.lastReport) / 1_000_000)
                        NSLog("PLANK tablet submission: session=%@ reportsAccepted=%llu messagesAccepted=%llu lastReport=%@ queueDepth=%ld queueAge=%.0fms nativeInputSent=%llu relay=[%@]",
                              sessionLabel, tablet.reports, tablet.messages, age, queued.depth,
                              Double(queued.oldestAgeNanos) / 1_000_000, stats.input_packets_sent, relayState())
                        lastTabletFlowLog = reportTime
                    }
                    timing.roll()
                    if reportTime - lastVideoReport >= 1_000_000_000 {
                        if shouldReportVideoProgress() {
                            var stats = PlankVisionVideoStats()
                            _ = plank_vision_transport_video_stats(transport, &stats)
                            onVideoProgress(PlankVideoProgress(
                                received: receivedFrames,
                                decoded: decodedFrames,
                                keyFrames: keyFrames,
                                receiveDrops: stats.receive_drops,
                                fecUnrecovered: stats.fec_symbols_unrecovered,
                                frameGaps: frameGaps,
                                averageDecodeMilliseconds: decodeCalls == 0 ? 0 :
                                    Double(decodeNanos) / Double(decodeCalls) / 1_000_000,
                                decoder: hardwareDecoder == nil ? "FFmpeg software" : "VideoToolbox xf44"
                            ))
                        }
                        decodeNanos = 0
                        decodeCalls = 0
                        lastVideoReport = reportTime
                    }

                    if result == PLANK_VISION_TRANSPORT_BUFFER_TOO_SMALL,
                       payloadSize > payload.count - decoderPadding,
                       payloadSize <= 64 * 1024 * 1024 {
                        payload = [UInt8](repeating: 0, count: payloadSize + decoderPadding)
                        continue
                    }
                    if result == PLANK_VISION_TRANSPORT_TIMEOUT { continue }
                    if result == PLANK_VISION_TRANSPORT_BUFFER_TOO_SMALL {
                        throw PlankSessionError.transport(
                            "The native video receiver stopped unexpectedly: " +
                            "buffer limit (frame of \(payloadSize) bytes)."
                        )
                    }
                    guard result == PLANK_VISION_TRANSPORT_OK, payloadSize > 0 else {
                        throw PlankSessionError.transport(
                            "The native video receiver stopped unexpectedly" + Self.failureSuffix(transport)
                        )
                    }
                    receivedFrames &+= 1
                    lastFrameTime = reportTime
                    timing.arrival(size: payloadSize)
                    if frame.flags & 1 != 0 { keyFrames &+= 1 }
                    if let lastReceivedFrameNumber,
                       frame.frame_number > lastReceivedFrameNumber + 1 {
                        frameGaps &+= frame.frame_number - lastReceivedFrameNumber - 1
                    }
                    lastReceivedFrameNumber = frame.frame_number

                    // The independent control receiver keeps running throughout
                    // keyframe recovery, frame waits, and synchronous decoding.
                    if !recovery.admit(isKeyFrame: frame.flags & 1 != 0) {
                        if recovery.keyFrameRequestDue(now: reportTime) {
                            guard plank_vision_transport_request_idr(transport) ==
                                    PLANK_VISION_TRANSPORT_OK else {
                                throw PlankSessionError.transport("Unable to request a fresh video frame.")
                            }
                        }
                        continue
                    }

                    // libavcodec may read beyond the compressed packet size for
                    // vectorized bitstream parsing. The transport buffer is reused,
                    // so clear the trailing bytes after every received frame.
                    payload.replaceSubrange(
                        payloadSize..<(payloadSize + decoderPadding),
                        with: repeatElement(0, count: decoderPadding)
                    )

                    if let hardware = hardwareDecoder {
                        let decodeStart = DispatchTime.now().uptimeNanoseconds
                        let outcome = payload.withUnsafeBufferPointer { bytes in
                            hardware.decode(UnsafeBufferPointer(rebasing: bytes.prefix(payloadSize)))
                        }
                        let hardwareDecodeNanos = DispatchTime.now().uptimeNanoseconds - decodeStart
                        decodeNanos &+= hardwareDecodeNanos
                        decodeCalls &+= 1
                        switch outcome {
                        case let .frame(image):
                            decodedFrames &+= 1
                            timing.decoded(nanos: hardwareDecodeNanos)
                            lastDecodedFrameTime = reportTime
                            Self.logResumption(recovery.frameDecoded(now: reportTime))
                            onFrame(PlankRenderedFrame(
                                pixels: Data(), pixelBuffer: image,
                                width: CVPixelBufferGetWidth(image),
                                height: CVPixelBufferGetHeight(image),
                                bytesPerRow: 0, frameNumber: frame.frame_number
                            ))
                            continue
                        case .waiting:
                            if reportTime - lastDecodedFrameTime >= 1_000_000_000 &&
                               recovery.keyFrameRequestDue(now: reportTime) {
                                guard plank_vision_transport_request_idr(transport) ==
                                        PLANK_VISION_TRANSPORT_OK else {
                                    throw PlankSessionError.transport("Unable to request a fresh video frame.")
                                }
                            }
                            continue
                        case let .badData(decodeStatus, frameStatus):
                            // Damaged input, not a decoder fault: rebuild VideoToolbox
                            // and resync on a keyframe without using the FFmpeg budget.
                            _ = recovery.badData(now: reportTime)
                            NSLog("PLANK VideoToolbox recovery: reason=%@ status=%d/%d rejection=%ld; rebuilt the hardware decoder, discarding frames until a keyframe",
                                  PlankVideoDecoderRecovery.Reason.badData.rawValue,
                                  decodeStatus, frameStatus, recovery.badDataRejections)
                            hardwareDecoder = PlankHardwareVideoDecoder()
                            if recovery.keyFrameRequestDue(now: reportTime) {
                                guard plank_vision_transport_request_idr(transport) ==
                                        PLANK_VISION_TRANSPORT_OK else {
                                    throw PlankSessionError.transport("Unable to request a fresh video frame.")
                                }
                            }
                            continue
                        case .unavailable:
                            let action = recovery.hardwareFailed(now: reportTime)
                            NSLog("PLANK VideoToolbox decoder failed: %@; failure %ld, %@",
                                  hardware.lastError ?? "unknown error", recovery.hardwareFailures,
                                  action == .selectSoftware ?
                                      "FFmpeg selected for the rest of the session" :
                                      "rebuilt the hardware decoder")
                            hardwareDecoder = action == .rebuildHardware ?
                                PlankHardwareVideoDecoder() : nil
                            guard plank_vision_transport_request_idr(transport) ==
                                    PLANK_VISION_TRANSPORT_OK else {
                                throw PlankSessionError.transport("Unable to request a fresh video frame.")
                            }
                            recovery.recordKeyFrameRequest(now: reportTime)
                            continue
                        }
                    }

                    var decodedWidth: UInt32 = 0
                    var decodedHeight: UInt32 = 0
                    var decodedStride: UInt32 = 0
                    let decodeStart = DispatchTime.now().uptimeNanoseconds
                    let decodeResult = payload.withUnsafeBytes { encodedBytes in
                        pixels.withUnsafeMutableBytes { pixelBytes in
                            plank_video_decoder_decode(
                                decoder,
                                encodedBytes.bindMemory(to: UInt8.self).baseAddress,
                                payloadSize,
                                pixelBytes.bindMemory(to: UInt8.self).baseAddress,
                                pixelBytes.count,
                                &decodedWidth,
                                &decodedHeight,
                                &decodedStride,
                                &error,
                                error.count
                            )
                        }
                    }
                    let softwareDecodeNanos = DispatchTime.now().uptimeNanoseconds - decodeStart
                    decodeNanos &+= softwareDecodeNanos
                    decodeCalls &+= 1
                    if decodeResult == PLANK_VIDEO_DECODER_NO_FRAME {
                        if reportTime - lastDecodedFrameTime >= 1_000_000_000 &&
                           recovery.keyFrameRequestDue(now: reportTime) {
                            guard plank_vision_transport_request_idr(transport) ==
                                    PLANK_VISION_TRANSPORT_OK else {
                                throw PlankSessionError.transport("Unable to request a fresh video frame.")
                            }
                        }
                        continue
                    }
                    guard decodeResult == PLANK_VIDEO_DECODER_FRAME else {
                        throw PlankSessionError.transport(
                            "The video decoder failed: " + String(cString: error)
                        )
                    }
                    decodedFrames &+= 1
                    timing.decoded(nanos: softwareDecodeNanos)
                    lastDecodedFrameTime = reportTime
                    Self.logResumption(recovery.frameDecoded(now: reportTime))
                    let byteCount = Int(decodedStride) * Int(decodedHeight)
                    onFrame(PlankRenderedFrame(
                        pixels: Data(pixels.prefix(byteCount)),
                        pixelBuffer: nil,
                        width: Int(decodedWidth),
                        height: Int(decodedHeight),
                        bytesPerRow: Int(decodedStride),
                        frameNumber: frame.frame_number
                    ))
                }
            } catch {
                if !Task.isCancelled {
                    let now = DispatchTime.now().uptimeNanoseconds
                    var stats = PlankVisionVideoStats()
                    _ = plank_vision_transport_video_stats(transport, &stats)
                    NSLog("%@", Self.failureSnapshot(
                        session: sessionLabel,
                        elapsedNanos: now - sessionStart,
                        lastFrameNumber: lastReceivedFrameNumber,
                        lastFrameAgeNanos: lastFrameTime == 0 ? nil : now - lastFrameTime,
                        received: receivedFrames,
                        decoded: decodedFrames,
                        decoder: Self.decoderSummary(recovery),
                        controls: controls.snapshot.summary,
                        queue: inputQueue.diagnostics,
                        sender: senderState.sendSummary,
                        stats: stats,
                        transport: transport,
                        relay: relayState(),
                        error: error
                    ))
                }
                controls.stop()
                inputQueue.stop()
                await sender.value
                throw error
            }
            NSLog("PLANK video summary: session=%@ elapsed=%.1fs decoder=[%@] received=%llu decoded=%llu",
                  sessionLabel,
                  Double(DispatchTime.now().uptimeNanoseconds - sessionStart) / 1_000_000_000,
                  Self.decoderSummary(recovery), receivedFrames, decodedFrames)
            NSLog("PLANK control summary: session=%@ %@", sessionLabel, controls.snapshot.summary)
            controls.stop()
            inputQueue.stop()
            await sender.value
        }
        try await withTaskCancellationHandler {
            try await worker.value
        } onCancel: {
            controlLifetime.requestStop()
            inputQueue.stop()
            worker.cancel()
        }
    }

    private static func failureKindName(_ kind: UInt32) -> String {
        switch Int(kind) {
        case Int(PLANK_VISION_FAILURE_TERMINATED): return "transport terminated"
        case Int(PLANK_VISION_FAILURE_CANCELLED): return "cancelled"
        case Int(PLANK_VISION_FAILURE_INVALID_PAYLOAD): return "invalid or empty payload"
        case Int(PLANK_VISION_FAILURE_BUFFER_LIMIT): return "buffer limit"
        case Int(PLANK_VISION_FAILURE_QUEUE_FULL): return "native input queue full"
        case Int(PLANK_VISION_FAILURE_INTERNAL): return "internal error"
        default: return "none recorded"
        }
    }

    private static func laneName(_ lane: UInt32) -> String {
        switch Int(lane) {
        case Int(PLANK_VISION_LANE_VIDEO): return "video"
        case Int(PLANK_VISION_LANE_INPUT): return "input"
        case Int(PLANK_VISION_LANE_DATA): return "data"
        case Int(PLANK_VISION_LANE_AUDIO): return "audio"
        default: return "-"
        }
    }

    /// The first terminal transport failure and the endpoint's close reason.
    private static func firstFailure(_ transport: OpaquePointer) -> String {
        var failure = PlankVisionTransportFailure()
        var reason = [CChar](repeating: 0, count: 256)
        _ = plank_vision_transport_first_failure(transport, &failure, &reason, reason.count)
        let text = String(cString: reason)
        var summary = failureKindName(failure.kind)
        if failure.kind != UInt32(PLANK_VISION_FAILURE_NONE) {
            summary += " lane=\(laneName(failure.lane)) native=\(failure.native_result)" +
                " state=\(failure.endpoint_state)"
        }
        if !text.isEmpty { summary += " reason=\"\(text)\"" }
        return summary
    }

    fileprivate static func failureSuffix(_ transport: OpaquePointer) -> String {
        ": " + firstFailure(transport) + "."
    }

    private static func decoderSummary(_ recovery: PlankVideoDecoderRecovery) -> String {
        "active=\(recovery.usesHardware ? "VideoToolbox" : "FFmpeg")" +
            " badData=\(recovery.badDataRejections) resumed=\(recovery.resumptions)" +
            " hardwareFailures=\(recovery.hardwareFailures)" +
            " awaitingKeyFrame=\(recovery.awaitingKeyFrame)"
    }

    private static func logResumption(_ resumption: PlankVideoDecoderRecovery.Resumption?) {
        guard let resumption else { return }
        NSLog("PLANK video resumed: decoder=%@ reason=%@ waited=%.0fms discarded=%ld keyframeRequests=%ld",
              resumption.hardware ? "VideoToolbox" : "FFmpeg", resumption.reason.rawValue,
              Double(resumption.waitedNanos) / 1_000_000,
              resumption.discardedFrames, resumption.keyFrameRequests)
    }

    private static func failureSnapshot(
        session: String,
        elapsedNanos: UInt64,
        lastFrameNumber: UInt64?,
        lastFrameAgeNanos: UInt64?,
        received: UInt64,
        decoded: UInt64,
        decoder: String,
        controls: String,
        queue: PlankInputQueueDiagnostics,
        sender: (accepted: UInt64, maxNanos: UInt64, failedNanos: UInt64),
        stats: PlankVisionVideoStats,
        transport: OpaquePointer,
        relay: String,
        error: Error
    ) -> String {
        func ms(_ nanos: UInt64) -> String { String(format: "%.1fms", Double(nanos) / 1_000_000) }
        let frame = lastFrameNumber.map { "#\($0)" } ?? "none"
        let frameAge = lastFrameAgeNanos.map(ms) ?? "-"
        // Accepted counts Client submissions; a two-axis scroll is two native packets.
        let backlog = Int64(bitPattern: sender.accepted &- stats.input_packets_sent)
        return "PLANK session failure: session=\(session)" +
            String(format: " elapsed=%.1fs", Double(elapsedNanos) / 1_000_000_000) +
            " first=[\(firstFailure(transport))]" +
            " video=[last=\(frame) age=\(frameAge) received=\(received) decoded=\(decoded)]" +
            " decoder=[\(decoder)]" +
            " controls=[\(controls)]" +
            " inputQueue=[depth=\(queue.depth) oldest=\(ms(queue.oldestAgeNanos))" +
            " highWater=\(queue.highWaterDepth) maxDrainAge=\(ms(queue.maxDrainAgeNanos))]" +
            " send=[accepted=\(sender.accepted) nativeSent=\(stats.input_packets_sent)" +
            " nativeBacklog=\(backlog) maxSubmit=\(ms(sender.maxNanos))" +
            " failedSubmit=\(sender.failedNanos == 0 ? "none" : ms(sender.failedNanos))]" +
            " quic=[rtt=\(stats.quic_rtt_us)us lost=\(stats.quic_packets_lost)" +
            " kyprotoDrops=\(stats.kyproto_packets_dropped)]" +
            " relay=[\(relay)]" +
            " error=\"\(error.localizedDescription)\""
    }

    private static func send(
        _ input: PlankInputEvent,
        to transport: OpaquePointer,
        rawHidAvailable: Bool,
        penAvailable: Bool
    ) -> Int32 {
        switch input {
        case let .pointer(x, y, maximumX, maximumY):
            return plank_vision_transport_send_mouse_position(
                transport, x, y, maximumX, maximumY
            )
        case let .button(number, pressed):
            return plank_vision_transport_send_mouse_button(
                transport, number, pressed ? 1 : 0
            )
        case let .scroll(vertical, horizontal):
            let verticalResult = vertical == 0 ? Int32(PLANK_VISION_TRANSPORT_OK) :
                plank_vision_transport_send_scroll(transport, vertical, 0)
            let horizontalResult = horizontal == 0 ? Int32(PLANK_VISION_TRANSPORT_OK) :
                plank_vision_transport_send_scroll(transport, horizontal, 1)
            return verticalResult == PLANK_VISION_TRANSPORT_OK ? horizontalResult : verticalResult
        case let .key(code, pressed, modifiers):
            return plank_vision_transport_send_key(
                transport, code, pressed ? 1 : 0, modifiers
            )
        case let .text(data):
            return data.withUnsafeBytes { bytes in
                plank_vision_transport_send_utf8(
                    transport,
                    bytes.bindMemory(to: UInt8.self).baseAddress,
                    bytes.count
                )
            }
        case let .pen(pen):
            guard penAvailable else { return Int32(PLANK_VISION_TRANSPORT_ERROR) }
            return plank_vision_transport_send_pen(
                transport, pen.phase.rawValue, pen.x, pen.y,
                pen.pressureOrDistance, pen.tilt, pen.rotation
            )
        case let .rawHid(frame):
            guard rawHidAvailable else { return Int32(PLANK_VISION_TRANSPORT_ERROR) }
            return frame.withUnsafeBytes { bytes in
                plank_vision_transport_send_raw_hid(
                    transport,
                    bytes.bindMemory(to: UInt8.self).baseAddress,
                    bytes.count
                )
            }
        }
    }
}
