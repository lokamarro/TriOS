// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace uml::platform {

/* The host GPU's device memory budget and what the host GPU held against it
 * when the device last counted, for a reader that must not wait on the GPU
 * device: the session record's memory checkpoint
 * (runtime/session_record.hpp). The device publishes the budget when it makes
 * its pool and withdraws it (zero) when the pool goes, and publishes what is
 * held, the pool's usage less the reservations in flight, each time the pool
 * counts it: at every reservation and every statistics reading. An idle
 * device counts nothing, so the reading carries the moment it was taken. One
 * copy per image, and the runtime and the GPU device share an image. */
struct GpuMemoryFigures final {
    std::uint64_t budgetBytes {};
    std::uint64_t heldBytes {};
    /* steady_clock milliseconds of the held reading; 0 before the first. */
    std::int64_t heldAtMs {};
};

namespace detail {
inline std::atomic<std::uint64_t> gpuMemoryBudget {0};
inline std::atomic<std::uint64_t> gpuMemoryHeld {0};
inline std::atomic<std::int64_t> gpuMemoryHeldAt {0};
} // namespace detail

[[nodiscard]] inline std::int64_t gpuMemoryClockMs() noexcept
{
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

inline void publishGpuMemoryBudget(std::uint64_t bytes) noexcept
{
    detail::gpuMemoryBudget.store(bytes, std::memory_order_relaxed);
    if (bytes == 0)
        detail::gpuMemoryHeldAt.store(0, std::memory_order_relaxed);
}

inline void publishGpuMemoryHeld(std::uint64_t bytes) noexcept
{
    detail::gpuMemoryHeld.store(bytes, std::memory_order_relaxed);
    detail::gpuMemoryHeldAt.store(gpuMemoryClockMs(),
        std::memory_order_relaxed);
}

[[nodiscard]] inline GpuMemoryFigures publishedGpuMemory() noexcept
{
    return {
        .budgetBytes = detail::gpuMemoryBudget.load(std::memory_order_relaxed),
        .heldBytes = detail::gpuMemoryHeld.load(std::memory_order_relaxed),
        .heldAtMs = detail::gpuMemoryHeldAt.load(std::memory_order_relaxed),
    };
}

} // namespace uml::platform
