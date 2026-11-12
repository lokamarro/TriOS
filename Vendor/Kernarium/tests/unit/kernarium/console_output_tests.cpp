// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumConsoleOutput.hpp"
#include "require.hpp"
#include "wait_until.hpp"

#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <signal.h>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;
using uml::testing::require;

void callbacksMayChangeTheirOwnRegistration()
{
    kernarium::console::Output output;
    struct Receiver final {
        kernarium::console::Output& output;
        std::string received;
    } receiver {output, {}};
    output.setCallback([](void* context, const void* buffer, std::size_t size) {
        auto& receiver = *static_cast<Receiver*>(context);
        receiver.received.append(static_cast<const char*>(buffer), size);
        receiver.output.setCallback(nullptr, nullptr);
    }, &receiver);
    output.publish("first", 5);
    output.publish("second", 6);
    require(receiver.received == "first",
        "clearing the callback did not stop later deliveries");
}

std::atomic<unsigned> deliveries {};

void keepRelayingAfterStaticDestruction()
{
    // The detached publisher delivers about every millisecond, so a healthy
    // relay passes at its first delivery; only a stuck one waits out the
    // deadline, which a loaded host cannot reach.
    const auto before = deliveries.load();
    if (!uml::testing::waitUntil([before] { return deliveries.load() > before; },
            3s))
        std::_Exit(2);
}

void relaySurvivesProcessExit()
{
    const pid_t child = fork();
    require(child >= 0, "fork");
    if (child == 0) {
        // Register before creating output(), so this runs after any static
        // destructor that output() might accidentally acquire in the future.
        if (std::atexit(keepRelayingAfterStaticDestruction) != 0)
            std::_Exit(3);
        auto& output = kernarium::console::output();
        output.setCallback([](void*, const void*, std::size_t) {
            deliveries.fetch_add(1);
        }, nullptr);
        std::thread([&output] {
            for (;;) {
                output.publish("x", 1);
                {
                    std::lock_guard lock(output.stdoutMutex());
                }
                std::this_thread::sleep_for(1ms);
            }
        }).detach();
        if (!uml::testing::waitUntil([] { return deliveries.load() > 1; }, 2s))
            std::_Exit(4);
        std::exit(0);
    }
    int status = 0;
    // The child waits up to 2 s for its first deliveries and 3 s more at exit.
    const bool ended = uml::testing::waitUntil([&] {
        return waitpid(child, &status, WNOHANG) == child;
    }, 10s);
    if (!ended) {
        (void)kill(child, SIGKILL);
        (void)waitpid(child, &status, 0);
    }
    require(ended, "the exiting console relay did not end");
    require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "console output failed while static destructors ran");
}

} // namespace

int main()
{
    callbacksMayChangeTheirOwnRegistration();
    relaySurvivesProcessExit();
    std::puts("PASS: console output survives process exit");
    return EXIT_SUCCESS;
}
