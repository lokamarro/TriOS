// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan_draw_test.hpp"
#include "shaders/geometry_side_effects_spirv.hpp"
#include "shaders/legacy_vertex_input_spirv.hpp"
#include "shaders/line_multiview_spirv.hpp"
#include "require.hpp"

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
using uml::gpu::test::DrawExtensions;
using uml::gpu::test::DrawResult;
using uml::gpu::test::VertexInputDraw;

void verify(const DrawConfig &config, const DrawResult &result,
            const std::string &name)
{
    constexpr uint32_t words = 12;
    const uint32_t views = std::max(1, std::popcount(config.view_mask));
    require(result.statistics == result.copied_statistics,
            name + ": host and copied query results differ");
    require(result.statistics.size() == words * (views + 1),
            name + ": incorrect query count");
    std::array<uint64_t, 11> total{};
    for (uint32_t query = 0; query <= views; ++query) {
        require(result.statistics[query * words + 11] == 1,
                name + ": query unavailable " + std::to_string(query));
        for (uint32_t counter = 0; counter < total.size(); ++counter) {
            const auto value = result.statistics[query * words + counter];
            if (query == views)
                require(value == 0, name + ": post-render query is not empty");
            else
                total[counter] += value;
        }
    }
    const bool enabled = (!config.indirect_count ||
                           (*config.indirect_count && config.max_indirect_count)) &&
        (!config.conditional_value ||
         ((*config.conditional_value != 0) != config.conditional_inverted));
    const uint64_t instances = enabled ? uint64_t(config.instance_count) * views *
        std::max(1u, config.secondary_replays) *
        std::max(size_t(1), config.vertex_input_draws.size()) : 0;
    const bool geometry = !config.geometry_shader.empty();
    const uint64_t vertices = config.primitive_restart ? 4 : config.vertex_count;
    const uint64_t primitives = geometry ? vertices : config.primitive_restart ? 2 :
        config.topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ? vertices / 2 :
        vertices > 1 ? vertices - 1 : 0;
    require(total[0] == vertices * instances, name + ": IA vertex count");
    require(total[1] == primitives * instances, name + ": IA primitive count");
    require(total[2] == vertices * instances, name + ": VS invocation count");
    require(total[3] == (geometry ? vertices * instances : 0),
            name + ": GS invocation count");
    require(total[4] == (geometry ? vertices * instances * 2 : 0),
            name + ": GS primitive count");
    const uint64_t clipping = config.rasterizer_discard ? 0 :
        geometry ? vertices * instances * 2 : primitives * instances;
    require(total[5] == clipping && total[6] == clipping,
            name + ": clipping counters include synthetic expansion");
    require(total[8] == 0 && total[9] == 0 && total[10] == 0,
            name + ": inactive stage counted invocations");
    require(!result.pipeline_cache.empty(), name + ": empty pipeline cache");
}

} // namespace

int main(int argc, char **argv)
{
    try {
        require(argc <= 2, "expected at most one exact test selector");
        constexpr DrawExtensions extensions{true, true, true};
        DrawContext context(true, true, true, extensions);
        require_venus_when_asked(context);
        const auto &properties = context.capabilities().maintenance10;
        require(!properties.rgba4OpaqueBlackSwizzled &&
                    properties.resolveSrgbFormatAppliesTransferFunction &&
                    properties.resolveSrgbFormatSupportsTransferFunctionControl,
                "maintenance10 properties do not match the native provider");
        const std::array<uint32_t, 16> counters{};
        const std::array<uint32_t, 3> indices{0, 1, 2};
        const std::array<uint32_t, 5> restart{0, 1, UINT32_MAX, 1, 2};
        unsigned cases = 0;
        const auto run = [&](DrawConfig config, const std::string &name) {
            if (argc == 2 && std::string_view(argv[1]) != name)
                return;
            config.pipeline_statistics = 0x7ff;
            config.query_after_rendering = true;
            const auto ordinary = context.draw(config);
            verify(config, ordinary, name + ",buffers");
            config.device_address_commands = true;
            config.end_rendering2 = true;
            DrawContext fresh(true, true, true, extensions);
            const auto address = fresh.draw(config, ordinary.pipeline_cache);
            verify(config, address, name + ",addresses");
            require(ordinary.pixels == address.pixels, name + ": rendered bytes differ");
            require(ordinary.counters == address.counters, name + ": shader counters differ");
            require(ordinary.transform_feedback == address.transform_feedback,
                    name + ": transform-feedback bytes differ");
            require(ordinary.statistics == address.statistics,
                    name + ": pipeline statistics differ");
            ++cases;
            std::printf("PASS device address %s\n", name.c_str());
            std::fflush(stdout);
        };
        const auto line = [&](uint32_t views, unsigned mode) {
            DrawConfig config;
            config.vertex_shader = line_multiview_vert_spirv;
            config.fragment_shader = line_multiview_frag_spirv;
            config.first_vertex = 5;
            config.first_instance = 3;
            config.counters = counters;
            config.instance_count = 2;
            config.view_mask = views;
            config.layers = views ? 4 : 1;
            if (mode) {
                config.line_mode = VK_LINE_RASTERIZATION_MODE_RECTANGULAR_KHR;
                config.provoking_vertex = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
                config.stipple = mode == 2;
            }
            return config;
        };
        for (uint32_t views : {0u, 0xau}) {
            for (unsigned mode = 0; mode < 3; ++mode) {
                for (unsigned kind = 0; kind < 6; ++kind) {
                    for (bool secondary : {false, true}) {
                        auto config = line(views, mode);
                        config.indirect = kind % 3 != 0;
                        if (kind >= 3)
                            config.indices = indices;
                        if (kind % 3 == 2)
                            config.indirect_count = 1;
                        config.secondary_replays = secondary ? 2 : 0;
                        config.secondary_query_transitions = secondary;
                        run(config, "views=" + std::to_string(views) + ",mode=" +
                            std::to_string(mode) + ",kind=" + std::to_string(kind) +
                            ",secondary=" + std::to_string(secondary));
                    }
                }
            }
            for (bool indexed : {false, true}) {
                for (unsigned edge = 0; edge < 3; ++edge) {
                    auto config = line(views, 1);
                    config.indirect = true;
                    config.indirect_count = edge == 0 ? 0 : edge == 1 ? UINT32_MAX : 1;
                    config.max_indirect_count = edge == 2 ? 0 : 1;
                    if (indexed)
                        config.indices = indices;
                    run(config, "count,views=" + std::to_string(views) + ",indexed=" +
                        std::to_string(indexed) + ",edge=" + std::to_string(edge));
                }
            }
            for (unsigned condition = 0; condition < 4; ++condition) {
                auto config = line(views, 2);
                config.indirect = true;
                config.indices = restart;
                config.vertex_count = restart.size();
                config.primitive_restart = true;
                config.conditional_value = condition & 1u;
                config.conditional_inverted = condition & 2u;
                run(config, "conditional,views=" + std::to_string(views) +
                    ",condition=" + std::to_string(condition));
            }
            for (unsigned mode = 0; mode < 3; ++mode) {
                for (bool secondary : {false, true}) {
                    if (views && !context.capabilities().multiview_geometry_shader)
                        continue;
                    auto config = line(views, mode);
                    config.vertex_shader = gs_vertex;
                    config.geometry_shader = gs_unused_1;
                    config.fragment_shader = gs_fragment;
                    config.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
                    config.first_vertex = 0;
                    config.first_instance = 0;
                    config.instance_count = 1;
                    config.transform_feedback_words = 8;
                    config.indirect = true;
                    config.secondary_replays = secondary ? 2 : 0;
                    config.secondary_query_transitions = secondary;
                    run(config, "GS-XFB,views=" + std::to_string(views) + ",mode=" +
                        std::to_string(mode) + ",secondary=" + std::to_string(secondary));
                }
            }
        }
        std::array<uint8_t, 32> bytes;
        bytes.fill(0xff);
        const std::array<VertexInputDraw, 2> vertex_draws{{
            {VK_FORMAT_R8_UNORM, 3, 1, 1, VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT, false},
            {VK_FORMAT_R16_UINT, 5, 2, 1, VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT, true},
        }};
        for (bool indexed : {false, true}) {
            DrawConfig config;
            config.vertex_shader = legacy_capture;
            config.fragment_shader = legacy_fragment;
            config.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            config.line_mode = VK_LINE_RASTERIZATION_MODE_BRESENHAM_KHR;
            config.vertex_count = 2;
            config.vertex_data = bytes;
            config.vertex_input_draws = vertex_draws;
            config.transform_feedback_words = 8;
            config.indirect = true;
            if (indexed)
                config.indices = std::span(indices).first(2);
            run(config, "vertex-bind,indexed=" + std::to_string(indexed));
        }
        require(cases != 0, "unknown device address test selector");
        std::printf("PASS device address and maintenance commands: %u cases\n", cases);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL device address commands: %s\n", error.what());
        return 1;
    }
}
