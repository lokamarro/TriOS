// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_encoder_test_support.hpp"
#include "video_encoder_testing.hpp"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

/* The encoder's answer to losing its VideoToolbox session. The platform's
 * encoder is built with a hook that makes the next outputs report the session
 * gone, which is what the system does when it revokes the hardware. One loss
 * is recovered without the caller noticing beyond a key frame; a second with
 * no output between is final, every frame in flight is answered with an error,
 * and a reset starts over. */

namespace {
using namespace uml::testing::video_encoder;

VideoEncodeSettings settings(unsigned bFrames = 0)
{
    auto s = settingsFor(VideoEncodeProfile::h264High, RawVideoLayout::nv12, 128, 96);
    s.color = {VideoColorPrimaries::bt709, VideoTransferFunction::bt709, VideoMatrixCoefficients::bt709, false};
    s.bFrames = bFrames;
    return s;
}

// The session discovery would have been consulted for is not under test here.
std::unique_ptr<VideoEncoder> open(const VideoEncodeSettings& s)
{
    auto encoder = makeVideoEncoder();
    const auto status = static_cast<apple_encoder::ProbeEncoder&>(*encoder).configureUnchecked(s);
    require(status == VideoEncodeStatus::ok, "configure");
    return encoder;
}

Output feed(VideoEncoder& encoder, unsigned count, std::uint64_t firstTimestamp, const Picture& base)
{
    Feed f;
    f.count = count;
    f.fill = [&](WritableVideoFrame& frame, unsigned index) { writePicture(frame, shifted(base, index * 2), RawVideoLayout::nv12); };
    f.timestamp = [firstTimestamp](unsigned index) { return firstTimestamp + index; };
    f.timeoutMilliseconds = 30'000;
    return run(encoder, f);
}

void requireWhole(const Output& output, unsigned count, std::uint64_t firstTimestamp)
{
    require(!output.timedOut, "the encoder finished");
    require(output.errors == 0, "no frame was lost");
    const auto frames = output.pictures();
    require(frames.size() == count, "every frame came out");
    std::set<std::uint64_t> seen;
    for (const auto* frame : frames) {
        require(frame->timestamp >= firstTimestamp && frame->timestamp < firstTimestamp + count, "only submitted timestamps");
        require(seen.insert(frame->timestamp).second, "each once");
    }
    require(output.drained == 1, "the drain completed");
}

void oneLossIsRecovered()
{
    std::puts("== one loss");
    const auto base = fixturePicture(false);
    auto encoder = open(settings());
    ::uml::platform::testing::loseSession(*encoder, 1);
    const auto output = feed(*encoder, 12, 0, base);
    requireWhole(output, 12, 0);
    const auto frames = output.pictures();
    for (unsigned i = 0; i < 12; ++i) require(frames[i]->timestamp == i, "in order");
    require(frames[0]->type == VideoFrameType::key && hasUnit(VideoCodec::h264, *frames[0], bits::h264NalSps),
            "the stream resumes at a key frame with its parameter sets");
    if (hardwareDecodes(VideoEncodeProfile::h264High)) {
        std::vector<Picture> sources;
        for (unsigned i = 0; i < 12; ++i) sources.push_back(shifted(base, i * 2));
        require(meanLumaPsnr(sources, decode(VideoCodec::h264, output)) >= 32.0, "and decodes to the source");
    }
}

void lossInTheMiddle()
{
    std::puts("== loss mid-stream");
    const auto base = fixturePicture(false);
    auto encoder = open(settings());
    const auto before = feed(*encoder, 6, 0, base);
    requireWhole(before, 6, 0);
    ::uml::platform::testing::loseSession(*encoder, 1);
    const auto after = feed(*encoder, 6, 100, base);
    requireWhole(after, 6, 100);
    const auto frames = after.pictures();
    require(frames[0]->timestamp == 100 && frames[0]->type == VideoFrameType::key, "the frame after the loss is a key frame");
    require(hasUnit(VideoCodec::h264, *frames[0], bits::h264NalSps), "carrying the parameter sets");
    for (unsigned i = 1; i < 6; ++i) require(frames[i]->type == VideoFrameType::predicted, "and the rest follow it");
    // Both halves decode: the second starts at the key frame the recovery made.
    if (hardwareDecodes(VideoEncodeProfile::h264High)) {
        require(decode(VideoCodec::h264, before).size() == 6 && decode(VideoCodec::h264, after).size() == 6, "both halves decode");
    }
}

void aSecondLossIsFinal()
{
    std::puts("== two losses in a row");
    const auto base = fixturePicture(false);
    auto encoder = open(settings());
    ::uml::platform::testing::loseSession(*encoder, 2);
    const auto output = feed(*encoder, 8, 0, base);
    require(!output.timedOut, "the encoder finished");
    require(output.pictures().empty(), "no frame comes out of a stream that was lost");
    require(output.errors == 8, "every frame is answered with an error");
    require(output.drained == 1, "the drain still completes");

    // A reset is a new stream.
    encoder->reset();
    const auto again = feed(*encoder, 5, 500, base);
    requireWhole(again, 5, 500);
    require(again.pictures()[0]->type == VideoFrameType::key, "the new stream starts at a key frame");
}

void lossesWithOutputBetweenAreEachRecovered()
{
    std::puts("== separate losses");
    const auto base = fixturePicture(false);
    auto encoder = open(settings());
    for (unsigned round = 0; round < 3; ++round) {
        ::uml::platform::testing::loseSession(*encoder, 1);
        const auto output = feed(*encoder, 4, 1000 * (round + 1), base);
        requireWhole(output, 4, 1000 * (round + 1));
    }
}

void lossWithReordering()
{
    std::puts("== loss with B-frames in flight");
    const auto base = fixturePicture(false);
    auto encoder = open(settings(2));
    ::uml::platform::testing::loseSession(*encoder, 1);
    const auto output = feed(*encoder, 24, 0, base);
    requireWhole(output, 24, 0);
    if (hardwareDecodes(VideoEncodeProfile::h264High)) {
        std::vector<Picture> sources;
        for (unsigned i = 0; i < 24; ++i) sources.push_back(shifted(base, i * 2));
        require(meanLumaPsnr(sources, decode(VideoCodec::h264, output)) >= 32.0, "the recovered stream decodes");
    }
}

void lossBeforeDrainNotice()
{
    std::puts("== loss noticed only by the drain");
    const auto base = fixturePicture(false);
    auto encoder = open(settings());
    ::uml::platform::testing::loseSession(*encoder, 1);
    // Three frames fit in flight; the drain is the first call after the loss.
    for (unsigned i = 0; i < 3; ++i) {
        auto frame = encoder->acquireFrame();
        require(frame != nullptr, "frame");
        writePicture(*frame, shifted(base, i * 2), RawVideoLayout::nv12);
        encoder->submit(std::move(frame), i, false);
    }
    encoder->drain();
    Output output;
    for (unsigned waited = 0; !output.drained && waited < 600; ++waited) collect(*encoder, output, 50);
    requireWhole(output, 3, 0);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: uml_video_encoder_recovery_tests FIXTURE_DIRECTORY\n");
        return 2;
    }
    fixtureDirectory = argv[1];
    try {
        oneLossIsRecovered();
        lossInTheMiddle();
        aSecondLossIsFinal();
        lossesWithOutputBetweenAreEachRecovered();
        lossWithReordering();
        lossBeforeDrainNotice();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("video encoder recovery ok");
    return 0;
}
