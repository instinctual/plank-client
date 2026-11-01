#pragma once
#include "microphonecapturequeue.h"
#include <memory>

class PlankMacMicrophone {
public:
    using Packet = PlankMicrophoneCaptureQueue::Packet;
    PlankMacMicrophone();
    ~PlankMacMicrophone();
    bool valid() const;
    bool deviceChanged() const;
    bool take(Packet& packet);
private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
