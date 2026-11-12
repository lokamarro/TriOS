// SPDX-License-Identifier: GPL-2.0-or-later

#include "deferred_fences.hpp"
#include "fence_deadline.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <stdexcept>
#include <sys/event.h>
#include <utility>

namespace uml::gpu {
namespace {

constexpr std::intptr_t fencePollMicroseconds = 100;

[[nodiscard]] bool changeFenceTimer(int queue, std::uint16_t flags)
{
    struct kevent change {};
    EV_SET(
        &change,
        fenceTimerIdentifier,
        EVFILT_TIMER,
        flags,
        NOTE_USECONDS,
        fencePollMicroseconds,
        nullptr);
    int result;
    do {
        result = ::kevent(queue, &change, 1, nullptr, 0, nullptr);
    } while (result < 0 && errno == EINTR);
    return result == 0;
}

} // namespace

DeferredFences::DeferredFences()
{
    pending_.reserve(protocol::maximumVirtqueueSize);
    completions_.reserve(protocol::maximumVirtqueueSize);
}

bool DeferredFences::full() const noexcept
{
    return pending_.size() + completions_.size() >=
        protocol::maximumVirtqueueSize;
}

void DeferredFences::reserveDefer()
{
    /* The constructor made room for every command a queue can hold, so a
     * deferral can only add its ring. */
    ringEscapes_.reserve(ringEscapes_.size() + 1);
}

void DeferredFences::defer(
    std::uint64_t requestId,
    const protocol::GpuControlHeader& requestHeader,
    std::vector<std::uint8_t> response,
    const DeferredFence& fence,
    Clock::time_point now)
{
    if (full())
        throw std::runtime_error("invalid deferred GPU fence");
    /* A ring that has stopped retiring has already been given the full
     * escape three times over and reported once. Keep polling the renderer
     * for it, so any retirement it does make revives the ring, but stop
     * buying it windows it has never used. */
    const bool deadRing =
        ringHasStoppedRetiring(escapesForRing(fence).consecutive);
    pending_.push_back(Pending {
        .requestId = requestId,
        .response = std::move(response),
        .fence = fence,
        .deadline = deadRing ? now : now + maximumFenceWait,
        .commandType = requestHeader.type,
        .orderingReleased = deadRing,
    });
}

void DeferredFences::forgetContext(std::uint32_t contextId)
{
    std::size_t completed = 0;
    for (std::size_t index = 0; index < pending_.size();) {
        if (pending_[index].fence.contextId != contextId) {
            ++index;
            continue;
        }
        complete(index);
        ++completed;
    }
    std::erase_if(ringEscapes_, [contextId](const RingEscapes& escapes) {
        return escapes.contextId == contextId;
    });
    if (pending_.empty())
        orderingRelaxed_ = false;
    if (completed != 0) {
        std::fprintf(
            stderr,
            "GPU context %u destroyed with %zu fences pending: completing "
            "them without the renderer\n",
            contextId,
            completed);
    }
}

bool DeferredFences::settle(
    std::size_t index,
    int status,
    Clock::time_point now)
{
    auto& pending = pending_[index];
    if (status == EBUSY) {
        auto& escapes = escapesForRing(pending.fence);
        const bool stopped = ringHasStoppedRetiring(escapes.consecutive);
        const auto action = classifyOverdueFence(
            now, pending.deadline, pending.orderingReleased);
        if (action == OverdueFenceAction::wait)
            return false;
        if (action == OverdueFenceAction::releaseOrdering) {
            pending.orderingReleased = true;
            orderingRelaxed_ = true;
            reportOverdueFence(pending, "releasing queue ordering");
            pending.deadline = now + maximumFenceWait;
            return false;
        }
        if (!stopped) {
            reportOverdueFence(pending, "retiring without the renderer");
            ++escapes.consecutive;
            if (ringHasStoppedRetiring(escapes.consecutive))
                reportRingStoppedRetiring(pending.fence);
        }
    } else if (status != 0) {
        throw std::runtime_error("reading GPU fence status failed");
    } else {
        auto& escapes = escapesForRing(pending.fence);
        if (ringHasStoppedRetiring(escapes.consecutive)) {
            std::fprintf(
                stderr,
                "GPU ring retired a fence again: context=%u ring=%u: "
                "the renderer is answering for it once more\n",
                pending.fence.contextId,
                pending.fence.ringIndex);
        }
        escapes.consecutive = 0;
    }
    complete(index);
    return true;
}

void DeferredFences::complete(std::size_t index)
{
    if (completions_.size() == protocol::maximumVirtqueueSize)
        throw std::runtime_error("GPU fence completion overflow");
    auto& pending = pending_[index];
    completions_.push_back(Completion {
        .requestId = pending.requestId,
        .response = std::move(pending.response),
    });
    pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(index));
}

bool DeferredFences::takeCompletion(Completion& completion)
{
    if (completions_.empty())
        return false;
    completion = std::move(completions_.front());
    completions_.erase(completions_.begin());
    return true;
}

void DeferredFences::clear() noexcept
{
    pending_.clear();
    completions_.clear();
    ringEscapes_.clear();
    orderingRelaxed_ = false;
}

DeferredFences::RingEscapes& DeferredFences::escapesForRing(
    const DeferredFence& fence)
{
    const auto existing = std::find_if(
        ringEscapes_.begin(),
        ringEscapes_.end(),
        [&fence](const RingEscapes& escapes) {
            return escapes.contextId == fence.contextId &&
                escapes.ringIndex == fence.ringIndex;
        });
    if (existing != ringEscapes_.end())
        return *existing;
    ringEscapes_.push_back(RingEscapes {
        .contextId = fence.contextId,
        .ringIndex = fence.ringIndex,
    });
    return ringEscapes_.back();
}

/* One line per transition, never per poll: this shares the guest console and
 * heavy logging is itself able to end a run. */
void DeferredFences::reportOverdueFence(
    const Pending& pending,
    const char* action) const noexcept
{
    std::fprintf(
        stderr,
        "GPU fence overdue: context=%u ring=%u command=0x%x: %s\n",
        pending.fence.contextId,
        pending.fence.ringIndex,
        pending.commandType,
        action);
}

/* Said once, when the ring crosses from one stuck submission into not
 * retiring at all, and said in full: the pairs above name a fence, and a
 * reader counting them has no way to learn that the guest is now being told
 * work completed that the renderer never did. */
void DeferredFences::reportRingStoppedRetiring(
    const DeferredFence& fence) const noexcept
{
    std::fprintf(
        stderr,
        "GPU ring stopped retiring: context=%u ring=%u completed %u fences "
        "in a row without the renderer. The renderer is not answering for "
        "this ring, so results the guest reads back from it cannot be "
        "trusted; later fences on it complete immediately and are not "
        "reported again\n",
        fence.contextId,
        fence.ringIndex,
        maximumRingEscapes);
}

void addFenceTimer(int queue)
{
    if (!changeFenceTimer(queue, EV_ADD | EV_DISABLE | EV_DISPATCH))
        throw std::runtime_error("adding GPU fence timer failed");
}

void armFenceTimer(int queue)
{
    /* Adding an existing knote modifies it and restarts its interval. */
    if (!changeFenceTimer(queue, EV_ADD | EV_ENABLE | EV_DISPATCH))
        throw std::runtime_error("arming GPU fence timer failed");
}

void cancelFenceTimer(int queue) noexcept
{
    (void)changeFenceTimer(queue, EV_DISABLE);
}

} // namespace uml::gpu
