// SPDX-License-Identifier: GPL-2.0-or-later
#include "encoder_capabilities.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace uml::video {
namespace {
using uml::platform::RawVideoLayout;
using uml::platform::VideoCodec;
using uml::platform::VideoEncodeCapability;

/* The order raw formats are listed in. A client that takes the first one it
 * understands gets the layout every encoder input path handles best. */
constexpr std::array rawOrder {RawVideoLayout::nv12, RawVideoLayout::i420, RawVideoLayout::p010,
                               RawVideoLayout::bgra, RawVideoLayout::rgba, RawVideoLayout::nv16, RawVideoLayout::nv24};

bool accepts(const VideoEncodeCapability& capability, RawVideoLayout layout)
{
    return std::any_of(capability.profiles.begin(), capability.profiles.end(), [layout](const auto& profile) {
        return std::find(profile.inputs.begin(), profile.inputs.end(), layout) != profile.inputs.end();
    });
}

/* Planar 4:2:0, 4:2:2 and 4:4:4 formats can be a buffer of their own planes
 * or one buffer; packed and 16-bit formats are only ever one. */
std::uint32_t planesLayout(RawVideoLayout layout)
{
    switch (layout) {
    case RawVideoLayout::nv12: case RawVideoLayout::i420: case RawVideoLayout::nv16:
    case RawVideoLayout::nv24:
        return VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER | VIRTIO_VIDEO_PLANES_LAYOUT_PER_PLANE;
    case RawVideoLayout::p010: case RawVideoLayout::bgra: case RawVideoLayout::rgba:
        return VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER;
    }
    return VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER;
}

template<class T> void append(std::vector<std::uint8_t>& bytes, const T& value)
{
    const auto offset = bytes.size();
    bytes.resize(offset + sizeof(value));
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void appendFormat(std::vector<std::uint8_t>& result, std::uint64_t mask, std::uint32_t format,
                  std::uint32_t layout, std::uint32_t minimumWidth, std::uint32_t minimumHeight,
                  std::uint32_t maximumWidth, std::uint32_t maximumHeight, std::uint32_t step)
{
    virtio_video_format_desc desc {};
    desc.mask = mask;
    desc.format = format;
    desc.planes_layout = layout;
    desc.plane_align = 1;
    desc.num_frames = 1;
    append(result, desc);
    virtio_video_format_frame frame {};
    frame.width.min = minimumWidth;
    frame.height.min = minimumHeight;
    frame.width.max = maximumWidth;
    frame.height.max = maximumHeight;
    frame.width.step = frame.height.step = std::max<std::uint32_t>(step, 1);
    frame.num_rates = 1;
    append(result, frame);
    virtio_video_format_range rate {};
    rate.min = 1;
    rate.max = maximumAdvertisedFrameRate;
    rate.step = 1;
    append(result, rate);
}
}

std::uint32_t wireRawFormat(RawVideoLayout layout) noexcept
{
    switch (layout) {
    case RawVideoLayout::nv12: return VIRTIO_VIDEO_FORMAT_NV12;
    case RawVideoLayout::i420: return VIRTIO_VIDEO_FORMAT_YUV420;
    case RawVideoLayout::p010: return VIRTIO_VIDEO_FORMAT_P010;
    case RawVideoLayout::nv16: return VIRTIO_VIDEO_FORMAT_NV16;
    case RawVideoLayout::nv24: return VIRTIO_VIDEO_FORMAT_NV24;
    case RawVideoLayout::bgra: return VIRTIO_VIDEO_FORMAT_BGRA8888;
    case RawVideoLayout::rgba: return VIRTIO_VIDEO_FORMAT_RGBA8888;
    }
    return 0;
}

std::optional<RawVideoLayout> platformRawLayout(std::uint32_t format) noexcept
{
    for (const auto layout : rawOrder)
        if (wireRawFormat(layout) == format) return layout;
    return std::nullopt;
}

std::optional<std::uint32_t> encodedWireFormat(VideoCodec codec) noexcept
{
    switch (codec) {
    case VideoCodec::h264: return VIRTIO_VIDEO_FORMAT_H264;
    case VideoCodec::hevc: return VIRTIO_VIDEO_FORMAT_HEVC;
    case VideoCodec::jpeg: return VIRTIO_VIDEO_FORMAT_MJPEG;
    case VideoCodec::vp9: break;
    }
    return std::nullopt;
}

std::optional<VideoCodec> encodedCodec(std::uint32_t format) noexcept
{
    switch (format) {
    case VIRTIO_VIDEO_FORMAT_H264: return VideoCodec::h264;
    case VIRTIO_VIDEO_FORMAT_HEVC: return VideoCodec::hevc;
    case VIRTIO_VIDEO_FORMAT_MJPEG: return VideoCodec::jpeg;
    default: return std::nullopt;
    }
}

std::vector<RawVideoLayout> advertisedRawLayouts(std::span<const VideoEncodeCapability> capabilities)
{
    std::vector<RawVideoLayout> layouts;
    for (const auto layout : rawOrder)
        if (std::any_of(capabilities.begin(), capabilities.end(),
                        [layout](const auto& capability) { return accepts(capability, layout); }))
            layouts.push_back(layout);
    return layouts;
}

std::vector<std::uint32_t> advertisedCodedFormats(std::span<const VideoEncodeCapability> capabilities)
{
    std::vector<std::uint32_t> formats;
    for (const auto& capability : capabilities) {
        const auto format = encodedWireFormat(capability.codec);
        if (format && std::find(formats.begin(), formats.end(), *format) == formats.end())
            formats.push_back(*format);
    }
    return formats;
}

std::vector<std::uint8_t> encoderCapabilityResponse(
    std::span<const VideoEncodeCapability> capabilities, std::uint32_t queue)
{
    if (queue != VIRTIO_VIDEO_QUEUE_TYPE_INPUT && queue != VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT)
        throw std::invalid_argument("invalid video queue");
    const auto raw = advertisedRawLayouts(capabilities);
    const auto coded = advertisedCodedFormats(capabilities);
    const bool input = queue == VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    virtio_video_query_capability_resp header {};
    header.hdr.type = VIRTIO_VIDEO_RESP_OK_QUERY_CAPABILITY;
    header.num_descs = std::uint32_t(input ? raw.size() : coded.size());
    std::vector<std::uint8_t> result;
    append(result, header);
    const auto codecOf = [&capabilities](std::uint32_t format) -> const VideoEncodeCapability& {
        return *std::find_if(capabilities.begin(), capabilities.end(), [format](const auto& capability) {
            return encodedWireFormat(capability.codec) == format;
        });
    };
    if (input) {
        for (const auto layout : raw) {
            std::uint64_t mask = 0;
            std::uint32_t minimumWidth = ~0u, minimumHeight = ~0u, maximumWidth = 0, maximumHeight = 0;
            std::uint32_t step = ~0u;
            for (std::size_t index = 0; index < coded.size(); ++index) {
                const auto& capability = codecOf(coded[index]);
                if (!accepts(capability, layout)) continue;
                mask |= std::uint64_t(1) << index;
                minimumWidth = std::min(minimumWidth, capability.minimumWidth);
                minimumHeight = std::min(minimumHeight, capability.minimumHeight);
                maximumWidth = std::max(maximumWidth, capability.maximumWidth);
                maximumHeight = std::max(maximumHeight, capability.maximumHeight);
                step = std::min(step, capability.sizeAlignment);
            }
            appendFormat(result, mask, wireRawFormat(layout), planesLayout(layout), minimumWidth,
                         minimumHeight, maximumWidth, maximumHeight, step);
        }
    } else {
        for (const auto format : coded) {
            const auto& capability = codecOf(format);
            std::uint64_t mask = 0;
            for (std::size_t index = 0; index < raw.size(); ++index)
                if (accepts(capability, raw[index])) mask |= std::uint64_t(1) << index;
            appendFormat(result, mask, format, VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER,
                         capability.minimumWidth, capability.minimumHeight, capability.maximumWidth,
                         capability.maximumHeight, capability.sizeAlignment);
        }
    }
    return result;
}

} // namespace uml::video
