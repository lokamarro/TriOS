// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// What the video encoder tests share: pictures to encode, a loop that feeds an
// encoder and collects what comes out with every wait bounded, and the decode
// and PSNR that say whether the stream is the picture that went in.

#include "apple/video_encoder_bitstream.hpp"
#include "apple/video_encoder_internal.hpp"
#include "require.hpp"
#include "video_decoder.hpp"
#include "video_encoder.hpp"

#include <poll.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace uml::testing::video_encoder {

using namespace uml::platform;
using uml::testing::require;
namespace bits = uml::platform::bitstream;

inline std::string fixtureDirectory;

inline std::vector<std::uint8_t> readFixture(const std::string& name)
{
    std::ifstream file(fixtureDirectory + "/" + name, std::ios::binary);
    if (!file) throw std::runtime_error("missing video fixture: " + fixtureDirectory + "/" + name);
    return {std::istreambuf_iterator<char>(file), {}};
}

// A 4:2:0 picture of 8 or 10 bits, with the luma and the interleaved chroma
// kept apart and tightly packed.
struct Picture {
    std::uint32_t width {};
    std::uint32_t height {};
    bool tenBit {};
    std::vector<std::uint8_t> luma;
    std::vector<std::uint8_t> chroma;

    [[nodiscard]] std::size_t sampleBytes() const { return tenBit ? 2 : 1; }
};

// The checked-in 128 x 96 fixture, as NV12 or P010.
inline Picture fixturePicture(bool tenBit)
{
    Picture picture;
    picture.width = 128;
    picture.height = 96;
    picture.tenBit = tenBit;
    const auto raw = readFixture(tenBit ? "hevc-main10.raw" : "h264-baseline.raw");
    const std::size_t lumaBytes = std::size_t(picture.width) * picture.height * picture.sampleBytes();
    require(raw.size() == lumaBytes * 3 / 2, "fixture size is not 4:2:0");
    picture.luma.assign(raw.begin(), raw.begin() + std::ptrdiff_t(lumaBytes));
    picture.chroma.assign(raw.begin() + std::ptrdiff_t(lumaBytes), raw.end());
    return picture;
}

// Smooth ramps and moving blocks with noise on top, at any even size: busy
// enough that a bitrate target binds.
inline Picture busyPicture(std::uint32_t width, std::uint32_t height, unsigned index)
{
    Picture picture;
    picture.width = width;
    picture.height = height;
    picture.luma.resize(std::size_t(width) * height);
    picture.chroma.resize(std::size_t(width) * height / 2);
    std::uint32_t state = 0x1234567u + index * 7919u;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            state = state * 1664525u + 1013904223u;
            const unsigned block = (((x + index * 3) / 24) + (y / 24)) & 1u;
            picture.luma[std::size_t(y) * width + x] = std::uint8_t(40 + block * 90 + ((x + y + index * 2) % 64) + ((state >> 24) & 31));
        }
    }
    for (std::uint32_t y = 0; y < height / 2; ++y) {
        for (std::uint32_t x = 0; x < width / 2; ++x) {
            state = state * 1664525u + 1013904223u;
            picture.chroma[std::size_t(y) * width + 2 * x] = std::uint8_t(100 + ((x + index) % 40) + ((state >> 27) & 7));
            picture.chroma[std::size_t(y) * width + 2 * x + 1] = std::uint8_t(120 + (y % 40) + ((state >> 24) & 7));
        }
    }
    return picture;
}

// The picture moved `shift` luma samples left, wrapping, so that successive
// frames differ the way a pan does. The shift is even so that a CbCr pair stays
// together.
inline Picture shifted(const Picture& base, unsigned shift)
{
    Picture out = base;
    const std::size_t sample = base.sampleBytes();
    const std::size_t rowBytes = std::size_t(base.width) * sample;
    const std::size_t byteShift = ((shift % base.width) & ~1u) * sample;
    for (std::size_t row = 0; row < base.height; ++row)
        for (std::size_t i = 0; i < rowBytes; ++i)
            out.luma[row * rowBytes + i] = base.luma[row * rowBytes + (i + byteShift) % rowBytes];
    for (std::size_t row = 0; row < base.height / 2; ++row)
        for (std::size_t i = 0; i < rowBytes; ++i)
            out.chroma[row * rowBytes + i] = base.chroma[row * rowBytes + (i + byteShift) % rowBytes];
    return out;
}

inline void copyPlane(WritableVideoFrame& frame, std::size_t plane, const std::uint8_t* source, std::size_t sourceStride, std::size_t rowBytes, std::size_t rows)
{
    auto bytes = frame.plane(plane);
    const auto stride = frame.stride(plane);
    require(frame.rows(plane) >= rows && stride >= rowBytes && bytes.size() >= stride * rows, "frame plane is smaller than the picture");
    for (std::size_t row = 0; row < rows; ++row) std::memcpy(bytes.data() + row * stride, source + row * sourceStride, rowBytes);
}

// Writes `picture` into `frame` in the layout the frame has. 8-bit pictures
// only for the planar and packed layouts other than NV12 and P010.
inline void writePicture(WritableVideoFrame& frame, const Picture& picture, RawVideoLayout layout)
{
    const std::size_t w = picture.width, h = picture.height;
    switch (layout) {
    case RawVideoLayout::nv12:
    case RawVideoLayout::p010: {
        require(frame.planeCount() == 2, "bi-planar frame expected");
        const std::size_t row = w * picture.sampleBytes();
        copyPlane(frame, 0, picture.luma.data(), row, row, h);
        copyPlane(frame, 1, picture.chroma.data(), row, row, h / 2);
        break;
    }
    case RawVideoLayout::i420: {
        require(frame.planeCount() == 3, "planar frame expected");
        std::vector<std::uint8_t> cb(w / 2 * (h / 2)), cr(w / 2 * (h / 2));
        for (std::size_t i = 0; i < cb.size(); ++i) {
            cb[i] = picture.chroma[2 * i];
            cr[i] = picture.chroma[2 * i + 1];
        }
        copyPlane(frame, 0, picture.luma.data(), w, w, h);
        copyPlane(frame, 1, cb.data(), w / 2, w / 2, h / 2);
        copyPlane(frame, 2, cr.data(), w / 2, w / 2, h / 2);
        break;
    }
    case RawVideoLayout::nv16: {
        require(frame.planeCount() == 2, "bi-planar frame expected");
        std::vector<std::uint8_t> chroma(w * h);
        for (std::size_t y = 0; y < h; ++y) std::memcpy(&chroma[y * w], &picture.chroma[(y / 2) * w], w);
        copyPlane(frame, 0, picture.luma.data(), w, w, h);
        copyPlane(frame, 1, chroma.data(), w, w, h);
        break;
    }
    case RawVideoLayout::nv24: {
        require(frame.planeCount() == 2, "bi-planar frame expected");
        std::vector<std::uint8_t> chroma(w * 2 * h);
        for (std::size_t y = 0; y < h; ++y)
            for (std::size_t x = 0; x < w; ++x) {
                chroma[y * w * 2 + 2 * x] = picture.chroma[(y / 2) * w + (x / 2) * 2];
                chroma[y * w * 2 + 2 * x + 1] = picture.chroma[(y / 2) * w + (x / 2) * 2 + 1];
            }
        copyPlane(frame, 0, picture.luma.data(), w, w, h);
        copyPlane(frame, 1, chroma.data(), w * 2, w * 2, h);
        break;
    }
    case RawVideoLayout::bgra:
    case RawVideoLayout::rgba: {
        require(frame.planeCount() == 1, "packed frame expected");
        const bool rgbFirst = layout == RawVideoLayout::rgba;
        std::vector<std::uint8_t> pixels(w * h * 4);
        for (std::size_t y = 0; y < h; ++y)
            for (std::size_t x = 0; x < w; ++x) {
                // Limited-range BT.709, the matrix the stream declares.
                const double yy = (double(picture.luma[y * w + x]) - 16.0) * 255.0 / 219.0;
                const double cb = (double(picture.chroma[(y / 2) * w + (x / 2) * 2]) - 128.0) * 255.0 / 224.0;
                const double cr = (double(picture.chroma[(y / 2) * w + (x / 2) * 2 + 1]) - 128.0) * 255.0 / 224.0;
                const auto clamp = [](double v) { return std::uint8_t(std::clamp(v + 0.5, 0.0, 255.0)); };
                const std::uint8_t blue = clamp(yy + 1.8556 * cb), green = clamp(yy - 0.1873 * cb - 0.4681 * cr), red = clamp(yy + 1.5748 * cr);
                std::uint8_t* pixel = &pixels[(y * w + x) * 4];
                pixel[0] = rgbFirst ? red : blue;
                pixel[1] = green;
                pixel[2] = rgbFirst ? blue : red;
                pixel[3] = 255;
            }
        copyPlane(frame, 0, pixels.data(), w * 4, w * 4, h);
        break;
    }
    }
}

// ---- Feeding an encoder.

inline const char* describe(VideoEncodeStatus status)
{
    switch (status) {
    case VideoEncodeStatus::ok: return "ok";
    case VideoEncodeStatus::unsupported: return "unsupported";
    case VideoEncodeStatus::invalid: return "invalid";
    case VideoEncodeStatus::exhausted: return "exhausted";
    case VideoEncodeStatus::failed: return "failed";
    }
    return "?";
}

inline std::string diagnosticOf(VideoEncoder& encoder)
{
    if (const auto* probe = dynamic_cast<apple_encoder::ProbeEncoder*>(&encoder)) return probe->diagnostic();
    return {};
}

struct Output {
    std::vector<EncodedVideoFrame> items;   // frames and parameter sets, in order
    unsigned errors {};
    unsigned drained {};
    bool timedOut {};

    [[nodiscard]] std::vector<const EncodedVideoFrame*> pictures() const
    {
        std::vector<const EncodedVideoFrame*> out;
        for (const auto& item : items)
            if (item.kind == EncodedVideoFrameKind::frame) out.push_back(&item);
        return out;
    }
    [[nodiscard]] std::size_t bytes(std::size_t from, std::size_t to) const
    {
        const auto frames = pictures();
        std::size_t total = 0;
        for (std::size_t i = from; i < to && i < frames.size(); ++i) total += frames[i]->bytes.size();
        return total;
    }
};

inline void collect(VideoEncoder& encoder, Output& output, int waitMilliseconds)
{
    pollfd readable {encoder.readinessDescriptor(), POLLIN, 0};
    (void)::poll(&readable, 1, waitMilliseconds);
    for (auto& item : encoder.takeFrames()) {
        switch (item.kind) {
        case EncodedVideoFrameKind::frame:
        case EncodedVideoFrameKind::parameterSets: output.items.push_back(std::move(item)); break;
        case EncodedVideoFrameKind::error: ++output.errors; break;
        case EncodedVideoFrameKind::drained: ++output.drained; break;
        }
    }
}

struct Feed {
    unsigned count {};
    std::function<void(WritableVideoFrame&, unsigned)> fill;
    std::function<bool(unsigned)> forceKey;
    std::function<std::uint64_t(unsigned)> timestamp;   // default: the frame's index
    std::function<void(VideoEncoder&, unsigned)> before;   // called ahead of frame `index`
    bool drain {true};
    unsigned timeoutMilliseconds {60'000};
};

// Submits `feed.count` frames, collecting output between submissions, then
// drains. Every wait is bounded; a stalled encoder ends the loop and is
// reported as timed out rather than hanging the test.
inline Output run(VideoEncoder& encoder, const Feed& feed)
{
    Output output;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(feed.timeoutMilliseconds);
    unsigned submitted = 0;
    bool draining = false;
    while (std::chrono::steady_clock::now() < deadline) {
        while (submitted < feed.count) {
            if (feed.before) feed.before(encoder, submitted);
            auto frame = encoder.acquireFrame();
            if (!frame) break;
            if (feed.fill) feed.fill(*frame, submitted);
            encoder.submit(std::move(frame), feed.timestamp ? feed.timestamp(submitted) : submitted, feed.forceKey && feed.forceKey(submitted));
            ++submitted;
        }
        if (submitted == feed.count && feed.drain && !draining) {
            encoder.drain();
            draining = true;
        }
        collect(encoder, output, 20);
        if (submitted == feed.count && (!feed.drain || output.drained)) return output;
    }
    output.timedOut = true;
    return output;
}

// The Annex B units of a frame as (type, size) in order, for the codec.
inline std::vector<std::pair<unsigned, std::size_t>> nalUnits(VideoCodec codec, const EncodedVideoFrame& frame)
{
    std::vector<std::pair<unsigned, std::size_t>> units;
    for (const auto unit : bits::splitAnnexB(frame.bytes))
        units.emplace_back(codec == VideoCodec::hevc ? bits::hevcNalType(unit) : bits::h264NalType(unit), unit.size());
    return units;
}

inline bool hasUnit(VideoCodec codec, const EncodedVideoFrame& frame, unsigned type)
{
    for (const auto& unit : nalUnits(codec, frame))
        if (unit.first == type) return true;
    return false;
}

// ---- Reading the result back.

inline std::optional<VideoProfile> decoderProfile(VideoEncodeProfile profile)
{
    switch (profile) {
    case VideoEncodeProfile::h264Baseline: return VideoProfile::h264Baseline;
    case VideoEncodeProfile::h264ConstrainedBaseline: return VideoProfile::h264ConstrainedBaseline;
    case VideoEncodeProfile::h264Main: return VideoProfile::h264Main;
    case VideoEncodeProfile::h264High:
    case VideoEncodeProfile::h264ConstrainedHigh: return VideoProfile::h264High;
    case VideoEncodeProfile::hevcMain: return VideoProfile::hevcMain;
    case VideoEncodeProfile::hevcMain10: return VideoProfile::hevcMain10;
    case VideoEncodeProfile::jpegBaseline: return VideoProfile::jpegBaseline;
    default: return std::nullopt;
    }
}

inline bool hardwareDecodes(VideoEncodeProfile profile)
{
    const auto wanted = decoderProfile(profile);
    if (!wanted) return false;
    for (const auto& capability : videoDecodeCapabilities())
        if (capability.profile == *wanted) return true;
    return false;
}

inline VideoCodec codecOf(VideoEncodeProfile profile)
{
    switch (profile) {
    case VideoEncodeProfile::hevcMain:
    case VideoEncodeProfile::hevcMain10:
    case VideoEncodeProfile::hevcMainStill: return VideoCodec::hevc;
    case VideoEncodeProfile::jpegBaseline: return VideoCodec::jpeg;
    default: return VideoCodec::h264;
    }
}

// Decodes the pictures of a stream with the hardware decoder, in the order the
// stream gives them, and returns them by timestamp. Parameter sets that
// arrived as frames of their own travel with the next picture. The decoder
// holds only so many undelivered pictures, so they are taken as they come.
inline std::map<std::uint64_t, DecodedVideoFrame> decode(VideoCodec codec, const Output& output)
{
    auto decoder = makeVideoDecoder(codec);
    std::map<std::uint64_t, DecodedVideoFrame> frames;
    const auto take = [&] {
        for (auto& frame : decoder->takeFrames()) frames.emplace(frame.timestamp, std::move(frame));
    };
    std::vector<std::uint8_t> carried;
    for (const auto& item : output.items) {
        if (item.kind == EncodedVideoFrameKind::parameterSets) {
            carried = item.bytes;
            continue;
        }
        carried.insert(carried.end(), item.bytes.begin(), item.bytes.end());
        decoder->submit(carried, item.timestamp);
        carried.clear();
        take();
    }
    decoder->drain();
    take();
    return frames;
}

// Peak-signal-to-noise ratio of the luma, in dB, with both sides brought to
// 8 bits so that a 10-bit stream can be compared with an 8-bit source.
inline double lumaPsnr(const Picture& reference, const DecodedVideoFrame& decoded)
{
    require(decoded.layout == VideoPixelLayout::planarYuv, "decoder did not return planar samples");
    require(decoded.format.width == reference.width && decoded.format.height == reference.height, "decoded size differs");
    const std::size_t samples = std::size_t(reference.width) * reference.height;
    const std::size_t decodedStep = decoded.format.bitDepth > 8 ? 2 : 1;
    require(decoded.luma.size() == samples * decodedStep, "decoded luma size differs");
    const std::size_t referenceStep = reference.sampleBytes();
    double error = 0;
    for (std::size_t i = 0; i < samples; ++i) {
        const double got = decoded.luma[i * decodedStep + decodedStep - 1];
        const double wanted = reference.luma[i * referenceStep + referenceStep - 1];
        error += (got - wanted) * (got - wanted);
    }
    error /= double(samples);
    return error == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / error);
}

inline double meanLumaPsnr(const std::vector<Picture>& references, const std::map<std::uint64_t, DecodedVideoFrame>& decoded)
{
    require(decoded.size() == references.size(), "decoder returned a different number of pictures");
    double total = 0;
    for (std::size_t i = 0; i < references.size(); ++i) {
        const auto found = decoded.find(i);
        require(found != decoded.end(), "decoded picture is missing");
        total += lumaPsnr(references[i], found->second);
    }
    return total / double(references.size());
}

// ---- Reading the bitstream.

struct H264Vui {
    bool colorDescription {};
    unsigned primaries {};
    unsigned transfer {};
    unsigned matrix {};
    bool fullRange {};
};

// The colour description of an H.264 SPS unit's VUI.
inline std::optional<H264Vui> h264Vui(bits::Bytes sps)
{
    const auto rbsp = bits::rbsp(sps, 1);
    bits::BitReader reader(rbsp);
    const unsigned profile = reader.bits(8);
    reader.bits(16);
    reader.unsignedExpGolomb();   // seq_parameter_set_id
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 || profile == 86 ||
        profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 || profile == 135) {
        if (reader.unsignedExpGolomb() == 3) reader.bits(1);
        reader.unsignedExpGolomb();
        reader.unsignedExpGolomb();
        reader.bits(1);
        if (reader.bits(1)) return std::nullopt;   // scaling matrices: not written by the hardware encoder
    }
    reader.unsignedExpGolomb();   // log2_max_frame_num_minus4
    const unsigned pocType = reader.unsignedExpGolomb();
    if (pocType == 0) {
        reader.unsignedExpGolomb();
    } else if (pocType == 1) {
        reader.bits(1);
        reader.unsignedExpGolomb();
        reader.unsignedExpGolomb();
        const unsigned cycle = reader.unsignedExpGolomb();
        for (unsigned i = 0; i < cycle; ++i) reader.unsignedExpGolomb();
    }
    reader.unsignedExpGolomb();   // max_num_ref_frames
    reader.bits(1);
    reader.unsignedExpGolomb();
    reader.unsignedExpGolomb();
    if (!reader.bits(1)) reader.bits(1);   // frame_mbs_only_flag, mb_adaptive_frame_field_flag
    reader.bits(1);                        // direct_8x8_inference_flag
    if (reader.bits(1)) for (int i = 0; i < 4; ++i) reader.unsignedExpGolomb();   // cropping
    if (!reader.bits(1) || !reader.ok()) return std::nullopt;                       // vui_parameters_present_flag
    if (reader.bits(1) && reader.bits(8) == 255) reader.bits(32);                   // aspect ratio
    if (reader.bits(1)) reader.bits(1);                                              // overscan
    H264Vui vui;
    if (reader.bits(1)) {                                                            // video_signal_type_present_flag
        reader.bits(3);
        vui.fullRange = reader.bits(1) != 0;
        if (reader.bits(1)) {
            vui.colorDescription = true;
            vui.primaries = reader.bits(8);
            vui.transfer = reader.bits(8);
            vui.matrix = reader.bits(8);
        }
    }
    if (!reader.ok()) return std::nullopt;
    return vui;
}

inline std::optional<bits::H264SpsInfo> firstH264Sps(const EncodedVideoFrame& frame)
{
    for (const auto unit : bits::splitAnnexB(frame.bytes))
        if (const auto sps = bits::parseH264Sps(unit)) return sps;
    return std::nullopt;
}

inline std::optional<std::vector<std::uint8_t>> firstUnit(VideoCodec codec, const EncodedVideoFrame& frame, unsigned type)
{
    for (const auto unit : bits::splitAnnexB(frame.bytes))
        if ((codec == VideoCodec::hevc ? bits::hevcNalType(unit) : bits::h264NalType(unit)) == type)
            return std::vector<std::uint8_t>(unit.begin(), unit.end());
    return std::nullopt;
}

// ---- Settings.

inline VideoEncodeSettings settingsFor(VideoEncodeProfile profile, RawVideoLayout layout, std::uint32_t width = 128, std::uint32_t height = 96)
{
    VideoEncodeSettings settings;
    settings.codec = codecOf(profile);
    settings.profile = profile;
    settings.input = layout;
    settings.width = width;
    settings.height = height;
    settings.bitrate = 2'000'000;
    settings.quality = 60;
    settings.jpegQuality = 85;
    return settings;
}

inline const VideoEncodeCapability* capabilityOf(const std::vector<VideoEncodeCapability>& capabilities, VideoCodec codec)
{
    for (const auto& capability : capabilities)
        if (capability.codec == codec) return &capability;
    return nullptr;
}

inline bool advertises(const VideoEncodeCapability& capability, VideoRateControl mode)
{
    return std::find(capability.rateControls.begin(), capability.rateControls.end(), mode) != capability.rateControls.end();
}

inline const VideoEncodeProfileCapability* profileOf(const VideoEncodeCapability& capability, VideoEncodeProfile profile)
{
    for (const auto& entry : capability.profiles)
        if (entry.profile == profile) return &entry;
    return nullptr;
}

inline const char* profileName(VideoEncodeProfile profile)
{
    switch (profile) {
    case VideoEncodeProfile::h264Baseline: return "h264-baseline";
    case VideoEncodeProfile::h264ConstrainedBaseline: return "h264-constrained-baseline";
    case VideoEncodeProfile::h264Main: return "h264-main";
    case VideoEncodeProfile::h264High: return "h264-high";
    case VideoEncodeProfile::h264ConstrainedHigh: return "h264-constrained-high";
    case VideoEncodeProfile::h264High422: return "h264-high422";
    case VideoEncodeProfile::h264High444Predictive: return "h264-high444-predictive";
    case VideoEncodeProfile::hevcMain: return "hevc-main";
    case VideoEncodeProfile::hevcMain10: return "hevc-main10";
    case VideoEncodeProfile::hevcMainStill: return "hevc-main-still";
    case VideoEncodeProfile::jpegBaseline: return "jpeg-baseline";
    }
    return "?";
}

inline const char* layoutName(RawVideoLayout layout)
{
    switch (layout) {
    case RawVideoLayout::nv12: return "nv12";
    case RawVideoLayout::i420: return "i420";
    case RawVideoLayout::p010: return "p010";
    case RawVideoLayout::nv16: return "nv16";
    case RawVideoLayout::nv24: return "nv24";
    case RawVideoLayout::bgra: return "bgra";
    case RawVideoLayout::rgba: return "rgba";
    }
    return "?";
}

// A 10-bit picture whose samples are the 8-bit ones, in the high bits of 16.
inline Picture widen(const Picture& eight)
{
    require(!eight.tenBit, "picture is already 10-bit");
    Picture out;
    out.width = eight.width;
    out.height = eight.height;
    out.tenBit = true;
    out.luma.resize(eight.luma.size() * 2);
    out.chroma.resize(eight.chroma.size() * 2);
    for (std::size_t i = 0; i < eight.luma.size(); ++i) {
        out.luma[2 * i] = 0;
        out.luma[2 * i + 1] = eight.luma[i];
    }
    for (std::size_t i = 0; i < eight.chroma.size(); ++i) {
        out.chroma[2 * i] = 0;
        out.chroma[2 * i + 1] = eight.chroma[i];
    }
    return out;
}

// What the stream's first parameter set says about the profile asked for,
// by the same criteria discovery verifies a profile with.
inline bool streamWritesProfile(VideoEncodeProfile profile, const EncodedVideoFrame& key)
{
    const auto codec = codecOf(profile);
    if (codec == VideoCodec::jpeg) return true;
    if (codec == VideoCodec::hevc) {
        for (const auto unit : bits::splitAnnexB(key.bytes))
            if (const auto sps = bits::parseHevcSps(unit))
                return sps->profileIdc == (profile == VideoEncodeProfile::hevcMain ? 1u : profile == VideoEncodeProfile::hevcMain10 ? 2u : 3u);
        return false;
    }
    const auto sps = firstH264Sps(key);
    if (!sps) return false;
    const unsigned idc = sps->profileIdc, flags = sps->constraintFlags;
    switch (profile) {
    case VideoEncodeProfile::h264Baseline: return idc == 66;
    case VideoEncodeProfile::h264ConstrainedBaseline: return idc == 66 && (flags & 0x40);
    case VideoEncodeProfile::h264Main: return idc == 77;
    case VideoEncodeProfile::h264High: return idc == 100 && (flags & 0x0c) == 0;
    case VideoEncodeProfile::h264ConstrainedHigh: return idc == 100 && (flags & 0x0c) == 0x0c;
    case VideoEncodeProfile::h264High422: return idc == 122;
    case VideoEncodeProfile::h264High444Predictive: return idc == 244;
    default: return false;
    }
}

// ---- A configured encoder with its source pictures.

inline std::unique_ptr<VideoEncoder> open(const VideoEncodeSettings& settings, const char* label)
{
    auto encoder = makeVideoEncoder();
    const auto status = encoder->configure(settings);
    if (status != VideoEncodeStatus::ok) {
        std::fprintf(stderr, "%s: configure returned %s (%s)\n", label, describe(status), diagnosticOf(*encoder).c_str());
        require(false, "configure failed");
    }
    return encoder;
}

struct Encoded {
    std::unique_ptr<VideoEncoder> encoder;
    std::vector<Picture> sources;
    Output output;
};

// Configures an encoder, feeds it `count` pictures from `source`, and drains.
inline Encoded encode(const VideoEncodeSettings& settings, unsigned count,
                      const std::function<Picture(unsigned)>& source, const char* label, Feed feed = {})
{
    Encoded result;
    result.encoder = open(settings, label);
    for (unsigned i = 0; i < count; ++i) result.sources.push_back(source(i));
    feed.count = count;
    const auto layout = settings.input;
    feed.fill = [&](WritableVideoFrame& frame, unsigned index) { writePicture(frame, result.sources[index], layout); };
    result.output = run(*result.encoder, feed);
    if (result.output.timedOut) std::fprintf(stderr, "%s: encoder stalled (%s)\n", label, diagnosticOf(*result.encoder).c_str());
    require(!result.output.timedOut, "encoder did not finish");
    return result;
}

// The picture source of a layout: the checked-in 128 x 96 frame, panned.
inline std::function<Picture(unsigned)> fixtureSource(RawVideoLayout layout)
{
    const Picture base = fixturePicture(layout == RawVideoLayout::p010);
    return [base](unsigned index) { return shifted(base, index * 2); };
}

inline std::function<Picture(unsigned)> busySource(std::uint32_t width, std::uint32_t height, bool tenBit = false)
{
    return [=](unsigned index) {
        const Picture eight = busyPicture(width, height, index);
        return tenBit ? widen(eight) : eight;
    };
}

inline double bitsPerSecond(const Output& output, double framesPerSecond)
{
    const auto frames = output.pictures();
    if (frames.empty()) return 0;
    return double(output.bytes(0, frames.size())) * 8.0 * framesPerSecond / double(frames.size());
}

} // namespace uml::testing::video_encoder
