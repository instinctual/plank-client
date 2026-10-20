#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

// The frontend chooses bounded or continuous recovery. Individual operations
// retain their HTTP timeout, cancellation and terminal authentication checks.
// The ordinary frontend's Ask policy still pauses requests at its deadline.
class PlankReconnectPolicy
{
public:
    void allowUntil(uint64_t deadline) { m_Deadline.store(deadline); }
    void allowContinuously() { allowUntil(std::numeric_limits<uint64_t>::max()); }
    bool allowsRequest(uint64_t now) const { return now < m_Deadline.load(); }

    static bool terminalStatus(int status, bool authenticating)
    {
        return status == 403 || status == 423 ||
                (authenticating && status == 401) ||
                (status >= 400 && status < 500 && status != 401 &&
                 status != 409 && status != 425 && status != 429);
    }

private:
    std::atomic<uint64_t> m_Deadline {0};
};
