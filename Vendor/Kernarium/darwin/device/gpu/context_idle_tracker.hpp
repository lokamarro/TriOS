// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace uml::gpu {

/* Finds the guest contexts that have gone quiet.
 *
 * The host renderer's GL contexts keep what their last batches used until the
 * context submits again, which a compositor or a launcher with nothing to draw
 * does not do for minutes. A context is idle once a whole idle interval has
 * passed since its last activity (a command buffer, a transfer, a fence it
 * asked for, a fence that retired) and no fence it asked for is still
 * outstanding. It is reported twice, an idle interval apart: the trim of the
 * first report flushes the context, and the batch that flush submits has not
 * completed when the trim looks at it, so only the second trim can release
 * what that last batch holds. Any new activity makes it eligible again.
 * Resources the host drops for other contexts' sake bring two more
 * (noteRelease), for the same reason.
 *
 * Only contexts that were added are tracked: the guest's, and the renderer's
 * own context 0. Safe to call from any thread. */
class ContextIdleTracker {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr auto defaultIdleInterval = std::chrono::seconds(1);
    /* How often an idle context is reported before it is active again. */
    static constexpr unsigned reportsPerIdlePeriod = 2;
    /* How many idle intervals a context waits for the report that follows one
     * made for a resource drop, when that report found more drops: resource
     * churn costs an idle context a bounded number of trims and never keeps it
     * from its own. */
    static constexpr unsigned dirtyReportSpacing = 2;

    explicit ContextIdleTracker(
        Clock::duration idleInterval = defaultIdleInterval) noexcept;

    void add(std::uint32_t contextId, Clock::time_point now);
    void remove(std::uint32_t contextId);
    void reset();

    /* The context ran something on the host GL stack. */
    void activity(std::uint32_t contextId, Clock::time_point now);
    /* The host dropped something that other contexts may have used: a
     * resource the guest released, a resource a context let go of, a context.
     * A texture one context deletes can still have a sampler view that
     * another made, and a context destroys its own views only when it next
     * flushes, so a context that sampled it has something to release
     * although nothing was sent to it. Every context is marked, without its
     * idle period moving. A context that is marked when its idle reports are
     * made is reported an interval after the first drop and, because that
     * flush leaves what it frees to be released at the next trim, once more
     * an interval later; drops found by that second report bring a third,
     * `dirtyReportSpacing` intervals later, and so on until a report finds
     * none. */
    void noteRelease(Clock::time_point now);
    /* A fence on a ring of the context was asked for and later retired.
     * Cookies on a ring only grow, and retiring one retires all before it. */
    void fenceCreated(
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t cookie,
        Clock::time_point now);
    void fenceRetired(
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t cookie,
        Clock::time_point now);

    /* The contexts to trim at `now`. Each is reported at the end of its idle
     * interval and once more an interval later, then not again until it is
     * active or something was dropped. */
    [[nodiscard]] std::vector<std::uint32_t> takeIdle(Clock::time_point now);

    /* How long until a context is next due for a report, zero if one already
     * is, and nothing if no context is waiting for it: all of them have been
     * reported twice, or are waiting on a fence. */
    [[nodiscard]] std::optional<Clock::duration> nextDelay(
        Clock::time_point now) const;

private:
    struct Ring {
        std::uint64_t created {};
        std::uint64_t retired {};
    };
    struct Context {
        Clock::time_point lastActivity {};
        /* The reports since the last activity, and when the latest was made. */
        unsigned reports {};
        Clock::time_point reportedAt {};
        /* When something was first dropped since the last report. */
        std::optional<Clock::time_point> dirtySince;
        /* The intervals until the report the latest one owes, because it
         * found something dropped: zero if it owes none. */
        unsigned followUpIn {};
        std::map<std::uint32_t, Ring> rings;

        [[nodiscard]] bool hasPendingFence() const noexcept;
        /* When the next report is due, or nothing once the reports owed were
         * made and nothing was dropped since, or while a fence is
         * outstanding. */
        [[nodiscard]] std::optional<Clock::time_point> nextReport(
            Clock::duration idleInterval) const noexcept;
    };

    Clock::duration idleInterval_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint32_t, Context> contexts_;
};

} // namespace uml::gpu
