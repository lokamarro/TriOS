// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan_draw_test.hpp"
#include "vulkan_draw_performance.hpp"
#include "shaders/geometry_side_effects_spirv.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
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

void verify(const DrawResult &result, unsigned shape, bool capture,
            uint32_t primitives)
{
    if (result.counters.size() != 2 + primitives ||
        result.counters[0] != primitives || result.counters[1] != 7 * primitives) {
        std::fprintf(stderr, "GS counter words:");
        for (const uint32_t counter : result.counters)
            std::fprintf(stderr, " %u", counter);
        std::fprintf(stderr, "\n");
    }
    require(result.counters.size() == 2 + primitives, "counter buffer size");
    require(result.counters[0] == primitives && result.counters[1] == 7 * primitives,
            "GS side effects: expected counters " + std::to_string(primitives) + "," +
                std::to_string(7 * primitives) + ", got " +
                std::to_string(result.counters[0]) + "," +
                std::to_string(result.counters[1]));
    for (uint32_t primitive = 0; primitive < primitives; ++primitive)
        require(result.counters[2 + primitive] == 1,
                "GS invocation count for primitive " + std::to_string(primitive));
    if (shape == 2 || primitives == 0) {
        require(std::all_of(result.pixels.begin(), result.pixels.end(),
                           [](const auto &pixel) { return pixel == std::array<uint8_t, 4>{}; }),
                "empty GS output unexpectedly rasterized pixels");
    } else {
        for (uint32_t x : {12u, 28u, 36u}) {
            require(result.pixels[16 * 64 + x] == std::array<uint8_t, 4>{255, 0, 0, 255},
                    "first GS line is missing or has incorrect attributes");
            const std::array<uint8_t, 4> second = shape == 0
                ? std::array<uint8_t, 4>{0, 255, 0, 255} : std::array<uint8_t, 4>{};
            require(result.pixels[40 * 64 + x] == second,
                    "second GS line is missing or EndPrimitive was lost");
        }
    }
    if (!capture)
        return;
    // XFB records independent line primitives, duplicating the middle vertex
    // of a three-vertex strip. Include one guard record beyond the active data.
    std::vector<uint32_t> expected(primitives * 16 + 4);
    if (shape != 2) {
        for (uint32_t primitive = 0; primitive < primitives; ++primitive) {
            for (uint32_t vertex = 0; vertex < 4; ++vertex) {
                const uint32_t offset = primitive * 16 + vertex * 4;
                expected[offset] = shape == 0 ? vertex : (vertex + 1) / 2;
                expected[offset + 1] = shape == 0 ? vertex / 2 : 0;
                expected[offset + 3] = 123 + primitive;
            }
        }
    }
    require(result.transform_feedback.size() == expected.size(), "XFB buffer size");
    for (size_t word = 0; word < expected.size(); ++word) {
        require(result.transform_feedback[word] == expected[word],
                "XFB word " + std::to_string(word) + ": expected " +
                    std::to_string(expected[word]) + ", got " +
                    std::to_string(result.transform_feedback[word]));
    }
}

void measure_side_effects(DrawContext &context, bool whole_submit)
{
    using namespace uml::gpu::test;
    DrawPerformanceSession session(context, whole_submit);
    print_draw_performance_device(context);
    for (uint32_t primitives : {1u, 64u, 1024u}) {
        for (unsigned shape : {0u, 2u}) {
            for (bool capture : {false, true}) {
                for (bool indirect : {false, true}) {
                    DrawConfig config;
                    config.vertex_shader = gs_vertex;
                    config.geometry_shader = shape == 0
                        ? (capture ? std::span<const uint32_t>(gs_two_strips_1)
                                   : std::span<const uint32_t>(gs_two_strips_0))
                        : (capture ? std::span<const uint32_t>(gs_no_output_1)
                                   : std::span<const uint32_t>(gs_no_output_0));
                    config.fragment_shader = gs_fragment;
                    config.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
                    config.vertex_count = primitives;
                    config.indirect = indirect;
                    config.timestamp = true;
                    // Fresh counters keep atomic-dependent emission identical
                    // in every warmup and measured draw. No-output is a control
                    // for the same application side effects without expansion.
                    const std::vector<uint32_t> counters(2 + primitives);
                    config.counters = counters;
                    config.transform_feedback_words = capture ? primitives * 16 + 4 : 0;
                    const auto timing = measure_draw_performance(context, config,
                        [&](const DrawResult &result) { verify(result, shape, capture, primitives); }, session);
                    std::printf("GS performance source=%s cpu_scope=instrumented-recording workload=%s primitives=%u capture=%u draw=%s gpu_median_ns=%.1f record_median_ns=%.1f gpu_ns_per_primitive=%.3f repeats=%u samples=%u\n",
                                session.source(), shape == 0 ? "atomic-emission" : "no-output",
                                primitives, unsigned(capture), indirect ? "indirect" : "direct",
                                timing.gpu_nanoseconds, timing.record_nanoseconds,
                                timing.gpu_nanoseconds / primitives,
                                config.repeat_count, draw_performance_samples);
                    std::fflush(stdout);
                }
            }
        }
    }
    session.finish();
}

} // namespace

int main(int argc, char **argv)
{
    std::string current;
    try {
        require(argc <= 2, "expected at most one exact GS test selector");
        DrawContext context;
        const auto &capabilities = context.capabilities();
        require_venus_when_asked(context);
        require(capabilities.geometry_shader, "geometryShader unavailable");
        require(capabilities.transform_feedback, "transformFeedback unavailable");
        if (argc == 2 && (std::string_view(argv[1]) == "performance" ||
                         std::string_view(argv[1]) == "performance-whole-submit")) {
            measure_side_effects(context, std::string_view(argv[1]) == "performance-whole-submit");
            return 0;
        }
        const std::array<uint32_t, 5> counters{};
        const std::array<uint32_t, 3> indices{0, 1, 2};
        struct ShaderCase {
            const char *name;
            unsigned shape;
            std::span<const uint32_t> plain;
            std::span<const uint32_t> capture;
        };
        const ShaderCase shaders[] = {
            {"two-strips", 0, gs_two_strips_0, gs_two_strips_1},
            {"one-strip", 1, gs_one_strip_0, gs_one_strip_1},
            {"no-output", 2, gs_no_output_0, gs_no_output_1},
            {"unused-result", 0, gs_unused_0, gs_unused_1},
        };
        size_t completed = 0;
        for (const auto &shader : shaders) {
            for (bool capture : {false, true}) {
                for (unsigned mode = 0; mode < 3; ++mode) {
                    for (unsigned draw = 0; draw < 8; ++draw) {
                        const uint32_t primitives = draw < 4 ? 1 : 3;
                        current = std::string(shader.name) + (capture ? "-xfb" : "-plain") +
                            "-mode" + std::to_string(mode) + "-draw" + std::to_string(draw % 4) +
                            (primitives == 3 ? "-three-primitives" : "");
                        if (argc == 2 && std::string_view(argv[1]) != current)
                            continue;
                        DrawConfig config;
                        config.vertex_shader = gs_vertex;
                        config.geometry_shader = capture ? shader.capture : shader.plain;
                        config.fragment_shader = gs_fragment;
                        config.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
                        config.vertex_count = primitives;
                        config.counters = std::span(counters).first(2 + primitives);
                        config.transform_feedback_words = capture ? primitives * 16 + 4 : 0;
                        config.provoking_vertex = mode == 1
                            ? VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT
                            : VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT;
                        config.line_mode = mode == 2
                            ? VK_LINE_RASTERIZATION_MODE_RECTANGULAR_KHR
                            : VK_LINE_RASTERIZATION_MODE_DEFAULT_KHR;
                        config.stipple = mode == 2;
                        config.stipple_pattern = 0xffff;
                        config.indirect = draw & 1;
                        config.indices = draw & 2 ? std::span(indices).first(primitives)
                                                  : std::span<const uint32_t>();
                        std::printf("GS probe starting %s\n", current.c_str());
                        std::fflush(stdout);
                        const auto cold = context.draw(config);
                        verify(cold, shader.shape, capture, primitives);
                        DrawContext cache_context;
                        verify(cache_context.draw(config, cold.pipeline_cache), shader.shape, capture,
                               primitives);
                        std::printf("GS probe PASS %s (cold and reloaded cache)\n", current.c_str());
                        ++completed;
                    }
                }
            }
        }
        // Exercise empty, complete and partial workgroups independently of the
        // opt-in performance path, including a fresh device loading the cache.
        for (const auto &shader : shaders) {
            for (uint32_t primitives : {0u, 63u, 64u, 65u}) {
                const std::vector<uint32_t> large_counters(2 + primitives);
                // A real index binding also selects the indexed API when the
                // index count is zero; the sentinel must never be consumed.
                std::vector<uint32_t> large_indices(std::max(primitives, 1u));
                for (uint32_t i = 0; i < primitives; ++i)
                    large_indices[i] = i;
                for (unsigned draw = 0; draw < 4; ++draw) {
                    current = std::string(shader.name) + "-xfb-large" +
                        std::to_string(primitives) + "-draw" + std::to_string(draw);
                    if (argc == 2 && std::string_view(argv[1]) != current)
                        continue;
                    DrawConfig config;
                    config.vertex_shader = gs_vertex;
                    config.geometry_shader = shader.capture;
                    config.fragment_shader = gs_fragment;
                    config.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
                    config.vertex_count = primitives;
                    config.counters = large_counters;
                    config.transform_feedback_words = primitives * 16 + 4;
                    config.indirect = draw & 1;
                    config.indices = draw & 2 ? std::span<const uint32_t>(large_indices)
                                              : std::span<const uint32_t>();
                    std::printf("GS probe starting %s\n", current.c_str());
                    std::fflush(stdout);
                    const auto cold = context.draw(config);
                    verify(cold, shader.shape, true, primitives);
                    DrawContext cache_context;
                    verify(cache_context.draw(config, cold.pipeline_cache),
                           shader.shape, true, primitives);
                    std::printf("GS probe PASS %s (cold and reloaded cache)\n", current.c_str());
                    ++completed;
                }
            }
        }
        require(completed != 0, "no matching GS test case");
        std::printf("GS probe PASS: %zu cases\n", completed);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "GS probe FAIL %s: %s\n", current.c_str(), error.what());
        return 1;
    }
}
