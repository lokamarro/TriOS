// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../../platform/descriptor.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <utility>

namespace uml::gpu {

/* Lets another thread stop a vhost-user server from handing requests to its
 * device, and later let them through again.
 *
 * Closing the gate does not touch the guest's rings: a request that arrives
 * while it is closed stays in its ring, unconsumed, whatever the ring's
 * enabled state does meanwhile, and runs in arrival order once the gate
 * opens. A guest that suspends and resumes while the gate is closed finds
 * every request it queued still waiting. Only the device calls the server
 * makes for a request are gated: a deferred request completing, a device's
 * event source and the control channel carry on.
 *
 * One thing ends a hold's promise: the guest itself ending. A guest that is
 * shutting down finishes its GPU only after its rings drain, and a closed gate
 * drains nothing, so a held guest cannot stop. discard() turns a closed gate
 * into a discarding one for the guest that is going away: the server consumes
 * each request, those already waiting and those that arrive, and completes it
 * with the device's answer for a request that did not run, without calling the
 * device. Only a closed gate can discard, so a guest that ends while the gate
 * is open runs everything it sent. The next guest finds a plain closed gate
 * again (returnToHold()).
 *
 * The server runs one thread; hold(), release(), discard() and
 * returnToHold() are for any other. */
class RequestGate final {
public:
    enum class State : std::uint8_t {
        open,
        held,
        discarding,
    };

    RequestGate() : wake_("request gate wake") {}

    RequestGate(const RequestGate&) = delete;
    RequestGate& operator=(const RequestGate&) = delete;

    /* Closes the gate. When this returns true no device call is running and
     * none starts until release(). When it returns false a call was still
     * running after `budget`; the gate is closed regardless, and the call
     * ends on its own. Closing a closed gate waits again for the same
     * thing. */
    [[nodiscard]] bool hold(std::chrono::milliseconds budget) noexcept
    {
        std::unique_lock lock(mutex_);
        if (state_.load(std::memory_order_relaxed) == State::open)
            state_.store(State::held, std::memory_order_release);
        return idle_.wait_for(lock, budget, [this] { return active_ == 0; });
    }

    /* Opens the gate, discarding or not, and wakes the server, which then
     * takes up the requests that waited. Opening an open gate does
     * nothing. */
    void release() noexcept
    {
        {
            std::lock_guard lock(mutex_);
            if (state_.exchange(State::open, std::memory_order_acq_rel) ==
                State::open) {
                return;
            }
        }
        (void)wake_.notify();
    }

    /* The guest this gate holds back is ending: has the server complete its
     * requests without running them, and wakes it to do so. Does nothing to
     * an open gate, which has held nothing back. Returns whether the gate is
     * discarding. */
    bool discard() noexcept
    {
        {
            std::lock_guard lock(mutex_);
            switch (state_.load(std::memory_order_relaxed)) {
            case State::open:
                return false;
            case State::discarding:
                return true;
            case State::held:
                state_.store(State::discarding, std::memory_order_release);
                break;
            }
        }
        (void)wake_.notify();
        return true;
    }

    /* A discarding gate becomes a plain closed one, for the guest that comes
     * next. Does nothing otherwise. */
    void returnToHold() noexcept
    {
        std::lock_guard lock(mutex_);
        if (state_.load(std::memory_order_relaxed) == State::discarding)
            state_.store(State::held, std::memory_order_release);
    }

    [[nodiscard]] State state() const noexcept
    {
        return state_.load(std::memory_order_acquire);
    }

    /* Closed, discarding or not. */
    [[nodiscard]] bool held() const noexcept
    {
        return state() != State::open;
    }

    [[nodiscard]] bool discarding() const noexcept
    {
        return state() == State::discarding;
    }

    /* A device call in progress. An entry that does not own the gate means
     * it is closed, and the call must not be made. */
    class Entry final {
    public:
        Entry() = default;
        Entry(const Entry&) = delete;
        Entry& operator=(const Entry&) = delete;
        Entry(Entry&& other) noexcept
            : gate_(std::exchange(other.gate_, nullptr))
        {
        }
        Entry& operator=(Entry&& other) noexcept
        {
            if (this != &other) {
                leave();
                gate_ = std::exchange(other.gate_, nullptr);
            }
            return *this;
        }
        ~Entry() { leave(); }

        [[nodiscard]] explicit operator bool() const noexcept
        {
            return gate_ != nullptr;
        }

    private:
        friend class RequestGate;
        explicit Entry(RequestGate& gate) noexcept : gate_(&gate) {}

        void leave() noexcept
        {
            if (gate_ != nullptr)
                gate_->leave();
            gate_ = nullptr;
        }

        RequestGate* gate_ {};
    };

    /* For the server. */
    [[nodiscard]] Entry enter() noexcept
    {
        std::lock_guard lock(mutex_);
        if (state_.load(std::memory_order_acquire) != State::open)
            return {};
        ++active_;
        return Entry {*this};
    }

    /* A descriptor that polls readable after release() or discard(), until
     * acknowledgeWake(). */
    [[nodiscard]] int wakeDescriptor() const noexcept
    {
        return wake_.descriptor();
    }

    void acknowledgeWake() const noexcept { (void)wake_.drain(); }

private:
    void leave() noexcept
    {
        {
            std::lock_guard lock(mutex_);
            --active_;
        }
        idle_.notify_all();
    }

    uml::platform::WakeSocket wake_;
    std::mutex mutex_;
    std::condition_variable idle_;
    std::atomic<State> state_ {State::open};
    unsigned active_ {};
};

} // namespace uml::gpu
