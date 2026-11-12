// SPDX-License-Identifier: GPL-2.0-or-later

#include "kosmickrisp_capability_test_support.hpp"

#include <cstdio>
#include <cstdlib>
#include <string_view>

using namespace uml::gpu::testing;

int main()
{
    const auto report = probeReport();
    constexpr std::string_view extensionProperty =
        "{\"name\":\"VK_EXT_dynamic_rendering_unused_attachments\","
        "\"spec_version\":1}";
    constexpr std::string_view enabledCapability =
        "\"dynamic_rendering_unused_attachments\":{"
        "\"extension\":true,\"feature\":true}";
    constexpr std::string_view enabledDevice = "\"device_create_result\":0";

    const auto extensionCount = countOccurrences(report, extensionProperty);
    const auto capabilityCount = countOccurrences(report, enabledCapability);
    const auto deviceCount = countOccurrences(report, enabledDevice);
    require(extensionCount != 0);
    require(capabilityCount == extensionCount);
    require(deviceCount == extensionCount);

    // Reject contradictory provider reports rather than accepting an
    // extension string without the feature or a feature with no extension.
    require(report.find(
                "\"dynamic_rendering_unused_attachments\":{"
                "\"extension\":true,\"feature\":false}") ==
            std::string_view::npos);
    require(report.find(
                "\"dynamic_rendering_unused_attachments\":{"
                "\"extension\":false,\"feature\":true}") ==
            std::string_view::npos);

    std::puts(
        "KosmicKrisp unused dynamic-rendering attachment capabilities passed");
    return EXIT_SUCCESS;
}
