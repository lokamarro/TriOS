// SPDX-License-Identifier: GPL-2.0-or-later

/* The renderer trims a guest context once a whole idle interval has passed
 * since its last activity and no fence it asked for is outstanding, and once
 * more an interval later, when the batch the first trim flushed has completed.
 * The tracker decides that from the times it is told, so the test drives it
 * with its own clock: a context is reported twice and then not again, activity
 * of any kind or a resource the host dropped makes it eligible again for two
 * reports at a bounded rate, a fence outstanding holds it back
 * however long it takes, a retired fence starts the interval, contexts the
 * tracker was not told of are ignored, and the delay it names is the time to
 * the next context that could be reported. */

#include "context_idle_tracker.hpp"
#include "require.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

using uml::gpu::ContextIdleTracker;
using uml::testing::require;
using Clock = ContextIdleTracker::Clock;
using namespace std::chrono_literals;

namespace {

const Clock::time_point origin {Clock::duration {1'000'000'000'000}};
constexpr std::uint32_t first = 3;
constexpr std::uint32_t second = 9;
const std::vector<std::uint32_t> none;

void checkIdleInterval()
{
    ContextIdleTracker tracker(1s);
    tracker.add(first, origin);

    require(tracker.takeIdle(origin + 999ms) == none,
        "a context was idle before the interval had passed");
    require(tracker.nextDelay(origin) == 1s,
        "the delay was not the whole interval right after creation");
    require(tracker.nextDelay(origin + 400ms) == 600ms,
        "the delay did not shrink with time");

    require(tracker.takeIdle(origin + 1s) == std::vector {first},
        "a context was not idle after the interval");
    require(tracker.takeIdle(origin + 1500ms) == none,
        "a context was reported again before an interval had passed");
    require(tracker.takeIdle(origin + 10s) == std::vector {first},
        "a context was not reported a second time");
    require(tracker.takeIdle(origin + 60s) == none,
        "a context was reported three times");
    require(!tracker.nextDelay(origin + 60s).has_value(),
        "a context reported twice still had a delay");
}

void checkSecondReport()
{
    ContextIdleTracker tracker(1s);
    tracker.add(first, origin);
    require(tracker.takeIdle(origin + 1s) == std::vector {first},
        "the first report is missing");

    /* The second report is an interval after the first, not after the last
     * activity, and the delay says so. */
    require(tracker.nextDelay(origin + 1s) == 1s,
        "the delay after the first report was not an interval");
    require(tracker.nextDelay(origin + 1400ms) == 600ms,
        "the delay to the second report did not shrink with time");
    require(tracker.takeIdle(origin + 1999ms) == none,
        "the second report came before an interval had passed");
    require(tracker.takeIdle(origin + 2s) == std::vector {first},
        "the second report is missing");
    require(!tracker.nextDelay(origin + 2s).has_value(),
        "a context reported twice had a delay");

    /* A late poll reports from the time it ran: the second interval is
     * counted from the first report, not from when it was due. */
    tracker.activity(first, origin + 10s);
    require(tracker.takeIdle(origin + 13s) == std::vector {first},
        "the first report after activity is missing");
    require(tracker.takeIdle(origin + 13999ms) == none,
        "the second report came before an interval after a late first report");
    require(tracker.takeIdle(origin + 14s) == std::vector {first},
        "the second report after a late first report is missing");

    /* Activity between the reports starts the whole period again. */
    tracker.activity(first, origin + 20s);
    require(tracker.takeIdle(origin + 21s) == std::vector {first},
        "a context was not reported after activity");
    tracker.activity(first, origin + 21500ms);
    require(tracker.takeIdle(origin + 22s) == none,
        "activity between the reports did not restart the interval");
    require(tracker.takeIdle(origin + 22500ms) == std::vector {first},
        "the first report after the interrupted period is missing");
    require(tracker.takeIdle(origin + 23500ms) == std::vector {first},
        "the second report after the interrupted period is missing");
    require(tracker.takeIdle(origin + 60s) == none,
        "a third report was made");

    /* A fence asked for between the reports holds the second one back until
     * it retires, and the period starts again at the retirement. */
    tracker.activity(first, origin + 70s);
    require(tracker.takeIdle(origin + 71s) == std::vector {first},
        "a context was not reported before its fence");
    tracker.fenceCreated(first, 0, 1, origin + 71500ms);
    require(tracker.takeIdle(origin + 90s) == none,
        "a context with a fence outstanding was reported a second time");
    tracker.fenceRetired(first, 0, 1, origin + 90s);
    require(tracker.takeIdle(origin + 91s) == std::vector {first},
        "the first report after the retirement is missing");
    require(tracker.takeIdle(origin + 92s) == std::vector {first},
        "the second report after the retirement is missing");
}

void checkAReleaseBringsTwoMoreReports()
{
    ContextIdleTracker tracker(1s);
    tracker.add(first, origin);
    require(tracker.takeIdle(origin + 1s) == std::vector {first},
        "the first report is missing");
    require(tracker.takeIdle(origin + 2s) == std::vector {first},
        "the second report is missing");
    require(tracker.takeIdle(origin + 60s) == none, "a third report was made");

    /* Something the host dropped: a report an interval after the drop, which
     * frees what the drop left, and one an interval after that, which
     * releases it, and then no more. */
    tracker.noteRelease(origin + 70s);
    require(tracker.nextDelay(origin + 70s) == 1s,
        "a release did not bring a report an interval later");
    require(tracker.takeIdle(origin + 70999ms) == none,
        "a context was reported before the interval after a release");
    require(tracker.takeIdle(origin + 71s) == std::vector {first},
        "a release did not bring a report");
    require(tracker.nextDelay(origin + 71s) == 1s,
        "the report after a release did not bring the releasing one");
    require(tracker.takeIdle(origin + 71999ms) == none,
        "the releasing report came before its interval");
    require(tracker.takeIdle(origin + 72s) == std::vector {first},
        "a release did not bring a second report");
    require(tracker.takeIdle(origin + 100s) == none &&
            !tracker.nextDelay(origin + 100s).has_value(),
        "a release brought more than two reports");

    /* Drops the releasing report finds bring one more, the spacing later, and
     * drops it does not find bring none. */
    tracker.noteRelease(origin + 110s);
    require(tracker.takeIdle(origin + 111s) == std::vector {first},
        "the report after a drop is missing");
    tracker.noteRelease(origin + 111500ms);
    require(tracker.takeIdle(origin + 112s) == std::vector {first},
        "the releasing report is missing");
    require(tracker.nextDelay(origin + 112s) == 2s,
        "drops the second report found did not wait for the spacing");
    require(tracker.takeIdle(origin + 113999ms) == none,
        "a context was reported inside the spacing");
    require(tracker.takeIdle(origin + 114s) == std::vector {first},
        "the report after the spacing is missing");
    require(tracker.takeIdle(origin + 140s) == none &&
            !tracker.nextDelay(origin + 140s).has_value(),
        "reports went on after the last drop was released");

    /* A release is a drop for every context, and the first one since a
     * report is the one that counts: later drops do not move the report. */
    tracker.noteRelease(origin + 150s);
    tracker.noteRelease(origin + 150900ms);
    require(tracker.takeIdle(origin + 150999ms) == none,
        "a context was reported before the interval after the first drop");
    require(tracker.takeIdle(origin + 151s) == std::vector {first},
        "a later drop moved the report");

    /* A drop while the idle reports are still being made does not add to
     * them, apart from the one that releases what the last of them freed. */
    ContextIdleTracker early(1s);
    early.add(first, origin);
    require(early.takeIdle(origin + 1s) == std::vector {first},
        "the first report is missing");
    early.noteRelease(origin + 1500ms);
    require(early.takeIdle(origin + 2s) == std::vector {first},
        "the second report is missing");
    require(early.takeIdle(origin + 3s) == std::vector {first},
        "the report that releases the drop is missing");
    require(early.takeIdle(origin + 30s) == none &&
            !early.nextDelay(origin + 30s).has_value(),
        "a drop during the idle reports brought more than one");
}

void checkReleasesNeitherStarveNorFlood()
{
    constexpr auto step = 100ms;
    ContextIdleTracker tracker(1s);
    tracker.add(first, origin);
    tracker.add(second, origin);
    (void)tracker.takeIdle(origin + 1s);
    (void)tracker.takeIdle(origin + 2s);

    /* Resources are dropped ten times a second for ten seconds. A context
     * that went idle is reported the same moment it would be without them, and
     * a context that was idle all along is reported once every two intervals,
     * not more. */
    tracker.activity(second, origin + 30s);
    int firstReports = 0;
    Clock::time_point secondReport {};
    for (auto at = origin + 30100ms; at <= origin + 40s; at += step) {
        tracker.noteRelease(at);
        for (const auto id : tracker.takeIdle(at)) {
            if (id == first)
                ++firstReports;
            else if (secondReport == Clock::time_point {})
                secondReport = at;
        }
    }
    require(secondReport == origin + 31s,
        "resource churn held a context back from its own idle report");
    require(firstReports == 5,
        "an idle context was reported at the wrong rate during resource churn");

    /* After the churn each context is reported for the last drops, and again
     * to release what that flush freed, and then nothing is. */
    int firstAfter = 0;
    int secondAfter = 0;
    for (auto at = origin + 40100ms; at <= origin + 60s; at += step) {
        for (const auto id : tracker.takeIdle(at))
            ++(id == first ? firstAfter : secondAfter);
    }
    require(firstAfter >= 1 && firstAfter <= 2 && secondAfter >= 1 &&
            secondAfter <= 2,
        "the reports after resource churn were not the last ones");
    require(tracker.takeIdle(origin + 61s) == none &&
            !tracker.nextDelay(origin + 61s).has_value(),
        "a context was still due after the last reports");

    /* A fence outstanding holds a context back from a release's report too, and
     * a removed context is not reported. */
    tracker.fenceCreated(second, 0, 3, origin + 70s);
    tracker.noteRelease(origin + 71s);
    require(tracker.takeIdle(origin + 90s) == std::vector {first},
        "a context with a fence outstanding was reported after a release");
    tracker.remove(first);
    tracker.noteRelease(origin + 100s);
    require(tracker.takeIdle(origin + 120s) == none,
        "a removed context or one with a fence outstanding was reported");
    ContextIdleTracker empty(1s);
    empty.noteRelease(origin);
    require(empty.takeIdle(origin + 60s) == none &&
            !empty.nextDelay(origin).has_value(),
        "a release invented a context");
}

void checkActivityMakesTheContextEligibleAgain()
{
    ContextIdleTracker tracker(1s);
    tracker.add(first, origin);
    require(tracker.takeIdle(origin + 2s) == std::vector {first},
        "the first report is missing");

    tracker.activity(first, origin + 3s);
    require(tracker.nextDelay(origin + 3s) == 1s,
        "activity did not start a new interval");
    require(tracker.takeIdle(origin + 3999ms) == none,
        "a context was reported before its new interval ended");
    require(tracker.takeIdle(origin + 4s) == std::vector {first},
        "a context active again was not reported again");

    /* An old timestamp cannot move the last activity backwards. */
    tracker.activity(first, origin + 10s);
    tracker.activity(first, origin + 5s);
    require(tracker.takeIdle(origin + 10999ms) == none,
        "an earlier activity time shortened the interval");
}

void checkPendingFences()
{
    ContextIdleTracker tracker(1s);
    tracker.add(first, origin);
    tracker.fenceCreated(first, 0, 7, origin);
    require(tracker.takeIdle(origin + 60s) == none,
        "a context with a fence outstanding was reported");
    require(!tracker.nextDelay(origin + 60s).has_value(),
        "a context with a fence outstanding had a delay");

    /* The interval starts at the retirement, not at the submission. */
    tracker.fenceRetired(first, 0, 7, origin + 60s);
    require(tracker.nextDelay(origin + 60s) == 1s,
        "a retired fence did not start the interval");
    require(tracker.takeIdle(origin + 60999ms) == none,
        "a context was reported before the retired fence's interval");
    require(tracker.takeIdle(origin + 61s) == std::vector {first},
        "a context was not reported after its last fence retired");

    /* Rings are separate timelines, and a retired cookie retires all before. */
    tracker.fenceCreated(first, 1, 5, origin + 70s);
    tracker.fenceCreated(first, 2, 6, origin + 70s);
    tracker.fenceRetired(first, 1, 5, origin + 71s);
    require(tracker.takeIdle(origin + 90s) == none,
        "a context with a fence outstanding on another ring was reported");
    tracker.fenceCreated(first, 2, 9, origin + 71s);
    tracker.fenceRetired(first, 2, 9, origin + 72s);
    require(tracker.takeIdle(origin + 72999ms) == none,
        "the merged retirement did not start the interval");
    require(tracker.takeIdle(origin + 73s) == std::vector {first},
        "a retirement covering earlier cookies left a fence outstanding");

    /* A fence that retires inside the call that asked for it is recorded
     * retired first; it must not leave the context pending. */
    tracker.fenceRetired(first, 3, 11, origin + 80s);
    tracker.fenceCreated(first, 3, 11, origin + 80s);
    require(tracker.takeIdle(origin + 81s) == std::vector {first},
        "a fence recorded retired before created left the context pending");
}

void checkSeveralContexts()
{
    ContextIdleTracker tracker(1s);
    tracker.add(first, origin);
    tracker.add(second, origin + 600ms);
    require(tracker.nextDelay(origin) == 1s,
        "the delay was not the earliest context's");
    require(tracker.takeIdle(origin + 1s) == std::vector {first},
        "only the older context should have been reported");
    require(tracker.nextDelay(origin + 1s) == 600ms,
        "the delay did not move to the remaining context");
    require(tracker.takeIdle(origin + 1600ms) == std::vector {second},
        "the second context was not reported");

    tracker.activity(first, origin + 5s);
    tracker.activity(second, origin + 5s);
    require((tracker.takeIdle(origin + 6s) == std::vector {first, second}),
        "contexts were not reported together in id order");
    require((tracker.takeIdle(origin + 7s) == std::vector {first, second}),
        "contexts were not reported together a second time");

    tracker.remove(first);
    tracker.activity(first, origin + 7s);
    tracker.fenceCreated(first, 0, 1, origin + 7s);
    tracker.fenceRetired(first, 0, 1, origin + 7s);
    require(tracker.takeIdle(origin + 60s) == none,
        "a removed context, or one never added, was reported");

    tracker.add(first, origin + 60s);
    tracker.reset();
    require(tracker.takeIdle(origin + 120s) == none &&
            !tracker.nextDelay(origin + 120s).has_value(),
        "a reset left a context behind");
}

void checkThreads()
{
    /* Retirements come from whichever thread polls the renderer while the
     * service thread asks for idle contexts. */
    ContextIdleTracker tracker(1ms);
    tracker.add(first, Clock::now());
    std::thread retiring([&] {
        for (std::uint64_t cookie = 1; cookie <= 20000; ++cookie) {
            tracker.fenceCreated(first, 0, cookie, Clock::now());
            tracker.fenceRetired(first, 0, cookie, Clock::now());
        }
    });
    for (int round = 0; round < 2000; ++round) {
        (void)tracker.takeIdle(Clock::now());
        (void)tracker.nextDelay(Clock::now());
    }
    retiring.join();
    std::this_thread::sleep_for(5ms);
    require(tracker.takeIdle(Clock::now()) == std::vector {first},
        "the context was not idle once the threads were done");
}

} // namespace

int main()
{
    checkIdleInterval();
    checkSecondReport();
    checkAReleaseBringsTwoMoreReports();
    checkReleasesNeitherStarveNorFlood();
    checkActivityMakesTheContextEligibleAgain();
    checkPendingFences();
    checkSeveralContexts();
    checkThreads();
    return EXIT_SUCCESS;
}
