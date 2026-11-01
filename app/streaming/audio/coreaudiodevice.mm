#include "coreaudiodevice.h"
#include <dispatch/dispatch.h>
#include <Block.h>
#include <QDebug>
#include <cmath>
#include <vector>

namespace PlankCoreAudio {
AudioStreamBasicDescription floatFormat(double rate, unsigned channels)
{
    AudioStreamBasicDescription format {};
    format.mSampleRate = rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    format.mBytesPerPacket = format.mBytesPerFrame = channels * sizeof(float);
    format.mFramesPerPacket = 1;
    format.mChannelsPerFrame = channels;
    format.mBitsPerChannel = 32;
    return format;
}

bool createUnit(OSType type, AudioUnit& unit)
{
    AudioComponentDescription description {};
    description.componentType = kAudioUnitType_Output;
    description.componentSubType = type;
    description.componentManufacturer = kAudioUnitManufacturer_Apple;
    const auto component = AudioComponentFindNext(nullptr, &description);
    return component && AudioComponentInstanceNew(component, &unit) == noErr;
}
void closeUnit(AudioUnit& unit)
{
    if (!unit) return;
    AudioOutputUnitStop(unit);
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
    unit = nullptr;
}

int outputLatencyMs(AudioDeviceID device, AudioUnit unit)
{
    Float64 rate = 0, unitSeconds = 0;
    UInt32 latency = 0, safety = 0, buffer = 0, size = sizeof(unitSeconds);
    if (!property(device, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, rate) ||
        !std::isfinite(rate) || rate <= 0 ||
        !property(device, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, buffer) ||
        !property(device, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, latency) ||
        !property(device, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, safety)) return -1;
    AudioUnitGetProperty(unit, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &unitSeconds, &size);
    // Stream latency is distinct from device latency. Use the first output
    // stream for the single A/V clock, also when a device exposes several.
    AudioObjectPropertyAddress streams {kAudioDevicePropertyStreams,
        kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
    UInt32 streamLatency = 0;
    size = 0;
    if (AudioObjectGetPropertyDataSize(device, &streams, 0, nullptr, &size) == noErr &&
            size >= sizeof(AudioStreamID) && size <= 64 * sizeof(AudioStreamID) &&
            size % sizeof(AudioStreamID) == 0) {
        std::vector<AudioStreamID> ids(size / sizeof(AudioStreamID));
        if (AudioObjectGetPropertyData(device, &streams, 0, nullptr, &size, ids.data()) == noErr &&
                size >= sizeof(AudioStreamID))
            property(ids.front(), kAudioStreamPropertyLatency, kAudioObjectPropertyScopeOutput, streamLatency);
    }
    const double milliseconds = 1000 * ((double(latency) + streamLatency + safety + buffer) / rate + unitSeconds);
    return std::isfinite(milliseconds) && milliseconds >= 0 && milliseconds < 2000 ? int(std::ceil(milliseconds)) : -1;
}

struct DeviceWatch::Impl {
    struct Entry { AudioObjectID object; AudioObjectPropertyAddress address; };
    std::atomic_bool changed {false};
    bool started = false;
    dispatch_queue_t queue = dispatch_queue_create("la.instinctual.Plank.Client.audio-device", DISPATCH_QUEUE_SERIAL);
    AudioObjectPropertyListenerBlock callback = nullptr;
    std::vector<Entry> entries;
    Impl() {
        auto* state = this;
        callback = Block_copy(^(UInt32, const AudioObjectPropertyAddress*) { state->changed.store(true); });
    }
    ~Impl() {
        for (const auto& entry : entries)
            AudioObjectRemovePropertyListenerBlock(entry.object, &entry.address, queue, callback);
        dispatch_sync(queue, ^{});
        Block_release(callback);
        dispatch_release(queue);
    }
    bool add(AudioObjectID object, AudioObjectPropertySelector selector) {
        Entry entry {object, {selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain}};
        if (AudioObjectAddPropertyListenerBlock(object, &entry.address, queue, callback) != noErr) return false;
        entries.push_back(entry);
        return true;
    }
};
DeviceWatch::DeviceWatch() : m_Impl(new Impl) {}
DeviceWatch::~DeviceWatch() = default;
bool DeviceWatch::start(bool input, AudioDeviceID& device)
{
    const auto selector = input ? kAudioHardwarePropertyDefaultInputDevice : kAudioHardwarePropertyDefaultOutputDevice;
    if (!m_Impl->add(kAudioObjectSystemObject, selector) ||
        !property(kAudioObjectSystemObject, selector, kAudioObjectPropertyScopeGlobal, device) || !device ||
        !m_Impl->add(device, kAudioDevicePropertyDeviceIsAlive) ||
        !m_Impl->add(device, kAudioDevicePropertyNominalSampleRate) ||
        !m_Impl->add(device, kAudioDevicePropertyBufferFrameSize)) return false;
    UInt32 alive = 0;
    m_Impl->started = property(device, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, alive) && alive;
    return valid();
}
bool DeviceWatch::valid() const { return m_Impl->started && !m_Impl->changed.load(); }
bool DeviceWatch::changed() const { return m_Impl->changed.load(); }
}
