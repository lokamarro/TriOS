// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan_draw_test.hpp"
#include "require.hpp"

#include "shaders/line_multiview_spirv.hpp"
#include "shaders/geometry_side_effects_spirv.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using uml::testing::require;
using uml::gpu::test::DrawConfig;
using uml::gpu::test::DrawContext;
using uml::gpu::test::require_venus_when_asked;
using uml::gpu::test::DrawResult;

void verify(const DrawConfig &config, const DrawResult &result,
            std::string_view name)
{
    constexpr uint32_t report_words = 12;
    const uint32_t views = std::max(1, std::popcount(config.view_mask));
    const auto fail = [&](std::string message) {
        return std::string(name) + ": " + message;
    };
    require(result.statistics == result.copied_statistics,
            fail("host and GPU query results differ"));
    require(result.statistics.size() == views * report_words,
            fail("incorrect multiview query count"));
    std::array<uint64_t, 11> total{};
    for (uint32_t view = 0; view < views; ++view) {
        require(result.statistics[view * report_words + 11] == 1,
                fail("query unavailable for view " + std::to_string(view)));
        for (uint32_t counter = 0; counter < total.size(); ++counter)
            total[counter] += result.statistics[view * report_words + counter];
    }
    const uint64_t vertices = config.primitive_restart ? 4 : config.vertex_count;
    const bool geometry = !config.geometry_shader.empty();
    const uint64_t primitives = geometry ? vertices : config.primitive_restart ? 2
        : config.vertex_count > 1 ? config.vertex_count - 1 : 0;
    const bool enabled = (!config.indirect_count ||
                           (*config.indirect_count && config.max_indirect_count)) &&
        (!config.conditional_value ||
         ((*config.conditional_value != 0) != config.conditional_inverted));
    const uint32_t draws = std::max(1u, config.secondary_replays);
    const uint64_t instances = enabled ? uint64_t(config.instance_count) * views * draws : 0;
    const uint64_t gs_invocations = geometry ? primitives * instances : 0;
    const uint64_t gs_primitives = gs_invocations * 2;
    const uint64_t clipping = config.rasterizer_discard ? 0
        : geometry ? gs_primitives : primitives * instances;
    const std::array<uint64_t, 11> expected{
        vertices * instances, primitives * instances, vertices * instances,
        gs_invocations, gs_primitives, clipping, clipping, total[7], 0, 0, 0};
    for (uint32_t counter = 0; counter < total.size(); ++counter) {
        require(total[counter] == expected[counter],
                fail("counter " + std::to_string(counter) + " expected " +
                     std::to_string(expected[counter]) + " got " +
                     std::to_string(total[counter])));
    }
    if (geometry) {
        const uint64_t actual_invocations = gs_invocations / draws *
            (draws + (config.secondary_query_transitions ? 2 : 0));
        require(result.counters[0] == actual_invocations,
                fail("synthetic consumer duplicated application GS invocations"));
    }
    if (config.rasterizer_discard || !vertices || !instances)
        require(total[7] == 0, fail("inactive rasterization counted fragments"));
    else
        require(total[7] != 0, fail("active rasterization counted no fragments"));
    require(!result.pipeline_cache.empty(), fail("empty pipeline cache"));
}

} // namespace

int main(int argc, char **argv)
{
    try {
        require(argc <= 2, "expected at most one exact statistics test selector");
        const auto selected = [&](const std::string &name) {
            return argc != 2 || std::string_view(argv[1]) == name;
        };
        DrawContext context(false, true, true);
        require_venus_when_asked(context);
        require(context.capabilities().pipeline_statistics,
                "pipeline statistics feature missing");
        const std::array<uint32_t, 16> counters{};
        const std::array<uint32_t, 3> indices{0, 1, 2};
        const std::array<uint32_t, 5> restart_indices{0, 1, UINT32_MAX, 1, 2};
        unsigned cases = 0;
        for (uint32_t views : {0u, 0xau}) {
            for (bool indirect : {false, true}) {
                for (unsigned mode = 0; mode < 3; ++mode) {
                    for (unsigned indexed = 0; indexed < 3; ++indexed) {
                        DrawConfig config;
                        config.vertex_shader = line_multiview_vert_spirv;
                        config.fragment_shader = line_multiview_frag_spirv;
                        config.first_vertex = 5;
                        config.first_instance = 3;
                        config.counters = counters;
                        config.view_mask = views;
                        config.layers = views ? 4 : 1;
                        config.instance_count = 2;
                        config.indirect = indirect;
                        config.pipeline_statistics = 0x7ff;
                        if (mode) {
                            config.line_mode = VK_LINE_RASTERIZATION_MODE_RECTANGULAR_KHR;
                            config.provoking_vertex = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
                            config.stipple = mode == 2;
                        }
                        if (indexed == 1)
                            config.indices = indices;
                        if (indexed == 2) {
                            config.indices = restart_indices;
                            config.vertex_count = restart_indices.size();
                            config.primitive_restart = true;
                        }
                        const std::string name = "views=" + std::to_string(views) +
                            ",indirect=" + std::to_string(indirect) +
                            ",mode=" + std::to_string(mode) +
                            ",indexed=" + std::to_string(indexed);
                        if (!selected(name))
                            continue;
                        auto cold = context.draw(config);
                        verify(config, cold, name + ",cold");
                        DrawContext fresh;
                        auto cached = fresh.draw(config, cold.pipeline_cache);
                        verify(config, cached, name + ",cached");
                        ++cases;
                        std::printf("pipeline statistics %s passed\n", name.c_str());
                        std::fflush(stdout);
                    }
                }
            }
        }
        for (bool empty_vertices : {false, true}) {
            const std::string name = empty_vertices ? "empty vertices" : "empty instances";
            if (!selected(name))
                continue;
            std::printf("pipeline statistics starting %s\n", name.c_str());
            std::fflush(stdout);
            DrawConfig config;
            config.vertex_shader = line_multiview_vert_spirv;
            config.fragment_shader = line_multiview_frag_spirv;
            config.first_vertex = 5;
            config.first_instance = 3;
            config.counters = counters;
            config.pipeline_statistics = 0x7ff;
            config.view_mask = 0xa;
            config.layers = 4;
            if (empty_vertices)
                config.vertex_count = 0;
            else
                config.instance_count = 0;
            verify(config, context.draw(config), "empty multiview draw");
            ++cases;
        }
        // Generated line consumers must not add application GS invocations,
        // and XFB capacity cannot clamp the number of primitives emitted.
        for (bool indirect : {false, true}) {
            for (unsigned mode = 0; mode < 3; ++mode) {
                for (bool discard : {false, true}) {
                    DrawConfig config;
                    config.vertex_shader = gs_vertex;
                    config.geometry_shader = gs_unused_1;
                    config.fragment_shader = gs_fragment;
                    config.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
                    config.pipeline_statistics = 0x7ff;
                    config.counters = counters;
                    config.transform_feedback_words = 8; // Only one of six lines fits.
                    config.indirect = indirect;
                    config.rasterizer_discard = discard;
                    if (mode) {
                        config.line_mode = VK_LINE_RASTERIZATION_MODE_RECTANGULAR_KHR;
                        config.provoking_vertex = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
                        config.stipple = mode == 2;
                    }
                    const std::string name = "GS-XFB,indirect=" + std::to_string(indirect) +
                        ",mode=" + std::to_string(mode) + ",discard=" + std::to_string(discard);
                    if (!selected(name))
                        continue;
                    std::printf("pipeline statistics starting %s\n", name.c_str());
                    std::fflush(stdout);
                    const auto cold = context.draw(config);
                    verify(config, cold, name + ",cold");
                    DrawContext fresh;
                    verify(config, fresh.draw(config, cold.pipeline_cache), name + ",cached");
                    ++cases;
                }
            }
        }
        for (unsigned mode = 0; mode < 3; ++mode) {
            const std::string name = "feature-off cache,mode=" + std::to_string(mode);
            if (!selected(name))
                continue;
            DrawConfig config;
            config.vertex_shader = gs_vertex;
            config.geometry_shader = gs_unused_1;
            config.fragment_shader = gs_fragment;
            config.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            config.counters = counters;
            config.transform_feedback_words = 8;
            if (mode) {
                config.line_mode = VK_LINE_RASTERIZATION_MODE_RECTANGULAR_KHR;
                config.provoking_vertex = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
                config.stipple = mode == 2;
            }
            DrawContext feature_off(false, false);
            const auto uninstrumented = feature_off.draw(config);
            config.pipeline_statistics = 0x7ff;
            DrawContext feature_on;
            const auto instrumented = feature_on.draw(config, uninstrumented.pipeline_cache);
            verify(config, instrumented, name);
            DrawContext fresh;
            verify(config, fresh.draw(config, instrumented.pipeline_cache), name + ",cached");
            ++cases;
            std::printf("pipeline statistics %s passed\n", name.c_str());
            std::fflush(stdout);
        }
        for (bool indexed : {false, true}) {
            for (unsigned limit = 0; limit < 4; ++limit) {
                DrawConfig config;
                config.vertex_shader = line_multiview_vert_spirv;
                config.fragment_shader = line_multiview_frag_spirv;
                config.first_vertex = 5;
                config.first_instance = 3;
                config.counters = counters;
                config.pipeline_statistics = 0x7ff;
                config.view_mask = 0xa;
                config.layers = 4;
                config.indirect = true;
                config.indirect_count = limit == 0 ? 0 : limit == 1 ? 1 : UINT32_MAX;
                config.max_indirect_count = limit == 3 ? 0 : 1;
                config.line_mode = VK_LINE_RASTERIZATION_MODE_RECTANGULAR_KHR;
                config.provoking_vertex = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
                if (indexed) {
                    config.indices = restart_indices;
                    config.vertex_count = restart_indices.size();
                    config.primitive_restart = true;
                }
                const std::string name = "indirect-count,indexed=" + std::to_string(indexed) +
                    ",limit=" + std::to_string(limit);
                if (!selected(name))
                    continue;
                verify(config, context.draw(config), name);
                ++cases;
                std::printf("pipeline statistics %s passed\n", name.c_str());
                std::fflush(stdout);
            }
        }
        for (bool geometry : {false, true}) {
            for (unsigned indirect = 0; indirect < 3; ++indirect) {
                for (uint32_t value : {0u, 1u}) {
                    for (bool inverted : {false, true}) {
                        DrawConfig config;
                        config.vertex_shader = line_multiview_vert_spirv;
                        config.fragment_shader = line_multiview_frag_spirv;
                        if (geometry) {
                            config.vertex_shader = gs_vertex;
                            config.fragment_shader = gs_fragment;
                            config.geometry_shader = gs_unused_1;
                            config.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
                            config.transform_feedback_words = 8;
                        } else {
                            config.first_vertex = 5;
                            config.first_instance = 3;
                            config.view_mask = 0xa;
                            config.layers = 4;
                            config.indices = restart_indices;
                            config.vertex_count = restart_indices.size();
                            config.primitive_restart = true;
                        }
                        config.line_mode = VK_LINE_RASTERIZATION_MODE_RECTANGULAR_KHR;
                        config.provoking_vertex = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
                        config.counters = counters;
                        config.pipeline_statistics = 0x7ff;
                        config.indirect = indirect != 0;
                        if (indirect == 2)
                            config.indirect_count = 1;
                        config.conditional_value = value;
                        config.conditional_inverted = inverted;
                        const std::string name = "conditional,GS=" + std::to_string(geometry) +
                            ",indirect=" + std::to_string(indirect) +
                            ",value=" + std::to_string(value) +
                            ",inverted=" + std::to_string(inverted);
                        if (!selected(name))
                            continue;
                        verify(config, context.draw(config), name);
                        ++cases;
                        std::printf("pipeline statistics %s passed\n", name.c_str());
                        std::fflush(stdout);
                    }
                }
            }
        }
        for (bool geometry : {false, true}) {
            for (bool software : {false, true}) {
                DrawConfig config;
                config.vertex_shader = line_multiview_vert_spirv;
                config.fragment_shader = line_multiview_frag_spirv;
                config.first_vertex = 5;
                config.first_instance = 3;
                config.view_mask = 0xa;
                config.layers = 4;
                config.counters = counters;
                config.pipeline_statistics = 0x7ff;
                config.secondary_replays = 2;
                config.secondary_query_transitions = true;
                if (geometry) {
                    config.vertex_shader = gs_vertex;
                    config.geometry_shader = gs_unused_1;
                    config.fragment_shader = gs_fragment;
                    config.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
                    config.first_vertex = 0;
                    config.first_instance = 0;
                    config.view_mask = 0;
                    config.layers = 1;
                    config.transform_feedback_words = 8;
                }
                if (software) {
                    config.line_mode = VK_LINE_RASTERIZATION_MODE_RECTANGULAR_KHR;
                    config.provoking_vertex = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
                    config.stipple = true;
                }
                const std::string name = "secondary replay,GS=" + std::to_string(geometry) +
                    ",software=" + std::to_string(software);
                if (!selected(name))
                    continue;
                const auto cold = context.draw(config);
                verify(config, cold, name);
                DrawContext fresh;
                verify(config, fresh.draw(config, cold.pipeline_cache), name + ",cached");
                ++cases;
                std::printf("pipeline statistics %s passed\n", name.c_str());
                std::fflush(stdout);
            }
        }
        require(cases != 0, "no matching statistics test case");
        std::printf("UML_VULKAN_PROBE_RESULT: %u pipeline statistics cases passed\n", cases);
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "pipeline statistics: %s\n", error.what());
        return EXIT_FAILURE;
    }
}
