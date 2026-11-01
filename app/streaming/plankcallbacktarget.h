#pragma once

#include <mutex>

// The session owner destroys the target after clearing the callback lease.
// Worker callbacks borrow it only while holding this gate. clear() waits for
// an in-flight callback and rejects later ones before the owner destroys it.
template<class T>
class PlankCallbackTarget
{
public:
    void publish(T* target)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Target = target;
    }

    void clear() { publish(nullptr); }

    template<class Callback>
    void invoke(Callback callback)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (m_Target != nullptr) callback(*m_Target);
    }

private:
    std::mutex m_Mutex;
    T* m_Target = nullptr;
};
