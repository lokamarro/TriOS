// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace uml::gpu {

/* When the host's graphics memory counts as under pressure, for a guest that
 * subscribed to hear of it (the Virtio-GPU memory report, kernel/common/gpu).
 *
 * The figure is the pool's own usage, the one a reservation is refused by, so
 * pressure is "within the margin of what the pool will refuse at", its
 * ceiling C, which is B less the pool's reserve:
 *
 *   usage + margin >= C
 *
 * The usage also holds memory the host can free itself, the caches of its
 * drivers, and the pool frees those before it refuses. Pressure is therefore
 * declared on the figure that is left once the host has done the same: when
 * the condition first holds the caller runs the host's reclaim (reclaimDue()
 * and reclaimed(), at most once per `reclaimInterval`), the figure is given
 * `settle` to show what it freed, and pressure begins once the condition
 * holds after that and has held for `sustain`. It ends once the condition has
 * been false for `calm`, so a spike that passes is not reported. A refused
 * allocation is an event of its own, raised at most once per
 * `refusalInterval`. An event is raised on a change of the pressure and on a
 * refusal, and never otherwise.
 *
 * The class keeps no clock and no timer: the caller reads the figures, passes
 * the time, runs the reclaim when it is due, and arms a timer for deadline()
 * when it returns one. It does nothing while nobody is subscribed. Not
 * thread-safe. */
class MemoryPressure {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr std::uint64_t margin = 96ULL * 1024ULL * 1024ULL;
    static constexpr Clock::duration sustain = std::chrono::seconds(1);
    static constexpr Clock::duration calm = std::chrono::seconds(5);
    static constexpr Clock::duration refusalInterval = std::chrono::seconds(1);
    /* How long the figure is given to show what a reclaim freed: the device
     * memory pool's own bound for its ledger to follow a release. */
    static constexpr Clock::duration settle = std::chrono::milliseconds(300);
    /* The least between two reclaims. */
    static constexpr Clock::duration reclaimInterval = std::chrono::seconds(1);

    /* The figures one evaluation reads, at one instant. */
    struct Reading {
        /* B; 0 when the process has no budget. */
        std::uint64_t budgetBytes {};
        /* The pool's usage: what a reservation is refused by. */
        std::uint64_t usageBytes {};
        /* Allocations refused so far. */
        std::uint64_t refusals {};
        /* C, the usage the pool refuses above; B when 0. */
        std::uint64_t ceilingBytes {};
    };

    /* What a reply reports. */
    struct State {
        bool pressure {};
        /* An allocation was refused since the previous reply. */
        bool refused {};
    };

    /* The guest subscribed. Nothing changes when it already has: every query
     * of an armed reclaimer carries the subscription. */
    void subscribe(const Reading& reading) noexcept;
    void unsubscribe() noexcept;
    [[nodiscard]] bool subscribed() const noexcept { return subscribed_; }

    /* The figures or the time changed. Returns whether the event is to be
     * raised now. */
    [[nodiscard]] bool evaluate(
        Clock::time_point now, const Reading& reading) noexcept;

    /* The reply to a query, evaluated at `now`: the state the guest is told,
     * so a change it carries needs no event. The refusals it reports are
     * consumed. */
    [[nodiscard]] State query(
        Clock::time_point now, const Reading& reading) noexcept;

    /* Whether the caller is to run the host's reclaim now: the condition holds
     * and the host has not tried to free memory since it began to. Read after
     * evaluate() or query(), which find whether the condition holds. */
    [[nodiscard]] bool reclaimDue(Clock::time_point now) const noexcept;
    /* The caller ran the reclaim at `now`; `freed` is whether it freed
     * anything. The figure is then given `settle` to show it. */
    void reclaimed(Clock::time_point now, bool freed) noexcept;

    /* When to evaluate again if nothing changes before, or nothing. A time
     * that has passed means now. */
    [[nodiscard]] std::optional<Clock::time_point> deadline() const noexcept;

    [[nodiscard]] bool pressure() const noexcept { return pressure_; }

private:
    /* Updates the condition, the pressure and the refusals, and returns
     * whether the pressure changed. */
    [[nodiscard]] bool step(
        Clock::time_point now, const Reading& reading) noexcept;

    bool subscribed_ {};
    bool high_ {};
    bool pressure_ {};
    bool queried_ {};
    bool refusalUnraised_ {};
    /* A reclaim has run since the condition began to hold. */
    bool reclaimedThisStretch_ {};
    std::uint64_t seenRefusals_ {};
    std::uint64_t queriedRefusals_ {};
    std::optional<Clock::time_point> highSince_;
    std::optional<Clock::time_point> calmSince_;
    std::optional<Clock::time_point> lastRefusalEvent_;
    std::optional<Clock::time_point> lastReclaim_;
    /* When the figure shows what the last reclaim freed. */
    Clock::time_point settledAt_ {};
};

} // namespace uml::gpu
