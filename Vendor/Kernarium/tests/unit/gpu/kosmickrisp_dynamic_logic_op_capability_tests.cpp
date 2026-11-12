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
        "{\"name\":\"VK_EXT_extended_dynamic_state2\","
        "\"spec_version\":1}";
    constexpr std::string_view enabledCapability =
        "\"extended_dynamic_state2\":{\"extension\":true,"
        "\"feature\":true,\"logic_op\":true,"
        "\"patch_control_points\":true}";
    constexpr std::string_view executionResult =
        "\"dynamic_logic_op_execution\":{\"entrypoint_count\":1,"
        "\"operation_count\":16,\"results_verified\":true,"
        "\"pipeline_reuse_verified\":true}";

    const auto extensionCount = countOccurrences(report, extensionProperty);
    require(extensionCount != 0);
    require(countOccurrences(report, enabledCapability) == extensionCount);
    require(countOccurrences(report, executionResult) == extensionCount);
    require(countOccurrences(report, "\"device_create_result\":0") ==
            extensionCount);

    require(report.find(
                "\"extended_dynamic_state2\":{\"extension\":true,"
                "\"feature\":true,\"logic_op\":false") ==
            std::string_view::npos);
    require(report.find(
                "\"extended_dynamic_state2\":{\"extension\":false,"
                "\"feature\":true") == std::string_view::npos);
    require(report.find("\"dynamic_logic_op_execution\":null") ==
            std::string_view::npos);

    std::puts("KosmicKrisp dynamic logic-op capabilities and execution passed");
    return EXIT_SUCCESS;
}
