#pragma once

#include <QDeadlineTimer>
#include <QMutex>
#include <QThread>
#include <QWaitCondition>

// Bookmark workers are single-use. Publish stop and wake under the same lock
// so cancellation cannot be lost between checking the predicate and waiting.
class PollingThread : public QThread
{
public:
    void stop()
    {
        QMutexLocker lock(&m_Mutex);
        m_Stopping = true;
        m_Changed.wakeAll();
    }

    bool stopping() const
    {
        QMutexLocker lock(&m_Mutex);
        return m_Stopping;
    }

protected:
    void waitForNextPoll(unsigned long milliseconds)
    {
        QMutexLocker lock(&m_Mutex);
        const QDeadlineTimer deadline(milliseconds);
        while (!m_Stopping && !deadline.hasExpired()) {
            m_Changed.wait(&m_Mutex, deadline);
        }
    }

private:
    mutable QMutex m_Mutex;
    QWaitCondition m_Changed;
    bool m_Stopping = false;
};
