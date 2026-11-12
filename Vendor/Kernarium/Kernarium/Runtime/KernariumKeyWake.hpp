// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <limits>

namespace kernarium::input {

/* Which key events send the guest a wake edge. A guest that suspended itself,
 * as Android does once its display is off, holds virtio input until something
 * wakes it, so the host wakes it for the keyboard, as a phone does. Power
 * wakes it at every press. Any other key wakes it at most once per interval,
 * because a held key repeats and a running guest only answers the edge. A key
 * release never wakes it. Touches do not go through this policy at all: they
 * never wake the guest, because a touchscreen on a phone with its display off
 * does not. */
class KeyWakePolicy final {
public:
    using Clock = std::chrono::steady_clock;

    explicit KeyWakePolicy(Clock::duration interval) noexcept
        : interval_(interval)
    {
    }

    /* Whether this key event sends a wake edge. Safe to call from several
     * threads; at most one caller wins each interval. */
    [[nodiscard]] bool shouldWake(
        bool power, bool pressed, Clock::time_point now) noexcept
    {
        if (!pressed)
            return false;
        if (power)
            return true;
        const Clock::rep tick = now.time_since_epoch().count();
        const Clock::rep window = interval_.count();
        Clock::rep last = lastWake_.load(std::memory_order_relaxed);
        do {
            if (last != never && tick - last < window)
                return false;
        } while (!lastWake_.compare_exchange_weak(
            last, tick, std::memory_order_relaxed));
        return true;
    }

private:
    static constexpr Clock::rep never = std::numeric_limits<Clock::rep>::min();

    Clock::duration interval_;
    std::atomic<Clock::rep> lastWake_ {never};
};

}  // namespace kernarium::input
