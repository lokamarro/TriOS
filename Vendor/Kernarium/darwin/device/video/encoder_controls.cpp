// SPDX-License-Identifier: GPL-2.0-or-later
#include "encoder_controls.hpp"

#include "encoder_capabilities.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace uml::video {
namespace {
using uml::platform::RawVideoLayout;
using uml::platform::VideoCodec;
using uml::platform::VideoColorPrimaries;
using uml::platform::VideoEncodeCapability;
using uml::platform::VideoEncodeProfile;
using uml::platform::VideoEncodeSettings;
using uml::platform::VideoEncodeUpdate;
using uml::platform::VideoMatrixCoefficients;
using uml::platform::VideoRateControl;
using uml::platform::VideoTransferFunction;

constexpr std::int64_t maximumInt = 0x7fffffff;
constexpr std::int64_t defaultBitrate = 2'000'000;
/* The lowest bit rate the bit rate control takes, in bits per second. The host's encoders accept
 * less, but an HEVC stream with inter frames of a 64x64 picture misses Android's CTS quality bar
 * (a PSNR of 20 dB) below about 10 kbit/s, so the floor is twice that. */
constexpr std::int64_t minimumBitrate = 20'000;
constexpr std::int64_t defaultFrameQp = 26;
constexpr std::int64_t defaultJpegQuality = 85;
/* Roles in the bit set of bitrate modes. */
constexpr unsigned modeBit(std::uint32_t menuIndex) { return 1u << menuIndex; }

struct ProfileEntry {
    VideoEncodeProfile profile;
    std::uint32_t index;
};
constexpr std::array h264Profiles {
    ProfileEntry {VideoEncodeProfile::h264Baseline, UML_V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE},
    ProfileEntry {VideoEncodeProfile::h264ConstrainedBaseline, UML_V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE},
    ProfileEntry {VideoEncodeProfile::h264Main, UML_V4L2_MPEG_VIDEO_H264_PROFILE_MAIN},
    ProfileEntry {VideoEncodeProfile::h264High, UML_V4L2_MPEG_VIDEO_H264_PROFILE_HIGH},
    ProfileEntry {VideoEncodeProfile::h264High422, UML_V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_422},
    ProfileEntry {VideoEncodeProfile::h264High444Predictive, UML_V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_444_PREDICTIVE},
    ProfileEntry {VideoEncodeProfile::h264ConstrainedHigh, UML_V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_HIGH},
};
constexpr std::array hevcProfiles {
    ProfileEntry {VideoEncodeProfile::hevcMain, UML_V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN},
    ProfileEntry {VideoEncodeProfile::hevcMainStill, UML_V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_STILL_PICTURE},
    ProfileEntry {VideoEncodeProfile::hevcMain10, UML_V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10},
};
/* The profile a stream that never chose one gets: the most capable of the
 * ordinary ones the host encodes. */
constexpr std::array h264Preference {
    VideoEncodeProfile::h264High, VideoEncodeProfile::h264Main, VideoEncodeProfile::h264ConstrainedHigh,
    VideoEncodeProfile::h264ConstrainedBaseline, VideoEncodeProfile::h264Baseline,
    VideoEncodeProfile::h264High422, VideoEncodeProfile::h264High444Predictive};
constexpr std::array hevcPreference {
    VideoEncodeProfile::hevcMain, VideoEncodeProfile::hevcMain10, VideoEncodeProfile::hevcMainStill};

struct LevelEntry {
    std::uint32_t idc;
    std::uint32_t index;
};
constexpr std::array h264Levels {
    LevelEntry {10, UML_V4L2_MPEG_VIDEO_H264_LEVEL_1_0}, LevelEntry {9, UML_V4L2_MPEG_VIDEO_H264_LEVEL_1B},
    LevelEntry {11, UML_V4L2_MPEG_VIDEO_H264_LEVEL_1_1}, LevelEntry {12, UML_V4L2_MPEG_VIDEO_H264_LEVEL_1_2},
    LevelEntry {13, UML_V4L2_MPEG_VIDEO_H264_LEVEL_1_3}, LevelEntry {20, UML_V4L2_MPEG_VIDEO_H264_LEVEL_2_0},
    LevelEntry {21, UML_V4L2_MPEG_VIDEO_H264_LEVEL_2_1}, LevelEntry {22, UML_V4L2_MPEG_VIDEO_H264_LEVEL_2_2},
    LevelEntry {30, UML_V4L2_MPEG_VIDEO_H264_LEVEL_3_0}, LevelEntry {31, UML_V4L2_MPEG_VIDEO_H264_LEVEL_3_1},
    LevelEntry {32, UML_V4L2_MPEG_VIDEO_H264_LEVEL_3_2}, LevelEntry {40, UML_V4L2_MPEG_VIDEO_H264_LEVEL_4_0},
    LevelEntry {41, UML_V4L2_MPEG_VIDEO_H264_LEVEL_4_1}, LevelEntry {42, UML_V4L2_MPEG_VIDEO_H264_LEVEL_4_2},
    LevelEntry {50, UML_V4L2_MPEG_VIDEO_H264_LEVEL_5_0}, LevelEntry {51, UML_V4L2_MPEG_VIDEO_H264_LEVEL_5_1},
    LevelEntry {52, UML_V4L2_MPEG_VIDEO_H264_LEVEL_5_2}, LevelEntry {60, UML_V4L2_MPEG_VIDEO_H264_LEVEL_6_0},
    LevelEntry {61, UML_V4L2_MPEG_VIDEO_H264_LEVEL_6_1}, LevelEntry {62, UML_V4L2_MPEG_VIDEO_H264_LEVEL_6_2},
};
constexpr std::array hevcLevels {
    LevelEntry {30, UML_V4L2_MPEG_VIDEO_HEVC_LEVEL_1}, LevelEntry {60, UML_V4L2_MPEG_VIDEO_HEVC_LEVEL_2},
    LevelEntry {63, UML_V4L2_MPEG_VIDEO_HEVC_LEVEL_2_1}, LevelEntry {90, UML_V4L2_MPEG_VIDEO_HEVC_LEVEL_3},
    LevelEntry {93, UML_V4L2_MPEG_VIDEO_HEVC_LEVEL_3_1}, LevelEntry {120, UML_V4L2_MPEG_VIDEO_HEVC_LEVEL_4},
    LevelEntry {123, UML_V4L2_MPEG_VIDEO_HEVC_LEVEL_4_1}, LevelEntry {150, UML_V4L2_MPEG_VIDEO_HEVC_LEVEL_5},
    LevelEntry {153, UML_V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1}, LevelEntry {156, UML_V4L2_MPEG_VIDEO_HEVC_LEVEL_5_2},
    LevelEntry {180, UML_V4L2_MPEG_VIDEO_HEVC_LEVEL_6}, LevelEntry {183, UML_V4L2_MPEG_VIDEO_HEVC_LEVEL_6_1},
    LevelEntry {186, UML_V4L2_MPEG_VIDEO_HEVC_LEVEL_6_2},
};

std::uint64_t codedBit(std::uint32_t format) { return std::uint64_t(1) << (format - VIRTIO_VIDEO_FORMAT_CODED_MIN); }

const VideoEncodeCapability* capabilityOf(std::span<const VideoEncodeCapability> capabilities, VideoCodec codec)
{
    const auto found = std::find_if(capabilities.begin(), capabilities.end(),
                                    [codec](const auto& capability) { return capability.codec == codec; });
    return found == capabilities.end() ? nullptr : &*found;
}

bool hasProfile(const VideoEncodeCapability& capability, VideoEncodeProfile profile)
{
    return std::any_of(capability.profiles.begin(), capability.profiles.end(),
                       [profile](const auto& entry) { return entry.profile == profile; });
}

bool has(const VideoEncodeCapability& capability, VideoRateControl mode)
{
    return std::find(capability.rateControls.begin(), capability.rateControls.end(), mode) !=
           capability.rateControls.end();
}

std::uint32_t highestLevel(const VideoEncodeCapability& capability)
{
    std::uint32_t highest = 0;
    for (const auto& profile : capability.profiles) highest = std::max(highest, profile.maximumLevel);
    return highest;
}

/* The menu index of the highest level a host that can be asked for `idc` has;
 * the table's last entry when it does not say. */
template<std::size_t N>
std::uint32_t highestLevelIndex(const std::array<LevelEntry, N>& levels, std::uint32_t idc)
{
    std::optional<LevelEntry> best;
    for (const auto& level : levels)
        if (level.idc <= idc && (!best || level.idc > best->idc)) best = level;
    return best ? best->index : levels.back().index;
}

class Builder final {
public:
    explicit Builder(std::vector<virtio_video_v4l2_control_desc>& descriptors) : descriptors_(descriptors) {}

    void integer(std::uint32_t id, std::int64_t minimum, std::int64_t maximum, std::int64_t defaultValue,
                 std::uint64_t codedFormats, std::uint32_t flags = 0, std::int64_t step = 1)
    {
        add(id, UML_V4L2_CTRL_TYPE_INTEGER, minimum, maximum, step, std::clamp(defaultValue, minimum, maximum),
            0, flags, 0, codedFormats);
    }
    void boolean(std::uint32_t id, bool defaultValue, std::uint64_t codedFormats, std::uint32_t flags = 0)
    {
        add(id, UML_V4L2_CTRL_TYPE_BOOLEAN, 0, 1, 1, defaultValue ? 1 : 0, 0, flags, 0, codedFormats);
    }
    void button(std::uint32_t id, std::uint64_t codedFormats, std::uint32_t flags)
    {
        add(id, UML_V4L2_CTRL_TYPE_BUTTON, 0, 0, 0, 0, 0, flags, 0, codedFormats);
    }
    /* A menu of the indices in `published`; every other index up to the
     * highest one is skipped. A menu of one item still spans at least two
     * indices when `topIndex` says so, which keeps its range non-empty for a
     * driver that rejects a menu with nothing to choose. */
    void menu(std::uint32_t id, std::uint64_t published, std::uint32_t defaultIndex,
              std::uint64_t codedFormats, std::uint32_t flags = 0, std::uint32_t topIndex = 0)
    {
        if (published == 0) return;
        std::uint32_t highest = topIndex;
        for (std::uint32_t index = 0; index < 64; ++index)
            if ((published >> index) & 1) highest = std::max(highest, index);
        const std::uint64_t all = highest == 63 ? ~std::uint64_t(0) : (std::uint64_t(1) << (highest + 1)) - 1;
        add(id, UML_V4L2_CTRL_TYPE_MENU, 0, highest, 1, defaultIndex, all & ~published, flags, 0, codedFormats);
    }
    void compound(std::uint32_t id, std::uint32_t type, std::uint32_t bytes, std::uint64_t codedFormats)
    {
        add(id, type, 0, 0, 0, 0, 0, 0, bytes, codedFormats);
    }

private:
    void add(std::uint32_t id, std::uint32_t type, std::int64_t minimum, std::int64_t maximum, std::int64_t step,
             std::int64_t defaultValue, std::uint64_t skipMask, std::uint32_t flags, std::uint32_t elementSize,
             std::uint64_t codedFormats)
    {
        virtio_video_v4l2_control_desc desc {};
        desc.id = id;
        desc.type = type;
        desc.minimum = static_cast<std::uint64_t>(minimum);
        desc.maximum = static_cast<std::uint64_t>(maximum);
        desc.step = static_cast<std::uint64_t>(step);
        desc.default_value = static_cast<std::uint64_t>(defaultValue);
        desc.menu_skip_mask = skipMask;
        desc.flags = flags;
        desc.elem_size = elementSize;
        desc.coded_formats = codedFormats;
        descriptors_.push_back(desc);
    }
    std::vector<virtio_video_v4l2_control_desc>& descriptors_;
};

template<std::size_t N>
std::uint64_t publishedProfiles(const VideoEncodeCapability& capability, const std::array<ProfileEntry, N>& table)
{
    std::uint64_t published = 0;
    for (const auto& entry : table)
        if (hasProfile(capability, entry.profile)) published |= std::uint64_t(1) << entry.index;
    return published;
}

template<std::size_t N, std::size_t M>
std::uint32_t defaultProfileIndex(const VideoEncodeCapability& capability, const std::array<ProfileEntry, N>& table,
                                  const std::array<VideoEncodeProfile, M>& preference)
{
    for (const auto profile : preference)
        if (hasProfile(capability, profile))
            for (const auto& entry : table)
                if (entry.profile == profile) return entry.index;
    return table.front().index;
}

std::int64_t signedValue(std::uint64_t value) noexcept { return static_cast<std::int64_t>(value); }

std::int64_t scalar(const ControlValues& values, std::uint32_t id, std::int64_t fallback = 0)
{
    const auto found = values.scalars.find(id);
    return found == values.scalars.end() ? fallback : found->second;
}

template<class T> T readAs(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    T value {};
    if (offset + sizeof(T) <= bytes.size()) std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

VideoEncodeProfile profileFor(VideoCodec codec, std::int64_t index)
{
    if (codec == VideoCodec::jpeg) return VideoEncodeProfile::jpegBaseline;
    const auto fallback = codec == VideoCodec::h264 ? VideoEncodeProfile::h264High : VideoEncodeProfile::hevcMain;
    const auto match = [&](const auto& table) {
        for (const auto& entry : table)
            if (std::int64_t(entry.index) == index) return entry.profile;
        return fallback;
    };
    return codec == VideoCodec::h264 ? match(h264Profiles) : match(hevcProfiles);
}

/* The colour aspects a stream states. A V4L2 client states the standard in the
 * colorspace and may leave the other fields at their defaults, which the
 * colorspace then implies (V4L2_MAP_*_DEFAULT). The V4L2 core resets the other
 * fields to DEFAULT whenever the colorspace is DEFAULT, so a client with a
 * transfer function or a range but no standard sends RAW instead: RAW states
 * no primaries, and then every default means what it says, not what a standard
 * implies. */
uml::platform::VideoEncodeColor colorFor(const StreamInfo& info)
{
    using uml::platform::VideoEncodeColor;
    const bool raw = info.colorspace == UML_V4L2_COLORSPACE_RAW;
    VideoEncodeColor color;
    switch (info.colorspace) {
    case UML_V4L2_COLORSPACE_SMPTE170M: color.primaries = VideoColorPrimaries::bt601Ntsc; break;
    case UML_V4L2_COLORSPACE_470_SYSTEM_BG: color.primaries = VideoColorPrimaries::bt601Pal; break;
    case UML_V4L2_COLORSPACE_REC709: case UML_V4L2_COLORSPACE_SRGB: color.primaries = VideoColorPrimaries::bt709; break;
    case UML_V4L2_COLORSPACE_BT2020: color.primaries = VideoColorPrimaries::bt2020; break;
    case UML_V4L2_COLORSPACE_DCI_P3: color.primaries = VideoColorPrimaries::p3D65; break;
    default: break;  // DEFAULT and RAW: unspecified
    }
    auto transfer = info.transfer;
    if (transfer == UML_V4L2_XFER_FUNC_DEFAULT && !raw) {
        switch (info.colorspace) {
        case UML_V4L2_COLORSPACE_SRGB: transfer = UML_V4L2_XFER_FUNC_SRGB; break;
        case UML_V4L2_COLORSPACE_REC709: case UML_V4L2_COLORSPACE_SMPTE170M:
        case UML_V4L2_COLORSPACE_470_SYSTEM_BG: case UML_V4L2_COLORSPACE_BT2020:
            transfer = UML_V4L2_XFER_FUNC_709; break;
        default: break;
        }
    }
    switch (transfer) {
    case UML_V4L2_XFER_FUNC_709: color.transfer = VideoTransferFunction::bt709; break;
    case UML_V4L2_XFER_FUNC_SRGB: color.transfer = VideoTransferFunction::srgb; break;
    case UML_V4L2_XFER_FUNC_SMPTE2084: color.transfer = VideoTransferFunction::pq; break;
    case UML_V4L2_XFER_FUNC_HLG: color.transfer = VideoTransferFunction::hlg; break;
    case UML_V4L2_XFER_FUNC_NONE: color.transfer = VideoTransferFunction::linear; break;
    default: break;
    }
    auto matrix = info.matrix;
    if (matrix == UML_V4L2_YCBCR_ENC_DEFAULT && !raw) {
        switch (info.colorspace) {
        case UML_V4L2_COLORSPACE_REC709: case UML_V4L2_COLORSPACE_DCI_P3: matrix = UML_V4L2_YCBCR_ENC_709; break;
        case UML_V4L2_COLORSPACE_BT2020: matrix = UML_V4L2_YCBCR_ENC_BT2020; break;
        case UML_V4L2_COLORSPACE_SMPTE170M: case UML_V4L2_COLORSPACE_470_SYSTEM_BG:
        case UML_V4L2_COLORSPACE_SRGB: matrix = UML_V4L2_YCBCR_ENC_601; break;
        default: break;
        }
    }
    switch (matrix) {
    case UML_V4L2_YCBCR_ENC_601: color.matrix = VideoMatrixCoefficients::bt601; break;
    case UML_V4L2_YCBCR_ENC_709: color.matrix = VideoMatrixCoefficients::bt709; break;
    case UML_V4L2_YCBCR_ENC_BT2020: color.matrix = VideoMatrixCoefficients::bt2020; break;
    default: break;
    }
    color.fullRange = info.quantization == UML_V4L2_QUANTIZATION_FULL_RANGE ||
        (info.quantization == UML_V4L2_QUANTIZATION_DEFAULT && info.colorspace == UML_V4L2_COLORSPACE_SRGB);
    return color;
}
}

StreamInfo sanitized(const StreamInfo& info) noexcept
{
    const auto keep = [](std::uint32_t value, std::initializer_list<std::uint32_t> known) {
        return std::find(known.begin(), known.end(), value) != known.end() ? value : 0u;
    };
    StreamInfo result = info;
    result.colorspace = keep(info.colorspace,
        {UML_V4L2_COLORSPACE_SMPTE170M, UML_V4L2_COLORSPACE_REC709, UML_V4L2_COLORSPACE_470_SYSTEM_BG,
         UML_V4L2_COLORSPACE_SRGB, UML_V4L2_COLORSPACE_BT2020, UML_V4L2_COLORSPACE_RAW, UML_V4L2_COLORSPACE_DCI_P3});
    result.transfer = keep(info.transfer,
        {UML_V4L2_XFER_FUNC_709, UML_V4L2_XFER_FUNC_SRGB, UML_V4L2_XFER_FUNC_SMPTE2084, UML_V4L2_XFER_FUNC_HLG,
         UML_V4L2_XFER_FUNC_NONE});
    result.matrix = keep(info.matrix, {UML_V4L2_YCBCR_ENC_601, UML_V4L2_YCBCR_ENC_709, UML_V4L2_YCBCR_ENC_BT2020});
    result.quantization = keep(info.quantization, {UML_V4L2_QUANTIZATION_FULL_RANGE, UML_V4L2_QUANTIZATION_LIM_RANGE});
    return result;
}

EncoderControls::EncoderControls(std::span<const VideoEncodeCapability> capabilities)
{
    Builder add(descriptors_);
    const auto* h264 = capabilityOf(capabilities, VideoCodec::h264);
    const auto* hevc = capabilityOf(capabilities, VideoCodec::hevc);
    const auto* jpeg = capabilityOf(capabilities, VideoCodec::jpeg);
    std::vector<const VideoEncodeCapability*> video;
    if (h264) video.push_back(h264);
    if (hevc) video.push_back(hevc);
    const std::uint64_t h264Format = codedBit(VIRTIO_VIDEO_FORMAT_H264);
    const std::uint64_t hevcFormat = codedBit(VIRTIO_VIDEO_FORMAT_HEVC);
    const std::uint64_t videoFormats = (h264 ? h264Format : 0) | (hevc ? hevcFormat : 0);
    const auto everyCodec = [&video](auto predicate) {
        return std::all_of(video.begin(), video.end(), [&](const auto* capability) { return predicate(*capability); });
    };

    if (!video.empty()) {
        /* A control shared by H.264 and HEVC is published as far as both
         * codecs go, so any value it accepts is one either can encode. */
        unsigned modes = 7;
        for (const auto* capability : video) {
            unsigned codecModes = 0;
            if (has(*capability, VideoRateControl::variableBitrate)) codecModes |= modeBit(UML_V4L2_MPEG_VIDEO_BITRATE_MODE_VBR);
            if (has(*capability, VideoRateControl::constantBitrate)) codecModes |= modeBit(UML_V4L2_MPEG_VIDEO_BITRATE_MODE_CBR);
            if (has(*capability, VideoRateControl::constantQuality)) codecModes |= modeBit(UML_V4L2_MPEG_VIDEO_BITRATE_MODE_CQ);
            modes &= codecModes;
        }
        std::int64_t maximumBitrate = std::numeric_limits<std::int64_t>::max();
        for (const auto* capability : video)
            if (capability->maximumBitrate)
                maximumBitrate = std::min<std::int64_t>(maximumBitrate, std::int64_t(std::min<std::uint64_t>(
                    capability->maximumBitrate, std::uint64_t(maximumInt))));
        if (maximumBitrate == std::numeric_limits<std::int64_t>::max()) maximumBitrate = maximumInt;
        maximumBitrate = std::max<std::int64_t>(maximumBitrate, minimumBitrate);

        const std::uint32_t defaultMode = (modes & modeBit(UML_V4L2_MPEG_VIDEO_BITRATE_MODE_VBR))
            ? UML_V4L2_MPEG_VIDEO_BITRATE_MODE_VBR
            : (modes & modeBit(UML_V4L2_MPEG_VIDEO_BITRATE_MODE_CBR)) ? UML_V4L2_MPEG_VIDEO_BITRATE_MODE_CBR
                                                                      : UML_V4L2_MPEG_VIDEO_BITRATE_MODE_CQ;
        add.menu(UML_V4L2_CID_MPEG_VIDEO_BITRATE_MODE, modes, defaultMode, videoFormats);
        if (modes & (modeBit(UML_V4L2_MPEG_VIDEO_BITRATE_MODE_VBR) | modeBit(UML_V4L2_MPEG_VIDEO_BITRATE_MODE_CBR)))
            add.integer(UML_V4L2_CID_MPEG_VIDEO_BITRATE, minimumBitrate, maximumBitrate, defaultBitrate, videoFormats,
                        VIRTIO_VIDEO_V4L2_CTRL_F_DYNAMIC);
        if (modes & modeBit(UML_V4L2_MPEG_VIDEO_BITRATE_MODE_VBR))
            add.integer(UML_V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, 0, maximumBitrate, 0, videoFormats,
                        VIRTIO_VIDEO_V4L2_CTRL_F_DYNAMIC);
        if (modes & modeBit(UML_V4L2_MPEG_VIDEO_BITRATE_MODE_CQ))
            add.integer(UML_V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY, 1, 100, 50, videoFormats,
                        VIRTIO_VIDEO_V4L2_CTRL_F_DYNAMIC);
        if (everyCodec([](const auto& capability) { return has(capability, VideoRateControl::constantQp); }))
            add.boolean(UML_V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, true, videoFormats);
        add.boolean(UML_V4L2_CID_MPEG_VIDEO_MB_RC_ENABLE, true, videoFormats);
        add.integer(UML_V4L2_CID_MPEG_VIDEO_VBV_SIZE, 0, maximumInt, 0, videoFormats);
        add.integer(UML_V4L2_CID_MPEG_VIDEO_GOP_SIZE, 0, maximumInt, 0, videoFormats);
        /* The platform codes B-frames in one pattern of a fixed run, or none:
         * a request below the run is no B-frames (encodeSettings). The control
         * runs to the longest run and belongs to the codecs that have one. */
        std::uint32_t longestRun = 0;
        for (const auto* capability : video) longestRun = std::max(longestRun, capability->maximumBFrames);
        if (longestRun > 0)
            add.integer(UML_V4L2_CID_MPEG_VIDEO_B_FRAMES, 0, longestRun, 0,
                        (h264 && h264->maximumBFrames ? h264Format : 0) | (hevc && hevc->maximumBFrames ? hevcFormat : 0));
        add.button(UML_V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, videoFormats, VIRTIO_VIDEO_V4L2_CTRL_F_DYNAMIC);
        add.menu(UML_V4L2_CID_MPEG_VIDEO_HEADER_MODE,
                 (1u << UML_V4L2_MPEG_VIDEO_HEADER_MODE_SEPARATE) |
                     (1u << UML_V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME),
                 UML_V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME, videoFormats);
        add.boolean(UML_V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, true, videoFormats,
                    VIRTIO_VIDEO_V4L2_CTRL_F_DYNAMIC);
    }

    const auto qpRange = [](const VideoEncodeCapability& capability) {
        const std::int64_t maximum = capability.maximumQp ? capability.maximumQp : 51;
        return std::pair<std::int64_t, std::int64_t> {std::min<std::int64_t>(capability.minimumQp, maximum), maximum};
    };
    const auto frameQp = [&](const VideoEncodeCapability& capability, std::uint32_t i, std::uint32_t p,
                             std::uint32_t b, std::uint64_t format) {
        if (!has(capability, VideoRateControl::constantQp)) return;
        const auto [minimum, maximum] = qpRange(capability);
        add.integer(i, minimum, maximum, defaultFrameQp, format);
        add.integer(p, minimum, maximum, defaultFrameQp, format);
        add.integer(b, minimum, maximum, defaultFrameQp, format);
    };
    const auto layers = [&](const VideoEncodeCapability& capability, std::uint64_t format, bool boolean,
                            std::uint32_t enable, std::uint32_t type, std::uint32_t layer, std::uint32_t l0,
                            std::uint32_t l1) {
        if (capability.maximumTemporalLayers < 2) return;
        if (boolean) add.boolean(enable, false, format);
        add.menu(type, std::uint64_t(1) << UML_V4L2_MPEG_VIDEO_H264_HIERARCHICAL_CODING_P,
                 UML_V4L2_MPEG_VIDEO_H264_HIERARCHICAL_CODING_P, format);
        add.integer(layer, 1, capability.maximumTemporalLayers, 1, format);
        const std::int64_t bitrate = capability.maximumBitrate
            ? std::int64_t(std::min<std::uint64_t>(capability.maximumBitrate, std::uint64_t(maximumInt)))
            : maximumInt;
        add.integer(l0, 0, bitrate, 0, format);
        add.integer(l1, 0, bitrate, 0, format);
    };

    if (h264) {
        const auto [minimumQp, maximumQp] = qpRange(*h264);
        add.menu(UML_V4L2_CID_MPEG_VIDEO_H264_PROFILE, publishedProfiles(*h264, h264Profiles),
                 defaultProfileIndex(*h264, h264Profiles, h264Preference), h264Format);
        /* A stream states the level it asked for, or the lowest its picture,
         * frame rate and bit rate fit if that is higher, so the lowest level
         * is the default and means "as low as it fits". */
        const auto top = highestLevel(*h264) ? highestLevelIndex(h264Levels, highestLevel(*h264))
                                              : std::uint32_t(UML_V4L2_MPEG_VIDEO_H264_LEVEL_5_2);
        add.menu(UML_V4L2_CID_MPEG_VIDEO_H264_LEVEL, (std::uint64_t(1) << (top + 1)) - 1,
                 UML_V4L2_MPEG_VIDEO_H264_LEVEL_1_0, h264Format);
        add.integer(UML_V4L2_CID_MPEG_VIDEO_H264_MIN_QP, minimumQp, maximumQp, minimumQp, h264Format);
        add.integer(UML_V4L2_CID_MPEG_VIDEO_H264_MAX_QP, minimumQp, maximumQp, maximumQp, h264Format,
                    VIRTIO_VIDEO_V4L2_CTRL_F_DYNAMIC);
        frameQp(*h264, UML_V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP, UML_V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP,
                UML_V4L2_CID_MPEG_VIDEO_H264_B_FRAME_QP, h264Format);
        if (hasProfile(*h264, VideoEncodeProfile::h264High) || hasProfile(*h264, VideoEncodeProfile::h264ConstrainedHigh) ||
            hasProfile(*h264, VideoEncodeProfile::h264High422) ||
            hasProfile(*h264, VideoEncodeProfile::h264High444Predictive))
            add.boolean(UML_V4L2_CID_MPEG_VIDEO_H264_8X8_TRANSFORM, true, h264Format);
        if (h264->entropyModeSelectable)
            add.menu(UML_V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE,
                     (1u << UML_V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CAVLC) | (1u << UML_V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC),
                     UML_V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC, h264Format);
        add.integer(UML_V4L2_CID_MPEG_VIDEO_H264_CPB_SIZE, 0, maximumInt, 0, h264Format);
        layers(*h264, h264Format, true, UML_V4L2_CID_MPEG_VIDEO_H264_HIERARCHICAL_CODING,
               UML_V4L2_CID_MPEG_VIDEO_H264_HIERARCHICAL_CODING_TYPE, UML_V4L2_CID_MPEG_VIDEO_H264_HIERARCHICAL_CODING_LAYER,
               UML_V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L0_BR, UML_V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L1_BR);
    }
    if (hevc) {
        const auto [minimumQp, maximumQp] = qpRange(*hevc);
        add.menu(UML_V4L2_CID_MPEG_VIDEO_HEVC_PROFILE, publishedProfiles(*hevc, hevcProfiles),
                 defaultProfileIndex(*hevc, hevcProfiles, hevcPreference), hevcFormat);
        const auto top = highestLevel(*hevc) ? highestLevelIndex(hevcLevels, highestLevel(*hevc))
                                              : std::uint32_t(UML_V4L2_MPEG_VIDEO_HEVC_LEVEL_6_2);
        add.menu(UML_V4L2_CID_MPEG_VIDEO_HEVC_LEVEL, (std::uint64_t(1) << (top + 1)) - 1,
                 UML_V4L2_MPEG_VIDEO_HEVC_LEVEL_1, hevcFormat);
        add.menu(UML_V4L2_CID_MPEG_VIDEO_HEVC_TIER, std::uint64_t(1) << UML_V4L2_MPEG_VIDEO_HEVC_TIER_MAIN,
                 UML_V4L2_MPEG_VIDEO_HEVC_TIER_MAIN, hevcFormat, 0, UML_V4L2_MPEG_VIDEO_HEVC_TIER_HIGH);
        add.integer(UML_V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP, minimumQp, maximumQp, minimumQp, hevcFormat);
        add.integer(UML_V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP, minimumQp, maximumQp, maximumQp, hevcFormat,
                    VIRTIO_VIDEO_V4L2_CTRL_F_DYNAMIC);
        frameQp(*hevc, UML_V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP, UML_V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP,
                UML_V4L2_CID_MPEG_VIDEO_HEVC_B_FRAME_QP, hevcFormat);
        layers(*hevc, hevcFormat, false, 0, UML_V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_TYPE,
               UML_V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_LAYER, UML_V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_L0_BR,
               UML_V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_L1_BR);
        if (hevc->hdrStaticMetadata && hasProfile(*hevc, VideoEncodeProfile::hevcMain10)) {
            add.compound(UML_V4L2_CID_COLORIMETRY_HDR10_CLL_INFO, UML_V4L2_CTRL_TYPE_HDR10_CLL_INFO, 4, hevcFormat);
            add.compound(UML_V4L2_CID_COLORIMETRY_HDR10_MASTERING_DISPLAY,
                         UML_V4L2_CTRL_TYPE_HDR10_MASTERING_DISPLAY, 24, hevcFormat);
        }
    }
    if (jpeg)
        add.integer(UML_V4L2_CID_JPEG_COMPRESSION_QUALITY, 1, 100, defaultJpegQuality,
                    codedBit(VIRTIO_VIDEO_FORMAT_MJPEG), VIRTIO_VIDEO_V4L2_CTRL_F_DYNAMIC);
    if (!descriptors_.empty()) {
        /* The host copies each raw frame as it arrives, so the guest never has
         * to keep more than two in flight. */
        std::uint64_t everything = 0;
        for (const auto& capability : capabilities)
            if (const auto format = encodedWireFormat(capability.codec)) everything |= codedBit(*format);
        add.integer(UML_V4L2_CID_MIN_BUFFERS_FOR_OUTPUT, 2, 2, 2, everything, VIRTIO_VIDEO_V4L2_CTRL_F_READ_ONLY);
    }
}

const virtio_video_v4l2_control_desc* EncoderControls::find(std::uint32_t id) const noexcept
{
    const auto found = std::find_if(descriptors_.begin(), descriptors_.end(),
                                    [id](const auto& descriptor) { return descriptor.id == id; });
    return found == descriptors_.end() ? nullptr : &*found;
}

ControlValues EncoderControls::defaults() const
{
    ControlValues values;
    for (const auto& descriptor : descriptors_) {
        if (descriptor.elem_size != 0 || descriptor.type == UML_V4L2_CTRL_TYPE_BUTTON) continue;
        values.scalars[descriptor.id] = signedValue(descriptor.default_value);
    }
    return values;
}

ControlCheck EncoderControls::check(std::uint32_t id, std::int64_t value, std::size_t payloadSize) const noexcept
{
    const auto* descriptor = find(id);
    if (!descriptor) return ControlCheck::unsupported;
    const auto minimum = signedValue(descriptor->minimum);
    const auto maximum = signedValue(descriptor->maximum);
    const auto step = signedValue(descriptor->step);
    switch (descriptor->type) {
    case UML_V4L2_CTRL_TYPE_INTEGER:
        return value >= minimum && value <= maximum && (step <= 0 || (value - minimum) % step == 0)
            ? ControlCheck::ok : ControlCheck::invalid;
    case UML_V4L2_CTRL_TYPE_BOOLEAN:
        return value == 0 || value == 1 ? ControlCheck::ok : ControlCheck::invalid;
    case UML_V4L2_CTRL_TYPE_MENU:
        return value >= minimum && value <= maximum && value < 64 && !((descriptor->menu_skip_mask >> value) & 1)
            ? ControlCheck::ok : ControlCheck::invalid;
    case UML_V4L2_CTRL_TYPE_BUTTON:
        return ControlCheck::ok;
    default:
        return descriptor->elem_size != 0 && payloadSize == descriptor->elem_size ? ControlCheck::ok
                                                                                  : ControlCheck::invalid;
    }
}

bool isDynamic(const virtio_video_v4l2_control_desc& descriptor) noexcept
{
    return (descriptor.flags & VIRTIO_VIDEO_V4L2_CTRL_F_DYNAMIC) != 0;
}

std::uint32_t levelIdc(VideoCodec codec, std::int64_t menuIndex) noexcept
{
    const auto lookup = [menuIndex](const auto& table) -> std::uint32_t {
        for (const auto& entry : table)
            if (std::int64_t(entry.index) == menuIndex) return entry.idc;
        return 0;
    };
    switch (codec) {
    case VideoCodec::h264: return lookup(h264Levels);
    case VideoCodec::hevc: return lookup(hevcLevels);
    default: return 0;
    }
}

VideoEncodeSettings encodeSettings(const EncodeStream& stream, const ControlValues& values)
{
    VideoEncodeSettings settings;
    settings.codec = stream.codec;
    settings.width = stream.width;
    settings.height = stream.height;
    settings.input = stream.input;
    settings.color = colorFor(stream.info);
    settings.frameRateNumerator = stream.info.intervalDenominator;
    settings.frameRateDenominator = stream.info.intervalNumerator;
    const bool h264 = stream.codec == VideoCodec::h264;
    if (stream.codec == VideoCodec::jpeg) {
        settings.profile = VideoEncodeProfile::jpegBaseline;
        settings.jpegQuality = std::uint32_t(scalar(values, UML_V4L2_CID_JPEG_COMPRESSION_QUALITY, defaultJpegQuality));
        settings.rateControl = VideoRateControl::constantQuality;
        return settings;
    }
    const auto id = [h264](std::uint32_t h264Id, std::uint32_t hevcId) { return h264 ? h264Id : hevcId; };
    settings.profile = profileFor(stream.codec, scalar(values, id(UML_V4L2_CID_MPEG_VIDEO_H264_PROFILE,
                                                                  UML_V4L2_CID_MPEG_VIDEO_HEVC_PROFILE)));
    settings.level = levelIdc(stream.codec, scalar(values, id(UML_V4L2_CID_MPEG_VIDEO_H264_LEVEL,
                                                              UML_V4L2_CID_MPEG_VIDEO_HEVC_LEVEL), -1));
    switch (scalar(values, UML_V4L2_CID_MPEG_VIDEO_BITRATE_MODE)) {
    case UML_V4L2_MPEG_VIDEO_BITRATE_MODE_CBR: settings.rateControl = VideoRateControl::constantBitrate; break;
    case UML_V4L2_MPEG_VIDEO_BITRATE_MODE_CQ: settings.rateControl = VideoRateControl::constantQuality; break;
    default: settings.rateControl = VideoRateControl::variableBitrate; break;
    }
    if (scalar(values, UML_V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 1) == 0)
        settings.rateControl = VideoRateControl::constantQp;
    settings.bitrate = std::uint64_t(scalar(values, UML_V4L2_CID_MPEG_VIDEO_BITRATE, defaultBitrate));
    settings.peakBitrate = std::uint64_t(scalar(values, UML_V4L2_CID_MPEG_VIDEO_BITRATE_PEAK));
    settings.quality = std::uint32_t(scalar(values, UML_V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY, 50));
    settings.frameQp = {
        std::uint32_t(scalar(values, id(UML_V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP, UML_V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP), defaultFrameQp)),
        std::uint32_t(scalar(values, id(UML_V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP, UML_V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP), defaultFrameQp)),
        std::uint32_t(scalar(values, id(UML_V4L2_CID_MPEG_VIDEO_H264_B_FRAME_QP, UML_V4L2_CID_MPEG_VIDEO_HEVC_B_FRAME_QP), defaultFrameQp))};
    /* A bound the guest left where the host publishes it is not a request. */
    const auto minimumId = id(UML_V4L2_CID_MPEG_VIDEO_H264_MIN_QP, UML_V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP);
    const auto maximumId = id(UML_V4L2_CID_MPEG_VIDEO_H264_MAX_QP, UML_V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP);
    if (values.scalars.contains(minimumId) && scalar(values, minimumId) > 0)
        settings.minimumQp = std::uint32_t(scalar(values, minimumId));
    if (values.scalars.contains(maximumId) && scalar(values, maximumId) < 51)
        settings.maximumQp = std::uint32_t(scalar(values, maximumId));
    settings.keyFrameInterval = std::uint32_t(scalar(values, UML_V4L2_CID_MPEG_VIDEO_GOP_SIZE));
    /* B-frames are coded in the host's one pattern or not at all: a request
     * for fewer than its run is no B-frames, one for more is the run. */
    const auto askedBFrames = scalar(values, UML_V4L2_CID_MPEG_VIDEO_B_FRAMES);
    settings.bFrames = stream.maximumBFrames != 0 && askedBFrames >= std::int64_t(stream.maximumBFrames)
        ? stream.maximumBFrames : 0;
    if (h264) {
        if (scalar(values, UML_V4L2_CID_MPEG_VIDEO_H264_HIERARCHICAL_CODING) != 0)
            settings.temporalLayers = std::uint32_t(std::max<std::int64_t>(
                1, scalar(values, UML_V4L2_CID_MPEG_VIDEO_H264_HIERARCHICAL_CODING_LAYER, 1)));
    } else {
        settings.temporalLayers = std::uint32_t(std::max<std::int64_t>(
            1, scalar(values, UML_V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_LAYER, 1)));
    }
    if (settings.temporalLayers > 1) {
        const auto first = scalar(values, id(UML_V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L0_BR,
                                             UML_V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_L0_BR));
        const auto second = scalar(values, id(UML_V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L1_BR,
                                              UML_V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_L1_BR));
        if (first > 0 && first + second > 0)
            settings.baseLayerBitratePercent = std::uint32_t((first * 100 + (first + second) / 2) / (first + second));
    }
    settings.cabac = scalar(values, UML_V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE,
                            UML_V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC) == UML_V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CABAC;
    const auto buffer = h264 && scalar(values, UML_V4L2_CID_MPEG_VIDEO_H264_CPB_SIZE) != 0
        ? scalar(values, UML_V4L2_CID_MPEG_VIDEO_H264_CPB_SIZE) : scalar(values, UML_V4L2_CID_MPEG_VIDEO_VBV_SIZE);
    settings.bufferBits = std::uint64_t(buffer) * 8192;
    /* Pacing policy of the host: a stream that asks for no reordering and no
     * quality target is a live stream, so it gets the encoder's real-time
     * behavior. */
    settings.realTime = settings.bFrames == 0 && settings.rateControl != VideoRateControl::constantQuality;
    settings.separateParameterSets = scalar(values, UML_V4L2_CID_MPEG_VIDEO_HEADER_MODE,
        UML_V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME) == UML_V4L2_MPEG_VIDEO_HEADER_MODE_SEPARATE;
    /* The device removes repeated parameter sets itself when the guest turns
     * the control off, which it may do at any time. */
    settings.parameterSetsWithEveryKeyFrame = true;
    const auto light = values.payloads.find(UML_V4L2_CID_COLORIMETRY_HDR10_CLL_INFO);
    const auto display = values.payloads.find(UML_V4L2_CID_COLORIMETRY_HDR10_MASTERING_DISPLAY);
    if (stream.codec == VideoCodec::hevc && (light != values.payloads.end() || display != values.payloads.end())) {
        uml::platform::VideoHdrStaticMetadata hdr;
        if (display != values.payloads.end()) {
            const auto& bytes = display->second;
            for (std::size_t primary = 0; primary < 3; ++primary) {
                hdr.displayPrimaries[2 * primary] = readAs<std::uint16_t>(bytes, 2 * primary);
                hdr.displayPrimaries[2 * primary + 1] = readAs<std::uint16_t>(bytes, 6 + 2 * primary);
            }
            hdr.whitePointX = readAs<std::uint16_t>(bytes, 12);
            hdr.whitePointY = readAs<std::uint16_t>(bytes, 14);
            hdr.maximumLuminance = readAs<std::uint32_t>(bytes, 16);
            hdr.minimumLuminance = readAs<std::uint32_t>(bytes, 20);
        }
        if (light != values.payloads.end()) {
            hdr.maximumContentLightLevel = readAs<std::uint16_t>(light->second, 0);
            hdr.maximumFrameAverageLightLevel = readAs<std::uint16_t>(light->second, 2);
        }
        settings.hdr = hdr;
    }
    return settings;
}

VideoEncodeUpdate encodeUpdate(const EncodeStream& stream, std::span<const std::uint32_t> changed,
                               const ControlValues& values)
{
    VideoEncodeUpdate update;
    const bool h264 = stream.codec == VideoCodec::h264;
    for (const auto id : changed) {
        switch (id) {
        case UML_V4L2_CID_MPEG_VIDEO_BITRATE: update.bitrate = std::uint64_t(scalar(values, id)); break;
        case UML_V4L2_CID_MPEG_VIDEO_BITRATE_PEAK: update.peakBitrate = std::uint64_t(scalar(values, id)); break;
        case UML_V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY: update.quality = std::uint32_t(scalar(values, id)); break;
        case UML_V4L2_CID_JPEG_COMPRESSION_QUALITY: update.jpegQuality = std::uint32_t(scalar(values, id)); break;
        case UML_V4L2_CID_MPEG_VIDEO_H264_MAX_QP:
            if (h264) update.maximumQp = std::uint32_t(scalar(values, id));
            break;
        case UML_V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP:
            if (!h264) update.maximumQp = std::uint32_t(scalar(values, id));
            break;
        default: break;
        }
    }
    return update;
}

} // namespace uml::video
