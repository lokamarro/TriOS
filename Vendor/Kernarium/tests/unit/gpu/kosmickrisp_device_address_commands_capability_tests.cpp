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
        "{\"name\":\"VK_KHR_device_address_commands\","
        "\"spec_version\":1}";
    constexpr std::string_view enabledCapability =
        "\"device_address_commands\":{\"extension\":true,"
        "\"feature\":true,\"buffer_device_address\":true}";
    constexpr std::string_view executionPrefix =
        "\"device_address_commands_execution\":{\"entrypoint_count\":";
    constexpr std::string_view executionResult =
        "\"state_commands_recorded\":true,"
        "\"transfer_verified\":true}";

    const auto extensionCount = countOccurrences(report, extensionProperty);
    require(extensionCount != 0);
    require(countOccurrences(report, enabledCapability) == extensionCount);
    require(countOccurrences(report, executionPrefix) == extensionCount);
    require(countOccurrences(report, executionResult) == extensionCount);
    require(countOccurrences(report, "\"device_create_result\":0") ==
            extensionCount);

    std::size_t position = 0;
    for (std::size_t index = 0; index < extensionCount; ++index) {
        position = report.find(executionPrefix, position);
        require(position != std::string_view::npos);
        position += executionPrefix.size();
        require(unsignedAt(report, position) >= 11);
    }

    require(report.find(
                "\"device_address_commands\":{\"extension\":true,"
                "\"feature\":false") == std::string_view::npos);
    require(report.find(
                "\"device_address_commands\":{\"extension\":false,"
                "\"feature\":true") == std::string_view::npos);
    require(report.find(
                "\"device_address_commands\":{\"extension\":true,"
                "\"feature\":true,\"buffer_device_address\":false}") ==
            std::string_view::npos);

    std::puts(
        "KosmicKrisp device-address command capabilities and execution passed");
    return EXIT_SUCCESS;
}
