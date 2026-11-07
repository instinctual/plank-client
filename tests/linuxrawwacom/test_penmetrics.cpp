#include "../../app/streaming/input/linuxpenmetrics.h"

#include <cstdlib>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "check failed at line " << __LINE__ << ": " #condition "\n"; \
    std::abort(); \
} } while (false)

static PlankPenActivity event(LinuxPenActivityTracker& tracker, unsigned short type,
                              unsigned short code, int value = 0)
{
    input_event input {};
    input.type = type; input.code = code; input.value = value;
    return tracker.consume(input);
}

int main()
{
    LinuxPenActivityTracker pen;
    CHECK(!pen.ready());
    event(pen, EV_ABS, ABS_X, 10);
    event(pen, EV_KEY, BTN_TOUCH, 1);
    CHECK(!event(pen, EV_SYN, SYN_REPORT).observed);
    pen.prime(100, 200, false, false, false);
    CHECK(pen.ready());
    event(pen, EV_KEY, BTN_TOOL_PEN, 1);
    event(pen, EV_ABS, ABS_X, 500);
    auto sample = event(pen, EV_SYN, SYN_REPORT);
    CHECK(sample.observed && sample.motion == 0 && sample.tipDown == 0);

    // Hover motion is counted once per decoded report, not once per axis.
    event(pen, EV_ABS, ABS_X, 501);
    event(pen, EV_ABS, ABS_Y, 201);
    sample = event(pen, EV_SYN, SYN_REPORT);
    CHECK(sample.motion == 1 && sample.tipDown == 0);
    for (int i = 0; i < 1000; ++i) {
        event(pen, EV_ABS, ABS_X, 501);
        event(pen, EV_ABS, ABS_PRESSURE, i);
        event(pen, EV_ABS, ABS_MT_POSITION_X, i);
        event(pen, EV_ABS, ABS_WHEEL, i);
        event(pen, EV_KEY, BTN_0, i & 1);
        event(pen, EV_MSC, MSC_SERIAL, i);
        sample = event(pen, EV_SYN, SYN_REPORT);
        CHECK(sample.observed && sample.motion == 0 && sample.tipDown == 0);
    }
    event(pen, EV_KEY, BTN_TOUCH, 1);
    sample = event(pen, EV_SYN, SYN_REPORT);
    CHECK(sample.motion == 0 && sample.tipDown == 1);
    CHECK(event(pen, EV_SYN, SYN_REPORT).tipDown == 0);
    event(pen, EV_ABS, ABS_X, 502);
    CHECK(event(pen, EV_SYN, SYN_REPORT).motion == 1);
    event(pen, EV_KEY, BTN_TOUCH, 0);
    CHECK(event(pen, EV_SYN, SYN_REPORT).tipDown == 0);

    // Movement without a pen/eraser in proximity is ignored.
    event(pen, EV_KEY, BTN_TOOL_PEN, 0);
    event(pen, EV_ABS, ABS_X, 0);
    CHECK(event(pen, EV_SYN, SYN_REPORT).motion == 0);
    event(pen, EV_KEY, BTN_TOOL_RUBBER, 1);
    event(pen, EV_ABS, ABS_X, 800);
    CHECK(event(pen, EV_SYN, SYN_REPORT).motion == 0);
    event(pen, EV_ABS, ABS_Y, 900);
    event(pen, EV_KEY, BTN_TOUCH, 1);
    sample = event(pen, EV_SYN, SYN_REPORT);
    CHECK(sample.motion == 1 && sample.tipDown == 1);

    // Overflow and resumed forwarding cannot replay stale motion or a held tip.
    event(pen, EV_SYN, SYN_DROPPED);
    CHECK(!pen.ready());
    event(pen, EV_ABS, ABS_X, 12345);
    CHECK(!event(pen, EV_SYN, SYN_REPORT).observed);
    pen.prime(12000, 9000, true, false, true);
    sample = event(pen, EV_SYN, SYN_REPORT);
    CHECK(sample.observed && sample.motion == 0 && sample.tipDown == 0);
    event(pen, EV_ABS, ABS_X, 12001);
    CHECK(event(pen, EV_SYN, SYN_REPORT).motion == 1);
    pen.reset();
    CHECK(!event(pen, EV_SYN, SYN_REPORT).observed);

    PlankPenActivityState normalized;
    sample = normalized.sample(100, 100, true, true);
    CHECK(sample.motion == 0 && sample.tipDown == 0);
    sample = normalized.sample(101, 100, true, true);
    CHECK(sample.motion == 1 && sample.tipDown == 0);
    normalized.reset();
    sample = normalized.sample(3000, 3000, true, true);
    CHECK(sample.motion == 0 && sample.tipDown == 0);
    std::cout << "pen metrics: hover, tip, idle reports, non-pen input and resynchronization passed\n";
}
