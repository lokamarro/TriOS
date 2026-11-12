// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdint>

namespace uml::audio {

/* The schedule on which a muted playback sink completes periods: one every
 * period duration, as the audio hardware would have. Period n of a busy
 * stretch is due n period durations after the stretch started, computed from
 * the start rather than from the previous period, so neither a late tick nor
 * the rounding of a period duration accumulates into the guest's clock. */
class MuteClock final {
public:
    using Clock = std::chrono::steady_clock;

    MuteClock() = default;

    MuteClock(std::uint32_t sampleRate, std::uint32_t periodFrames) noexcept
        : sampleRate_(sampleRate)
        , periodFrames_(periodFrames)
    {
    }

    /* Starts a busy stretch at `now`: the first period is due one period
     * duration later. A stretch starts whenever periods arrive at an idle
     * sink, so an idle sink builds up no credit. */
    void restart(Clock::time_point now) noexcept
    {
        origin_ = now;
        taken_ = 0;
    }

    /* When the next period of the stretch is due. */
    [[nodiscard]] Clock::time_point deadline() const noexcept
    {
        return origin_ + elapsed(taken_ + 1U);
    }

    /* Takes the next period if it is due at `now`. */
    [[nodiscard]] bool take(Clock::time_point now) noexcept
    {
        if (now < deadline())
            return false;
        ++taken_;
        return true;
    }

private:
    [[nodiscard]] Clock::duration elapsed(std::uint64_t periods) const noexcept
    {
        if (sampleRate_ == 0)
            return Clock::duration::zero();
        const std::uint64_t frames = periods * periodFrames_;
        const std::uint64_t seconds = frames / sampleRate_;
        const std::uint64_t remainder = frames % sampleRate_;
        constexpr std::uint64_t nanosecondsPerSecond = 1'000'000'000;
        return std::chrono::duration_cast<Clock::duration>(
            std::chrono::nanoseconds(static_cast<std::int64_t>(
                seconds * nanosecondsPerSecond +
                remainder * nanosecondsPerSecond / sampleRate_)));
    }

    std::uint32_t sampleRate_ {};
    std::uint32_t periodFrames_ {};
    Clock::time_point origin_ {};
    std::uint64_t taken_ {};
};

} // namespace uml::audio
