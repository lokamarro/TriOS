// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_encoder_test_support.hpp"
#include "apple/video_encoder_levels.hpp"
#include "scratch_directory.hpp"

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

/* The platform video encoder on this host's hardware.
 *
 * Every claim a stream makes is checked against the stream: a profile by its
 * SPS, a key frame by its IDR unit, HDR metadata by its SEI payload, a rate
 * by the bytes that came out, and the picture itself by decoding it with the
 * platform's hardware decoder and comparing luma against the source. Which
 * profiles, layouts and modes are exercised is what discovery reports for the
 * host, so a host without a feature skips it rather than failing. */

namespace {
using namespace uml::testing::video_encoder;
namespace lv = uml::platform::levels;

const VideoEncodeCapability* h264Capability;
const VideoEncodeCapability* hevcCapability;
const VideoEncodeCapability* jpegCapability;

constexpr double kFramesPerSecond = 30.0;

void step(const char* name)
{
    std::printf("== %s\n", name);
    std::fflush(stdout);
}

// The first of `preferred` the capability lists.
std::optional<VideoEncodeProfile> pick(const VideoEncodeCapability* capability, std::initializer_list<VideoEncodeProfile> preferred)
{
    if (!capability) return std::nullopt;
    for (const auto wanted : preferred)
        if (profileOf(*capability, wanted)) return wanted;
    return std::nullopt;
}

bool supportsLayout(const VideoEncodeCapability& capability, VideoEncodeProfile profile, RawVideoLayout layout)
{
    const auto* entry = profileOf(capability, profile);
    return entry && std::find(entry->inputs.begin(), entry->inputs.end(), layout) != entry->inputs.end();
}

VideoEncodeSettings baseSettings(VideoEncodeProfile profile, std::uint32_t width, std::uint32_t height,
                                 RawVideoLayout layout = RawVideoLayout::nv12)
{
    auto settings = settingsFor(profile, layout, width, height);
    settings.color = {VideoColorPrimaries::bt709, VideoTransferFunction::bt709, VideoMatrixCoefficients::bt709, false};
    return settings;
}

std::vector<Picture> sourcesOf(const Encoded& encoded) { return encoded.sources; }

// Decodes a stream and returns the mean luma PSNR against its sources, or
// nothing when this host has no hardware decoder for the profile.
std::optional<double> roundtripPsnr(VideoEncodeProfile profile, const Encoded& encoded)
{
    if (!hardwareDecodes(profile)) return std::nullopt;
    const auto decoded = decode(codecOf(profile), encoded.output);
    return meanLumaPsnr(sourcesOf(encoded), decoded);
}

void expectOrdered(const Output& output, unsigned count, bool keyFirst = true)
{
    const auto frames = output.pictures();
    require(output.errors == 0, "the encoder reported an error");
    require(frames.size() == count, "the encoder returned a different number of frames");
    if (keyFirst) require(frames.front()->type == VideoFrameType::key, "the first frame is not a key frame");
    for (unsigned i = 0; i < count; ++i)
        require(frames[i]->timestamp == i, "frames are not returned in submission order");
}

// ---- Capability invariants.

void capabilityInvariants()
{
    step("capability invariants");
    const auto capabilities = videoEncodeCapabilities();
    require(!capabilities.empty(), "no hardware video encoder available");
    h264Capability = hevcCapability = jpegCapability = nullptr;
    static const auto stored = capabilities;   // the tests point into this copy
    for (const auto& capability : stored) {
        require(capability.codec == VideoCodec::h264 || capability.codec == VideoCodec::hevc || capability.codec == VideoCodec::jpeg,
                "capability for a codec the interface cannot encode");
        const bool jpeg = capability.codec == VideoCodec::jpeg;
        std::printf("%s: %ux%u..%ux%u align %u, %zu profiles, %u B-frames, %u layers, %u sessions, %u/%u fps\n",
                    jpeg ? "jpeg" : capability.codec == VideoCodec::hevc ? "hevc" : "h264",
                    capability.minimumWidth, capability.minimumHeight, capability.maximumWidth, capability.maximumHeight,
                    capability.sizeAlignment, capability.profiles.size(), capability.maximumBFrames,
                    capability.maximumTemporalLayers, capability.concurrentSessions,
                    capability.measuredFramesPerSecond720p, capability.measuredFramesPerSecond1080p);
        require(!capability.profiles.empty(), "a codec with no profile is not listed");
        require(capability.minimumWidth > 0 && capability.minimumHeight > 0, "minimum size");
        // A video picture below the floor encodes but fails a guest's quality check, so it is never listed.
        require(jpeg || (capability.minimumWidth >= kVideoEncodeMinimumSide && capability.minimumHeight >= kVideoEncodeMinimumSide),
                "a video capability lists a picture below the floor");
        require(capability.minimumWidth <= capability.maximumWidth && capability.minimumHeight <= capability.maximumHeight, "size range");
        require(capability.sizeAlignment >= 1, "alignment");
        require(capability.minimumWidth % capability.sizeAlignment == 0 && capability.minimumHeight % capability.sizeAlignment == 0,
                "the minimum size is a multiple of the alignment");
        require(capability.maximumWidth % capability.sizeAlignment == 0 && capability.maximumHeight % capability.sizeAlignment == 0,
                "the maximum size is a multiple of the alignment");
        require(capability.concurrentSessions >= 1, "at least one session");
        require(!capability.rateControls.empty(), "rate controls");
        for (const auto& profile : capability.profiles) {
            require(codecOf(profile.profile) == capability.codec, "profile belongs to the codec");
            require(!profile.inputs.empty(), "a profile with no input layout is not listed");
            require(jpeg == (profile.maximumLevel == 0), "only JPEG has no level");
            require(!jpeg || !profile.levelSelectable, "JPEG has no level to select");
            const auto lists = [&](RawVideoLayout layout) {
                return std::find(profile.inputs.begin(), profile.inputs.end(), layout) != profile.inputs.end();
            };
            // Both packed orders reach the encoder through one pool, so one is not offered without the other.
            require(lists(RawVideoLayout::bgra) == lists(RawVideoLayout::rgba), "BGRA and RGBA input are offered together");
            require(profile.profile != VideoEncodeProfile::hevcMain10 || lists(RawVideoLayout::p010), "HEVC Main 10 takes 10-bit input");
        }
        if (jpeg) {
            require(capability.rateControls.size() == 1 && capability.rateControls[0] == VideoRateControl::constantQuality,
                    "JPEG is constant quality");
            jpegCapability = &capability;
            continue;
        }
        require(capability.maximumBitrate > 0, "maximum bitrate");
        require(capability.minimumQp <= capability.maximumQp && capability.maximumQp <= 51, "QP range");
        require(capability.maximumTemporalLayers >= 1, "temporal layers");
        (capability.codec == VideoCodec::hevc ? hevcCapability : h264Capability) = &capability;
    }
    require(h264Capability, "this host has no hardware H.264 encoder");
    // A second query returns the same facts.
    const auto again = videoEncodeCapabilities();
    require(again.size() == stored.size(), "capabilities changed between queries");
    for (std::size_t i = 0; i < again.size(); ++i)
        require(again[i].codec == stored[i].codec && again[i].profiles.size() == stored[i].profiles.size() &&
                again[i].maximumWidth == stored[i].maximumWidth && again[i].maximumBitrate == stored[i].maximumBitrate,
                "capabilities changed between queries");
}

// ---- Every advertised profile and input layout.

void profileLayoutRoundtrip()
{
    step("every profile and layout");
    unsigned cases = 0;
    for (const auto* capability : {h264Capability, hevcCapability, jpegCapability}) {
        if (!capability) continue;
        for (const auto& profile : capability->profiles) {
            for (const auto layout : profile.inputs) {
                const bool jpeg = capability->codec == VideoCodec::jpeg;
                const unsigned count = jpeg ? 3 : 6;
                auto settings = baseSettings(profile.profile, 128, 96, layout);
                settings.rateControl = jpeg ? VideoRateControl::constantQuality : VideoRateControl::variableBitrate;
                settings.bitrate = 4'000'000;
                const std::string label = std::string(profileName(profile.profile)) + "/" + layoutName(layout);
                const auto encoded = encode(settings, count, fixtureSource(layout), label.c_str());
                expectOrdered(encoded.output, count);
                const auto frames = encoded.output.pictures();
                if (jpeg) {
                    for (const auto* frame : frames)
                        require(frame->bytes.size() > 4 && frame->bytes[0] == 0xff && frame->bytes[1] == 0xd8, "JPEG starts with SOI");
                } else {
                    require(streamWritesProfile(profile.profile, *frames.front()), "the SPS does not state the requested profile");
                    for (std::size_t i = 1; i < frames.size(); ++i)
                        require(frames[i]->type == VideoFrameType::predicted, "later frames are predicted");
                }
                const auto psnr = roundtripPsnr(profile.profile, encoded);
                std::printf("  %-30s %6zu bytes, PSNR %s%.1f dB\n", label.c_str(), encoded.output.bytes(0, count),
                            psnr ? "" : "(no decoder) ", psnr.value_or(0));
                if (psnr) require(*psnr >= (layout == RawVideoLayout::bgra || layout == RawVideoLayout::rgba ? 26.0 : 32.0), "the decoded picture differs from the source");
                ++cases;
            }
        }
    }
    require(cases > 0, "no profile was exercised");
}

// ---- Rate control.

void rateControlModes()
{
    step("rate control modes");
    const auto profile = pick(h264Capability, {VideoEncodeProfile::h264High, VideoEncodeProfile::h264Main, VideoEncodeProfile::h264Baseline});
    require(profile.has_value(), "no H.264 profile to measure with");
    const std::uint32_t width = 640, height = 360;
    const unsigned count = 60;
    const auto measure = [&](auto&& configure, const char* label) {
        auto settings = baseSettings(*profile, width, height);
        configure(settings);
        auto encoded = encode(settings, count, busySource(width, height), label);
        expectOrdered(encoded.output, count);
        const double rate = bitsPerSecond(encoded.output, kFramesPerSecond);
        const auto psnr = roundtripPsnr(*profile, encoded);
        std::printf("  %-26s %9.0f bit/s, PSNR %.1f dB\n", label, rate, psnr.value_or(0));
        return std::pair {rate, psnr.value_or(0)};
    };

    const auto low = measure([](auto& s) { s.bitrate = 1'000'000; }, "vbr 1 Mbit/s");
    const auto high = measure([](auto& s) { s.bitrate = 6'000'000; }, "vbr 6 Mbit/s");
    require(high.first > low.first * 1.5, "a higher target produces a higher rate");
    if (hardwareDecodes(*profile)) require(high.second > low.second + 1.0, "a higher target produces a better picture");

    if (advertises(*h264Capability, VideoRateControl::constantBitrate)) {
        const auto cbr = measure([](auto& s) { s.rateControl = VideoRateControl::constantBitrate; s.bitrate = 1'000'000; }, "cbr 1 Mbit/s");
        require(cbr.first > 500'000 && cbr.first < 1'600'000, "constant bitrate holds its target");
    }
    if (advertises(*h264Capability, VideoRateControl::constantQuality)) {
        const auto weak = measure([](auto& s) { s.rateControl = VideoRateControl::constantQuality; s.quality = 15; }, "quality 15");
        const auto strong = measure([](auto& s) { s.rateControl = VideoRateControl::constantQuality; s.quality = 90; }, "quality 90");
        require(strong.first > weak.first * 1.5, "higher quality produces more bytes");
        if (hardwareDecodes(*profile)) require(strong.second > weak.second + 2.0, "higher quality produces a better picture");
    }
    if (advertises(*h264Capability, VideoRateControl::constantQp)) {
        const auto fine = measure([](auto& s) { s.rateControl = VideoRateControl::constantQp; s.frameQp = {20, 20, 20}; }, "qp 20");
        const auto coarse = measure([](auto& s) { s.rateControl = VideoRateControl::constantQp; s.frameQp = {38, 38, 38}; }, "qp 38");
        require(fine.first > coarse.first * 2.0, "a lower QP produces more bytes");
        if (hardwareDecodes(*profile)) require(fine.second > coarse.second + 3.0, "a lower QP produces a better picture");
    }

    // The bounds work against the rate: a maximum QP forces bytes the target
    // alone would not spend, and a minimum QP holds back a rate it would.
    const auto starved = measure([](auto& s) { s.bitrate = 150'000; }, "vbr 150 kbit/s");
    const auto bounded = measure([](auto& s) { s.bitrate = 150'000; s.maximumQp = 26; }, "vbr 150k, max QP 26");
    require(bounded.first > starved.first * 1.5, "a maximum QP overrides a starved target");
    const auto floored = measure([](auto& s) { s.bitrate = 20'000'000; s.minimumQp = 34; }, "vbr 20M, min QP 34");
    require(floored.first < 6'000'000, "a minimum QP holds a generous target back");
}

void bitrateUpdate()
{
    step("bitrate and frame rate updates");
    const auto profile = pick(h264Capability, {VideoEncodeProfile::h264High, VideoEncodeProfile::h264Main, VideoEncodeProfile::h264Baseline});
    require(profile.has_value(), "no H.264 profile");
    const std::uint32_t width = 320, height = 240;
    auto settings = baseSettings(*profile, width, height);
    settings.bitrate = 1'500'000;
    settings.rateControl = VideoRateControl::variableBitrate;
    Feed feed;
    feed.before = [](VideoEncoder& encoder, unsigned index) {
        if (index == 30) {
            VideoEncodeUpdate update;
            update.bitrate = 300'000;
            require(encoder.update(update) == VideoEncodeStatus::ok, "lowering the bitrate");
        }
        if (index == 60) {
            VideoEncodeUpdate update;
            update.bitrate = 3'000'000;
            require(encoder.update(update) == VideoEncodeStatus::ok, "raising the bitrate");
        }
    };
    const auto encoded = encode(settings, 90, busySource(width, height), "bitrate update", feed);
    expectOrdered(encoded.output, 90);
    const double before = double(encoded.output.bytes(15, 30)) / 15, low = double(encoded.output.bytes(45, 60)) / 15,
                 after = double(encoded.output.bytes(75, 90)) / 15;
    std::printf("  bytes per frame: %.0f -> %.0f -> %.0f\n", before, low, after);
    require(low < before * 0.7, "a lower target takes effect");
    require(after > low * 1.5, "a higher target takes effect");

    // Updates the session must refuse.
    auto encoder = open(settings, "update refusals");
    VideoEncodeUpdate update;
    update.bitrate = 0;
    require(encoder->update(update) == VideoEncodeStatus::invalid, "a zero bitrate is invalid");
    update = {};
    update.bitrate = 2'000'000;
    update.peakBitrate = 1'000'000;
    require(encoder->update(update) == VideoEncodeStatus::invalid, "a peak below the target is invalid");
    update = {};
    update.bitrate = h264Capability->maximumBitrate + 1;
    require(encoder->update(update) == VideoEncodeStatus::invalid, "a bitrate above the verified maximum is invalid");
    update = {};
    update.frameRateDenominator = 0;
    require(encoder->update(update) == VideoEncodeStatus::invalid, "a zero frame-rate denominator is invalid");
    update = {};
    update.frameRateNumerator = 60;
    update.frameRateDenominator = 1;
    require(encoder->update(update) == VideoEncodeStatus::ok, "a frame rate change");
    update = {};
    update.bitrate = 800'000;
    update.peakBitrate = 1'600'000;
    require(encoder->update(update) == VideoEncodeStatus::ok, "a bitrate with a peak");
    require(makeVideoEncoder()->update(update) == VideoEncodeStatus::invalid, "an unconfigured encoder has nothing to update");
}

void qualityAndQpUpdate()
{
    step("quality and QP updates");
    const auto profile = pick(h264Capability, {VideoEncodeProfile::h264High, VideoEncodeProfile::h264Main, VideoEncodeProfile::h264Baseline});
    require(profile.has_value(), "no H.264 profile");
    const std::uint32_t width = 320, height = 240;
    if (advertises(*h264Capability, VideoRateControl::constantQuality)) {
        auto settings = baseSettings(*profile, width, height);
        settings.rateControl = VideoRateControl::constantQuality;
        settings.quality = 20;
        Feed feed;
        feed.before = [](VideoEncoder& encoder, unsigned index) {
            if (index != 20) return;
            VideoEncodeUpdate update;
            update.quality = 85;
            require(encoder.update(update) == VideoEncodeStatus::ok, "raising the quality");
        };
        const auto encoded = encode(settings, 40, busySource(width, height), "quality update", feed);
        expectOrdered(encoded.output, 40);
        const double before = double(encoded.output.bytes(5, 20)), after = double(encoded.output.bytes(25, 40));
        std::printf("  quality 20 -> 85: %.0f -> %.0f bytes per 15 frames\n", before, after);
        require(after > before * 1.5, "a higher quality takes effect while encoding");
        auto encoder = open(settings, "quality refusals");
        VideoEncodeUpdate update;
        update.quality = 0;
        require(encoder->update(update) == VideoEncodeStatus::invalid, "quality 0 is invalid");
        update.quality = 101;
        require(encoder->update(update) == VideoEncodeStatus::invalid, "quality 101 is invalid");
    }
    {
        auto settings = baseSettings(*profile, width, height);
        settings.bitrate = 200'000;
        Feed feed;
        feed.before = [](VideoEncoder& encoder, unsigned index) {
            if (index != 20) return;
            VideoEncodeUpdate update;
            update.maximumQp = 24;
            require(encoder.update(update) == VideoEncodeStatus::ok, "bounding the QP");
        };
        const auto encoded = encode(settings, 40, busySource(width, height), "QP update", feed);
        expectOrdered(encoded.output, 40);
        const double before = double(encoded.output.bytes(5, 20)), after = double(encoded.output.bytes(25, 40));
        std::printf("  max QP 24 from frame 20: %.0f -> %.0f bytes per 15 frames\n", before, after);
        require(after > before * 1.5, "a maximum QP takes effect while encoding");
        auto encoder = open(settings, "QP refusals");
        VideoEncodeUpdate update;
        update.maximumQp = 52;
        require(encoder->update(update) == VideoEncodeStatus::invalid, "a QP above 51 is invalid");
    }
}

// ---- Key frames.

bool isKeyUnit(VideoCodec codec, unsigned type)
{
    return codec == VideoCodec::hevc ? type >= 16 && type <= 21 : type == bits::h264NalIdr;
}

bool hasKeyUnit(VideoCodec codec, const EncodedVideoFrame& frame)
{
    for (const auto& unit : nalUnits(codec, frame))
        if (isKeyUnit(codec, unit.first)) return true;
    return false;
}

void keyFrames()
{
    step("key frames");
    for (const auto* capability : {h264Capability, hevcCapability}) {
        if (!capability) continue;
        const auto profile = pick(capability, {VideoEncodeProfile::h264High, VideoEncodeProfile::h264Main, VideoEncodeProfile::h264Baseline,
                                               VideoEncodeProfile::hevcMain});
        require(profile.has_value(), "no profile");
        const VideoCodec codec = capability->codec;
        // A cadence: no gap longer than the interval, and a key frame is an
        // IDR or IRAP unit while every other frame has none.
        {
            auto settings = baseSettings(*profile, 128, 96);
            settings.keyFrameInterval = 10;
            const auto encoded = encode(settings, 35, fixtureSource(RawVideoLayout::nv12), "key interval");
            expectOrdered(encoded.output, 35);
            const auto frames = encoded.output.pictures();
            unsigned keys = 0, last = 0;
            for (unsigned i = 0; i < frames.size(); ++i) {
                const bool key = frames[i]->type == VideoFrameType::key;
                require(key == hasKeyUnit(codec, *frames[i]), "a key frame carries an IDR unit and no other frame does");
                if (!key) continue;
                require(i - last <= 10, "key frames are no further apart than the interval");
                last = i;
                ++keys;
            }
            require(35 - 1 - last <= 10, "the stream does not end a whole interval past its last key frame");
            require(keys >= 3, "an interval of 10 over 35 frames has at least three key frames");
        }
        // Requests: only the first frame and the frames asked for.
        {
            auto settings = baseSettings(*profile, 128, 96);
            settings.keyFrameInterval = 0;
            Feed feed;
            feed.forceKey = [](unsigned index) { return index == 7 || index == 19; };
            const auto encoded = encode(settings, 30, fixtureSource(RawVideoLayout::nv12), "forced key frames", feed);
            expectOrdered(encoded.output, 30);
            const auto frames = encoded.output.pictures();
            std::set<unsigned> keys;
            for (unsigned i = 0; i < frames.size(); ++i)
                if (frames[i]->type == VideoFrameType::key) keys.insert(i);
            require(keys == std::set<unsigned>({0, 7, 19}), "key frames are the first and the requested ones");
            if (hardwareDecodes(*profile)) {
                const auto psnr = meanLumaPsnr(encoded.sources, decode(codec, encoded.output));
                require(psnr >= 32.0, "a stream with forced key frames decodes");
            }
        }
    }
}

// ---- B-frames.

void bFrames()
{
    step("B-frames");
    for (const auto* capability : {h264Capability, hevcCapability}) {
        if (!capability || capability->maximumBFrames == 0) continue;
        const auto profile = pick(capability, {VideoEncodeProfile::h264High, VideoEncodeProfile::h264Main, VideoEncodeProfile::hevcMain});
        if (!profile) continue;
        auto settings = baseSettings(*profile, 320, 240);
        settings.bFrames = capability->maximumBFrames;   // the run the encoder codes
        settings.keyFrameInterval = 0;
        const unsigned count = 40;
        const auto encoded = encode(settings, count, busySource(320, 240), "B-frames");
        const auto frames = encoded.output.pictures();
        require(encoded.output.errors == 0 && frames.size() == count, "every frame is returned");
        require(frames.front()->type == VideoFrameType::key && frames.front()->timestamp == 0, "the stream starts with its first picture, a key frame");
        std::set<std::uint64_t> seen;
        unsigned bidirectional = 0, reordered = 0, run = 0, longest = 0;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            require(seen.insert(frames[i]->timestamp).second, "a timestamp is returned once");
            require(frames[i]->timestamp < count, "only submitted timestamps come back");
            if (frames[i]->type == VideoFrameType::bidirectional) {
                ++bidirectional;
                ++run;
                longest = std::max(longest, run);
            } else {
                run = 0;
            }
            if (i && frames[i]->timestamp < frames[i - 1]->timestamp) ++reordered;
        }
        std::printf("  %s: %u bidirectional of %u, %u out of order, longest run %u\n",
                    capability->codec == VideoCodec::hevc ? "hevc" : "h264", bidirectional, count, reordered, longest);
        require(bidirectional > 0 && reordered > 0, "B-frames reorder the output");
        require(longest <= capability->maximumBFrames, "no run is longer than the verified one");
        if (hardwareDecodes(*profile)) {
            const auto decoded = decode(capability->codec, encoded.output);
            require(decoded.size() == count, "the decoder returns every picture");
            require(meanLumaPsnr(encoded.sources, decoded) >= 24.0, "a stream with B-frames decodes to the source");
        }
        // The encoder cannot be asked for a shorter run: a caller who accepts
        // fewer B-frames than it codes gets none, so its output is never more
        // out of order than it was told.
        for (unsigned shorter = 1; shorter < capability->maximumBFrames; ++shorter) {
            auto limited = settings;
            limited.bFrames = shorter;
            const auto plain = encode(limited, 24, busySource(320, 240), "shorter B run");
            const auto plainFrames = plain.output.pictures();
            require(plainFrames.size() == 24, "every frame is returned");
            for (std::size_t i = 0; i < plainFrames.size(); ++i)
                require(plainFrames[i]->timestamp == i && plainFrames[i]->type != VideoFrameType::bidirectional,
                        "a run shorter than the encoder's is not coded");
        }
        // The caller's memory budget for reordering grows with it.
        auto encoder = open(settings, "B-frame acquire");
        unsigned acquired = 0;
        std::vector<std::unique_ptr<WritableVideoFrame>> held;
        while (auto frame = encoder->acquireFrame()) {
            held.push_back(std::move(frame));
            require(++acquired <= 16, "the in-flight limit is finite");
        }
        require(acquired >= 4, "B-frames need frames in flight");
    }
    // Profiles with no B-frames refuse them.
    if (const auto baseline = pick(h264Capability, {VideoEncodeProfile::h264Baseline, VideoEncodeProfile::h264ConstrainedBaseline})) {
        auto settings = baseSettings(*baseline, 128, 96);
        settings.bFrames = 1;
        auto encoder = makeVideoEncoder();
        require(encoder->configure(settings) == VideoEncodeStatus::invalid, "Baseline has no B-frames");
    }
}

// ---- Frames in flight, draining and resetting.

bool readable(const VideoEncoder& encoder)
{
    pollfd descriptor {encoder.readinessDescriptor(), POLLIN, 0};
    return ::poll(&descriptor, 1, 0) == 1;
}

void waitReadable(const VideoEncoder& encoder)
{
    pollfd descriptor {encoder.readinessDescriptor(), POLLIN, 0};
    require(::poll(&descriptor, 1, 20'000) == 1, "the encoder produced nothing within the bound");
}

void flow()
{
    step("flow: backpressure, drain, reset");
    const auto profile = pick(h264Capability, {VideoEncodeProfile::h264High, VideoEncodeProfile::h264Main, VideoEncodeProfile::h264Baseline});
    require(profile.has_value(), "no H.264 profile");
    const auto settings = baseSettings(*profile, 128, 96);
    const auto picture = fixturePicture(false);
    const auto fill = [&](WritableVideoFrame& frame, unsigned index) { writePicture(frame, shifted(picture, index * 2), RawVideoLayout::nv12); };

    // Draining an encoder with nothing in flight answers at once, and the
    // descriptor is readable exactly while an answer waits.
    {
        auto encoder = open(settings, "flow");
        require(!readable(*encoder), "a new encoder has nothing to read");
        encoder->drain();
        waitReadable(*encoder);
        auto items = encoder->takeFrames();
        require(items.size() == 1 && items[0].kind == EncodedVideoFrameKind::drained, "an empty drain is answered");
        require(!readable(*encoder), "taking the output clears the descriptor");
        require(encoder->takeFrames().empty(), "nothing is returned twice");
    }

    // The number of frames in flight is bounded, a dropped frame returns its
    // slot, and output frees the rest.
    {
        auto encoder = open(settings, "flow");
        std::vector<std::unique_ptr<WritableVideoFrame>> held;
        while (auto frame = encoder->acquireFrame()) {
            held.push_back(std::move(frame));
            require(held.size() <= 16, "the in-flight limit is finite");
        }
        const std::size_t limit = held.size();
        std::printf("  frames in flight: %zu\n", limit);
        require(limit >= 2, "a stream can have two frames in flight");
        held.pop_back();   // dropped without a submit
        auto again = encoder->acquireFrame();
        require(again != nullptr, "a dropped frame returns its slot");
        require(encoder->acquireFrame() == nullptr, "and only that one");
        held.push_back(std::move(again));
        unsigned index = 0;
        for (auto& frame : held) {
            fill(*frame, index);
            encoder->submit(std::move(frame), index, false);
            ++index;
        }
        held.clear();
        encoder->drain();
        Output output;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!output.drained && std::chrono::steady_clock::now() < deadline) collect(*encoder, output, 50);
        require(output.drained == 1, "the drain completes");
        require(output.errors == 0 && output.pictures().size() == limit, "every frame submitted before the drain is returned");
        require(output.items.size() == limit, "and nothing else");
        require(!readable(*encoder), "the descriptor clears with the output");
        require(encoder->acquireFrame() != nullptr, "output frees the slots");
    }

    // A drain follows its frames: the drained entry comes last.
    {
        auto encoder = open(settings, "flow");
        for (unsigned i = 0; i < 3; ++i) {
            auto frame = encoder->acquireFrame();
            require(frame != nullptr, "three frames fit");
            fill(*frame, i);
            encoder->submit(std::move(frame), 100 + i, false);
        }
        encoder->drain();
        std::vector<EncodedVideoFrameKind> kinds;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        bool done = false;
        while (!done && std::chrono::steady_clock::now() < deadline) {
            pollfd descriptor {encoder->readinessDescriptor(), POLLIN, 0};
            (void)::poll(&descriptor, 1, 50);
            for (auto& item : encoder->takeFrames()) {
                kinds.push_back(item.kind);
                done = done || item.kind == EncodedVideoFrameKind::drained;
            }
        }
        require(kinds.size() == 4 && kinds.back() == EncodedVideoFrameKind::drained, "the drained entry follows the frames");
        for (std::size_t i = 0; i + 1 < kinds.size(); ++i) require(kinds[i] == EncodedVideoFrameKind::frame, "frames before it");
    }

    // A reset discards frames in flight and output not yet taken, ignores a
    // frame acquired before it, and starts a new sequence at a key frame.
    {
        auto encoder = open(settings, "flow");
        auto stale = encoder->acquireFrame();
        require(stale != nullptr, "a frame to go stale");
        for (unsigned i = 0; i < 3; ++i) {
            auto frame = encoder->acquireFrame();
            require(frame != nullptr, "frames before the reset");
            fill(*frame, i);
            encoder->submit(std::move(frame), 500 + i, i == 1);
        }
        encoder->reset();
        fill(*stale, 0);
        encoder->submit(std::move(stale), 999, false);
        usleep(300'000);
        require(!readable(*encoder), "a reset leaves nothing to read");
        require(encoder->takeFrames().empty(), "a reset discards output");
        Feed feed;
        feed.count = 5;
        feed.fill = fill;
        feed.timestamp = [](unsigned index) { return 2000 + index; };
        const auto output = run(*encoder, feed);
        const auto frames = output.pictures();
        require(!output.timedOut && output.errors == 0 && frames.size() == 5, "the encoder works after a reset");
        for (unsigned i = 0; i < 5; ++i) require(frames[i]->timestamp == 2000 + i, "only the new sequence is returned");
        require(frames[0]->type == VideoFrameType::key, "the new sequence starts at a key frame");
        require(hasKeyUnit(VideoCodec::h264, *frames[0]) && hasUnit(VideoCodec::h264, *frames[0], bits::h264NalSps), "with its parameter sets");
    }
}

// ---- Parameter sets.

void parameterSets()
{
    step("parameter set placement");
    for (const auto* capability : {h264Capability, hevcCapability}) {
        if (!capability) continue;
        const auto profile = pick(capability, {VideoEncodeProfile::h264High, VideoEncodeProfile::h264Main, VideoEncodeProfile::h264Baseline,
                                               VideoEncodeProfile::hevcMain});
        require(profile.has_value(), "no profile");
        const VideoCodec codec = capability->codec;
        const std::vector<unsigned> required = codec == VideoCodec::hevc
            ? std::vector<unsigned> {bits::hevcNalVps, bits::hevcNalSps, bits::hevcNalPps}
            : std::vector<unsigned> {bits::h264NalSps, bits::h264NalPps};
        const auto carries = [&](const EncodedVideoFrame& frame) {
            for (const auto type : required)
                if (!hasUnit(codec, frame, type)) return false;
            return true;
        };
        const auto carriesAny = [&](const EncodedVideoFrame& frame) {
            for (const auto type : required)
                if (hasUnit(codec, frame, type)) return true;
            return false;
        };
        for (const bool separate : {false, true}) {
            for (const bool everyKey : {true, false}) {
                auto settings = baseSettings(*profile, 128, 96);
                settings.keyFrameInterval = 10;
                settings.separateParameterSets = separate;
                settings.parameterSetsWithEveryKeyFrame = everyKey;
                const auto encoded = encode(settings, 30, fixtureSource(RawVideoLayout::nv12), "parameter sets");
                const auto frames = encoded.output.pictures();
                require(encoded.output.errors == 0 && frames.size() == 30, "all frames are returned");
                unsigned headers = 0;
                bool firstItemIsHeaders = false;
                for (std::size_t i = 0; i < encoded.output.items.size(); ++i) {
                    const auto& item = encoded.output.items[i];
                    if (item.kind != EncodedVideoFrameKind::parameterSets) continue;
                    ++headers;
                    firstItemIsHeaders = firstItemIsHeaders || i == 0;
                    require(carries(item), "a parameter set frame carries the whole set");
                    for (const auto& unit : nalUnits(codec, item))
                        require(std::find(required.begin(), required.end(), unit.first) != required.end(), "and nothing but parameter sets");
                }
                std::printf("  %s separate=%d everyKey=%d: %u header frames\n", codec == VideoCodec::hevc ? "hevc" : "h264", int(separate),
                            int(everyKey), headers);
                if (separate) {
                    require(headers >= 1 && firstItemIsHeaders, "separate parameter sets come before the first key frame");
                } else {
                    require(headers == 0, "joined parameter sets are not frames of their own");
                }
                for (std::size_t i = 0; i < frames.size(); ++i) {
                    const bool key = frames[i]->type == VideoFrameType::key;
                    if (!key) {
                        require(!carriesAny(*frames[i]), "only key frames carry parameter sets");
                    } else if (everyKey) {
                        require(carries(*frames[i]), "every key frame carries the parameter sets");
                    } else if (!separate && i == 0) {
                        require(carries(*frames[i]), "the first key frame carries them when nothing else does");
                    } else {
                        require(!carriesAny(*frames[i]), "a later key frame does not repeat them");
                    }
                }
                if (hardwareDecodes(*profile)) {
                    const auto psnr = meanLumaPsnr(encoded.sources, decode(codec, encoded.output));
                    require(psnr >= 32.0, "the stream decodes with its parameter sets placed this way");
                }
            }
        }
    }
}

// ---- HDR static metadata.

std::vector<std::uint8_t> be16(std::initializer_list<unsigned> values)
{
    std::vector<std::uint8_t> out;
    for (const auto value : values) {
        out.push_back(std::uint8_t(value >> 8));
        out.push_back(std::uint8_t(value));
    }
    return out;
}

std::vector<std::uint8_t> be32(unsigned value)
{
    return {std::uint8_t(value >> 24), std::uint8_t(value >> 16), std::uint8_t(value >> 8), std::uint8_t(value)};
}

void hdr()
{
    step("HDR static metadata");
    if (!hevcCapability || !profileOf(*hevcCapability, VideoEncodeProfile::hevcMain10) ||
        !supportsLayout(*hevcCapability, VideoEncodeProfile::hevcMain10, RawVideoLayout::p010)) {
        std::puts("  no HEVC Main 10 from 10-bit input on this host; skipped");
        return;
    }
    VideoHdrStaticMetadata metadata;
    metadata.displayPrimaries = {13250, 34500, 7500, 3000, 34000, 16000};
    metadata.whitePointX = 15635;
    metadata.whitePointY = 16450;
    metadata.maximumLuminance = 10'000'000;
    metadata.minimumLuminance = 50;
    metadata.maximumContentLightLevel = 1000;
    metadata.maximumFrameAverageLightLevel = 400;
    std::vector<std::uint8_t> mastering = be16({13250, 34500, 7500, 3000, 34000, 16000, 15635, 16450});
    const auto maxLuminance = be32(10'000'000), minLuminance = be32(50);
    mastering.insert(mastering.end(), maxLuminance.begin(), maxLuminance.end());
    mastering.insert(mastering.end(), minLuminance.begin(), minLuminance.end());
    const auto light = be16({1000, 400});

    const auto messagesOf = [](const EncodedVideoFrame& frame) {
        std::vector<bits::SeiMessage> messages;
        for (const auto unit : bits::splitAnnexB(frame.bytes))
            if (bits::hevcNalType(unit) == bits::hevcNalPrefixSei)
                for (auto& message : bits::hevcSeiMessages(unit)) messages.push_back(std::move(message));
        return messages;
    };

    auto settings = baseSettings(VideoEncodeProfile::hevcMain10, 128, 96, RawVideoLayout::p010);
    settings.color = {VideoColorPrimaries::bt2020, VideoTransferFunction::pq, VideoMatrixCoefficients::bt2020, false};
    settings.keyFrameInterval = 10;
    {
        auto withMetadata = settings;
        withMetadata.hdr = metadata;
        const auto encoded = encode(withMetadata, 25, fixtureSource(RawVideoLayout::p010), "hdr");
        expectOrdered(encoded.output, 25);
        unsigned keys = 0;
        for (const auto* frame : encoded.output.pictures()) {
            const auto messages = messagesOf(*frame);
            bool sawMastering = false, sawLight = false;
            for (const auto& message : messages) {
                if (message.type == 137) {
                    sawMastering = true;
                    require(message.payload == mastering, "the mastering display payload is the metadata, byte for byte");
                } else if (message.type == 144) {
                    sawLight = true;
                    require(message.payload == light, "the content light level payload is the metadata");
                }
            }
            if (frame->type == VideoFrameType::key) {
                ++keys;
                require(sawMastering && sawLight, "every key frame carries both messages");
            } else {
                require(!sawMastering && !sawLight, "other frames carry neither");
            }
        }
        require(keys >= 3, "several key frames were checked");
        if (hardwareDecodes(VideoEncodeProfile::hevcMain10)) {
            const auto decoded = decode(VideoCodec::hevc, encoded.output);
            require(meanLumaPsnr(encoded.sources, decoded) >= 32.0, "an HDR stream decodes");
        }
    }
    {
        const auto encoded = encode(settings, 12, fixtureSource(RawVideoLayout::p010), "no hdr");
        for (const auto* frame : encoded.output.pictures())
            for (const auto& message : messagesOf(*frame))
                require(message.type != 137 && message.type != 144, "a stream with no HDR metadata states none");
    }
    // The metadata belongs to HEVC Main 10.
    if (const auto main = pick(hevcCapability, {VideoEncodeProfile::hevcMain})) {
        auto bad = baseSettings(*main, 128, 96);
        bad.hdr = metadata;
        auto encoder = makeVideoEncoder();
        require(encoder->configure(bad) == VideoEncodeStatus::unsupported, "HDR metadata needs Main 10");
    }
    {
        auto bad = baseSettings(VideoEncodeProfile::h264High, 128, 96);
        bad.hdr = metadata;
        auto encoder = makeVideoEncoder();
        require(encoder->configure(bad) != VideoEncodeStatus::ok, "H.264 has no HDR metadata here");
    }
}

// ---- HEVC Main 10 colour aspects: what an HDR10 or HLG stream states.

void hdrColor()
{
    step("HEVC Main 10 colour aspects");
    if (!hevcCapability || !supportsLayout(*hevcCapability, VideoEncodeProfile::hevcMain10, RawVideoLayout::p010)) {
        std::puts("  no HEVC Main 10 from 10-bit input on this host; skipped");
        return;
    }
    struct Case {
        const char* name;
        VideoEncodeColor color;
        unsigned primaries, transfer, matrix;   // the ITU-T H.273 code points the VUI states
        VideoColorMatrix decoded;
    };
    const Case cases[] = {
        {"BT.2020 PQ", {VideoColorPrimaries::bt2020, VideoTransferFunction::pq, VideoMatrixCoefficients::bt2020, false}, 9, 16, 9, VideoColorMatrix::bt2020},
        {"BT.2020 HLG", {VideoColorPrimaries::bt2020, VideoTransferFunction::hlg, VideoMatrixCoefficients::bt2020, false}, 9, 18, 9, VideoColorMatrix::bt2020},
        {"BT.2020 PQ, full range", {VideoColorPrimaries::bt2020, VideoTransferFunction::pq, VideoMatrixCoefficients::bt2020, true}, 9, 16, 9, VideoColorMatrix::bt2020},
        {"BT.709", {VideoColorPrimaries::bt709, VideoTransferFunction::bt709, VideoMatrixCoefficients::bt709, false}, 1, 1, 1, VideoColorMatrix::bt709},
    };
    for (const auto& c : cases) {
        auto settings = baseSettings(VideoEncodeProfile::hevcMain10, 128, 96, RawVideoLayout::p010);
        settings.color = c.color;
        const auto encoded = encode(settings, 4, fixtureSource(RawVideoLayout::p010), "hdr colour");
        expectOrdered(encoded.output, 4);
        const auto sps = firstUnit(VideoCodec::hevc, *encoded.output.pictures().front(), bits::hevcNalSps);
        require(sps.has_value(), "an SPS");
        const auto format = bits::parseHevcSpsFormat(*sps);
        require(format.has_value(), "the SPS can be read to its VUI");
        std::printf("  %s: %u-bit 4:2:0 %d, VUI primaries %u transfer %u matrix %u full range %d\n", c.name, format->bitDepthLuma,
                    int(format->chromaFormat == 1), format->primaries, format->transfer, format->matrix, int(format->fullRange));
        require(format->chromaFormat == 1 && format->bitDepthLuma == 10 && format->bitDepthChroma == 10, "ten-bit 4:2:0");
        require(format->vuiPresent && format->colorDescription, "the SPS states a colour description");
        require(format->primaries == c.primaries && format->transfer == c.transfer && format->matrix == c.matrix, "the VUI states what was asked for");
        require(format->fullRange == c.color.fullRange, "the range is stated");
        if (hardwareDecodes(VideoEncodeProfile::hevcMain10)) {
            const auto decoded = decode(VideoCodec::hevc, encoded.output);
            require(decoded.size() == 4, "four pictures");
            for (const auto& [timestamp, frame] : decoded) {
                require(frame.format.bitDepth == 10, "the decoder sees ten bits");
                require(frame.color.matrix == c.decoded, "the decoder reads the matrix back");
            }
            // The decoder may hand back a full-range stream in video range, so only limited range is compared sample for sample.
            if (!c.color.fullRange) require(meanLumaPsnr(encoded.sources, decoded) >= 32.0, "and the picture is right");
        }
    }
    {
        // A stream that asks for no colour description states none.
        auto settings = baseSettings(VideoEncodeProfile::hevcMain10, 128, 96, RawVideoLayout::p010);
        settings.color = {};
        const auto encoded = encode(settings, 2, fixtureSource(RawVideoLayout::p010), "no colour");
        const auto sps = firstUnit(VideoCodec::hevc, *encoded.output.pictures().front(), bits::hevcNalSps);
        require(sps.has_value(), "an SPS");
        const auto format = bits::parseHevcSpsFormat(*sps);
        require(format.has_value() && !format->colorDescription, "no colour was asked for and none is stated");
    }
    {
        // HLG with static metadata: the transfer function and the SEI messages are independent.
        VideoHdrStaticMetadata metadata;
        metadata.displayPrimaries = {13250, 34500, 7500, 3000, 34000, 16000};
        metadata.whitePointX = 15635;
        metadata.whitePointY = 16450;
        metadata.maximumLuminance = 10'000'000;
        metadata.minimumLuminance = 50;
        metadata.maximumContentLightLevel = 1000;
        metadata.maximumFrameAverageLightLevel = 400;
        auto settings = baseSettings(VideoEncodeProfile::hevcMain10, 128, 96, RawVideoLayout::p010);
        settings.color = {VideoColorPrimaries::bt2020, VideoTransferFunction::hlg, VideoMatrixCoefficients::bt2020, false};
        settings.hdr = metadata;
        const auto encoded = encode(settings, 3, fixtureSource(RawVideoLayout::p010), "hlg metadata");
        expectOrdered(encoded.output, 3);
        const auto& key = *encoded.output.pictures().front();
        bool mastering = false, light = false;
        for (const auto unit : bits::splitAnnexB(key.bytes))
            if (bits::hevcNalType(unit) == bits::hevcNalPrefixSei)
                for (const auto& message : bits::hevcSeiMessages(unit)) {
                    mastering = mastering || message.type == 137;
                    light = light || message.type == 144;
                }
        require(mastering && light, "an HLG key frame carries the static metadata");
        const auto sps = firstUnit(VideoCodec::hevc, key, bits::hevcNalSps);
        require(sps.has_value(), "an SPS");
        const auto format = bits::parseHevcSpsFormat(*sps);
        require(format.has_value() && format->transfer == 18, "and the VUI still states HLG");
    }
}

// ---- RGBA input: the memory order of Android's AB24 surfaces.

void rgbaInput()
{
    step("RGBA input");
    const auto profile = pick(h264Capability, {VideoEncodeProfile::h264High, VideoEncodeProfile::h264Main, VideoEncodeProfile::h264Baseline});
    require(profile.has_value(), "no H.264 profile");
    require(supportsLayout(*h264Capability, *profile, RawVideoLayout::rgba), "H.264 takes RGBA input");
    if (!hardwareDecodes(*profile)) {
        std::puts("  no hardware decoder to read the pictures back; skipped");
        return;
    }
    // A solid picture of one colour, as BT.709 limited-range 4:2:0 samples.
    const auto solid = [](std::uint8_t y, std::uint8_t cb, std::uint8_t cr) {
        Picture picture;
        picture.width = 128;
        picture.height = 96;
        picture.luma.assign(std::size_t(picture.width) * picture.height, y);
        picture.chroma.resize(std::size_t(picture.width) * picture.height / 2);
        for (std::size_t i = 0; i < picture.chroma.size(); i += 2) {
            picture.chroma[i] = cb;
            picture.chroma[i + 1] = cr;
        }
        return picture;
    };
    struct Color {
        const char* name;
        std::uint8_t y, cb, cr;
    };
    // Red (255, 0, 0), green (0, 255, 0) and blue (0, 0, 255): swapping red and blue exchanges the first and last.
    const Color colors[] = {{"red", 63, 102, 240}, {"green", 173, 42, 26}, {"blue", 32, 240, 118}};
    // The same three colours as BT.709 full-range samples (Y = 255 * Kr, Kg, Kb; Cb and Cr clamp at 255).
    const Color fullRangeColors[] = {{"red", 54, 99, 255}, {"green", 182, 30, 12}, {"blue", 18, 255, 116}};
    // The decoder hands pictures back in the range the stream states, so a full-range stream decodes to
    // the full-range samples of the colour that was written and a limited one to the limited samples.
    for (const bool fullRange : {false, true}) {
        for (const auto layout : {RawVideoLayout::bgra, RawVideoLayout::rgba}) {
            for (std::size_t index = 0; index < std::size(colors); ++index) {
                // The picture is written in video range either way: the encoder converts it to RGB.
                const auto& source = colors[index];
                const auto& color = fullRange ? fullRangeColors[index] : source;
                auto settings = baseSettings(*profile, 128, 96, layout);
                settings.color.fullRange = fullRange;
                settings.bitrate = 4'000'000;
                const auto picture = solid(source.y, source.cb, source.cr);
                const auto encoded = encode(settings, 3, [&](unsigned) { return picture; }, "channel order");
                expectOrdered(encoded.output, 3);
                const auto decoded = decode(VideoCodec::h264, encoded.output);
                require(decoded.size() == 3, "three pictures");
                bool first = true;
                for (const auto& [timestamp, frame] : decoded) {
                    require(frame.layout == VideoPixelLayout::planarYuv && frame.format.bitDepth == 8, "8-bit planar samples");
                    // The centre chroma pair: the encoder subsamples by averaging, so a solid picture stays solid.
                    const std::size_t pair = (std::size_t(frame.format.height) / 4) * frame.format.width + (frame.format.width / 4) * 2;
                    const int cb = frame.chroma[pair], cr = frame.chroma[pair + 1];
                    const int luma = frame.luma[(std::size_t(frame.format.height) / 2) * frame.format.width + frame.format.width / 2];
                    if (std::exchange(first, false))
                        std::printf("  %s %s %s: decoded Y %d Cb %d Cr %d (expected %u %u %u)\n", layoutName(layout),
                                    fullRange ? "full" : "limited", color.name, luma, cb, cr, color.y, color.cb, color.cr);
                    require(std::abs(luma - int(color.y)) <= 12 && std::abs(cb - int(color.cb)) <= 12 && std::abs(cr - int(color.cr)) <= 12,
                            "the decoded colour is the colour that was written");
                }
            }
        }
    }
    // The two orders carry the same picture when the same samples are written.
    double psnr[2] = {};
    std::size_t bytes[2] = {};
    unsigned slot = 0;
    for (const auto layout : {RawVideoLayout::bgra, RawVideoLayout::rgba}) {
        auto settings = baseSettings(*profile, 128, 96, layout);
        settings.bitrate = 1'000'000;
        const auto encoded = encode(settings, 8, fixtureSource(RawVideoLayout::nv12), "rgba equivalence");
        expectOrdered(encoded.output, 8);
        psnr[slot] = meanLumaPsnr(encoded.sources, decode(VideoCodec::h264, encoded.output));
        bytes[slot++] = encoded.output.bytes(0, 8);
    }
    std::printf("  BGRA %.1f dB in %zu bytes, RGBA %.1f dB in %zu bytes\n", psnr[0], bytes[0], psnr[1], bytes[1]);
    require(std::abs(psnr[0] - psnr[1]) < 0.5, "RGBA and BGRA input decode to the same quality");
    require(bytes[1] * 10 < bytes[0] * 11 && bytes[0] * 10 < bytes[1] * 11, "and cost the same bits");
}

// ---- Temporal layers and a QP per frame type: the low-latency encoder.

void lowLatency()
{
    step("temporal layers and per-type QP");
    const auto profile = pick(h264Capability, {VideoEncodeProfile::h264High, VideoEncodeProfile::h264Main, VideoEncodeProfile::h264Baseline});
    require(profile.has_value(), "no H.264 profile");
    const auto standard = [&] {
        auto encoder = open(baseSettings(*profile, 128, 96), "identifier");
        return static_cast<apple_encoder::ProbeEncoder&>(*encoder).encoderIdentifier();
    }();
    std::printf("  standard encoder: %s\n", standard.c_str());
    require(!standard.empty() && standard.find("rtvc") == std::string::npos, "an ordinary stream uses the standard encoder");

    if (h264Capability->maximumTemporalLayers >= 2) {
        auto settings = baseSettings(*profile, 320, 240);
        settings.temporalLayers = 2;
        settings.realTime = true;
        settings.bitrate = 1'000'000;
        settings.keyFrameInterval = 0;
        const unsigned count = 30;
        const auto encoded = encode(settings, count, busySource(320, 240), "temporal layers");
        expectOrdered(encoded.output, count);
        auto encoder = open(settings, "identifier");
        const auto identifier = static_cast<apple_encoder::ProbeEncoder&>(*encoder).encoderIdentifier();
        std::printf("  layered encoder: %s\n", identifier.c_str());
        require(identifier.find("rtvc") != std::string::npos, "temporal layers select the low-latency encoder");
        const auto frames = encoded.output.pictures();
        unsigned upper = 0;
        for (const auto* frame : frames) {
            require(frame->temporalLayer <= 1, "two layers");
            if (frame->type == VideoFrameType::key) require(frame->temporalLayer == 0, "key frames are in the base layer");
            upper += frame->temporalLayer == 1;
        }
        require(upper > 0 && upper < count, "both layers carry frames");
        // The upper layer is not referenced: without it the stream decodes.
        if (hardwareDecodes(*profile)) {
            Output base;
            for (const auto& item : encoded.output.items)
                if (item.kind != EncodedVideoFrameKind::frame || item.temporalLayer == 0) base.items.push_back(item);
            const auto decoded = decode(VideoCodec::h264, base);
            require(decoded.size() == count - upper, "the base layer decodes on its own");
            double total = 0;
            for (const auto& [timestamp, frame] : decoded) total += lumaPsnr(encoded.sources[timestamp], frame);
            require(total / double(decoded.size()) >= 22.0, "and matches the source");
        }
        // What layers cannot be combined with.
        auto bad = settings;
        bad.bFrames = 1;
        auto other = makeVideoEncoder();
        require(other->configure(bad) == VideoEncodeStatus::invalid, "layers and B-frames cannot be combined");
        bad = settings;
        bad.temporalLayers = h264Capability->maximumTemporalLayers + 1;
        require(other->configure(bad) == VideoEncodeStatus::unsupported, "more layers than verified");
        if (advertises(*h264Capability, VideoRateControl::constantBitrate)) {
            bad = settings;
            bad.rateControl = VideoRateControl::constantBitrate;
            require(other->configure(bad) == VideoEncodeStatus::unsupported, "layers with constant bitrate");
        }
    } else {
        std::puts("  no temporal layers on this host; skipped");
    }

    if (advertises(*h264Capability, VideoRateControl::constantQp)) {
        const auto measure = [&](std::array<std::uint32_t, 3> qp) {
            auto settings = baseSettings(*profile, 320, 240);
            settings.rateControl = VideoRateControl::constantQp;
            settings.frameQp = qp;
            settings.keyFrameInterval = 0;
            Feed feed;
            feed.forceKey = [](unsigned index) { return index == 15; };
            const auto encoded = encode(settings, 30, busySource(320, 240), "per-type QP", feed);
            expectOrdered(encoded.output, 30);
            const auto identifier = [&] {
                auto encoder = open(settings, "identifier");
                return static_cast<apple_encoder::ProbeEncoder&>(*encoder).encoderIdentifier();
            }();
            double keyBytes = 0, predictedBytes = 0;
            unsigned keys = 0, predicted = 0;
            for (const auto* frame : encoded.output.pictures()) {
                if (frame->type == VideoFrameType::key) { keyBytes += double(frame->bytes.size()); ++keys; }
                else { predictedBytes += double(frame->bytes.size()); ++predicted; }
            }
            require(keys == 2, "the first frame and the requested one are key frames");
            return std::tuple {keyBytes / keys, predictedBytes / predicted, identifier};
        };
        const auto split = measure({20, 40, 40});
        const auto even = measure({40, 40, 40});
        std::printf("  qp I20 P40: key %.0f P %.0f (%s); qp 40: key %.0f P %.0f (%s)\n", std::get<0>(split), std::get<1>(split),
                    std::get<2>(split).c_str(), std::get<0>(even), std::get<1>(even), std::get<2>(even).c_str());
        require(std::get<0>(split) > std::get<0>(even) * 1.5, "a lower key frame QP produces a bigger key frame");
        require(std::get<1>(split) < std::get<1>(even) * 2.0 && std::get<1>(split) > std::get<1>(even) * 0.5,
                "and leaves the predicted frames where they were");
        require(std::get<2>(split).find("rtvc") != std::string::npos, "a QP per frame type selects the low-latency encoder");
    }
}

// ---- Colour description.

void color()
{
    step("colour description");
    const auto profile = pick(h264Capability, {VideoEncodeProfile::h264High, VideoEncodeProfile::h264Main, VideoEncodeProfile::h264Baseline});
    require(profile.has_value(), "no H.264 profile");
    struct Case {
        VideoEncodeColor color;
        unsigned primaries, transfer, matrix;
        VideoColorMatrix decoded;
    };
    const Case cases[] = {
        {{VideoColorPrimaries::bt709, VideoTransferFunction::bt709, VideoMatrixCoefficients::bt709, false}, 1, 1, 1, VideoColorMatrix::bt709},
        {{VideoColorPrimaries::bt601Ntsc, VideoTransferFunction::bt709, VideoMatrixCoefficients::bt601, false}, 6, 1, 6, VideoColorMatrix::bt601},
        {{VideoColorPrimaries::bt709, VideoTransferFunction::bt709, VideoMatrixCoefficients::bt709, true}, 1, 1, 1, VideoColorMatrix::bt709},
    };
    for (const auto& c : cases) {
        auto settings = baseSettings(*profile, 128, 96);
        settings.color = c.color;
        const auto encoded = encode(settings, 3, fixtureSource(RawVideoLayout::nv12), "colour");
        expectOrdered(encoded.output, 3);
        const auto sps = firstUnit(VideoCodec::h264, *encoded.output.pictures().front(), bits::h264NalSps);
        require(sps.has_value(), "an SPS");
        const auto vui = h264Vui(*sps);
        require(vui.has_value(), "the SPS carries a VUI");
        std::printf("  VUI: primaries %u transfer %u matrix %u full range %d\n", vui->primaries, vui->transfer, vui->matrix, int(vui->fullRange));
        require(vui->colorDescription, "and a colour description");
        require(vui->primaries == c.primaries && vui->transfer == c.transfer && vui->matrix == c.matrix, "the VUI states what was asked for");
        require(vui->fullRange == c.color.fullRange, "the range is stated");
        if (hardwareDecodes(*profile)) {
            const auto decoded = decode(VideoCodec::h264, encoded.output);
            require(decoded.size() == 3, "three pictures");
            for (const auto& [timestamp, frame] : decoded) {
                require(frame.color.matrix == c.decoded, "the decoder reads the matrix back");
            }
        }
    }
}

// ---- Every combination of colour aspects a guest API can state.

// What a stream's VUI states, whichever codec wrote it.
struct StatedColor {
    bool present {};
    unsigned primaries {2}, transfer {2}, matrix {2};
    bool fullRange {};
};

std::optional<StatedColor> statedColor(VideoCodec codec, const EncodedVideoFrame& frame)
{
    StatedColor stated;
    if (codec == VideoCodec::h264) {
        const auto sps = firstUnit(codec, frame, bits::h264NalSps);
        require(sps.has_value(), "an SPS");
        const auto vui = h264Vui(*sps);
        if (!vui) return stated;
        stated.present = vui->colorDescription;
        if (vui->colorDescription) {
            stated.primaries = vui->primaries;
            stated.transfer = vui->transfer;
            stated.matrix = vui->matrix;
        }
        stated.fullRange = vui->fullRange;
        return stated;
    }
    const auto sps = firstUnit(codec, frame, bits::hevcNalSps);
    require(sps.has_value(), "an SPS");
    const auto format = bits::parseHevcSpsFormat(*sps);
    if (!format) return std::nullopt;
    stated.present = format->colorDescription;
    if (format->colorDescription) {
        stated.primaries = format->primaries;
        stated.transfer = format->transfer;
        stated.matrix = format->matrix;
    }
    stated.fullRange = format->fullRange;
    return stated;
}

void colorAspects()
{
    step("every combination of colour aspects");
    struct Standard {
        const char* name;
        VideoColorPrimaries primaries;
        VideoMatrixCoefficients matrix;
        unsigned primariesCode, matrixCode;
    };
    // The standards a guest names set the primaries and the matrix together; an unnamed one sets neither.
    const Standard standards[] = {
        {"unspecified", VideoColorPrimaries::unspecified, VideoMatrixCoefficients::unspecified, 2, 2},
        {"BT.709", VideoColorPrimaries::bt709, VideoMatrixCoefficients::bt709, 1, 1},
        {"BT.601 NTSC", VideoColorPrimaries::bt601Ntsc, VideoMatrixCoefficients::bt601, 6, 6},
        {"BT.601 PAL", VideoColorPrimaries::bt601Pal, VideoMatrixCoefficients::bt601, 5, 6},
    };
    struct Transfer {
        const char* name;
        VideoTransferFunction transfer;
        unsigned code;
    };
    const Transfer transfers[] = {
        {"unspecified", VideoTransferFunction::unspecified, 2},
        {"linear", VideoTransferFunction::linear, 8},
        {"BT.709", VideoTransferFunction::bt709, 1},
    };
    struct Case {
        VideoCodec codec;
        VideoEncodeProfile profile;
        RawVideoLayout layout;
    };
    std::vector<Case> cases;
    for (const auto layout : {RawVideoLayout::nv12, RawVideoLayout::i420, RawVideoLayout::bgra, RawVideoLayout::rgba}) {
        for (const auto* capability : {h264Capability, hevcCapability}) {
            if (!capability) continue;
            const auto profile = pick(capability, {VideoEncodeProfile::h264High, VideoEncodeProfile::h264Main, VideoEncodeProfile::hevcMain});
            if (profile && supportsLayout(*capability, *profile, layout)) cases.push_back({capability->codec, *profile, layout});
        }
    }
    unsigned checked = 0;
    for (const auto& c : cases) {
        for (const bool fullRange : {false, true}) {
            for (const auto& standard : standards) {
                for (const auto& transfer : transfers) {
                    auto settings = baseSettings(c.profile, 128, 96, c.layout);
                    settings.color = {standard.primaries, transfer.transfer, standard.matrix, fullRange};
                    // Pictures matter in the sections above; here a stream is read for what it states.
                    const auto encoded = encode(settings, 1, fixtureSource(RawVideoLayout::nv12), "colour aspects");
                    expectOrdered(encoded.output, 1);
                    const auto stated = statedColor(c.codec, *encoded.output.pictures().front());
                    require(stated.has_value(), "the SPS can be read to its VUI");
                    const bool anything = standard.primaries != VideoColorPrimaries::unspecified ||
                                          transfer.transfer != VideoTransferFunction::unspecified;
                    const std::string what = std::string(layoutName(c.layout)) + (c.codec == VideoCodec::hevc ? " hevc " : " h264 ") +
                                             (fullRange ? "full " : "limited ") + standard.name + " / " + transfer.name;
                    if (anything || fullRange) {
                        if (!stated->present) std::fprintf(stderr, "%s: no colour description\n", what.c_str());
                        require(stated->present, "a stream with any colour aspect states a colour description");
                        if (stated->primaries != standard.primariesCode || stated->transfer != transfer.code ||
                            stated->matrix != standard.matrixCode)
                            std::fprintf(stderr, "%s: states %u/%u/%u, wanted %u/%u/%u\n", what.c_str(), stated->primaries, stated->transfer,
                                         stated->matrix, standard.primariesCode, transfer.code, standard.matrixCode);
                        require(stated->primaries == standard.primariesCode && stated->transfer == transfer.code &&
                                stated->matrix == standard.matrixCode, "each aspect is the one asked for and none other");
                    } else {
                        require(!stated->present, "a stream that asks for nothing states nothing");
                    }
                    if (stated->fullRange != fullRange) std::fprintf(stderr, "%s: range flag %d\n", what.c_str(), int(stated->fullRange));
                    require(stated->fullRange == fullRange, "the range is the one asked for");
                    ++checked;
                }
            }
        }
    }
    std::printf("  %u combinations of range, standard and transfer across %zu codec and layout pairs\n", checked, cases.size());
}

// ---- Levels, entropy mode and timestamps.

void levelsAndEntropyMode()
{
    step("levels and entropy mode");
    for (const auto h264Profile : {VideoEncodeProfile::h264Baseline, VideoEncodeProfile::h264Main, VideoEncodeProfile::h264High}) {
        const auto* entry = profileOf(*h264Capability, h264Profile);
        if (!entry) continue;
        const auto levelOf = [&](std::uint32_t level, std::uint32_t width, std::uint32_t height) {
            auto settings = baseSettings(h264Profile, width, height);
            settings.level = level;
            settings.bitrate = 3'000'000;
            const auto encoded = encode(settings, 8, busySource(width, height), "level");
            expectOrdered(encoded.output, 8);
            const auto sps = firstH264Sps(*encoded.output.pictures().front());
            require(sps.has_value(), "an SPS");
            return sps->levelIdc;
        };
        // A named level, a level VideoToolbox has no name for and one it refuses for the picture's shape.
        const auto stateOf = [&](std::uint32_t level, std::uint32_t width, std::uint32_t height, std::uint32_t kilobits,
                                 std::uint32_t framesPerSecond) {
            auto settings = baseSettings(h264Profile, width, height);
            settings.rateControl = VideoRateControl::constantBitrate;
            settings.bitrate = std::uint64_t(kilobits) * 1000;
            settings.frameRateNumerator = framesPerSecond;
            settings.level = level;
            const auto encoded = encode(settings, 6, busySource(width, height), "stated level");
            expectOrdered(encoded.output, 6);
            const auto sps = firstH264Sps(*encoded.output.pictures().front());
            require(sps.has_value(), "an SPS");
            if (hardwareDecodes(h264Profile))
                require(meanLumaPsnr(encoded.sources, decode(VideoCodec::h264, encoded.output)) >= 12.0, "and the stream decodes");
            return sps->levelIdc;
        };
        const auto automatic = levelOf(0, 1280, 720);
        std::printf("  %s: automatic level at 720p30 is %u\n", profileName(h264Profile), automatic);
        require(automatic >= 31 && automatic <= entry->maximumLevel, "an automatic level holds 720p30");
        if (entry->levelSelectable) {
            require(levelOf(40, 1280, 720) == 40, "a level the stream fits is the level written");
            require(levelOf(10, 1280, 720) >= 31, "a level the stream does not fit is raised to one it fits");
            if (entry->maximumLevel >= 52 && h264Profile != VideoEncodeProfile::h264Baseline)
                require(levelOf(52, 1280, 720) == 52, "the highest level is selectable");
            // VideoToolbox names no level between 1.3 and 3.0 and weighs no bit rate.
            require(stateOf(20, 352, 288, 832, 30) == 20, "a level VideoToolbox does not name is stated");
            require(stateOf(21, 176, 144, 200, 15) == 21, "even one far above what the stream needs");
            require(stateOf(0, 352, 288, 832, 30) >= (h264Profile == VideoEncodeProfile::h264High ? 13u : 20u),
                    "an automatic level holds the bit rate");
            // Baseline 1.3 is refused for a picture 704 samples high.
            const auto tall = stateOf(13, 144, 704, 512, 30);
            std::printf("  %s: a 144x704 stream asked to state level 1.3 states %u\n", profileName(h264Profile), tall);
            require(tall >= 13, "a tall picture is encoded and states at least the level asked for");
            require(stateOf(0, 704, 144, 512, 30) >= 13, "and so is a wide one");

            // The level a stream states holds what the stream is: whatever was asked for, the macroblocks the
            // hardware really codes (it codes no picture narrower than 192 samples), the frame rate and the
            // references. Nothing may state a level the coded stream breaks.
            const auto statedFor = [&](std::uint32_t width, std::uint32_t height, std::uint32_t framesPerSecond, VideoRateControl control,
                                       std::uint64_t kilobits, std::uint64_t peakKilobits, std::uint32_t asked) {
                auto settings = baseSettings(h264Profile, width, height);
                settings.frameRateNumerator = framesPerSecond;
                settings.rateControl = control;
                settings.bitrate = kilobits * 1000;
                settings.peakBitrate = peakKilobits * 1000;
                settings.level = asked;
                const auto encoded = encode(settings, 12, busySource(width, height), "stated level holds the stream");
                expectOrdered(encoded.output, 12);
                const auto unit = firstUnit(VideoCodec::h264, *encoded.output.pictures().front(), bits::h264NalSps);
                require(unit.has_value(), "an SPS");
                const auto limits = bits::parseH264SpsLimits(*unit);
                require(limits.has_value(), "the SPS can be read");
                const unsigned level = bits::h264Level(*unit).value_or(0);
                const auto row = std::find_if(std::begin(lv::kH264Levels), std::end(lv::kH264Levels),
                                              [&](const lv::H264LevelLimits& candidate) { return candidate.idc == level; });
                require(row != std::end(lv::kH264Levels), "a level of Table A-1");
                const std::uint64_t macroblocks = std::uint64_t(limits->widthMacroblocks) * limits->heightMacroblocks;
                require(lv::h264Rank(level) >= lv::h264Rank(asked), "the level asked for is the least that is stated");
                require(macroblocks <= row->frameMacroblocks, "the coded picture is within MaxFS");
                require(double(macroblocks) * framesPerSecond <= double(row->macroblocksPerSecond), "the coded rate is within MaxMBPS");
                require(limits->maxNumRefFrames <= std::min<std::uint64_t>(row->dpbMacroblocks / macroblocks, 16), "the references fit the DPB");
                require(!row->frameMbsOnly || limits->frameMbsOnly, "no interlace where the level forbids it");
                if (control != VideoRateControl::constantQuality && peakKilobits)
                    require(lv::h264Rank(level) >= lv::h264Rank(lv::h264RequiredLevel(h264Profile, settings)),
                            "and the rate asked for");
                if (hardwareDecodes(h264Profile))
                    require(meanLumaPsnr(encoded.sources, decode(VideoCodec::h264, encoded.output)) >= 12.0, "the stream decodes");
                return level;
            };
            for (const std::uint32_t asked : {0u, 10u, 12u, 20u, 30u}) {
                for (const auto control : {VideoRateControl::variableBitrate, VideoRateControl::constantBitrate}) {
                    for (const auto size : {std::pair {160u, 128u}, std::pair {176u, 144u}, std::pair {320u, 240u}}) {
                        const auto level = statedFor(size.first, size.second, 15, control, 64, control == VideoRateControl::variableBitrate ? 64 : 0, asked);
                        if (asked == 10 && size.first == 160) require(level == 10, "a small stream held to 64 kbit/s states the level 1.0 asked for");
                    }
                }
            }
            require(statedFor(160, 128, 15, VideoRateControl::variableBitrate, 64, 0, 10) == 10, "and so does one with no peak when VideoToolbox states it");
            require(statedFor(160, 128, 15, VideoRateControl::variableBitrate, 64, 64, 9) == 9, "level 1b asked for is stated");
            // The hardware codes QCIF as 192x144, 108 macroblocks, which is 1620 per second at 15 frames per second:
            // level 1.0 holds 99 and 1485, so 1.1 is the lowest level that is true of the stream.
            require(statedFor(176, 144, 15, VideoRateControl::variableBitrate, 64, 64, 10) >= 11, "QCIF is not stated as level 1.0");

            // The level is that of the settings the stream starts with. A rate that grows past what it holds is not
            // restated: that would be a second set of parameter sets inside the stream and a key frame nobody
            // asked for, and V4L2 and MediaCodec both give the header once, ahead of the first picture. The
            // caller who will raise the rate says so in the level or the rate it starts with.
            for (const bool separate : {true, false}) {
                auto settings = baseSettings(h264Profile, 160, 128);
                settings.frameRateNumerator = 15;
                settings.rateControl = VideoRateControl::variableBitrate;
                settings.bitrate = settings.peakBitrate = 64'000;
                settings.level = 10;
                settings.keyFrameInterval = 10;
                settings.separateParameterSets = separate;
                Feed feed;
                feed.before = [&](VideoEncoder& encoder, unsigned index) {
                    if (index != 15) return;
                    VideoEncodeUpdate update;
                    update.bitrate = update.peakBitrate = 400'000;
                    require(encoder.update(update) == VideoEncodeStatus::ok, "raising the rate");
                    VideoEncodeUpdate quicker;
                    quicker.frameRateNumerator = 30;
                    require(encoder.update(quicker) == VideoEncodeStatus::ok, "and the frame rate");
                };
                const auto encoded = encode(settings, 40, busySource(160, 128), "rate beyond the level", feed);
                expectOrdered(encoded.output, 40);
                auto faster = settings;
                faster.bitrate = faster.peakBitrate = 400'000;
                faster.frameRateNumerator = 30;
                require(lv::h264Rank(lv::h264RequiredLevel(h264Profile, faster)) > lv::h264Rank(10), "the new rate needs a higher level");
                unsigned headers = 0;
                bool headersFirst = true;
                bool pictureSeen = false;
                for (const auto& item : encoded.output.items) {
                    if (item.kind == EncodedVideoFrameKind::parameterSets) {
                        ++headers;
                        headersFirst = headersFirst && !pictureSeen;
                    } else if (item.kind == EncodedVideoFrameKind::frame) {
                        pictureSeen = true;
                    }
                }
                std::printf("  rate raised under a running stream, %s parameter sets: %u header frames\n", separate ? "separate" : "joined", headers);
                require(headers == (separate ? 1u : 0u) && headersFirst, "the parameter sets are given once, ahead of the first picture");
                std::vector<std::uint64_t> keys;
                std::vector<unsigned> keyLevels;
                for (const auto* picture : encoded.output.pictures()) {
                    if (picture->type != VideoFrameType::key) continue;
                    keys.push_back(picture->timestamp);
                    if (const auto unit = firstUnit(VideoCodec::h264, *picture, bits::h264NalSps)) keyLevels.push_back(bits::h264Level(*unit).value_or(0));
                }
                require((keys == std::vector<std::uint64_t> {0, 10, 20, 30}), "and the new rate asks for no key frame");
                require(keyLevels.size() == 4 && std::all_of(keyLevels.begin(), keyLevels.end(), [&](unsigned level) { return level == keyLevels.front(); }),
                        "every key frame states the level the stream started with");
            }

            // A session made again, here for a QP bound, states the level of the settings as they now are. Its
            // parameter sets differ from the stream's, so a separate buffer for them comes right before its key frame.
            for (const bool separate : {true, false}) {
                auto settings = baseSettings(h264Profile, 160, 128);
                settings.frameRateNumerator = 15;
                settings.rateControl = VideoRateControl::variableBitrate;
                settings.bitrate = settings.peakBitrate = 64'000;
                settings.level = 10;
                settings.keyFrameInterval = 100;
                settings.separateParameterSets = separate;
                Feed feed;
                feed.before = [&](VideoEncoder& encoder, unsigned index) {
                    if (index != 15) return;
                    VideoEncodeUpdate update;
                    update.bitrate = update.peakBitrate = 400'000;
                    update.maximumQp = 40;
                    require(encoder.update(update) == VideoEncodeStatus::ok, "raising the rate and bounding the QP");
                };
                const auto encoded = encode(settings, 30, busySource(160, 128), "session made again", feed);
                expectOrdered(encoded.output, 30);
                const auto& items = encoded.output.items;
                unsigned headers = 0;
                std::vector<unsigned> keyLevels;
                std::vector<std::uint64_t> keys;
                for (std::size_t i = 0; i < items.size(); ++i) {
                    const auto& item = items[i];
                    if (item.kind == EncodedVideoFrameKind::parameterSets) {
                        ++headers;
                        require(i + 1 < items.size() && items[i + 1].kind == EncodedVideoFrameKind::frame && items[i + 1].type == VideoFrameType::key,
                                "parameter sets come right before a key frame");
                    } else if (item.kind == EncodedVideoFrameKind::frame && item.type == VideoFrameType::key) {
                        keys.push_back(item.timestamp);
                        if (const auto unit = firstUnit(VideoCodec::h264, item, bits::h264NalSps)) keyLevels.push_back(bits::h264Level(*unit).value_or(0));
                    }
                }
                require((keys == std::vector<std::uint64_t> {0, 15}), "the new session starts at a key frame");
                require(keyLevels.size() == 2 && keyLevels.front() == 10 && lv::h264Rank(keyLevels.back()) > lv::h264Rank(10),
                        "which states the level the new rate needs");
                require(headers == (separate ? 2u : 0u), "and the parameter sets of each session are given once");
            }
        }
    }
    if (hevcCapability) {
        for (const auto& entry : hevcCapability->profiles) {
            if (entry.profile == VideoEncodeProfile::hevcMainStill) continue;
            auto settings = baseSettings(entry.profile, 1280, 720, entry.inputs.front());
            settings.level = 120;
            const auto encoded = encode(settings, 4, [&](unsigned index) {
                const Picture eight = busyPicture(1280, 720, index);
                return entry.inputs.front() == RawVideoLayout::p010 ? widen(eight) : eight;
            }, "hevc level");
            expectOrdered(encoded.output, 4);
            std::uint32_t level = 0;
            for (const auto unit : bits::splitAnnexB(encoded.output.pictures().front()->bytes))
                if (const auto sps = bits::parseHevcSps(unit)) level = sps->levelIdc;
            std::printf("  %s: level_idc %u for a request of 120\n", profileName(entry.profile), level);
            require(level >= 90 && level <= entry.maximumLevel, "an HEVC stream states a level within the verified maximum");
            if (entry.levelSelectable) {
                // VideoToolbox states level 2.1 for CIF; 2.0 holds 832 kbit/s at 30 frames per second.
                auto cif = baseSettings(entry.profile, 352, 288, entry.inputs.front());
                cif.rateControl = VideoRateControl::constantBitrate;
                cif.bitrate = 832'000;
                cif.level = 60;
                const auto cifEncoded = encode(cif, 6, [&](unsigned index) {
                    const Picture eight = busyPicture(352, 288, index);
                    return entry.inputs.front() == RawVideoLayout::p010 ? widen(eight) : eight;
                }, "hevc cif");
                expectOrdered(cifEncoded.output, 6);
                std::uint32_t cifSps = 0, cifVps = 0;
                for (const auto unit : bits::splitAnnexB(cifEncoded.output.pictures().front()->bytes)) {
                    if (const auto sps = bits::parseHevcSps(unit)) cifSps = sps->levelIdc;
                    if (bits::hevcNalType(unit) == bits::hevcNalVps) cifVps = bits::hevcLevel(unit).value_or(0);
                }
                require(cifSps == 60 && cifVps == 60, "a bounded CIF stream states level 2 in the SPS and the VPS");
                // Level 2 allows sixteen slice segments a picture; the encoder writes the picture whole.
                for (const auto* picture : cifEncoded.output.pictures()) {
                    unsigned slices = 0;
                    for (const auto unit : bits::splitAnnexB(picture->bytes))
                        if (bits::hevcNalType(unit) < 32) ++slices;
                    require(slices >= 1 && slices <= 16, "within the slice segments the level allows");
                }
                require(level == 120, "the level asked for is the level stated");
                std::uint32_t vpsLevel = 0;
                for (const auto unit : bits::splitAnnexB(encoded.output.pictures().front()->bytes))
                    if (const auto vps = bits::hevcLevel(unit); vps && bits::hevcNalType(unit) == bits::hevcNalVps) vpsLevel = *vps;
                require(vpsLevel == level, "in the VPS as well");
                // And never less than the stream needs, which VideoToolbox's choice may not be.
                auto busy = settings;
                busy.level = 0;
                busy.bitrate = 20'000'000;
                busy.rateControl = VideoRateControl::constantBitrate;
                const auto automatic = encode(busy, 4, [&](unsigned index) {
                    const Picture eight = busyPicture(1280, 720, index);
                    return entry.inputs.front() == RawVideoLayout::p010 ? widen(eight) : eight;
                }, "hevc automatic level");
                std::uint32_t chosen = 0;
                for (const auto unit : bits::splitAnnexB(automatic.output.pictures().front()->bytes))
                    if (const auto sps = bits::parseHevcSps(unit)) chosen = sps->levelIdc;
                require(chosen >= 123, "20 Mbit/s at 720p30 needs level 4.1 in the Main tier");
            }
        }
    }

    for (const auto h264Profile : {VideoEncodeProfile::h264Main, VideoEncodeProfile::h264High}) {
        if (!profileOf(*h264Capability, h264Profile) || !h264Capability->entropyModeSelectable) continue;
        for (const bool cabac : {true, false}) {
            auto settings = baseSettings(h264Profile, 128, 96);
            settings.cabac = cabac;
            const auto encoded = encode(settings, 3, fixtureSource(RawVideoLayout::nv12), "entropy mode");
            const auto pps = firstUnit(VideoCodec::h264, *encoded.output.pictures().front(), bits::h264NalPps);
            require(pps.has_value(), "a PPS");
            const auto flag = bits::h264Cabac(*pps);
            require(flag.has_value() && *flag == cabac, "the entropy coding mode is the one asked for");
        }
    }
    if (const auto baseline = pick(h264Capability, {VideoEncodeProfile::h264Baseline})) {
        auto settings = baseSettings(*baseline, 128, 96);
        settings.cabac = true;
        const auto encoded = encode(settings, 3, fixtureSource(RawVideoLayout::nv12), "baseline entropy");
        const auto pps = firstUnit(VideoCodec::h264, *encoded.output.pictures().front(), bits::h264NalPps);
        require(pps.has_value() && bits::h264Cabac(*pps) == std::optional<bool>(false), "Baseline is always CAVLC");
    }
}

void timestamps()
{
    step("timestamps");
    const auto profile = pick(h264Capability, {VideoEncodeProfile::h264High, VideoEncodeProfile::h264Main, VideoEncodeProfile::h264Baseline});
    require(profile.has_value(), "no H.264 profile");
    auto settings = baseSettings(*profile, 128, 96);
    Feed feed;
    feed.timestamp = [](unsigned index) { return 0xfffffffe00000000ull + index * 33'333ull; };
    const auto encoded = encode(settings, 10, fixtureSource(RawVideoLayout::nv12), "timestamps", feed);
    const auto frames = encoded.output.pictures();
    require(encoded.output.errors == 0 && frames.size() == 10, "all frames");
    for (unsigned i = 0; i < 10; ++i) require(frames[i]->timestamp == 0xfffffffe00000000ull + i * 33'333ull, "a 64-bit timestamp comes back unchanged");
}

// ---- Sizes and concurrent sessions.

void sizes()
{
    step("sizes");
    for (const auto* capability : {h264Capability, hevcCapability}) {
        if (!capability) continue;
        const auto profile = pick(capability, {VideoEncodeProfile::h264High, VideoEncodeProfile::h264Main, VideoEncodeProfile::h264Baseline,
                                               VideoEncodeProfile::hevcMain});
        require(profile.has_value(), "no profile");
        const std::vector<std::pair<std::uint32_t, std::uint32_t>> wanted {
            {capability->minimumWidth, capability->minimumHeight}, {130, 98}, {640, 360}, {1280, 720}, {1920, 1080}};
        for (const auto& [width, height] : wanted) {
            if (width < capability->minimumWidth || height < capability->minimumHeight || width > capability->maximumWidth ||
                height > capability->maximumHeight || width % capability->sizeAlignment || height % capability->sizeAlignment)
                continue;
            auto settings = baseSettings(*profile, width, height);
            settings.bitrate = std::max<std::uint64_t>(500'000, std::uint64_t(width) * height * 2);
            const auto encoded = encode(settings, 4, busySource(width, height), "sizes");
            expectOrdered(encoded.output, 4);
            std::string description = std::to_string(width) + "x" + std::to_string(height);
            if (hardwareDecodes(*profile)) {
                const auto decoded = decode(capability->codec, encoded.output);
                require(decoded.size() == 4, "every picture decodes");
                require(decoded.begin()->second.format.width == width && decoded.begin()->second.format.height == height,
                        "the stream states the picture size it was given");
                require(meanLumaPsnr(encoded.sources, decoded) >= 18.0, "and the picture is the source");
            }
            std::printf("  %s %s: %zu bytes\n", capability->codec == VideoCodec::hevc ? "hevc" : "h264", description.c_str(),
                        encoded.output.bytes(0, 4));
        }
    }
}

void concurrentStreams()
{
    step("two streams at once, and the session limit");
    // Two sessions of different codecs, fed in turn, keep to their own frames.
    if (hevcCapability) {
        const auto h264Profile = pick(h264Capability, {VideoEncodeProfile::h264High, VideoEncodeProfile::h264Main, VideoEncodeProfile::h264Baseline});
        const auto hevcProfile = pick(hevcCapability, {VideoEncodeProfile::hevcMain});
        if (h264Profile && hevcProfile) {
            auto first = open(baseSettings(*h264Profile, 128, 96), "first");
            auto second = open(baseSettings(*hevcProfile, 160, 96), "second");
            const auto base = fixturePicture(false);
            Output a, b;
            unsigned sentA = 0, sentB = 0;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            while ((sentA < 8 || sentB < 8) && std::chrono::steady_clock::now() < deadline) {
                if (sentA < 8) {
                    if (auto frame = first->acquireFrame()) {
                        writePicture(*frame, shifted(base, sentA * 2), RawVideoLayout::nv12);
                        first->submit(std::move(frame), 10 + sentA++, false);
                    }
                }
                if (sentB < 8) {
                    if (auto frame = second->acquireFrame()) {
                        auto picture = busyPicture(160, 96, sentB);
                        writePicture(*frame, picture, RawVideoLayout::nv12);
                        second->submit(std::move(frame), 100 + sentB++, false);
                    }
                }
                collect(*first, a, 5);
                collect(*second, b, 5);
            }
            first->drain();
            second->drain();
            while ((!a.drained || !b.drained) && std::chrono::steady_clock::now() < deadline) {
                collect(*first, a, 20);
                collect(*second, b, 20);
            }
            require(a.drained && b.drained, "both drain");
            require(a.errors == 0 && b.errors == 0 && a.pictures().size() == 8 && b.pictures().size() == 8, "each returns its own frames");
            for (unsigned i = 0; i < 8; ++i)
                require(a.pictures()[i]->timestamp == 10 + i && b.pictures()[i]->timestamp == 100 + i, "in order, with their own timestamps");
            require(hasUnit(VideoCodec::h264, *a.pictures()[0], bits::h264NalSps) && hasUnit(VideoCodec::hevc, *b.pictures()[0], bits::hevcNalSps),
                    "each in its own codec");
        }
    }

    // The sessions discovery counted can all be open at once; one more than the
    // hardware allows is reported as exhausted rather than as a failure, and
    // closing one makes room.
    const auto profile = pick(h264Capability, {VideoEncodeProfile::h264Baseline, VideoEncodeProfile::h264Main, VideoEncodeProfile::h264High});
    require(profile.has_value(), "no H.264 profile");
    auto settings = baseSettings(*profile, 128, 96);
    // The system limits hardware sessions across every process, so another
    // process holding some makes this one open fewer: try again a few times
    // before calling that a fault.
    std::vector<std::unique_ptr<VideoEncoder>> sessions;
    VideoEncodeStatus last = VideoEncodeStatus::ok;
    for (unsigned attempt = 0; attempt < 6; ++attempt) {
        sessions.clear();
        last = VideoEncodeStatus::ok;
        for (unsigned i = 0; i < 48; ++i) {
            auto encoder = makeVideoEncoder();
            last = encoder->configure(settings);
            if (last != VideoEncodeStatus::ok) break;
            sessions.push_back(std::move(encoder));
        }
        if (sessions.size() >= h264Capability->concurrentSessions || attempt == 5) break;
        sessions.clear();
        usleep(1'000'000);
    }
    std::printf("  %zu sessions open at once (discovery counted %u)\n", sessions.size(), h264Capability->concurrentSessions);
    require(sessions.size() >= h264Capability->concurrentSessions, "the sessions discovery counted can be open together");
    if (last != VideoEncodeStatus::ok) {
        require(last == VideoEncodeStatus::exhausted, "running out of hardware sessions is reported as exhausted");
        sessions.pop_back();
        // VideoToolbox gives a closed session's hardware back a moment later.
        VideoEncodeStatus again = VideoEncodeStatus::exhausted;
        for (unsigned attempt = 0; attempt < 50 && again == VideoEncodeStatus::exhausted; ++attempt) {
            if (attempt) usleep(100'000);
            auto encoder = makeVideoEncoder();
            again = encoder->configure(settings);
        }
        require(again == VideoEncodeStatus::ok, "closing a session makes room");
    }
}

// ---- Arguments the encoder refuses.

void refusals()
{
    step("refused arguments");
    const auto profile = pick(h264Capability, {VideoEncodeProfile::h264High, VideoEncodeProfile::h264Main, VideoEncodeProfile::h264Baseline});
    require(profile.has_value(), "no H.264 profile");
    const auto good = baseSettings(*profile, 128, 96);
    const auto expect = [&](const char* what, VideoEncodeStatus expected, auto&& change) {
        auto settings = good;
        change(settings);
        auto encoder = makeVideoEncoder();
        const auto status = encoder->configure(settings);
        if (status != expected)
            std::fprintf(stderr, "%s: got %s (%s), wanted %s\n", what, describe(status), diagnosticOf(*encoder).c_str(), describe(expected));
        require(status == expected, what);
        require(encoder->acquireFrame() == nullptr, "a refused configuration hands out no frame");
    };
    using S = VideoEncodeStatus;
    expect("zero width", S::invalid, [](auto& s) { s.width = 0; });
    expect("zero height", S::invalid, [](auto& s) { s.height = 0; });
    expect("odd width", S::invalid, [](auto& s) { s.width = 129; });
    expect("odd height", S::invalid, [](auto& s) { s.height = 97; });
    expect("below the minimum size", S::invalid, [&](auto& s) { s.width = h264Capability->minimumWidth - 2; });
    expect("above the maximum size", S::invalid, [&](auto& s) { s.width = h264Capability->maximumWidth + 2; });
    expect("profile of another codec", S::invalid, [](auto& s) { s.profile = VideoEncodeProfile::hevcMain; });
    expect("a codec with no encoder", S::unsupported, [](auto& s) { s.codec = VideoCodec::vp9; });
    expect("zero frame rate", S::invalid, [](auto& s) { s.frameRateNumerator = 0; });
    expect("peak below target", S::invalid, [](auto& s) { s.bitrate = 2'000'000; s.peakBitrate = 1'000'000; });
    expect("bitrate above the maximum", S::invalid, [&](auto& s) { s.bitrate = h264Capability->maximumBitrate + 1; });
    expect("quality out of range", S::invalid, [](auto& s) { s.rateControl = VideoRateControl::constantQuality; s.quality = 101; });
    expect("minimum QP above maximum QP", S::invalid, [](auto& s) { s.minimumQp = 40; s.maximumQp = 30; });
    expect("QP above 51", S::invalid, [](auto& s) { s.maximumQp = 52; });
    expect("temporal layers with B-frames", S::invalid, [](auto& s) { s.temporalLayers = 2; s.bFrames = 1; });
    expect("B-frames above the maximum", S::invalid, [&](auto& s) { s.bFrames = h264Capability->maximumBFrames + 1; });
    for (const auto layout : {RawVideoLayout::nv12, RawVideoLayout::i420, RawVideoLayout::p010, RawVideoLayout::nv16, RawVideoLayout::nv24,
                              RawVideoLayout::bgra, RawVideoLayout::rgba}) {
        if (supportsLayout(*h264Capability, *profile, layout)) continue;
        expect("an input layout that was not verified", S::unsupported, [&](auto& s) { s.input = layout; });
        break;
    }
    for (const auto other : {VideoEncodeProfile::h264Baseline, VideoEncodeProfile::h264ConstrainedBaseline, VideoEncodeProfile::h264Main,
                             VideoEncodeProfile::h264High, VideoEncodeProfile::h264ConstrainedHigh, VideoEncodeProfile::h264High422,
                             VideoEncodeProfile::h264High444Predictive}) {
        if (profileOf(*h264Capability, other)) continue;
        expect("a profile that was not verified", S::unsupported, [&](auto& s) { s.profile = other; });
        break;
    }
    if (jpegCapability) {
        auto jpeg = baseSettings(VideoEncodeProfile::jpegBaseline, 128, 96);
        jpeg.jpegQuality = 101;
        auto encoder = makeVideoEncoder();
        require(encoder->configure(jpeg) == S::invalid, "JPEG quality out of range");
        jpeg.jpegQuality = 80;
        jpeg.bFrames = 1;
        require(encoder->configure(jpeg) == S::invalid, "JPEG has no B-frames");
    }
    // An encoder that was never configured accepts nothing.
    auto encoder = makeVideoEncoder();
    require(encoder->acquireFrame() == nullptr, "no frame before configure");
    encoder->drain();
    waitReadable(*encoder);
    auto items = encoder->takeFrames();
    require(items.size() == 1 && items[0].kind == EncodedVideoFrameKind::drained, "draining an unconfigured encoder still answers");
    // A refusal leaves the encoder usable: a good configuration after a bad one works.
    auto settings = good;
    settings.width = 129;
    require(encoder->configure(settings) == S::invalid, "a bad configuration");
    require(encoder->configure(good) == S::ok, "and then a good one");
    require(encoder->acquireFrame() != nullptr, "gives frames");
}

// ---- JPEG.

std::pair<unsigned, unsigned> jpegSize(const std::vector<std::uint8_t>& data)
{
    for (std::size_t at = 2; at + 9 < data.size();) {
        require(data[at] == 0xff, "JPEG segment marker");
        const unsigned marker = data[at + 1];
        const std::size_t length = (std::size_t(data[at + 2]) << 8) | data[at + 3];
        if (marker >= 0xc0 && marker <= 0xc3) return {(unsigned(data[at + 7]) << 8) | data[at + 8], (unsigned(data[at + 5]) << 8) | data[at + 6]};
        at += 2 + length;
    }
    return {};
}

void jpeg()
{
    step("JPEG");
    if (!jpegCapability) {
        std::puts("  no JPEG encoder on this host; skipped");
        return;
    }
    const auto& capability = *jpegCapability;
    const auto layout = capability.profiles.front().inputs.front();
    const struct { std::uint32_t width, height; } sizes[] = {{128, 96}, {97, 65}, {capability.minimumWidth, capability.minimumHeight}, {320, 240}, {321, 241}};
    for (const auto& size : sizes) {
        if (size.width % capability.sizeAlignment || size.height % capability.sizeAlignment) continue;
        auto settings = baseSettings(VideoEncodeProfile::jpegBaseline, size.width, size.height, layout);
        settings.jpegQuality = 90;
        // Odd sizes need the source padded to even chroma; the picture is the
        // even size above it and the encoder is given the odd one.
        const std::uint32_t evenWidth = (size.width + 1) & ~1u, evenHeight = (size.height + 1) & ~1u;
        Feed feed;
        auto encoder = open(settings, "jpeg");
        const auto source = busyPicture(evenWidth, evenHeight, 3);
        Output output;
        feed.count = 3;
        feed.fill = [&](WritableVideoFrame& frame, unsigned) {
            if (layout != RawVideoLayout::nv12) {
                for (std::size_t plane = 0; plane < frame.planeCount(); ++plane)
                    std::memset(frame.plane(plane).data(), 0x70, frame.plane(plane).size());
                return;
            }
            // The frame is the size asked for; its chroma rounds up.
            auto luma = frame.plane(0);
            for (std::uint32_t y = 0; y < size.height; ++y)
                std::memcpy(luma.data() + std::size_t(y) * frame.stride(0), source.luma.data() + std::size_t(y) * evenWidth, size.width);
            auto chroma = frame.plane(1);
            for (std::uint32_t y = 0; y < frame.rows(1); ++y)
                std::memcpy(chroma.data() + std::size_t(y) * frame.stride(1), source.chroma.data() + std::size_t(y) * evenWidth,
                            std::min<std::size_t>(frame.stride(1), evenWidth));
        };
        output = run(*encoder, feed);
        expectOrdered(output, 3);
        for (const auto* frame : output.pictures()) {
            const auto& data = frame->bytes;
            require(data.size() > 20 && data[0] == 0xff && data[1] == 0xd8, "SOI");
            require(data[2] == 0xff && data[3] == 0xe0 && std::memcmp(&data[6], "JFIF\0", 5) == 0, "a JFIF segment follows SOI");
            require(data[data.size() - 2] == 0xff && data[data.size() - 1] == 0xd9, "EOI");
            const auto [width, height] = jpegSize(data);
            require(width == size.width && height == size.height, "the image states the size it was given");
            require(frame->type == VideoFrameType::key, "every JPEG image is a key frame");
        }
        std::printf("  %ux%u: %zu bytes\n", size.width, size.height, output.pictures().front()->bytes.size());
        // The hardware decoder takes no image smaller than a macroblock row of pictures.
        if (layout == RawVideoLayout::nv12 && hardwareDecodes(VideoEncodeProfile::jpegBaseline) && size.width >= 64 && size.height >= 64 &&
            size.width % 2 == 0 && size.height % 2 == 0) {
            const auto decoded = decode(VideoCodec::jpeg, output);
            require(decoded.size() == 3, "three images decode");
            Picture reference = source;
            for (const auto& [timestamp, image] : decoded) require(lumaPsnr(reference, image) >= 28.0, "the image is the source");
        }
    }

    // Quality changes while encoding and the size follows it.
    auto settings = baseSettings(VideoEncodeProfile::jpegBaseline, 320, 240, layout);
    settings.jpegQuality = 95;
    Feed feed;
    feed.before = [](VideoEncoder& encoder, unsigned index) {
        if (index == 3) {
            VideoEncodeUpdate update;
            update.jpegQuality = 20;
            require(encoder.update(update) == VideoEncodeStatus::ok, "lowering the JPEG quality");
        }
    };
    const auto encoded = encode(settings, 6, [&](unsigned index) { return busyPicture(320, 240, index); }, "jpeg quality", feed);
    expectOrdered(encoded.output, 6);
    require(encoded.output.bytes(3, 6) * 2 < encoded.output.bytes(0, 3), "a lower quality produces smaller images");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: uml_video_encoder_tests FIXTURE_DIRECTORY\n"
                             "  FIXTURE_DIRECTORY holds a copy of tests/support/video\n");
        return 2;
    }
    fixtureDirectory = argv[1];
    try {
        // The capability cache of this run lives in the test's scratch
        // directory, so the run discovers the host itself and leaves nothing in
        // the user's temporary directory. The cache has its own test.
        const uml::testing::ScratchSubdirectory cacheDirectory("video-encoder-XXXXXX");
        const auto cachePath = (cacheDirectory.path() / "capabilities.json").string();
        require(::setenv("UML_VIDEO_ENCODE_CACHE", cachePath.c_str(), 1) == 0, "cache path");
        capabilityInvariants();
        profileLayoutRoundtrip();
        rateControlModes();
        bitrateUpdate();
        qualityAndQpUpdate();
        keyFrames();
        bFrames();
        flow();
        parameterSets();
        hdr();
        hdrColor();
        rgbaInput();
        lowLatency();
        color();
        colorAspects();
        levelsAndEntropyMode();
        timestamps();
        sizes();
        concurrentStreams();
        refusals();
        jpeg();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("video encoder ok");
    return 0;
}
