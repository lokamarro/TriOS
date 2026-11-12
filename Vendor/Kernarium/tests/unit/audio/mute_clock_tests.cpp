// SPDX-License-Identifier: GPL-2.0-or-later

#include "playback_mute_clock.hpp"
#include "require.hpp"

#include <chrono>
#include <cstdint>

namespace {

using uml::audio::MuteClock;
using uml::testing::require;
using namespace std::chrono;

const MuteClock::Clock::time_point origin {seconds(1000)};

// No period is due before one period duration has passed, and one is due at
// exactly that moment.
void firstPeriodIsDueOnePeriodLater()
{
    MuteClock clock(48000, 1024);
    clock.restart(origin);
    require(clock.deadline() == origin + nanoseconds(21'333'333),
        "the first period is due 1024 / 48000 s after the start");
    require(!clock.take(origin), "nothing is due at the start");
    require(!clock.take(origin + nanoseconds(21'333'332)),
        "nothing is due a nanosecond early");
    require(clock.take(origin + nanoseconds(21'333'333)),
        "the first period is due on time");
    require(!clock.take(origin + nanoseconds(21'333'333)),
        "one period is taken once");
}

// A schedule measured from its start does not drift: a million periods are
// due at exactly the time they take to play, whatever 1024 / 48000 s rounds to.
void periodsDoNotDrift()
{
    MuteClock clock(48000, 1024);
    clock.restart(origin);
    for (std::uint64_t period = 1; period < 1'000'000; ++period)
        require(clock.take(clock.deadline()), "each period is due on time");
    // 1e6 * 1024 / 48000 s = 21333.333... s
    require(clock.deadline() == origin + nanoseconds(21'333'333'333'333),
        "a million periods take exactly as long as they play");

    MuteClock odd(44100, 1024);
    odd.restart(origin);
    for (std::uint64_t period = 1; period < 44100; ++period)
        require(odd.take(odd.deadline()), "each period is due on time");
    require(odd.deadline() == origin + seconds(1024),
        "44100 periods of 1024 frames at 44.1 kHz take exactly 1024 s");
}

// A tick that comes late takes every period that fell due, and the periods
// after it stay on the original schedule: lateness does not accumulate.
void lateTicksDoNotAccumulate()
{
    MuteClock clock(48000, 1024);
    clock.restart(origin);
    const auto late = origin + milliseconds(100);
    int taken = 0;
    while (clock.take(late))
        ++taken;
    require(taken == 4, "100 ms holds four 21.3 ms periods");
    require(clock.deadline() == origin + nanoseconds(5 * 21'333'333 + 1),
        "the fifth period is still due at its place on the schedule");
}

// A restart discards the history, so a sink that sat idle owes nothing and
// does not complete a burst of periods when the next one arrives.
void restartBuildsNoCredit()
{
    MuteClock clock(48000, 1024);
    clock.restart(origin);
    require(clock.take(origin + seconds(1)), "a period is taken");
    const auto later = origin + seconds(60);
    clock.restart(later);
    require(!clock.take(later), "no period is due at a restart");
    require(clock.deadline() == later + nanoseconds(21'333'333),
        "a restart schedules the next period one period duration later");
}

// A clock with no rate is due at once and never divides by zero.
void unconfiguredClockIsHarmless()
{
    MuteClock clock;
    clock.restart(origin);
    require(clock.deadline() == origin, "no rate means no delay");
}

} // namespace

int main()
{
    firstPeriodIsDueOnePeriodLater();
    periodsDoNotDrift();
    lateTicksDoNotAccumulate();
    restartBuildsNoCredit();
    unconfiguredClockIsHarmless();
    return 0;
}
