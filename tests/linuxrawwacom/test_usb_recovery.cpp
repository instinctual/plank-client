// Exercise the production adapter with an immutable fake udev topology and
// fault-injected device I/O. No physical device is opened or grabbed.
#include "../../app/streaming/input/linuxrawwacom.h"
#include <Limelight.h>
#include <plank.h>
#include <QtEndian>
#include <libudev.h>
#include <linux/hidraw.h>
#include <linux/input.h>
#include <linux/uhid.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>

using namespace std::chrono_literals;
#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "check failed at line " << __LINE__ << ": " #condition "\n"; \
    std::abort(); \
} } while (false)

struct udev_device {
    std::string subsystem, type, path, node;
    std::map<std::string, std::string> attributes;
    udev_device* parent = nullptr;
    int interfaceNumber = -1;
    unsigned product = 0x0358;
};
struct Tree { std::vector<std::unique_ptr<udev_device>> nodes; };
struct udev { std::shared_ptr<Tree> tree; };
struct udev_list_entry { udev_device* device; udev_list_entry* next; };
struct udev_enumerate {
    udev* context;
    std::string subsystem;
    std::vector<udev_list_entry> entries;
};

namespace {
std::mutex mutex;
std::condition_variable changed;
std::shared_ptr<Tree> current;
// Borrowed udev node pointers remain valid through a simulated unplug.
std::vector<std::shared_ptr<Tree>> trees;
struct Handle {
    const udev_device* device;
    bool grabbed = false;
    std::deque<std::vector<unsigned char>> reports;
};
std::map<int, Handle> handles;
struct Frame {
    std::uint16_t type, generation, interfaceId;
    std::vector<unsigned char> payload;
};
std::vector<Frame> frames;
enum class Fault { None, Hup, Error, Invalid, PollError, PollInterrupt,
                   ReadEof, ReadError, ReadInterrupt, ReadAgain };
Fault pollFault = Fault::None, readFault = Fault::None;
unsigned faultsConsumed = 0, discoveryCount = 0, duplicateCount = 0;

template<class Predicate> void waitFor(Predicate predicate)
{
    std::unique_lock<std::mutex> lock(mutex);
    CHECK(changed.wait_for(lock, 3s, predicate));
}

unsigned count(std::uint16_t type)
{
    return static_cast<unsigned>(std::count_if(frames.begin(), frames.end(),
        [=](const Frame& frame) { return frame.type == type; }));
}

unsigned grabs()
{
    return static_cast<unsigned>(std::count_if(handles.begin(), handles.end(),
        [](const auto& handle) { return handle.second.grabbed; }));
}

udev_device* add(Tree& tree, const std::string& subsystem, const std::string& type,
                 const std::string& path, udev_device* parent = nullptr,
                 const std::string& node = {}, int number = -1, unsigned product = 0x0358)
{
    auto device = std::make_unique<udev_device>();
    device->subsystem = subsystem;
    device->type = type;
    device->path = path;
    device->node = node;
    device->parent = parent;
    device->interfaceNumber = number;
    device->product = product;
    tree.nodes.push_back(std::move(device));
    return tree.nodes.back().get();
}

void topology(bool renumbered, bool missingPen = false, bool invalidNumber = false,
              unsigned product = 0x0358, bool missingPenEvents = false)
{
    auto tree = std::make_shared<Tree>();
    auto usb = add(*tree, "usb", "usb_device", "/sys/plank-fixture/1-1");
    usb->attributes = {{"idVendor", "056a"}, {"idProduct", "0358"}};
    // Noncontiguous USB interface numbers, enumerated in reverse order.
    for (int number : {2, 0}) {
        auto interface = add(*tree, "usb", "usb_interface",
            usb->path + ":1." + std::to_string(number), usb);
        interface->attributes = {{"bInterfaceClass", "03"},
            {"bInterfaceNumber", invalidNumber && number == 0 ? "invalid" :
                (number == 0 ? "00" : "02")}};
        if (missingPen && number == 0) continue;
        const std::string suffix = number == 0 ? (renumbered ? "7" : "0") : "1";
        add(*tree, "hidraw", "", interface->path + "/hidraw" + suffix,
            interface, "/dev/plank-fixture-hidraw" + suffix, number, product);
        if (missingPenEvents && number == 0) continue;
        add(*tree, "input", "", interface->path + "/event" + suffix,
            interface, "/dev/input/event-plank-fixture" + suffix, number, product);
    }
    std::lock_guard<std::mutex> lock(mutex);
    trees.push_back(tree);
    current = std::move(tree);
    changed.notify_all();
}

void control(LinuxRawWacomInput& raw, std::uint16_t generation, std::uint16_t type)
{
    PLANK_RAW_HID_WIRE_HEADER header{};
    header.magic = qToLittleEndian<std::uint32_t>(PLANK_RAW_HID_WIRE_MAGIC);
    header.version = qToLittleEndian<std::uint16_t>(PLANK_RAW_HID_WIRE_VERSION);
    header.type = qToLittleEndian(type);
    header.generation = qToLittleEndian(generation);
    const unsigned int length = type == PLANK_RAW_HID_GET_REPORT ? 2 : sizeof(std::int32_t);
    header.payloadLength = qToLittleEndian<std::uint32_t>(length);
    std::array<unsigned char, sizeof(header) + sizeof(std::int32_t)> data{};
    std::memcpy(data.data(), &header, sizeof(header));
    if (type == PLANK_RAW_HID_GET_REPORT) data[sizeof(header) + 1] = UHID_FEATURE_REPORT;
    raw.handleControl(data.data(), sizeof(header) + length);
}

std::vector<Frame> attachment(unsigned ordinal)
{
    waitFor([&] { return count(PLANK_RAW_HID_DESCRIPTOR) >= ordinal * 2; });
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<Frame> result;
    unsigned seen = 0;
    for (const auto& frame : frames) {
        if (frame.type == PLANK_RAW_HID_DEVICE) ++seen;
        if (seen == ordinal && (frame.type == PLANK_RAW_HID_DEVICE ||
                               frame.type == PLANK_RAW_HID_DESCRIPTOR)) result.push_back(frame);
    }
    CHECK(result.size() == 3);
    CHECK(result[0].payload.size() == sizeof(PLANK_RAW_HID_DEVICE_MESSAGE));
    CHECK(result[1].interfaceId == 0 && result[1].payload.back() == 0);
    CHECK(result[2].interfaceId == 1 && result[2].payload.back() == 2);
    return result;
}

void acknowledge(LinuxRawWacomInput& raw, const std::vector<Frame>& attach)
{
    control(raw, attach.front().generation, PLANK_RAW_HID_ATTACH_RESULT);
    waitFor([] { return grabs() == 2; });
}

void inject(Fault fault)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (pollFault != Fault::None || readFault != Fault::None) {
        std::cerr << "Unconsumed test fault before " << static_cast<int>(fault)
                  << ": poll=" << static_cast<int>(pollFault)
                  << " read=" << static_cast<int>(readFault) << '\n';
    }
    CHECK(pollFault == Fault::None && readFault == Fault::None);
    pollFault = fault;
    changed.notify_all();
}

void penReport(unsigned marker)
{
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& [fd, handle] : handles) {
        (void)fd;
        if (handle.device->subsystem == "hidraw" && handle.device->interfaceNumber == 0) {
            handle.reports.push_back({0x10, static_cast<unsigned char>(marker)});
            changed.notify_all();
            return;
        }
    }
    CHECK(false);
}
}

extern "C" {
udev* __wrap_udev_new()
{
    std::lock_guard<std::mutex> lock(mutex);
    ++discoveryCount;
    changed.notify_all();
    return new udev{current};
}
udev* __wrap_udev_unref(udev* context) { delete context; return nullptr; }
udev_enumerate* __wrap_udev_enumerate_new(udev* context)
{ return new udev_enumerate{context, {}, {}}; }
udev_enumerate* __wrap_udev_enumerate_unref(udev_enumerate* enumerate)
{ delete enumerate; return nullptr; }
int __wrap_udev_enumerate_add_match_subsystem(udev_enumerate* enumerate, const char* subsystem)
{ enumerate->subsystem = subsystem; return 0; }
int __wrap_udev_enumerate_scan_devices(udev_enumerate* enumerate)
{
    for (auto& node : enumerate->context->tree->nodes) {
        if (node->subsystem == enumerate->subsystem) enumerate->entries.push_back({node.get(), nullptr});
    }
    for (std::size_t i = 1; i < enumerate->entries.size(); ++i)
        enumerate->entries[i - 1].next = &enumerate->entries[i];
    return 0;
}
udev_list_entry* __wrap_udev_enumerate_get_list_entry(udev_enumerate* enumerate)
{ return enumerate->entries.empty() ? nullptr : enumerate->entries.data(); }
udev_list_entry* __wrap_udev_list_entry_get_next(udev_list_entry* entry) { return entry->next; }
const char* __wrap_udev_list_entry_get_name(udev_list_entry* entry) { return entry->device->path.c_str(); }
udev_device* __wrap_udev_device_new_from_syspath(udev* context, const char* path)
{
    for (auto& node : context->tree->nodes) if (node->path == path) return node.get();
    return nullptr;
}
udev_device* __wrap_udev_device_unref(udev_device*) { return nullptr; }
udev_device* __wrap_udev_device_get_parent_with_subsystem_devtype(
    udev_device* device, const char* subsystem, const char* type)
{
    for (auto parent = device->parent; parent; parent = parent->parent)
        if (parent->subsystem == subsystem && parent->type == type) return parent;
    return nullptr;
}
const char* __wrap_udev_device_get_syspath(udev_device* device) { return device->path.c_str(); }
const char* __wrap_udev_device_get_devnode(udev_device* device)
{ return device->node.empty() ? nullptr : device->node.c_str(); }
const char* __wrap_udev_device_get_devtype(udev_device* device) { return device->type.c_str(); }
const char* __wrap_udev_device_get_sysattr_value(udev_device* device, const char* name)
{
    auto attr = device->attributes.find(name);
    return attr == device->attributes.end() ? nullptr : attr->second.c_str();
}

int __real_open(const char*, int, ...);
int __real_close(int);
int __real_fcntl(int, int, ...);
int __real_poll(pollfd*, nfds_t, int);
ssize_t __real_read(int, void*, size_t);
int __real_ioctl(int, unsigned long, ...);

int __wrap_open(const char* path, int flags, ...)
{
    if (std::strstr(path, "plank-fixture") == nullptr) {
        mode_t mode = 0;
        if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = va_arg(args, int); va_end(args); }
        return __real_open(path, flags, mode);
    }
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& node : current->nodes) {
        if (node->node != path) continue;
        int fd = __real_open("/dev/null", O_RDWR | O_CLOEXEC);
        CHECK(fd >= 0);
        handles.emplace(fd, Handle{node.get(), false, {}});
        return fd;
    }
    errno = ENOENT;
    return -1;
}
int __wrap_close(int fd)
{
    std::lock_guard<std::mutex> lock(mutex);
    handles.erase(fd);
    changed.notify_all();
    return __real_close(fd);
}
int __wrap_fcntl(int fd, int command, ...)
{
    CHECK(command == F_DUPFD_CLOEXEC);
    va_list args; va_start(args, command); int minimum = va_arg(args, int); va_end(args);
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(handles.count(fd) == 1);
    ++duplicateCount;
    int duplicate = __real_fcntl(fd, command, minimum);
    CHECK(duplicate >= 0);
    handles.emplace(duplicate, handles.at(fd));
    return duplicate;
}
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    va_list args; va_start(args, request);
    const int grab = request == EVIOCGRAB ? va_arg(args, int) : 0;
    void* data = request == EVIOCGRAB ? nullptr : va_arg(args, void*);
    va_end(args);
    std::lock_guard<std::mutex> lock(mutex);
    auto found = handles.find(fd);
    if (found == handles.end()) return __real_ioctl(fd, request, data);
    auto& handle = found->second;
    const auto& device = *handle.device;
    if (request == EVIOCGRAB) {
        handle.grabbed = grab != 0;
        changed.notify_all();
    }
    else if (request == HIDIOCGRAWINFO) {
        auto info = static_cast<hidraw_devinfo*>(data);
        *info = {}; info->bustype = BUS_USB; info->vendor = 0x056a; info->product = device.product;
    }
    else if (request == HIDIOCGRDESCSIZE) *static_cast<int*>(data) = 2;
    else if (request == HIDIOCGRDESC) {
        auto descriptor = static_cast<hidraw_report_descriptor*>(data);
        descriptor->size = 2; descriptor->value[0] = 0xa0;
        descriptor->value[1] = static_cast<unsigned char>(device.interfaceNumber);
    }
    else if (request == EVIOCGID) {
        auto id = static_cast<input_id*>(data);
        *id = {}; id->version = 0x100;
    }
    else if (request == EVIOCGBIT(EV_KEY, (KEY_MAX + 8) / 8)) {
        std::memset(data, 0, (KEY_MAX + 8) / 8);
        if (device.interfaceNumber == 0)
            static_cast<unsigned char*>(data)[BTN_TOOL_PEN / 8] |= 1u << (BTN_TOOL_PEN % 8);
    }
    else if (request == HIDIOCGRAWNAME(128) || request == HIDIOCGRAWPHYS(64) ||
             request == HIDIOCGRAWUNIQ(64)) {
        std::string value = "fixture-interface-" + std::to_string(device.interfaceNumber);
        std::memcpy(data, value.c_str(), value.size() + 1);
    }
    else { errno = ENOTTY; return -1; }
    return 0;
}
int __wrap_poll(pollfd* fds, nfds_t size, int timeout)
{
    std::unique_lock<std::mutex> lock(mutex);
    if (!size || !handles.count(fds[0].fd)) {
        lock.unlock(); return __real_poll(fds, size, timeout);
    }
    changed.wait_for(lock, std::chrono::milliseconds(timeout), [&] {
        if (pollFault != Fault::None) return true;
        for (nfds_t i = 0; i < size; ++i) if (!handles.at(fds[i].fd).reports.empty()) return true;
        return false;
    });
    int ready = 0;
    for (nfds_t i = 0; i < size; ++i) {
        fds[i].revents = handles.at(fds[i].fd).reports.empty() ? 0 : POLLIN;
        if (fds[i].revents) ++ready;
    }
    auto fault = pollFault;
    pollFault = Fault::None;
    if (fault != Fault::None) {
        ++faultsConsumed;
        changed.notify_all();
        if (fault == Fault::PollError || fault == Fault::PollInterrupt) {
            errno = fault == Fault::PollError ? EIO : EINTR; return -1;
        }
        if (fault == Fault::Hup) fds[0].revents = POLLHUP;
        else if (fault == Fault::Error) fds[0].revents = POLLERR;
        else if (fault == Fault::Invalid) fds[0].revents = POLLNVAL;
        else { fds[0].revents = POLLIN; readFault = fault; }
        return 1;
    }
    return ready;
}
ssize_t __wrap_read(int fd, void* data, size_t size)
{
    std::unique_lock<std::mutex> lock(mutex);
    if (!handles.count(fd)) { lock.unlock(); return __real_read(fd, data, size); }
    auto fault = readFault;
    readFault = Fault::None;
    if (fault == Fault::ReadEof) return 0;
    if (fault != Fault::None) {
        errno = fault == Fault::ReadError ? ENODEV : fault == Fault::ReadInterrupt ? EINTR : EAGAIN;
        return -1;
    }
    auto& reports = handles.at(fd).reports;
    if (reports.empty()) { errno = EAGAIN; return -1; }
    auto report = std::move(reports.front()); reports.pop_front();
    CHECK(size >= report.size());
    std::memcpy(data, report.data(), report.size());
    return report.size();
}
// With _FORTIFY_SOURCE=3 the sanitizer build may call checked libc entry
// points instead of read/poll. Preserve their bounds checks and route them
// through the same fixture; otherwise /dev/null can masquerade as a USB EOF.
ssize_t __wrap___read_chk(int fd, void* data, size_t size, size_t capacity)
{
    CHECK(size <= capacity);
    return __wrap_read(fd, data, size);
}
int __wrap___poll_chk(pollfd* fds, nfds_t size, int timeout, size_t capacity)
{
    CHECK(size <= capacity / sizeof(pollfd));
    return __wrap_poll(fds, size, timeout);
}
int LiSendRawHidEvent(const unsigned char* data, unsigned int size)
{
    CHECK(size >= sizeof(PLANK_RAW_HID_WIRE_HEADER));
    PLANK_RAW_HID_WIRE_HEADER header;
    std::memcpy(&header, data, sizeof(header));
    CHECK(qFromLittleEndian(header.payloadLength) == size - sizeof(header));
    std::lock_guard<std::mutex> lock(mutex);
    frames.push_back({qFromLittleEndian(header.type), qFromLittleEndian(header.generation),
        qFromLittleEndian(header.interfaceId), {data + sizeof(header), data + size}});
    changed.notify_all();
    return 0;
}
}

int main()
{
    topology(false);
    {
        LinuxRawWacomInput raw([] {});
        raw.setActive(true);
        auto first = attachment(1);
        acknowledge(raw, first);
        // Only the finger interface survives a driver rebind at first. Do not
        // publish that partial tablet, even though it has an input event node.
        topology(true, true);
        inject(Fault::Hup);
        waitFor([] { return count(PLANK_RAW_HID_SUSPEND) == 1 && handles.empty(); });
        {
            std::unique_lock<std::mutex> lock(mutex);
            const auto before = discoveryCount;
            CHECK(changed.wait_for(lock, 3s, [&] { return discoveryCount >= before + 2; }));
            CHECK(count(PLANK_RAW_HID_DEVICE) == 1);
            CHECK(count(PLANK_RAW_HID_DETACH) == 0);
        }
        // The HID descriptor can arrive before the pen's evdev node. A
        // surviving touch input node alone must not make this group ready.
        topology(true, false, false, 0x0358, true);
        {
            std::unique_lock<std::mutex> lock(mutex);
            const auto before = discoveryCount;
            CHECK(changed.wait_for(lock, 3s, [&] { return discoveryCount >= before + 2; }));
            CHECK(count(PLANK_RAW_HID_DEVICE) == 1);
        }
        // Invalid identity must also fail closed, never invent an ordering.
        topology(true, false, true);
        {
            std::unique_lock<std::mutex> lock(mutex);
            const auto before = discoveryCount;
            CHECK(changed.wait_for(lock, 3s, [&] { return discoveryCount >= before + 2; }));
            CHECK(count(PLANK_RAW_HID_DEVICE) == 1);
        }
        topology(true);
        auto resumed = attachment(2);
        CHECK(first.front().generation != resumed.front().generation);
        for (std::size_t i = 0; i < first.size(); ++i) CHECK(first[i].payload == resumed[i].payload);
        control(raw, first.front().generation, PLANK_RAW_HID_ATTACH_RESULT);
        control(raw, first.front().generation, PLANK_RAW_HID_GET_REPORT);
        {
            std::unique_lock<std::mutex> lock(mutex);
            CHECK(!changed.wait_for(lock, 30ms, [] { return grabs() != 0 || duplicateCount != 0; }));
        }
        acknowledge(raw, resumed);
        unsigned ordinal = 2, suspendCount = 1;
        for (auto fault : {Fault::Error, Fault::Invalid, Fault::PollError, Fault::ReadEof, Fault::ReadError}) {
            inject(fault);
            ++suspendCount;
            waitFor([&] { return count(PLANK_RAW_HID_SUSPEND) == suspendCount; });
            resumed = attachment(++ordinal);
            for (std::size_t i = 0; i < first.size(); ++i) CHECK(first[i].payload == resumed[i].payload);
            acknowledge(raw, resumed);
        }
        unsigned inputs = 0;
        for (auto fault : {Fault::PollInterrupt, Fault::ReadInterrupt, Fault::ReadAgain}) {
            unsigned before;
            { std::lock_guard<std::mutex> lock(mutex); before = faultsConsumed; }
            inject(fault);
            waitFor([&] { return faultsConsumed > before; });
            penReport(++inputs);
            waitFor([&] { return count(PLANK_RAW_HID_INPUT) == inputs; });
            std::lock_guard<std::mutex> lock(mutex);
            CHECK(count(PLANK_RAW_HID_DEVICE) == ordinal);
            CHECK(count(PLANK_RAW_HID_SUSPEND) == suspendCount);
            const auto& input = frames.back();
            CHECK(input.type == PLANK_RAW_HID_INPUT && input.interfaceId == 0);
            CHECK(input.payload == std::vector<unsigned char>({0x10, static_cast<unsigned char>(inputs)}));
        }
        // Keep the reconnect barrier and focus lifecycle after physical recovery.
        for (unsigned cycle = 0; cycle < 10; ++cycle) {
            raw.beginReconnect();
            { std::lock_guard<std::mutex> lock(mutex); CHECK(handles.empty()); }
            raw.finishReconnect();
            resumed = attachment(++ordinal);
            acknowledge(raw, resumed);
        }
        // A genuinely changed tablet still reaches the Host with changed
        // metadata; the Host's retained-endpoint gate decides when to recreate.
        topology(false, false, false, 0x0357);
        inject(Fault::Hup);
        auto replacement = attachment(++ordinal);
        CHECK(replacement.front().payload != first.front().payload);
        acknowledge(raw, replacement);
        raw.setActive(false);
        std::lock_guard<std::mutex> lock(mutex);
        CHECK(handles.empty());
        CHECK(count(PLANK_RAW_HID_DETACH) == 0);
    }
    { std::lock_guard<std::mutex> lock(mutex); CHECK(handles.empty()); }
    std::cout << "Production Wacom USB recovery, stable identity, complete groups, stale controls and fd cleanup passed\n";
}
