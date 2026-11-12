// SPDX-License-Identifier: GPL-2.0-or-later

#include "kosmickrisp_capability_test_support.hpp"

#include <vulkan/vulkan_core.h>

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

using namespace uml::gpu::testing;

namespace {

std::vector<std::uint64_t> bufferFeatures(
    std::string_view report,
    VkFormat format)
{
    const std::string formatMarker =
        "{\"format\":" + std::to_string(static_cast<int>(format)) + ',';
    constexpr std::string_view bufferMarker = ",\"buffer\":";
    std::vector<std::uint64_t> features;

    for (std::size_t position = 0;
         (position = report.find(formatMarker, position)) !=
         std::string_view::npos;) {
        const auto objectEnd = report.find('}', position);
        const auto valueStart = report.find(bufferMarker, position);
        require(objectEnd != std::string_view::npos);
        require(valueStart != std::string_view::npos && valueStart < objectEnd);

        const auto first = report.data() + valueStart + bufferMarker.size();
        const auto last = report.data() + objectEnd;
        std::uint64_t value = 0;
        const auto parsed = std::from_chars(first, last, value);
        require(parsed.ec == std::errc{} && parsed.ptr == last);
        features.push_back(value);
        position = objectEnd + 1;
    }

    return features;
}

void requireRgb32UniformOnly(
    std::string_view report,
    std::size_t deviceCount,
    VkFormat format)
{
    const auto values = bufferFeatures(report, format);
    require(!values.empty());
    require(values.size() == deviceCount);
    for (const auto value : values) {
        require((value & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT) != 0);
        require((value & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT) == 0);
        require(
            (value & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT) == 0);
    }
}

} // namespace

int main()
{
    const auto report = probeReport();
    const auto deviceCount =
        countOccurrences(report, "\"supported_core_formats\":[");
    require(deviceCount != 0);

    requireRgb32UniformOnly(
        report, deviceCount, VK_FORMAT_R32G32B32_SFLOAT);
    requireRgb32UniformOnly(
        report, deviceCount, VK_FORMAT_R32G32B32_SINT);
    requireRgb32UniformOnly(
        report, deviceCount, VK_FORMAT_R32G32B32_UINT);

    std::puts("KosmicKrisp RGB32 uniform texel-buffer capabilities passed");
    return EXIT_SUCCESS;
}
