// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "period_clock.hpp"

#include <cstdint>
#include <memory>
#include <span>

namespace uml::audio {

struct PlaybackParameters final {
    std::uint32_t bufferBytes {};
    std::uint32_t periodBytes {};
    std::uint32_t sampleRate {};
    std::uint8_t channels {};
    std::uint8_t bytesPerSample {};
};

/* What became of a period the sink had accepted. */
enum class PlaybackOutcome : std::uint8_t {
    // The output consumed it, or a muted sink completed it on the schedule
    // the output would have kept.
    played,
    // The output dropped it unplayed: a route change, a fault or a release.
    // The device keeps the guest's playback clock steady by completing it on
    // its own period clock instead.
    discarded,
    // The sink could not handle it.
    failed,
};

struct PlaybackCompletion final {
    std::uint64_t requestId {};
    // How long the period's sound still takes to be heard once it completes,
    // in bytes of the stream: the Virtio sound status's device latency. The
    // guest adds it to the periods it still has queued, so it never counts
    // them; it is 0 for a period nothing plays.
    std::uint32_t latencyBytes {};
    PlaybackOutcome outcome {PlaybackOutcome::played};
};

enum class PlaybackEnqueueResult : std::uint8_t {
    // No slot is free; offer the period again when a completion arrives.
    retry,
    // The sink owns the period and completes it through takeCompletion().
    accepted,
    // The output cannot play now (it faulted, or there is none). The sink
    // holds nothing; the device completes the period on its period clock and
    // drops the data, so the guest still sees a device that plays in real
    // time.
    discarded,
    // The request itself is invalid.
    failed,
};

/* The host's audio output. The sink signals the device's `Waker` whenever it
 * has a completion or a notification to handle; the transport worker then
 * calls handleEvents() and takes the completions. */
class PlaybackSink {
public:
    virtual ~PlaybackSink() = default;

    [[nodiscard]] virtual bool configure(
        const PlaybackParameters& parameters) = 0;
    [[nodiscard]] virtual bool prepare() = 0;
    [[nodiscard]] virtual bool start() = 0;
    virtual void stop() noexcept = 0;
    virtual void release() noexcept = 0;
    virtual void reset() noexcept = 0;
    [[nodiscard]] virtual PlaybackEnqueueResult enqueue(
        std::uint64_t requestId,
        std::span<const std::uint8_t> pcm) = 0;
    virtual void handleEvents() = 0;
    [[nodiscard]] virtual bool takeCompletion(
        PlaybackCompletion& completion) = 0;
};

/* The Apple output; `waker` is the device's. */
[[nodiscard]] std::unique_ptr<PlaybackSink> makeApplePlaybackSink(
    std::shared_ptr<const Waker> waker);

/* An output that plays nothing: every period is discarded, and the device
 * completes it at the rate real playback would. For runs that must be silent,
 * such as a conformance campaign on a desk. */
[[nodiscard]] std::unique_ptr<PlaybackSink> makeNullPlaybackSink();

} // namespace uml::audio
