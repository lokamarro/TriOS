// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The level a coded video stream states, and the limits behind it.
//
// VideoToolbox chooses a level itself for most requests and writes it into the
// parameter sets: for H.264 it has constants only for Baseline 1.3 and for the
// levels from 3.0, it weighs the frame rate and the picture size but not the
// bit rate, and for HEVC it has no level to name at all. The stream a guest
// asked to be level 1.0 can come back as level 1.1, and a CIF stream at
// 832 kbit/s as level 1.3, whose limit is 768. This header decides what the
// parameter sets state instead:
//
//   - at least the level the caller asked for, and at least the lowest level
//     the picture size, frame rate and bit rate need, so a level only rises;
//   - and no more than that when the stream provably fits it: the level is
//     lowered from VideoToolbox's choice to the lowest level that holds every
//     limit the settings and the parameter sets let the encoder prove.
//
// Plain C++ without VideoToolbox, so that the decision is unit-tested on units
// built here.

#include "video_encoder.hpp"
#include "video_encoder_bitstream.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

namespace uml::platform::levels {

// ---- H.264 (ITU-T H.264, Table A-1).

struct H264LevelLimits {
    unsigned idc;                       // level_idc; 9 is level 1b
    std::uint64_t macroblocksPerSecond; // MaxMBPS
    std::uint32_t frameMacroblocks;     // MaxFS
    std::uint32_t dpbMacroblocks;       // MaxDpbMbs
    std::uint32_t kilobitsPerSecond;    // MaxBR, in units of cpbBrVclFactor
    std::uint32_t cpbKilobits;          // MaxCPB, in units of cpbBrVclFactor
    std::uint32_t verticalMvRange;      // MaxVmvR: luma samples either way
    bool frameMbsOnly;                  // frame_mbs_only_flag must be 1
};

// In order of rank, which is not the order of level_idc: level 1b sits between
// 1.0 and 1.1.
inline constexpr H264LevelLimits kH264Levels[] = {
    {10, 1485, 99, 396, 64, 175, 64, true},
    {9, 1485, 99, 396, 128, 350, 64, true},
    {11, 3000, 396, 900, 192, 500, 128, true},
    {12, 6000, 396, 2376, 384, 1000, 128, true},
    {13, 11880, 396, 2376, 768, 2000, 128, true},
    {20, 11880, 396, 2376, 2000, 2000, 128, true},
    {21, 19800, 792, 4752, 4000, 4000, 256, false},
    {22, 20250, 1620, 8100, 4000, 4000, 256, false},
    {30, 40500, 1620, 8100, 10000, 10000, 256, false},
    {31, 108000, 3600, 18000, 14000, 14000, 512, false},
    {32, 216000, 5120, 20480, 20000, 20000, 512, false},
    {40, 245760, 8192, 32768, 20000, 25000, 512, false},
    {41, 245760, 8192, 32768, 50000, 62500, 512, false},
    {42, 522240, 8704, 34816, 50000, 62500, 512, false},
    {50, 589824, 22080, 110400, 135000, 135000, 512, false},
    {51, 983040, 36864, 184320, 240000, 240000, 512, false},
    {52, 2073600, 36864, 184320, 240000, 240000, 512, false},
    {60, 4177920, 139264, 696320, 240000, 240000, 8192, false},
    {61, 8355840, 139264, 696320, 480000, 480000, 8192, false},
    {62, 16711680, 139264, 696320, 800000, 800000, 8192, false},
};

// Orders level_idc values by level: 1b is above 1.0 and below 1.1.
[[nodiscard]] constexpr unsigned h264Rank(unsigned idc) noexcept { return idc == 9 ? 105 : idc * 10; }

// cpbBrVclFactor of Table A-2 over 1000; zero for a profile whose factor this
// does not know.
[[nodiscard]] constexpr double h264BitrateFactor(unsigned profileIdc) noexcept
{
    switch (profileIdc) {
    case 66: case 77: case 88: return 1.0;
    case 100: return 1.25;
    case 110: return 3.0;
    case 122: case 244: case 44: return 4.0;
    default: return 0.0;
    }
}

[[nodiscard]] constexpr double h264BitrateFactor(VideoEncodeProfile profile) noexcept
{
    switch (profile) {
    case VideoEncodeProfile::h264High:
    case VideoEncodeProfile::h264ConstrainedHigh: return 1.25;
    case VideoEncodeProfile::h264High422: return 4.0;
    case VideoEncodeProfile::h264High444Predictive: return 4.0;
    default: return 1.0;
    }
}

[[nodiscard]] inline double framesPerSecond(const VideoEncodeSettings& settings) noexcept
{
    return settings.frameRateDenominator ? double(settings.frameRateNumerator) / double(settings.frameRateDenominator) : 30.0;
}

// The lowest level (never 1b) whose limits hold the picture, the frame rate and
// the bit rate the settings call for.
[[nodiscard]] inline unsigned h264RequiredLevel(VideoEncodeProfile profile, const VideoEncodeSettings& settings)
{
    const std::uint64_t widthMacroblocks = (settings.width + 15) / 16, heightMacroblocks = (settings.height + 15) / 16;
    const std::uint64_t macroblocks = widthMacroblocks * heightMacroblocks;
    const double rate = framesPerSecond(settings);
    const double factor = h264BitrateFactor(profile);
    const double peak = double(std::max(settings.bitrate, settings.peakBitrate));
    for (const auto& level : kH264Levels) {
        if (level.idc == 9) continue;
        if (macroblocks > level.frameMacroblocks) continue;
        // Neither side of the picture may exceed what a picture of the largest size could be as a strip.
        if (widthMacroblocks * widthMacroblocks > 8ull * level.frameMacroblocks ||
            heightMacroblocks * heightMacroblocks > 8ull * level.frameMacroblocks) continue;
        if (double(macroblocks) * rate > double(level.macroblocksPerSecond)) continue;
        if (peak > double(level.kilobitsPerSecond) * 1000.0 * factor) continue;
        return level.idc;
    }
    return 62;
}

// ---- HEVC (ITU-T H.265, Tables A.8 and A.9, Main tier).

struct HevcLevelLimits {
    unsigned idc;   // general_level_idc: 30 times the level
    std::uint64_t lumaSamplesPerSecond;   // MaxLumaSr
    std::uint32_t lumaPictureSize;        // MaxLumaPs
    std::uint32_t kilobitsPerSecond;      // MaxBR
    std::uint32_t cpbKilobits;            // MaxCPB
};

inline constexpr HevcLevelLimits kHevcLevels[] = {
    {30, 552960, 36864, 128, 350},          {60, 3686400, 122880, 1500, 1500},
    {63, 7372800, 245760, 3000, 3000},      {90, 16588800, 552960, 6000, 6000},
    {93, 33177600, 983040, 10000, 10000},   {120, 66846720, 2228224, 12000, 12000},
    {123, 133693440, 2228224, 20000, 20000}, {150, 267386880, 8912896, 25000, 25000},
    {153, 534773760, 8912896, 40000, 40000}, {156, 1069547520, 8912896, 60000, 60000},
    {180, 1069547520, 35651584, 60000, 60000}, {183, 2139095040, 35651584, 120000, 120000},
    {186, 4278190080, 35651584, 240000, 240000},
};

// MaxDpbSize of A.4.2 for a profile with maxDpbPicBuf 6.
[[nodiscard]] constexpr unsigned hevcMaxDpbSize(const HevcLevelLimits& level, std::uint64_t pictureSize) noexcept
{
    if (pictureSize <= (level.lumaPictureSize >> 2)) return 16;
    if (pictureSize <= (level.lumaPictureSize >> 1)) return 12;
    if (pictureSize <= ((level.lumaPictureSize * 3ull) >> 2)) return 8;
    return 6;
}

[[nodiscard]] inline unsigned hevcRequiredLevel(const VideoEncodeSettings& settings)
{
    const std::uint64_t samples = std::uint64_t(settings.width) * settings.height;
    const double rate = framesPerSecond(settings);
    const double peak = double(std::max(settings.bitrate, settings.peakBitrate));
    for (const auto& level : kHevcLevels) {
        if (samples > level.lumaPictureSize) continue;
        if (std::uint64_t(settings.width) * settings.width > 8ull * level.lumaPictureSize ||
            std::uint64_t(settings.height) * settings.height > 8ull * level.lumaPictureSize) continue;
        if (double(samples) * rate > double(level.lumaSamplesPerSecond)) continue;
        if (peak > double(level.kilobitsPerSecond) * 1000.0) continue;
        return level.idc;
    }
    return 186;
}

// ---- What a stream states.

// Frames a stream with B-frames holds back for reordering, as the longest
// reorder lag the hardware encoders were seen to code.
inline constexpr unsigned kReorderLag = 3;

// What the settings let the encoder prove about a stream. The level the
// parameter sets state is decided from this and from the parameter sets.
struct Policy {
    unsigned floor {};   // the lowest level that may be stated, in level_idc (H.264) or general_level_idc (HEVC)
    double frameRate {30.0};
    // Bits per second the rate control holds the stream under over any window
    // of a second: the peak of a variable bit rate stream, the rate of a
    // constant one. Zero when nothing bounds it (constant quality or QP, a
    // variable rate without a peak): the stream is then left at the level
    // VideoToolbox chose, or raised.
    std::uint64_t boundedBitrate {};
    std::uint64_t bufferBits {};   // the caller's buffer size; zero when none was asked for
    bool reorders {};              // B-frames may be coded
};

// The level the stream states at least: the one asked for, and the lowest one
// its picture size, frame rate and bit rate fit.
[[nodiscard]] inline unsigned statedLevelFloor(const VideoEncodeSettings& settings)
{
    switch (settings.codec) {
    case VideoCodec::h264: {
        const unsigned required = h264RequiredLevel(settings.profile, settings);
        return h264Rank(settings.level) > h264Rank(required) ? settings.level : required;
    }
    case VideoCodec::hevc: return std::max(settings.level, hevcRequiredLevel(settings));
    default: return 0;
    }
}

[[nodiscard]] inline Policy makePolicy(const VideoEncodeSettings& settings, bool reorders, bool peakEnforced)
{
    Policy policy;
    policy.floor = statedLevelFloor(settings);
    policy.frameRate = framesPerSecond(settings);
    policy.reorders = reorders;
    policy.bufferBits = settings.bufferBits;
    switch (settings.rateControl) {
    case VideoRateControl::constantBitrate: policy.boundedBitrate = settings.bitrate; break;
    case VideoRateControl::variableBitrate:
        if (settings.peakBitrate && peakEnforced) policy.boundedBitrate = std::max(settings.bitrate, settings.peakBitrate);
        break;
    default: break;
    }
    return policy;
}

namespace detail {

// The bit rate and the CPB the policy and the parameter set hold under, against
// a level's limits in bits; false when the stream may exceed either.
[[nodiscard]] inline bool holdsRate(const Policy& policy, double rateLimit, double cpbLimit, std::uint64_t hrdBitRate, std::uint64_t hrdCpb)
{
    if (!policy.boundedBitrate || double(policy.boundedBitrate) > rateLimit) return false;
    // Any second of the stream is at most the bounded rate, so a buffer of that
    // size, filled at that rate, never underflows or overflows.
    if (double(policy.boundedBitrate) > cpbLimit) return false;
    if (policy.bufferBits && double(policy.bufferBits) > cpbLimit) return false;
    if (hrdBitRate && double(hrdBitRate) > rateLimit) return false;
    if (hrdCpb && double(hrdCpb) > cpbLimit) return false;
    return true;
}

[[nodiscard]] inline bool h264Fits(const H264LevelLimits& level, const bitstream::H264SpsLimits& sps, double factor, const Policy& policy)
{
    if (level.frameMbsOnly && !sps.frameMbsOnly) return false;
    const std::uint64_t width = sps.widthMacroblocks, height = sps.heightMacroblocks;
    const std::uint64_t macroblocks = width * height;
    if (!macroblocks || macroblocks > level.frameMacroblocks) return false;
    if (width * width > 8ull * level.frameMacroblocks || height * height > 8ull * level.frameMacroblocks) return false;
    double rate = policy.frameRate;
    if (sps.timingInfo && sps.numUnitsInTick) rate = std::max(rate, double(sps.timeScale) / (2.0 * double(sps.numUnitsInTick)));
    if (double(macroblocks) * rate > double(level.macroblocksPerSecond)) return false;
    if (!holdsRate(policy, double(level.kilobitsPerSecond) * 1000.0 * factor, double(level.cpbKilobits) * 1000.0 * factor, sps.hrdBitRate, sps.hrdCpbSize))
        return false;
    const unsigned dpbFrames = unsigned(std::min<std::uint64_t>(level.dpbMacroblocks / macroblocks, 16));
    if (sps.maxNumRefFrames > dpbFrames) return false;
    if (sps.bitstreamRestriction) {
        if (sps.maxDecFrameBuffering > dpbFrames || sps.maxDecFrameBuffering < sps.maxNumRefFrames) return false;
        // log2_max_mv_length_vertical is in quarter samples.
        if (sps.log2MaxMvLengthVertical > 31 || (1ull << sps.log2MaxMvLengthVertical) > 4ull * level.verticalMvRange) return false;
    } else if (sps.maxNumRefFrames + (policy.reorders ? kReorderLag : 0) > dpbFrames) {
        // No bitstream restriction: the decoder holds MaxDpbFrames and the stream
        // needs its references and the frames waiting for output.
        return false;
    }
    return true;
}

[[nodiscard]] inline bool hevcFits(const HevcLevelLimits& level, const bitstream::HevcSpsFormat& sps, const Policy& policy)
{
    const std::uint64_t width = sps.width, height = sps.height;
    const std::uint64_t samples = width * height;
    if (!samples || samples > level.lumaPictureSize) return false;
    if (width * width > 8ull * level.lumaPictureSize || height * height > 8ull * level.lumaPictureSize) return false;
    double rate = policy.frameRate;
    if (sps.timingInfo && sps.numUnitsInTick) rate = std::max(rate, double(sps.timeScale) / double(sps.numUnitsInTick));
    if (double(samples) * rate > double(level.lumaSamplesPerSecond)) return false;
    if (!holdsRate(policy, double(level.kilobitsPerSecond) * 1000.0, double(level.cpbKilobits) * 1000.0, 0, 0)) return false;
    return sps.maxDecPicBuffering <= hevcMaxDpbSize(level, samples);
}

} // namespace detail

// The level an H.264 SPS should state under `policy`, or nullopt to leave it.
// A level below the floor rises to it. A level above the floor falls to the
// lowest level, from the floor up, that holds every limit the policy and the
// SPS prove; where none below the stated one does, it stays.
[[nodiscard]] inline std::optional<unsigned> chooseH264Level(bitstream::Bytes sps, const Policy& policy)
{
    const auto stated = bitstream::h264Level(sps);
    if (!stated || !policy.floor) return std::nullopt;
    if (h264Rank(*stated) < h264Rank(policy.floor)) return policy.floor;
    const auto limits = bitstream::parseH264SpsLimits(sps);
    if (!limits) return std::nullopt;
    const double factor = h264BitrateFactor(limits->profileIdc);
    if (factor == 0.0) return std::nullopt;
    for (const auto& level : kH264Levels) {
        if (h264Rank(level.idc) < h264Rank(policy.floor)) continue;
        if (h264Rank(level.idc) >= h264Rank(*stated)) break;
        if (level.idc == 9 && policy.floor != 9) continue;   // level 1b is stated only where it was asked for
        if (detail::h264Fits(level, *limits, factor, policy)) return level.idc;
    }
    return std::nullopt;
}

// The level an HEVC stream should state, from its SPS and PPS: the same for the
// VPS and the SPS. nullopt leaves them as written.
[[nodiscard]] inline std::optional<unsigned> chooseHevcLevel(bitstream::Bytes sps, bitstream::Bytes pps, const Policy& policy)
{
    const auto stated = bitstream::hevcLevel(sps);
    if (!stated || !policy.floor) return std::nullopt;
    if (*stated < policy.floor) return policy.floor;
    const auto format = bitstream::parseHevcSpsFormat(sps);
    const auto tiles = bitstream::parseHevcPps(pps);
    // The Main tier limits only; a lone temporal layer, because the levels of
    // the others are stated beside the general one; one slice-sized picture
    // without tiles; and no HRD, whose rates would need walking.
    if (!format || !tiles || format->highTier || format->maxSubLayersMinus1 != 0 || format->subLayerLevels || format->hrd ||
        tiles->tilesEnabled)
        return std::nullopt;
    for (const auto& level : kHevcLevels) {
        if (level.idc < policy.floor) continue;
        if (level.idc >= *stated) break;
        if (detail::hevcFits(level, *format, policy)) return level.idc;
    }
    return std::nullopt;
}

// Restates the level in the parameter sets of one key frame, as units without
// start codes. Returns the level the stream now states, or zero for a codec or
// units it does not read.
inline unsigned restateLevel(VideoCodec codec, std::vector<std::vector<std::uint8_t>>& units, const Policy& policy)
{
    if (codec == VideoCodec::h264) {
        for (auto& unit : units) {
            if (bitstream::h264NalType(unit) != bitstream::h264NalSps) continue;
            if (const auto level = chooseH264Level(unit, policy)) (void)bitstream::setH264Level(unit, *level);
            return bitstream::h264Level(unit).value_or(0);
        }
        return 0;
    }
    if (codec != VideoCodec::hevc) return 0;
    const std::vector<std::uint8_t>* sps = nullptr;
    const std::vector<std::uint8_t>* pps = nullptr;
    for (const auto& unit : units) {
        if (bitstream::hevcNalType(unit) == bitstream::hevcNalSps) sps = &unit;
        if (bitstream::hevcNalType(unit) == bitstream::hevcNalPps) pps = &unit;
    }
    if (!sps) return 0;
    const std::vector<std::uint8_t> none;
    const auto level = chooseHevcLevel(*sps, pps ? *pps : none, policy);
    if (level)
        for (auto& unit : units) (void)bitstream::setHevcLevel(unit, *level);
    for (const auto& unit : units)
        if (bitstream::hevcNalType(unit) == bitstream::hevcNalSps) return bitstream::hevcLevel(unit).value_or(0);
    return 0;
}

} // namespace uml::platform::levels
