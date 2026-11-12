// SPDX-License-Identifier: GPL-2.0-or-later
#include "period_clock.hpp"
#include "playback_sink.hpp"

#include <poll.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>

namespace {
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

/* A period the output has consumed is heard after the output's own latency,
 * and the guest counts the periods still queued itself: every completion of
 * a stream reports the same latency, however many periods are queued, and
 * it is never nothing. */
class Latency final {
public:
    void add(const uml::audio::PlaybackCompletion& completion)
    {
        low_ = std::min(low_, completion.latencyBytes);
        high_ = std::max(high_, completion.latencyBytes);
    }

    void check(const char* stream) const
    {
        // Stereo 16-bit frames at 48 kHz.
        constexpr double bytesPerMillisecond = 48.0 * 4.0;
        std::printf("%s: every period reports %.1f to %.1f ms of output latency\n",
                stream, low_ / bytesPerMillisecond, high_ / bytesPerMillisecond);
        require(low_ == high_,
                "the Apple output's reported latency changed with the queue");
        require(high_ > 0 && high_ <= 5000 * bytesPerMillisecond,
                "the Apple output reported no latency of its own");
    }

private:
    std::uint32_t low_ {UINT32_MAX};
    std::uint32_t high_ {};
};

void playback(uml::audio::PlaybackSink& sink, uml::audio::PeriodClock& clock)
{
    constexpr std::uint32_t frames = 1024;
    constexpr std::uint32_t periodBytes = frames * 2 * sizeof(std::int16_t);
    constexpr std::uint64_t periods = 32;
    require(sink.configure({.bufferBytes = periodBytes * 4,
                .periodBytes = periodBytes, .sampleRate = 48000,
                .channels = 2, .bytesPerSample = 2}),
            "Apple audio format configuration failed");
    require(sink.prepare(), "Apple audio preparation failed");
    std::array<std::uint8_t, periodBytes> pcm {};
    std::uint64_t queued {};
    std::set<std::uint64_t> completed;
    Latency latency;
    bool started {};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (completed.size() != periods && std::chrono::steady_clock::now() < deadline) {
        while (queued != periods) {
            for (std::uint32_t frame = 0; frame < frames; ++frame) {
                for (std::uint32_t channel = 0; channel < 2; ++channel) {
                    const auto phase = static_cast<double>(queued * frames + frame) /
                        48000.0 * (channel == 0 ? 220.0 : 440.0);
                    const auto sample = static_cast<std::int16_t>(
                        1000.0 * std::sin(phase * 6.283185307179586));
                    std::memcpy(pcm.data() + (frame * 2 + channel) * sizeof(sample),
                            &sample, sizeof(sample));
                }
            }
            const auto result = sink.enqueue(queued + 1, pcm);
            require(result != uml::audio::PlaybackEnqueueResult::failed &&
                    result != uml::audio::PlaybackEnqueueResult::discarded,
                    "Apple audio rejected a PCM period");
            if (result == uml::audio::PlaybackEnqueueResult::retry)
                break;
            ++queued;
        }
        if (!started) {
            require(sink.start(), "Apple audio output engine failed to start");
            started = true;
        }
        pollfd descriptor {clock.descriptor(), POLLIN, 0};
        require(poll(&descriptor, 1, 50) >= 0, "Apple audio completion poll failed");
        clock.drain();
        sink.handleEvents();
        uml::audio::PlaybackCompletion completion;
        while (sink.takeCompletion(completion)) {
            require(completion.outcome == uml::audio::PlaybackOutcome::played &&
                    completion.requestId > 0 &&
                    completion.requestId <= queued, "invalid Apple audio completion");
            require(completed.insert(completion.requestId).second,
                    "duplicate Apple audio completion");
            latency.add(completion);
        }
    }
    require(completed.size() == periods, "Apple audio playback did not complete");
    sink.stop();
    sink.release();
    latency.check("Apple playback");
    std::printf("PASS Apple playback: %zu stereo PCM periods consumed\n", completed.size());
}
/* A low-latency guest keeps four 5 ms periods queued and hands one back as
 * soon as one completes. The output must never run dry: its IO cycle has to
 * leave the guest time to refill (OutputCycle), where the usual 512-frame
 * cycle would consume more than the buffer holds. */
void lowLatencyPlayback(uml::audio::PlaybackSink& sink, uml::audio::PeriodClock& clock)
{
    constexpr std::uint32_t frames = 240;
    constexpr std::uint32_t periods = 4;
    constexpr std::uint32_t periodBytes = frames * 2 * sizeof(std::int16_t);
    require(sink.configure({.bufferBytes = periodBytes * periods,
                .periodBytes = periodBytes, .sampleRate = 48000,
                .channels = 2, .bytesPerSample = 2}),
            "Apple audio low-latency configuration failed");
    require(sink.prepare(), "Apple audio low-latency preparation failed");
    std::array<std::uint8_t, periodBytes> pcm {};
    std::uint64_t next = 1;
    std::uint64_t phase = 0;
    std::uint32_t pending = 0;
    const auto topUp = [&] {
        while (pending != periods) {
            for (std::uint32_t frame = 0; frame < frames; ++frame, ++phase) {
                const auto sample = static_cast<std::int16_t>(1000.0 *
                    std::sin(static_cast<double>(phase) / 48000.0 * 440.0 * 6.283185307179586));
                for (std::uint32_t channel = 0; channel < 2; ++channel)
                    std::memcpy(pcm.data() + (frame * 2 + channel) * sizeof(sample),
                            &sample, sizeof(sample));
            }
            const auto result = sink.enqueue(next, pcm);
            require(result != uml::audio::PlaybackEnqueueResult::failed &&
                    result != uml::audio::PlaybackEnqueueResult::discarded,
                    "Apple audio rejected a low-latency period");
            if (result == uml::audio::PlaybackEnqueueResult::retry)
                return;
            ++next;
            ++pending;
        }
    };
    topUp();
    require(sink.start(), "Apple audio output engine failed to start");
    const auto started = std::chrono::steady_clock::now();
    // The engine's start-up is not playback.
    const auto settled = started + std::chrono::milliseconds(500);
    const auto end = started + std::chrono::milliseconds(2500);
    std::uint64_t completedSinceSettled {};
    std::uint64_t ranDry {};
    Latency latency;
    while (std::chrono::steady_clock::now() < end) {
        pollfd descriptor {clock.descriptor(), POLLIN, 0};
        require(poll(&descriptor, 1, 50) >= 0, "Apple audio completion poll failed");
        clock.drain();
        sink.handleEvents();
        bool completed {};
        uml::audio::PlaybackCompletion completion;
        while (sink.takeCompletion(completion)) {
            require(completion.outcome == uml::audio::PlaybackOutcome::played,
                    "Apple audio dropped a low-latency period");
            latency.add(completion);
            --pending;
            completed = true;
            if (std::chrono::steady_clock::now() >= settled)
                ++completedSinceSettled;
        }
        if (completed && std::chrono::steady_clock::now() >= settled && pending == 0)
            ++ranDry;
        if (completed)
            topUp();
    }
    sink.stop();
    sink.release();
    // Two seconds of 240-frame periods at 48 kHz.
    std::printf("Apple low-latency playback: %llu periods in 2 s (400 expected), "
                "%llu times nothing was left queued\n",
            static_cast<unsigned long long>(completedSinceSettled),
            static_cast<unsigned long long>(ranDry));
    require(ranDry == 0, "the Apple output ran out of low-latency periods");
    require(completedSinceSettled >= 380 && completedSinceSettled <= 420,
            "the Apple output did not play low-latency periods in real time");
    latency.check("Apple low-latency playback");
}
} // namespace

int main()
{
    try {
        const auto clock = uml::audio::makePeriodClock();
        auto sink = uml::audio::makeApplePlaybackSink(clock);
        playback(*sink, *clock);
        playback(*sink, *clock);
        lowLatencyPlayback(*sink, *clock);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
