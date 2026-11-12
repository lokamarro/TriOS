// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumKeyWake.hpp"
#include "require.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

using kernarium::input::KeyWakePolicy;
using uml::testing::require;

constexpr bool power = true;
constexpr bool ordinary = false;
constexpr bool down = true;
constexpr bool up = false;

const KeyWakePolicy::Clock::time_point origin {1000s};

/* A release never wakes the guest, whatever key it is. */
void aKeyReleaseNeverWakes()
{
    KeyWakePolicy policy(1s);
    require(!policy.shouldWake(power, up, origin), "a Power release woke");
    require(!policy.shouldWake(ordinary, up, origin), "a key release woke");
    require(policy.shouldWake(ordinary, down, origin),
        "a release used up the first key-down's wake");
}

/* Power wakes at every press, even a second press at the same instant. */
void powerWakesAtEveryPress()
{
    KeyWakePolicy policy(1s);
    for (int press = 0; press < 5; ++press)
        require(policy.shouldWake(power, down, origin + 10ms * press),
            "a Power press did not wake");
}

/* An ordinary key wakes on its first press, then at most once per interval:
 * a press just inside the interval does not, one exactly an interval later
 * does. */
void anOrdinaryKeyWakesOncePerInterval()
{
    KeyWakePolicy policy(1s);
    require(policy.shouldWake(ordinary, down, origin), "the first key did not wake");
    require(!policy.shouldWake(ordinary, down, origin + 1ms),
        "a second key a millisecond later woke");
    require(!policy.shouldWake(ordinary, down, origin + 999999999ns),
        "a key just inside the interval woke");
    require(policy.shouldWake(ordinary, down, origin + 1s),
        "a key an interval later did not wake");
    require(!policy.shouldWake(ordinary, down, origin + 1s + 500ms),
        "the interval did not restart at the last wake");
    require(policy.shouldWake(ordinary, down, origin + 2s),
        "a key an interval after the second wake did not wake");
}

/* A held key repeats fast; a burst inside one interval is one wake edge. */
void aBurstIsOneWake()
{
    KeyWakePolicy policy(1s);
    int wakes = 0;
    for (int key = 0; key < 100; ++key)
        wakes += policy.shouldWake(ordinary, down, origin + 5ms * key) ? 1 : 0;
    require(wakes == 1, "a burst inside one interval sent more than one wake");
}

/* Power neither waits for nor uses up the ordinary keys' interval. */
void powerAndOrdinaryKeysDoNotShareAnInterval()
{
    KeyWakePolicy policy(1s);
    require(policy.shouldWake(ordinary, down, origin), "the first key did not wake");
    require(policy.shouldWake(power, down, origin + 1ms),
        "Power waited for the key's interval");
    require(!policy.shouldWake(ordinary, down, origin + 2ms),
        "Power restarted the key's interval");
    KeyWakePolicy fresh(1s);
    require(fresh.shouldWake(power, down, origin), "a fresh Power press did not wake");
    require(fresh.shouldWake(ordinary, down, origin + 1ms),
        "Power used up the key's first wake");
}

/* Callers on several threads at one instant share the interval: one wins. */
void oneCallerWinsAnInterval()
{
    for (int round = 0; round < 50; ++round) {
        KeyWakePolicy policy(1s);
        std::atomic<int> wakes {0};
        std::atomic<bool> go {false};
        std::vector<std::thread> threads;
        for (int index = 0; index < 8; ++index)
            threads.emplace_back([&] {
                while (!go.load())
                    std::this_thread::yield();
                if (policy.shouldWake(ordinary, down, origin))
                    wakes.fetch_add(1);
            });
        go.store(true);
        for (auto& thread : threads)
            thread.join();
        require(wakes.load() == 1, "several threads each won the same interval");
    }
}

}  // namespace

int main()
{
    aKeyReleaseNeverWakes();
    powerWakesAtEveryPress();
    anOrdinaryKeyWakesOncePerInterval();
    aBurstIsOneWake();
    powerAndOrdinaryKeysDoNotShareAnInterval();
    oneCallerWinsAnInterval();
    std::puts("PASS: key wake policy wakes Power at every press and other keys once an interval");
    return EXIT_SUCCESS;
}
