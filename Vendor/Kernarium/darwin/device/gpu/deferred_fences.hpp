// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "protocol.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace uml::gpu {

/* The renderer fence a deferred control command completes behind: the ring
 * that queued it and the host cookie the renderer retires. */
struct DeferredFence {
    std::uint32_t contextId {};
    std::uint32_t ringIndex {};
    std::uint64_t cookie {};
};

/* One connection's control commands that wait on a renderer fence, and what
 * the connection has learned about each ring's retirement.
 *
 * Everything held here belongs to a live context. Destroying a context takes
 * its renderer fence state with it, so the connection forgets the context in
 * the same step: its held commands complete, since nothing is left to retire
 * them, and its rings' escape counts go, so the next context the guest gives
 * the same ID starts with a ring that has never failed. */
class DeferredFences {
public:
    using Clock = std::chrono::steady_clock;

    /* One window of the two-stage escape in fence_deadline.hpp. A renderer
     * fence covers a single Virgl submission, which takes milliseconds; a
     * host that kills a GPU workload for running too long kills it in well
     * under a second. So this bounds a fence that is stuck, not one that is
     * slow, and the only thing a longer window buys is a longer stall. It
     * costs two windows to clear each stuck fence, and a Vulkan CTS group
     * whose shaders exceed the device's command-buffer limit produced
     * fourteen of them in twelve cases. Fifteen seconds still leaves three
     * orders of magnitude over a real submission. */
    static constexpr auto maximumFenceWait = std::chrono::seconds(15);

    struct Completion {
        std::uint64_t requestId {};
        std::vector<std::uint8_t> response;
    };

    DeferredFences();

    /* No command is waiting on the renderer. */
    [[nodiscard]] bool empty() const noexcept
    {
        return pending_.empty();
    }

    /* Held commands and completions the transport has not taken yet together
     * fill the largest virtqueue, so another deferral has to wait. */
    [[nodiscard]] bool full() const noexcept;

    /* A held fence has spent its first window, so queue ordering no longer
     * holds later commands back. */
    [[nodiscard]] bool orderingRelaxed() const noexcept
    {
        return orderingRelaxed_;
    }

    /* Makes room for one more defer(), so that it allocates nothing. The
     * transport holds a command only after the device has run it, and a
     * failure then would leave the command committed with nothing held to
     * answer the guest. So it calls this before the device runs a command
     * that may defer; it throws bad_alloc with nothing changed. */
    void reserveDefer();

    void defer(
        std::uint64_t requestId,
        const protocol::GpuControlHeader& requestHeader,
        std::vector<std::uint8_t> response,
        const DeferredFence& fence,
        Clock::time_point now);

    /* The guest destroyed this context. */
    void forgetContext(std::uint32_t contextId);

    /* Calls poll(contextId) once for each context with a held command. */
    template <typename Poll>
    void forEachContext(Poll&& poll) const
    {
        for (std::size_t index = 0; index < pending_.size(); ++index) {
            const auto contextId = pending_[index].fence.contextId;
            bool seen = false;
            for (std::size_t earlier = 0; earlier < index && !seen; ++earlier)
                seen = pending_[earlier].fence.contextId == contextId;
            if (!seen)
                poll(contextId);
        }
    }

    /* Completes each held command whose fence the renderer has retired, or
     * that has spent both of its windows. status(fence) answers 0 for a
     * retired fence and EBUSY for a busy one; any other answer is a renderer
     * failure and throws. */
    template <typename Status>
    void retire(Clock::time_point now, Status&& status)
    {
        for (std::size_t index = 0; index < pending_.size();) {
            if (!settle(index, status(pending_[index].fence), now))
                ++index;
        }
        if (pending_.empty())
            orderingRelaxed_ = false;
    }

    [[nodiscard]] bool takeCompletion(Completion& completion);
    void clear() noexcept;

private:
    struct Pending {
        std::uint64_t requestId {};
        std::vector<std::uint8_t> response;
        DeferredFence fence;
        Clock::time_point deadline;
        /* The held command's type, which an overdue report names. */
        std::uint32_t commandType {};
        /* Whether this fence has already spent its ordering-release window.
         * Connection-wide relaxation is a consequence of some fence escaping,
         * not evidence that this one has had its second chance. */
        bool orderingReleased {};
    };

    /* How many fences in a row this ring has completed without the renderer.
     * A ring is few per connection: one per Venus guest queue, and one for
     * every Virgl context. */
    struct RingEscapes {
        std::uint32_t contextId {};
        std::uint32_t ringIndex {};
        unsigned consecutive {};
    };

    /* Whether the held command at index left the queue. */
    [[nodiscard]] bool settle(
        std::size_t index,
        int status,
        Clock::time_point now);
    void complete(std::size_t index);
    [[nodiscard]] RingEscapes& escapesForRing(const DeferredFence& fence);
    void reportOverdueFence(
        const Pending& pending,
        const char* action) const noexcept;
    void reportRingStoppedRetiring(const DeferredFence& fence) const noexcept;

    std::vector<Pending> pending_;
    std::vector<Completion> completions_;
    std::vector<RingEscapes> ringEscapes_;
    /* Set when an overdue fence releases queue ordering, and cleared once
     * every held command has completed. */
    bool orderingRelaxed_ {};
};

/* The device's poll for held fences and renderer work: a timer on the
 * device's kqueue that fires once, shortly after it is armed.
 *
 * The server arms it after the device has run a command, when a failure
 * would leave the command committed and fail the connection. So the timer's
 * knote is added once, disabled, with the queue, and arming, firing and
 * cancelling only modify it, which allocates nothing in the kernel: a
 * dispatched timer is disabled rather than deleted when it fires. Once the
 * knote is added, only a broken queue fails to arm it. */
inline constexpr std::uintptr_t fenceTimerIdentifier = 1;
void addFenceTimer(int queue);
void armFenceTimer(int queue);
void cancelFenceTimer(int queue) noexcept;

} // namespace uml::gpu
