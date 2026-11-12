// SPDX-License-Identifier: GPL-2.0-or-later
#include "encode_map.hpp"

#include <linux/videodev2.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace uml::va {
namespace {

struct CodecControls {
    std::uint32_t profile;
    std::uint32_t level;
    std::uint32_t minQp;
    std::uint32_t maxQp;
    std::uint32_t iQp;
    std::uint32_t pQp;
    std::uint32_t bQp;
};

constexpr CodecControls kH264Controls {
    V4L2_CID_MPEG_VIDEO_H264_PROFILE,    V4L2_CID_MPEG_VIDEO_H264_LEVEL,
    V4L2_CID_MPEG_VIDEO_H264_MIN_QP,     V4L2_CID_MPEG_VIDEO_H264_MAX_QP,
    V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP, V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP,
    V4L2_CID_MPEG_VIDEO_H264_B_FRAME_QP,
};

constexpr CodecControls kHevcControls {
    V4L2_CID_MPEG_VIDEO_HEVC_PROFILE,    V4L2_CID_MPEG_VIDEO_HEVC_LEVEL,
    V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP,     V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP,
    V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP, V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP,
    V4L2_CID_MPEG_VIDEO_HEVC_B_FRAME_QP,
};

const CodecControls& controlsFor(std::uint32_t codec) noexcept
{
    return codec == V4L2_PIX_FMT_HEVC ? kHevcControls : kH264Controls;
}

// The profiles a V4L2 profile menu can stand for, per VA profile.
struct ProfileMap {
    VAProfile profile;
    std::uint32_t codec;
    std::uint32_t control;
    std::vector<unsigned> menu;
};

std::vector<ProfileMap> profileMaps()
{
    return {
        {VAProfileH264ConstrainedBaseline, V4L2_PIX_FMT_H264, V4L2_CID_MPEG_VIDEO_H264_PROFILE,
         {V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE, V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE}},
        {VAProfileH264Main, V4L2_PIX_FMT_H264, V4L2_CID_MPEG_VIDEO_H264_PROFILE,
         {V4L2_MPEG_VIDEO_H264_PROFILE_MAIN}},
        {VAProfileH264High, V4L2_PIX_FMT_H264, V4L2_CID_MPEG_VIDEO_H264_PROFILE,
         {V4L2_MPEG_VIDEO_H264_PROFILE_HIGH}},
        {VAProfileHEVCMain, V4L2_PIX_FMT_HEVC, V4L2_CID_MPEG_VIDEO_HEVC_PROFILE,
         {V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN}},
    };
}

// level_idc and the V4L2 menu item that stands for it.
struct LevelItem {
    unsigned levelIdc;
    unsigned item;
};

constexpr LevelItem kH264Levels[] = {
    {10, V4L2_MPEG_VIDEO_H264_LEVEL_1_0}, {11, V4L2_MPEG_VIDEO_H264_LEVEL_1_1},
    {12, V4L2_MPEG_VIDEO_H264_LEVEL_1_2}, {13, V4L2_MPEG_VIDEO_H264_LEVEL_1_3},
    {20, V4L2_MPEG_VIDEO_H264_LEVEL_2_0}, {21, V4L2_MPEG_VIDEO_H264_LEVEL_2_1},
    {22, V4L2_MPEG_VIDEO_H264_LEVEL_2_2}, {30, V4L2_MPEG_VIDEO_H264_LEVEL_3_0},
    {31, V4L2_MPEG_VIDEO_H264_LEVEL_3_1}, {32, V4L2_MPEG_VIDEO_H264_LEVEL_3_2},
    {40, V4L2_MPEG_VIDEO_H264_LEVEL_4_0}, {41, V4L2_MPEG_VIDEO_H264_LEVEL_4_1},
    {42, V4L2_MPEG_VIDEO_H264_LEVEL_4_2}, {50, V4L2_MPEG_VIDEO_H264_LEVEL_5_0},
    {51, V4L2_MPEG_VIDEO_H264_LEVEL_5_1}, {52, V4L2_MPEG_VIDEO_H264_LEVEL_5_2},
    {60, V4L2_MPEG_VIDEO_H264_LEVEL_6_0}, {61, V4L2_MPEG_VIDEO_H264_LEVEL_6_1},
    {62, V4L2_MPEG_VIDEO_H264_LEVEL_6_2},
};

constexpr LevelItem kHevcLevels[] = {
    {30, V4L2_MPEG_VIDEO_HEVC_LEVEL_1},   {60, V4L2_MPEG_VIDEO_HEVC_LEVEL_2},
    {63, V4L2_MPEG_VIDEO_HEVC_LEVEL_2_1}, {90, V4L2_MPEG_VIDEO_HEVC_LEVEL_3},
    {93, V4L2_MPEG_VIDEO_HEVC_LEVEL_3_1}, {120, V4L2_MPEG_VIDEO_HEVC_LEVEL_4},
    {123, V4L2_MPEG_VIDEO_HEVC_LEVEL_4_1}, {150, V4L2_MPEG_VIDEO_HEVC_LEVEL_5},
    {153, V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1}, {156, V4L2_MPEG_VIDEO_HEVC_LEVEL_5_2},
    {180, V4L2_MPEG_VIDEO_HEVC_LEVEL_6},   {183, V4L2_MPEG_VIDEO_HEVC_LEVEL_6_1},
    {186, V4L2_MPEG_VIDEO_HEVC_LEVEL_6_2},
};

bool enough(std::size_t size, std::size_t need) noexcept { return size >= need; }

unsigned clampUnsigned(std::int64_t value, unsigned low, unsigned high) noexcept
{
    return unsigned(std::clamp<std::int64_t>(value, low, high));
}

}

const EncodeProfile* EncodeSupport::profile(VAProfile wanted) const noexcept
{
    for (const auto& entry : profiles)
        if (entry.profile == wanted) return &entry;
    return nullptr;
}

const EncodeCodec* EncodeSupport::limits(VAProfile wanted) const noexcept
{
    const EncodeProfile* entry = profile(wanted);
    return entry && capabilities ? capabilities->codec(entry->codec) : nullptr;
}

EncodeSupport deriveEncodeSupport(std::shared_ptr<const EncoderCapabilities> capabilities)
{
    EncodeSupport support;
    if (!capabilities) return support;
    support.capabilities = capabilities;
    const EncoderCapabilities& caps = *capabilities;
    // The driver hands the node NV12, so a node that takes neither layout of
    // it cannot be served at all.
    if (!caps.offersRaw(V4L2_PIX_FMT_NV12M) && !caps.offersRaw(V4L2_PIX_FMT_NV12)) return support;

    for (const auto& map : profileMaps()) {
        if (!caps.codec(map.codec)) continue;
        EncodeProfile entry;
        entry.profile = map.profile;
        entry.codec = map.codec;
        for (const unsigned item : map.menu)
            if (caps.offersItem(map.control, item)) entry.menu.push_back(item);
        if (!entry.menu.empty()) support.profiles.push_back(std::move(entry));
    }

    const ControlInfo* mode = caps.control(V4L2_CID_MPEG_VIDEO_BITRATE_MODE);
    if (caps.offers(V4L2_CID_MPEG_VIDEO_BITRATE)) {
        if (!mode) {
            support.rateControl |= VA_RC_CBR | VA_RC_VBR;
        } else {
            if (caps.offersItem(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_CBR))
                support.rateControl |= VA_RC_CBR;
            if (caps.offersItem(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_VBR))
                support.rateControl |= VA_RC_VBR;
        }
    }
    if (caps.offersItem(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_CQ) &&
        caps.offers(V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY))
        support.rateControl |= VA_RC_ICQ;
    // Constant quantiser is rate control switched off, with the quantiser
    // named by the per-picture-type controls.
    if (caps.offers(V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE) &&
        (caps.offers(V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP) || caps.offers(V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP)))
        support.rateControl |= VA_RC_CQP;
    return support;
}

bool readH264Sequence(const void* data, std::size_t size, SequenceInfo& sequence)
{
    if (!enough(size, sizeof(VAEncSequenceParameterBufferH264))) return false;
    VAEncSequenceParameterBufferH264 parameters;
    std::memcpy(&parameters, data, sizeof(parameters));
    SequenceInfo read;
    read.codedWidth = unsigned(parameters.picture_width_in_mbs) * 16;
    read.codedHeight = unsigned(parameters.picture_height_in_mbs) * 16;
    read.width = read.codedWidth;
    read.height = read.codedHeight;
    if (parameters.frame_cropping_flag) {
        // Crop offsets count in units of chroma samples: two luma samples
        // across, and two rows down for a frame (four for a field pair).
        const bool colour = parameters.seq_fields.bits.chroma_format_idc != 0;
        const unsigned unitX = colour ? 2 : 1;
        const unsigned unitY = (colour ? 2 : 1) * (parameters.seq_fields.bits.frame_mbs_only_flag ? 1 : 2);
        const std::uint64_t cropX = std::uint64_t(parameters.frame_crop_left_offset) +
                                    parameters.frame_crop_right_offset;
        const std::uint64_t cropY = std::uint64_t(parameters.frame_crop_top_offset) +
                                    parameters.frame_crop_bottom_offset;
        if (cropX * unitX >= read.codedWidth || cropY * unitY >= read.codedHeight) return false;
        read.width = read.codedWidth - unsigned(cropX * unitX);
        read.height = read.codedHeight - unsigned(cropY * unitY);
        if (parameters.frame_crop_left_offset || parameters.frame_crop_top_offset) {
            // The encoder takes the top-left of the surface; a crop that
            // starts anywhere else is not something it can express.
            return false;
        }
    }
    read.intraIdrPeriod = parameters.intra_idr_period;
    read.ipPeriod = parameters.ip_period;
    read.level = parameters.level_idc;
    read.bitsPerSecond = parameters.bits_per_second;
    if (parameters.vui_parameters_present_flag && parameters.vui_fields.bits.timing_info_present_flag &&
        parameters.num_units_in_tick && parameters.time_scale) {
        // A frame is two fields, and the tick counts fields.
        read.timingNumerator = parameters.time_scale;
        read.timingDenominator = parameters.num_units_in_tick * 2;
    }
    read.valid = read.width && read.height;
    sequence = read;
    return true;
}

bool readH264Picture(const void* data, std::size_t size, PictureInfo& picture)
{
    if (!enough(size, sizeof(VAEncPictureParameterBufferH264))) return false;
    VAEncPictureParameterBufferH264 parameters;
    std::memcpy(&parameters, data, sizeof(parameters));
    picture.valid = true;
    picture.idr = parameters.pic_fields.bits.idr_pic_flag != 0;
    picture.cabac = parameters.pic_fields.bits.entropy_coding_mode_flag != 0;
    picture.transform8x8 = parameters.pic_fields.bits.transform_8x8_mode_flag != 0;
    picture.initQp = parameters.pic_init_qp;
    picture.codedBuffer = parameters.coded_buf;
    return true;
}

bool readH264Slice(const void* data, std::size_t size, SliceInfo& slice)
{
    if (!enough(size, sizeof(VAEncSliceParameterBufferH264))) return false;
    VAEncSliceParameterBufferH264 parameters;
    std::memcpy(&parameters, data, sizeof(parameters));
    slice.valid = true;
    slice.qpDelta = parameters.slice_qp_delta;
    return true;
}

bool readHevcSequence(const void* data, std::size_t size, SequenceInfo& sequence)
{
    if (!enough(size, sizeof(VAEncSequenceParameterBufferHEVC))) return false;
    VAEncSequenceParameterBufferHEVC parameters;
    std::memcpy(&parameters, data, sizeof(parameters));
    const auto& fields = parameters.seq_fields.bits;
    // Main is 8-bit 4:2:0; a client asking for more has the wrong profile.
    if (fields.chroma_format_idc != 1 || fields.bit_depth_luma_minus8 || fields.bit_depth_chroma_minus8) return false;
    SequenceInfo read;
    read.codedWidth = read.width = parameters.pic_width_in_luma_samples;
    read.codedHeight = read.height = parameters.pic_height_in_luma_samples;
    // The key frame interval is the shorter of the two a client names, because
    // the encoder can only start a stream afresh.
    read.intraIdrPeriod = parameters.intra_idr_period;
    if (parameters.intra_period && (!read.intraIdrPeriod || parameters.intra_period < read.intraIdrPeriod))
        read.intraIdrPeriod = parameters.intra_period;
    read.ipPeriod = parameters.ip_period;
    read.level = parameters.general_level_idc;
    read.highTier = parameters.general_tier_flag != 0;
    read.bitsPerSecond = parameters.bits_per_second;
    if (parameters.vui_parameters_present_flag && parameters.vui_fields.bits.vui_timing_info_present_flag &&
        parameters.vui_num_units_in_tick && parameters.vui_time_scale) {
        // HEVC counts frames in the tick.
        read.timingNumerator = parameters.vui_time_scale;
        read.timingDenominator = parameters.vui_num_units_in_tick;
    }
    read.valid = read.width && read.height;
    sequence = read;
    return true;
}

bool readHevcPicture(const void* data, std::size_t size, PictureInfo& picture)
{
    if (!enough(size, sizeof(VAEncPictureParameterBufferHEVC))) return false;
    VAEncPictureParameterBufferHEVC parameters;
    std::memcpy(&parameters, data, sizeof(parameters));
    picture.valid = true;
    picture.idr = parameters.pic_fields.bits.idr_pic_flag != 0;
    // HEVC has one entropy coder and no 8x8 transform to choose.
    picture.cabac = true;
    picture.initQp = parameters.pic_init_qp;
    picture.codedBuffer = parameters.coded_buf;
    return true;
}

bool readHevcSlice(const void* data, std::size_t size, SliceInfo& slice)
{
    if (!enough(size, sizeof(VAEncSliceParameterBufferHEVC))) return false;
    VAEncSliceParameterBufferHEVC parameters;
    std::memcpy(&parameters, data, sizeof(parameters));
    slice.valid = true;
    slice.qpDelta = parameters.slice_qp_delta;
    return true;
}

bool readMisc(const void* data, std::size_t size, EncodeState& state)
{
    if (size < sizeof(VAEncMiscParameterBuffer)) return false;
    VAEncMiscParameterType type;
    std::memcpy(&type, data, sizeof(type));
    const auto* payload = static_cast<const std::uint8_t*>(data) + sizeof(VAEncMiscParameterBuffer);
    const std::size_t room = size - sizeof(VAEncMiscParameterBuffer);
    switch (type) {
    case VAEncMiscParameterTypeFrameRate: {
        if (room < sizeof(VAEncMiscParameterFrameRate)) return false;
        VAEncMiscParameterFrameRate rate;
        std::memcpy(&rate, payload, sizeof(rate));
        const unsigned numerator = rate.framerate & 0xffffu;
        const unsigned denominator = (rate.framerate >> 16) & 0xffffu;
        if (numerator) state.frameRate = {true, numerator, denominator ? denominator : 1u};
        return true;
    }
    case VAEncMiscParameterTypeRateControl: {
        if (room < sizeof(VAEncMiscParameterRateControl)) return false;
        VAEncMiscParameterRateControl rate;
        std::memcpy(&rate, payload, sizeof(rate));
        RateInfo read;
        read.valid = true;
        read.bitsPerSecond = rate.bits_per_second;
        read.targetPercentage = rate.target_percentage ? std::min(rate.target_percentage, 100u) : 100u;
        read.windowMs = rate.window_size;
        read.minQp = rate.min_qp;
        read.maxQp = rate.max_qp;
        read.qualityFactor = rate.ICQ_quality_factor ? rate.ICQ_quality_factor : rate.quality_factor;
        state.rate = read;
        return true;
    }
    case VAEncMiscParameterTypeHRD: {
        if (room < sizeof(VAEncMiscParameterHRD)) return false;
        VAEncMiscParameterHRD hrd;
        std::memcpy(&hrd, payload, sizeof(hrd));
        state.hrd = {true, hrd.buffer_size};
        return true;
    }
    default:
        return true;
    }
}

FrameRateInfo effectiveFrameRate(const EncodeState& state) noexcept
{
    if (state.frameRate.valid) return state.frameRate;
    if (state.sequence.timingNumerator && state.sequence.timingDenominator)
        return {true, state.sequence.timingNumerator, state.sequence.timingDenominator};
    return {true, 30, 1};
}

Rates ratesFor(unsigned rateControl, const EncodeState& state) noexcept
{
    const std::uint32_t configured = state.rate.valid && state.rate.bitsPerSecond ? state.rate.bitsPerSecond
                                                                                  : state.sequence.bitsPerSecond;
    Rates rates;
    if (rateControl & VA_RC_VBR) {
        const unsigned percentage = state.rate.valid ? std::clamp(state.rate.targetPercentage, 1u, 100u) : 100u;
        rates.peak = configured;
        rates.average = std::uint32_t(std::uint64_t(configured) * percentage / 100);
    } else {
        rates.average = rates.peak = configured;
    }
    return rates;
}

bool encodeLevelItem(const EncoderCapabilities& capabilities, std::uint32_t codec, unsigned levelIdc,
                     unsigned& item) noexcept
{
    const LevelItem* best = nullptr;
    const LevelItem* highest = nullptr;
    const bool hevc = codec == V4L2_PIX_FMT_HEVC;
    const std::uint32_t control = hevc ? V4L2_CID_MPEG_VIDEO_HEVC_LEVEL : V4L2_CID_MPEG_VIDEO_H264_LEVEL;
    const auto consider = [&](const LevelItem& entry) {
        if (!capabilities.offersItem(control, entry.item)) return;
        highest = &entry;
        if (!best && entry.levelIdc >= levelIdc) best = &entry;
    };
    if (hevc)
        for (const auto& entry : kHevcLevels) consider(entry);
    else
        for (const auto& entry : kH264Levels) consider(entry);
    // A level above everything the node offers is the highest it offers.
    const LevelItem* chosen = best ? best : highest;
    if (!chosen) return false;
    item = chosen->item;
    return true;
}

namespace {

// The quantiser a constant-quantiser picture asks for.
unsigned constantQp(const EncodeState& state) noexcept
{
    return clampUnsigned(std::int64_t(state.picture.initQp) + state.slice.qpDelta, 0, 51);
}

// libva's ICQ factor counts like a quantiser (1 is best); V4L2's constant
// quality counts the other way (100 is best). The map is linear and puts the
// quantiser 26 that encoders treat as neutral at the middle of the range.
unsigned constantQuality(unsigned factor) noexcept
{
    factor = std::clamp(factor ? factor : 26u, 1u, 51u);
    return clampUnsigned(std::lround(100.0 - (double(factor) - 1.0) * 99.0 / 50.0), 1, 100);
}

unsigned kilobytes(std::uint32_t bits) noexcept
{
    return unsigned((std::uint64_t(bits) + 8191) / 8192);
}

}

std::vector<ControlValue> startupControls(const EncodeSupport& support, VAProfile profile, unsigned rateControl,
                                          const EncodeState& state)
{
    std::vector<ControlValue> out;
    if (!support.capabilities) return out;
    const EncoderCapabilities& caps = *support.capabilities;
    const EncodeProfile* entry = support.profile(profile);
    if (!entry) return out;
    const CodecControls& codec = controlsFor(entry->codec);
    const bool h264 = entry->codec == V4L2_PIX_FMT_H264;
    const auto add = [&](std::uint32_t id, std::int64_t value) {
        if (caps.offers(id)) out.push_back({id, value});
    };

    // A key frame has to be decodable on its own, so the parameter sets go in
    // front of every one of them, and arrive in the same buffer.
    add(V4L2_CID_MPEG_VIDEO_HEADER_MODE, V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME);
    add(V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 1);
    // The client asked for a stream with no B-frames, and VideoToolbox would
    // otherwise decide for itself.
    add(V4L2_CID_MPEG_VIDEO_B_FRAMES, 0);
    if (!entry->menu.empty()) add(codec.profile, entry->menu.front());
    if (state.sequence.level) {
        unsigned item = 0;
        if (encodeLevelItem(caps, entry->codec, state.sequence.level, item)) add(codec.level, item);
    }
    if (!h264 && state.sequence.valid)
        add(V4L2_CID_MPEG_VIDEO_HEVC_TIER, state.sequence.highTier ? V4L2_MPEG_VIDEO_HEVC_TIER_HIGH
                                                                   : V4L2_MPEG_VIDEO_HEVC_TIER_MAIN);
    if (h264 && state.picture.valid) {
        add(V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE, state.picture.cabac ? V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC
                                                                        : V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CAVLC);
        add(V4L2_CID_MPEG_VIDEO_H264_8X8_TRANSFORM, state.picture.transform8x8 ? 1 : 0);
    }
    add(V4L2_CID_MPEG_VIDEO_GOP_SIZE, state.sequence.intraIdrPeriod);

    if (rateControl & VA_RC_CQP) {
        // Constant quantiser is rate control switched off.
        add(V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 0);
        const unsigned qp = constantQp(state);
        add(codec.iQp, qp);
        add(codec.pQp, qp);
        add(codec.bQp, qp);
    } else {
        add(V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 1);
        if (rateControl & VA_RC_ICQ) {
            add(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_CQ);
            add(V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY, constantQuality(state.rate.qualityFactor));
        } else {
            const Rates rates = ratesFor(rateControl, state);
            if (caps.control(V4L2_CID_MPEG_VIDEO_BITRATE_MODE))
                add(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, (rateControl & VA_RC_VBR) ? V4L2_MPEG_VIDEO_BITRATE_MODE_VBR
                                                                                : V4L2_MPEG_VIDEO_BITRATE_MODE_CBR);
            if (rates.average) add(V4L2_CID_MPEG_VIDEO_BITRATE, rates.average);
            if ((rateControl & VA_RC_VBR) && rates.peak) add(V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, rates.peak);
        }
        if (state.rate.minQp) add(codec.minQp, std::min(state.rate.minQp, 51u));
        if (state.rate.maxQp) add(codec.maxQp, std::min(state.rate.maxQp, 51u));
        if (state.hrd.valid && state.hrd.bufferBits)
            add(h264 && caps.offers(V4L2_CID_MPEG_VIDEO_H264_CPB_SIZE) ? V4L2_CID_MPEG_VIDEO_H264_CPB_SIZE
                                                                       : V4L2_CID_MPEG_VIDEO_VBV_SIZE,
                kilobytes(state.hrd.bufferBits));
    }
    return out;
}

std::vector<ControlValue> dynamicControls(const EncodeSupport& support, VAProfile profile, unsigned rateControl,
                                          const EncodeState& before, const EncodeState& after)
{
    std::vector<ControlValue> out;
    if (!support.capabilities) return out;
    const EncoderCapabilities& caps = *support.capabilities;
    const EncodeProfile* entry = support.profile(profile);
    if (!entry) return out;
    const CodecControls& codec = controlsFor(entry->codec);
    const auto add = [&](std::uint32_t id, std::int64_t value) {
        if (caps.offers(id)) out.push_back({id, value});
    };
    if (rateControl & VA_RC_CQP) {
        if (constantQp(before) != constantQp(after)) {
            const unsigned qp = constantQp(after);
            add(codec.iQp, qp);
            add(codec.pQp, qp);
            add(codec.bQp, qp);
        }
        return out;
    }
    if (rateControl & VA_RC_ICQ) {
        if (before.rate.qualityFactor != after.rate.qualityFactor)
            add(V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY, constantQuality(after.rate.qualityFactor));
        return out;
    }
    const Rates then = ratesFor(rateControl, before);
    const Rates now = ratesFor(rateControl, after);
    if (now.average && now.average != then.average) add(V4L2_CID_MPEG_VIDEO_BITRATE, now.average);
    if ((rateControl & VA_RC_VBR) && now.peak && now.peak != then.peak)
        add(V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, now.peak);
    if (after.rate.maxQp && after.rate.maxQp != before.rate.maxQp) add(codec.maxQp, std::min(after.rate.maxQp, 51u));
    return out;
}

}
