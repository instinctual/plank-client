#include "linuxmicrophone.h"
#include "microphonecaptureclock.h"
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-utils.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>

static_assert(PW_CHECK_VERSION(1, 0, 5), "Timestamped capture requires PipeWire 1.0.5 or newer");

struct PlankLinuxMicrophone::Impl {
    pw_thread_loop* loop = nullptr;
    pw_stream* stream = nullptr;
    std::atomic<bool> failed {false};
    bool formatted = false;
    std::mutex mutex;
    PlankMicrophoneCaptureQueue packets;

    static void stateChanged(void* data, pw_stream_state, pw_stream_state state, const char*) {
        if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED)
            static_cast<Impl*>(data)->failed.store(true);
    }
    static void paramChanged(void* data, std::uint32_t id, const spa_pod* param) {
        auto* self = static_cast<Impl*>(data);
        if (id != SPA_PARAM_Format || !param) return;
        spa_audio_info_raw info {};
        self->formatted = spa_format_audio_raw_parse(param, &info) >= 0 &&
            info.format == SPA_AUDIO_FORMAT_F32 && info.rate == 48000 && info.channels == 2 &&
            info.position[0] == SPA_AUDIO_CHANNEL_FL && info.position[1] == SPA_AUDIO_CHANNEL_FR;
        if (!self->formatted) self->failed.store(true);
    }
    static void process(void* data) {
        auto* self = static_cast<Impl*>(data);
        // Bounded drain; use the timestamp stored with each buffer, including
        // when multiple graph cycles have queued before this callback runs.
        for (unsigned i = 0; i < 8; i++) {
            pw_buffer* buffer = pw_stream_dequeue_buffer(self->stream);
            if (!buffer) break;
            pw_time time {};
            const auto now = pw_stream_get_nsec(self->stream);
            std::uint64_t capture = 0;
            if (!self->failed.load() && self->formatted &&
                    pw_stream_get_time_n(self->stream, &time, sizeof(time)) >= 0)
                capture = plankMicrophoneCaptureTime(buffer->time, time.delay,
                    time.rate.num, time.rate.denom, time.buffered, now);
            spa_buffer* raw = buffer->buffer;
            bool valid = capture && raw && raw->n_datas == 1;
            if (valid) {
                const spa_data& plane = raw->datas[0];
                const spa_chunk* chunk = plane.chunk;
                valid = plane.data && chunk && chunk->stride == 8 && chunk->size &&
                    !(chunk->size % 8) && chunk->size <= 2880 * 8 &&
                    chunk->offset <= plane.maxsize && chunk->size <= plane.maxsize - chunk->offset &&
                    !(chunk->flags & SPA_CHUNK_FLAG_CORRUPTED);
                if (valid) {
                    const auto* samples = reinterpret_cast<const float*>(
                        static_cast<const std::uint8_t*>(plane.data) + chunk->offset);
                    valid = !(reinterpret_cast<std::uintptr_t>(samples) % alignof(float));
                    if (valid) for (unsigned n = 0; n < chunk->size / sizeof(float); n++)
                        if (!std::isfinite(samples[n])) { valid = false; break; }
                    if (valid) {
                        std::lock_guard<std::mutex> guard(self->mutex);
                        valid = self->packets.append(samples, chunk->size / 8, capture, now);
                    }
                }
            }
            pw_stream_queue_buffer(self->stream, buffer);
            if (!valid) self->failed.store(true);
        }
    }
    Impl() {
        static std::once_flag initialized;
        std::call_once(initialized, [] { pw_init(nullptr, nullptr); });
        loop = pw_thread_loop_new("PLANK microphone", nullptr);
        if (!loop) { failed.store(true); return; }
        static const pw_stream_events events = [] {
            pw_stream_events value {};
            value.version = PW_VERSION_STREAM_EVENTS;
            value.state_changed = stateChanged; value.param_changed = paramChanged; value.process = process;
            return value;
        }();
        stream = pw_stream_new_simple(pw_thread_loop_get_loop(loop), "PLANK microphone",
            pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
                PW_KEY_MEDIA_ROLE, "Communication", PW_KEY_NODE_LATENCY, "480/48000", nullptr), &events, this);
        if (!stream) { failed.store(true); return; }
        std::uint8_t storage[1024]; spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
        spa_audio_info_raw info {};
        info.format = SPA_AUDIO_FORMAT_F32; info.rate = 48000; info.channels = 2;
        info.position[0] = SPA_AUDIO_CHANNEL_FL; info.position[1] = SPA_AUDIO_CHANNEL_FR;
        const spa_pod* params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};
        // Callbacks run on the dedicated ordinary PipeWire loop, not an RT
        // processing thread. They only validate/copy into a fixed 60 ms queue.
        if (pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1) < 0 ||
                pw_thread_loop_start(loop) < 0) failed.store(true);
    }
    ~Impl() {
        if (loop) pw_thread_loop_stop(loop);
        if (stream) pw_stream_destroy(stream);
        if (loop) pw_thread_loop_destroy(loop);
    }
};
PlankLinuxMicrophone::PlankLinuxMicrophone() : m_Impl(new Impl) {}
PlankLinuxMicrophone::~PlankLinuxMicrophone() = default;
bool PlankLinuxMicrophone::valid() const { return !m_Impl->failed.load(); }
bool PlankLinuxMicrophone::take(Packet& packet) {
    std::lock_guard<std::mutex> guard(m_Impl->mutex);
    return !m_Impl->failed.load() && m_Impl->packets.take(packet, pw_stream_get_nsec(m_Impl->stream));
}
