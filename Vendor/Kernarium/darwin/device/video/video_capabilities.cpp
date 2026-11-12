// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_capabilities.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace uml::video {
namespace {
template<class T> void append(std::vector<std::uint8_t>& bytes, const T& value)
{
    const auto offset = bytes.size();
    bytes.resize(offset + sizeof(value));
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
}

// The host platform may decode a codec this private wire contract has no
// identifier for. Such a capability is dropped before the device sees it
// rather than becoming an unnamed format on the queue.
bool wireNames(uml::platform::VideoCodec codec) noexcept
{
    using enum uml::platform::VideoCodec;
    switch (codec) {
    case h264: case hevc: case jpeg: case vp9: return true;
    }
    return false;
}

std::uint32_t wireCodec(uml::platform::VideoCodec codec)
{
    using enum uml::platform::VideoCodec;
    switch (codec) {
    case h264: return VIRTIO_VIDEO_FORMAT_H264;
    case hevc: return VIRTIO_VIDEO_FORMAT_HEVC;
    case jpeg: return VIRTIO_VIDEO_FORMAT_MJPEG;
    case vp9: return VIRTIO_VIDEO_FORMAT_VP9;
    }
    throw std::invalid_argument("invalid codec");
}

std::uint32_t wireProfile(uml::platform::VideoProfile profile)
{
    using enum uml::platform::VideoProfile;
    switch (profile) {
    case h264Baseline: return VIRTIO_VIDEO_PROFILE_H264_BASELINE;
    case h264ConstrainedBaseline: return VIRTIO_VIDEO_PROFILE_H264_CONSTRAINED_BASELINE;
    case h264Main: return VIRTIO_VIDEO_PROFILE_H264_MAIN;
    case h264High: return VIRTIO_VIDEO_PROFILE_H264_HIGH;
    case h264ConstrainedHigh: return VIRTIO_VIDEO_PROFILE_H264_CONSTRAINED_HIGH;
    case h264High10: return VIRTIO_VIDEO_PROFILE_H264_HIGH10PROFILE;
    case hevcMain: return VIRTIO_VIDEO_PROFILE_HEVC_MAIN;
    case hevcMain10: return VIRTIO_VIDEO_PROFILE_HEVC_MAIN10;
    case jpegBaseline: return 0;
    case vp9Profile0: return VIRTIO_VIDEO_PROFILE_VP9_PROFILE0;
    }
    throw std::invalid_argument("invalid video profile");
}

std::vector<std::uint32_t> profileValues(
    std::span<const uml::platform::VideoDecodeCapability> capabilities, std::uint32_t format)
{
    std::vector<std::uint32_t> values;
    for (const auto& cap : capabilities) {
        if (wireCodec(cap.codec) == format && wireProfile(cap.profile))
            values.push_back(wireProfile(cap.profile));
    }
    return values;
}

namespace {
struct LevelRow {
    std::uint32_t idc;    // level_idc, or general_level_idc for HEVC
    std::uint32_t wire;
};
constexpr LevelRow h264Levels[] = {
    {10, VIRTIO_VIDEO_LEVEL_H264_1_0}, {11, VIRTIO_VIDEO_LEVEL_H264_1_1},
    {12, VIRTIO_VIDEO_LEVEL_H264_1_2}, {13, VIRTIO_VIDEO_LEVEL_H264_1_3},
    {20, VIRTIO_VIDEO_LEVEL_H264_2_0}, {21, VIRTIO_VIDEO_LEVEL_H264_2_1},
    {22, VIRTIO_VIDEO_LEVEL_H264_2_2}, {30, VIRTIO_VIDEO_LEVEL_H264_3_0},
    {31, VIRTIO_VIDEO_LEVEL_H264_3_1}, {32, VIRTIO_VIDEO_LEVEL_H264_3_2},
    {40, VIRTIO_VIDEO_LEVEL_H264_4_0}, {41, VIRTIO_VIDEO_LEVEL_H264_4_1},
    {42, VIRTIO_VIDEO_LEVEL_H264_4_2}, {50, VIRTIO_VIDEO_LEVEL_H264_5_0},
    {51, VIRTIO_VIDEO_LEVEL_H264_5_1}, {52, VIRTIO_VIDEO_LEVEL_H264_5_2},
    {60, VIRTIO_VIDEO_LEVEL_H264_6_0}, {61, VIRTIO_VIDEO_LEVEL_H264_6_1},
    {62, VIRTIO_VIDEO_LEVEL_H264_6_2},
};
constexpr LevelRow hevcLevels[] = {
    {30, VIRTIO_VIDEO_LEVEL_HEVC_1},    {60, VIRTIO_VIDEO_LEVEL_HEVC_2},
    {63, VIRTIO_VIDEO_LEVEL_HEVC_2_1},  {90, VIRTIO_VIDEO_LEVEL_HEVC_3},
    {93, VIRTIO_VIDEO_LEVEL_HEVC_3_1},  {120, VIRTIO_VIDEO_LEVEL_HEVC_4},
    {123, VIRTIO_VIDEO_LEVEL_HEVC_4_1}, {150, VIRTIO_VIDEO_LEVEL_HEVC_5},
    {153, VIRTIO_VIDEO_LEVEL_HEVC_5_1}, {156, VIRTIO_VIDEO_LEVEL_HEVC_5_2},
    {180, VIRTIO_VIDEO_LEVEL_HEVC_6},   {183, VIRTIO_VIDEO_LEVEL_HEVC_6_1},
    {186, VIRTIO_VIDEO_LEVEL_HEVC_6_2},
};
} // namespace

std::vector<std::uint32_t> levelValues(
    std::span<const uml::platform::VideoDecodeCapability> capabilities, std::uint32_t format)
{
    std::span<const LevelRow> rows;
    if (format == VIRTIO_VIDEO_FORMAT_H264) rows = h264Levels;
    else if (format == VIRTIO_VIDEO_FORMAT_HEVC) rows = hevcLevels;
    std::uint32_t highest = 0;
    for (const auto& cap : capabilities)
        if (wireCodec(cap.codec) == format) highest = std::max(highest, cap.maximumLevel);
    std::vector<std::uint32_t> values;
    for (const auto& row : rows)
        if (row.idc <= highest) values.push_back(row.wire);
    return values;
}

std::vector<std::uint8_t> capabilityResponse(
    std::span<const uml::platform::VideoDecodeCapability> capabilities, std::uint32_t queue)
{
    if (queue != VIRTIO_VIDEO_QUEUE_TYPE_INPUT && queue != VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT)
        throw std::invalid_argument("invalid video queue");
    std::vector<std::uint32_t> coded;
    bool tenBit = false;
    for (const auto& cap : capabilities) {
        const auto format = wireCodec(cap.codec);
        if (std::find(coded.begin(), coded.end(), format) == coded.end()) coded.push_back(format);
        tenBit |= cap.bitDepth == 10;
    }
    /* Packed BGRA is decoded to by the host, so it is offered for every
     * stream regardless of the coded bit depth. A client that can only draw
     * through a 32-bit RGB surface would otherwise convert each frame itself
     * on the guest processor.
     *
     * It is offered first because the head of this list is the format a client
     * that never negotiates one receives, and because the host packs it in
     * hardware at no more cost than a planar frame. A client whose renderer
     * needs a single renderable plane - which is every GL client - then finds
     * the device already producing what it draws, instead of picking a packed
     * format and being handed planar data. A client that wants planar frames
     * asks for them, and the ones below are unchanged. */
    std::vector<std::uint32_t> raw {VIRTIO_VIDEO_FORMAT_BGRA8888, VIRTIO_VIDEO_FORMAT_NV12,
                                    VIRTIO_VIDEO_FORMAT_YUV420};
    if (tenBit) raw.push_back(VIRTIO_VIDEO_FORMAT_P010);
    const auto& formats = queue == VIRTIO_VIDEO_QUEUE_TYPE_INPUT ? coded : raw;
    virtio_video_query_capability_resp header {};
    header.hdr.type = VIRTIO_VIDEO_RESP_OK_QUERY_CAPABILITY;
    header.num_descs = std::uint32_t(formats.size());
    std::vector<std::uint8_t> result;
    append(result, header);
    for (const auto format : formats) {
        std::uint32_t width = 0, height = 0;
        std::uint64_t mask = 0;
        for (const auto& cap : capabilities) {
            const bool input = queue == VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
            if (input && wireCodec(cap.codec) != format) continue;
            // P010 carries 10-bit samples and nothing else; every other raw
            // format is reached from either depth, a 10-bit stream being narrowed.
            if (!input && format == VIRTIO_VIDEO_FORMAT_P010 && cap.bitDepth != 10) continue;
            width = std::max(width, cap.maximumWidth);
            height = std::max(height, cap.maximumHeight);
            if (input) {
                /* A descriptor's mask names positions in the *other* queue's
                 * list, in the order this very response carries it. A coded
                 * format reaches the packed format and the 8-bit planar
                 * formats whatever its coded bit depth, and a 10-bit one also
                 * reaches P010 - which is what the device itself enforces
                 * when a stream negotiates its capture format. */
                for (std::size_t index = 0; index < raw.size(); ++index) {
                    const bool tenBitRaw = raw[index] == VIRTIO_VIDEO_FORMAT_P010;
                    if (!tenBitRaw || cap.bitDepth == 10) mask |= std::uint64_t(1) << index;
                }
            } else {
                const auto found = std::find(coded.begin(), coded.end(), wireCodec(cap.codec));
                mask |= std::uint64_t(1) << std::size_t(found - coded.begin());
            }
        }
        virtio_video_format_desc desc {};
        desc.mask = mask;
        desc.format = format;
        desc.planes_layout = format == VIRTIO_VIDEO_FORMAT_NV12 || format == VIRTIO_VIDEO_FORMAT_YUV420
            ? VIRTIO_VIDEO_PLANES_LAYOUT_PER_PLANE : VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER;
        desc.plane_align = 1;
        desc.num_frames = 1;
        append(result, desc);
        virtio_video_format_frame frame {};
        frame.width.min = frame.height.min = 64;
        frame.width.max = width;
        frame.height.max = height;
        frame.width.step = frame.height.step = 1;
        frame.num_rates = 1;
        append(result, frame);
        virtio_video_format_range rate {};
        rate.min = 1;
        rate.max = 240;
        rate.step = 1;
        append(result, rate);
    }
    return result;
}
} // namespace uml::video
