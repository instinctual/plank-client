#pragma once
#include "videoframe.h"
#include <functional>

// One decoder generation's connection to the GUI-owned presenter. Operations
// are callable from a worker. submit never waits for the GUI; qualify must
// prove an actual GPU draw within a bounded, cancellable wait. Qualification
// frames are explicitly marked and cleared to black before output swap; they
// cannot be submitted as live playback. discard retires
// this generation only, including a pending qualification request.
struct QtDecodedVideoDelivery
{
    std::function<bool(QtVideoFrame::Ptr)> submit;
    std::function<bool(QtVideoFrame::Ptr, QString&)> qualify;
    std::function<void()> discard;
    std::function<void(QString)> fail;
};
