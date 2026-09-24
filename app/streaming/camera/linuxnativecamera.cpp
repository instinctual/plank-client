// SPDX-License-Identifier: GPL-3.0-or-later
#include "linuxnativecamera.h"
#include "nativecameraframe.h"
#include "uvch264control.h"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <linux/uvcvideo.h>
#include <linux/usb/video.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

namespace {
int control(int fd, unsigned long request, void* argument)
{
    int result;
    do { result = ioctl(fd, request, argument); } while (result < 0 && errno == EINTR);
    return result;
}

std::uint8_t h264Unit(dev_t device)
{
    std::error_code error;
    auto path = std::filesystem::canonical("/sys/dev/char/" + std::to_string(major(device)) +
        ":" + std::to_string(minor(device)) + "/device", error);
    if (error) return 0;
    unsigned interface = 256;
    for (unsigned depth = 0; depth < 10 && path != path.root_path(); ++depth, path = path.parent_path()) {
        if (interface == 256) {
            std::ifstream number(path / "bInterfaceNumber");
            unsigned value = 256;
            if (number >> std::hex >> value && value <= 255) interface = value;
        }
        std::ifstream descriptors(path / "descriptors", std::ios::binary);
        if (!descriptors) continue;
        unsigned configuration = 0;
        std::ifstream number(path / "bConfigurationValue");
        if (interface > 255 || !(number >> configuration)) return 0;
        std::array<std::uint8_t, 65536> bytes {};
        descriptors.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        if (!descriptors.eof() || descriptors.gcount() <= 0) return 0;
        return plankUvcH264Unit(bytes.data(), std::size_t(descriptors.gcount()), configuration, interface);
    }
    return 0;
}
}

PlankLinuxNativeCamera::~PlankLinuxNativeCamera() { close(); }

bool PlankLinuxNativeCamera::open(const char* path, Codec codec, unsigned width, unsigned height)
{
    if (m_Fd >= 0 || !path ||
        !((width == 1280 && height == 720) || (width == 1920 && height == 1080))) return false;
    m_Fd = ::open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (m_Fd < 0) return false;
    const auto fail = [&] { close(); return false; };
    struct stat stat {};
    v4l2_capability capabilities {};
    if (fstat(m_Fd, &stat) || !S_ISCHR(stat.st_mode) || control(m_Fd, VIDIOC_QUERYCAP, &capabilities)) return fail();
    m_UvcUnit = codec == Codec::H264 ? h264Unit(stat.st_rdev) : 0;
    m_LastKeyRequest = {};
    const auto caps = (capabilities.capabilities & V4L2_CAP_DEVICE_CAPS) ?
        capabilities.device_caps : capabilities.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING) ||
        (caps & V4L2_CAP_VIDEO_M2M)) return fail();
    const auto fourcc = codec == Codec::H264 ? V4L2_PIX_FMT_H264 : V4L2_PIX_FMT_MJPEG;
    bool native = false;
    for (unsigned index = 0; index < 256; ++index) {
        v4l2_fmtdesc descriptor {};
        descriptor.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; descriptor.index = index;
        if (control(m_Fd, VIDIOC_ENUM_FMT, &descriptor)) break;
        if (descriptor.pixelformat == fourcc) {
            native = (descriptor.flags & V4L2_FMT_FLAG_COMPRESSED) &&
                !(descriptor.flags & V4L2_FMT_FLAG_EMULATED);
            break;
        }
    }
    if (!native) return fail();
    m_PreviousFormat = {}; m_PreviousFormat.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    m_PreviousParameters = {}; m_PreviousParameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (control(m_Fd, VIDIOC_G_FMT, &m_PreviousFormat) ||
        control(m_Fd, VIDIOC_G_PARM, &m_PreviousParameters)) return fail();
    v4l2_format selected {}; selected.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    selected.fmt.pix.width = width; selected.fmt.pix.height = height;
    selected.fmt.pix.pixelformat = fourcc; selected.fmt.pix.field = V4L2_FIELD_NONE;
    // Busy devices fail here. Never stop a stream owned by another descriptor.
    if (control(m_Fd, VIDIOC_S_FMT, &selected)) return fail();
    m_Changed = true;
    if (control(m_Fd, VIDIOC_G_FMT, &selected)) return fail();
    m_Format = selected.fmt.pix;
    if (m_Format.width != width || m_Format.height != height || m_Format.pixelformat != fourcc ||
        m_Format.field != V4L2_FIELD_NONE || !m_Format.sizeimage ||
        m_Format.sizeimage > PlankNativeCameraFrame::MaxBytes || m_Format.colorspace > 12 ||
        m_Format.xfer_func > 7 || m_Format.ycbcr_enc > 8 || m_Format.quantization > 2) return fail();
    v4l2_streamparm parameters {}; parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parameters.parm.capture.timeperframe = {1, 30};
    if (control(m_Fd, VIDIOC_S_PARM, &parameters) || control(m_Fd, VIDIOC_G_PARM, &parameters)) return fail();
    const auto interval = parameters.parm.capture.timeperframe;
    if (!interval.numerator || std::uint64_t(interval.numerator) * 30 != interval.denominator) return fail();
    v4l2_requestbuffers request {};
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; request.memory = V4L2_MEMORY_MMAP; request.count = 4;
    if (control(m_Fd, VIDIOC_REQBUFS, &request)) return fail();
    m_Allocated = true;
    if (request.count < 2 || request.count > m_Mappings.size()) return fail();
    m_Count = request.count;
    for (unsigned index = 0; index < m_Count; ++index) {
        v4l2_buffer buffer {}; buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP; buffer.index = index;
        if (control(m_Fd, VIDIOC_QUERYBUF, &buffer) || !buffer.length ||
            buffer.length > PlankNativeCameraFrame::MaxBytes || buffer.length < m_Format.sizeimage) return fail();
        void* address = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, m_Fd, buffer.m.offset);
        if (address == MAP_FAILED) return fail();
        m_Mappings[index] = {address, buffer.length};
        if (control(m_Fd, VIDIOC_QBUF, &buffer)) return fail();
    }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (control(m_Fd, VIDIOC_STREAMON, &type)) return fail();
    m_Streaming = true; m_Codec = codec;
    m_HaveSequence = false; m_Discontinuity = true; m_CaptureTime = 0;
    return true;
}

PlankLinuxNativeCamera::Result PlankLinuxNativeCamera::read(Frame& frame, unsigned waitMs)
{
    frame = {};
    if (!m_Streaming) return Result::Failed;
    pollfd descriptor {m_Fd, POLLIN, 0};
    const int result = poll(&descriptor, 1, int(std::min(waitMs, 50u)));
    if (!result || (result < 0 && errno == EINTR)) return Result::Again;
    if (result < 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) { close(); return Result::Failed; }
    if (!(descriptor.revents & POLLIN)) return Result::Again;
    v4l2_buffer buffer {}; buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; buffer.memory = V4L2_MEMORY_MMAP;
    if (control(m_Fd, VIDIOC_DQBUF, &buffer)) {
        if (errno == EAGAIN) return Result::Again;
        close(); return Result::Failed;
    }
    if (buffer.index >= m_Count || !buffer.bytesused || buffer.bytesused > m_Mappings[buffer.index].size ||
        buffer.bytesused > PlankNativeCameraFrame::MaxBytes ||
        (buffer.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) != V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC ||
        buffer.timestamp.tv_sec < 0 || buffer.timestamp.tv_usec < 0 || buffer.timestamp.tv_usec >= 1000000 ||
        std::uint64_t(buffer.timestamp.tv_sec) > std::uint64_t(INT64_MAX) / 1000000000) {
        close(); return Result::Failed;
    }
    const auto capture = std::uint64_t(buffer.timestamp.tv_sec) * 1000000 + buffer.timestamp.tv_usec;
    timespec now {};
    if (clock_gettime(CLOCK_MONOTONIC, &now) || !capture || capture > std::uint64_t(INT64_MAX) / 1000) {
        close(); return Result::Failed;
    }
    const auto current = std::uint64_t(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
    bool dropped = (buffer.flags & V4L2_BUF_FLAG_ERROR) || capture <= m_CaptureTime ||
        capture > current + 5000 || (current > capture && current - capture > 150000);
    // Driver sequence is diagnostic as well as a conservative discontinuity
    // signal. It is not the transport frame counter or proof of coded loss.
    if (m_HaveSequence && buffer.sequence != std::uint32_t(m_Sequence + 1)) m_Discontinuity = true;
    m_Sequence = buffer.sequence; m_HaveSequence = true;
    const auto bytes = static_cast<const std::uint8_t*>(m_Mappings[buffer.index].address);
    bool key = m_Codec == Codec::Mjpeg;
    if (!dropped) dropped = m_Codec == Codec::H264 ?
        !PlankNativeCameraFrame::h264(bytes, buffer.bytesused, key) :
        !PlankNativeCameraFrame::mjpeg(bytes, buffer.bytesused, m_Format.width, m_Format.height);
    if (!dropped) {
        frame.bytes.assign(bytes, bytes + buffer.bytesused);
        frame.captureTimeUs = capture; frame.driverSequence = buffer.sequence;
        frame.independent = key; frame.discontinuity = m_Discontinuity;
        m_CaptureTime = capture;
    }
    if (control(m_Fd, VIDIOC_QBUF, &buffer)) { frame = {}; close(); return Result::Failed; }
    m_Discontinuity = dropped;
    return dropped ? Result::Dropped : Result::Frame;
}

bool PlankLinuxNativeCamera::requestKeyframe()
{
    if (!m_Streaming) return false;
    if (m_Codec == Codec::Mjpeg) return true;
    const auto now = std::chrono::steady_clock::now();
    if (m_LastKeyRequest != std::chrono::steady_clock::time_point {} &&
        now - m_LastKeyRequest < std::chrono::milliseconds(500)) return false;
    m_LastKeyRequest = now;
    v4l2_queryctrl query {}; query.id = V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME;
    if (!control(m_Fd, VIDIOC_QUERYCTRL, &query) && query.type == V4L2_CTRL_TYPE_BUTTON &&
        !(query.flags & (V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_READ_ONLY))) {
        v4l2_control value {}; value.id = query.id;
        return control(m_Fd, VIDIOC_S_CTRL, &value) == 0;
    }
    if (!m_UvcUnit) return false;
    std::uint8_t info = 0, length[2] = {};
    uvc_xu_control_query extension {};
    extension.unit = m_UvcUnit; extension.selector = 9;
    extension.query = UVC_GET_INFO; extension.size = 1; extension.data = &info;
    if (control(m_Fd, UVCIOC_CTRL_QUERY, &extension) || (info & 3) != 3) return false;
    extension.query = UVC_GET_LEN; extension.size = sizeof(length); extension.data = length;
    if (control(m_Fd, UVCIOC_CTRL_QUERY, &extension) || length[0] != 4 || length[1]) return false;
    // Little-endian wLayerID=0, wPicType=2 (IDR with its own SPS/PPS).
    // No configuration commit/reset, persistent control mapping or bitrate write.
    std::uint8_t request[] = {0, 0, 2, 0};
    extension.query = UVC_SET_CUR; extension.size = sizeof(request); extension.data = request;
    return control(m_Fd, UVCIOC_CTRL_QUERY, &extension) == 0;
}

bool PlankLinuxNativeCamera::close()
{
    if (m_Fd < 0) return true;
    bool okay = true;
    if (m_Streaming) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (control(m_Fd, VIDIOC_STREAMOFF, &type)) okay = false;
    }
    m_Streaming = false;
    for (auto& mapping : m_Mappings) {
        if (mapping.address && munmap(mapping.address, mapping.size)) okay = false;
        mapping = {};
    }
    if (m_Allocated) {
        v4l2_requestbuffers request {}; request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        request.memory = V4L2_MEMORY_MMAP;
        if (control(m_Fd, VIDIOC_REQBUFS, &request)) okay = false;
    }
    if (m_Changed) {
        if (control(m_Fd, VIDIOC_S_FMT, &m_PreviousFormat)) okay = false;
        if (control(m_Fd, VIDIOC_S_PARM, &m_PreviousParameters)) okay = false;
    }
    if (::close(m_Fd)) okay = false;
    m_Fd = -1; m_Changed = false; m_Allocated = false; m_Count = 0;
    return okay;
}
