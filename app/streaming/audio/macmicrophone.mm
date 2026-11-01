#include "macmicrophone.h"
#include "coreaudiodevice.h"
#include <CoreAudio/HostTime.h>
#include <QDebug>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>
extern "C" {
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

struct PlankMacMicrophone::Impl {
    static constexpr unsigned Capacity = 8, MaximumFrames = 16384;
    struct Block {
        std::vector<float> samples;
        unsigned frames = 0;
        std::uint64_t time = 0;
    };
    PlankCoreAudio::DeviceWatch device;
    AudioUnit unit = nullptr;
    std::atomic_bool failed {false};
    std::array<Block, Capacity> blocks;
    std::atomic<std::uint64_t> written {0}, read {0};
    std::vector<float> scratch, converted;
    unsigned rate = 0, channels = 0;
    SwrContext* swr = nullptr;
    PlankMicrophoneCaptureQueue packets;
    std::uint64_t expected = 0;

    static OSStatus input(void* context, AudioUnitRenderActionFlags* flags,
                          const AudioTimeStamp* stamp, UInt32, UInt32 frames, AudioBufferList*) {
        auto& self = *static_cast<Impl*>(context);
        if (!stamp || !(stamp->mFlags & kAudioTimeStampHostTimeValid) || !frames || frames > MaximumFrames) {
            self.failed.store(true); return kAudio_ParamError;
        }
        AudioBufferList data {};
        data.mNumberBuffers = 1;
        data.mBuffers[0] = {self.channels, UInt32(frames * self.channels * sizeof(float)), self.scratch.data()};
        const auto status = AudioUnitRender(self.unit, flags, stamp, 1, frames, &data);
        if (status != noErr || data.mNumberBuffers != 1 || data.mBuffers[0].mData != self.scratch.data() ||
            data.mBuffers[0].mDataByteSize != frames * self.channels * sizeof(float)) {
            self.failed.store(true); return status != noErr ? status : kAudio_ParamError;
        }
        const auto w = self.written.load(std::memory_order_relaxed);
        if (w - self.read.load(std::memory_order_acquire) == Capacity) return noErr;
        auto& block = self.blocks[w % Capacity];
        std::memcpy(block.samples.data(), self.scratch.data(), frames * self.channels * sizeof(float));
        block.frames = frames;
        block.time = AudioConvertHostTimeToNanos(stamp->mHostTime);
        self.written.store(w + 1, std::memory_order_release);
        return noErr;
    }

    bool open() {
        AudioDeviceID id = kAudioObjectUnknown;
        if (!device.start(true, id) || !PlankCoreAudio::createUnit(kAudioUnitSubType_HALOutput, unit)) return false;
        UInt32 yes = 1, no = 0;
        if (AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &yes, sizeof(yes)) != noErr ||
            AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &no, sizeof(no)) != noErr ||
            AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &id, sizeof(id)) != noErr) return false;
        AudioStreamBasicDescription hardware {};
        UInt32 size = sizeof(hardware);
        if (AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 1, &hardware, &size) != noErr ||
            !std::isfinite(hardware.mSampleRate) || hardware.mSampleRate < 8000 || hardware.mSampleRate > 192000 ||
            !hardware.mChannelsPerFrame) return false;
        rate = unsigned(std::llround(hardware.mSampleRate));
        channels = std::min(2U, hardware.mChannelsPerFrame);
        const auto format = PlankCoreAudio::floatFormat(rate, channels);
        // AUHAL converts PCM layout but not sample rate. Resample on our worker,
        // never inside its realtime capture callback or by changing OS settings.
        if (AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &format, sizeof(format)) != noErr) return false;
        const SInt32 map[] {0, 1};
        if (AudioUnitSetProperty(unit, kAudioOutputUnitProperty_ChannelMap, kAudioUnitScope_Output, 1,
                                 map, channels * sizeof(SInt32)) != noErr) return false;
        UInt32 maximum = MaximumFrames;
        if (AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
                                 &maximum, sizeof(maximum)) != noErr) return false;
        scratch.resize(MaximumFrames * channels);
        for (auto& block : blocks) block.samples.resize(MaximumFrames * channels);
        AVChannelLayout source {}, target = AV_CHANNEL_LAYOUT_STEREO;
        av_channel_layout_default(&source, channels);
        const int allocated = swr_alloc_set_opts2(&swr, &target, AV_SAMPLE_FMT_FLT, 48000,
            &source, AV_SAMPLE_FMT_FLT, rate, 0, nullptr);
        av_channel_layout_uninit(&source);
        if (allocated < 0 || !swr || swr_init(swr) < 0) return false;
        AURenderCallbackStruct callback {input, this};
        // AUHAL input notification is a global property on element 0;
        // AudioUnitRender below reads the captured data from input bus 1.
        if (AudioUnitSetProperty(unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0,
                                 &callback, sizeof(callback)) != noErr ||
            AudioUnitInitialize(unit) != noErr || AudioOutputUnitStart(unit) != noErr || !device.valid()) return false;
        qInfo("Core Audio microphone: OS-selected input, %u channels at %u Hz; stream is stereo 48000 Hz", channels, rate);
        return true;
    }
    Impl() { if (!open()) failed.store(true); }
    ~Impl() { PlankCoreAudio::closeUnit(unit); swr_free(&swr); }

    bool take(Packet& packet) {
        const auto now = AudioConvertHostTimeToNanos(AudioGetCurrentHostTime());
        if (packets.take(packet, now)) return true;
        for (unsigned attempt = 0; attempt < Capacity; ++attempt) {
            const auto r = read.load(std::memory_order_relaxed);
            if (r == written.load(std::memory_order_acquire)) break;
            const auto& block = blocks[r % Capacity];
            if (block.time && block.time <= now && now - block.time < 100000000) {
                if (expected && (block.time > expected ? block.time - expected : expected - block.time) > 2000000) {
                    swr_close(swr);
                    if (swr_init(swr) < 0) { failed.store(true); return false; }
                }
                expected = block.time + std::uint64_t(block.frames) * 1000000000 / rate;
                const auto delay = swr_get_delay(swr, 1000000000);
                const auto first = delay >= 0 && std::uint64_t(delay) < block.time ? block.time - delay : 0;
                const int capacity = swr_get_out_samples(swr, block.frames);
                if (!first || capacity <= 0 || capacity > 100000) { failed.store(true); return false; }
                converted.resize(std::size_t(capacity) * 2);
                const auto* input = reinterpret_cast<const std::uint8_t*>(block.samples.data());
                auto* output = reinterpret_cast<std::uint8_t*>(converted.data());
                const int count = swr_convert(swr, &output, capacity, &input, block.frames);
                if (count < 0) { failed.store(true); return false; }
                // Bound each queue append independently of hardware quantum.
                for (unsigned at = 0; at < unsigned(count);) {
                    const unsigned chunk = std::min(2880U, unsigned(count) - at);
                    if (!packets.append(converted.data() + at * 2, chunk,
                        first + std::uint64_t(at) * 1000000000 / 48000, now)) { failed.store(true); return false; }
                    at += chunk;
                }
            }
            read.store(r + 1, std::memory_order_release);
            if (packets.take(packet, now)) return true;
        }
        return false;
    }
};
PlankMacMicrophone::PlankMacMicrophone() : m_Impl(new Impl) {}
PlankMacMicrophone::~PlankMacMicrophone() = default;
bool PlankMacMicrophone::valid() const { return !m_Impl->failed.load() && m_Impl->device.valid(); }
bool PlankMacMicrophone::deviceChanged() const { return m_Impl->device.changed(); }
bool PlankMacMicrophone::take(Packet& packet) { return valid() && m_Impl->take(packet); }
