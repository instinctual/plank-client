// SPDX-License-Identifier: GPL-3.0-or-later
#include "camera.h"
#include <chrono>
#include <QDir>
#include <QFile>
#include <cstring>
#include <QFileInfo>
#include <QDebug>
#if defined(Q_OS_LINUX) && defined(PLANK_TRANSPORT)
#include "linuxnativecamera.h"
#include <plank_transport.h>
#include <plank_transport_camera.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <algorithm>

namespace {
struct Device {
    QString path, node, name;
    bool h264 = false, mjpeg = false;
};
std::vector<Device> enumerate()
{
    std::vector<Device> result;
    for (const auto& name : QDir(QStringLiteral("/dev")).entryList({QStringLiteral("video*")}, QDir::System, QDir::Name)) {
        if (name.mid(5).isEmpty()) continue;
        bool numeric = false; name.mid(5).toUInt(&numeric); if (!numeric) continue;
        QString path = QStringLiteral("/dev/") + name;
        const auto encoded = QFile::encodeName(path);
        const int fd = ::open(encoded.constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
        if (fd < 0) continue;
        v4l2_capability caps{};
        bool valid = ioctl(fd, VIDIOC_QUERYCAP, &caps) == 0;
        const auto flags = caps.capabilities & V4L2_CAP_DEVICE_CAPS ? caps.device_caps : caps.capabilities;
        valid = valid && (flags & V4L2_CAP_VIDEO_CAPTURE) && (flags & V4L2_CAP_STREAMING);
        Device device;
        device.path = path; device.node = path;
        if (valid) {
            device.name = QString::fromUtf8(reinterpret_cast<const char*>(caps.card), strnlen(reinterpret_cast<const char*>(caps.card), sizeof(caps.card)));
            for (unsigned i = 0; i < 64; ++i) {
                v4l2_fmtdesc format{}; format.index = i; format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                if (ioctl(fd, VIDIOC_ENUM_FMT, &format) != 0) break;
                if (!(format.flags & V4L2_FMT_FLAG_COMPRESSED) || (format.flags & V4L2_FMT_FLAG_EMULATED)) continue;
                device.h264 |= format.pixelformat == V4L2_PIX_FMT_H264;
                device.mjpeg |= format.pixelformat == V4L2_PIX_FMT_MJPEG;
            }
        }
        ::close(fd);
        if (!device.h264 && !device.mjpeg) continue;
        // Prefer the stable udev identity while retaining only actual video
        // nodes. Recheck the opened device's formats at every activation.
        const QDir identities(QStringLiteral("/dev/v4l/by-id"));
        for (const auto& id : identities.entryList(QDir::Files | QDir::System, QDir::Name)) {
            const QString candidate = identities.filePath(id);
            if (QFileInfo(candidate).canonicalFilePath() == path) { device.path = candidate; break; }
        }
        if (device.name.isEmpty()) device.name = QStringLiteral("USB camera");
        result.push_back(device);
    }
    return result;
}
}
#endif

QVariantList PlankCamera::devices()
{
    QVariantList result;
#if defined(Q_OS_LINUX) && defined(PLANK_TRANSPORT)
    for (const auto& device : enumerate())
        result.append(QVariantMap{{QStringLiteral("id"), device.path}, {QStringLiteral("name"), device.name}});
#endif
    return result;
}
PlankCamera::PlankCamera(PlankTransportNativeEndpoint* endpoint, std::atomic<bool>& requested, const QString& device)
    : m_Endpoint(endpoint), m_Requested(requested), m_Device(device), m_Thread(&PlankCamera::run, this) {}
PlankCamera::~PlankCamera()
{
    { std::lock_guard<std::mutex> guard(m_Mutex); m_Stop = true; }
    m_Wake.notify_all(); m_Thread.join();
}
void PlankCamera::acknowledge(std::uint64_t generation, std::uint32_t state)
{
    std::lock_guard<std::mutex> guard(m_Mutex);
    if (generation >= m_AckGeneration) { m_AckGeneration = generation; m_AckState = state; }
    m_Wake.notify_all();
}
void PlankCamera::requestKeyframe(std::uint64_t generation)
{
    std::lock_guard<std::mutex> guard(m_Mutex);
    m_KeyGeneration = generation; m_Wake.notify_all();
}
void PlankCamera::run()
{
#if !defined(Q_OS_LINUX) || !defined(PLANK_TRANSPORT)
    m_State.store(State::Unavailable);
#else
    using Clock = std::chrono::steady_clock;
    PlankLinuxNativeCamera camera;
    bool opened = false, configured = false, sentEnabled = false, failed = false, captured = false;
    bool lastRequested = m_Requested.load();
    std::uint64_t command = 0, sequence = 0;
    auto deadline = Clock::now(), lastFrame = Clock::now();
    const auto close = [&] { if (opened) camera.close(); opened = false; };
    std::vector<std::uint8_t> record;
    for (;;) {
        std::uint64_t ackGeneration, keyGeneration; std::uint32_t ackState;
        {
            std::unique_lock<std::mutex> lock(m_Mutex);
            if (m_Wake.wait_for(lock, std::chrono::milliseconds(5), [&] { return m_Stop; })) break;
            ackGeneration = m_AckGeneration; ackState = m_AckState; keyGeneration = m_KeyGeneration;
            m_KeyGeneration = 0;
        }
        const bool requested = m_Requested.load();
        if (requested != lastRequested) { failed = false; lastRequested = requested; }
        const bool enabled = requested && !failed;
        if (!enabled) close();
        if (!configured || enabled != sentEnabled) {
            close(); plank_transport_native_camera_activate(m_Endpoint, 0);
            if (command >= UINT64_MAX - 2) { m_State.store(State::Unavailable); break; }
            const auto next = command + 1;
            const std::uint32_t words[] = {std::uint32_t(next >> 32), std::uint32_t(next), enabled ? 1u : 0u};
            std::uint8_t packet[20]; size_t size = 0;
            plank_transport_control_encode(PLANK_TRANSPORT_CONTROL_SET_CAMERA, words, 3, packet, sizeof(packet), &size);
            if (plank_transport_native_data_send(m_Endpoint, packet, size) != PLANK_TRANSPORT_OK) {
                m_State.store(failed ? State::Unavailable : State::Pending); continue;
            }
            command = next; configured = true; sentEnabled = enabled; sequence = 0; captured = false;
            deadline = Clock::now() + std::chrono::seconds(5);
        }
        if (failed) { m_State.store(State::Unavailable); continue; }
        if (ackGeneration != command || ackState == PLANK_TRANSPORT_CAMERA_PENDING) {
            if (Clock::now() >= deadline) { failed = true; close(); }
            m_State.store(failed ? State::Unavailable : State::Pending); continue;
        }
        if (ackState == PLANK_TRANSPORT_CAMERA_UNAVAILABLE || plank_transport_native_camera_state(m_Endpoint) == 3) {
            failed = true; close(); m_State.store(State::Unavailable); continue;
        }
        if (!enabled) { m_State.store(State::Off); continue; }
        if (ackState != PLANK_TRANSPORT_CAMERA_ACTIVE) { failed = true; continue; }
        if (!opened) {
            const auto choices = enumerate();
            const auto chosen = m_Device.isEmpty() ? choices.begin() : std::find_if(choices.begin(), choices.end(),
                [&](const Device& device) { return device.path == m_Device; });
            if (chosen != choices.end()) {
                const auto path = QFile::encodeName(chosen->node);
                for (const auto codec : {PlankLinuxNativeCamera::Codec::H264, PlankLinuxNativeCamera::Codec::Mjpeg}) {
                    if (codec == PlankLinuxNativeCamera::Codec::H264 ? !chosen->h264 : !chosen->mjpeg) continue;
                    for (unsigned width : {1920u, 1280u}) {
                        if (camera.open(path.constData(), codec, width, width == 1920 ? 1080 : 720)) { opened = true; break; }
                    }
                    if (opened) break;
                }
            }
            if (!opened || plank_transport_native_camera_activate(m_Endpoint, command) != PLANK_TRANSPORT_OK) {
                qWarning() << "PLANK native camera capture is unavailable";
                failed = true; close(); continue;
            }
            camera.requestKeyframe(); lastFrame = Clock::now();
        }
        if (keyGeneration == command || plank_transport_native_camera_keyframe_needed(m_Endpoint) == command)
            camera.requestKeyframe();
        PlankLinuxNativeCamera::Frame frame;
        const auto result = camera.read(frame, 0);
        if (result == PlankLinuxNativeCamera::Result::Failed) { failed = true; close(); continue; }
        if (result == PlankLinuxNativeCamera::Result::Dropped) camera.requestKeyframe();
        if (result == PlankLinuxNativeCamera::Result::Frame) {
            const auto& format = camera.format();
            PlankCameraHeader header{};
            header.generation = command; header.sequence = sequence++; header.capture_time_us = frame.captureTimeUs;
            header.codec = format.pixelformat == V4L2_PIX_FMT_H264 ? PLANK_CAMERA_H264 : PLANK_CAMERA_MJPEG;
            header.width = std::uint16_t(format.width); header.height = std::uint16_t(format.height);
            header.colorspace = format.colorspace; header.transfer = format.xfer_func;
            header.ycbcr = format.ycbcr_enc; header.quantization = format.quantization;
            header.driver_sequence = frame.driverSequence;
            header.flags = (frame.independent ? PLANK_CAMERA_KEY_FRAME : 0) | (frame.discontinuity ? PLANK_CAMERA_DISCONTINUITY : 0);
            record.resize(PLANK_CAMERA_HEADER_BYTES + frame.bytes.size());
            if (plank_camera_header_encode(&header, frame.bytes.size(), record.data(), record.size())) { failed = true; continue; }
            std::copy(frame.bytes.begin(), frame.bytes.end(), record.begin() + PLANK_CAMERA_HEADER_BYTES);
            // Recheck camera-off/teardown after dequeue and before handing over.
            { std::lock_guard<std::mutex> guard(m_Mutex); if (m_Stop || !m_Requested.load()) continue; }
            const auto sent = plank_transport_native_camera_send(m_Endpoint, record.data(), record.size());
            if (sent != PLANK_TRANSPORT_OK && sent != PLANK_TRANSPORT_DROPPED && sent != PLANK_TRANSPORT_TIMEOUT) failed = true;
            if (sent != PLANK_TRANSPORT_OK) camera.requestKeyframe();
            lastFrame = Clock::now(); captured |= sent == PLANK_TRANSPORT_OK;
        }
        if (Clock::now() - lastFrame > std::chrono::seconds(2)) failed = true;
        if (failed) close();
        m_State.store(failed ? State::Unavailable : captured ? State::Active : State::Pending);
    }
    close(); plank_transport_native_camera_activate(m_Endpoint, 0);
#endif
}
