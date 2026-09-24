// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <QString>
#include <QVariantList>

struct PlankTransportNativeEndpoint;
class PlankCamera
{
public:
    enum class State { Off, Pending, Active, Unavailable };
    PlankCamera(PlankTransportNativeEndpoint* endpoint, std::atomic<bool>& requested,
                const QString& device);
    ~PlankCamera();
    State state() const { return m_State.load(); }
    void acknowledge(std::uint64_t generation, std::uint32_t state);
    void requestKeyframe(std::uint64_t generation);
    // Read-only native format enumeration. No capture or format changes.
    static QVariantList devices();
private:
    void run();
    PlankTransportNativeEndpoint* const m_Endpoint;
    std::atomic<bool>& m_Requested;
    const QString m_Device;
    std::atomic<State> m_State {State::Off};
    std::mutex m_Mutex;
    std::condition_variable m_Wake;
    bool m_Stop = false;
    std::uint64_t m_AckGeneration = 0, m_KeyGeneration = 0;
    std::uint32_t m_AckState = 0;
    std::thread m_Thread;
};
