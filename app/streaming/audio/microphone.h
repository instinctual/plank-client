#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

struct PlankTransportNativeEndpoint;

// Owns capture only while the authenticated Host has acknowledged activation.
// Stop/join before endpoint destruction. The separate output renderer is not
// touched, and no captured samples are written to disk or diagnostic logs.
class PlankMicrophone
{
public:
    enum class State { Off, Pending, Active, Unavailable };
    PlankMicrophone(PlankTransportNativeEndpoint* endpoint,
                    std::atomic<bool>& requested, bool automaticInput, bool timed = false);
    ~PlankMicrophone();
    State state() const { return m_State.load(); }
    void acknowledge(std::uint64_t generation, std::uint32_t state);

private:
    void run();
    PlankTransportNativeEndpoint* const m_Endpoint;
    std::atomic<bool>& m_Requested;
    const bool m_AutomaticInput;
    const bool m_Timed;
    std::atomic<State> m_State {State::Off};
    std::mutex m_Mutex;
    std::condition_variable m_Wake;
    bool m_Stop = false;
    std::uint64_t m_AckGeneration = 0;
    std::uint32_t m_AckState = 0;
    std::thread m_Thread;
};
