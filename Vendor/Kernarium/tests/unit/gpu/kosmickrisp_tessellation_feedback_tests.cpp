// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan_draw_test.hpp"
#include "shaders/tessellation_feedback_spirv.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using uml::testing::require;
using namespace uml::gpu::test;
constexpr unsigned stride = 16;

struct Case {
    std::string name;
    unsigned primitive = 0;
    unsigned level = 1;
    unsigned form = 0;
    unsigned patches = 2;
    unsigned instances = 2;
    unsigned repeats = 2;
    float z = 0.5f;
    float clip = 1;
    float cull = 1;
    bool discard = false;
    bool generated = false;
    bool cull_faces = false;
    bool cache_reload = false;
    bool provoking_last = true;
    unsigned incomplete_vertices = 0;
    bool clockwise = false;
    bool lower_left = false;
    bool flat_fragment = true;
    bool packed = false;
    bool stipple = true;
};

std::span<const uint32_t> evaluation(const Case &c, bool capture)
{
    using namespace tessellation_feedback;
    if (c.packed && capture) {
        if (c.primitive == 0) return points_packed_capture;
        if (c.primitive == 1) return lines_packed_capture;
        if (c.primitive == 2) return triangles_packed_capture;
        return quads_packed_capture;
    }
    if (c.primitive == 0)
        return capture ? std::span<const uint32_t>(points_capture) : points;
    if (c.primitive == 1)
        return capture ? std::span<const uint32_t>(lines_capture) : lines;
    if (c.primitive == 3) {
        if (c.clockwise)
            return capture ? std::span<const uint32_t>(quads_cw_capture) : quads_cw;
        return capture ? std::span<const uint32_t>(quads_capture) : quads;
    }
    if (c.clockwise)
        return capture ? std::span<const uint32_t>(triangles_cw_capture) : triangles_cw;
    return capture ? std::span<const uint32_t>(triangles_capture) : triangles;
}

unsigned primitive_vertices(const Case &c)
{
    return std::min(c.primitive + 1, 3u);
}

unsigned vertices_per_patch(const Case &c)
{
    if (!c.level || c.incomplete_vertices) return 0;
    if (c.primitive == 0) return c.level == 1 ? 3 : 7;
    if (c.primitive == 1) return c.level == 1 ? 2 : 8;
    if (c.primitive == 3) return c.level == 1 ? 6 : 24;
    return c.level == 1 ? 3 : 18;
}

void validate_records(const Case &c, const DrawResult &result, unsigned vertices)
{
    const unsigned per_patch = vertices_per_patch(c);
    const unsigned arity = primitive_vertices(c);
    const auto value = [&](unsigned vertex, unsigned component) {
        return std::bit_cast<float>(result.transform_feedback.at(vertex * stride + component));
    };
    double signed_area = 0;
    for (unsigned v = 0; v < vertices; ++v) {
        const unsigned sequence_patch = v / per_patch;
        const unsigned patch = sequence_patch % c.patches;
        const unsigned instance = (sequence_patch / c.patches) % c.instances + 1;
        const float u = value(v, 9), t = value(v, 10), w = value(v, 11);
        require(std::isfinite(u) && std::isfinite(t) && std::isfinite(w) &&
                    u >= 0 && u <= 1 && t >= 0 && t <= 1 &&
                    (c.primitive == 1 || c.primitive == 3 ? w == 0
                        : std::abs(u + t + w - 1) < 0.00001),
                c.name + ": invalid captured domain coordinate");
        const bool raw_matches = value(v, 0) == u - 0.5f && value(v, 1) == t - 0.5f &&
                    value(v, 2) == c.z && value(v, 3) == 1 && value(v, 4) == 5 &&
                    value(v, 5) == 1 && value(v, 6) == c.clip &&
                    value(v, 7) == 1 && value(v, 8) == c.cull;
        if (!raw_matches) {
            std::printf("RAW vertex=%u expected_patch=%u instance=%u uv=%g,%g words=", v, patch, instance, u, t);
            for (unsigned word = 0; word < stride; ++word)
                std::printf(" %08x", result.transform_feedback[v * stride + word]);
            std::puts("");
        }
        require(raw_matches,
                c.name + ": raw position/size/clip/cull output modified");
        const bool identity_matches = result.transform_feedback[v * stride + 12] == patch &&
                    result.transform_feedback[v * stride + 13] == instance &&
                    result.transform_feedback[v * stride + 14] ==
                        (0x12340000u | (unsigned(u * 64) + unsigned(t * 64) * 128)) &&
                    result.transform_feedback[v * stride + 15] == 0;
        if (!identity_matches) {
            std::printf("RECORD vertex=%u expected_patch=%u instance=%u uv=%g,%g words=", v, patch, instance, u, t);
            for (unsigned word = 0; word < stride; ++word)
                std::printf(" %08x", result.transform_feedback[v * stride + word]);
            std::puts("");
        }
        require(identity_matches,
                c.name + ": patch/instance/primitive order or stride padding changed");
        if (c.primitive == 1 && v % arity == 1)
            require(value(v - 1, 10) == t && value(v - 1, 9) < u,
                    c.name + ": isoline endpoint order changed");
        if (c.primitive >= 2 && v % arity == 2) {
            const float a = value(v - 2, 9), b = value(v - 2, 10);
            const float d = value(v - 1, 9), e = value(v - 1, 10);
            const double area = a * e - d * b + d * t - u * e + u * b - a * t;
            const double direction = c.clockwise != c.lower_left ? 1 : -1;
            require(area * direction > 0, c.name + ": tessellated triangle winding changed");
            signed_area += area;
            if (v % per_patch == per_patch - 1) {
                require(std::abs(signed_area - direction * (c.primitive == 3 ? 2 : 1)) < 0.0001,
                        c.name + ": tessellated triangles do not cover the domain");
                signed_area = 0;
            }
        }
    }
}

// An independent capture/replay oracle: each filled triangle's interior must
// carry its own captured first/last flat value, not just match another pipeline.
void validate_provoking(const Case &c, const DrawResult &result, unsigned vertices)
{
    if (c.primitive < 2 || !c.flat_fragment || c.generated || c.discard || c.cull_faces ||
        c.z < 0 || c.z > 1 || c.clip < 0 || c.cull < 0)
        return;
    for (unsigned vertex = 0; vertex < std::min(vertices, vertices_per_patch(c)); vertex += 3) {
        float x = 0, y = 0;
        for (unsigned corner = 0; corner < 3; ++corner) {
            x += std::bit_cast<float>(result.transform_feedback[(vertex + corner) * stride]);
            y += std::bit_cast<float>(result.transform_feedback[(vertex + corner) * stride + 1]);
        }
        const unsigned column = unsigned((x / 6 + 0.5f) * 64);
        const unsigned row = unsigned((y / 6 + 0.5f) * 64);
        const auto pixel = result.pixels.at(row * 64 + column);
        const unsigned provoking = vertex + (c.provoking_last ? 2 : 0);
        const unsigned blue = result.transform_feedback[provoking * stride + 14] & 255;
        require(pixel[3] == 255 && std::abs(int(pixel[2]) - int(blue)) <= 1,
                c.name + ": raster flat value differs from captured provoking vertex");
    }
}

unsigned run(const Case &c)
{
    DrawContext context;
    require(context.capabilities().driver_id == VK_DRIVER_ID_MESA_KOSMICKRISP &&
                context.capabilities().transform_feedback && context.capabilities().pipeline_statistics &&
                context.capabilities().transform_feedback_preserves_provoking_vertex,
            "requires native KosmicKrisp transform feedback and statistics");
    DrawConfig config;
    config.vertex_shader = tessellation_feedback::vertex;
    config.tess_control_shader = tessellation_feedback::control;
    config.tess_eval_shader = evaluation(c, false);
    config.tessellation_origin = c.lower_left ? VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT
                                             : VK_TESSELLATION_DOMAIN_ORIGIN_UPPER_LEFT;
    config.fragment_shader = c.flat_fragment ? std::span<const uint32_t>(tessellation_feedback::fragment)
                                            : tessellation_feedback::constant_fragment;
    config.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    config.vertex_count = 3 * c.patches;
    if (c.incomplete_vertices) config.vertex_count = c.incomplete_vertices;
    config.instance_count = c.instances;
    config.first_instance = 1;
    config.repeat_count = c.repeats;
    config.indirect = (c.form & 1) != 0;
    const std::array<uint32_t, 6> indices{2, 0, 1, 5, 3, 4};
    if (c.form & 2) config.indices = std::span(indices).first(config.vertex_count);
    config.pipeline_statistics = 0x39f;
    config.rasterizer_discard = c.discard;
    config.cull_mode = c.cull_faces ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    config.front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    config.provoking_vertex = c.provoking_last ? VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT
                                              : VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT;
    config.pipeline_library = c.cache_reload;
    if (c.generated) {
        config.polygon_mode = c.primitive >= 2 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        config.line_mode = VK_LINE_RASTERIZATION_MODE_RECTANGULAR_KHR;
        config.stipple = c.stipple;
    }
    std::array<uint32_t, 64> data{};
    data[0] = c.level;
    data[1] = c.patches;
    data[2] = std::bit_cast<uint32_t>(c.z);
    data[3] = std::bit_cast<uint32_t>(c.clip);
    data[4] = std::bit_cast<uint32_t>(c.cull);
    config.counters = data;
    const auto reference = context.draw(config);
    const unsigned input_patches = config.vertex_count / 3 * c.instances * c.repeats;
    require(reference.counters[5] == config.vertex_count * c.instances * c.repeats &&
                reference.counters[6] == input_patches * 3 && reference.counters[7] == input_patches,
            c.name + ": native VS/TCS dispatch exceeded or lost input invocations");
    const unsigned vertices = vertices_per_patch(c) * c.patches * c.instances * c.repeats;
    const unsigned arity = primitive_vertices(c);
    const unsigned primitives = vertices / arity;
    const bool visible = vertices && !c.discard &&
        c.z >= 0 && c.z <= 1 && c.clip >= 0 && c.cull >= 0;
    if (!c.cull_faces)
        require(std::any_of(reference.pixels.begin(), reference.pixels.end(),
                           [](auto pixel) { return pixel[3] != 0; }) == visible,
                c.name + ": no-XFB coverage control failed");

    config.tess_eval_shader = evaluation(c, true);
    config.transform_feedback_words = vertices * stride + 4;
    config.transform_feedback_query = true;
    const auto full = context.draw(config, reference.pipeline_cache);
    const auto validate = [&](const DrawResult &actual, unsigned written) {
        if (actual.counters != reference.counters) {
            std::printf("EFFECTS expected:");
            for (auto word : reference.counters) std::printf(" %u", word);
            std::printf("\nEFFECTS actual:");
            for (auto word : actual.counters) std::printf(" %u", word);
            std::puts("");
        }
        require(actual.counters == reference.counters,
                c.name + ": capture duplicated or lost application effects");
        require(actual.statistics == actual.copied_statistics &&
                    actual.statistics.size() == 9 && actual.statistics[3] == 0 && actual.statistics[4] == 0,
                c.name + ": host/GPU statistics differ or synthetic GS counted");
        for (unsigned counter : {0u, 1u, 2u, 6u, 7u, 8u})
            require(actual.statistics[counter] == reference.statistics[counter],
                    c.name + ": capture changed logical application-stage statistics");
        if (actual.pixels != reference.pixels) {
            std::array<unsigned, 4> mismatches{};
            unsigned shown = 0;
            for (unsigned pixel = 0; pixel < actual.pixels.size(); ++pixel) {
                for (unsigned channel = 0; channel < 4; ++channel)
                    mismatches[channel] += actual.pixels[pixel][channel] != reference.pixels[pixel][channel];
                if (actual.pixels[pixel] != reference.pixels[pixel] && shown++ < 8)
                    std::printf("PIXEL %u,%u reference=%u,%u,%u,%u capture=%u,%u,%u,%u\n",
                        pixel % 64, pixel / 64,
                        reference.pixels[pixel][0], reference.pixels[pixel][1], reference.pixels[pixel][2], reference.pixels[pixel][3],
                        actual.pixels[pixel][0], actual.pixels[pixel][1], actual.pixels[pixel][2], actual.pixels[pixel][3]);
            }
            std::printf("PIXEL mismatches RGBA=%u,%u,%u,%u\n", mismatches[0], mismatches[1], mismatches[2], mismatches[3]);
        }
        require(actual.transform_feedback_query == actual.copied_transform_feedback_query &&
                    actual.transform_feedback_query == std::vector<uint64_t>{written / arity, primitives, 1},
                c.name + ": XFB query expected " + std::to_string(written / arity) + "/" +
                    std::to_string(primitives) + " got " +
                    std::to_string(actual.transform_feedback_query.at(0)) + "/" +
                    std::to_string(actual.transform_feedback_query.at(1)) +
                    " availability=" + std::to_string(actual.transform_feedback_query.at(2)));
        require(actual.transform_feedback_bytes_written == written * stride * sizeof(uint32_t),
                c.name + ": final XFB byte counter mismatch");
        validate_records(c, actual, written);
        validate_provoking(c, actual, written);
        require(actual.pixels == reference.pixels,
                c.name + ": capture changed rasterization");
        require(std::all_of(actual.transform_feedback.begin() + written * stride,
                            actual.transform_feedback.end(), [](auto word) { return word == 0; }),
                c.name + ": incomplete primitive or out-of-range bytes written");
    };
    validate(full, vertices);
    unsigned draws = 2;
    // Test no complete primitive, and capacity ending part-way through a later
    // primitive. Padding remains unwritten even when XFB reports overflow.
    for (unsigned capacity : {arity * stride - 2, (arity + 1) * stride - 2}) {
        config.transform_feedback_words = capacity;
        const auto limited = context.draw(config, full.pipeline_cache);
        const unsigned written = std::min(vertices, (capacity / (arity * stride)) * arity);
        validate(limited, written);
        require(std::equal(limited.transform_feedback.begin(), limited.transform_feedback.begin() + written * stride,
                           full.transform_feedback.begin()), c.name + ": bounded XFB changed the captured prefix");
        ++draws;
    }
    if (c.cache_reload) {
        DrawContext fresh;
        config.transform_feedback_words = vertices * stride + 4;
        const auto reloaded = fresh.draw(config, full.pipeline_cache);
        validate(reloaded, vertices);
        require(reloaded.transform_feedback == full.transform_feedback,
                c.name + ": fresh-device cached capture changed records");
        ++draws;
    }
    std::printf("CASE %s PASS vertices=%u primitives=%u draws=%u\n",
                c.name.c_str(), vertices, primitives, draws);
    return draws;
}

unsigned run_packed(const Case &c)
{
    DrawContext context;
    DrawConfig config;
    config.vertex_shader = tessellation_feedback::vertex;
    config.tess_control_shader = tessellation_feedback::control;
    config.tess_eval_shader = evaluation(c, false);
    config.fragment_shader = tessellation_feedback::fragment;
    config.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    config.vertex_count = 6;
    config.instance_count = 2;
    config.first_instance = 1;
    config.repeat_count = 2;
    config.indirect = (c.form & 1) != 0;
    const std::array<uint32_t, 6> indices{2, 0, 1, 5, 3, 4};
    if (c.form & 2) config.indices = indices;
    config.provoking_vertex = c.provoking_last ? VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT
                                              : VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT;
    config.pipeline_statistics = 0x39f;
    if (c.generated) {
        config.polygon_mode = c.primitive >= 2 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        config.line_mode = VK_LINE_RASTERIZATION_MODE_RECTANGULAR_KHR;
        config.stipple = c.stipple;
    }
    std::array<uint32_t, 64> data{};
    data[0] = c.level;
    data[1] = 2;
    data[2] = std::bit_cast<uint32_t>(0.5f);
    data[3] = data[4] = std::bit_cast<uint32_t>(1.0f);
    config.counters = data;
    const auto reference = context.draw(config);
    const unsigned vertices = vertices_per_patch(c) * 8;
    const unsigned arity = primitive_vertices(c);
    Case ordinary = c;
    ordinary.packed = false;
    config.tess_eval_shader = evaluation(ordinary, true);
    config.transform_feedback_words = vertices * stride + 4;
    const auto ordered = context.draw(config, reference.pipeline_cache);
    validate_records(c, ordered, vertices);
    validate_provoking(c, ordered, vertices);
    require(ordered.pixels == reference.pixels && ordered.counters == reference.counters,
            c.name + ": ordered capture control changed raster/effects");

    config.tess_eval_shader = evaluation(c, true);
    config.transform_feedback_words = 0;
    config.transform_feedback_query = true;
    constexpr std::array<unsigned, 2> strides{7, 11};
    constexpr std::array<unsigned, 2> output_ends{6, 8};
    for (unsigned limited = 0; limited < 3; ++limited) {
        std::array<TransformFeedbackBinding, 2> bindings{{
            {vertices * strides[0] + 16, 3, vertices * strides[0], 0x5ac39e71},
            {vertices * strides[1] + 20, 5, vertices * strides[1], 0xa5368cd2},
        }};
        unsigned written = vertices;
        if (limited) {
            const unsigned binding = limited - 1;
            // End one word before the final output of primitive two. Each
            // buffer independently limits the shared stream to one primitive.
            bindings[binding].size_words =
                (2 * arity - 1) * strides[binding] + output_ends[binding] - 1;
            written = arity;
        }
        config.transform_feedback_bindings = bindings;
        const auto actual = context.draw(config, ordered.pipeline_cache);
        const std::string label = c.name + ": packed limit " + std::to_string(limited);
        require(actual.counters == reference.counters, label + " changed application effects");
        require(actual.statistics == actual.copied_statistics && actual.statistics.size() == 9,
                label + " host/GPU statistics differ");
        require(actual.statistics[3] == 0 && actual.statistics[4] == 0,
                label + " counted synthetic GS invocations");
        if (actual.pixels != reference.pixels) {
            unsigned shown = 0;
            for (unsigned pixel = 0; pixel < actual.pixels.size(); ++pixel) {
                if (actual.pixels[pixel] != reference.pixels[pixel] && shown++ < 8)
                    std::printf("PIXEL %u,%u reference=%u,%u,%u,%u packed=%u,%u,%u,%u\n",
                        pixel % 64, pixel / 64,
                        reference.pixels[pixel][0], reference.pixels[pixel][1], reference.pixels[pixel][2], reference.pixels[pixel][3],
                        actual.pixels[pixel][0], actual.pixels[pixel][1], actual.pixels[pixel][2], actual.pixels[pixel][3]);
            }
            std::printf("PIXEL mismatches=%u\n", shown);
        }
        for (unsigned counter : {0u, 1u, 2u, 6u, 7u, 8u})
            require(actual.statistics[counter] == reference.statistics[counter],
                    c.name + ": packed capture changed logical application counts");
        require(actual.transform_feedback_query == actual.copied_transform_feedback_query &&
                    actual.transform_feedback_query == std::vector<uint64_t>{written / arity, vertices / arity, 1} &&
                    actual.transform_feedback_bytes_written_per_buffer ==
                        std::vector<uint32_t>{written * strides[0] * 4, written * strides[1] * 4},
                c.name + ": packed capture query/counter mismatch");
        require(actual.transform_feedback_buffers.size() == 2, c.name + ": missing packed buffer");
        for (unsigned binding = 0; binding < 2; ++binding) {
            std::vector<uint32_t> expected(bindings[binding].words, bindings[binding].initial_word);
            for (unsigned vertex = 0; vertex < written; ++vertex) {
                const unsigned base = bindings[binding].offset_words + vertex * strides[binding];
                const unsigned original = vertex * stride + (binding ? 12 : 9);
                for (unsigned component = 0; component < 3; ++component)
                    expected[base + (binding ? 2 : 1) + component] = ordered.transform_feedback[original + component];
                const float coord = std::bit_cast<float>(ordered.transform_feedback[vertex * stride + 9 + binding]);
                expected[base + (binding ? 7 : 5)] = std::bit_cast<uint32_t>(coord + (binding ? 0.5f : 0.25f));
            }
            const auto &buffer = actual.transform_feedback_buffers[binding];
            if (buffer != expected) {
                const auto mismatch = std::mismatch(buffer.begin(), buffer.end(),
                                                    expected.begin(), expected.end());
                const auto word = unsigned(mismatch.first - buffer.begin());
                std::printf("PACKED limit=%u binding=%u word=%u expected=%08x actual=%08x\n",
                            limited, binding, word, expected.at(word), buffer.at(word));
            }
            require(buffer == expected,
                    c.name + ": packed component, binding/stride offset, or untouched guard changed");
        }
        require(actual.pixels == reference.pixels, label + " changed rasterization");
    }
    std::printf("CASE %s PASS vertices=%u two_buffers=3 draws=5\n", c.name.c_str(), vertices);
    return 5;
}
} // namespace

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    try {
        require(argc == 1 || (argc == 3 && std::string(argv[1]) == "--case"),
                "usage: tessellation-feedback-tests [--case NAME]");
        const std::string selected = argc == 3 ? argv[2] : "";
        std::vector<Case> cases;
        for (unsigned primitive = 0; primitive < 4; ++primitive) {
            const std::string prefix = primitive == 0 ? "points" : primitive == 1 ? "lines"
                : primitive == 2 ? "triangles" : "quads";
            for (unsigned level : {1u, 2u}) {
                for (unsigned form = 0; form < 4; ++form)
                    cases.push_back({prefix + "_level" + std::to_string(level) + "_form" + std::to_string(form), primitive, level, form});
            }
            for (unsigned condition = 0; condition < 6; ++condition) {
                Case c{prefix + "_control" + std::to_string(condition), primitive};
                c.level = condition == 0 ? 0 : 1;
                c.clip = condition == 1 ? -1 : 1;
                c.cull = condition == 2 ? -1 : 1;
                c.z = condition == 3 ? -0.5f : 0.5f;
                c.discard = condition == 4;
                c.cache_reload = condition == 5;
                cases.push_back(c);
            }
            for (bool zero_instances : {false, true}) {
                Case c{prefix + (zero_instances ? "_empty_instances" : "_empty_vertices"), primitive};
                c.form = 1;
                if (zero_instances) c.instances = 0;
                else c.patches = 0;
                cases.push_back(c);
            }
            if (primitive) {
                Case c{prefix + "_generated", primitive, 2};
                c.generated = true;
                cases.push_back(c);
                c.name += "_no_flat";
                c.flat_fragment = false;
                cases.push_back(c);
            }
            if (primitive >= 2) {
                Case c{prefix + "_cull", primitive};
                c.cull_faces = true;
                cases.push_back(c);
                c = Case{prefix + "_first", primitive};
                c.provoking_last = false;
                cases.push_back(c);
                c = Case{prefix + "_no_flat", primitive, 2};
                c.flat_fragment = false;
                cases.push_back(c);
                for (bool clockwise : {false, true}) {
                    for (bool lower_left : {false, true}) {
                        for (bool last : {false, true}) {
                            c = Case{prefix + (clockwise ? "_cw" : "_ccw") +
                                (lower_left ? "_lower" : "_upper") + (last ? "_last" : "_first"), primitive, 2};
                            c.clockwise = clockwise;
                            c.lower_left = lower_left;
                            c.provoking_last = last;
                            cases.push_back(c);
                            c.name += "_generated";
                            c.generated = true;
                            cases.push_back(c);
                        }
                    }
                }
            } else {
                for (unsigned form : {0u, 1u, 2u, 3u}) {
                    for (unsigned vertices : {1u, 2u}) {
                        Case c{prefix + "_incomplete" + std::to_string(vertices) + "_form" + std::to_string(form), primitive};
                        c.form = form;
                        c.incomplete_vertices = vertices;
                        cases.push_back(c);
                    }
                }
            }
        }
        for (unsigned primitive = 0; primitive < 4; ++primitive) {
            Case c{"packed_" + std::to_string(primitive), primitive, 2, 3};
            c.packed = true;
            c.generated = primitive == 1;
            cases.push_back(c);
        }
        Case packed_generated{"packed_generated", 2, 2, 3};
        packed_generated.packed = packed_generated.generated = true;
        cases.push_back(packed_generated);
        Case packed_line{"packed_lines_unstippled", 1, 2, 3};
        packed_line.packed = packed_line.generated = true;
        packed_line.stipple = false;
        cases.push_back(packed_line);
        packed_line.name = "packed_lines_first";
        packed_line.stipple = true;
        packed_line.provoking_last = false;
        cases.push_back(packed_line);
        unsigned passed = 0, draws = 0;
        for (const auto &c : cases) {
            if (!selected.empty() && c.name != selected) continue;
            std::printf("BEGIN %s\n", c.name.c_str());
            draws += c.packed ? run_packed(c) : run(c);
            ++passed;
        }
        require(passed, "selector matched no cases");
        std::printf("SUMMARY tessellation_feedback_pass=%u draws=%u fail=0\n", passed, draws);
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
