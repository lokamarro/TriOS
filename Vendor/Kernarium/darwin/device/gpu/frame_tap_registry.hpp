// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "frame_tap.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>

namespace uml::gpu {

/* The process-wide mirror setting and the wakeups of the threads that render
 * it. Internal to the display library. */
class FrameTapRegistry final {
public:
    struct Armed final {
        FrameTapOptions options;
        std::uint64_t generation {};
    };

    [[nodiscard]] static FrameTapRegistry& instance() noexcept;

    void set(
        FrameTapSink sink,
        void* context,
        FrameTapOptions options) noexcept;

    [[nodiscard]] bool armed() const noexcept
    {
        return armed_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::optional<Armed> snapshot() const noexcept;

    /* Delivers a frame to the sink of generation `generation`, if that is
     * still the one set. Returns whether the sink ran. */
    bool deliver(std::uint64_t generation, void* pixelBuffer) noexcept;

    /* A renderer has something new for the mirror. */
    void wake() noexcept;

    /* Blocks until `ready` holds, the setting changes, or `limit` passes.
     * `ready` is evaluated under the registry's lock and must not block. */
    template<typename Predicate>
    void wait(std::chrono::milliseconds limit, Predicate ready) noexcept
    {
        std::unique_lock lock(mutex_);
        const auto generation = generation_;
        (void)changed_.wait_for(lock, limit, [&] {
            return generation_ != generation || ready();
        });
    }

    /* Blocks while nothing is set, until `ready` holds or the setting
     * changes. */
    template<typename Predicate>
    void waitArmed(Predicate ready) noexcept
    {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] {
            return armed_.load(std::memory_order_relaxed) || ready();
        });
    }

private:
    FrameTapRegistry() = default;

    mutable std::mutex mutex_;
    std::mutex deliveryMutex_;
    std::condition_variable changed_;
    FrameTapSink sink_ {};
    void* context_ {};
    FrameTapOptions options_ {};
    std::uint64_t generation_ {};
    std::atomic_bool armed_ {};
};

} // namespace uml::gpu
