// SPDX-License-Identifier: GPL-2.0-or-later

#include "vnc_display_testing.hpp"

#include "vnc_display.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace uml::gpu::testing {
namespace {

std::atomic_bool failEventThreadStart {};
std::atomic<std::uint16_t> failedPort {};

std::mutex completionMutex;
std::condition_variable completionChanged;
bool completionsHeld {};
unsigned completionsWaiting {};

} // namespace

void failNextEventThreadStart() noexcept
{
    failedPort.store(0, std::memory_order_relaxed);
    failEventThreadStart.store(true, std::memory_order_release);
}

void eventThreadStartCheckpoint(const std::uint16_t port)
{
    if (!failEventThreadStart.exchange(false, std::memory_order_acq_rel))
        return;
    failedPort.store(port, std::memory_order_release);
    throw std::runtime_error("forced VNC event-thread startup failure");
}

std::uint16_t failedEventThreadPort() noexcept
{
    return failedPort.load(std::memory_order_acquire);
}

void holdReadbackCompletions() noexcept
{
    std::lock_guard lock(completionMutex);
    completionsHeld = true;
}

void releaseReadbackCompletions() noexcept
{
    {
        std::lock_guard lock(completionMutex);
        completionsHeld = false;
    }
    completionChanged.notify_all();
}

unsigned heldReadbackCompletions() noexcept
{
    std::lock_guard lock(completionMutex);
    return completionsWaiting;
}

void readbackCompletionCheckpoint() noexcept
{
    std::unique_lock lock(completionMutex);
    if (!completionsHeld)
        return;
    ++completionsWaiting;
    (void)completionChanged.wait_for(
        lock, std::chrono::seconds(10), [] { return !completionsHeld; });
    --completionsWaiting;
}

} // namespace uml::gpu::testing
