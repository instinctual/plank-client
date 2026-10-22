#include "PlankMacWacom.h"
#include "macrawwacom.h"
#include <mutex>
#include <memory>
struct CallbackState {
    std::mutex mutex;
    PlankMacWacomSend send;
    PlankMacWacomActivity activity;
    void* context;
    bool deliver(const unsigned char* bytes, std::size_t length) {
        std::lock_guard<std::mutex> guard(mutex);
        return send && send(context, bytes, length);
    }
    void notifyActivity() {
        std::lock_guard<std::mutex> guard(mutex);
        if (activity) activity(context);
    }
    void cancel() {
        std::lock_guard<std::mutex> guard(mutex);
        send = nullptr;
        activity = nullptr;
        context = nullptr;
    }
};
struct PlankMacWacom {
    std::shared_ptr<CallbackState> callbacks;
    std::unique_ptr<MacRawWacomInput> capture;
};
extern "C" PlankMacWacom* plank_mac_wacom_create(PlankMacWacomSend send, PlankMacWacomActivity activity, void* context) {
    if (!send || !context) return nullptr;
    auto result = std::make_unique<PlankMacWacom>();
    result->callbacks = std::make_shared<CallbackState>();
    result->callbacks->send = send;
    result->callbacks->activity = activity;
    result->callbacks->context = context;
    result->capture = std::make_unique<MacRawWacomInput>(
        [state = result->callbacks] { state->notifyActivity(); },
        [state = result->callbacks](const unsigned char* bytes, std::size_t length) {
            return state->deliver(bytes, length);
        });
    return result.release();
}
extern "C" void plank_mac_wacom_active(PlankMacWacom* state, bool active) {
    if (state) state->capture->setActive(active);
}
extern "C" void plank_mac_wacom_control(PlankMacWacom* state, const uint8_t* bytes, size_t size) {
    if (state && size <= UINT32_MAX) state->capture->handleControl(bytes, static_cast<unsigned>(size));
}
extern "C" void plank_mac_wacom_destroy(PlankMacWacom* state) {
    if (!state) return;
    // Release and emit SUSPEND before revoking the sender and destroying the
    // worker. A driver that exceeds its deadline cannot call Swift afterwards.
    state->capture->setActive(false);
    state->callbacks->cancel();
    state->capture.reset();
    delete state;
}
