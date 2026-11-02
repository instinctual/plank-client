#pragma once
#include <atomic>

// The existing session worker owns successful startup/recovery transitions.
// Callback threads may request recovery; any thread may cancel permanently.
// No later completion can revive a cancelled session.
class PlankSessionLifecycle
{
public:
    enum class State { Starting, Streaming, RecoveryQueued, Recovering, Stopping, Stopped };
    State state() const { return m_State.load(); }
    bool streaming() const { return state() == State::Streaming; }
    bool recovering() const { return state() == State::Recovering; }
    bool recoveryPending() const {
        const auto current = state();
        return current == State::RecoveryQueued || current == State::Recovering;
    }
    bool stopping() const {
        const auto current = state();
        return current == State::Stopping || current == State::Stopped;
    }
    bool started() { return transition(State::Starting, State::Streaming); }
    bool requestRecovery() { return transition(State::Streaming, State::RecoveryQueued); }
    bool beginRecovery() { return transition(State::RecoveryQueued, State::Recovering); }
    bool recovered() { return transition(State::Recovering, State::Streaming); }
    void stop() {
        auto current = state();
        while (current != State::Stopping && current != State::Stopped &&
               !m_State.compare_exchange_weak(current, State::Stopping)) {}
    }
    void finished() { m_State.store(State::Stopped); }
private:
    bool transition(State from, State to) { return m_State.compare_exchange_strong(from, to); }
    std::atomic<State> m_State{State::Starting};
};
