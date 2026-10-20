#include "pipewire.h"
#include "pipewirebuffer.h"
#include "streaming/avsynccontroller.h"

#include <SDL3/SDL.h> // shared video-clock epoch and product log only; no SDL audio
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-utils.h>
#include <chrono>
#include <mutex>
#include <thread>
extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

static_assert(PW_CHECK_VERSION(1, 1, 0), "Native playback requires PipeWire 1.1 or newer");

struct PipeWireAudioRenderer::Impl {
    explicit Impl(bool correction, bool epoch) : correctionEnabled(correction), commonEpoch(epoch) {}
    pw_thread_loop* loop = nullptr;
    pw_stream* stream = nullptr;
    bool loopStarted = false;
    std::atomic<int> state {PW_STREAM_STATE_UNCONNECTED};
    std::atomic<bool> formatted {false}, failed {false};
    int rate = 0, channels = 0, samplesPerFrame = 0;
    bool correctionEnabled, commonEpoch;
    std::unique_ptr<PlankPipeWire::PcmRing> ring;
    std::unique_ptr<PlankPipeWire::Playback> playback;
    PlankPipeWire::ClockSnapshot snapshot;
    std::atomic<int> minimumHeadroom {INT32_MAX};
    std::vector<float> input, corrected;
    SwrContext* swr = nullptr;
    PlankAvSync::AudioTimestampObserver timestamp;
    PlankAvSync::AudioPhaseController phase;
    PlankAvSync::AudioRateController relativeRate;
    PlankPipeWire::HeadroomGuard guard;
    std::uint64_t rawFrames = 0, submittedFrames = 0;
    std::uint64_t lastCompensationMs = 0, lastLogMs = 0;
    int appliedPpm = 0, requestedPpm = 0;
    qint64 lastMediaMs = -1;

    static void stateChanged(void* data, pw_stream_state old, pw_stream_state current, const char* error) {
        auto* self = static_cast<Impl*>(data);
        self->state.store(current);
        if (current == PW_STREAM_STATE_ERROR ||
            (current == PW_STREAM_STATE_UNCONNECTED && old != PW_STREAM_STATE_UNCONNECTED)) {
            self->failed.store(true);
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "PLANK PipeWire stream failed: %s", error ? error : "disconnected");
        }
    }
    static void paramChanged(void* data, std::uint32_t id, const spa_pod* param) {
        if (id != SPA_PARAM_Format) return;
        auto* self = static_cast<Impl*>(data);
        spa_audio_info_raw info {};
        const bool valid = param && spa_format_audio_raw_parse(param, &info) >= 0 &&
            info.format == SPA_AUDIO_FORMAT_F32 && info.rate == unsigned(self->rate) &&
            info.channels == unsigned(self->channels);
        self->formatted.store(valid);
        if (param && !valid) self->failed.store(true);
    }
    static void process(void* data) {
        auto* self = static_cast<Impl*>(data);
        pw_buffer* buffer = pw_stream_dequeue_buffer(self->stream);
        if (!buffer) return;
        auto* raw = buffer->buffer;
        const unsigned stride = self->channels * sizeof(float);
        if (!raw || raw->n_datas != 1 || !raw->datas[0].data || !raw->datas[0].chunk ||
            raw->datas[0].maxsize < stride ||
            reinterpret_cast<std::uintptr_t>(raw->datas[0].data) % alignof(float)) {
            self->failed.store(true);
            pw_stream_return_buffer(self->stream, buffer);
            return;
        }
        auto& plane = raw->datas[0];
        const unsigned capacity = plane.maxsize / stride;
        const unsigned frames = buffer->requested ?
            static_cast<unsigned>(std::min<std::uint64_t>(buffer->requested, capacity)) : capacity;
        // Reject pathological negotiated buffers; the queue and callback must
        // remain bounded even when the server/device fails.
        if (!frames || frames > unsigned(self->rate / 10) ||
            (buffer->requested && buffer->requested > capacity)) {
            self->failed.store(true);
            pw_stream_return_buffer(self->stream, buffer);
            return;
        }
        auto* output = static_cast<float*>(plane.data);
        if (self->formatted.load() && !self->failed.load()) self->playback->render(output, frames);
        else std::fill_n(output, frames * self->channels, 0.0f);
        plane.chunk->offset = 0;
        plane.chunk->size = frames * stride;
        plane.chunk->stride = stride;
        plane.chunk->flags = 0;
        buffer->size = frames; // units used by pw_time.queued
        if (pw_stream_queue_buffer(self->stream, buffer) < 0) self->failed.store(true);

        auto& clock = self->playback->clock();
        clock.observedNs = pw_stream_get_nsec(self->stream);
        pw_time time {};
        clock.tailNs = pw_stream_get_time_n(self->stream, &time, sizeof(time)) < 0 ? 0 :
            PlankPipeWire::presentationTailNs(time.now, time.delay, time.rate.num, time.rate.denom,
                time.queued, time.buffered, self->rate, clock.observedNs);
        self->snapshot.publish(clock);
        // One producer exchange per input block; no blocking or mutex on RT.
        const int reserve = clock.priming ? 0 : static_cast<int>(clock.headroom);
        int previous = self->minimumHeadroom.load(std::memory_order_relaxed);
        while (reserve < previous && !self->minimumHeadroom.compare_exchange_weak(previous, reserve)) {}
    }
    ~Impl() {
        // Stop the ordinary loop, then disconnect/destroy synchronizes the
        // stream's realtime data loop before any callback state is freed.
        if (loopStarted) pw_thread_loop_stop(loop);
        if (stream) pw_stream_destroy(stream);
        if (loop) pw_thread_loop_destroy(loop);
        swr_free(&swr);
    }
};

PipeWireAudioRenderer::PipeWireAudioRenderer(bool correction, bool epoch)
    : m_Impl(new Impl(correction, epoch)) {}
PipeWireAudioRenderer::~PipeWireAudioRenderer() = default;

bool PipeWireAudioRenderer::prepareForPlayback(const OPUS_MULTISTREAM_CONFIGURATION* config)
{
    auto& p = *m_Impl;
    if (p.loop || !config || config->sampleRate != 48000 || config->samplesPerFrame <= 0 ||
        config->samplesPerFrame > 5760 ||
        (config->channelCount != 2 && config->channelCount != 6 && config->channelCount != 8)) return false;
    p.rate = config->sampleRate;
    p.channels = config->channelCount;
    p.samplesPerFrame = config->samplesPerFrame;
    p.input.resize(p.samplesPerFrame * p.channels);
    // A hard capacity, not a latency target. Producer backpressure starts at
    // 50ms; startup/recovery requires one graph pull plus 12ms of reserve.
    p.ring.reset(new PlankPipeWire::PcmRing(p.rate / 5, p.channels));
    p.playback.reset(new PlankPipeWire::Playback(*p.ring, p.rate, p.channels));
    if (p.correctionEnabled) {
        AVChannelLayout layout;
        av_channel_layout_default(&layout, p.channels);
        const int result = swr_alloc_set_opts2(&p.swr, &layout, AV_SAMPLE_FMT_FLT, p.rate,
            &layout, AV_SAMPLE_FMT_FLT, p.rate, 0, nullptr);
        av_channel_layout_uninit(&layout);
        if (result < 0 || !p.swr || av_opt_set_int(p.swr, "flags", SWR_FLAG_RESAMPLE, 0) < 0 ||
            swr_init(p.swr) < 0) return false;
    }
    static std::once_flag initialized;
    std::call_once(initialized, [] { pw_init(nullptr, nullptr); });
    p.loop = pw_thread_loop_new("PLANK Qt playback", nullptr);
    if (!p.loop) return false;
    static const pw_stream_events events = [] {
        pw_stream_events result {};
        result.version = PW_VERSION_STREAM_EVENTS;
        result.state_changed = Impl::stateChanged;
        result.param_changed = Impl::paramChanged;
        result.process = Impl::process;
        return result;
    }();
    p.stream = pw_stream_new_simple(pw_thread_loop_get_loop(p.loop), "PLANK Client Qt audio",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Movie", PW_KEY_APP_NAME, "PLANK Client Qt",
            PW_KEY_NODE_NAME, "plank-client-qt-playback", PW_KEY_NODE_LATENCY, "480/48000", nullptr),
        &events, &p);
    if (!p.stream) return false;
    std::uint8_t storage[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    spa_audio_info_raw info {};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = p.rate;
    info.channels = p.channels;
    const std::array<std::uint32_t, 8> positions {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR,
        SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE, SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR,
        SPA_AUDIO_CHANNEL_SL, SPA_AUDIO_CHANNEL_SR};
    std::copy_n(positions.begin(), p.channels, info.position);
    const spa_pod* params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};
    // Default-device selection, volume and routing belong to PipeWire/session
    // policy. Never force HDMI, bypass the mixer, or change system settings.
    if (pw_stream_connect(p.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
            pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
            params, 1) < 0 || pw_thread_loop_start(p.loop) < 0) return false;
    p.loopStarted = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (!p.failed.load() && std::chrono::steady_clock::now() < deadline) {
        if (p.state.load() == PW_STREAM_STATE_STREAMING && p.formatted.load()) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "PLANK Qt native PipeWire playback: rate=%d channels=%d source_block=%d mode=%s reserve_us=%d",
                p.rate, p.channels, p.samplesPerFrame, p.commonEpoch ? "source-phase" : "relative-rate",
                PlankPipeWire::ReserveUs);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "PLANK PipeWire playback did not become ready (state=%d)", p.state.load());
    return false;
}

void* PipeWireAudioRenderer::getAudioBuffer(int* size)
{
    const int capacity = static_cast<int>(m_Impl->input.size() * sizeof(float));
    if (size) *size = std::min(std::max(*size, 0), capacity);
    return m_Impl->input.data();
}

bool PipeWireAudioRenderer::submitAudio(int bytes, qint64 sourceUs)
{
    auto& p = *m_Impl;
    if (p.failed.load()) return false;
    if (!bytes) return true;
    if (bytes < 0 || bytes % (p.channels * sizeof(float)) ||
        std::size_t(bytes) > p.input.size() * sizeof(float)) return false;
    const int frames = bytes / (p.channels * sizeof(float));
    for (int n = 0; n < frames * p.channels; ++n) if (!std::isfinite(p.input[n])) return false;
    // Bounded wait on the decoder worker only. No realtime callback, GUI or
    // transport lock is held while waiting for the device to consume samples.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (!p.failed.load() && p.ring->queued() > unsigned(p.rate / 20) &&
        std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (p.failed.load() || p.ring->queued() > unsigned(p.rate / 20)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "PLANK PipeWire playback stalled; recreating output");
        return false;
    }

    const auto clock = p.snapshot.get();
    const auto nowNs = pw_stream_get_nsec(p.stream);
    const auto nowMs = SDL_GetTicks(); // same epoch as existing video swap feedback
    const auto delayUs = PlankPipeWire::queueDelayUs(clock, p.ring->written(), nowNs, p.rate);
    const auto video = PlankAvSync::readVideoClock();
    const auto timing = p.timestamp.observe(sourceUs, frames, p.rate, static_cast<std::uint32_t>(nowMs),
        delayUs < 0 ? -1 : static_cast<int>(delayUs / 1000), 0,
        p.swr ? swr_get_delay(p.swr, 1000000) : 0, video, p.commonEpoch);
    const int minimumFrames = p.minimumHeadroom.exchange(INT32_MAX);
    const int minimumUs = minimumFrames == INT32_MAX ? -1 : minimumFrames * 1000000LL / p.rate;
    const bool healthy = delayUs >= 0 && !clock.priming;
    if (p.swr) {
        if (p.commonEpoch) {
            PlankAvSync::AudioPlaybackObserver::Observation demand;
            demand.pulls = clock.pulls;
            demand.shortageRequests = clock.underruns;
            demand.requestUs = clock.quantum * 1000000LL / p.rate;
            demand.headroomUs = clock.headroom * 1000000LL / p.rate;
            demand.ticks = static_cast<std::uint32_t>(nowMs -
                (nowNs >= clock.observedNs ? (nowNs - clock.observedNs) / 1000000 : 1000));
            p.requestedPpm = p.phase.update(timing, getQueuedAudioDurationMs(), nowMs, demand,
                frames * 1000000LL / p.rate).correctionPpm;
        }
        else p.requestedPpm = p.relativeRate.update(p.rawFrames, p.submittedFrames, p.rate, nowMs, video).correctionPpm;
        const int correction = p.guard.apply(p.requestedPpm, minimumUs, nowMs, healthy);
        if (correction != p.appliedPpm || nowMs - p.lastCompensationMs >= 100) {
            const int delta = static_cast<int>(std::llround(-double(correction) * p.rate / 1000000.0));
            if (swr_set_compensation(p.swr, delta, p.rate) < 0) return false;
            p.appliedPpm = correction;
            p.lastCompensationMs = nowMs;
        }
    }
    const float* samples = p.input.data();
    int count = frames;
    if (p.swr) {
        const int capacity = swr_get_out_samples(p.swr, frames);
        if (capacity < 0 || capacity > p.rate / 5) return false;
        p.corrected.resize(capacity * p.channels);
        const std::uint8_t* inputs[] = {reinterpret_cast<const std::uint8_t*>(samples)};
        std::uint8_t* outputs[] = {reinterpret_cast<std::uint8_t*>(p.corrected.data())};
        count = swr_convert(p.swr, outputs, capacity, inputs, frames);
        if (count < 0) return false;
        samples = p.corrected.data();
    }
    if (!p.ring->push(samples, count)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "PLANK PipeWire PCM capacity exceeded; recreating output");
        return false;
    }
    p.rawFrames += frames;
    p.lastMediaMs = p.submittedFrames * 1000 / p.rate;
    p.submittedFrames += count;
    if (!p.lastLogMs || nowMs - p.lastLogMs >= 1000) {
        p.lastLogMs = nowMs;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "PLANK PipeWire playback: common=%d valid=%d lead_us=%lld delay_us=%lld queue_ms=%d quantum=%u reserve_us=%d requested_ppm=%d applied_ppm=%d pulls=%llu underruns=%llu missing_frames=%llu priming=%d gaps=%llu",
            p.commonEpoch, timing.phaseValid, static_cast<long long>(timing.estimatedLeadUs),
            static_cast<long long>(delayUs), getQueuedAudioDurationMs(), clock.quantum, minimumUs,
            p.requestedPpm, p.appliedPpm, static_cast<unsigned long long>(clock.pulls),
            static_cast<unsigned long long>(clock.underruns), static_cast<unsigned long long>(clock.missingFrames),
            clock.priming, static_cast<unsigned long long>(timing.discontinuities));
    }
    return true;
}

int PipeWireAudioRenderer::getCapabilities() { return CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION; }
int PipeWireAudioRenderer::getQueuedAudioDurationMs() { return m_Impl->ring ? m_Impl->ring->queued() * 1000 / m_Impl->rate : -1; }
int PipeWireAudioRenderer::getDeviceBufferDurationMs() {
    const auto c = m_Impl->snapshot.get();
    if (!m_Impl->stream || !c.tailNs) return -1;
    const auto now = pw_stream_get_nsec(m_Impl->stream);
    return c.tailNs > now ? static_cast<int>((c.tailNs - now) / 1000000) : 0;
}
qint64 PipeWireAudioRenderer::getSubmittedAudioMediaTimeMs() { return m_Impl->lastMediaMs; }
int PipeWireAudioRenderer::getAudioClockCorrectionPpm() { return m_Impl->appliedPpm; }
int PipeWireAudioRenderer::getAudioBacklogCorrectionPpm() { return m_Impl->appliedPpm - m_Impl->requestedPpm; }
PipeWireAudioRenderer::Diagnostics PipeWireAudioRenderer::diagnostics() const {
    const auto c = m_Impl->snapshot.get();
    return {c.pulls, c.underruns, c.missingFrames,
        m_Impl->ring ? static_cast<int>(m_Impl->ring->queued() * 1000 / m_Impl->rate) : 0,
        static_cast<int>(c.quantum), c.priming};
}
