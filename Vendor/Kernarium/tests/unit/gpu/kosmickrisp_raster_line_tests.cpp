// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan_draw_test.hpp"
#include "vulkan_draw_performance.hpp"
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
#include <vector>

namespace {
using uml::testing::require;
using uml::gpu::test::DrawConfig;
using uml::gpu::test::DrawContext;
using uml::gpu::test::require_venus_when_asked;
using uml::gpu::test::DrawResult;

bool diamond_exit(std::array<double, 2> a, std::array<double, 2> b,
                  uint32_t x, uint32_t y)
{
    constexpr double epsilon = 1.0e-5;
    for (unsigned c = 0; c < 2; ++c) {
        a[c] -= c ? epsilon * epsilon : epsilon;
        b[c] -= c ? epsilon * epsilon : epsilon;
        a[c] -= (c ? y : x) + 0.5;
        b[c] -= (c ? y : x) + 0.5;
    }
    double lo = 0, hi = 1;
    bool end_inside = true;
    for (int sign : {-1, 1}) {
        const double start = a[0] + sign * a[1];
        const double end = b[0] + sign * b[1];
        end_inside &= std::abs(end) < 0.5;
        const double delta = end - start;
        if (delta == 0) {
            if (std::abs(start) >= 0.5)
                return false;
        } else {
            const double t0 = (-0.5 - start) / delta;
            const double t1 = (0.5 - start) / delta;
            lo = std::max(lo, std::min(t0, t1));
            hi = std::min(hi, std::max(t0, t1));
        }
    }
    return lo < hi && !end_inside;
}

unsigned verify_endpoints(DrawContext &context)
{
    using Position = std::array<double, 2>;
    using Path = std::array<Position, 3>;
    std::vector<Path> paths;
    // Shared endpoints on pixel centers, edges, diamonds and nearby subpixels,
    // plus short segments entirely inside or grazing one pixel's diamond.
    for (double fraction : {0.0, 0.25, 0.5, 0.75}) {
        paths.push_back({Position{8 + fraction, 16 + fraction},
                         Position{24 + fraction, 16 + fraction},
                         Position{48 + fraction, 16 + fraction}});
        paths.push_back({Position{8 + fraction, 8 + fraction},
                         Position{24 + fraction, 24 + fraction},
                         Position{48 + fraction, 48 + fraction}});
        paths.push_back({Position{16 + fraction, 16.5},
                         Position{16.5, 16 + fraction},
                         Position{17 - fraction, 16.5}});
        paths.push_back({Position{8 + fraction, 16.5},
                         Position{24.5, 24 + fraction},
                         Position{48 - fraction, 16.5}});
    }
    uint32_t random = 0x41893f21;
    for (unsigned path = 0; path < 16; ++path) {
        Path positions{};
        for (auto &position : positions)
            for (auto &coordinate : position) {
                random = random * 1664525u + 1013904223u;
                coordinate = 4 + double((random >> 16) % 14081) / 256;
            }
        paths.push_back(positions);
    }

    DrawConfig config;
    config.vertex_shader = polygon_endpoints_vertex;
    config.fragment_shader = polygon_endpoints_fragment;
    config.line_mode = VK_LINE_RASTERIZATION_MODE_BRESENHAM_KHR;
    config.provoking_vertex = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
    config.stipple = true;
    std::vector<uint8_t> cache;
    unsigned completed = 0;
    for (unsigned path = 0; path < paths.size(); ++path) {
        for (unsigned transform = 0; transform < 4; ++transform) {
            Path positions = paths[path];
            if (transform & 1)
                std::reverse(positions.begin(), positions.end());
            if (transform & 2)
                for (auto &position : positions)
                    std::swap(position[0], position[1]);
            for (uint16_t pattern : {0xffff, 0x5555, 0x80e1}) {
                config.stipple_pattern = pattern;
                config.stipple_factor = pattern == 0x80e1 ? 3 : 1;
                std::vector<uint32_t> data(32 + 64 * 64);
                for (unsigned vertex = 0; vertex < 3; ++vertex) {
                    data[vertex * 4] = std::bit_cast<uint32_t>(float(positions[vertex][0]));
                    data[vertex * 4 + 1] = std::bit_cast<uint32_t>(float(positions[vertex][1]));
                    data[vertex * 4 + 2] = 0;
                    data[vertex * 4 + 3] = std::bit_cast<uint32_t>(1.0f);
                }
                config.counters = data;
                const auto result = context.draw(config, cache);
                cache = result.pipeline_cache;
                std::vector<uint32_t> expected(64 * 64);
                unsigned phase = 0;
                for (unsigned edge = 0; edge < 2; ++edge) {
                    const auto &a = positions[edge], &b = positions[edge + 1];
                    const unsigned major = std::abs(b[0] - a[0]) < std::abs(b[1] - a[1]);
                    std::vector<std::array<uint32_t, 2>> pixels;
                    for (uint32_t y = 0; y < 64; ++y)
                        for (uint32_t x = 0; x < 64; ++x)
                            if (diamond_exit(a, b, x, y))
                                pixels.push_back({x, y});
                    std::sort(pixels.begin(), pixels.end(), [&](auto left, auto right) {
                        return b[major] < a[major] ? left[major] > right[major]
                                                  : left[major] < right[major];
                    });
                    for (const auto &pixel : pixels) {
                        if ((pattern >> ((phase / config.stipple_factor) & 15)) & 1)
                            ++expected[pixel[1] * 64 + pixel[0]];
                        ++phase;
                    }
                }
                for (unsigned pixel = 0; pixel < expected.size(); ++pixel)
                    require(result.counters[32 + pixel] == expected[pixel],
                            "endpoint/continuity count mismatch path " + std::to_string(path) +
                            " transform " + std::to_string(transform) + " pattern " +
                            std::to_string(pattern) + " at " + std::to_string(pixel % 64) +
                            "," + std::to_string(pixel / 64) + ": got " +
                            std::to_string(result.counters[32 + pixel]) + " expected " +
                            std::to_string(expected[pixel]));
                ++completed;
            }
        }
    }
    return completed;
}

unsigned verify_smooth_caps(DrawContext &context)
{
    unsigned completed = 0;
    for (bool dynamic : {false, true}) {
        for (bool negative : {false, true}) {
            for (unsigned transform = 0; transform < 4; ++transform) {
                for (float offset : {0.0f, 16.0f}) {
                    std::array<std::array<float, 2>, 2> positions{{
                        {0.4375f + offset, 0.75f + offset},
                        {64.0f, 32.0f + offset},
                    }};
                    if (transform & 1)
                        std::reverse(positions.begin(), positions.end());
                    if (transform & 2)
                        for (auto &position : positions)
                            std::swap(position[0], position[1]);
                    for (bool stipple : {false, true}) {
                        DrawConfig config;
                        config.vertex_shader = polygon_endpoints_vertex;
                        config.fragment_shader = polygon_endpoints_fragment;
                        config.vertex_count = 2;
                        config.line_mode = VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_KHR;
                        config.dynamic_raster = dynamic;
                        config.negative_viewport = negative;
                        config.stipple = stipple;
                        config.stipple_pattern = 0xffff;
                        std::vector<uint32_t> data(32 + 64 * 64);
                        for (unsigned vertex = 0; vertex < 2; ++vertex) {
                            data[vertex * 4] = std::bit_cast<uint32_t>(positions[vertex][0]);
                            data[vertex * 4 + 1] = std::bit_cast<uint32_t>(positions[vertex][1]);
                            data[vertex * 4 + 3] = std::bit_cast<uint32_t>(1.0f);
                        }
                        config.counters = data;
                        const auto cold = context.draw(config);
                        // This pixel intersects the endpoint rectangle, but its
                        // center projects behind the endpoint along the line.
                        const unsigned x = static_cast<unsigned>(offset);
                        const unsigned y = negative ? 63 - x : x;
                        const auto pixel = cold.pixels[y * 64 + x];
                        require(pixel[0] == 255 && pixel[1] == 255 && pixel[2] == 255 &&
                                    pixel[3] > 0 && pixel[3] < 255,
                                "smooth endpoint lost partial coverage");
                        require(cold.counters[32 + y * 64 + x] == 1,
                                "smooth endpoint was missing or shaded more than once");
                        DrawContext fresh;
                        config.fail_on_compile_required = true;
                        const auto cached = fresh.draw(config, cold.pipeline_cache);
                        require(cold.pixels == cached.pixels && cold.counters == cached.counters,
                                "fresh-device cache changed smooth cap coverage");
                        completed += 2;
                    }
                }
            }
        }
    }
    return completed;
}

void measure_line_scaling(DrawContext &context, bool whole_submit)
{
    using namespace uml::gpu::test;
    DrawPerformanceSession session(context, whole_submit);
    print_draw_performance_device(context);
    struct Workload { uint32_t width, primitives; };
    // Keep edge length and primitive cardinality as separate axes. Repeated
    // triangles deliberately overlap, so scaling retains an exact pixel oracle.
    constexpr Workload workloads[]{{64, 1}, {1024, 1}, {8192, 1},
                                   {64, 64}, {64, 1024}};
    for (const auto workload : workloads) {
        for (bool stipple : {false, true}) {
            for (unsigned mode = stipple ? 1 : 0; mode < 4; ++mode) {
                DrawConfig config;
                config.vertex_shader = polygon_vertex;
                config.fragment_shader = polygon_facing;
                config.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                config.polygon_mode = VK_POLYGON_MODE_LINE;
                config.width = workload.width;
                config.stipple = stipple;
                config.stipple_pattern = 0x39c7;
                config.stipple_factor = 3;
                config.provoking_vertex = mode == 0 ? VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT
                                                    : VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
                config.line_mode = mode < 2 ? VK_LINE_RASTERIZATION_MODE_BRESENHAM_KHR
                                 : mode == 2 ? VK_LINE_RASTERIZATION_MODE_RECTANGULAR_KHR
                                             : VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_KHR;
                const auto reference = session.reference(context, config);
                require(std::any_of(reference.pixels.begin(), reference.pixels.end(),
                                    [](const auto &pixel) { return pixel[3] != 0; }),
                        "performance reference produced no line coverage");
                config.vertex_count = 3 * workload.primitives;
                config.timestamp = true;
                config.repeat_count = 32;
                const auto timing = measure_draw_performance(context, config,
                    [&](const DrawResult &result) {
                        require(result.pixels == reference.pixels,
                                "line scaling changed single-triangle reference pixels");
                    }, session);
                std::printf("Raster performance source=%s cpu_scope=instrumented-recording width=%u primitives=%u input_edges=%u class=%s stipple=%u gpu_median_ns=%.1f record_median_ns=%.1f gpu_ns_per_input_edge=%.3f repeats=%u samples=%u\n",
                            session.source(), workload.width, workload.primitives, 3 * workload.primitives,
                            mode == 0 ? "native" : mode == 1 ? "bresenham"
                                       : mode == 2 ? "rectangular" : "smooth",
                            unsigned(stipple), timing.gpu_nanoseconds,
                            timing.record_nanoseconds,
                            timing.gpu_nanoseconds / (3 * workload.primitives),
                            config.repeat_count, draw_performance_samples);
                std::fflush(stdout);
            }
        }
    }
    session.finish();
}

void verify(const DrawResult &result, const DrawConfig &config, bool front,
            unsigned shape, uint8_t flat)
{
    const bool culled = config.cull_mode &
        (front ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT);
    const bool empty = culled || shape == 4 || config.stipple_pattern == 0;
    unsigned covered = 0;
    std::array<std::array<double, 2>, 3> positions{{{16, 16}, {48, 16}, {16, 48}}};
    if (config.negative_viewport)
        for (auto &position : positions)
            position[1] = 64 - position[1];
    for (uint32_t y = 0; y < config.height; ++y) {
        for (uint32_t x = 0; x < config.width; ++x) {
            const auto &pixel = result.pixels[y * config.width + x];
            const uint8_t stencil = config.stencil ? result.stencil[y * config.width + x] : 0;
            if (empty) {
                require(pixel == std::array<uint8_t, 4>{} && stencil == 0,
                        "culled or stipple-rejected fragment updated color/stencil");
                continue;
            }
            if (pixel[3]) {
                ++covered;
                require(pixel[0] == (front ? 255 : 0), "polygon line lost FrontFacing");
                if (config.line_mode != VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_KHR)
                    require(std::abs(int(pixel[3]) - int(flat)) <= 1,
                            "polygon line changed flat provoking value");
                require(!config.stencil || stencil == (front ? 1 : 2),
                        "polygon line used incorrect stencil face");
            }
            if (config.stencil && config.line_mode != VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_KHR)
                require(bool(stencil) == bool(pixel[3]), "early stencil affected a rejected line fragment");
            if (shape == 0 && config.line_mode == VK_LINE_RASTERIZATION_MODE_BRESENHAM_KHR &&
                config.stipple_pattern == 0xffff) {
                bool expected = false;
                for (unsigned edge = 0; edge < 3; ++edge)
                    expected |= diamond_exit(positions[edge], positions[(edge + 1) % 3], x, y);
                require(bool(pixel[3]) == expected, "diamond-exit coverage mismatch at " +
                        std::to_string(x) + "," + std::to_string(y));
            }
        }
    }
    require(empty || covered != 0, "nonempty polygon line produced no coverage");
    if (config.transform_feedback_words) {
        std::array<uint32_t, 16> expected{};
        for (unsigned vertex = 0; vertex < 3; ++vertex) {
            expected[vertex * 4] = vertex;
            expected[vertex * 4 + 1] = 7;
            expected[vertex * 4 + 2] = 11;
            expected[vertex * 4 + 3] = 13;
        }
        require(std::equal(expected.begin(), expected.end(), result.transform_feedback.begin()),
                "line clipping/stipple changed transform feedback");
    }
}
} // namespace

int main(int argc, char **argv)
{
    std::string current;
    try {
        require(argc <= 2, "expected at most one exact stage selector");
        DrawContext context;
        require_venus_when_asked(context);
        if (argc == 2 && std::string_view(argv[1]) == "endpoints") {
            std::printf("Line endpoint probe: %u draws passed\n", verify_endpoints(context));
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "smooth-caps") {
            std::printf("Smooth cap probe: %u draws passed\n", verify_smooth_caps(context));
            return 0;
        }
        if (argc == 2 && (std::string_view(argv[1]) == "performance" ||
                         std::string_view(argv[1]) == "performance-whole-submit")) {
            measure_line_scaling(context, std::string_view(argv[1]) == "performance-whole-submit");
            return 0;
        }
        const char *stages[] = {"vs", "gs", "tes", "tes-gs", "xfb"};
        struct Shaders {
            std::span<const uint32_t> vertex, capture, geometry, control, evaluation;
            unsigned shape;
        };
#define SHADERS(suffix, shape) Shaders{polygon_vertex##suffix, polygon_capture##suffix, \
    polygon_geometry##suffix, polygon_control##suffix, polygon_evaluation##suffix, shape}
        const Shaders shapes[] = {SHADERS(, 0), SHADERS(_1, 1), SHADERS(_4, 4)};
#undef SHADERS
        const VkLineRasterizationModeKHR modes[] = {VK_LINE_RASTERIZATION_MODE_BRESENHAM_KHR,
            VK_LINE_RASTERIZATION_MODE_RECTANGULAR_KHR, VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_KHR};
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
                    config.vertex_shader = stage == 4 ? std::span<const uint32_t>(polygon_capture)
                                                       : std::span<const uint32_t>(polygon_vertex);
                    config.fragment_shader = polygon_facing;
                    config.topology = stage == 2 || stage == 3 ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST
                                                               : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                    if (stage == 1 || stage == 3)
                        config.geometry_shader = polygon_geometry;
                    if (stage == 2 || stage == 3) {
                        config.tess_control_shader = polygon_control;
                        config.tess_eval_shader = polygon_evaluation;
                    }
                    config.transform_feedback_words = stage == 4 ? 16 : 0;
                    config.negative_viewport = negative;
                    config.front_face = winding;
                    config.provoking_vertex = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
                    const auto reference = context.draw(config);
                    const auto inside = reference.pixels[(negative ? 40 : 24) * 64 + 24];
                    require(inside[3] != 0, "native reference polygon missing");
                    const bool front = inside[0] == 255;
                    config.polygon_mode = VK_POLYGON_MODE_LINE;
                    config.fragment_shader = polygon_early_facing;
                    config.stencil = true;
                    config.stipple = true;
                    for (const auto mode : modes) {
                        config.line_mode = mode;
                        for (const auto &shape : shapes) {
                            config.vertex_shader = stage == 4 ? shape.capture : shape.vertex;
                            if (stage == 1 || stage == 3)
                                config.geometry_shader = shape.geometry;
                            if (stage == 2 || stage == 3) {
                                config.tess_control_shader = shape.control;
                                config.tess_eval_shader = shape.evaluation;
                            }
                            for (uint16_t pattern : {0, 0x5555, 0xffff}) {
                                config.stipple_pattern = pattern;
                                config.cull_mode = pattern == 0x5555 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
                                config.indirect = negative;
                                config.indices = winding == VK_FRONT_FACE_CLOCKWISE ? std::span(indices)
                                                                                     : std::span<const uint32_t>();
                                current = std::string(stages[stage]) + "-v" + std::to_string(negative) +
                                    "-f" + std::to_string(winding) + "-m" + std::to_string(mode) +
                                    "-s" + std::to_string(shape.shape) + "-p" + std::to_string(pattern);
                                std::printf("Line raster %s\n", current.c_str());
                                std::fflush(stdout);
                                const auto cold = context.draw(config);
                                verify(cold, config, front, shape.shape, inside[3]);
                                DrawContext cache_context;
                                const auto reloaded = cache_context.draw(config, cold.pipeline_cache);
                                verify(reloaded, config, front, shape.shape, inside[3]);
                                require(cold.pixels == reloaded.pixels && cold.stencil == reloaded.stencil,
                                        "fresh-device cache changed line results");
                                completed += 2;
                                if (shape.shape == 0 && mode == VK_LINE_RASTERIZATION_MODE_BRESENHAM_KHR &&
                                    pattern == 0xffff) {
                                    config.stencil = false;
                                    config.samples = VK_SAMPLE_COUNT_4_BIT;
                                    config.sample_locations = locations;
                                    const auto msaa = context.draw(config);
                                    verify(msaa, config, front, shape.shape, inside[3]);
                                    require(msaa.pixels == cold.pixels, "Bresenham MSAA did not cover all samples");
                                    config.samples = VK_SAMPLE_COUNT_1_BIT;
                                    config.sample_locations = {};
                                    config.stencil = true;
                                    ++completed;
                                }
                            }
                        }
                    }
                }
            }
        }
        require(completed != 0, "stage selector matched no cases");
        if (argc == 1)
            completed += verify_endpoints(context) + verify_smooth_caps(context);
        std::printf("Line raster probe: %u draws passed\n", completed);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Line raster probe failed %s: %s\n", current.c_str(), error.what());
        return 1;
    }
}
