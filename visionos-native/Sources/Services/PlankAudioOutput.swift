import AVFAudio
import Foundation

/// Owns one session's playback queue. The render block retains it, so the
/// queue outlives any callback that can still read from it.
final class PlankAudioRingHandle: @unchecked Sendable {
    let pointer: OpaquePointer

    init?() {
        var config = plank_audio_ring_default_config()
        guard let pointer = plank_audio_ring_create(&config) else { return nil }
        self.pointer = pointer
    }

    deinit { plank_audio_ring_destroy(pointer) }

    var stats: PlankAudioRingStats {
        var stats = PlankAudioRingStats()
        plank_audio_ring_stats(pointer, &stats)
        return stats
    }
}

struct PlankAudioOutputStatus: Sendable {
    let running: Bool
    let outputLatencySeconds: Double
    let ioBufferSeconds: Double
    let error: String?
    /// Every gain stage after decoding, for level investigations: the route,
    /// its channel count, the system output volume and PLANK's mixer gain.
    let route: String
    let outputChannels: Int
    let systemVolume: Float
    let mixerVolume: Float
}

/// Native headset playback for Host audio. Every engine and session change
/// runs on one serial queue, never on the main actor. Playback starts only for
/// an active session in a foreground scene, and any interruption, route or
/// configuration change, or media-services reset discards queued audio before
/// playing again so nothing stale is heard.
final class PlankAudioOutput: @unchecked Sendable {
    static let shared = PlankAudioOutput()

    private let queue = DispatchQueue(label: "la.instinctual.plank.audio-output",
                                      qos: .userInitiated)
    private let format = AVAudioFormat(standardFormatWithSampleRate: 48_000, channels: 2)!

    // Queue-confined state.
    private var engine: AVAudioEngine?
    private var source: AVAudioSourceNode?
    private var configurationObserver: NSObjectProtocol?
    private var ring: PlankAudioRingHandle?
    private var sceneActive = true
    private var interrupted = false
    private var volume = PlankAudioPreferences.volume()
    private var muted = PlankAudioPreferences.muted()
    private var outputLatency: Double = 0
    private var ioBufferDuration: Double = 0
    private var route = "-"
    private var lastError: String?

    private init() {
        let center = NotificationCenter.default
        let session = AVAudioSession.sharedInstance()
        _ = center.addObserver(forName: AVAudioSession.interruptionNotification,
                               object: session, queue: nil) { [weak self] note in
            let raw = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt
            let began = raw.flatMap(AVAudioSession.InterruptionType.init) == .began
            guard let self else { return }
            self.queue.async { self.handleInterruption(began: began) }
        }
        _ = center.addObserver(forName: AVAudioSession.routeChangeNotification,
                               object: session, queue: nil) { [weak self] _ in
            guard let self else { return }
            self.queue.async { self.restart(reason: "route change") }
        }
        _ = center.addObserver(forName: AVAudioSession.mediaServicesWereResetNotification,
                               object: session, queue: nil) { [weak self] _ in
            guard let self else { return }
            self.queue.async { self.rebuild(reason: "media services reset") }
        }
    }

    /// Starts playback of a new session's queue, replacing any previous one.
    func begin(_ ring: PlankAudioRingHandle) {
        queue.sync {
            if self.ring != nil { tearDownEngine(deactivate: false) }
            self.ring = ring
            interrupted = false
            startIfPossible()
        }
    }

    /// Stops and discards playback for this session. A late call for an older
    /// session never stops a newer one.
    func end(_ ring: PlankAudioRingHandle) {
        queue.sync {
            guard self.ring === ring else { return }
            plank_audio_ring_request_flush(ring.pointer)
            tearDownEngine(deactivate: true)
            self.ring = nil
            lastError = nil
        }
    }

    /// Without the background audio mode playback cannot continue in the
    /// background; stop there and resume with a fresh queue on return.
    func setSceneActive(_ active: Bool) {
        queue.async { [self] in
            guard sceneActive != active else { return }
            sceneActive = active
            if active {
                restart(reason: "scene active")
            } else if let ring {
                plank_audio_ring_request_flush(ring.pointer)
                engine?.stop()
            }
        }
    }

    func setVolume(_ volume: Float, muted: Bool) {
        queue.async { [self] in
            self.volume = min(max(volume, 0), 1)
            self.muted = muted
            applyVolume()
        }
    }

    var status: PlankAudioOutputStatus {
        queue.sync {
            PlankAudioOutputStatus(
                running: engine?.isRunning ?? false,
                outputLatencySeconds: outputLatency,
                ioBufferSeconds: ioBufferDuration,
                error: lastError,
                route: route,
                outputChannels: Int(engine?.outputNode.outputFormat(forBus: 0).channelCount ?? 0),
                systemVolume: AVAudioSession.sharedInstance().outputVolume,
                mixerVolume: engine?.mainMixerNode.outputVolume ?? 0
            )
        }
    }

    // MARK: - Queue-confined

    private func startIfPossible() {
        guard let ring, sceneActive, !interrupted else { return }
        do {
            let session = AVAudioSession.sharedInstance()
            // Mix rather than interrupt other apps: a desktop stream is often
            // silent and should not stop the user's own audio.
            try session.setCategory(.playback, mode: .default, options: [.mixWithOthers])
            // Plain stereo: Host left/right stay fixed to the ears instead of
            // being rendered from the window's position.
#if os(visionOS)
            try? session.setIntendedSpatialExperience(.bypassed)
#endif
            try? session.setPreferredSampleRate(48_000)
            try? session.setPreferredIOBufferDuration(0.005)
            try session.setActive(true)
            if engine == nil { buildEngine(ring) }
            guard let engine else { return }
            applyVolume()
            if !engine.isRunning {
                engine.prepare()
                try engine.start()
            }
            outputLatency = session.outputLatency
            ioBufferDuration = session.ioBufferDuration
            route = session.currentRoute.outputs
                .map { "\($0.portType.rawValue)×\($0.channels?.count ?? 0)" }
                .joined(separator: "+")
            NSLog("PLANK audio output: route=%@ channels=%d sampleRate=%.0f systemVolume=%.2f mixer=%.2f",
                  route, Int(engine.outputNode.outputFormat(forBus: 0).channelCount),
                  session.sampleRate, session.outputVolume, engine.mainMixerNode.outputVolume)
            lastError = nil
        } catch {
            lastError = error.localizedDescription
            NSLog("PLANK audio output could not start: %@", error.localizedDescription)
        }
    }

    private func buildEngine(_ ring: PlankAudioRingHandle) {
        let engine = AVAudioEngine()
        let source = AVAudioSourceNode(format: format) { isSilence, _, frameCount, bufferList in
            let buffers = UnsafeMutableAudioBufferListPointer(bufferList)
            guard buffers.count >= 2,
                  let left = buffers[0].mData?.assumingMemoryBound(to: Float.self),
                  let right = buffers[1].mData?.assumingMemoryBound(to: Float.self) else {
                return kAudioUnitErr_InvalidParameter
            }
            if plank_audio_ring_read(ring.pointer, left, right, frameCount) == 0 {
                isSilence.pointee = true
            }
            return noErr
        }
        engine.attach(source)
        engine.connect(source, to: engine.mainMixerNode, format: format)
        configurationObserver = NotificationCenter.default.addObserver(
            forName: .AVAudioEngineConfigurationChange, object: engine, queue: nil
        ) { [weak self] _ in
            guard let self else { return }
            self.queue.async { self.restart(reason: "engine configuration change") }
        }
        self.engine = engine
        self.source = source
    }

    private func tearDownEngine(deactivate: Bool) {
        if let configurationObserver {
            NotificationCenter.default.removeObserver(configurationObserver)
        }
        configurationObserver = nil
        engine?.stop()
        if let engine, let source { engine.detach(source) }
        engine = nil
        source = nil
        if deactivate {
            try? AVAudioSession.sharedInstance().setActive(
                false, options: .notifyOthersOnDeactivation
            )
        }
    }

    private func applyVolume() {
        engine?.mainMixerNode.outputVolume = muted ? 0 : volume
    }

    private func handleInterruption(began: Bool) {
        guard let ring else { return }
        plank_audio_ring_request_flush(ring.pointer)
        interrupted = began
        if began {
            engine?.stop()
            NSLog("PLANK audio interrupted")
        } else {
            NSLog("PLANK audio interruption ended; resuming with a fresh queue")
            startIfPossible()
        }
    }

    private func restart(reason: String) {
        guard let ring else { return }
        plank_audio_ring_request_flush(ring.pointer)
        if let engine, !engine.isRunning, let source {
            // A configuration change can leave the graph disconnected.
            engine.connect(source, to: engine.mainMixerNode, format: format)
        }
        NSLog("PLANK audio restarting after %@", reason)
        startIfPossible()
    }

    private func rebuild(reason: String) {
        guard let ring else { return }
        plank_audio_ring_request_flush(ring.pointer)
        tearDownEngine(deactivate: false)
        NSLog("PLANK audio rebuilding after %@", reason)
        startIfPossible()
    }
}
