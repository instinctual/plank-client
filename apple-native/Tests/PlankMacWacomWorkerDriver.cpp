#include "PlankMacWacomWorkerDriver.h"
#include "macrawwacom.h"
#include <thread>
#include <atomic>

static MacRawWacomInput::SendFrame sender;
static std::function<void()> activity;
static std::atomic<unsigned> activations{0};
class MacRawWacomInput::Impl {};
MacRawWacomInput::MacRawWacomInput(std::function<void()> onActivity, SendFrame send, GenerationProvider, bool) { sender = send; activity = onActivity; activations = 0; }
MacRawWacomInput::~MacRawWacomInput() = default;
void MacRawWacomInput::setActive(bool active) {
    if (active) { ++activations; return; }
    uint8_t release[20]{}; release[6] = 13;
    if (sender) sender(release, sizeof(release));
}
void MacRawWacomInput::handleControl(const unsigned char*, unsigned) {}

extern "C" bool plank_mac_test_worker_send(const uint8_t* bytes, size_t length) {
    bool accepted = false;
    std::thread worker([&] { accepted = sender && sender(bytes, length); });
    worker.join();
    return accepted;
}

extern "C" void plank_mac_test_worker_activity() {
    std::thread worker([] { if (activity) activity(); }); worker.join();
}
extern "C" unsigned plank_mac_test_worker_activations() { return activations.load(); }
