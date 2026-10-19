#include "videomailbox.h"

#include <QThread>
#include <mutex>

struct QtVideoMailbox::State {
    std::mutex mutex;
    QtVideoMailbox* owner = nullptr; // only used while holding mutex
    QtVideoFrame::Ptr pending;
    uint64_t generation = 0;
    uint64_t submitted = 0;
    uint64_t replaced = 0;
    bool notificationQueued = false;
};

QtVideoMailbox::QtVideoMailbox(std::function<void(QtVideoFrame::Ptr)> consumer,
                               QObject* parent)
    : QObject(parent), m_State(std::make_shared<State>()), m_Consumer(std::move(consumer))
{
    m_State->owner = this;
}

QtVideoMailbox::~QtVideoMailbox()
{
    Q_ASSERT(QThread::currentThread() == thread());
    QtVideoFrame::Ptr retired;
    {
        std::lock_guard<std::mutex> lock(m_State->mutex);
        m_State->owner = nullptr;
        retired = std::move(m_State->pending);
    }
    // QObject removes queued drain calls. A Producer that acquired State
    // earlier now sees owner=null; one that arrives later has only a weak ref.
    // Release frame/device refs outside the mutex, including on retirement.
}

QtVideoMailbox::Producer QtVideoMailbox::startGeneration()
{
    retire();
    Producer producer;
    producer.m_State = m_State;
    producer.m_Generation = m_State->generation; // generation changes only on this GUI thread
    return producer;
}

void QtVideoMailbox::retire()
{
    Q_ASSERT(QThread::currentThread() == thread());
    QtVideoFrame::Ptr retired;
    {
        std::lock_guard<std::mutex> lock(m_State->mutex);
        ++m_State->generation;
        retired = std::move(m_State->pending);
        // Keep an already queued notification: it can drain the next
        // generation too. Reconnect churn must not accumulate Qt events.
    }
}

bool QtVideoMailbox::Producer::submit(QtVideoFrame::Ptr frame) const
{
    if (!frame) return false;
    auto state = m_State.lock();
    if (!state) return false;
    QtVideoFrame::Ptr retired;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->owner || state->generation != m_Generation) return false;
        if (state->pending) ++state->replaced;
        retired = std::move(state->pending);
        state->pending = std::move(frame);
        ++state->submitted;
        if (!state->notificationQueued) {
            auto* owner = state->owner;
            // Posting and destructor retirement share the same gate. The
            // explicit queued connection never invokes GUI work under it.
            state->notificationQueued = QMetaObject::invokeMethod(owner,
                [owner] { owner->drain(); }, Qt::QueuedConnection);
            if (!state->notificationQueued) {
                frame = std::move(state->pending);
                return false;
            }
        }
    }
    return true;
}

void QtVideoMailbox::drain()
{
    Q_ASSERT(QThread::currentThread() == thread());
    QtVideoFrame::Ptr frame;
    {
        std::lock_guard<std::mutex> lock(m_State->mutex);
        m_State->notificationQueued = false;
        frame = std::move(m_State->pending);
    }
    // Delivery may synchronously close its session and delete this mailbox.
    // Keep the callable alive independently and do not touch this afterward.
    const auto consumer = m_Consumer;
    if (frame && consumer) consumer(std::move(frame));
}

QtVideoMailbox::Stats QtVideoMailbox::stats() const
{
    std::lock_guard<std::mutex> lock(m_State->mutex);
    return {m_State->submitted, m_State->replaced, bool(m_State->pending)};
}
