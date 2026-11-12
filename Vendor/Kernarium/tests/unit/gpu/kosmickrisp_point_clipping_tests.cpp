// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan_draw_test.hpp"
#include "shaders/point_clipping_spirv.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using uml::testing::require;
using namespace uml::gpu::test;
using Position = std::array<float, 4>;

std::string verify_properties()
{
    auto application = structure<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
    application.pApplicationName = "native point clipping regression";
    application.apiVersion = VK_API_VERSION_1_3;
    auto info = structure<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
    info.pApplicationInfo = &application;
    VkInstance instance = VK_NULL_HANDLE;
    require(instance_function<PFN_vkCreateInstance>(nullptr, "vkCreateInstance")(
                &info, nullptr, &instance) == VK_SUCCESS, "vkCreateInstance");
    struct InstanceCleanup {
        VkInstance value;
        ~InstanceCleanup()
        {
            instance_function<PFN_vkDestroyInstance>(value, "vkDestroyInstance")(value, nullptr);
        }
    } cleanup{instance};
    auto enumerate = instance_function<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices");
    uint32_t count = 0;
    require(enumerate(instance, &count, nullptr) == VK_SUCCESS && count != 0,
            "physical device count");
    std::vector<VkPhysicalDevice> devices(count);
    require(enumerate(instance, &count, devices.data()) == VK_SUCCESS, "physical devices");
    auto query = instance_function<PFN_vkGetPhysicalDeviceProperties2>(instance, "vkGetPhysicalDeviceProperties2");
    auto driver = structure<VkPhysicalDeviceDriverProperties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES);
    auto dedicated = structure<VkPhysicalDevicePointClippingProperties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES);
    dedicated.pointClippingBehavior = VK_POINT_CLIPPING_BEHAVIOR_MAX_ENUM;
    dedicated.pNext = &driver;
    auto properties = structure<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
    properties.pNext = &dedicated;
    query(devices.front(), &properties);
    require(driver.driverID == VK_DRIVER_ID_MESA_KOSMICKRISP, "wrong native Vulkan provider");
    auto aggregate = structure<VkPhysicalDeviceVulkan11Properties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES);
    aggregate.pointClippingBehavior = VK_POINT_CLIPPING_BEHAVIOR_MAX_ENUM;
    properties.pNext = &aggregate;
    query(devices.front(), &properties);
    // Metal keeps a point whose centre leaves the view volume as long as the
    // point's size still reaches the viewport, which is the rule the
    // conformance reference rasterizer and OpenGL apply, so the provider clips
    // a point against user planes and the depth planes only.
    require(dedicated.pointClippingBehavior == VK_POINT_CLIPPING_BEHAVIOR_USER_CLIP_PLANES_ONLY &&
                aggregate.pointClippingBehavior == VK_POINT_CLIPPING_BEHAVIOR_USER_CLIP_PLANES_ONLY,
            "both property spellings must report USER_CLIP_PLANES_ONLY");
    auto features = structure<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    auto depth = structure<VkPhysicalDeviceDepthClipEnableFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT);
    features.pNext = &depth;
    instance_function<PFN_vkGetPhysicalDeviceFeatures2>(instance, "vkGetPhysicalDeviceFeatures2")(
        devices.front(), &features);
    require(features.features.largePoints && features.features.geometryShader &&
                features.features.tessellationShader && features.features.shaderTessellationAndGeometryPointSize &&
                features.features.shaderClipDistance && features.features.shaderCullDistance &&
                features.features.depthClamp && depth.depthClipEnable,
            "required advertised point stages/controls unavailable");
    require(properties.properties.limits.pointSizeRange[0] <= 1.0f &&
                properties.properties.limits.pointSizeRange[1] >= 511.0f &&
                properties.properties.limits.maxClipDistances >= 8 &&
                properties.properties.limits.maxCombinedClipAndCullDistances >= 8,
            "required point sizes or eight application clip planes unavailable");
    std::printf("PROPERTIES dedicated=%u vulkan11=%u device=%s driver=%s\n",
                unsigned(dedicated.pointClippingBehavior), unsigned(aggregate.pointClippingBehavior),
                properties.properties.deviceName, driver.driverName);
    return properties.properties.deviceName;
}

struct Case {
    std::string name;
    Position position{0, 0, 0.5f, 1};
    float size = 48;
    float clip_distance = 1;
    bool visible = true;
    bool depth_clip = true;
    bool depth_clamp = false;
    bool capture_depth = false;
    float min_depth = 0;
    float max_depth = 1;
    std::optional<float> expected_depth = std::nullopt;
    Position upstream_position{0, 0, 0.5f, 1};
    float upstream_clip = 1;
    unsigned clip_planes = 1;
    unsigned selected_plane = 0;
    bool cull = false;
    bool preclip = false;
    bool partial = false;
    unsigned partial_branch = 0;
};

std::vector<Case> cases()
{
    std::vector<Case> result;
    result.push_back({"inside_size1", {1.0f / 64, 1.0f / 64, 0.5f, 1}, 1});
    result.push_back({"inside_size48"});
    result.push_back({"inside_w2", {0, 0, 1, 2}});
    result.push_back({"on_near_plane", {0, 0, 0, 1}});
    result.push_back({"on_far_plane", {0, 0, 1, 1}});
    // A point is not clipped against the view-volume side planes: a centre
    // just outside one still rasterizes the part of the point that reaches the
    // viewport. Only the guard band the hardware keeps around the volume -
    // measured at 1.5 in normalized device coordinates on every viewport size,
    // whatever the point size - drops such a point entirely.
    const std::array<const char *, 4> planes{"left", "right", "bottom", "top"};
    for (unsigned index = 0; index < planes.size(); ++index) {
        const float sign = index % 2 == 0 ? -1.0f : 1.0f;
        for (float magnitude : {0.9375f, 1.0f, 1.125f}) {
            Case value{std::string(planes[index]) + (magnitude < 1 ? "_inside" :
                magnitude == 1 ? "_on_plane" : "_outside_overlap")};
            value.position[index / 2] = sign * magnitude;
            result.push_back(value);
        }
    }
    result.push_back(Case{"left_outside_w2", {-2.25f, 0, 1, 2}});
    for (bool far : {false, true}) {
        for (bool clip : {false, true}) {
            for (bool clamp : {false, true}) {
                Case value{std::string(far ? "far" : "near") + "_clip" +
                           (clip ? "1" : "0") + "_clamp" + (clamp ? "1" : "0")};
                value.position[2] = far ? 1.25f : -0.25f;
                value.visible = !clip;
                value.depth_clip = clip;
                value.depth_clamp = clamp;
                value.capture_depth = clamp;
                value.min_depth = 0.25f;
                value.max_depth = 0.75f;
                if (clamp && !clip)
                    value.expected_depth = far ? 0.75f : 0.25f;
                result.push_back(value);
            }
        }
    }
    for (float distance : {-1.0f, 0.0f, 1.0f}) {
        Case value{distance < 0 ? "user_negative" : distance == 0 ? "user_zero" : "user_positive"};
        value.clip_distance = distance;
        value.visible = distance >= 0;
        result.push_back(value);
    }
    for (bool clamp : {false, true}) {
        Case value{std::string("user_negative_depth_clip0_clamp") + (clamp ? "1" : "0")};
        value.clip_distance = -1;
        value.visible = false;
        value.depth_clip = false;
        value.depth_clamp = clamp;
        result.push_back(value);
    }
    Case none{"no_user_inside"};
    none.clip_planes = 0;
    result.push_back(none);
    for (unsigned index = 0; index < planes.size(); ++index) {
        Case outside = none;
        outside.name = std::string("no_user_") + planes[index];
        outside.position[index / 2] = index % 2 == 0 ? -1.125f : 1.125f;
        result.push_back(outside);
    }
    // Past the guard band the largest point the device advertises still
    // disappears, so the rule is a bound on the centre, not on the footprint.
    none.name = "no_user_far_corner_large_footprint";
    none.position = {-2, -2, 0.5f, 1};
    none.size = 511;
    none.visible = false;
    result.push_back(none);
    none.name = "no_user_xy_depth_clip0_clamp1";
    none.position = {-1.125f, 0, -0.25f, 1};
    none.size = 48;
    none.visible = true;
    none.depth_clip = false;
    none.depth_clamp = true;
    result.push_back(none);
    Case eight{"eight_planes_positive"};
    eight.clip_planes = 8;
    result.push_back(eight);
    for (unsigned plane = 0; plane < 8; ++plane) {
        Case negative = eight;
        negative.name = "eight_planes_negative_" + std::to_string(plane);
        negative.selected_plane = plane;
        negative.clip_distance = -1;
        negative.visible = false;
        result.push_back(negative);
    }
    eight.name = "eight_planes_zero_7";
    eight.selected_plane = 7;
    eight.clip_distance = 0;
    result.push_back(eight);
    eight.name = "eight_planes_xy_outside";
    eight.position[0] = -1.125f;
    eight.visible = true;
    eight.clip_distance = 1;
    result.push_back(eight);
    for (bool cull : {false, true}) {
        for (unsigned condition = 0; condition < 3; ++condition) {
            Case value{std::string(cull ? "cull_only_" : "preclip_effects_queries_") +
                       (condition == 0 ? "inside" : condition == 1 ? "xy_outside" : "user_negative")};
            value.cull = cull;
            value.clip_planes = cull ? 0 : 1;
            value.preclip = !cull;
            value.visible = condition != 2;
            if (condition == 1)
                value.position[0] = -1.125f;
            if (condition == 2)
                value.clip_distance = -1;
            result.push_back(value);
        }
    }
    for (unsigned branch = 0; branch < 2; ++branch) {
        for (unsigned condition = 0; condition < 3; ++condition) {
            Case value{"partial_branch" + std::to_string(branch) +
                       (condition == 0 ? "_inside" : condition == 1 ? "_xy_outside" : "_user_negative")};
            value.partial = true;
            value.partial_branch = branch;
            value.position = {condition == 1 ? 2.25f : 1.5f, 0, 1, 2};
            value.upstream_position = value.position;
            value.visible = condition != 2;
            value.clip_distance = value.upstream_clip = condition == 2 ? -1 : 1;
            result.push_back(value);
        }
    }
    return result;
}

void capture(const std::filesystem::path &directory, const std::string &name,
             const DrawResult &result)
{
    if (directory.empty())
        return;
    std::ofstream image(directory / (name + ".ppm"), std::ios::binary);
    image << "P6\n64 64\n255\n";
    for (const auto &pixel : result.pixels)
        image.write(reinterpret_cast<const char *>(pixel.data()), 3);
    require(bool(image), "image evidence write failed");
    if (!result.depth.empty()) {
        std::ofstream depth(directory / (name + ".depth-f32"), std::ios::binary);
        depth.write(reinterpret_cast<const char *>(result.depth.data()),
                    result.depth.size() * sizeof(float));
        require(bool(depth), "depth evidence write failed");
    }
}

bool run(DrawContext &context, const std::string &stage, const Case &value,
         const std::filesystem::path &directory)
{
    using namespace point_clipping;
    const bool capturing = value.preclip;
    const auto select = [&](auto &plain, auto &none, auto &eight, auto &cull,
                            auto &xfb, auto &effects, auto &partial) {
        return value.partial ? std::span<const uint32_t>(partial) :
            value.preclip ? (capturing ? std::span<const uint32_t>(xfb)
                                      : std::span<const uint32_t>(effects)) :
            value.cull ? std::span<const uint32_t>(cull) :
            value.clip_planes == 8 ? std::span<const uint32_t>(eight) :
            value.clip_planes == 0 ? std::span<const uint32_t>(none) :
            std::span<const uint32_t>(plain);
    };
    DrawConfig config;
    config.vertex_shader = stage == "vs"
        ? select(vertex, vertex_none, vertex_eight, vertex_cull,
                 vertex_capture, vertex_effects, vertex_partial)
        : std::span<const uint32_t>(upstream);
    config.fragment_shader = fragment;
    config.topology = stage == "tes" ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST
                                      : VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    config.vertex_count = stage == "tes" ? 3 : 1;
    if (stage == "gs")
        config.geometry_shader = select(geometry, geometry_none, geometry_eight,
                                        geometry_cull, geometry_capture, geometry_effects, geometry_partial);
    if (stage == "tes") {
        config.tess_control_shader = control;
        config.tess_eval_shader = select(evaluation, evaluation_none, evaluation_eight,
                                         evaluation_cull, evaluation_capture, evaluation_effects, evaluation_partial);
    }
    config.depth_clip = value.depth_clip;
    config.depth_clamp = value.depth_clamp;
    config.depth = value.capture_depth;
    config.depth_format = VK_FORMAT_D32_SFLOAT;
    config.min_depth = value.min_depth;
    config.max_depth = value.max_depth;
    std::array<uint32_t, 40> data{};
    for (unsigned component = 0; component < 4; ++component) {
        data[component] = std::bit_cast<uint32_t>(value.position[component]);
        data[16 + component] = std::bit_cast<uint32_t>(value.upstream_position[component]);
    }
    data[4] = std::bit_cast<uint32_t>(value.size);
    std::fill_n(data.begin() + 5, 8, std::bit_cast<uint32_t>(1.0f));
    data[5 + value.selected_plane] = std::bit_cast<uint32_t>(value.clip_distance);
    data[13] = std::bit_cast<uint32_t>(value.clip_distance);
    data[20] = std::bit_cast<uint32_t>(1.0f);
    data[21] = std::bit_cast<uint32_t>(value.upstream_clip);
    data[33] = value.partial_branch;
    config.counters = data;
    if (value.preclip) {
        config.transform_feedback_words = capturing ? (stage == "tes" ? 22 : 10) : 0;
        // Exclude implementation-specific clipping counters. Input/stage work
        // and raw XFB records must survive clipping, while fragments must not.
        config.pipeline_statistics = 0x39f;
    }
    const auto name = stage + "_" + value.name;
    std::printf("BEGIN %s\n", name.c_str());
    const auto image = context.draw(config);
    require(image.pixels.size() == 64 * 64, name + ": incorrect readback dimensions");
    require(!value.capture_depth || image.depth.size() == image.pixels.size(),
            name + ": missing depth readback");
    if (value.preclip) {
        const uint32_t invocations = stage == "tes" ? 3 : 1;
        data[32] = invocations;
        std::vector<uint32_t> expected(config.transform_feedback_words);
        for (unsigned index = 0; capturing && index < invocations; ++index)
            std::copy_n(data.begin(), 6, expected.begin() + index * 6);
        require(image.transform_feedback.size() == expected.size(), name + ": missing XFB readback");
        if (image.transform_feedback != expected) {
            std::printf("XFB %s effects=%u actual=", name.c_str(), image.counters.at(32));
            for (uint32_t word : image.transform_feedback)
                std::printf(" %08x", word);
            std::puts("");
            for (unsigned word = 0; word < expected.size(); ++word)
                require(image.transform_feedback[word] == expected[word], name + ": XFB word " +
                        std::to_string(word) + " expected " + std::to_string(expected[word]) +
                        " got " + std::to_string(image.transform_feedback[word]));
        }
        require(image.statistics == image.copied_statistics && image.statistics.size() == 9,
                name + ": host/GPU query results differ");
        const auto &s = image.statistics;
        require(s[0] == config.vertex_count && s[1] == 1 && s[2] == config.vertex_count &&
                    s[3] == unsigned(stage == "gs") && s[4] == unsigned(stage == "gs") &&
                    s[6] == unsigned(stage == "tes") && s[7] == (stage == "tes" ? 3u : 0u) &&
                    s[8] == 1 && (value.visible ? s[5] > 0 : s[5] == 0),
                name + ": clipping changed pre-raster work or executed clipped fragments");
        std::printf("PRECLIP %s effects=%u xfb_records=%u ia=%llu vs=%llu gs=%llu tes=%llu fs=%llu\n",
                    name.c_str(), image.counters.at(32), capturing ? invocations : 0,
                    static_cast<unsigned long long>(s[0]), static_cast<unsigned long long>(s[2]),
                    static_cast<unsigned long long>(s[3]), static_cast<unsigned long long>(s[7]),
                    static_cast<unsigned long long>(s[5]));
    }
    require(image.counters == std::vector<uint32_t>(data.begin(), data.end()),
            name + ": read-only input changed or application effects duplicated/lost");
    capture(directory, name, image);
    const float center_x = (value.position[0] / value.position[3] + 1) * 32;
    const float center_y = (value.position[1] / value.position[3] + 1) * 32;
    unsigned expected_covered = 0, covered = 0, bad_pixels = 0, bad_depth = 0;
    for (unsigned index = 0; index < image.pixels.size(); ++index) {
        const float x = float(index % 64) + 0.5f;
        const float y = float(index / 64) + 0.5f;
        const bool expected = value.visible && x >= center_x - value.size / 2 &&
            x < center_x + value.size / 2 && y >= center_y - value.size / 2 &&
            y < center_y + value.size / 2;
        expected_covered += expected;
        const auto &pixel = image.pixels[index];
        covered += pixel[3] != 0;
        const bool correct = expected ? pixel[0] == 255 && pixel[3] == 255 &&
            std::abs(int(pixel[1]) - 64) <= 1 && std::abs(int(pixel[2]) - 128) <= 1
            : pixel == std::array<uint8_t, 4>{};
        bad_pixels += !correct;
        if (value.capture_depth) {
            const float wanted = expected ? value.expected_depth.value_or(1.0f) : 1.0f;
            bad_depth += !std::isfinite(image.depth[index]) ||
                std::abs(image.depth[index] - wanted) > 0.00001f;
        }
    }
    const bool passed = bad_pixels == 0 && bad_depth == 0;
    std::printf("CASE %s status=%s expected_covered=%u covered=%u bad_pixels=%u bad_depth=%u\n",
                name.c_str(), passed ? "PASS" : "FAIL", expected_covered, covered,
                bad_pixels, bad_depth);
    return passed;
}

} // namespace

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    try {
        require(argc == 1 || argc == 2 ||
                    ((argc == 3 || argc == 4) && std::string(argv[1]) == "--case"),
                "usage: point-clipping-tests [--case NAME] [NEW_EVIDENCE_DIRECTORY]");
        const std::string selected = argc >= 3 ? argv[2] : "";
        const std::filesystem::path directory = argc == 4 ? argv[3] : argc == 2 ? argv[1] : "";
        require(directory.empty() || std::filesystem::create_directory(directory),
                "output directory already exists");
        const auto device = verify_properties();
        DrawContext context;
        require(context.capabilities().driver_id == VK_DRIVER_ID_MESA_KOSMICKRISP &&
                    context.capabilities().device_name == device &&
                    context.capabilities().transform_feedback && context.capabilities().pipeline_statistics,
                "property/draw provider mismatch or required XFB/queries unavailable");
        unsigned passed = 0, failed = 0;
        for (const std::string stage : {"vs", "gs", "tes"}) {
            auto matrix = cases();
            if (stage != "vs") {
                Case rescued{"final_inside_upstream_outside_and_user_negative"};
                rescued.upstream_position = {-1.125f, 0, 0.5f, 1};
                rescued.upstream_clip = -1;
                matrix.push_back(rescued);
            }
            for (const auto &value : matrix) {
                if (!selected.empty() && stage + "_" + value.name != selected)
                    continue;
                if (run(context, stage, value, directory))
                    ++passed;
                else
                    ++failed;
            }
        }
        require(passed + failed != 0, "case selector matched no tests");
        std::printf("SUMMARY point_clipping_pass=%u fail=%u total=%u\n", passed, failed, passed + failed);
        return failed ? 1 : 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "FAIL %s\n", error.what());
        return 1;
    }
}
