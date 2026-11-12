// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "video_encoder.hpp"
#include "virtio_video.h"
#include "virtio_video_v4l2_ids.h"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace uml::video {

/* What the encoder device publishes as V4L2 controls, and how a stream's
 * values become the platform encoder's settings. The control table in
 * darwin/device/video/README.md#encoder-controls summarizes the contract; this
 * is its implementation, and it is a pure function of the platform's
 * capabilities so that a test can check it without a device. */

/* The fractional frame interval and the colorimetry of the raw frames, in V4L2
 * enum values, as VIRTIO_VIDEO_CMD_SET_STREAM_INFO carries them. */
struct StreamInfo {
    std::uint32_t intervalNumerator {1};
    std::uint32_t intervalDenominator {30};
    std::uint32_t colorspace {};
    std::uint32_t transfer {};
    std::uint32_t matrix {};
    std::uint32_t quantization {};
    friend bool operator==(const StreamInfo&, const StreamInfo&) = default;
};

/* `info` with every enum value the encoder cannot describe replaced by the
 * default, which is how a V4L2 driver answers a value it does not support. */
[[nodiscard]] StreamInfo sanitized(const StreamInfo& info) noexcept;

/* The value of every control a stream has set or defaults. A compound
 * control keeps the bytes the guest sent. */
struct ControlValues {
    std::map<std::uint32_t, std::int64_t> scalars;
    std::map<std::uint32_t, std::vector<std::uint8_t>> payloads;
};

enum class ControlCheck : std::uint8_t {
    ok,
    unsupported,  // not a control this host publishes
    invalid,      // outside the published range, step or menu
};

class EncoderControls final {
public:
    explicit EncoderControls(std::span<const uml::platform::VideoEncodeCapability> capabilities);

    [[nodiscard]] const std::vector<virtio_video_v4l2_control_desc>& descriptors() const noexcept
    {
        return descriptors_;
    }
    [[nodiscard]] const virtio_video_v4l2_control_desc* find(std::uint32_t id) const noexcept;
    /* Every published control at its default. */
    [[nodiscard]] ControlValues defaults() const;
    /* Whether `scalar` (or, for a compound control, a payload of
     * `payloadSize` bytes) is a value `id` accepts. */
    [[nodiscard]] ControlCheck check(std::uint32_t id, std::int64_t scalar,
                                     std::size_t payloadSize) const noexcept;

private:
    std::vector<virtio_video_v4l2_control_desc> descriptors_;
};

/* Whether a control may change while the stream encodes. */
[[nodiscard]] bool isDynamic(const virtio_video_v4l2_control_desc& descriptor) noexcept;

/* The level_idc (H.264) or general_level_idc (HEVC) a level menu index names,
 * and zero for an index no level has. */
[[nodiscard]] std::uint32_t levelIdc(uml::platform::VideoCodec codec, std::int64_t menuIndex) noexcept;

/* What a stream asks of the platform encoder. `width` and `height` are the
 * encoded size. */
struct EncodeStream {
    uml::platform::VideoCodec codec {uml::platform::VideoCodec::h264};
    uml::platform::RawVideoLayout input {uml::platform::RawVideoLayout::nv12};
    std::uint32_t width {};
    std::uint32_t height {};
    StreamInfo info;
    /* The B-frame run the host codes for this codec (its capability's
     * maximumBFrames), zero when it codes none. The platform codes the whole
     * run or none, so a request below it reaches the encoder as none. */
    std::uint32_t maximumBFrames {};
};

[[nodiscard]] uml::platform::VideoEncodeSettings encodeSettings(
    const EncodeStream& stream, const ControlValues& values);

/* The part of `settings` a running session may change, as controls `changed`
 * set it. Fields of other controls stay unset. */
[[nodiscard]] uml::platform::VideoEncodeUpdate encodeUpdate(
    const EncodeStream& stream, std::span<const std::uint32_t> changed, const ControlValues& values);

} // namespace uml::video
