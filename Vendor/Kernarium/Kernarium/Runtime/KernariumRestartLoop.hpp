// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "KernariumRuntime.h"
#include "restart_loop_guard.hpp"
#include "uml/emulator.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace kernarium::restart {

using Clock = uml::emulator::RestartLoopGuard::Clock;

/*
 * The run loop of a Kernarium launch: boot the guest, and boot it again each
 * time it restarts itself, until it powers off, is stopped, fails to run, or
 * restarts early too often. It is the loop of the macOS launchers
 * (darwin/app/macos/main.cpp, Launch::run) with the application's additions:
 * the next boot waits while the application holds the guest's GPU requests or
 * has paused the guest, and the application is told when a restart begins and
 * when the restarted guest's console answers.
 *
 * The loop knows nothing of devices, kernels or Apple frameworks. The runtime
 * implements Machine, and the host unit tests implement it with a recording
 * fake, so the order of the steps, the retry of a refused run, the stop flag,
 * the hold and the events are tested without a guest.
 */

/* How the guest of the last run ended (uml_emulator_get_run_end). */
struct RunEnd final {
    enum class Kind { none, powerOff, restart, stopped, failed };

    Kind kind {Kind::none};
    /* The guest's raw restart command. */
    std::string command;
};

/* The delays the loop waits for. The runtime uses the defaults. */
struct Timing final {
    /* How often the restarted guest's console is asked whether it answers,
     * and for how long before the loop reports the boot without an answer. */
    std::chrono::milliseconds consolePoll {500};
    std::chrono::milliseconds consoleBound {std::chrono::seconds {120}};
    /* The longest the loop sleeps between looks at a hold that keeps the next
     * boot back. */
    std::chrono::milliseconds heldPoll {250};
};

/*
 * What the application has told the runtime about the guest's life, which the
 * loop reads between boots: whether the user stopped it, and whether the next
 * boot must wait. Any thread may call any member.
 *
 * The next boot waits while the guest is paused (quiesce until resume), and
 * while the GPU's request gate is held (set_gpu_hold). A guest that booted
 * behind a closed gate would find its GPU unanswered: the virtio_gpu probe
 * waits five seconds for each capset and, on a timeout, drops every capset for
 * the whole boot, so the guest would run without 3D after the application
 * returned. The first boot is not held: a hold asked for before the guest's
 * GPU exists is applied when it does.
 */
class Lifecycle final {
public:
    /* The GPU service's request gate while a service is registered; empty
     * after. */
    void setGpuHeld(std::function<bool()> held)
    {
        {
            std::lock_guard lock(mutex_);
            gpuHeld_ = std::move(held);
        }
        changed_.notify_all();
    }

    void quiesce() noexcept { quiesced_.store(true, std::memory_order_release); }

    /* Whether the application has quiesced the guest and not yet resumed it. */
    [[nodiscard]] bool quiesced() const noexcept
    {
        return quiesced_.load(std::memory_order_acquire);
    }

    void resume() noexcept
    {
        quiesced_.store(false, std::memory_order_release);
        changed();
    }

    /* A run begins: a stop asked for before it is not carried over. */
    void begin() noexcept { stop_.store(false, std::memory_order_release); }

    void requestStop() noexcept
    {
        stop_.store(true, std::memory_order_release);
        changed();
    }

    [[nodiscard]] bool stopRequested() const noexcept
    {
        return stop_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool held()
    {
        if (quiesced_.load(std::memory_order_acquire))
            return true;
        std::lock_guard lock(mutex_);
        return gpuHeld_ && gpuHeld_();
    }

    /* Wakes a boot that waits, after a hold ended. */
    void changed() noexcept
    {
        {
            std::lock_guard lock(mutex_);
        }
        changed_.notify_all();
    }

    /* Sleeps until something changed, or `slice` passes. */
    void waitWhileHeld(std::chrono::milliseconds slice)
    {
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, slice);
    }

private:
    std::atomic_bool quiesced_ {};
    std::atomic_bool stop_ {};
    std::mutex mutex_;
    std::condition_variable changed_;
    std::function<bool()> gpuHeld_;
};

class Machine {
public:
    virtual ~Machine() = default;

    /* Builds the boot: everything that belongs to one guest (its Virtio
     * services, its connection to the GPU, the registrations that route the
     * application's input to it) and its kernel command line. `restarts` is
     * the number of restarts so far; a restarted boot also picks its Android
     * ramdisk again and takes the GPU's next connection. False, with the
     * reason reported, when it cannot be built. */
    [[nodiscard]] virtual bool startBoot(unsigned restarts) = 0;

    /* Runs the guest of the boot: uml_emulator_run. */
    [[nodiscard]] virtual int enter() = 0;

    /* How the last run ended. False when the library cannot say. */
    [[nodiscard]] virtual bool runEnd(RunEnd& end) = 0;

    /* The guest is gone: stops every service of the boot, which must not
     * borrow the guest's RAM when the library hands it back. */
    virtual void stopBoot() noexcept = 0;

    /* Resets the user network's link, so the old guest's connections end. */
    virtual void resetLink() noexcept = 0;

    /* Whether the application asked to stop. */
    [[nodiscard]] virtual bool stopRequested() noexcept = 0;

    /* Whether the application holds the next boot back: the guest is paused,
     * or its GPU requests are held. A guest that booted behind a closed gate
     * would probe its GPU, time out, and run the whole boot without 3D. */
    [[nodiscard]] virtual bool bootHeld() noexcept = 0;

    /* Sleeps until the hold may have changed, or `slice` passes. */
    virtual void waitWhileHeld(std::chrono::milliseconds slice) = 0;

    /* Sleeps `delay`. */
    virtual void pause(std::chrono::milliseconds delay) = 0;

    [[nodiscard]] virtual Clock::time_point now() noexcept = 0;

    /* Says something once, on the log. */
    virtual void say(const std::string& message) noexcept = 0;

    /* The restart event KERNARIUM_RUNTIME_RESTART_BEGAN: the guest asked to
     * restart, and its services are stopped. */
    virtual void restarting(unsigned restarts) noexcept = 0;

    /* Whether the new guest's management console answers. */
    [[nodiscard]] virtual bool consoleAnswers() noexcept = 0;

    /* The restart event KERNARIUM_RUNTIME_RESTART_BOOTED. `answered` is false
     * when the console had not answered by the end of the bound. */
    virtual void booted(unsigned restarts, bool answered) noexcept = 0;
};

namespace detail {

/* Asks the restarted guest's console until it answers, then tells the
 * machine. It ends with the run it watches, without a word if the guest ended
 * first. */
class BootWatcher final {
public:
    BootWatcher(Machine& machine, unsigned restarts, const Timing& timing)
        : machine_(machine)
        , restarts_(restarts)
        , timing_(timing)
        , thread_([this] { watch(); })
    {
    }

    ~BootWatcher()
    {
        {
            std::lock_guard lock(mutex_);
            ended_ = true;
        }
        changed_.notify_all();
        thread_.join();
    }

    BootWatcher(const BootWatcher&) = delete;
    BootWatcher& operator=(const BootWatcher&) = delete;

private:
    void watch()
    {
        const auto deadline = std::chrono::steady_clock::now() +
            timing_.consoleBound;
        for (;;) {
            const bool answered = machine_.consoleAnswers();
            std::unique_lock lock(mutex_);
            if (ended_)
                return;
            if (answered || std::chrono::steady_clock::now() >= deadline) {
                lock.unlock();
                machine_.booted(restarts_, answered);
                return;
            }
            changed_.wait_for(lock, timing_.consolePoll,
                [this] { return ended_; });
            if (ended_)
                return;
        }
    }

    Machine& machine_;
    const unsigned restarts_;
    const Timing timing_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool ended_ {};
    std::thread thread_;
};

/* Waits for the hold on the next boot to end. False when the application
 * stopped meanwhile. */
[[nodiscard]] inline bool waitForRelease(Machine& machine,
                                         const Timing& timing)
{
    bool announced = false;
    while (machine.bootHeld()) {
        if (machine.stopRequested())
            return false;
        if (!announced) {
            machine.say(
                "the restarted guest waits until the application releases "
                "its GPU requests or resumes the guest");
            announced = true;
        }
        machine.waitWhileHeld(timing.heldPoll);
    }
    if (announced)
        machine.say("the restarted guest may boot");
    return !machine.stopRequested();
}

} // namespace detail

/* Runs the loop and returns what the application's run returns: the last
 * guest's status, a negated uml_emulator error when a run or start-up failed,
 * or KERNARIUM_RUNTIME_RESULT_RESTART_LOOP when the loop guard ended it. */
[[nodiscard]] inline int run(Machine& machine, const Timing& timing = {})
{
    using uml::emulator::RestartLoopGuard;

    RestartLoopGuard guard;
    unsigned restarts = 0;
    for (;;) {
        if (restarts != 0 && !detail::waitForRelease(machine, timing))
            return 0;
        if (!machine.startBoot(restarts))
            return -UML_EMULATOR_RUNTIME_ERROR;
        guard.boot_started(machine.now());
        /* A stop that came between two boots was dropped by the handle: the
         * library does not inherit a request made while idle. */
        if (machine.stopRequested())
            return 0;

        int status;
        {
            /* Asks the restarted guest's console whether it answers until
             * this run ends. */
            std::optional<detail::BootWatcher> watcher;
            if (restarts != 0)
                watcher.emplace(machine, restarts, timing);

            status = machine.enter();
            /* The library refuses to take a restarted guest back while a
             * kernel thread of the old one is still ending, and changes
             * nothing when it does. */
            for (unsigned attempt = 1;
                 restarts != 0 && status == -UML_EMULATOR_RUNTIME_ERROR &&
                 attempt <= uml::emulator::restart_run_retries &&
                 !machine.stopRequested();
                 ++attempt) {
                machine.say(
                    "the restarted guest is not handed back yet; trying "
                    "again (" + std::to_string(attempt) + " of " +
                    std::to_string(uml::emulator::restart_run_retries) + ")");
                machine.pause(uml::emulator::restart_run_retry_delay);
                status = machine.enter();
            }
        }

        RunEnd end;
        const bool known = machine.runEnd(end);
        if (restarts != 0 && status < 0) {
            /* A stop that ended the retries is not a failure. */
            if (machine.stopRequested())
                return 0;
            machine.say("unable to boot the restarted guest: " +
                std::to_string(status));
            return status;
        }
        if (!known || end.kind != RunEnd::Kind::restart)
            return status;
        /* The guest restarted as the user quit: the quit wins, and is a
         * clean stop whatever status the restart carried. */
        if (machine.stopRequested())
            return 0;
        /* The guest is gone: its services stop before the library hands its
         * RAM back to the process, and its connections end with its network
         * link. */
        machine.stopBoot();
        machine.resetLink();
        if (!guard.may_restart(machine.now())) {
            machine.say(
                "the guest restarted " +
                std::to_string(guard.early_restarts()) +
                " times in a row within " +
                std::to_string(
                    uml::emulator::early_restart_window.count()) +
                " s of booting; not booting it again");
            return KERNARIUM_RUNTIME_RESULT_RESTART_LOOP;
        }
        ++restarts;
        machine.say(
            "guest restarting (command '" +
            uml::emulator::printable_restart_command(end.command) +
            "', restart " + std::to_string(restarts) + ")");
        machine.restarting(restarts);
    }
}

} // namespace kernarium::restart
