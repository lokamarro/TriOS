// SPDX-License-Identifier: GPL-2.0-or-later

#include "kosmickrisp_capability_test_support.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

using namespace uml::gpu::testing;

namespace {

struct CubeCoordinate {
    double u;
    double v;
    uint32_t layer;
};

CubeCoordinate mapCube(const std::array<double, 3> &coordinate,
                       double cubeLayer = 0.0)
{
    const auto x = coordinate[0];
    const auto y = coordinate[1];
    const auto z = coordinate[2];
    const auto absX = std::abs(x);
    const auto absY = std::abs(y);
    const auto absZ = std::abs(z);
    const auto layerBase =
        static_cast<uint32_t>(std::nearbyint(cubeLayer)) * 6U;
    if (absX >= absY && absX >= absZ) {
        const auto scale = -0.5 / absX;
        return {
            std::signbit(x) ? -scale * z + 0.5 : scale * z + 0.5,
            scale * y + 0.5,
            layerBase + (std::signbit(x) ? 1U : 0U),
        };
    }
    if (absY >= absX && absY >= absZ) {
        const auto scale = 0.5 / absY;
        return {
            scale * x + 0.5,
            std::signbit(y) ? -scale * z + 0.5 : scale * z + 0.5,
            layerBase + (std::signbit(y) ? 3U : 2U),
        };
    }
    const auto scale = -0.5 / absZ;
    return {
        std::signbit(z) ? scale * x + 0.5 : -scale * x + 0.5,
        scale * y + 0.5,
        layerBase + (std::signbit(z) ? 5U : 4U),
    };
}

void requireNear(double actual, double expected, double tolerance = 1.0e-9)
{
    require(std::abs(actual - expected) <= tolerance);
}

void verifyCubeMapping()
{
    const auto positiveZ = mapCube({0.5, 0.25, 1.0});
    require(positiveZ.layer == 4);
    requireNear(positiveZ.u, 0.75);
    requireNear(positiveZ.v, 0.375);

    const auto negativeZ = mapCube({0.5, 0.25, -1.0});
    require(negativeZ.layer == 5);
    requireNear(negativeZ.u, 0.25);
    requireNear(negativeZ.v, 0.375);

    constexpr double epsilon = 1.0e-6;
    const auto xSide = mapCube({1.0, 0.2, 1.0 - epsilon});
    const auto zSide = mapCube({1.0 - epsilon, 0.2, 1.0});
    require(xSide.layer == 0);
    require(zSide.layer == 4);
    requireNear(xSide.u, 0.5 * epsilon, epsilon * epsilon);
    requireNear(zSide.u, 1.0 - 0.5 * epsilon, epsilon * epsilon);
    requireNear(xSide.v, zSide.v, epsilon);

    // X wins exact major-axis ties, matching the lowering's ordered selects.
    require(mapCube({1.0, 1.0, 1.0}).layer == 0);

    // Cube-array coordinates use round-to-nearest-even, then six faces.
    require(mapCube({0.5, 0.25, -1.0}, 1.5).layer == 17);
    require(mapCube({0.5, 0.25, -1.0}, 2.5).layer == 17);
}

} // namespace

int main()
{
    const auto report = probeReport();
    constexpr std::string_view extensionProperty =
        "{\"name\":\"VK_EXT_non_seamless_cube_map\",\"spec_version\":1}";
    constexpr std::string_view enabledCapability =
        "\"non_seamless_cube_map\":{\"extension\":true,\"feature\":true}";
    constexpr std::string_view enabledDevice = "\"device_create_result\":0";

    const auto extensionCount = countOccurrences(report, extensionProperty);
    require(extensionCount != 0);
    require(countOccurrences(report, enabledCapability) == extensionCount);
    require(countOccurrences(report, enabledDevice) == extensionCount);
    require(report.find(
                "\"non_seamless_cube_map\":{\"extension\":true,"
                "\"feature\":false}") == std::string_view::npos);
    require(report.find(
                "\"non_seamless_cube_map\":{\"extension\":false,"
                "\"feature\":true}") == std::string_view::npos);

    const auto supportedFormats = unsignedField(
        report, "\"cube_alias_views\":{\"supported_format_count\":");
    const auto cubeViews = unsignedField(report, "\"cube_view_count\":");
    const auto cubeArrayViews =
        unsignedField(report, "\"cube_array_view_count\":");
    require(supportedFormats != 0);
    require(cubeViews == supportedFormats);
    require(cubeArrayViews == supportedFormats);

    verifyCubeMapping();
    std::printf(
        "KosmicKrisp non-seamless cube capabilities and %llu format aliases "
        "passed\n",
        static_cast<unsigned long long>(supportedFormats));
    return EXIT_SUCCESS;
}
