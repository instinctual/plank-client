#pragma once
#include "videoframe.h"
#include <functional>

// One decoder generation's connection to the GUI-owned presenter. Operations
// are callable from a worker. submit never waits for the GUI; qualify must
// prove an actual GPU draw within a bounded, cancellable wait. discard retires
// this generation only, including a pending qualification request.
struct QtDecodedVideoDelivery
{
    std::function<bool(QtVideoFrame::Ptr)> submit;
    std::function<bool(QtVideoFrame::Ptr, QString&)> qualify;
    std::function<void()> discard;
    std::function<void(QString)> fail;
};
