// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan_draw_test.hpp"
#include "shaders/raster_polygon_spirv.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using uml::testing::require;
using namespace uml::gpu::test;

void equal(const DrawResult &a, const DrawResult &b)
{
    if (a.pixels != b.pixels) {
        for (unsigned pixel = 0; pixel < a.pixels.size(); ++pixel)
            if (a.pixels[pixel] != b.pixels[pixel])
                throw std::runtime_error("dynamic raster color/coverage at pixel " +
                    std::to_string(pixel) + ": expected R/A " +
                    std::to_string(a.pixels[pixel][0]) + "/" + std::to_string(a.pixels[pixel][3]) +
                    " got " + std::to_string(b.pixels[pixel][0]) + "/" +
                    std::to_string(b.pixels[pixel][3]));
    }
    require(a.stencil == b.stencil, "dynamic raster class changed front/back stencil");
    require(a.transform_feedback == b.transform_feedback,
            "dynamic raster class changed pre-clipping transform feedback");
}

void depth_bias_probe(DrawContext &context)
{
    unsigned cases = 0;
    const std::array ranges{std::array{0.0f, 1.0f}, std::array{0.2f, 0.8f},
                            std::array{0.8f, 0.2f}, std::array{0.4f, 0.4f}};
    for (auto format : {VK_FORMAT_D16_UNORM, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
    for (unsigned stage = 0; stage < 5; ++stage) {
        for (unsigned raster = 0; raster < 4; ++raster) {
            for (const auto &range : ranges) {
                for (bool clamp : {false, true}) {
                    DrawConfig config;
                    config.vertex_shader = stage == 4 ? std::span<const uint32_t>(polygon_capture_6)
                                                       : std::span<const uint32_t>(polygon_vertex_6);
                    config.fragment_shader = polygon_early_facing;
                    config.topology = stage == 2 || stage == 3 ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST
                                                               : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                    if (stage == 1 || stage == 3)
                        config.geometry_shader = polygon_geometry_6;
                    if (stage == 2 || stage == 3) {
                        config.tess_control_shader = polygon_control_6;
                        config.tess_eval_shader = polygon_evaluation_6;
                    }
                    config.transform_feedback_words = stage == 4 ? 16 : 0;
                    config.polygon_mode = raster == 0 ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_LINE;
                    const VkLineRasterizationModeKHR modes[] = {
                        VK_LINE_RASTERIZATION_MODE_DEFAULT_KHR, VK_LINE_RASTERIZATION_MODE_BRESENHAM_KHR,
                        VK_LINE_RASTERIZATION_MODE_RECTANGULAR_KHR, VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_KHR};
                    config.line_mode = modes[raster];
                    config.provoking_vertex = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
                    config.depth = true;
                    config.depth_format = format;
                    config.depth_clamp = clamp;
                    config.min_depth = range[0];
                    config.max_depth = range[1];
                    const auto reference = context.draw(config);
                    const float slope = std::abs(range[1] - range[0]) / 128.0f;
                    const float maximum = std::max(range[0] + (range[1] - range[0]) * 0.25f,
                                                   range[0] + (range[1] - range[0]) * 0.5f);
                    const float r = format == VK_FORMAT_D16_UNORM ? 0x1p-16f
                        : std::exp2(std::floor(std::log2(maximum)) - 23);
                    for (unsigned bias = 0; bias < 4; ++bias) {
                        config.depth_bias = true;
                        config.depth_bias_constant = bias & 1 ? -1024 : 1024;
                        config.depth_bias_slope = bias & 1 ? -2 : 2;
                        config.depth_bias_clamp = bias & 2 ? (bias & 1 ? -0.003f : 0.003f) : 0;
                        const auto result = context.draw(config);
                        equal(reference, result);
                        float offset = slope * config.depth_bias_slope + r * config.depth_bias_constant;
                        if (config.depth_bias_clamp > 0)
                            offset = std::min(offset, config.depth_bias_clamp);
                        if (config.depth_bias_clamp < 0)
                            offset = std::max(offset, config.depth_bias_clamp);
                        unsigned checked = 0;
                        for (unsigned pixel = 0; pixel < reference.depth.size(); ++pixel) {
                            if (!reference.pixels[pixel][3])
                                continue;
                            float expected = reference.depth[pixel] + offset;
                            if (clamp)
                                expected = std::clamp(expected, std::min(range[0], range[1]),
                                                       std::max(range[0], range[1]));
                            const float tolerance = format == VK_FORMAT_D16_UNORM ? 2.0f / 65535 : 2e-6f;
                            if (std::abs(result.depth[pixel] - expected) > tolerance)
                                throw std::runtime_error("source polygon bias stage=" + std::to_string(stage) +
                                    " raster=" + std::to_string(raster) + " pixel=" + std::to_string(pixel) +
                                    " range=" + std::to_string(range[0]) + "," + std::to_string(range[1]) +
                                    " expected=" + std::to_string(expected) +
                                    " got=" + std::to_string(result.depth[pixel]));
                            ++checked;
                        }
                        require(checked != 0, "depth bias case covered no fragments");
                        ++cases;
                    }
                }
            }
        }
    }
    }
    std::printf("Raster depth bias: %u cases / %u draws passed\n", cases, cases * 5 / 4);
}
} // namespace

int main(int argc, char **argv)
{
    std::string current;
    try {
        require(argc <= 2, "expected at most one exact stage selector");
        DrawContext context;
        require_venus_when_asked(context);
        if (argc == 1 || std::string_view(argv[1]) == "depth") {
            depth_bias_probe(context);
            if (argc == 2)
                return 0;
        }
        const char *stages[] = {"vs", "gs", "tes", "tes-gs", "xfb"};
        std::array states{
            RasterState{VK_POLYGON_MODE_FILL, VK_LINE_RASTERIZATION_MODE_DEFAULT_KHR,
                        VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT, false, true},
            RasterState{VK_POLYGON_MODE_FILL, VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_KHR,
                        VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT, true, false},
            RasterState{VK_POLYGON_MODE_POINT, VK_LINE_RASTERIZATION_MODE_DEFAULT_KHR,
                        VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT, true, true},
            RasterState{VK_POLYGON_MODE_POINT, VK_LINE_RASTERIZATION_MODE_RECTANGULAR_KHR,
                        VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT, true, false},
            RasterState{VK_POLYGON_MODE_LINE, VK_LINE_RASTERIZATION_MODE_DEFAULT_KHR,
                        VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT, false, true},
            RasterState{VK_POLYGON_MODE_LINE, VK_LINE_RASTERIZATION_MODE_BRESENHAM_KHR,
                        VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT, false, false},
            RasterState{VK_POLYGON_MODE_LINE, VK_LINE_RASTERIZATION_MODE_RECTANGULAR_KHR,
                        VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT, true, true},
            RasterState{VK_POLYGON_MODE_LINE, VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_KHR,
                        VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT, true, false}};
        const std::array<uint32_t, 3> indices{0, 1, 2};
        unsigned cases = 0;
        unsigned draws = 0;
        for (unsigned stage = 0; stage < std::size(stages); ++stage) {
            if (argc == 2 && std::string_view(argv[1]) != stages[stage])
                continue;
            for (bool negative : {false, true}) {
                std::vector<uint8_t> cache;
                for (unsigned state = 0; state < states.size(); ++state) {
                    const RasterState &live = states[state];
                    DrawConfig config;
                    config.vertex_shader = stage == 4 ? std::span<const uint32_t>(polygon_capture_2)
                                                       : std::span<const uint32_t>(polygon_vertex_2);
                    config.fragment_shader = polygon_early_facing;
                    config.topology = stage == 2 || stage == 3 ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST
                                                               : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                    if (stage == 1 || stage == 3)
                        config.geometry_shader = polygon_geometry_2;
                    if (stage == 2 || stage == 3) {
                        config.tess_control_shader = polygon_control_2;
                        config.tess_eval_shader = polygon_evaluation_2;
                    }
                    config.transform_feedback_words = stage == 4 ? 16 : 0;
                    config.stencil = true;
                    config.negative_viewport = negative;
                    config.indices = negative ? std::span(indices) : std::span<const uint32_t>();
                    config.indirect = negative;
                    if (negative)
                        config.pipeline_statistics = VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT |
                            VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
                            VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
                            VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT |
                            VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT |
                            VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
                            VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
                            VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_CONTROL_SHADER_PATCHES_BIT |
                            VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_EVALUATION_SHADER_INVOCATIONS_BIT;
                    config.query_restoration = negative;
                    config.polygon_mode = live.polygon_mode;
                    config.line_mode = live.line_mode;
                    config.provoking_vertex = live.provoking_vertex;
                    config.depth_clip = live.depth_clip;
                    config.stipple = live.stipple;
                    config.stipple_pattern = 0x5555;
                    current = std::string(stages[stage]) + "-v" + std::to_string(negative) +
                              "-state" + std::to_string(state);
                    std::printf("Raster state %s\n", current.c_str());
                    std::fflush(stdout);
                    const auto reference = context.draw(config);
                    config.dynamic_raster = true;
                    config.pipeline_library = negative;
                    config.ignored_raster_create = states[(state + 3) % states.size()];
                    config.fail_on_compile_required = !cache.empty();
                    const auto direct_dynamic = context.draw(config, cache);
                    equal(reference, direct_dynamic);
                    require(reference.statistics == direct_dynamic.statistics,
                            "initial dynamic raster class changed statistics");
                    cache = direct_dynamic.pipeline_cache;
                    config.raster_warmup = states;
                    config.fail_on_compile_required = true;
                    const auto dynamic = context.draw(config, cache);
                    equal(reference, dynamic);
                    require(dynamic.statistics.size() == reference.statistics.size(),
                            "query result layout changed across raster classes");
                    for (unsigned statistic = 0; statistic + 1 < reference.statistics.size(); ++statistic)
                        require(dynamic.statistics[statistic] ==
                                    reference.statistics[statistic] * (states.size() + 1),
                                "generated raster work changed logical pipeline statistics");
                    require(dynamic.statistics == dynamic.copied_statistics,
                            "dynamic raster host and GPU query copies differ");
                    cache = dynamic.pipeline_cache;
                    DrawContext fresh;
                    config.ignored_raster_create = states[(state + 5) % states.size()];
                    config.fail_on_compile_required = true;
                    const auto reloaded = fresh.draw(config, cache);
                    equal(reference, reloaded);
                    require(dynamic.statistics == reloaded.statistics,
                            "fresh-device raster/query cache changed statistics");
                    ++cases;
                    draws += 2 + 2 * (unsigned(states.size()) + 1) + (negative ? 8 : 0);
                }
            }
        }
        require(cases != 0, "stage selector matched no cases");
        std::printf("Raster state probe: %u cases / %u draws passed\n", cases,
                    draws);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Raster state probe failed %s: %s\n", current.c_str(), error.what());
        return 1;
    }
}
