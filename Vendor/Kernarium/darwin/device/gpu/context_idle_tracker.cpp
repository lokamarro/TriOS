// SPDX-License-Identifier: GPL-2.0-or-later

#include "context_idle_tracker.hpp"

#include <algorithm>

namespace uml::gpu {

bool ContextIdleTracker::Context::hasPendingFence() const noexcept
{
    return std::ranges::any_of(rings, [](const auto& ring) {
        return ring.second.created > ring.second.retired;
    });
}

std::optional<ContextIdleTracker::Clock::time_point>
ContextIdleTracker::Context::nextReport(
    Clock::duration idleInterval) const noexcept
{
    if (hasPendingFence())
        return std::nullopt;
    if (reports == 0)
        return lastActivity + idleInterval;
    if (reports < reportsPerIdlePeriod)
        return reportedAt + idleInterval;
    if (followUpIn != 0)
        return reportedAt + followUpIn * idleInterval;
    if (!dirtySince)
        return std::nullopt;
    return *dirtySince + idleInterval;
}

ContextIdleTracker::ContextIdleTracker(Clock::duration idleInterval) noexcept
    : idleInterval_(idleInterval)
{
}

void ContextIdleTracker::add(std::uint32_t contextId, Clock::time_point now)
{
    std::lock_guard lock(mutex_);
    contexts_[contextId] = Context {.lastActivity = now};
}

void ContextIdleTracker::remove(std::uint32_t contextId)
{
    std::lock_guard lock(mutex_);
    contexts_.erase(contextId);
}

void ContextIdleTracker::reset()
{
    std::lock_guard lock(mutex_);
    contexts_.clear();
}

void ContextIdleTracker::activity(
    std::uint32_t contextId, Clock::time_point now)
{
    std::lock_guard lock(mutex_);
    const auto found = contexts_.find(contextId);
    if (found == contexts_.end())
        return;
    found->second.lastActivity = std::max(found->second.lastActivity, now);
    found->second.reports = 0;
    found->second.followUpIn = 0;
}

void ContextIdleTracker::noteRelease(Clock::time_point now)
{
    std::lock_guard lock(mutex_);
    for (auto& entry : contexts_) {
        if (!entry.second.dirtySince)
            entry.second.dirtySince = now;
    }
}

void ContextIdleTracker::fenceCreated(
    std::uint32_t contextId,
    std::uint32_t ringIndex,
    std::uint64_t cookie,
    Clock::time_point now)
{
    std::lock_guard lock(mutex_);
    const auto found = contexts_.find(contextId);
    if (found == contexts_.end())
        return;
    auto& ring = found->second.rings[ringIndex];
    ring.created = std::max(ring.created, cookie);
    found->second.lastActivity = std::max(found->second.lastActivity, now);
    found->second.reports = 0;
    found->second.followUpIn = 0;
}

void ContextIdleTracker::fenceRetired(
    std::uint32_t contextId,
    std::uint32_t ringIndex,
    std::uint64_t cookie,
    Clock::time_point now)
{
    std::lock_guard lock(mutex_);
    const auto found = contexts_.find(contextId);
    if (found == contexts_.end())
        return;
    auto& ring = found->second.rings[ringIndex];
    ring.retired = std::max(ring.retired, cookie);
    /* A retirement the tracker was not told of (a fence of a ring it never saw
     * created) cannot leave a fence pending. */
    ring.created = std::max(ring.created, ring.retired);
    found->second.lastActivity = std::max(found->second.lastActivity, now);
    found->second.reports = 0;
    found->second.followUpIn = 0;
}

std::vector<std::uint32_t> ContextIdleTracker::takeIdle(Clock::time_point now)
{
    std::vector<std::uint32_t> idle;
    std::lock_guard lock(mutex_);
    for (auto& [contextId, context] : contexts_) {
        const auto due = context.nextReport(idleInterval_);
        if (!due || now < *due)
            continue;
        /* A report made because something was dropped, after the idle ones,
         * does not start an idle period of its own. What it finds dropped it
         * frees, and the report after it releases: the next one is an
         * interval away, or the spacing if this was already such a report. */
        const bool dropped = context.dirtySince.has_value();
        const bool followUp =
            context.followUpIn != 0 && context.reports >= reportsPerIdlePeriod;
        if (context.reports < reportsPerIdlePeriod)
            ++context.reports;
        context.reportedAt = now;
        context.dirtySince.reset();
        context.followUpIn = !dropped ? 0 : followUp ? dirtyReportSpacing : 1;
        idle.push_back(contextId);
    }
    std::ranges::sort(idle);
    return idle;
}

std::optional<ContextIdleTracker::Clock::duration>
ContextIdleTracker::nextDelay(Clock::time_point now) const
{
    std::optional<Clock::duration> delay;
    std::lock_guard lock(mutex_);
    for (const auto& entry : contexts_) {
        const auto due = entry.second.nextReport(idleInterval_);
        if (!due)
            continue;
        const auto wait = std::max(*due - now, Clock::duration::zero());
        if (!delay || wait < *delay)
            delay = wait;
    }
    return delay;
}

} // namespace uml::gpu
