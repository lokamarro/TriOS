// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "video_decoder.hpp"
#include "virtio_video.h"

#include <span>

namespace uml::video {
bool wireNames(uml::platform::VideoCodec codec) noexcept;
std::uint32_t wireCodec(uml::platform::VideoCodec codec);
std::uint32_t wireProfile(uml::platform::VideoProfile profile);
std::vector<std::uint8_t> capabilityResponse(
    std::span<const uml::platform::VideoDecodeCapability> capabilities, std::uint32_t queue);
std::vector<std::uint32_t> profileValues(
    std::span<const uml::platform::VideoDecodeCapability> capabilities, std::uint32_t format);
// The levels of a coded format, in ascending order, from the lowest up to the
// highest the capabilities for that format state as their maximumLevel.
std::vector<std::uint32_t> levelValues(
    std::span<const uml::platform::VideoDecodeCapability> capabilities, std::uint32_t format);
} // namespace uml::video
