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
        "{\"name\":\"VK_KHR_incremental_present\",\"spec_version\":2}";
    constexpr std::string_view enabledCapability =
        "\"incremental_present\":{\"extension\":true,"
        "\"swapchain_extension\":true}";
    constexpr std::string_view enabledDevice = "\"device_create_result\":0";

    const auto extensionCount = countOccurrences(report, extensionProperty);
    const auto capabilityCount = countOccurrences(report, enabledCapability);
    const auto deviceCount = countOccurrences(report, enabledDevice);
    require(extensionCount != 0);
    require(capabilityCount == extensionCount);
    require(deviceCount == extensionCount);

    // The device extension depends on VK_KHR_swapchain. Reject a provider
    // report that advertises incremental present without its prerequisite.
    require(report.find(
                "\"incremental_present\":{\"extension\":true,"
                "\"swapchain_extension\":false}") ==
            std::string_view::npos);

    std::puts("KosmicKrisp incremental-present capabilities passed");
    return EXIT_SUCCESS;
}
