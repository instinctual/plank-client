#pragma once

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <atomic>
#include <memory>

namespace PlankCoreAudio {
template<class T> bool property(AudioObjectID object, AudioObjectPropertySelector selector,
                                AudioObjectPropertyScope scope, T& result)
{
    AudioObjectPropertyAddress address {selector, scope, kAudioObjectPropertyElementMain};
    UInt32 bytes = sizeof(result);
    return AudioObjectGetPropertyData(object, &address, 0, nullptr, &bytes, &result) == noErr && bytes == sizeof(result);
}
AudioStreamBasicDescription floatFormat(double rate, unsigned channels);
bool createUnit(OSType type, AudioUnit& unit);
void closeUnit(AudioUnit& unit);
int outputLatencyMs(AudioDeviceID device, AudioUnit unit);

// Owns notifications on a private serial queue. A changed default device or
// device format invalidates the current audio epoch; the worker recreates it.
// Destruction removes registrations and drains callbacks before state is freed.
class DeviceWatch {
public:
    DeviceWatch();
    ~DeviceWatch();
    bool start(bool input, AudioDeviceID& device);
    bool valid() const;
    bool changed() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
}
