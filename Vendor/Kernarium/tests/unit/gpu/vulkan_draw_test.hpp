// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "require.hpp"

#include <vulkan/vulkan_core.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <vector>

namespace uml::gpu::test {

/* Fails the test unless a Vulkan call succeeded. */
inline void check(VkResult result, const char *operation,
                  const std::source_location location = std::source_location::current())
{
    if (result != VK_SUCCESS)
        uml::testing::fail(std::string(operation) + ": " + std::to_string(result), location);
}

/* A Vulkan structure of type `type`, otherwise zeroed. */
template <typename T>
T structure(VkStructureType type)
{
    T value{};
    value.sType = type;
    return value;
}

/* The entry point `name` of `instance` (null for the global ones); the test
 * fails when the loader has none. */
template <typename Function>
Function instance_function(VkInstance instance, const char *name,
                           const std::source_location location = std::source_location::current())
{
    auto result = reinterpret_cast<Function>(vkGetInstanceProcAddr(instance, name));
    uml::testing::require(result != nullptr, std::string("missing ") + name, location);
    return result;
}

/* The first memory type in `mask` that has all of `flags`. */
[[nodiscard]] inline std::optional<uint32_t> memory_type(
    const VkPhysicalDeviceMemoryProperties &memory, uint32_t mask, VkMemoryPropertyFlags flags)
{
    for (uint32_t type = 0; type < memory.memoryTypeCount; ++type) {
        if ((mask & (1u << type)) && (memory.memoryTypes[type].propertyFlags & flags) == flags)
            return type;
    }
    return std::nullopt;
}

struct DrawCapabilities {
    std::string device_name;
    std::string driver_name;
    VkDriverId driver_id;
    bool geometry_shader;
    bool transform_feedback;
    bool multiview;
    bool geometry_streams;
    bool transform_feedback_streams_lines_triangles;
    bool transform_feedback_queries;
    bool transform_feedback_preserves_provoking_vertex;
    bool pipeline_statistics;
    bool multiview_geometry_shader;
    VkPhysicalDeviceMaintenance10PropertiesKHR maintenance10;
};

struct DrawExtensions {
    bool device_address_commands = false;
    bool maintenance8 = false;
    bool maintenance10 = false;
};

struct VertexInputDraw {
    VkFormat format;
    uint32_t stride;
    uint32_t attribute_offset;
    VkDeviceSize binding_offset;
    VkProvokingVertexModeEXT provoking_vertex;
    bool stipple;
};

struct RasterState {
    VkPolygonMode polygon_mode = VK_POLYGON_MODE_FILL;
    VkLineRasterizationModeKHR line_mode = VK_LINE_RASTERIZATION_MODE_DEFAULT_KHR;
    VkProvokingVertexModeEXT provoking_vertex = VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT;
    bool stipple = false;
    bool depth_clip = true;
};

struct TransformFeedbackBinding {
    uint32_t words;
    uint32_t offset_words;
    uint32_t size_words;
    uint32_t initial_word = 0;
};

struct DrawConfig {
    std::span<const uint32_t> vertex_shader;
    std::span<const uint32_t> fragment_shader;
    std::span<const uint32_t> geometry_shader;
    std::span<const uint32_t> tess_control_shader;
    std::span<const uint32_t> tess_eval_shader;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    VkPolygonMode polygon_mode = VK_POLYGON_MODE_FILL;
    VkCullModeFlags cull_mode = VK_CULL_MODE_NONE;
    VkFrontFace front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkTessellationDomainOrigin tessellation_origin = VK_TESSELLATION_DOMAIN_ORIGIN_UPPER_LEFT;
    VkLineRasterizationModeKHR line_mode = VK_LINE_RASTERIZATION_MODE_DEFAULT_KHR;
    VkProvokingVertexModeEXT provoking_vertex =
        VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT;
    uint32_t view_mask = 0;
    uint32_t width = 64;
    uint32_t height = 64;
    uint32_t layers = 1;
    uint32_t vertex_count = 3;
    uint32_t instance_count = 1;
    uint32_t first_vertex = 0;
    uint32_t first_instance = 0;
    bool indirect = false;
    bool negative_viewport = false;
    bool stencil = false;
    bool depth = false;
    VkFormat depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
    bool depth_clamp = false;
    bool depth_bias = false;
    float depth_bias_constant = 0;
    float depth_bias_slope = 0;
    float depth_bias_clamp = 0;
    float min_depth = 0;
    float max_depth = 1;
    bool stipple = false;
    bool depth_clip = true;
    bool dynamic_raster = false;
    bool fail_on_compile_required = false;
    bool pipeline_library = false;
    RasterState ignored_raster_create;
    std::span<const RasterState> raster_warmup;
    bool query_restoration = false;
    bool timestamp = false;
    uint32_t repeat_count = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    std::span<const VkSampleLocationEXT> sample_locations;
    uint16_t stipple_pattern = 0x00ff;
    uint32_t stipple_factor = 1;
    std::span<const uint32_t> indices;
    std::span<const uint32_t> counters;
    uint32_t transform_feedback_words = 0;
    // Opt-in guarded buffer ranges; mutually exclusive with the single buffer.
    std::span<const TransformFeedbackBinding> transform_feedback_bindings;
    // Stream-zero written/needed query, GPU copy, and final byte counter.
    bool transform_feedback_query = false;
    std::span<const uint8_t> vertex_data;
    // Each state draws into a separate horizontal band through one pipeline.
    std::span<const VertexInputDraw> vertex_input_draws;
    VkQueryPipelineStatisticFlags pipeline_statistics = 0;
    bool primitive_restart = false;
    bool rasterizer_discard = false;
    std::optional<uint32_t> indirect_count;
    uint32_t max_indirect_count = 1;
    std::optional<uint32_t> conditional_value;
    bool conditional_inverted = false;
    uint32_t secondary_replays = 0;
    bool secondary_query_transitions = false;
    bool device_address_commands = false;
    bool end_rendering2 = false;
    bool query_after_rendering = false;
};

// A stream of one-time command buffers, which is what a virtualized GL or
// Vulkan stack submits: each records a distinct value into its own slot of one
// buffer, so the result proves every recording both reached the GPU and landed
// where it belonged. `outstanding` bounds how many are in flight at once, so a
// driver that recycles per-submission command storage is exercised with more
// live recordings than it can cache.
struct SubmitStormConfig {
    uint32_t submissions = 64;
    uint32_t outstanding = 32;
    // Return with the submissions still in flight and let the caller destroy
    // the context underneath them: a completion that runs after the device is
    // gone must still be safe.
    bool abandon = false;
    // Called with a submission's index right after the host has waited for its
    // fence, before the storm records anything else. A test that observes the
    // driver's completion handling uses it to let that handling finish, so what
    // it counts does not depend on how the host's threads happen to be
    // scheduled. Not called for an abandoned storm.
    std::function<void(uint32_t)> completed;
};

struct SubmitStormResult {
    std::vector<uint32_t> values;
    double wall_nanoseconds = 0;
    uint32_t submissions = 0;
};

struct DrawResult {
    std::vector<std::array<uint8_t, 4>> pixels;
    std::vector<uint32_t> counters;
    std::vector<uint32_t> transform_feedback;
    std::vector<std::vector<uint32_t>> transform_feedback_buffers;
    std::vector<uint64_t> transform_feedback_query;
    std::vector<uint64_t> copied_transform_feedback_query;
    uint32_t transform_feedback_bytes_written = 0;
    std::vector<uint32_t> transform_feedback_bytes_written_per_buffer;
    std::vector<uint8_t> stencil;
    std::vector<float> depth;
    std::vector<uint8_t> pipeline_cache;
    std::vector<uint64_t> statistics;
    std::vector<uint64_t> copied_statistics;
    double gpu_nanoseconds = 0;
    double record_nanoseconds = 0;
};

// Uses only Vulkan entry points, so the same fixture runs against the native
// provider and the ordinary loader in an arm64 Linux guest.
class DrawContext final {
public:
    explicit DrawContext(bool legacy_vertex_input = false,
                         bool pipeline_statistics = true,
                         bool conditional_rendering = false,
                         DrawExtensions extensions = {});
    ~DrawContext();
    DrawContext(const DrawContext &) = delete;
    DrawContext &operator=(const DrawContext &) = delete;
    DrawResult draw(const DrawConfig &config,
                    std::span<const uint8_t> pipeline_cache = {});
    SubmitStormResult submit_storm(const SubmitStormConfig &config);
    const DrawCapabilities &capabilities() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/* With UML_VULKAN_REQUIRE_VENUS=1, which the guest probe runner sets, fails
 * the test unless `context` is on the Venus driver, then prints the line the
 * runner requires of a verified guest device. */
void require_venus_when_asked(
    const DrawContext &context,
    const std::source_location location = std::source_location::current());

} // namespace uml::gpu::test
