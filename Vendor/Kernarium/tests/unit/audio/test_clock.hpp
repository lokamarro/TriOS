// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "period_clock.hpp"

#include <chrono>
#include <optional>

namespace uml::audio_testing {

using namespace std::chrono_literals;

/* A period clock whose time the test advances by hand. */
class TestClock final : public uml::audio::PeriodClock {
public:
    uml::audio::ClockTime now() const noexcept override { return time; }
    void notify() const noexcept override { ++notifyCount; }
    void wakeAt(uml::audio::ClockTime deadline) noexcept override
    {
        armed = deadline;
    }
    void cancel() noexcept override { armed.reset(); }
    int descriptor() const noexcept override { return -1; }
    void drain() noexcept override { ++drainCount; }

    /* What the operating system does as time passes: a deadline that has
     * come fires once and is gone. */
    void fire()
    {
        if (armed && *armed <= time)
            armed.reset();
    }

    uml::audio::ClockTime time {std::chrono::steady_clock::time_point {} + 1h};
    std::optional<uml::audio::ClockTime> armed;
    mutable unsigned notifyCount {};
    unsigned drainCount {};
};

} // namespace uml::audio_testing
