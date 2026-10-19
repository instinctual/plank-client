#pragma once

#include "videoframe.h"
#include <QObject>
#include <functional>
#include <memory>

// One pending decoded frame and one queued GUI notification, not one Qt event
// per video frame. Decoding never waits for the GUI, and old connection workers
// cannot deliver into a replacement session. The consumer runs on the owner
// thread and must not block or queue another unbounded series of frame events.
class QtVideoMailbox final : public QObject
{
    struct State;
public:
    class Producer {
    public:
        bool submit(QtVideoFrame::Ptr frame) const;
        Producer restart() const; // decoder startup/reconnect; no GUI wait
        void discard() const;     // retires this generation only
        bool isCurrent() const;
    private:
        friend class QtVideoMailbox;
        std::weak_ptr<State> m_State;
        uint64_t m_Generation = 0;
    };

    struct Stats { uint64_t submitted, replaced; bool pending; };
    explicit QtVideoMailbox(std::function<void(QtVideoFrame::Ptr)> consumer,
                            QObject* parent = nullptr);
    explicit QtVideoMailbox(std::function<void(QtVideoFrame::Ptr, Producer)> consumer,
                            QObject* parent = nullptr);
    ~QtVideoMailbox() override;
    Producer startGeneration(); // GUI thread only; invalidates previous Producer copies
    void retire();              // GUI thread only; does not wait for a decoder
    Stats stats() const;

private:
    void drain();
    const std::shared_ptr<State> m_State;
    const std::function<void(QtVideoFrame::Ptr, Producer)> m_Consumer;
};
