// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <chrono>
#include <vector>
#include <linux/videodev2.h>

// Single-thread owner. Call only after explicit user activation and Host
// acknowledgement. The destructor stops capture and restores the prior mode.
// No encoder, libv4l conversion, privilege helper, file output or hidden device
// fallback. Discovery/selection and session authorization belong to the caller.
class PlankLinuxNativeCamera
{
public:
    enum class Codec { H264, Mjpeg };
    enum class Result { Frame, Again, Dropped, Failed };
    struct Frame {
        std::vector<std::uint8_t> bytes;
        std::uint64_t captureTimeUs = 0;
        std::uint32_t driverSequence = 0;
        bool independent = false;
        bool discontinuity = false;
    };
    PlankLinuxNativeCamera() = default;
    ~PlankLinuxNativeCamera();
    PlankLinuxNativeCamera(const PlankLinuxNativeCamera&) = delete;
    PlankLinuxNativeCamera& operator=(const PlankLinuxNativeCamera&) = delete;
    bool open(const char* path, Codec codec, unsigned width, unsigned height);
    Result read(Frame& frame, unsigned waitMs = 20);
    // Standard V4L2 control, or descriptor-verified UVC H.264 IDR with SPS/PPS.
    // Rate limited to two requests/second. Success means request accepted;
    // callers must still await and validate an actual independent frame.
    bool requestKeyframe();
    bool close();
    const v4l2_pix_format& format() const { return m_Format; }

private:
    struct Mapping { void* address = nullptr; std::size_t size = 0; };
    int m_Fd = -1;
    bool m_Changed = false, m_Allocated = false, m_Streaming = false;
    bool m_HaveSequence = false, m_Discontinuity = true;
    unsigned m_Count = 0;
    std::uint32_t m_Sequence = 0;
    std::uint64_t m_CaptureTime = 0;
    Codec m_Codec = Codec::H264;
    std::uint8_t m_UvcUnit = 0;
    std::chrono::steady_clock::time_point m_LastKeyRequest {};
    v4l2_format m_PreviousFormat {};
    v4l2_streamparm m_PreviousParameters {};
    v4l2_pix_format m_Format {};
    std::array<Mapping, 4> m_Mappings {};
};
