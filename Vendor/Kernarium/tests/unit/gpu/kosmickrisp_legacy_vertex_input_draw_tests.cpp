// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan_draw_test.hpp"
#include "shaders/legacy_vertex_input_spirv.hpp"
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
using uml::gpu::test::VertexInputDraw;

constexpr auto first = VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT;
constexpr auto last = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
constexpr std::array<VertexInputDraw, 8> draws{{
    {VK_FORMAT_R8_UNORM, 3, 1, 1, first, false},
    {VK_FORMAT_R8_UNORM, 3, 1, 1, last, true},
    {VK_FORMAT_R16_UINT, 3, 1, 1, first, false},
    {VK_FORMAT_R16_UINT, 5, 2, 1, last, true},
    {VK_FORMAT_R8_UNORM, 3, 1, 1, first, false},
    {VK_FORMAT_R16_UNORM, 5, 2, 1, last, false},
    {VK_FORMAT_R16_UNORM, 5, 2, 1, first, false},
    {VK_FORMAT_R8_UNORM, 3, 1, 1, last, true},
}};

void verify(const DrawConfig &config, const DrawResult &result,
            const std::string &name)
{
    for (uint32_t y = 0; y < config.height; ++y) {
        const auto &draw = draws[y / 8];
        for (uint32_t x = 0; x < config.width; ++x) {
            // Native and generated lines may differ in exact endpoint ownership.
            if (y % 8 == 4 && (x == 8 || x == 56))
                continue;
            bool covered = y % 8 == 4 && x > 8 && x < 56;
            if (draw.stipple && covered)
                covered = (config.stipple_pattern >> ((x - 8) % 16)) & 1u;
            std::array<uint8_t, 4> expected{};
            if (covered) {
                expected = {uint8_t(draw.format == VK_FORMAT_R16_UINT ? 191 : 64),
                            uint8_t(draw.provoking_vertex == first ? 64 : 191),
                            128, 255};
            }
            const auto &actual = result.pixels[y * config.width + x];
            require(actual == expected, name + " pixel " + std::to_string(x) + "," +
                        std::to_string(y) + " expected " + std::to_string(expected[0]) + "," +
                        std::to_string(expected[1]) + " got " + std::to_string(actual[0]) + "," +
                        std::to_string(actual[1]));
        }
    }
    if (config.transform_feedback_words) {
        for (uint32_t index = 0; index < result.transform_feedback.size(); ++index) {
            const uint32_t expected = index >= draws.size() * 2 ? 0
                : draws[index / 2].format == VK_FORMAT_R16_UINT ? 65535u : 0x3f800000u;
            require(result.transform_feedback[index] == expected,
                    name + " XFB word " + std::to_string(index));
        }
    }
    require(!result.pipeline_cache.empty(), name + " empty serialized pipeline cache");
}

} // namespace

int main(int argc, char **argv)
{
    try {
        DrawContext context(true);
        require_venus_when_asked(context);
        std::array<uint8_t, 32> bytes;
        bytes.fill(0xff);
        const std::array<uint32_t, 2> indices{0, 1};
        uint32_t executed = 0;
        for (uint32_t capture = 0; capture < 2; ++capture) {
            for (uint32_t kind = 0; kind < 4; ++kind) {
                const std::string name = std::string(capture ? "xfb" : "plain") +
                                         "-draw" + std::to_string(kind);
                if (argc > 1 && std::string_view(argv[1]) != name)
                    continue;
                DrawConfig config;
                config.vertex_shader = capture ? std::span<const uint32_t>(legacy_capture)
                                                : std::span<const uint32_t>(legacy_vertex);
                config.fragment_shader = legacy_fragment;
                config.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
                config.line_mode = VK_LINE_RASTERIZATION_MODE_BRESENHAM_KHR;
                config.vertex_count = 2;
                config.width = 64;
                config.height = draws.size() * 8;
                config.vertex_data = bytes;
                config.vertex_input_draws = draws;
                config.indirect = kind & 1u;
                if (kind & 2u)
                    config.indices = indices;
                if (capture)
                    config.transform_feedback_words = draws.size() * 2 + 4;
                const auto cold = context.draw(config);
                verify(config, cold, name);
                DrawContext reloaded(true);
                verify(config, reloaded.draw(config, cold.pipeline_cache), name + "-cache");
                std::printf("PASS %s and fresh-device serialized cache\n", name.c_str());
                std::fflush(stdout);
                ++executed;
            }
        }
        require(executed != 0, "unknown legacy draw case");
        std::printf("PASS legacy vertex input: %u sequences, %zu draws\n",
                    executed * 2, executed * 2 * draws.size());
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL legacy vertex input: %s\n", error.what());
        return 1;
    }
}
