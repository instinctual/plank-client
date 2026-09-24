#pragma once
#include "microphonecapturequeue.h"
#include <memory>

// Capture-only PipeWire source. It supplies fixed stereo PCM packets and the
// first sample's source clock. No encoding or network operations in callbacks.
class PlankLinuxMicrophone
{
public:
    using Packet = PlankMicrophoneCaptureQueue::Packet;
    PlankLinuxMicrophone();
    ~PlankLinuxMicrophone();
    bool valid() const;
    bool take(Packet& packet);
private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
