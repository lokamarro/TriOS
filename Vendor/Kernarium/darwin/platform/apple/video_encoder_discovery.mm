// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_encoder.hpp"
#include "video_encoder_bitstream.hpp"
#include "video_encoder_internal.hpp"

#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <TargetConditionals.h>
#include <poll.h>
#include <pthread.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>

/* What the hardware encoders of this host do, learned by asking them to encode.
 *
 * VideoToolbox lists the properties an encoder has, and the profile values it
 * knows, but no numeric range, and its lists have been wrong before (the
 * decoder's High 10 is listed on a host whose hardware rejects it). So the
 * lists only say what is worth trying. A profile, an input layout, a size or a
 * rate control mode is published when an encode that used it produced a stream
 * that says so: the profile and level in its SPS, the entropy mode in its PPS,
 * the HDR messages in its SEI.
 *
 * Every probe is the production encoder, so discovery also proves the pipeline
 * a guest's stream takes. The result is cached per host model and OS build. */

namespace uml::platform {
namespace apple_encoder {
namespace {

using bitstream::Bytes;

// 5: the smallest picture side is kVideoEncodeMinimumSide; earlier results list smaller ones.
constexpr unsigned kDiscoveryRevision = 5;
constexpr std::chrono::seconds kDiscoveryWait {300};   // a discovery takes seconds; the bound only ends a stuck one
constexpr std::uint32_t kProbeWidth = 128;
constexpr std::uint32_t kProbeHeight = 96;
constexpr unsigned kSessionProbeLimit = 16;   // the device bounds a guest's streams at eight
constexpr std::uint64_t kHevcLevelSixLumaSamples = 35'651'584;

// ---- Probing.

using Clock = std::chrono::steady_clock;

struct Stream {
    VideoEncodeStatus status {VideoEncodeStatus::failed};
    bool encoded {};   // every frame produced a coded picture and the drain completed
    std::vector<EncodedVideoFrame> frames;
    std::string encoder;
    std::string diagnostic;
    double seconds {};
};

struct Probe {
    VideoEncodeSettings settings;
    unsigned frames {2};
    bool noisy {};   // fill with a pattern rather than leave the pool's zeros
    unsigned timeoutMilliseconds {15'000};
};

VideoEncodeSettings baseSettings(VideoCodec codec, VideoEncodeProfile profile, RawVideoLayout layout,
                                 std::uint32_t width = kProbeWidth, std::uint32_t height = kProbeHeight)
{
    VideoEncodeSettings settings;
    settings.codec = codec;
    settings.profile = profile;
    settings.input = layout;
    settings.width = width;
    settings.height = height;
    settings.bitrate = 2'000'000;
    settings.quality = 60;
    settings.frameQp = {26, 26, 26};
    settings.jpegQuality = 85;
    return settings;
}

class Filler {
public:
    void fill(WritableVideoFrame& frame, unsigned index, bool noisy)
    {
        if (!noisy) return;
        for (std::size_t plane = 0; plane < frame.planeCount(); ++plane) {
            auto bytes = frame.plane(plane);
            if (bytes.empty()) continue;
            if (templates_.size() <= plane) templates_.resize(plane + 1);
            auto& pattern = templates_[plane];
            if (pattern.size() != bytes.size()) {
                pattern.resize(bytes.size());
                std::uint32_t state = 0x9e3779b9u * std::uint32_t(plane + 1);
                for (std::size_t i = 0; i < pattern.size(); ++i) {
                    state ^= state << 13;
                    state ^= state >> 17;
                    state ^= state << 5;
                    // Smooth ramps with some noise on top: neither flat nor
                    // incompressible, so a rate target is actually held.
                    pattern[i] = std::uint8_t(60 + (i % frame.stride(plane)) / 3 + (state & 0x1f));
                }
            }
            // Slide the pattern by a few bytes per frame so the pictures differ.
            const std::size_t shift = (std::size_t(index) * 7) % std::max<std::size_t>(1, frame.stride(plane));
            std::memcpy(bytes.data(), pattern.data() + shift, bytes.size() - shift);
            std::memcpy(bytes.data() + (bytes.size() - shift), pattern.data(), shift);
        }
    }
private:
    std::vector<std::vector<std::uint8_t>> templates_;
};

Stream runProbe(const Probe& probe)
{
    Stream out;
    auto encoder = makeProbeEncoder();
    out.status = encoder->configureUnchecked(probe.settings);
    out.diagnostic = encoder->diagnostic();
    if (out.status != VideoEncodeStatus::ok) return out;
    out.encoder = encoder->encoderIdentifier();

    Filler filler;
    const auto start = Clock::now();
    const auto deadline = start + std::chrono::milliseconds(probe.timeoutMilliseconds);
    unsigned submitted = 0, pictures = 0, errors = 0;
    bool drained = false, draining = false;
    while (!drained && Clock::now() < deadline) {
        while (submitted < probe.frames) {
            auto frame = encoder->acquireFrame();
            if (!frame) break;
            filler.fill(*frame, submitted, probe.noisy);
            encoder->submit(std::move(frame), submitted, false);
            ++submitted;
        }
        if (submitted == probe.frames && !draining) {
            encoder->drain();
            draining = true;
        }
        pollfd readable {encoder->readinessDescriptor(), POLLIN, 0};
        (void)::poll(&readable, 1, 20);
        for (auto& frame : encoder->takeFrames()) {
            switch (frame.kind) {
            case EncodedVideoFrameKind::frame: ++pictures; out.frames.push_back(std::move(frame)); break;
            case EncodedVideoFrameKind::parameterSets: out.frames.push_back(std::move(frame)); break;
            case EncodedVideoFrameKind::error: ++errors; break;
            case EncodedVideoFrameKind::drained: drained = true; break;
            }
        }
    }
    out.seconds = std::chrono::duration<double>(Clock::now() - start).count();
    out.diagnostic = encoder->diagnostic();
    out.encoded = drained && !errors && pictures == probe.frames;
    return out;
}

// ---- Reading a probe's stream.

struct Facts {
    std::optional<unsigned> profileIdc;
    unsigned constraintFlags {};
    unsigned levelIdc {};
    bool highTier {};
    std::optional<bool> cabac;
    std::vector<bitstream::SeiMessage> sei;
    unsigned keyFrames {};
    unsigned longestBidirectionalRun {};
    unsigned upperLayerFrames {};
    bool sawLowerLayerFrame {};
};

Facts analyze(VideoCodec codec, const std::vector<EncodedVideoFrame>& frames)
{
    Facts facts;
    unsigned run = 0;
    bool firstKey = true;
    for (const auto& frame : frames) {
        if (frame.kind != EncodedVideoFrameKind::frame) continue;
        if (frame.type == VideoFrameType::key) ++facts.keyFrames;
        run = frame.type == VideoFrameType::bidirectional ? run + 1 : 0;
        facts.longestBidirectionalRun = std::max(facts.longestBidirectionalRun, run);
        if (frame.type != VideoFrameType::key) {
            if (frame.temporalLayer > 0) ++facts.upperLayerFrames;
            else facts.sawLowerLayerFrame = true;
        }
        if (!firstKey || frame.type != VideoFrameType::key) continue;
        firstKey = false;
        for (const auto unit : bitstream::splitAnnexB(frame.bytes)) {
            if (codec == VideoCodec::h264) {
                if (const auto sps = bitstream::parseH264Sps(unit)) {
                    facts.profileIdc = sps->profileIdc;
                    facts.constraintFlags = sps->constraintFlags;
                    facts.levelIdc = sps->levelIdc;
                } else if (const auto cabac = bitstream::h264Cabac(unit)) {
                    facts.cabac = cabac;
                }
            } else if (codec == VideoCodec::hevc) {
                if (const auto sps = bitstream::parseHevcSps(unit)) {
                    facts.profileIdc = sps->profileIdc;
                    facts.highTier = sps->highTier;
                    facts.levelIdc = sps->levelIdc;
                } else if (bitstream::hevcNalType(unit) == bitstream::hevcNalPrefixSei) {
                    for (auto& message : bitstream::hevcSeiMessages(unit)) facts.sei.push_back(std::move(message));
                }
            }
        }
    }
    return facts;
}

bool writesProfile(VideoEncodeProfile profile, const Facts& facts)
{
    if (!facts.profileIdc) return false;
    const unsigned idc = *facts.profileIdc;
    switch (profile) {
    case VideoEncodeProfile::h264Baseline: return idc == 66;
    case VideoEncodeProfile::h264ConstrainedBaseline: return idc == 66 && (facts.constraintFlags & 0x40);
    case VideoEncodeProfile::h264Main: return idc == 77;
    case VideoEncodeProfile::h264High: return idc == 100 && (facts.constraintFlags & 0x0c) == 0;
    case VideoEncodeProfile::h264ConstrainedHigh: return idc == 100 && (facts.constraintFlags & 0x0c) == 0x0c;
    case VideoEncodeProfile::h264High422: return idc == 122;
    case VideoEncodeProfile::h264High444Predictive: return idc == 244;
    case VideoEncodeProfile::hevcMain: return idc == 1;
    case VideoEncodeProfile::hevcMain10: return idc == 2;
    case VideoEncodeProfile::hevcMainStill: return idc == 3;
    case VideoEncodeProfile::jpegBaseline: return true;
    }
    return false;
}

// The dimensions a JPEG image states, or zeros when it states none.
std::pair<std::uint32_t, std::uint32_t> jpegDimensions(Bytes data)
{
    if (data.size() < 4 || data[0] != 0xff || data[1] != 0xd8) return {};
    for (std::size_t at = 2; at + 4 <= data.size();) {
        if (data[at] != 0xff) return {};
        while (at < data.size() && data[at] == 0xff) ++at;
        if (at >= data.size()) return {};
        const unsigned marker = data[at++];
        if (marker == 0xd9 || marker == 0xda) return {};
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        if (at + 2 > data.size()) return {};
        const std::size_t length = (std::size_t(data[at]) << 8) | data[at + 1];
        if (length < 2 || at + length > data.size()) return {};
        if ((marker >= 0xc0 && marker <= 0xcf) && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
            if (length < 8) return {};
            return {(std::uint32_t(data[at + 5]) << 8) | data[at + 6], (std::uint32_t(data[at + 3]) << 8) | data[at + 4]};
        }
        at += length;
    }
    return {};
}

bool jpegIsWhole(const Stream& stream, std::uint32_t width, std::uint32_t height)
{
    if (stream.frames.empty()) return false;
    for (const auto& frame : stream.frames) {
        const auto& bytes = frame.bytes;
        if (bytes.size() < 4 || bytes[bytes.size() - 2] != 0xff || bytes.back() != 0xd9) return false;
        if (jpegDimensions(bytes) != std::pair{width, height}) return false;
    }
    return true;
}

// Whether a probe produced a stream that proves what it was asked for.
bool proves(const Probe& probe, const Stream& stream)
{
    if (!stream.encoded) return false;
    if (probe.settings.codec == VideoCodec::jpeg)
        return jpegIsWhole(stream, probe.settings.width, probe.settings.height);
    return writesProfile(probe.settings.profile, analyze(probe.settings.codec, stream.frames));
}

// Total coded bytes of a stream.
std::size_t codedBytes(const Stream& stream)
{
    std::size_t total = 0;
    for (const auto& frame : stream.frames) total += frame.bytes.size();
    return total;
}

// Whether a rate control mode does what its name says. A property the encoder
// accepts and ignores is not a mode: the stream it makes is the same whatever
// the mode's parameter is, so the mode is published only when its parameter
// moves the size of the stream, the right way round.
bool modeTakesEffect(VideoCodec codec, const VideoEncodeProfileCapability& reference, VideoRateControl mode)
{
    const auto bytesWith = [&](auto&& choose) -> std::optional<std::size_t> {
        Probe probe {baseSettings(codec, reference.profile, reference.inputs.front()), 24, true};
        probe.settings.rateControl = mode;
        choose(probe.settings);
        const auto stream = runProbe(probe);
        if (!proves(probe, stream)) return std::nullopt;
        return codedBytes(stream);
    };
    std::optional<std::size_t> smaller, larger;
    switch (mode) {
    case VideoRateControl::variableBitrate: return true;
    case VideoRateControl::constantBitrate:
        smaller = bytesWith([](auto& s) { s.bitrate = 200'000; });
        larger = bytesWith([](auto& s) { s.bitrate = 8'000'000; });
        break;
    case VideoRateControl::constantQuality:
        smaller = bytesWith([](auto& s) { s.quality = 10; });
        larger = bytesWith([](auto& s) { s.quality = 90; });
        break;
    case VideoRateControl::constantQp:
        smaller = bytesWith([](auto& s) { s.frameQp = {40, 40, 40}; });
        larger = bytesWith([](auto& s) { s.frameQp = {18, 18, 18}; });
        break;
    }
    return smaller && larger && *larger > *smaller + *smaller / 5;
}

// ---- The tables of what to try.

struct Candidate {
    VideoEncodeProfile profile;
    std::vector<RawVideoLayout> layouts;
};

std::vector<Candidate> candidates(VideoCodec codec)
{
    const std::vector<RawVideoLayout> common {RawVideoLayout::nv12, RawVideoLayout::i420, RawVideoLayout::bgra, RawVideoLayout::rgba};
    switch (codec) {
    case VideoCodec::h264:
        return {{VideoEncodeProfile::h264Baseline, common}, {VideoEncodeProfile::h264ConstrainedBaseline, common},
                {VideoEncodeProfile::h264Main, common}, {VideoEncodeProfile::h264High, common},
                {VideoEncodeProfile::h264ConstrainedHigh, common},
                {VideoEncodeProfile::h264High422, {RawVideoLayout::nv16}},
                {VideoEncodeProfile::h264High444Predictive, {RawVideoLayout::nv24}}};
    case VideoCodec::hevc:
        return {{VideoEncodeProfile::hevcMain, common},
                {VideoEncodeProfile::hevcMain10, {RawVideoLayout::p010, RawVideoLayout::nv12, RawVideoLayout::i420, RawVideoLayout::bgra, RawVideoLayout::rgba}},
                {VideoEncodeProfile::hevcMainStill, common}};
    case VideoCodec::jpeg: return {{VideoEncodeProfile::jpegBaseline, common}};
    case VideoCodec::vp9: break;
    }
    return {};
}

// The profile the codec-wide measurements use: one every encoder handles and
// that permits the features measured (B-frames, for one).
const VideoEncodeProfileCapability* referenceProfile(const VideoEncodeCapability& capability)
{
    static const VideoEncodeProfile preferred[] = {VideoEncodeProfile::h264High, VideoEncodeProfile::h264Main,
        VideoEncodeProfile::hevcMain, VideoEncodeProfile::jpegBaseline};
    for (const auto wanted : preferred)
        for (const auto& profile : capability.profiles)
            if (profile.profile == wanted) return &profile;
    return capability.profiles.empty() ? nullptr : &capability.profiles.front();
}

RawVideoLayout referenceInput(const VideoEncodeProfileCapability& profile)
{
    return std::find(profile.inputs.begin(), profile.inputs.end(), RawVideoLayout::nv12) != profile.inputs.end()
        ? RawVideoLayout::nv12 : profile.inputs.front();
}

// The profile values the encoder says ProfileLevel takes. An empty set means
// the encoder would not say, and every candidate is tried.
std::set<std::string> listedProfileValues(CMVideoCodecType type)
{
    std::set<std::string> values;
    NSDictionary* specification = @{(__bridge NSString*)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES};
    CFDictionaryRef properties = nullptr;
    CFStringRef identifier = nullptr;
    if (VTCopySupportedPropertyDictionaryForEncoder(1280, 720, type, (__bridge CFDictionaryRef)specification, &identifier, &properties) != noErr)
        return values;
    if (identifier) CFRelease(identifier);
    NSDictionary* dictionary = (__bridge_transfer NSDictionary*)properties;
    NSDictionary* level = dictionary[(__bridge NSString*)kVTCompressionPropertyKey_ProfileLevel];
    NSArray* list = [level isKindOfClass:[NSDictionary class]] ? level[(__bridge NSString*)kVTPropertySupportedValueListKey] : nil;
    if ([list isKindOfClass:[NSArray class]])
        for (id value in list)
            if ([value isKindOfClass:[NSString class]]) values.insert([(NSString*)value UTF8String]);
    return values;
}

// Whether VideoToolbox lists a hardware encoder for the codec.
bool hardwareEncoderListed(CMVideoCodecType type)
{
    CFArrayRef list = nullptr;
    if (VTCopyVideoEncoderList(nullptr, &list) != noErr || !list) return false;
    NSArray* entries = (__bridge_transfer NSArray*)list;
    for (NSDictionary* entry in entries) {
        NSNumber* codec = entry[(__bridge NSString*)kVTVideoEncoderList_CodecType];
        NSNumber* hardware = entry[(__bridge NSString*)kVTVideoEncoderList_IsHardwareAccelerated];
        NSString* identifier = entry[(__bridge NSString*)kVTVideoEncoderList_EncoderID];
        // The depth, disparity and alpha encoders carry the base encoder's
        // identifier after a colon and are other codecs' business.
        if ([codec unsignedIntValue] == type && [hardware boolValue] && ![identifier containsString:@":"]) return true;
    }
    return false;
}

struct Size {
    std::uint32_t width, height;
};

constexpr Size kSizeLadder[] = {{176, 144}, {352, 288}, {640, 480}, {1280, 720}, {1920, 1080}, {3840, 2160},
                                {4096, 2304}, {4096, 4096}, {7680, 4320}, {8192, 4320}, {8192, 8192}};

unsigned secondsToTimeout(const Size& size)
{
    return size.width * size.height >= 3840u * 2160u ? 45'000 : 20'000;
}

unsigned countSessions(const VideoEncodeSettings& settings)
{
    std::vector<std::unique_ptr<ProbeEncoder>> held;
    while (held.size() < kSessionProbeLimit) {
        auto encoder = makeProbeEncoder();
        if (encoder->configureUnchecked(settings) != VideoEncodeStatus::ok) break;
        held.push_back(std::move(encoder));
    }
    return unsigned(held.size());
}

// Frames per second one session sustains: the marginal rate between a short
// and a longer run, which leaves the first frame's latency out.
unsigned measureFramesPerSecond(const VideoEncodeSettings& settings, unsigned shortRun, unsigned longRun)
{
    Probe a {settings, shortRun, true};
    Probe b {settings, longRun, true};
    const auto first = runProbe(a);
    const auto second = runProbe(b);
    if (!first.encoded || !second.encoded || second.seconds <= first.seconds) return 0;
    return unsigned(double(longRun - shortRun) / (second.seconds - first.seconds) + 0.5);
}

VideoHdrStaticMetadata sampleHdr()
{
    VideoHdrStaticMetadata hdr;
    hdr.displayPrimaries = {13250, 34500, 7500, 3000, 34000, 16000};   // P3 green, blue, red
    hdr.whitePointX = 15635;
    hdr.whitePointY = 16450;
    hdr.maximumLuminance = 10'000'000;
    hdr.minimumLuminance = 50;
    hdr.maximumContentLightLevel = 1000;
    hdr.maximumFrameAverageLightLevel = 400;
    return hdr;
}

std::optional<CodecDiscovery> discoverCodec(VideoCodec codec)
{
    const CMVideoCodecType type = codec == VideoCodec::h264 ? kCMVideoCodecType_H264
        : codec == VideoCodec::hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_JPEG;
    if (!hardwareEncoderListed(type)) return std::nullopt;
    const bool jpeg = codec == VideoCodec::jpeg;
    CodecDiscovery found;
    auto& capability = found.capability;
    capability.codec = codec;
    capability.sizeAlignment = 2;

    // Profiles and the layouts each accepts.
    const auto listed = jpeg ? std::set<std::string> {} : listedProfileValues(type);
    for (const auto& candidate : candidates(codec)) {
        if (!listed.empty() && !listed.count(autoLevelName(candidate.profile))) continue;
        VideoEncodeProfileCapability profile;
        profile.profile = candidate.profile;
        for (const auto layout : candidate.layouts) {
            const Probe probe {baseSettings(codec, candidate.profile, layout)};
            if (proves(probe, runProbe(probe))) profile.inputs.push_back(layout);
        }
        if (!profile.inputs.empty()) capability.profiles.push_back(std::move(profile));
    }
    if (capability.profiles.empty()) return std::nullopt;
    const auto reference = *referenceProfile(capability);
    const auto input = referenceInput(reference);
    const auto referenceSettings = [&](std::uint32_t width, std::uint32_t height) {
        return baseSettings(codec, reference.profile, input, width, height);
    };

    // Levels. A level is selectable where the stream states the one it was
    // asked for, which the encoder guarantees by raising the level in the
    // parameter sets VideoToolbox wrote (H.264, 5.2; HEVC, 4.0, which no
    // picture of a probe's size needs).
    for (auto& profile : capability.profiles) {
        if (codec == VideoCodec::jpeg) continue;
        const unsigned asked = codec == VideoCodec::h264 ? 52 : 120;
        Probe probe {baseSettings(codec, profile.profile, profile.inputs.front())};
        probe.settings.level = asked;
        const auto stream = runProbe(probe);
        if (codec == VideoCodec::h264) profile.maximumLevel = asked;
        profile.levelSelectable = stream.encoded && analyze(codec, stream.frames).levelIdc == asked;
    }

    // Sizes: the smallest square that encodes, then a ladder up to the first
    // size that does not, then the extreme aspect ratios of the result. A
    // video stream is never listed below kVideoEncodeMinimumSide, so its probe
    // starts there: smaller pictures encode, but a guest's quality check
    // rejects them. A JPEG still has no such check.
    capability.minimumWidth = kProbeWidth;
    capability.minimumHeight = kProbeHeight;
    const std::vector<std::uint32_t> smallest = jpeg ? std::vector<std::uint32_t> {16, 32, 48, 64, 96}
                                                     : std::vector<std::uint32_t> {kVideoEncodeMinimumSide, 96};
    for (const std::uint32_t side : smallest) {
        const Probe probe {referenceSettings(side, side), 2};
        if (proves(probe, runProbe(probe))) {
            capability.minimumWidth = capability.minimumHeight = side;
            break;
        }
    }
    capability.maximumWidth = kProbeWidth;
    capability.maximumHeight = kProbeHeight;
    unsigned observedLevel = 0;
    for (const auto& size : kSizeLadder) {
        Probe probe {referenceSettings(size.width, size.height), 1};
        probe.timeoutMilliseconds = secondsToTimeout(size);
        const auto stream = runProbe(probe);
        if (!proves(probe, stream)) break;
        capability.maximumWidth = std::max(capability.maximumWidth, size.width);
        capability.maximumHeight = std::max(capability.maximumHeight, size.height);
        if (codec == VideoCodec::hevc && std::uint64_t(size.width) * size.height <= kHevcLevelSixLumaSamples)
            observedLevel = std::max(observedLevel, analyze(codec, stream.frames).levelIdc);
    }
    const auto narrow = [&](std::uint32_t width, std::uint32_t height) {
        const Probe probe {referenceSettings(width, height), 1};
        return proves(probe, runProbe(probe));
    };
    while (capability.maximumWidth > 1280 && !narrow(capability.maximumWidth, capability.minimumHeight))
        capability.maximumWidth /= 2;
    while (capability.maximumHeight > 1280 && !narrow(capability.minimumWidth, capability.maximumHeight))
        capability.maximumHeight /= 2;
    if (codec == VideoCodec::hevc)
        for (auto& profile : capability.profiles) profile.maximumLevel = observedLevel;
    if (jpeg) {
        // The hardware JPEG encoder keeps an odd picture exactly.
        const Probe probe {referenceSettings(65, 33), 1};
        if (proves(probe, runProbe(probe))) capability.sizeAlignment = 1;
    }

    // Rate control. Variable bitrate is the encoder's baseline: without it
    // the codec is not usable.
    if (jpeg) {
        capability.rateControls = {VideoRateControl::constantQuality};
    } else {
        for (const auto mode : {VideoRateControl::variableBitrate, VideoRateControl::constantBitrate,
                                VideoRateControl::constantQuality, VideoRateControl::constantQp}) {
            Probe probe {referenceSettings(kProbeWidth, kProbeHeight), 4};
            probe.settings.rateControl = mode;
            if (!proves(probe, runProbe(probe))) continue;
            if (mode != VideoRateControl::variableBitrate && !modeTakesEffect(codec, reference, mode)) continue;
            capability.rateControls.push_back(mode);
        }
        if (capability.rateControls.empty() || capability.rateControls.front() != VideoRateControl::variableBitrate) return std::nullopt;

        Probe bounds {referenceSettings(kProbeWidth, kProbeHeight), 4};
        bounds.settings.minimumQp = 0;
        bounds.settings.maximumQp = 51;
        if (proves(bounds, runProbe(bounds))) {
            capability.minimumQp = 0;
            capability.maximumQp = 51;
        }
        for (const std::uint64_t bitrate : {300'000'000ull, 240'000'000ull, 100'000'000ull, 50'000'000ull, 20'000'000ull}) {
            Probe probe {referenceSettings(kProbeWidth, kProbeHeight), 4};
            probe.settings.bitrate = bitrate;
            if (proves(probe, runProbe(probe))) {
                capability.maximumBitrate = bitrate;
                break;
            }
        }
        if (!capability.maximumBitrate) return std::nullopt;

        // B-frames: whatever the encoder does when it is allowed to reorder.
        // The hardware encoders refuse every property that would limit the run
        // (MaxFrameDelayCount, ReferenceBufferCount), so the longest one seen
        // is the run, which a stream codes whole or not at all.
        Probe reorder {referenceSettings(kProbeWidth, kProbeHeight), 36, true};
        reorder.settings.bFrames = 4;
        const auto reordered = runProbe(reorder);
        if (reordered.encoded) capability.maximumBFrames = std::min(analyze(codec, reordered.frames).longestBidirectionalRun, 4u);

        // Temporal layers exist only on the low-latency encoder, which also
        // writes fewer profiles than the standard one.
        Probe layered {referenceSettings(kProbeWidth, kProbeHeight), 24, true};
        layered.settings.temporalLayers = 2;
        layered.settings.realTime = true;
        const auto layeredStream = runProbe(layered);
        const auto layeredFacts = analyze(codec, layeredStream.frames);
        if (layeredStream.encoded && writesProfile(reference.profile, layeredFacts) && layeredFacts.upperLayerFrames > 0 &&
            layeredFacts.sawLowerLayerFrame) {
            capability.maximumTemporalLayers = 2;
            for (const auto& profile : capability.profiles) {
                Probe each {baseSettings(codec, profile.profile, profile.inputs.front()), 2};
                each.settings.temporalLayers = 2;
                each.settings.realTime = true;
                if (proves(each, runProbe(each))) found.lowLatencyProfiles.push_back(profile.profile);
            }
        }
    }

    if (codec == VideoCodec::h264) {
        // The entropy mode is selectable when each choice shows in the PPS.
        const bool baselineOnly = reference.profile == VideoEncodeProfile::h264Baseline ||
                                  reference.profile == VideoEncodeProfile::h264ConstrainedBaseline;
        if (!baselineOnly) {
            bool selectable = true;
            for (const bool cabac : {true, false}) {
                Probe probe {referenceSettings(kProbeWidth, kProbeHeight), 2};
                probe.settings.cabac = cabac;
                const auto stream = runProbe(probe);
                const auto facts = analyze(codec, stream.frames);
                selectable = selectable && stream.encoded && facts.cabac && *facts.cabac == cabac;
            }
            capability.entropyModeSelectable = selectable;
        }
    }

    if (codec == VideoCodec::hevc) {
        const auto main10 = std::find_if(capability.profiles.begin(), capability.profiles.end(),
            [](const auto& profile) { return profile.profile == VideoEncodeProfile::hevcMain10; });
        if (main10 != capability.profiles.end()) {
            Probe probe {baseSettings(codec, VideoEncodeProfile::hevcMain10, main10->inputs.front()), 2};
            probe.settings.color = {VideoColorPrimaries::bt2020, VideoTransferFunction::pq, VideoMatrixCoefficients::bt2020, false};
            probe.settings.hdr = sampleHdr();
            const auto stream = runProbe(probe);
            bool mastering = false, light = false;
            for (const auto& message : analyze(codec, stream.frames).sei) {
                mastering = mastering || (message.type == 137 && message.payload.size() == 24);
                light = light || (message.type == 144 && message.payload.size() == 4);
            }
            capability.hdrStaticMetadata = proves(probe, stream) && mastering && light;
        }
    }

    capability.concurrentSessions = countSessions(referenceSettings(kProbeWidth, kProbeHeight));
    if (!capability.concurrentSessions) return std::nullopt;

    // Throughput, for the codecs a guest streams in real time.
    if (!jpeg) {
        auto settings = referenceSettings(1280, 720);
        settings.bitrate = 8'000'000;
        capability.measuredFramesPerSecond720p = measureFramesPerSecond(settings, 10, 70);
        settings = referenceSettings(1920, 1080);
        settings.bitrate = 16'000'000;
        capability.measuredFramesPerSecond1080p = measureFramesPerSecond(settings, 10, 50);
    }
    return found;
}

// ---- The cache.

NSString* cachePath()
{
#ifndef UML_APP_STORE_RUNTIME
    if (const char* override_ = std::getenv("UML_VIDEO_ENCODE_CACHE")) return override_[0] ? [NSString stringWithUTF8String:override_] : nil;
#endif
#if TARGET_OS_OSX
    return [NSTemporaryDirectory() stringByAppendingPathComponent:@"uml-video-encoder-capabilities.json"];
#else
    NSArray* directories = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
    return directories.count ? [directories[0] stringByAppendingPathComponent:@"uml-video-encoder-capabilities.json"] : nil;
#endif
}

std::string sysctlString(const char* name)
{
    std::size_t size = 0;
    if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0 || size > 256) return {};
    std::string value(size, '\0');
    if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) return {};
    value.resize(std::strlen(value.c_str()));
    return value;
}

// What the cached result is valid for: this machine and this build of the OS.
NSString* hostKey()
{
    return [NSString stringWithFormat:@"%s/%s/%s", sysctlString("hw.machine").c_str(), sysctlString("hw.model").c_str(),
            sysctlString("kern.osversion").c_str()];
}

NSArray* numbers(const std::vector<int>& values)
{
    NSMutableArray* array = [NSMutableArray array];
    for (const int value : values) [array addObject:@(value)];
    return array;
}

NSDictionary* encodeCapability(const CodecDiscovery& entry)
{
    const auto& c = entry.capability;
    NSMutableArray* profiles = [NSMutableArray array];
    for (const auto& profile : c.profiles) {
        std::vector<int> inputs;
        for (const auto layout : profile.inputs) inputs.push_back(int(layout));
        [profiles addObject:@{@"profile": @(int(profile.profile)), @"inputs": numbers(inputs), @"maximumLevel": @(profile.maximumLevel),
                              @"levelSelectable": @(profile.levelSelectable)}];
    }
    std::vector<int> modes, lowLatency;
    for (const auto mode : c.rateControls) modes.push_back(int(mode));
    for (const auto profile : entry.lowLatencyProfiles) lowLatency.push_back(int(profile));
    return @{
        @"codec": @(int(c.codec)), @"profiles": profiles,
        @"minimumWidth": @(c.minimumWidth), @"minimumHeight": @(c.minimumHeight),
        @"maximumWidth": @(c.maximumWidth), @"maximumHeight": @(c.maximumHeight), @"sizeAlignment": @(c.sizeAlignment),
        @"rateControls": numbers(modes), @"maximumBitrate": @(c.maximumBitrate),
        @"minimumQp": @(c.minimumQp), @"maximumQp": @(c.maximumQp), @"maximumBFrames": @(c.maximumBFrames),
        @"maximumTemporalLayers": @(c.maximumTemporalLayers), @"entropyModeSelectable": @(c.entropyModeSelectable),
        @"hdrStaticMetadata": @(c.hdrStaticMetadata), @"concurrentSessions": @(c.concurrentSessions),
        @"fps720": @(c.measuredFramesPerSecond720p), @"fps1080": @(c.measuredFramesPerSecond1080p),
        @"lowLatencyProfiles": numbers(lowLatency),
    };
}

// A reader that refuses a cache file at the first field that is missing or not
// what a writer of this revision stores. A cache is a courtesy: whatever it
// cannot vouch for is rediscovered.
class CacheReader {
public:
    bool good() const { return good_; }
    NSNumber* number(NSDictionary* from, NSString* key)
    {
        id value = from[key];
        if (![value isKindOfClass:[NSNumber class]]) good_ = false;
        return good_ ? value : @0;
    }
    std::uint64_t unsignedValue(NSDictionary* from, NSString* key, std::uint64_t maximum)
    {
        const auto value = [number(from, key) doubleValue];
        if (!(value >= 0) || value > double(maximum)) good_ = false;
        return good_ ? std::uint64_t(value) : 0;
    }
    template<class E> E enumeration(id value, int last)
    {
        const int index = [value isKindOfClass:[NSNumber class]] ? [value intValue] : -1;
        if (index < 0 || index > last) good_ = false;
        return static_cast<E>(good_ ? index : 0);
    }
    template<class E> std::vector<E> enumerations(NSDictionary* from, NSString* key, int last)
    {
        std::vector<E> out;
        NSArray* array = from[key];
        if (![array isKindOfClass:[NSArray class]] || array.count > 32) {
            good_ = false;
            return out;
        }
        for (id value in array) out.push_back(enumeration<E>(value, last));
        return out;
    }
    NSArray* array(NSDictionary* from, NSString* key)
    {
        NSArray* value = from[key];
        if (![value isKindOfClass:[NSArray class]] || value.count > 32) {
            good_ = false;
            return @[];
        }
        return value;
    }
private:
    bool good_ {true};
};

std::optional<Discovery> decodeDiscovery(NSDictionary* root)
{
    if (![root isKindOfClass:[NSDictionary class]]) return std::nullopt;
    CacheReader reader;
    if (reader.unsignedValue(root, @"revision", 1000) != kDiscoveryRevision) return std::nullopt;
    if (![root[@"host"] isKindOfClass:[NSString class]] || ![root[@"host"] isEqualToString:hostKey()]) return std::nullopt;
    Discovery discovery;
    for (NSDictionary* entry in reader.array(root, @"codecs")) {
        if (![entry isKindOfClass:[NSDictionary class]]) return std::nullopt;
        CodecDiscovery decoded;
        auto& c = decoded.capability;
        c.codec = reader.enumeration<VideoCodec>(entry[@"codec"], int(VideoCodec::jpeg));
        for (NSDictionary* item in reader.array(entry, @"profiles")) {
            if (![item isKindOfClass:[NSDictionary class]]) return std::nullopt;
            VideoEncodeProfileCapability profile;
            profile.profile = reader.enumeration<VideoEncodeProfile>(item[@"profile"], int(VideoEncodeProfile::jpegBaseline));
            profile.inputs = reader.enumerations<RawVideoLayout>(item, @"inputs", int(RawVideoLayout::rgba));
            profile.maximumLevel = std::uint32_t(reader.unsignedValue(item, @"maximumLevel", 1000));
            profile.levelSelectable = [reader.number(item, @"levelSelectable") boolValue];
            c.profiles.push_back(std::move(profile));
        }
        c.minimumWidth = std::uint32_t(reader.unsignedValue(entry, @"minimumWidth", 65536));
        c.minimumHeight = std::uint32_t(reader.unsignedValue(entry, @"minimumHeight", 65536));
        c.maximumWidth = std::uint32_t(reader.unsignedValue(entry, @"maximumWidth", 65536));
        c.maximumHeight = std::uint32_t(reader.unsignedValue(entry, @"maximumHeight", 65536));
        c.sizeAlignment = std::uint32_t(reader.unsignedValue(entry, @"sizeAlignment", 64));
        c.rateControls = reader.enumerations<VideoRateControl>(entry, @"rateControls", int(VideoRateControl::constantQp));
        c.maximumBitrate = reader.unsignedValue(entry, @"maximumBitrate", 4'000'000'000ull);
        c.minimumQp = std::uint32_t(reader.unsignedValue(entry, @"minimumQp", 64));
        c.maximumQp = std::uint32_t(reader.unsignedValue(entry, @"maximumQp", 64));
        c.maximumBFrames = std::uint32_t(reader.unsignedValue(entry, @"maximumBFrames", 16));
        c.maximumTemporalLayers = std::uint32_t(reader.unsignedValue(entry, @"maximumTemporalLayers", 8));
        c.entropyModeSelectable = [reader.number(entry, @"entropyModeSelectable") boolValue];
        c.hdrStaticMetadata = [reader.number(entry, @"hdrStaticMetadata") boolValue];
        c.concurrentSessions = std::uint32_t(reader.unsignedValue(entry, @"concurrentSessions", 1024));
        c.measuredFramesPerSecond720p = std::uint32_t(reader.unsignedValue(entry, @"fps720", 100000));
        c.measuredFramesPerSecond1080p = std::uint32_t(reader.unsignedValue(entry, @"fps1080", 100000));
        decoded.lowLatencyProfiles = reader.enumerations<VideoEncodeProfile>(entry, @"lowLatencyProfiles", int(VideoEncodeProfile::jpegBaseline));
        if (!reader.good() || c.profiles.empty() || c.minimumWidth > c.maximumWidth || c.minimumHeight > c.maximumHeight) return std::nullopt;
        // A video size below the floor is a result that predates it, or a file that is not one.
        if (c.codec != VideoCodec::jpeg && (c.minimumWidth < kVideoEncodeMinimumSide || c.minimumHeight < kVideoEncodeMinimumSide))
            return std::nullopt;
        discovery.codecs.push_back(std::move(decoded));
    }
    if (!reader.good()) return std::nullopt;
    return discovery;
}

std::optional<Discovery> loadCache()
{
    NSString* path = cachePath();
    if (!path) return std::nullopt;
    NSData* data = [NSData dataWithContentsOfFile:path options:0 error:nil];
    if (!data || data.length > (1u << 20)) return std::nullopt;
    id root = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    return decodeDiscovery(root);
}

void storeCache(const Discovery& discovery)
{
    NSString* path = cachePath();
    if (!path) return;
    NSMutableArray* codecs = [NSMutableArray array];
    for (const auto& entry : discovery.codecs) [codecs addObject:encodeCapability(entry)];
    NSDictionary* root = @{@"revision": @(kDiscoveryRevision), @"host": hostKey(), @"codecs": codecs};
    NSData* data = [NSJSONSerialization dataWithJSONObject:root options:NSJSONWritingSortedKeys error:nil];
    if (!data) return;
    // Written aside and renamed in, so a reader never sees half a file and a
    // second process racing this one leaves one whole file or the other.
    NSString* temporary = [path stringByAppendingFormat:@".%d.tmp", int(::getpid())];
    if ([data writeToFile:temporary options:0 error:nil]) {
        if (std::rename(temporary.fileSystemRepresentation, path.fileSystemRepresentation) != 0) (void)::unlink(temporary.fileSystemRepresentation);
    }
}

// ---- The process-wide result.

struct State {
    std::mutex mutex;
    std::condition_variable condition;
    bool started {};
    std::shared_ptr<const Discovery> result;
};

State& state()
{
    // Never destroyed: the discovery thread may still be running at exit.
    static State* instance = new State;
    return *instance;
}

void start()
{
    auto& shared = state();
    {
        std::lock_guard lock(shared.mutex);
        if (shared.started) return;
        shared.started = true;
    }
    std::thread([] {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
        @autoreleasepool {
            auto cached = loadCache();
            const bool fromCache = cached.has_value();
            Discovery computed = fromCache ? std::move(*cached) : runDiscovery();
            if (!fromCache) storeCache(computed);
            auto& global = state();
            std::lock_guard lock(global.mutex);
            global.result = std::make_shared<const Discovery>(std::move(computed));
            global.condition.notify_all();
        }
    }).detach();
}

} // namespace

Discovery runDiscovery()
{
    @autoreleasepool {
        Discovery discovery;
        for (const auto codec : {VideoCodec::h264, VideoCodec::hevc, VideoCodec::jpeg})
            if (auto found = discoverCodec(codec)) discovery.codecs.push_back(std::move(*found));
        return discovery;
    }
}

std::shared_ptr<const Discovery> discovery()
{
    start();
    auto& shared = state();
    std::unique_lock lock(shared.mutex);
    // Every probe has its own deadline, so discovery ends; the bound here keeps
    // a caller from waiting on a VideoToolbox that never answers, in which case
    // the host reports no encoder rather than hanging the guest's probe.
    if (!shared.condition.wait_for(lock, kDiscoveryWait, [&] { return shared.result != nullptr; }))
        return std::make_shared<const Discovery>();
    return shared.result;
}

void invalidateDiscoveryCache() noexcept
{
    @autoreleasepool {
        if (NSString* path = cachePath()) (void)::unlink(path.fileSystemRepresentation);
    }
}

} // namespace apple_encoder

void prepareVideoEncodeCapabilities()
{
    apple_encoder::start();
}

std::vector<VideoEncodeCapability> videoEncodeCapabilities()
{
    std::vector<VideoEncodeCapability> capabilities;
    for (const auto& entry : apple_encoder::discovery()->codecs) capabilities.push_back(entry.capability);
    return capabilities;
}

} // namespace uml::platform
