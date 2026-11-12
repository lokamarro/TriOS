// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "playback_sink.hpp"

#include <algorithm>
#include <cstdint>

namespace uml::audio {

/* The IO cycle the host output is asked to run at while a guest stream plays,
 * in frames of the stream. The output consumes a whole cycle at a time, so the
 * guest's buffer has to hold a cycle plus the period it is refilling, plus the
 * time a completion takes to reach the guest. A quarter of the buffer leaves
 * the guest at least a quarter of it for that, however few periods the buffer
 * has. The cycle never exceeds 512 frames, the usual output default, so that a
 * large guest buffer does not lengthen it, nor falls below 64 frames, under
 * which the output's per-cycle cost dominates. */
struct OutputCycle final {
    static constexpr std::uint32_t minimumFrames = 64;
    static constexpr std::uint32_t maximumFrames = 512;

    [[nodiscard]] static constexpr std::uint32_t frames(
        const PlaybackParameters& parameters) noexcept
    {
        const std::uint32_t frameBytes =
            static_cast<std::uint32_t>(parameters.channels) *
            parameters.bytesPerSample;
        if (frameBytes == 0)
            return maximumFrames;
        return std::clamp(parameters.bufferBytes / frameBytes / 4U,
            minimumFrames, maximumFrames);
    }

    /* The same cycle in seconds, which is what the platform is asked for. */
    [[nodiscard]] static constexpr double seconds(
        const PlaybackParameters& parameters) noexcept
    {
        if (parameters.sampleRate == 0)
            return 0.0;
        return static_cast<double>(frames(parameters)) /
            static_cast<double>(parameters.sampleRate);
    }
};

} // namespace uml::audio
