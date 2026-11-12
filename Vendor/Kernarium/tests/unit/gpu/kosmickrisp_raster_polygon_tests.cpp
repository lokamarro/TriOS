// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan_draw_test.hpp"
#include "shaders/raster_polygon_spirv.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <bit>
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

struct ReferenceVertex {
    std::array<float, 4> position;
    float size;
    float clip;
    float smooth_value;
    float linear_value;
};

std::vector<ReferenceVertex> reference_polygon(unsigned shape)
{
    std::vector<ReferenceVertex> polygon{
        {{{-0.5f, -0.5f, 0, 1}}, 4, -1, 0.2f, 0.2f},
        {{{2, -2, 0, 4}}, 8, 1, 0.6f, 0.6f},
        {{{-1, 1, 0, 2}}, 12, 1, 1.0f, 1.0f},
    };
    for (unsigned v = 0; v < polygon.size(); ++v) {
        if (shape == 1 || shape == 2 || shape == 5)
            polygon[v].size = 2;
        if (shape == 2)
            polygon[v].position[2] = polygon[v].position[3] * (v == 0 ? -1.0f : 0.5f);
        if (shape == 5)
            polygon[v].position[1] = -0.5f * polygon[v].position[3];
    }
    const auto clip = [&](auto distance) {
        std::vector<ReferenceVertex> output;
        for (unsigned i = 0; i < polygon.size(); ++i) {
            const auto &a = polygon[(i + polygon.size() - 1) % polygon.size()];
            const auto &b = polygon[i];
            const float da = distance(a), db = distance(b);
            if ((da < 0 && db > 0) || (da > 0 && db < 0)) {
                const float t = da / (da - db);
                ReferenceVertex intersection{};
                for (unsigned c = 0; c < 4; ++c)
                    intersection.position[c] = a.position[c] + t * (b.position[c] - a.position[c]);
                intersection.size = a.size + t * (b.size - a.size);
                intersection.clip = a.clip + t * (b.clip - a.clip);
                intersection.smooth_value = a.smooth_value + t * (b.smooth_value - a.smooth_value);
                const float linear_t = t * b.position[3] / intersection.position[3];
                intersection.linear_value = a.linear_value + linear_t * (b.linear_value - a.linear_value);
                output.push_back(intersection);
            }
            if (db >= 0)
                output.push_back(b);
        }
        polygon = std::move(output);
    };
    if (shape == 1)
        clip([](const ReferenceVertex &v) { return v.clip; });
    if (shape == 2)
        clip([](const ReferenceVertex &v) { return v.position[2]; });
    if (shape == 4)
        polygon.clear();
    return polygon;
}

void verify(const DrawResult &result, const DrawConfig &config, bool front,
            unsigned shape, uint8_t flat_value, bool attributes = false)
{
    const bool culled = config.cull_mode &
        (front ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT);
    const auto polygon = reference_polygon(shape);
    for (uint32_t y = 0; y < config.height; ++y) {
        for (uint32_t x = 0; x < config.width; ++x) {
            std::array<uint8_t, 4> expected{};
            for (unsigned point = 0; !culled && point < polygon.size(); ++point) {
                const auto &v = polygon[point];
                const float center_x = (v.position[0] / v.position[3] + 1) * 32;
                float center_y = (v.position[1] / v.position[3] + 1) * 32;
                if (config.negative_viewport)
                    center_y = 64 - center_y;
                const float s = (float(x) + 0.5f - center_x) / v.size + 0.5f;
                const float t = (float(y) + 0.5f - center_y) / v.size + 0.5f;
                if (s >= 0 && s < 1 && t >= 0 && t < 1)
                    expected = {uint8_t(front ? 255 : 0),
                                uint8_t(std::lround((attributes ? v.smooth_value : s) * 255)),
                                uint8_t(std::lround((attributes ? v.linear_value : t) * 255)), flat_value};
            }
            const auto actual = result.pixels[y * config.width + x];
            if (config.stencil)
                require(result.stencil[y * config.width + x] ==
                            (expected[3] ? (front ? 1 : 2) : 0),
                        "two-sided polygon-point stencil mismatch at (" +
                        std::to_string(x) + "," + std::to_string(y) + "): " +
                        std::to_string(result.stencil[y * config.width + x]));
            for (unsigned channel = 0; channel < 4; ++channel)
                require(std::abs(int(actual[channel]) - int(expected[channel])) <= 1,
                        "pixel (" + std::to_string(x) + "," + std::to_string(y) +
                        ") channel " + std::to_string(channel) + ": expected " +
                        std::to_string(expected[channel]) + ", got " +
                        std::to_string(actual[channel]) + " (RGBA " +
                        std::to_string(actual[0]) + "," + std::to_string(actual[1]) +
                        "," + std::to_string(actual[2]) + "," + std::to_string(actual[3]) + ")");
        }
    }
    if (config.transform_feedback_words) {
        std::array<uint32_t, 16> expected{};
        for (uint32_t vertex = 0; vertex < 3; ++vertex) {
            expected[vertex * 4] = vertex;
            expected[vertex * 4 + 1] = 7;
            expected[vertex * 4 + 2] = 11;
            expected[vertex * 4 + 3] = 13;
        }
        require(std::equal(expected.begin(), expected.end(), result.transform_feedback.begin()),
                "polygon point conversion changed XFB records");
    }
}

unsigned verify_frustum_boundaries(DrawContext &context)
{
    unsigned completed = 0;
    for (bool dynamic : {false, true}) {
        for (bool negative : {false, true}) {
            for (bool reverse : {false, true}) {
                for (float scale : {1.0f, 2.0f, 4096.0f}) {
                    // Every source edge lies outside the viewport. Frustum
                    // clipping closes the filled polygon, but must not invent
                    // a point or line along the viewport's artificial border.
                    std::array<std::array<float, 2>, 3> positions{{
                        {-32.0f * scale, -32.0f * scale},
                        {-32.0f * scale, 224.0f * scale},
                        {224.0f * scale, -32.0f * scale},
                    }};
                    if (reverse)
                        std::reverse(positions.begin(), positions.end());
                    DrawConfig config;
                    config.vertex_shader = polygon_endpoints_vertex;
                    config.fragment_shader = polygon_endpoints_fragment;
                    config.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                    config.dynamic_raster = dynamic;
                    config.negative_viewport = negative;
                    std::vector<uint32_t> data(32 + 64 * 64);
                    for (unsigned vertex = 0; vertex < 3; ++vertex) {
                        data[vertex * 4] = std::bit_cast<uint32_t>(positions[vertex][0]);
                        data[vertex * 4 + 1] = std::bit_cast<uint32_t>(positions[vertex][1]);
                        data[vertex * 4 + 2] = std::bit_cast<uint32_t>(0.5f);
                        data[vertex * 4 + 3] = std::bit_cast<uint32_t>(1.0f);
                    }
                    config.counters = data;
                    const auto filled = context.draw(config);
                    for (unsigned pixel = 0; pixel < filled.pixels.size(); ++pixel)
                        require(filled.pixels[pixel] == std::array<uint8_t, 4>{255, 255, 255, 255} &&
                                    filled.counters[32 + pixel] == 1,
                                "oversized filled reference did not cover the viewport");
                    ++completed;
                    for (VkPolygonMode mode : {VK_POLYGON_MODE_POINT, VK_POLYGON_MODE_LINE}) {
                        config.polygon_mode = mode;
                        const auto cold = context.draw(config);
                        for (unsigned pixel = 0; pixel < cold.pixels.size(); ++pixel)
                            require(cold.pixels[pixel] == std::array<uint8_t, 4>{} &&
                                        cold.counters[32 + pixel] == 0,
                                    "artificial frustum boundary became a polygon point or line");
                        DrawContext fresh;
                        config.fail_on_compile_required = true;
                        const auto cached = fresh.draw(config, cold.pipeline_cache);
                        require(cold.pixels == cached.pixels && cold.counters == cached.counters,
                                "fresh-device cache changed frustum boundary coverage");
                        config.fail_on_compile_required = false;
                        completed += 2;
                    }
                }
            }
        }
    }
    return completed;
}

unsigned verify_large_point_guard_band(DrawContext &context)
{
    unsigned completed = 0;
    for (bool dynamic : {false, true}) {
        for (bool negative : {false, true}) {
            for (bool reverse : {false, true}) {
                for (float size : {96.0f, 511.0f}) {
                    std::array<std::array<float, 2>, 3> positions{{
                        {-32, -32}, {-32, 224}, {224, -32},
                    }};
                    if (reverse)
                        std::reverse(positions.begin(), positions.end());
                    DrawConfig config;
                    config.vertex_shader = polygon_endpoints_point_vertex;
                    config.fragment_shader = polygon_endpoints_fragment;
                    config.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                    config.polygon_mode = VK_POLYGON_MODE_POINT;
                    config.dynamic_raster = dynamic;
                    config.negative_viewport = negative;
                    std::vector<uint32_t> data(32 + 64 * 64);
                    for (unsigned vertex = 0; vertex < 3; ++vertex) {
                        data[vertex * 4] = std::bit_cast<uint32_t>(positions[vertex][0]);
                        data[vertex * 4 + 1] = std::bit_cast<uint32_t>(positions[vertex][1]);
                        data[vertex * 4 + 2] = std::bit_cast<uint32_t>(0.5f);
                        data[vertex * 4 + 3] = std::bit_cast<uint32_t>(1.0f);
                        data[16 + vertex] = std::bit_cast<uint32_t>(size);
                    }
                    config.counters = data;
                    const auto cold = context.draw(config);
                    for (unsigned y = 0; y < 64; ++y) {
                        for (unsigned x = 0; x < 64; ++x) {
                            unsigned expected = 0;
                            for (const auto &position : positions) {
                                const float center_y = negative ? 64 - position[1] : position[1];
                                expected += std::abs(float(x) + 0.5f - position[0]) < size * 0.5f &&
                                            std::abs(float(y) + 0.5f - center_y) < size * 0.5f;
                            }
                            const auto color = expected ? std::array<uint8_t, 4>{255, 255, 255, 255}
                                                        : std::array<uint8_t, 4>{};
                            require(cold.pixels[y * 64 + x] == color &&
                                        cold.counters[32 + y * 64 + x] == expected,
                                    "polygon clipping changed visible guard-band point coverage");
                        }
                    }
                    DrawContext fresh;
                    config.fail_on_compile_required = true;
                    const auto cached = fresh.draw(config, cold.pipeline_cache);
                    require(cold.pixels == cached.pixels && cold.counters == cached.counters,
                            "fresh-device cache changed large-point guard-band coverage");
                    completed += 2;
                }
            }
        }
    }
    return completed;
}

} // namespace

int main(int argc, char **argv)
{
    std::string current;
    try {
        require(argc <= 2, "expected at most one exact stage selector");
        DrawContext context;
        require_venus_when_asked(context);
        if (argc == 2 && std::string_view(argv[1]) == "frustum-boundaries") {
            std::printf("Polygon frustum boundary probe: %u draws passed\n",
                        verify_frustum_boundaries(context) + verify_large_point_guard_band(context));
            return 0;
        }
        const char *stages[] = {"vs", "gs", "tes", "tes-gs", "xfb"};
        struct ShaderSet {
            std::span<const uint32_t> vertex, capture, geometry, control, evaluation;
        };
#define SHADERS(suffix) ShaderSet{polygon_vertex##suffix, polygon_capture##suffix, \
    polygon_geometry##suffix, polygon_control##suffix, polygon_evaluation##suffix}
        const ShaderSet shapes[] = {SHADERS(), SHADERS(_1), SHADERS(_2),
                                     SHADERS(_3), SHADERS(_4), SHADERS(_5)};
#undef SHADERS
        const std::array<uint32_t, 3> indices{0, 1, 2};
        const std::array<VkSampleLocationEXT, 4> locations{{{0.125f, 0.125f}, {0.875f, 0.125f},
                                                           {0.125f, 0.875f}, {0.875f, 0.875f}}};
        unsigned completed = 0;
        for (unsigned stage = 0; stage < std::size(stages); ++stage) {
            if (argc == 2 && std::string_view(argv[1]) != stages[stage])
                continue;
            for (bool negative : {false, true}) {
                for (VkFrontFace winding : {VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FRONT_FACE_CLOCKWISE}) {
                    DrawConfig config;
                    config.vertex_shader = stage == 4
                        ? std::span<const uint32_t>(polygon_capture)
                        : std::span<const uint32_t>(polygon_vertex);
                    config.fragment_shader = polygon_facing;
                    config.topology = stage == 2 || stage == 3
                        ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                    if (stage == 1 || stage == 3)
                        config.geometry_shader = polygon_geometry;
                    if (stage == 2 || stage == 3) {
                        config.tess_control_shader = polygon_control;
                        config.tess_eval_shader = polygon_evaluation;
                    }
                    config.transform_feedback_words = stage == 4 ? 16 : 0;
                    config.stencil = true;
                    config.negative_viewport = negative;
                    config.front_face = winding;
                    current = std::string(stages[stage]) + "-viewport" + std::to_string(negative) +
                              "-front" + std::to_string(winding);
                    const auto reference = context.draw(config);
                    const auto inside = reference.pixels[(negative ? 40 : 24) * 64 + 24];
                    require(inside[3] != 0, "filled reference polygon missing");
                    const bool front = inside[0] == 255;
                    const uint8_t first_flat_value = inside[3];
                    config.provoking_vertex = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
                    const auto last_reference = context.draw(config);
                    const uint8_t last_flat_value =
                        last_reference.pixels[(negative ? 40 : 24) * 64 + 24][3];
                    config.fragment_shader = polygon_fragment;
                    config.polygon_mode = VK_POLYGON_MODE_POINT;
                    for (unsigned shape = 0; shape < std::size(shapes); ++shape) {
                      config.vertex_shader = stage == 4 ? shapes[shape].capture : shapes[shape].vertex;
                      if (stage == 1 || stage == 3)
                          config.geometry_shader = shapes[shape].geometry;
                      if (stage == 2 || stage == 3) {
                          config.tess_control_shader = shapes[shape].control;
                          config.tess_eval_shader = shapes[shape].evaluation;
                      }
                      for (VkCullModeFlags cull = 0; cull < 4; ++cull) {
                        config.cull_mode = cull;
                        config.provoking_vertex = cull & 1
                            ? VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT
                            : VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT;
                        const uint8_t flat_value = cull & 1 ? last_flat_value : first_flat_value;
                        config.indirect = cull & 1;
                        config.indices = cull & 2 ? std::span(indices) : std::span<const uint32_t>();
                        std::printf("Polygon probe %s-shape%u-cull%u\n", current.c_str(), shape, cull);
                        std::fflush(stdout);
                        const auto cold = context.draw(config);
                        verify(cold, config, shape == 5 ? false : front, shape, flat_value);
                        DrawContext cache_context;
                        const auto cached = cache_context.draw(config, cold.pipeline_cache);
                        verify(cached, config, shape == 5 ? false : front, shape, flat_value);
                        completed += 2;
                        if (shape == 0 && cull == VK_CULL_MODE_NONE) {
                            config.stencil = false;
                            config.samples = VK_SAMPLE_COUNT_4_BIT;
                            config.sample_locations = locations;
                            const auto msaa = context.draw(config);
                            verify(msaa, config, front, shape, flat_value);
                            for (unsigned pixel = 0; pixel < cold.pixels.size(); ++pixel)
                                for (unsigned component = 0; component < 4; ++component)
                                    require(std::abs(int(msaa.pixels[pixel][component]) -
                                                     int(cold.pixels[pixel][component])) <= 1,
                                            "explicit MSAA changed polygon-point coverage/attributes");
                            config.samples = VK_SAMPLE_COUNT_1_BIT;
                            config.sample_locations = {};
                            config.stencil = true;
                            ++completed;
                        }
                        if (!negative && winding == VK_FRONT_FACE_COUNTER_CLOCKWISE &&
                            cull == VK_CULL_MODE_NONE && shape != 5) {
                            config.fragment_shader = polygon_attributes;
                            const auto interpolated = context.draw(config);
                            verify(interpolated, config, front, shape, flat_value, true);
                            DrawContext attribute_cache;
                            const auto reloaded = attribute_cache.draw(config, interpolated.pipeline_cache);
                            verify(reloaded, config, front, shape, flat_value, true);
                            config.fragment_shader = polygon_fragment;
                            completed += 2;
                        }
                      }
                    }
                }
            }
        }
        if (argc == 1)
            completed += verify_frustum_boundaries(context) + verify_large_point_guard_band(context);
        require(completed != 0, "stage selector matched no cases");
        std::printf("Polygon raster probe: %u draws passed\n", completed);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Polygon raster probe failed %s: %s\n", current.c_str(), error.what());
        return 1;
    }
}
