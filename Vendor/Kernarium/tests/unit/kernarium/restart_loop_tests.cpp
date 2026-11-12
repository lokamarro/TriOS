// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The run loop of a Kernarium launch, with a recording machine in place of the
 * guest: the order of the steps of a restart, the retry of a refused run, the
 * stop flag, the hold on the next boot, the loop guard and the runtime's
 * restart events.
 */
#include "KernariumRestartLoop.hpp"
#include "named_tests.hpp"
#include "require.hpp"
#include "watchdog.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using kernarium::restart::Lifecycle;
using kernarium::restart::Machine;
using kernarium::restart::RunEnd;
using kernarium::restart::Timing;
using uml::testing::require;
using Clock = uml::emulator::RestartLoopGuard::Clock;

/* One scripted boot: what its run returns, how it ended, and how long it
 * lasted on the fake clock. */
struct Boot final {
    int status {0};
    RunEnd end {RunEnd::Kind::powerOff, ""};
    std::chrono::seconds lasted {60};
};

Boot powersOff(std::chrono::seconds lasted = 60s)
{
    return {0, {RunEnd::Kind::powerOff, ""}, lasted};
}

Boot restarts(std::string command = "", std::chrono::seconds lasted = 60s)
{
    return {2, {RunEnd::Kind::restart, std::move(command)}, lasted};
}

/* The machine the loop drives. Every step is recorded in `calls`, in order,
 * from whichever thread made it. */
class FakeMachine final : public Machine {
public:
    /* The boots in order; the last one repeats when the script runs out. */
    std::deque<Boot> script;
    /* A restarted boot's first `refusals` runs are refused. */
    unsigned refusals {};
    /* Building the boot numbered this fails (-1: none does). */
    int failStart {-1};
    bool endUnknown {};
    std::atomic_bool stop {};
    std::atomic_bool held {};
    /* The console answers from this poll on; never when 0. */
    unsigned answerFromPoll {};
    /* A restarted boot's run lasts until its console event arrives, or this
     * long. */
    bool runWaitsForBooted {};
    /* Runs after the call is recorded; a test uses it to act at a moment. */
    std::function<void(FakeMachine&, const std::string&)> onCall;

    [[nodiscard]] std::vector<std::string> calls() const
    {
        std::lock_guard lock(mutex_);
        return calls_;
    }

    [[nodiscard]] std::vector<std::string> said() const
    {
        std::lock_guard lock(mutex_);
        return said_;
    }

    [[nodiscard]] bool called(const std::string& call) const
    {
        for (const auto& made : calls())
            if (made == call)
                return true;
        return false;
    }

    [[nodiscard]] std::size_t count(const std::string& prefix) const
    {
        std::size_t total = 0;
        for (const auto& made : calls())
            if (made.starts_with(prefix))
                ++total;
        return total;
    }

    [[nodiscard]] unsigned polls() const { return polls_.load(); }

    bool startBoot(unsigned restarts) override
    {
        record("start " + std::to_string(restarts));
        restarts_ = restarts;
        entered_.store(false);
        return static_cast<int>(restarts) != failStart;
    }

    int enter() override
    {
        record("enter");
        entered_.store(true);
        if (restarts_ != 0 && refused_ < refusals) {
            ++refused_;
            return -UML_EMULATOR_RUNTIME_ERROR;
        }
        {
            std::lock_guard lock(mutex_);
            current_ = script.front();
            if (script.size() > 1U)
                script.pop_front();
            clock_ += current_.lasted;
        }
        if (runWaitsForBooted && restarts_ != 0) {
            std::unique_lock lock(mutex_);
            booted_.wait_for(lock, 10s, [this] { return bootedSeen_; });
        }
        return current_.status;
    }

    bool runEnd(RunEnd& end) override
    {
        record("end");
        end = current_.end;
        return !endUnknown;
    }

    void stopBoot() noexcept override { record("stop-services"); }
    void resetLink() noexcept override { record("reset-link"); }
    bool stopRequested() noexcept override { return stop.load(); }
    bool bootHeld() noexcept override { return held.load(); }

    void waitWhileHeld(std::chrono::milliseconds) override
    {
        record("wait-held");
        std::this_thread::sleep_for(1ms);
    }

    void pause(std::chrono::milliseconds delay) override
    {
        record("pause " + std::to_string(delay.count()));
    }

    Clock::time_point now() noexcept override
    {
        std::lock_guard lock(mutex_);
        return clock_;
    }

    void say(const std::string& message) noexcept override
    {
        std::lock_guard lock(mutex_);
        said_.push_back(message);
    }

    void restarting(unsigned restarts) noexcept override
    {
        record("began " + std::to_string(restarts));
    }

    bool consoleAnswers() noexcept override
    {
        // A guest answers only once its run is under way.
        if (!entered_.load())
            return false;
        const unsigned poll = ++polls_;
        return answerFromPoll != 0U && poll >= answerFromPoll;
    }

    void booted(unsigned restarts, bool answered) noexcept override
    {
        record("booted " + std::to_string(restarts) +
            (answered ? " answered" : " unanswered"));
        {
            std::lock_guard lock(mutex_);
            bootedSeen_ = true;
        }
        booted_.notify_all();
    }

private:
    void record(const std::string& call)
    {
        {
            std::lock_guard lock(mutex_);
            calls_.push_back(call);
        }
        if (onCall)
            onCall(*this, call);
    }

    mutable std::mutex mutex_;
    std::condition_variable booted_;
    std::vector<std::string> calls_;
    std::vector<std::string> said_;
    Boot current_;
    Clock::time_point clock_;
    unsigned restarts_ {};
    unsigned refused_ {};
    std::atomic_uint polls_ {};
    std::atomic_bool entered_ {};
    bool bootedSeen_ {};
};

const Timing quickly {
    .consolePoll = 1ms, .consoleBound = 60s, .heldPoll = 1ms};

[[nodiscard]] bool contains(const std::vector<std::string>& lines,
                            const std::string& line)
{
    for (const auto& candidate : lines)
        if (candidate == line)
            return true;
    return false;
}

using Calls = std::vector<std::string>;

void aGuestThatPowersOffIsBootedOnce()
{
    FakeMachine machine;
    machine.script = {powersOff()};
    require(kernarium::restart::run(machine, quickly) == 0,
            "a power-off did not return the guest's status");
    require((machine.calls() == Calls {"start 0", "enter", "end"}),
            "a power-off ran more than the one boot");
}

void everyEndButARestartEndsTheLaunchWithTheRunsStatus()
{
    for (const auto& boot : {
             Boot {0, {RunEnd::Kind::powerOff, ""}, 60s},
             Boot {0, {RunEnd::Kind::stopped, ""}, 60s},
             Boot {7, {RunEnd::Kind::failed, ""}, 60s},
             Boot {5, {RunEnd::Kind::none, ""}, 60s},
         }) {
        FakeMachine machine;
        machine.script = {boot};
        require(kernarium::restart::run(machine, quickly) == boot.status,
                "an end that is not a restart did not return the status");
        require((machine.calls() == Calls {"start 0", "enter", "end"}),
                "an end that is not a restart went on");
    }
    FakeMachine unknown;
    unknown.script = {restarts()};
    unknown.endUnknown = true;
    require(kernarium::restart::run(unknown, quickly) == 2,
            "a run whose end the library cannot say did not end the loop");
    require((unknown.calls() == Calls {"start 0", "enter", "end"}),
            "a run of unknown end was restarted");
}

void aRestartStopsTheServicesResetsTheLinkAndBootsAgainInThisOrder()
{
    FakeMachine machine;
    machine.script = {restarts("recovery"), powersOff()};
    require(kernarium::restart::run(machine, quickly) == 0,
            "the restarted guest's power-off did not end the launch");
    // The services stop before the library hands the RAM back, which its next
    // run does; the link resets before the next guest's network opens; the
    // event comes after both and before the next boot is built.
    require((machine.calls() == Calls {"start 0", "enter", "end",
                "stop-services", "reset-link", "began 1", "start 1",
                "enter", "end"}),
            "the steps of a restart are not in the loop's order");
    require(contains(machine.said(),
                "guest restarting (command 'recovery', restart 1)"),
            "the restart was not logged with its command");
}

void theRestartCommandIsLoggedSanitized()
{
    FakeMachine machine;
    machine.script = {restarts("x'\nguest restarting"), powersOff()};
    (void)kernarium::restart::run(machine, quickly);
    require(contains(machine.said(),
                "guest restarting (command 'x\\'\\x0aguest restarting', "
                "restart 1)"),
            "a restart command broke the log line");
}

void aRefusedHandBackIsRetriedThreeTimesFiveHundredMillisecondsApart()
{
    FakeMachine machine;
    machine.script = {restarts(), powersOff()};
    machine.refusals = 3;
    require(kernarium::restart::run(machine, quickly) == 0,
            "a hand-back that succeeded on the third retry failed the launch");
    require(machine.count("pause 500") == 3U,
            "the refusal was not retried 500 ms apart three times");
    require(machine.count("enter") == 5U,
            "the refused run was not entered again each time");

    FakeMachine hopeless;
    hopeless.script = {restarts(), powersOff()};
    hopeless.refusals = 4;
    require(kernarium::restart::run(hopeless, quickly) ==
                -UML_EMULATOR_RUNTIME_ERROR,
            "a refusal past the retries did not end the launch with it");
    require(hopeless.count("pause 500") == 3U,
            "the retries were not bounded by three");
    require(contains(hopeless.said(),
                "unable to boot the restarted guest: -4"),
            "the refusal was not reported");
}

void theFirstBootIsNeverRetried()
{
    FakeMachine machine;
    machine.script = {Boot {-UML_EMULATOR_RUNTIME_ERROR,
        {RunEnd::Kind::none, ""}, 1s}};
    require(kernarium::restart::run(machine, quickly) ==
                -UML_EMULATOR_RUNTIME_ERROR,
            "a first run's error was not returned");
    require(machine.count("pause") == 0U,
            "a first run's error was retried as a restart's refusal");
}

void aStopThatArrivesAtAnyStepOfARestartEndsTheLaunchCleanly()
{
    struct Case {
        const char* at;
        bool secondEnter;
        bool secondStart;
        bool servicesStopped;
    };
    // The guest ended in a restart as the user quit; the gap after its
    // services stopped; between building the next boot and running it.
    for (const Case& stopCase : {
             Case {"end", false, false, false},
             Case {"began 1", false, false, true},
             Case {"start 1", false, true, true},
         }) {
        FakeMachine machine;
        machine.script = {restarts(), powersOff()};
        machine.onCall = [&](FakeMachine& self, const std::string& call) {
            if (call == stopCase.at)
                self.stop.store(true);
        };
        require(kernarium::restart::run(machine, quickly) == 0,
                "a stop did not end the launch cleanly");
        require(machine.called("start 1") == stopCase.secondStart,
                "a stop did not keep the next boot from being built");
        require(machine.count("enter") == (stopCase.secondEnter ? 2U : 1U),
                "a stop did not keep the next guest from running");
        require(machine.called("stop-services") == stopCase.servicesStopped,
                "a stop changed which services the loop stopped");
    }
}

void aStopThatEndsTheRetriesIsNotAFailure()
{
    FakeMachine machine;
    machine.script = {restarts(), powersOff()};
    machine.refusals = 10;
    machine.onCall = [](FakeMachine& self, const std::string& call) {
        if (call == "pause 500")
            self.stop.store(true);
    };
    require(kernarium::restart::run(machine, quickly) == 0,
            "a stop during the retries was reported as an error");
    require(machine.count("pause") == 1U,
            "the retries went on after the stop");
}

void aStopBeforeTheFirstRunKeepsTheGuestFromRunning()
{
    FakeMachine machine;
    machine.script = {powersOff()};
    machine.stop.store(true);
    require(kernarium::restart::run(machine, quickly) == 0,
            "a stop before the first run did not end the launch");
    require(!machine.called("enter"),
            "a stop before the first run still ran the guest");
}

void theNextBootWaitsWhileTheApplicationHoldsIt()
{
    FakeMachine machine;
    machine.script = {restarts(), powersOff()};
    machine.held.store(true);
    machine.onCall = [](FakeMachine& self, const std::string&) {
        if (self.count("wait-held") == 3U)
            self.held.store(false);
    };
    require(kernarium::restart::run(machine, quickly) == 0,
            "a released hold did not let the guest boot");
    const auto calls = machine.calls();
    // The restart is announced, then the loop waits with nothing built, and
    // the boot follows the release.
    require((calls == Calls {"start 0", "enter", "end", "stop-services",
                "reset-link", "began 1", "wait-held", "wait-held",
                "wait-held", "start 1", "enter", "end"}),
            "the hold did not keep the next boot back");
    require(contains(machine.said(),
                "the restarted guest may boot"),
            "the end of the hold was not logged");
}

void theFirstBootIsNeverHeld()
{
    FakeMachine machine;
    machine.script = {powersOff()};
    machine.held.store(true);
    require(kernarium::restart::run(machine, quickly) == 0,
            "a hold kept the first boot back");
    require(machine.count("wait-held") == 0U,
            "the first boot waited for a hold");
}

void aStopEndsTheWaitForTheHold()
{
    FakeMachine machine;
    machine.script = {restarts(), powersOff()};
    machine.held.store(true);
    machine.onCall = [](FakeMachine& self, const std::string&) {
        if (self.count("wait-held") == 2U)
            self.stop.store(true);
    };
    require(kernarium::restart::run(machine, quickly) == 0,
            "a stop during the hold did not end the launch");
    require(!machine.called("start 1"),
            "a stop during the hold still built the next boot");
}

void fiveEarlyRestartsInARowEndTheLoopWithItsOwnResult()
{
    FakeMachine machine;
    machine.script = {restarts("", 3s)};
    require(kernarium::restart::run(machine, quickly) ==
                KERNARIUM_RUNTIME_RESULT_RESTART_LOOP,
            "a guest that never boots did not end with the loop result");
    require(machine.count("enter") == 5U,
            "the loop guard did not end the loop at five boots");
    // The fifth restart is refused: its events never happen.
    require(machine.count("began") == 4U,
            "a restart the guard refused was announced");
    require(machine.count("stop-services") == 5U &&
                machine.count("reset-link") == 5U,
            "the services of the last guest were left running");
    require(contains(machine.said(),
                "the guest restarted 5 times in a row within 15 s of "
                "booting; not booting it again"),
            "the loop guard's end was not logged");
    require(KERNARIUM_RUNTIME_RESULT_RESTART_LOOP < -UML_EMULATOR_RUNTIME_ERROR,
            "the loop result collides with a uml_emulator error");
}

void aBootThatLastsFifteenSecondsResetsTheCount()
{
    FakeMachine machine;
    machine.script = {restarts("", 3s), restarts("", 3s), restarts("", 3s),
        restarts("", 3s), restarts("", 15s), restarts("", 3s),
        restarts("", 3s), restarts("", 3s), restarts("", 3s), powersOff(3s)};
    require(kernarium::restart::run(machine, quickly) == 0,
            "a boot of fifteen seconds did not reset the early count");
    require(machine.count("enter") == 10U, "the script did not run through");
}

void aBootThatCannotBeBuiltFailsTheLaunch()
{
    FakeMachine first;
    first.script = {powersOff()};
    first.failStart = 0;
    require(kernarium::restart::run(first, quickly) ==
                -UML_EMULATOR_RUNTIME_ERROR,
            "a first boot that could not be built did not fail the launch");
    require(!first.called("enter"), "a boot that failed to build ran");

    FakeMachine restarted;
    restarted.script = {restarts(), powersOff()};
    restarted.failStart = 1;
    require(kernarium::restart::run(restarted, quickly) ==
                -UML_EMULATOR_RUNTIME_ERROR,
            "a restarted boot that could not be built did not fail");
    require(restarted.count("enter") == 1U,
            "a restarted boot that failed to build ran");
}

void theBootedEventFollowsTheNewGuestsConsole()
{
    FakeMachine machine;
    machine.script = {restarts(), powersOff()};
    machine.answerFromPoll = 3;
    machine.runWaitsForBooted = true;
    {
        uml::testing::Watchdog guard {"the restart events", 30s};
        require(kernarium::restart::run(machine, quickly) == 0,
                "the restarted guest did not end the launch");
    }
    const auto calls = machine.calls();
    // BEGAN comes before the new boot is built, BOOTED while its run is
    // under way, and the first boot sends neither.
    require((calls == Calls {"start 0", "enter", "end", "stop-services",
                "reset-link", "began 1", "start 1", "enter",
                "booted 1 answered", "end"}),
            "the restart events are not in the order of the restart");
    require(machine.polls() >= 3U,
            "the event did not wait for the console to answer");
}

void theFirstBootAsksNothingOfTheConsole()
{
    FakeMachine machine;
    machine.script = {powersOff()};
    machine.answerFromPoll = 1;
    (void)kernarium::restart::run(machine, quickly);
    require(machine.polls() == 0U && machine.count("booted") == 0U,
            "the first boot was probed for a restart event");
}

void theBootedEventIsSentWithoutAnAnswerAfterTheBound()
{
    FakeMachine machine;
    machine.script = {restarts(), powersOff()};
    machine.answerFromPoll = 0;
    machine.runWaitsForBooted = true;
    const Timing brief {.consolePoll = 1ms, .consoleBound = 30ms,
        .heldPoll = 1ms};
    {
        uml::testing::Watchdog guard {"the unanswered restart event", 30s};
        require(kernarium::restart::run(machine, brief) == 0,
                "the launch did not end");
    }
    require(machine.called("booted 1 unanswered"),
            "a console that never answers was not reported after the bound");
}

void noBootedEventIsSentWhenTheGuestEndsFirst()
{
    FakeMachine machine;
    machine.script = {restarts(), powersOff()};
    machine.answerFromPoll = 0;
    require(kernarium::restart::run(machine, quickly) == 0,
            "the launch did not end");
    require(machine.count("booted") == 0U,
            "an event was sent for a guest that had ended");
}

void aStoppedAndQuiescedLifecycleHoldsTheNextBoot()
{
    Lifecycle lifecycle;
    require(!lifecycle.held(), "a fresh lifecycle held the boot");

    // A paused guest.
    require(!lifecycle.quiesced(), "a fresh lifecycle was quiesced");
    lifecycle.quiesce();
    require(lifecycle.held(), "a quiesce between boots did not hold the boot");
    require(lifecycle.quiesced(), "a quiesce was not reported");
    lifecycle.resume();
    require(!lifecycle.held(), "a resume did not release the boot");
    require(!lifecycle.quiesced(), "a resume left the guest quiesced");

    // The GPU's request gate.
    std::atomic_bool gate {};
    lifecycle.setGpuHeld([&] { return gate.load(); });
    require(!lifecycle.held(), "an open GPU gate held the boot");
    gate.store(true);
    require(lifecycle.held(), "held GPU requests did not hold the boot");
    require(!lifecycle.quiesced(),
        "held GPU requests are not a quiesced guest");
    gate.store(false);
    require(!lifecycle.held(), "released GPU requests held the boot");

    // Either holds; both must end.
    lifecycle.quiesce();
    gate.store(true);
    gate.store(false);
    require(lifecycle.held(), "a quiesce was forgotten when the GPU opened");
    lifecycle.resume();
    gate.store(true);
    require(lifecycle.held(), "the GPU's hold was forgotten at a resume");
    lifecycle.setGpuHeld({});
    require(!lifecycle.held(), "an unregistered GPU still held the boot");
}

/* Calls `nudge` until `done`, so that a wake-up sent before the waiter began
 * to wait is sent again. */
template<typename Nudge, typename Done>
void nudgeUntil(Nudge nudge, Done done)
{
    while (!done()) {
        nudge();
        std::this_thread::sleep_for(1ms);
    }
}

void aWaitingBootWakesWhenTheHoldEnds()
{
    Lifecycle lifecycle;
    std::atomic_bool gate {true};
    lifecycle.setGpuHeld([&] { return gate.load(); });
    lifecycle.quiesce();
    std::atomic_uint waits {};
    // A long slice: only a wake-up ends a wait early.
    std::thread waiter {[&] {
        while (lifecycle.held()) {
            ++waits;
            lifecycle.waitWhileHeld(60s);
        }
    }};
    uml::testing::Watchdog guard {"the waiting boot", 30s};
    nudgeUntil([] {}, [&] { return waits.load() >= 1U; });
    // The GPU opens but the guest is still paused: it wakes, looks, and
    // waits again.
    gate.store(false);
    nudgeUntil([&] { lifecycle.changed(); },
               [&] { return waits.load() >= 2U; });
    require(lifecycle.held(), "a released GPU gate released a paused guest");
    lifecycle.resume();
    waiter.join();
    require(!lifecycle.held(), "the boot was not released");
}

void aStopWakesAWaitingBootAndIsNotInheritedByTheNextRun()
{
    Lifecycle lifecycle;
    lifecycle.quiesce();
    std::atomic_bool woke {};
    std::thread waiter {[&] {
        lifecycle.waitWhileHeld(60s);
        woke.store(true);
    }};
    {
        uml::testing::Watchdog guard {"the stop wake-up", 30s};
        nudgeUntil([&] { lifecycle.requestStop(); },
                   [&] { return woke.load(); });
        waiter.join();
    }
    require(lifecycle.stopRequested(),
            "a stop did not wake the waiting boot");
    lifecycle.begin();
    require(!lifecycle.stopRequested(),
            "a stop asked for before a run was carried into it");
}

const uml::testing::NamedTest tests[] {
    {"a-guest-that-powers-off-is-booted-once", aGuestThatPowersOffIsBootedOnce},
    {"every-end-but-a-restart-ends-the-launch",
        everyEndButARestartEndsTheLaunchWithTheRunsStatus},
    {"a-restart-runs-its-steps-in-order",
        aRestartStopsTheServicesResetsTheLinkAndBootsAgainInThisOrder},
    {"the-restart-command-is-logged-sanitized",
        theRestartCommandIsLoggedSanitized},
    {"a-refused-hand-back-is-retried",
        aRefusedHandBackIsRetriedThreeTimesFiveHundredMillisecondsApart},
    {"the-first-boot-is-never-retried", theFirstBootIsNeverRetried},
    {"a-stop-at-any-step-of-a-restart",
        aStopThatArrivesAtAnyStepOfARestartEndsTheLaunchCleanly},
    {"a-stop-during-the-retries-is-not-a-failure",
        aStopThatEndsTheRetriesIsNotAFailure},
    {"a-stop-before-the-first-run", aStopBeforeTheFirstRunKeepsTheGuestFromRunning},
    {"the-next-boot-waits-while-held", theNextBootWaitsWhileTheApplicationHoldsIt},
    {"the-first-boot-is-never-held", theFirstBootIsNeverHeld},
    {"a-stop-ends-the-wait-for-the-hold", aStopEndsTheWaitForTheHold},
    {"five-early-restarts-end-the-loop",
        fiveEarlyRestartsInARowEndTheLoopWithItsOwnResult},
    {"a-fifteen-second-boot-resets-the-count",
        aBootThatLastsFifteenSecondsResetsTheCount},
    {"a-boot-that-cannot-be-built", aBootThatCannotBeBuiltFailsTheLaunch},
    {"the-booted-event-follows-the-console",
        theBootedEventFollowsTheNewGuestsConsole},
    {"the-first-boot-asks-nothing-of-the-console",
        theFirstBootAsksNothingOfTheConsole},
    {"the-booted-event-after-the-bound",
        theBootedEventIsSentWithoutAnAnswerAfterTheBound},
    {"no-booted-event-when-the-guest-ends-first",
        noBootedEventIsSentWhenTheGuestEndsFirst},
    {"the-lifecycle-holds-the-next-boot",
        aStoppedAndQuiescedLifecycleHoldsTheNextBoot},
    {"a-waiting-boot-wakes-when-the-hold-ends", aWaitingBootWakesWhenTheHoldEnds},
    {"a-stop-wakes-a-waiting-boot",
        aStopWakesAWaitingBootAndIsNotInheritedByTheNextRun},
};

} // namespace

int main()
{
    const uml::testing::Watchdog bound {
        "the Kernarium restart loop tests", 120s};
    return uml::testing::runNamedTests(tests);
}
