// SPDX-License-Identifier: GPL-2.0-or-later

#include "playback_output_cycle.hpp"
#include "require.hpp"

#include <cstdint>

namespace {

using uml::audio::OutputCycle;
using uml::audio::PlaybackParameters;
using uml::testing::require;

// A stereo 16-bit stream at 48 kHz of `periods` periods of `frames` frames.
constexpr PlaybackParameters stream(std::uint32_t frames, std::uint32_t periods)
{
    return {
        .bufferBytes = frames * periods * 4U,
        .periodBytes = frames * 4U,
        .sampleRate = 48000,
        .channels = 2,
        .bytesPerSample = 2,
    };
}

// A low-latency guest buffer gets a cycle of a quarter of it, so the output
// never holds more of the guest's buffer than the guest has left to refill.
void cycleIsAQuarterOfTheGuestBuffer()
{
    require(OutputCycle::frames(stream(240, 4)) == 240,
        "four 5 ms periods give a 5 ms cycle");
    require(OutputCycle::frames(stream(240, 2)) == 120,
        "two 5 ms periods give a 2.5 ms cycle");
    require(OutputCycle::frames(stream(480, 2)) == 240,
        "two 10 ms periods give a 5 ms cycle");
    require(OutputCycle::seconds(stream(240, 4)) == 240.0 / 48000.0,
        "the cycle in seconds is its frames at the stream's rate");
}

// A large buffer does not lengthen the cycle past the usual output default,
// and a tiny one does not shorten it past what an output can serve.
void cycleIsBounded()
{
    require(OutputCycle::frames(stream(1024, 4)) == OutputCycle::maximumFrames,
        "an 85 ms buffer keeps the 512-frame cycle");
    require(OutputCycle::frames(stream(8192, 128)) == OutputCycle::maximumFrames,
        "the largest buffer keeps the 512-frame cycle");
    require(OutputCycle::frames(stream(64, 2)) == OutputCycle::minimumFrames,
        "a 128-frame buffer still gets a 64-frame cycle");
    require(OutputCycle::frames(stream(16, 2)) == OutputCycle::minimumFrames,
        "no cycle is shorter than 64 frames");
}

// Parameters the device would refuse do not divide by zero.
void degenerateParametersAreHarmless()
{
    PlaybackParameters none {};
    require(OutputCycle::frames(none) == OutputCycle::maximumFrames,
        "a stream without frames asks for the default cycle");
    require(OutputCycle::seconds(none) == 0.0,
        "a stream without a rate asks for nothing");
}

} // namespace

int main()
{
    cycleIsAQuarterOfTheGuestBuffer();
    cycleIsBounded();
    degenerateParametersAreHarmless();
    return 0;
}
