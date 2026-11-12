// SPDX-License-Identifier: GPL-2.0-or-later

#include "memory_pressure.hpp"

#include <algorithm>

namespace uml::gpu {

void MemoryPressure::subscribe(const Reading& reading) noexcept
{
    if (subscribed_)
        return;
    *this = {};
    subscribed_ = true;
    seenRefusals_ = reading.refusals;
    queriedRefusals_ = reading.refusals;
    queried_ = true;
}

void MemoryPressure::unsubscribe() noexcept
{
    *this = {};
}

bool MemoryPressure::step(
    Clock::time_point now, const Reading& reading) noexcept
{
    /* Compared as "usage >= C - margin" so that no sum can overflow. */
    const std::uint64_t ceiling = reading.ceilingBytes != 0
        ? std::min(reading.ceilingBytes, reading.budgetBytes)
        : reading.budgetBytes;
    const std::uint64_t threshold = ceiling - std::min(ceiling, margin);
    high_ = reading.budgetBytes != 0 && reading.usageBytes >= threshold;

    bool changed = false;
    if (high_) {
        calmSince_.reset();
        if (!highSince_)
            highSince_ = now;
        /* Only a figure the host's own reclaim has left, and has had time to
         * show, counts as pressure. */
        if (!pressure_ && reclaimedThisStretch_ && now >= settledAt_ &&
            now - *highSince_ >= sustain) {
            pressure_ = true;
            changed = true;
        }
    } else {
        highSince_.reset();
        reclaimedThisStretch_ = false;
        if (pressure_) {
            if (!calmSince_)
                calmSince_ = now;
            if (now - *calmSince_ >= calm) {
                pressure_ = false;
                calmSince_.reset();
                changed = true;
            }
        }
    }

    if (reading.refusals > seenRefusals_)
        refusalUnraised_ = true;
    seenRefusals_ = reading.refusals;
    return changed;
}

bool MemoryPressure::evaluate(
    Clock::time_point now, const Reading& reading) noexcept
{
    if (!subscribed_)
        return false;
    bool raise = step(now, reading);
    if (refusalUnraised_ &&
        (!lastRefusalEvent_ || now - *lastRefusalEvent_ >= refusalInterval)) {
        raise = true;
        refusalUnraised_ = false;
        lastRefusalEvent_ = now;
    }
    return raise;
}

MemoryPressure::State MemoryPressure::query(
    Clock::time_point now, const Reading& reading) noexcept
{
    if (subscribed_)
        (void)step(now, reading);
    State state {.pressure = pressure_};
    if (queried_ && reading.refusals > queriedRefusals_)
        state.refused = true;
    queriedRefusals_ = reading.refusals;
    queried_ = true;
    /* The reply tells the guest of the refusals, so no event follows. */
    refusalUnraised_ = false;
    return state;
}

bool MemoryPressure::reclaimDue(Clock::time_point now) const noexcept
{
    return subscribed_ && high_ && !pressure_ && !reclaimedThisStretch_ &&
        (!lastReclaim_ || now - *lastReclaim_ >= reclaimInterval);
}

void MemoryPressure::reclaimed(Clock::time_point now, bool freed) noexcept
{
    if (!subscribed_)
        return;
    lastReclaim_ = now;
    reclaimedThisStretch_ = true;
    settledAt_ = freed ? now + settle : now;
}

std::optional<MemoryPressure::Clock::time_point> MemoryPressure::deadline()
    const noexcept
{
    if (!subscribed_)
        return std::nullopt;
    std::optional<Clock::time_point> earliest;
    const auto consider = [&](Clock::time_point when) {
        if (!earliest || when < *earliest)
            earliest = when;
    };
    if (high_ && !pressure_ && highSince_) {
        if (!reclaimedThisStretch_) {
            /* The reclaim is due, or due once the interval since the last
             * one has passed. */
            consider(lastReclaim_
                    ? std::max(*highSince_, *lastReclaim_ + reclaimInterval)
                    : *highSince_);
        } else {
            consider(std::max(*highSince_ + sustain, settledAt_));
        }
    }
    if (!high_ && pressure_ && calmSince_)
        consider(*calmSince_ + calm);
    if (refusalUnraised_ && lastRefusalEvent_)
        consider(*lastRefusalEvent_ + refusalInterval);
    return earliest;
}

} // namespace uml::gpu
