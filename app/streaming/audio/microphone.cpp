#include "microphone.h"
#include "microphoneopus.h"
#include <QDebug>
#include <chrono>
#include <memory>
#ifdef Q_OS_LINUX
#include "linuxmicrophone.h"
#endif
#ifdef Q_OS_MACOS
#include "macmicrophonepermission.h"
#include "macmicrophone.h"
#endif
#ifdef PLANK_TRANSPORT
#include <plank_transport.h>
#include <plank_transport_control.h>
#endif

PlankMicrophone::PlankMicrophone(PlankTransportNativeEndpoint* endpoint,
                               std::atomic<bool>& requested, bool automaticInput, bool timed)
    : m_Endpoint(endpoint), m_Requested(requested), m_AutomaticInput(automaticInput), m_Timed(timed),
      m_Thread(&PlankMicrophone::run, this)
{
}

PlankMicrophone::~PlankMicrophone()
{
    {
        std::lock_guard<std::mutex> guard(m_Mutex);
        m_Stop = true;
    }
    m_Wake.notify_all();
    m_Thread.join();
}

void PlankMicrophone::acknowledge(std::uint64_t generation, std::uint32_t state)
{
    std::lock_guard<std::mutex> guard(m_Mutex);
    if (generation >= m_AckGeneration) {
        m_AckGeneration = generation;
        m_AckState = state;
    }
    m_Wake.notify_all();
}

void PlankMicrophone::run()
{
#ifndef PLANK_TRANSPORT
    m_State.store(State::Unavailable);
#else
    using Clock = std::chrono::steady_clock;
#ifdef Q_OS_LINUX
    using Capture = PlankLinuxMicrophone;
#else
    using Capture = PlankMacMicrophone;
#endif
    std::unique_ptr<Capture> capture;
    std::uint64_t nextSample = 0;
    OpusEncoder* encoder = nullptr;
    const auto closeCapture = [&] {
        capture.reset();
        if (encoder) opus_encoder_destroy(encoder);
        encoder = nullptr;
    };
    std::uint64_t command = 0;
    bool configured = false, sentEnabled = false, failed = false;
    bool lastRequested = m_Requested.load();
    auto deadline = Clock::now();
    auto lastSamples = Clock::now();
    bool captured = false;
#ifdef Q_OS_MACOS
    unsigned deviceRetries = 0;
    auto nextCaptureAttempt = Clock::now();
#endif
    for (;;) {
        std::uint64_t ackGeneration;
        std::uint32_t ackState;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            if (m_Wake.wait_for(lock, std::chrono::milliseconds(5), [&] { return m_Stop; })) break;
            ackGeneration = m_AckGeneration; ackState = m_AckState;
        }
        const bool requested = m_Requested.load();
        if (requested != lastRequested) { failed = false; lastRequested = requested; }
        int permission = 1;
#ifdef Q_OS_MACOS
        // Consent is requested by the launcher, never by a stream/toolbar toggle.
        // An unprovisioned CLI launch remains usable without microphone capture.
        if (requested && !failed) {
            permission = plankMacMicrophonePermission() == 1 ? 1 : -1;
            if (permission < 0) qWarning(
                "Microphone permission is unavailable; authorize PLANK Client in System Settings or reopen the launcher before connecting");
        }
#endif
        if (permission < 0) failed = true;
        const bool enabled = requested && !failed && permission == 1;
        if (!enabled) closeCapture();
        if (!configured || enabled != sentEnabled) {
            // Stop locally before sending mute; a full reliable queue must never
            // leave capture running while waiting for the remote acknowledgement.
            closeCapture(); plank_transport_native_microphone_activate(m_Endpoint, 0);
            const std::uint64_t next = command + 1;
            const std::uint32_t words[] = {std::uint32_t(next >> 32), std::uint32_t(next),
                (enabled ? PLANK_TRANSPORT_MICROPHONE_ENABLED : 0u) |
                (m_AutomaticInput ? PLANK_TRANSPORT_MICROPHONE_AUTO_INPUT : 0u)};
            std::uint8_t packet[20]; size_t size = 0;
            plank_transport_control_encode(PLANK_TRANSPORT_CONTROL_SET_MICROPHONE, words, 3, packet, sizeof(packet), &size);
            if (plank_transport_native_data_send(m_Endpoint, packet, size) != PLANK_TRANSPORT_OK) {
                m_State.store(failed ? State::Unavailable : State::Pending); continue;
            }
            command = next; configured = true; sentEnabled = enabled;
            deadline = Clock::now() + std::chrono::seconds(5);
            captured = false;
        }
        if (failed) { m_State.store(State::Unavailable); continue; }
        if (requested && !permission) { m_State.store(State::Pending); continue; }
        if (ackGeneration != command || ackState == PLANK_TRANSPORT_MICROPHONE_PENDING) {
            if (Clock::now() >= deadline) { failed = true; closeCapture(); }
            m_State.store(failed ? State::Unavailable : State::Pending); continue;
        }
        if (ackState == PLANK_TRANSPORT_MICROPHONE_UNAVAILABLE ||
            plank_transport_native_microphone_state(m_Endpoint) == 3) {
            failed = true; closeCapture(); m_State.store(State::Unavailable); continue;
        }
        if (!enabled) { m_State.store(State::Off); continue; }
        if (ackState != PLANK_TRANSPORT_MICROPHONE_ACTIVE) { failed = true; continue; }
#ifdef Q_OS_MACOS
        // Default-input changes and unplug/replug are local device events. Start
        // a new negotiated generation so the Host never sees a sample clock
        // restart inside the previous one. Retry briefly while Core Audio settles.
        if (capture && capture->deviceChanged()) {
            closeCapture();
            plank_transport_native_microphone_activate(m_Endpoint, 0);
            configured = false;
            captured = false;
            deviceRetries = 5;
            nextCaptureAttempt = Clock::now() + std::chrono::milliseconds(500);
            m_State.store(State::Pending);
            continue;
        }
        if (Clock::now() < nextCaptureAttempt) {
            m_State.store(State::Pending);
            continue;
        }
#endif
        if (!capture) {
            capture.reset(new Capture);
            nextSample = 0;
            encoder = plankMicrophoneCreateEncoder();
#ifdef Q_OS_MACOS
            if (!capture->valid() && deviceRetries) {
                --deviceRetries;
                closeCapture();
                configured = false;
                nextCaptureAttempt = Clock::now() + std::chrono::seconds(1);
                m_State.store(State::Pending);
                continue;
            }
#endif
            if (!capture->valid() || !encoder ||
                    plank_transport_native_microphone_activate(m_Endpoint, command) != PLANK_TRANSPORT_OK) {
                failed = true; closeCapture(); continue;
            }
#ifdef Q_OS_MACOS
            deviceRetries = 0;
#endif
            lastSamples = Clock::now();
        }
        if (!capture->valid()) { failed = true; closeCapture(); continue; }
        int lookahead = 0;
        if (opus_encoder_ctl(encoder, OPUS_GET_LOOKAHEAD(&lookahead)) != OPUS_OK || lookahead < 0 || lookahead > 480) {
            failed = true; closeCapture(); continue;
        }
        Capture::Packet input;
        for (unsigned i = 0; i < 6 && capture->take(input); i++) {
            const auto codecDelay = std::uint64_t(lookahead) * 1000000000 / PlankMicrophoneRate;
            if (m_Timed && input.captureTimeNs <= codecDelay) { failed = true; break; }
            if (input.sampleTime != nextSample && opus_encoder_ctl(encoder, OPUS_RESET_STATE) != OPUS_OK) {
                failed = true; break;
            }
            nextSample = input.sampleTime + 480;
            std::uint8_t packet[1275];
            const int bytes = opus_encode_float(encoder, input.samples, 480, packet, sizeof(packet));
            if (bytes < 1) { failed = true; break; }
            const int result = m_Timed ?
                plank_transport_native_microphone_send_timed(m_Endpoint, command,
                    input.sampleTime, input.captureTimeNs - codecDelay, packet, std::size_t(bytes)) :
                plank_transport_native_microphone_send(m_Endpoint, command,
                    input.sampleTime, packet, std::size_t(bytes));
            captured = true; lastSamples = Clock::now();
            if (result != PLANK_TRANSPORT_OK && result != PLANK_TRANSPORT_DROPPED && result != PLANK_TRANSPORT_TIMEOUT) {
                failed = true; break;
            }
        }
        if (Clock::now() - lastSamples > std::chrono::seconds(2)) failed = true;
        if (failed) closeCapture();
        m_State.store(failed ? State::Unavailable : captured ? State::Active : State::Pending);
    }
    closeCapture();
    plank_transport_native_microphone_activate(m_Endpoint, 0);
#endif
}
