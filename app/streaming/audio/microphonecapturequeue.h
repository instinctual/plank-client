#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

// Serialized by the capture source. Receipt age is independent of source time:
// a stopped source must never leave old speech waiting for an encoder to resume.
class PlankMicrophoneCaptureQueue
{
public:
    struct Packet {
        float samples[480 * 2] {};
        std::uint64_t sampleTime = 0, captureTimeNs = 0;
    };
    bool append(const float* samples, unsigned frames, std::uint64_t capture, std::uint64_t now) {
        if (!samples || !frames || frames > 2880 || !capture || !now ||
                capture > INT64_MAX - 60000000) return false;
        for (unsigned n = 0; n < frames * 2; n++) if (!std::isfinite(samples[n])) return false;
        if (m_ExpectedCapture && (capture > m_ExpectedCapture ? capture - m_ExpectedCapture : m_ExpectedCapture - capture) > 2000000)
            discard();
        expire(now);
        for (unsigned at = 0; at < frames;) {
            if (!m_PartialFrames) {
                m_Partial.packet.sampleTime = m_NextSample;
                m_Partial.packet.captureTimeNs = capture + std::uint64_t(at) * 1000000000 / 48000;
                m_Partial.arrived = now;
            }
            const unsigned copy = std::min(480 - m_PartialFrames, frames - at);
            std::memcpy(m_Partial.packet.samples + m_PartialFrames * 2, samples + at * 2, copy * 2 * sizeof(float));
            at += copy; m_PartialFrames += copy;
            if (m_PartialFrames == 480) {
                if (m_Count == m_Packets.size()) drop();
                m_Packets[(m_Head + m_Count++) % m_Packets.size()] = m_Partial;
                m_NextSample += 480; m_PartialFrames = 0; m_Partial = {};
            }
        }
        m_ExpectedCapture = capture + std::uint64_t(frames) * 1000000000 / 48000;
        return true;
    }
    bool take(Packet& packet, std::uint64_t now) {
        expire(now);
        if (!m_Count) return false;
        packet = m_Packets[m_Head].packet; drop(); return true;
    }
private:
    struct Entry { Packet packet; std::uint64_t arrived = 0; };
    std::array<Entry, 6> m_Packets {};
    Entry m_Partial {};
    unsigned m_Head = 0, m_Count = 0, m_PartialFrames = 0;
    std::uint64_t m_NextSample = 0, m_ExpectedCapture = 0;
    static bool stale(std::uint64_t arrived, std::uint64_t now) {
        return now < arrived || now - arrived >= 100000000;
    }
    void drop() { m_Packets[m_Head] = {}; m_Head = (m_Head + 1) % m_Packets.size(); m_Count--; }
    void discard() {
        while (m_Count) drop();
        // Keep packet sequence gaps visible to reset the codec at both ends.
        m_NextSample += 480;
        m_Partial = {}; m_PartialFrames = 0; m_ExpectedCapture = 0;
    }
    void expire(std::uint64_t now) {
        while (m_Count && stale(m_Packets[m_Head].arrived, now)) drop();
        if (m_PartialFrames && stale(m_Partial.arrived, now)) discard();
    }
};
