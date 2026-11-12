// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan_draw_test.hpp"
#include "vulkan_draw_performance.hpp"
#include "shaders/geometry_invocations_spirv.hpp"
#include "require.hpp"

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

void verify(const DrawResult &result, uint32_t invocations, uint32_t primitives)
{
    require(result.counters.size() == 1 + primitives * invocations, "counter size");
    require(result.counters[0] == primitives * invocations, "total invocation count");
    for (size_t slot = 1; slot < result.counters.size(); ++slot)
        require(result.counters[slot] == 1, "per-invocation side effect count");

    for (uint32_t invocation = 0; invocation < invocations; ++invocation) {
        const uint32_t row = 8 + 12 * invocation;
        require(result.pixels[row * 64 + 16] == std::array<uint8_t, 4>{255, 0, 0, 255},
                "stream-zero raster output missing for invocation " +
                    std::to_string(invocation));
    }

    // Each invocation emits one two-vertex line to stream one. Its implicit
    // termination must prevent a third line connecting adjacent invocations.
    std::vector<uint32_t> expected(result.transform_feedback.size());
    size_t offset = 0;
    for (uint32_t primitive = 0; primitive < primitives; ++primitive) {
        for (uint32_t invocation = 0; invocation < invocations; ++invocation) {
            for (uint32_t vertex = 0; vertex < 2; ++vertex) {
                expected[offset++] = primitive;
                expected[offset++] = invocation;
                expected[offset++] = vertex;
                expected[offset++] = 0x51;
            }
        }
    }
    for (size_t word = 0; word < expected.size(); ++word) {
        require(result.transform_feedback[word] == expected[word],
                "stream-one XFB word " + std::to_string(word) + ": expected " +
                    std::to_string(expected[word]) + ", got " +
                    std::to_string(result.transform_feedback[word]));
    }
}

void measure_invocations(DrawContext &context, bool whole_submit)
{
    using namespace uml::gpu::test;
    DrawPerformanceSession session(context, whole_submit);
    print_draw_performance_device(context);
    for (uint32_t primitives : {1u, 64u, 1024u}) {
        for (uint32_t invocations : {2u, 4u}) {
            for (bool indirect : {false, true}) {
                DrawConfig config;
                config.vertex_shader = gi_vertex;
                config.geometry_shader = invocations == 2
                    ? std::span<const uint32_t>(gi_i2_end1)
                    : std::span<const uint32_t>(gi_i4_end1);
                config.fragment_shader = gi_fragment;
                config.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
                config.vertex_count = primitives;
                config.indirect = indirect;
                config.timestamp = true;
                // The atomic-return guard requires fresh counters for every
                // draw, so scale the dispatch rather than repeating it.
                const std::vector<uint32_t> counters(1 + primitives * invocations);
                config.counters = counters;
                config.transform_feedback_words = primitives * invocations * 16 + 16;
                const auto timing = measure_draw_performance(context, config,
                    [&](const DrawResult &result) { verify(result, invocations, primitives); }, session);
                std::printf("GS performance source=%s cpu_scope=instrumented-recording workload=invocation-streams primitives=%u invocations=%u draw=%s gpu_median_ns=%.1f record_median_ns=%.1f gpu_ns_per_invocation=%.3f repeats=%u samples=%u\n",
                            session.source(), primitives, invocations, indirect ? "indirect" : "direct",
                            timing.gpu_nanoseconds, timing.record_nanoseconds,
                            timing.gpu_nanoseconds / (primitives * invocations),
                            config.repeat_count, draw_performance_samples);
                std::fflush(stdout);
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
        require(argc <= 2, "expected at most one exact test selector");
        DrawContext context;
        const auto &capabilities = context.capabilities();
        require_venus_when_asked(context);
        require(capabilities.geometry_shader && capabilities.transform_feedback &&
                    capabilities.geometry_streams &&
                    capabilities.transform_feedback_streams_lines_triangles,
                "geometry streams with transform-feedback lines unavailable");
        if (argc == 2 && (std::string_view(argv[1]) == "performance" ||
                         std::string_view(argv[1]) == "performance-whole-submit")) {
            measure_invocations(context, std::string_view(argv[1]) == "performance-whole-submit");
            return 0;
        }
        struct ShaderCase {
            uint32_t invocations;
            bool explicit_end;
            std::span<const uint32_t> shader;
        };
        const ShaderCase shaders[] = {
            {2, true, gi_i2_end1}, {2, false, gi_i2_end0},
            {4, true, gi_i4_end1}, {4, false, gi_i4_end0},
        };
        size_t completed = 0;
        for (const auto &shader : shaders) {
            for (uint32_t primitives : {1u, 3u}) {
                for (bool indirect : {false, true}) {
                    current = std::string(shader.explicit_end ? "explicit" : "implicit") +
                        "-i" + std::to_string(shader.invocations) + "-p" +
                        std::to_string(primitives) + (indirect ? "-indirect" : "-direct");
                    if (argc == 2 && std::string_view(argv[1]) != current)
                        continue;
                    DrawConfig config;
                    config.vertex_shader = gi_vertex;
                    config.geometry_shader = shader.shader;
                    config.fragment_shader = gi_fragment;
                    config.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
                    config.vertex_count = primitives;
                    const std::vector<uint32_t> counters(1 + primitives * shader.invocations);
                    config.counters = counters;
                    config.transform_feedback_words = primitives * shader.invocations * 16 + 16;
                    config.indirect = indirect;
                    std::printf("GS invocation probe starting %s\n", current.c_str());
                    std::fflush(stdout);
                    const auto cold = context.draw(config);
                    verify(cold, shader.invocations, primitives);
                    DrawContext cache_context;
                    verify(cache_context.draw(config, cold.pipeline_cache), shader.invocations,
                           primitives);
                    std::printf("GS invocation probe PASS %s (cold and reloaded cache)\n",
                                current.c_str());
                    ++completed;
                }
            }
        }
        require(completed != 0, "no matching test case");
        std::printf("GS invocation probe PASS: %zu cases\n", completed);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "GS invocation probe FAIL %s: %s\n", current.c_str(), error.what());
        return 1;
    }
}
