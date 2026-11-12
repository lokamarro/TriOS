// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "video_encoder.hpp"
#include "virtio_video.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace uml::video {

/* How the encoder device names what the platform encoder can do on the
 * Virtio video wire. */

[[nodiscard]] std::uint32_t wireRawFormat(uml::platform::RawVideoLayout layout) noexcept;
[[nodiscard]] std::optional<uml::platform::RawVideoLayout> platformRawLayout(std::uint32_t format) noexcept;
/* The coded format of a codec the encoder produces, and the codec of a coded
 * format; empty for a codec the encoder wire does not carry (VP9 has no
 * hardware encoder here). */
[[nodiscard]] std::optional<std::uint32_t> encodedWireFormat(uml::platform::VideoCodec codec) noexcept;
[[nodiscard]] std::optional<uml::platform::VideoCodec> encodedCodec(std::uint32_t format) noexcept;

/* The frame rates every format advertises; the rate a stream actually runs at
 * is its frame interval. */
inline constexpr std::uint32_t maximumAdvertisedFrameRate = 240;

/* The raw formats of the encoder's input queue in the order the capability
 * response lists them: those some profile of some codec accepted during
 * discovery, in a fixed order that does not depend on the platform's. */
[[nodiscard]] std::vector<uml::platform::RawVideoLayout> advertisedRawLayouts(
    std::span<const uml::platform::VideoEncodeCapability> capabilities);
/* The coded formats of the output queue, in the capability order. */
[[nodiscard]] std::vector<std::uint32_t> advertisedCodedFormats(
    std::span<const uml::platform::VideoEncodeCapability> capabilities);

/* The answer to VIRTIO_VIDEO_CMD_QUERY_CAPABILITY for `queue`: raw formats on
 * the input queue and coded formats on the output queue, each descriptor's
 * mask naming the formats of the other queue it works with. */
[[nodiscard]] std::vector<std::uint8_t> encoderCapabilityResponse(
    std::span<const uml::platform::VideoEncodeCapability> capabilities, std::uint32_t queue);

} // namespace uml::video
