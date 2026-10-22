#pragma once
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

// Process-wide and cross-process ownership for cooperating Mac Clients and
// Relay hosts. IOHID exclusive open remains the last gate for older Clients.
// Held by the physical worker, never a UI object: a stalled close cannot hand
// ownership to another session before the old interfaces really close.
class MacWacomCaptureLease {
public:
    ~MacWacomCaptureLease() { release(); }
    MacWacomCaptureLease() = default;
    MacWacomCaptureLease(const MacWacomCaptureLease&) = delete;
    MacWacomCaptureLease& operator=(const MacWacomCaptureLease&) = delete;
    bool acquire(const std::string& directory = productionDirectory()) {
        if (fd_ >= 0) return true;
        if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) return false;
        const int dir = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        struct stat st{};
        if (dir < 0) return false;
        if (::fstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
            (st.st_mode & 0777) != 0700) { ::close(dir); return false; }
        const int candidate = ::openat(dir, "capture.lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
        ::close(dir);
        if (candidate < 0) return false;
        if (::fstat(candidate, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
            (st.st_mode & 0777) != 0600 || st.st_nlink != 1 ||
            ::flock(candidate, LOCK_EX | LOCK_NB) != 0) { ::close(candidate); return false; }
        fd_ = candidate;
        return true;
    }
    void release() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
    static std::string productionDirectory() {
        return "/private/tmp/plank-tablet-capture-" + std::to_string(geteuid());
    }
private:
    int fd_ = -1;
};
