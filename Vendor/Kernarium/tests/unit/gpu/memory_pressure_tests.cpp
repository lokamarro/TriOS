// SPDX-License-Identifier: GPL-2.0-or-later

/* The host tells a guest reclaimer when its graphics memory budget is under
 * pressure. The evaluator decides from the figures it is given and the time it
 * is told: pressure needs the pool's usage to come within the margin of the
 * budget, the host's own reclaim to have run since and had time to show what
 * it freed, and the condition to have held for a whole second; it ends after
 * five calm seconds; memory the reclaim frees is no pressure, however full the
 * budget was before; a refused allocation is an event of its own, at most once
 * a second; an unsubscribed evaluator does nothing; and a reply reports the
 * state, so what it carries needs no event. The test drives it with its own
 * clock and plays the transport's part: it runs the reclaim when it is due. */

#include "memory_pressure.hpp"
#include "require.hpp"

#include <chrono>
#include <cstdint>

using uml::gpu::MemoryPressure;
using uml::testing::require;
using Clock = MemoryPressure::Clock;
using Reading = MemoryPressure::Reading;
using namespace std::chrono_literals;

namespace {

const Clock::time_point origin {Clock::duration {1'000'000'000'000}};
constexpr std::uint64_t mebibyte = 1U << 20;
constexpr std::uint64_t budget = 600 * mebibyte;

/* The pool's usage at which the condition starts to hold: B less the margin. */
constexpr std::uint64_t threshold = budget - 96 * mebibyte;

Reading reading(std::uint64_t usage, std::uint64_t refusals = 0)
{
    return {.budgetBytes = budget, .usageBytes = usage, .refusals = refusals};
}

MemoryPressure subscribed(const Reading& first = reading(100 * mebibyte))
{
    MemoryPressure pressure;
    pressure.subscribe(first);
    return pressure;
}

/* One evaluation the way the transport makes it: the evaluator reads the
 * figures, and the host's reclaim runs when the evaluator asks for it; it
 * `freed` something or nothing. Returns whether the event is to be raised. */
bool tick(MemoryPressure& pressure, Clock::time_point now, const Reading& figures,
    bool freed = false, int* reclaims = nullptr)
{
    const bool raise = pressure.evaluate(now, figures);
    if (pressure.reclaimDue(now)) {
        pressure.reclaimed(now, freed);
        if (reclaims)
            ++*reclaims;
    }
    return raise;
}

void checkMarginAndSustain()
{
    auto pressure = subscribed();
    require(!tick(pressure, origin, reading(threshold - 1)),
        "one byte under the margin raised an event");
    require(!pressure.deadline().has_value(),
        "a calm evaluator named a deadline");

    require(!tick(pressure, origin + 1s, reading(threshold)),
        "reaching the margin raised the event at once");
    require(!pressure.pressure(), "pressure began before the sustain time");
    require(pressure.deadline() == origin + 2s,
        "the deadline was not the end of the sustain time");
    require(!tick(pressure, origin + 1s + 999ms, reading(threshold + mebibyte)),
        "an event came before the sustain time had passed");
    require(tick(pressure, origin + 2s, reading(threshold + mebibyte)),
        "no event came after the sustain time");
    require(pressure.pressure(), "pressure did not begin");
    require(!pressure.deadline().has_value(),
        "pressure that lasts named a deadline");
    require(!tick(pressure, origin + 3s, reading(budget)),
        "a second event came while pressure lasted");
}

void checkASpikeThatPassesIsNotReported()
{
    auto pressure = subscribed();
    require(!tick(pressure, origin, reading(threshold)), "early event");
    /* The condition drops before the sustain time: the clock starts over. */
    require(!tick(pressure, origin + 900ms, reading(threshold - mebibyte)),
        "dropping under the margin raised an event");
    require(!pressure.deadline().has_value(), "a dropped spike kept a deadline");
    /* The reclaim of the first rise is under a second ago: the second rise
     * waits for the reclaim interval. */
    require(!tick(pressure, origin + 950ms, reading(threshold)), "early event");
    require(pressure.deadline() == origin + 1s,
        "the second rise did not wait for the reclaim interval");
    int reclaims = 0;
    require(!tick(pressure, origin + 1s, reading(threshold), false, &reclaims),
        "early event");
    require(reclaims == 1, "the second rise did not reclaim");
    require(!tick(pressure, origin + 1950ms - 1ms, reading(threshold)),
        "the sustain time was not counted from the second rise");
    require(tick(pressure, origin + 1950ms, reading(threshold)),
        "no event a sustain time after the second rise");
}

void checkCalmEndsPressure()
{
    auto pressure = subscribed();
    (void)tick(pressure, origin, reading(threshold));
    require(tick(pressure, origin + 1s, reading(threshold)), "pressure did not begin");

    require(!tick(pressure, origin + 10s, reading(threshold - mebibyte)),
        "dropping under the margin ended pressure at once");
    require(pressure.pressure(), "pressure ended before the calm time");
    require(pressure.deadline() == origin + 15s,
        "the deadline was not the end of the calm time");
    /* A rise within the calm time starts the calm time over. */
    require(!tick(pressure, origin + 14s, reading(threshold)),
        "a rise during the calm time raised an event");
    require(!pressure.deadline().has_value(), "pressure that returned kept a deadline");
    require(!tick(pressure, origin + 20s, reading(threshold - mebibyte)),
        "calm raised an event at once");
    require(!tick(pressure, origin + 25s - 1ms, reading(threshold - mebibyte)),
        "calm ended pressure early");
    require(tick(pressure, origin + 25s, reading(threshold - mebibyte)),
        "no event after the calm time");
    require(!pressure.pressure(), "pressure did not end");
    require(!pressure.deadline().has_value(), "a calm evaluator named a deadline");
}

/* The usage holds what the host's drivers cache, which the host frees itself:
 * a budget that is full of it is no pressure. */
void checkCachesThatAreEnoughAreNoPressure()
{
    auto pressure = subscribed();
    int reclaims = 0;
    require(!tick(pressure, origin, reading(budget - 10 * mebibyte), true, &reclaims),
        "reaching the margin raised the event");
    require(reclaims == 1, "the evaluator did not ask for the reclaim");
    /* The reclaim freed the caches; the figure shows it once it has settled
     * and is back under the margin. */
    require(pressure.deadline() == origin + 1s,
        "the deadline was not the end of the sustain time");
    require(!tick(pressure, origin + 300ms, reading(budget - 200 * mebibyte)),
        "memory the reclaim freed raised the event");
    require(!pressure.deadline().has_value(), "a freed budget kept a deadline");
    require(!tick(pressure, origin + 1s, reading(budget - 200 * mebibyte)),
        "memory the reclaim freed raised the event after the sustain time");
    require(!tick(pressure, origin + 10s, reading(budget - 200 * mebibyte)) &&
            !pressure.pressure(),
        "memory the reclaim freed became pressure");
    require(reclaims == 1, "the evaluator reclaimed again with nothing to reclaim for");
}

/* What the reclaim cannot free is pressure, and it is declared before the
 * budget refuses anything. */
void checkCachesThatAreNotEnoughArePressure()
{
    auto pressure = subscribed();
    int reclaims = 0;
    require(!tick(pressure, origin, reading(budget - 40 * mebibyte), true, &reclaims),
        "reaching the margin raised the event");
    require(reclaims == 1, "the evaluator did not ask for the reclaim");
    /* The reclaim freed something, but the figure is still within the
     * margin: nothing is declared until the sustain time has passed. */
    require(!tick(pressure, origin + 300ms, reading(budget - 30 * mebibyte)),
        "the event came before the sustain time");
    require(pressure.deadline() == origin + 1s,
        "the deadline was not the end of the sustain time");
    require(!tick(pressure, origin + 1s - 1ms, reading(budget - 30 * mebibyte)),
        "the event came before the sustain time had passed");
    require(tick(pressure, origin + 1s, reading(budget - 30 * mebibyte)),
        "no event for memory the reclaim could not free");
    require(pressure.pressure(), "pressure did not begin");
    require(reclaims == 1, "the evaluator reclaimed more than once for one stretch");
}

/* Pressure is never declared on a figure the host's reclaim has not left. */
void checkTheReclaimComesBeforeThePressure()
{
    auto pressure = subscribed();
    /* The figure is found high by a reply, which runs no reclaim. */
    auto state = pressure.query(origin, reading(budget - 10 * mebibyte));
    require(!state.pressure, "a reply declared pressure before any reclaim");
    require(pressure.reclaimDue(origin), "the reclaim was not due for a high figure");
    require(pressure.deadline() == origin,
        "the due reclaim named no deadline for the transport to arm");
    state = pressure.query(origin + 10s, reading(budget - 10 * mebibyte));
    require(!state.pressure, "a reply declared pressure after a long wait with no reclaim");
    require(!tick(pressure, origin + 10s, reading(budget - 10 * mebibyte)),
        "the evaluation after a long wait raised the event before the reclaim");
    require(!pressure.pressure(), "pressure began before the reclaim");

    /* The reclaim has run now, so the long stretch counts at once. */
    require(!pressure.reclaimDue(origin + 10s), "the reclaim was due twice for one stretch");
    state = pressure.query(origin + 10s, reading(budget - 10 * mebibyte));
    require(state.pressure, "a reply did not declare the pressure that held after the reclaim");
    require(!pressure.evaluate(origin + 10s, reading(budget - 10 * mebibyte)),
        "a pressure a reply reported raised an event");
}

/* A reclaim that freed something is given time to show in the figure. */
void checkTheFigureSettlesAfterAReclaim()
{
    auto pressure = subscribed();
    int reclaims = 0;
    require(!tick(pressure, origin, reading(budget - 10 * mebibyte), true, &reclaims), "early event");
    /* The next rise comes at once and the reclaim interval holds the second
     * reclaim back; the sustain time since the rise ends before the second
     * reclaim has settled. */
    require(!tick(pressure, origin + 100ms, reading(threshold - mebibyte)),
        "dropping under the margin raised an event");
    require(!tick(pressure, origin + 200ms, reading(budget - 10 * mebibyte), true, &reclaims),
        "early event");
    require(reclaims == 1, "the reclaim interval did not hold the second reclaim back");
    require(pressure.deadline() == origin + 1s,
        "the deadline was not the end of the reclaim interval");
    require(!tick(pressure, origin + 1s, reading(budget - 10 * mebibyte), true, &reclaims),
        "the event came with the reclaim");
    require(reclaims == 2, "the held back reclaim did not run");
    /* The sustain time since the rise ended at origin + 1.2 s; the reclaim of
     * origin + 1 s settles at origin + 1.3 s. */
    require(pressure.deadline() == origin + 1300ms,
        "the deadline was not the end of the settle time");
    require(!tick(pressure, origin + 1200ms, reading(budget - 10 * mebibyte)),
        "the event came before the reclaim had settled");
    require(tick(pressure, origin + 1300ms, reading(budget - 10 * mebibyte)),
        "no event after the reclaim had settled");

    /* A reclaim that freed nothing needs no settling. */
    auto nothing = subscribed();
    (void)tick(nothing, origin, reading(budget - 10 * mebibyte), false);
    require(nothing.deadline() == origin + 1s,
        "a reclaim that freed nothing was given time to settle");
}

/* A pool that keeps a reserve refuses at its ceiling, C, below B; the
 * condition holds the margin ahead of C, not of B. A ceiling of 0 or one
 * above B reads as B. */
void checkTheMarginIsAheadOfTheCeiling()
{
    constexpr std::uint64_t ceiling = budget - 16 * mebibyte;
    const auto at = [&](std::uint64_t usage, std::uint64_t ceilingBytes) {
        return Reading {.budgetBytes = budget, .usageBytes = usage,
            .ceilingBytes = ceilingBytes};
    };
    auto pressure = subscribed();
    require(!tick(pressure, origin, at(ceiling - 96 * mebibyte - 1, ceiling)),
        "one byte under the margin of the ceiling was not calm");
    require(!pressure.deadline().has_value(),
        "one byte under the margin of the ceiling named a deadline");
    (void)tick(pressure, origin + 1s, at(ceiling - 96 * mebibyte, ceiling));
    require(pressure.deadline() == origin + 2s,
        "the margin of the ceiling did not start the sustain time");
    require(tick(pressure, origin + 2s, at(ceiling - 96 * mebibyte, ceiling)),
        "the margin ahead of the ceiling was no pressure");

    auto unset = subscribed();
    (void)tick(unset, origin, at(threshold - 1, 0));
    require(!unset.deadline().has_value(), "a ceiling of 0 did not read as B");
    auto above = subscribed();
    (void)tick(above, origin, at(threshold - 1, budget + 64 * mebibyte));
    require(!above.deadline().has_value(),
        "a ceiling above B did not read as B");
}

void checkBudgetEdges()
{
    /* No budget, no pressure. */
    auto none = subscribed();
    require(!tick(none, origin, {.budgetBytes = 0, .usageBytes = 1000 * mebibyte}),
        "pressure without a budget");
    require(!tick(none, origin + 1h, {.budgetBytes = 0, .usageBytes = 1000 * mebibyte}),
        "pressure without a budget");
    require(!none.pressure() && !none.deadline().has_value(), "pressure without a budget");

    /* A budget under the margin is always under pressure, and a usage that
     * would overflow is not a calm reading. */
    auto tiny = subscribed();
    (void)tick(tiny, origin, {.budgetBytes = 50 * mebibyte, .usageBytes = 0});
    require(tick(tiny, origin + 1s, {.budgetBytes = 50 * mebibyte}),
        "a budget under the margin was calm");
    auto huge = subscribed();
    (void)tick(huge, origin, {.budgetBytes = budget, .usageBytes = UINT64_MAX});
    require(huge.deadline() == origin + 1s, "an overflowing reading was calm");
}

void checkRefusalEdges()
{
    auto pressure = subscribed(reading(100 * mebibyte, 5));
    require(!tick(pressure, origin, reading(100 * mebibyte, 5)),
        "refusals before the subscription raised an event");

    require(tick(pressure, origin, reading(100 * mebibyte, 6)),
        "a refusal raised no event");
    require(!tick(pressure, origin + 1ms, reading(100 * mebibyte, 6)),
        "one refusal raised two events");
    /* A second refusal within the interval waits for it. */
    require(!tick(pressure, origin + 100ms, reading(100 * mebibyte, 9)),
        "refusals faster than the interval each raised an event");
    require(pressure.deadline() == origin + 1s, "the deadline was not the end of the interval");
    require(!tick(pressure, origin + 999ms, reading(100 * mebibyte, 12)),
        "an event came before the interval had passed");
    require(tick(pressure, origin + 1s, reading(100 * mebibyte, 12)),
        "the refusals held back raised no event after the interval");
    require(!pressure.deadline().has_value(), "a raised refusal kept a deadline");
    require(!tick(pressure, origin + 10s, reading(100 * mebibyte, 12)),
        "a refusal raised a second event");
}

void checkEventsOfOneEvaluationAreOne()
{
    auto pressure = subscribed();
    (void)tick(pressure, origin, reading(threshold));
    /* The pressure begins and an allocation is refused in the same reading:
     * one event, and the refusal is not held back for another. */
    require(tick(pressure, origin + 1s, reading(threshold, 1)),
        "no event for pressure with a refusal");
    require(!pressure.deadline().has_value(), "the refusal was held back after its event");
}

void checkUnsubscribedDoesNothing()
{
    MemoryPressure pressure;
    require(!pressure.subscribed(), "a new evaluator was subscribed");
    require(!tick(pressure, origin, reading(budget, 7)), "an unsubscribed evaluator raised an event");
    require(!tick(pressure, origin + 1h, reading(budget, 9)), "an unsubscribed evaluator raised an event");
    require(!pressure.deadline().has_value(), "an unsubscribed evaluator named a deadline");
    require(!pressure.pressure() && !pressure.reclaimDue(origin),
        "an unsubscribed evaluator had pressure or asked for a reclaim");

    pressure.subscribe(reading(budget, 9));
    require(pressure.subscribed(), "subscribing did not subscribe");
    require(!tick(pressure, origin, reading(budget, 9)), "the baseline refusals raised an event");
    require(pressure.deadline() == origin + 1s, "the subscription ignored the held budget");

    pressure.unsubscribe();
    require(!pressure.subscribed(), "unsubscribing did not unsubscribe");
    require(!pressure.deadline().has_value(), "an unsubscribed evaluator kept a deadline");
    require(!tick(pressure, origin + 2s, reading(budget, 11)), "an unsubscribed evaluator raised an event");
    require(!pressure.pressure(), "an unsubscribed evaluator kept the pressure");
}

void checkSubscribingAgainChangesNothing()
{
    auto pressure = subscribed();
    (void)tick(pressure, origin, reading(threshold));
    pressure.subscribe(reading(0, 100));
    require(pressure.deadline() == origin + 1s, "subscribing again cleared the evaluation");
    require(tick(pressure, origin + 1s, reading(threshold)), "subscribing again lost the sustain time");
}

void checkAReplyReportsTheState()
{
    auto pressure = subscribed(reading(100 * mebibyte, 2));
    auto state = pressure.query(origin, reading(100 * mebibyte, 2));
    require(!state.pressure && !state.refused, "a calm first reply was not calm");

    /* A refusal is reported once, and its event is not raised after. */
    state = pressure.query(origin + 10ms, reading(100 * mebibyte, 4));
    require(state.refused, "the reply did not report a refusal");
    require(!tick(pressure, origin + 20ms, reading(100 * mebibyte, 4)),
        "a refusal a reply reported raised an event");
    state = pressure.query(origin + 30ms, reading(100 * mebibyte, 4));
    require(!state.refused, "a refusal was reported twice");

    /* A reply reports a pressure that began since the last evaluation, with
     * no event. */
    (void)tick(pressure, origin + 1s, reading(threshold, 4));
    state = pressure.query(origin + 2s, reading(threshold, 4));
    require(state.pressure, "the reply did not report the pressure that began");
    require(pressure.pressure(), "the reply left the evaluator calm");
    require(!tick(pressure, origin + 2s, reading(threshold, 4)),
        "a pressure a reply reported raised an event");

    /* An unsubscribed query takes its baseline from the first reply. */
    MemoryPressure idle;
    state = idle.query(origin, reading(0, 40));
    require(!state.refused, "the first reply of an unsubscribed query reported old refusals");
    state = idle.query(origin, reading(0, 41));
    require(state.refused, "an unsubscribed query missed a refusal");
    require(!state.pressure, "an unsubscribed query reported pressure");
}

} // namespace

int main()
{
    checkMarginAndSustain();
    checkASpikeThatPassesIsNotReported();
    checkCalmEndsPressure();
    checkCachesThatAreEnoughAreNoPressure();
    checkCachesThatAreNotEnoughArePressure();
    checkTheReclaimComesBeforeThePressure();
    checkTheFigureSettlesAfterAReclaim();
    checkTheMarginIsAheadOfTheCeiling();
    checkBudgetEdges();
    checkRefusalEdges();
    checkEventsOfOneEvaluationAreOne();
    checkUnsubscribedDoesNothing();
    checkSubscribingAgainChangesNothing();
    checkAReplyReportsTheState();
    return 0;
}
