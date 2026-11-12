// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan_draw_test.hpp"
#include "shaders/line_multiview_spirv.hpp"
#include "require.hpp"

#include <array>
#include <cmath>
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

uint8_t unorm(float value)
{
    return static_cast<uint8_t>(std::lround(value * 255.0f));
}

void verify(const DrawConfig &config, const DrawResult &result,
            std::string_view name, bool fixed_layer)
{
    const uint32_t active = config.view_mask ? config.view_mask : 1u;
    const auto fail = [&](const std::string &message) {
        return std::string(name) + ": " + message;
    };
    for (uint32_t view = 0; view < config.layers; ++view) {
        const bool invoked = active & (1u << view);
        const bool enabled = fixed_layer ? view == 3 : invoked;
        const uint32_t shader_view = fixed_layer ? 0 : view;
        for (uint32_t instance = 0; instance < config.instance_count; ++instance) {
            const uint32_t count = result.counters[view * 2 + instance];
            const bool software = config.stipple ||
                config.line_mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_KHR ||
                config.provoking_vertex == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
            // Native input assembly may invoke a shared strip endpoint more
            // than once. The software producer dispatch is exact by contract.
            require(invoked ? (software ? count == config.vertex_count
                                        : count >= config.vertex_count)
                            : count == 0,
                    fail("view " + std::to_string(view) + " instance " +
                         std::to_string(instance) + " VS count " + std::to_string(count)));
        }
        for (uint32_t y = 0; y < config.height; ++y) {
            for (uint32_t x = 0; x < config.width; ++x) {
                const auto &pixel = result.pixels[(view * config.height + y) * config.width + x];
                const uint32_t length = 8 + shader_view * 2;
                const uint32_t end = 8 + length * (config.vertex_count - 1);
                int instance = -1;
                for (uint32_t i = 0; i < config.instance_count; ++i) {
                    if (y == 8 + shader_view * 10 + i * 4)
                        instance = static_cast<int>(i);
                }
                // Sample the interior of each segment. Exact endpoint ownership
                // differs between the native relaxed and rectangular line modes.
                const bool boundary = x == 8 || x == 8 + length || x == end;
                if (enabled && instance >= 0 && boundary)
                    continue;
                bool covered = enabled && instance >= 0 && x > 8 && x < end;
                if (config.stipple && covered)
                    covered = (config.stipple_pattern >> ((x - 8) % 16)) & 1u;
                std::array<uint8_t, 4> expected{};
                if (covered) {
                    const uint32_t segment = (x - 8) / length;
                    const uint32_t vertex = segment +
                        (config.provoking_vertex == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT);
                    expected = {unorm(float(shader_view + 1) / 8.0f),
                                unorm(float(vertex + 1) / 4.0f),
                                unorm(float(instance + 1) / 4.0f),
                                unorm(float(shader_view + 1) / 8.0f)};
                }
                require(pixel == expected,
                        fail("view " + std::to_string(view) + " pixel " +
                             std::to_string(x) + "," + std::to_string(y) + " expected " +
                             std::to_string(expected[0]) + "," + std::to_string(expected[1]) + "," +
                             std::to_string(expected[2]) + "," + std::to_string(expected[3]) + " got " +
                             std::to_string(pixel[0]) + "," + std::to_string(pixel[1]) + "," +
                             std::to_string(pixel[2]) + "," + std::to_string(pixel[3])));
            }
        }
    }
    require(!result.pipeline_cache.empty(), fail("empty serialized cache"));
}

} // namespace

int main(int argc, char **argv)
{
    try {
        DrawContext context;
        require_venus_when_asked(context);
        const std::array<uint32_t, 16> counters{};
        const std::array<uint32_t, 3> indices{0, 1, 2};
        struct Case {
            const char *name;
            uint32_t views;
            bool indirect;
            bool indexed;
            bool rectangular;
            bool last;
            bool stipple;
            bool fixed_layer = false;
        };
        const Case cases[] = {
            {"native-dense", 0x3, false, false, false, false, false},
            {"last-dense", 0x3, false, false, false, true, false},
            {"rectangular-sparse", 0x5, false, false, true, false, false},
            {"stipple-sparse-indirect", 0x5, true, false, true, true, true},
            {"last-single-nonzero-indirect", 0x8, true, false, false, true, false},
            {"stipple-single-nonzero-indexed", 0x8, false, true, true, true, true},
            {"stipple-dense-indexed-indirect", 0x3, true, true, true, true, true},
            {"rectangular-view-zero", 0x1, false, false, true, true, false},
            {"rectangular-no-multiview", 0x0, false, false, true, true, false},
            {"rectangular-no-multiview-layer", 0x0, false, false, true, true, false, true},
        };
        uint32_t executed = 0;
        for (const auto &test : cases) {
            if (argc > 1 && std::string_view(argv[1]) != test.name)
                continue;
            DrawConfig config;
            config.vertex_shader = line_multiview_vert_spirv;
            if (test.fixed_layer)
                config.vertex_shader = line_multiview_layer_vert_spirv;
            config.fragment_shader = line_multiview_frag_spirv;
            config.counters = counters;
            config.layers = 4;
            config.view_mask = test.views;
            config.vertex_count = 3;
            config.instance_count = 2;
            config.first_vertex = 5;
            config.first_instance = 3;
            config.indirect = test.indirect;
            if (test.indexed)
                config.indices = indices;
            config.line_mode = test.rectangular ? VK_LINE_RASTERIZATION_MODE_RECTANGULAR_KHR
                                                : VK_LINE_RASTERIZATION_MODE_DEFAULT_KHR;
            config.provoking_vertex = test.last ? VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT
                                               : VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT;
            config.stipple = test.stipple;
            const auto first = context.draw(config);
            verify(config, first, test.name, test.fixed_layer);
            DrawContext cache_context;
            const auto cached = cache_context.draw(config, first.pipeline_cache);
            verify(config, cached, std::string(test.name) + "-serialized-cache",
                    test.fixed_layer);
            std::printf("PASS %s and serialized cache\n", test.name);
            std::fflush(stdout);
            ++executed;
        }
        require(executed != 0, "unknown multiview test case");
        std::printf("PASS line multiview: %u cases, %u draws\n", executed, executed * 2);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL line multiview: %s\n", error.what());
        return 1;
    }
}
