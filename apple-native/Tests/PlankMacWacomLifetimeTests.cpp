#include "PlankMacWacom.h"
#include "macrawwacom.h"
#include "macrawwacomasync.h"
#include "macrawwacomlogic.h"
#include <cassert>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstdio>
// A fake stalled driver retains its sender after destruction, just as the
// real bounded worker may. The test links the production C wrapper.
static MacRawWacomInput::SendFrame lateSender;
static std::function<void()> lateActivity;
class MacRawWacomInput::Impl {};
MacRawWacomInput::MacRawWacomInput(std::function<void()> activity, SendFrame send, GenerationProvider, bool) { lateSender = send; lateActivity = activity; }
MacRawWacomInput::~MacRawWacomInput() = default;
void MacRawWacomInput::setActive(bool active) { if (!active) { unsigned char release = 13; lateSender(&release, 1); } }
void MacRawWacomInput::handleControl(const unsigned char*, unsigned) {}
static bool send(void* ctx, const uint8_t*, size_t) { ++*static_cast<int*>(ctx); return true; }
int main() {
    assert(!plank_mac_wacom_create(nullptr, nullptr, nullptr));
    int called = 0;
    auto capture = plank_mac_wacom_create(send, [](void* ctx) { ++*static_cast<int*>(ctx); }, &called);
    unsigned char b = 3;
    assert(lateSender(&b, 1) && called == 1);
    lateActivity(); assert(called == 2);
    plank_mac_wacom_destroy(capture);
    assert(called == 3); // release precedes revocation
    lateActivity(); assert(called == 3);
    assert(!lateSender(&b, 1) && called == 3); // late driver cannot touch freed Swift context
    MacWacomLifecycle life;
    life.setActive(true); assert(life.canForward());
    auto ticket = life.setActive(false); assert(!life.canForward());
    assert(!life.wait(ticket, std::chrono::milliseconds(1)));
    life.setActive(true); assert(!life.canForward()); // timed-out release still owns barrier
    life.complete(ticket); assert(life.canForward());
    life.stop(); assert(!life.canForward());
    MacWacomAsyncResults results;
    const auto old = results.epoch(); results.invalidate();
    results.publish({old, 10, 0, 1, 0, 0, {1}}); assert(results.take().empty());
    results.publish({results.epoch(), 10, 0, 2, 0, 0, {2}}); assert(results.take().size() == 1);
    for (int n = 0; n < 100; ++n) results.publish({results.epoch(),10,0,2,0,0,{}});
    assert(results.take().size() == 64);
    assert(MacWacomWire::ioReportType(0) == 2 && MacWacomWire::ioReportType(2) == 0);
    assert(MacWacomWire::reportPrefix(0) == 1 && MacWacomWire::reportPrefix(5) == 0);
    std::puts("Mac Wacom lifetime, barrier, epoch, bounded callback checks passed");
}
