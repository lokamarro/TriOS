// SPDX-License-Identifier: GPL-2.0-or-later

#include "kosmickrisp_capability_test_support.hpp"

#include <cstdio>
#include <cstdlib>
#include <string_view>

using namespace uml::gpu::testing;

int main()
{
    const auto report = probeReport();
    constexpr std::string_view dynamicVertexInputExtension =
        "{\"name\":\"VK_EXT_vertex_input_dynamic_state\","
        "\"spec_version\":2}";
    constexpr std::string_view legacyVertexAttributesExtension =
        "{\"name\":\"VK_EXT_legacy_vertex_attributes\","
        "\"spec_version\":1}";
    constexpr std::string_view dynamicVertexInputCapability =
        "\"vertex_input_dynamic_state\":{\"extension\":true,"
        "\"feature\":true}";
    constexpr std::string_view legacyVertexAttributesCapability =
        "\"legacy_vertex_attributes\":{\"extension\":true,"
        "\"feature\":true,\"native_unaligned_performance\":false}";
    constexpr std::string_view successfulProbe =
        "\"legacy_vertex_attributes_probe\":{\"entrypoint_count\":1,"
        "\"compile_required_result\":1000297000,\"variant_count\":2,";
    constexpr std::string_view verifiedExecution =
        "\"same_key_race_recorded\":true,"
        "\"numeric_mismatch_verified\":true,"
        "\"unaligned_offset_stride_verified\":true,"
        "\"parent_destroy_verified\":true";

    const auto dynamicCount =
        countOccurrences(report, dynamicVertexInputExtension);
    const auto legacyCount =
        countOccurrences(report, legacyVertexAttributesExtension);
    require(dynamicCount != 0);
    require(legacyCount == dynamicCount);
    require(countOccurrences(report, dynamicVertexInputCapability) ==
            legacyCount);
    require(countOccurrences(report, legacyVertexAttributesCapability) ==
            legacyCount);
    require(countOccurrences(report, successfulProbe) == legacyCount);
    require(countOccurrences(report, verifiedExecution) == legacyCount);
    require(report.find("\"cold_draw_ns\":0") == std::string_view::npos);
    require(report.find("\"cached_draw_ns\":0") == std::string_view::npos);
    require(report.find("\"cold_switch_ns\":0") == std::string_view::npos);
    require(report.find("\"cached_switch_ns\":0") ==
            std::string_view::npos);
    require(countOccurrences(report, "\"device_create_result\":0") ==
            legacyCount);

    require(report.find(
                "\"vertex_input_dynamic_state\":{\"extension\":false,"
                "\"feature\":true}") == std::string_view::npos);
    require(report.find(
                "\"legacy_vertex_attributes\":{\"extension\":true,"
                "\"feature\":false") == std::string_view::npos);
    require(report.find("\"native_unaligned_performance\":true") ==
            std::string_view::npos);
    require(report.find("\"legacy_vertex_attributes_probe\":null") ==
            std::string_view::npos);

    std::puts(
        "KosmicKrisp legacy vertex-attribute capabilities and entry point "
        "passed");
    return EXIT_SUCCESS;
}
