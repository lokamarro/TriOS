// SPDX-License-Identifier: GPL-2.0-or-later
#include "kosmickrisp_capability_probe.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace uml::gpu {
namespace {

// Generated from minimal GLSL 4.50 vertex and fragment shaders with
// `glslc -O`.  Keeping the words in the probe makes the regression independent
// of build-host shader tools.
constexpr std::array<uint32_t, 127> gpl_vertex_spirv{
    0x07230203, 0x00010000, 0x000d000b, 0x00000015,
    0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000000, 0x00000004, 0x6e69616d,
    0x00000000, 0x0000000d, 0x00030047, 0x0000000b,
    0x00000002, 0x00050048, 0x0000000b, 0x00000000,
    0x0000000b, 0x00000000, 0x00050048, 0x0000000b,
    0x00000001, 0x0000000b, 0x00000001, 0x00050048,
    0x0000000b, 0x00000002, 0x0000000b, 0x00000003,
    0x00050048, 0x0000000b, 0x00000003, 0x0000000b,
    0x00000004, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00030016, 0x00000006,
    0x00000020, 0x00040017, 0x00000007, 0x00000006,
    0x00000004, 0x00040015, 0x00000008, 0x00000020,
    0x00000000, 0x0004002b, 0x00000008, 0x00000009,
    0x00000001, 0x0004001c, 0x0000000a, 0x00000006,
    0x00000009, 0x0006001e, 0x0000000b, 0x00000007,
    0x00000006, 0x0000000a, 0x0000000a, 0x00040020,
    0x0000000c, 0x00000003, 0x0000000b, 0x0004003b,
    0x0000000c, 0x0000000d, 0x00000003, 0x00040015,
    0x0000000e, 0x00000020, 0x00000001, 0x0004002b,
    0x0000000e, 0x0000000f, 0x00000000, 0x0004002b,
    0x00000006, 0x00000010, 0x00000000, 0x0004002b,
    0x00000006, 0x00000011, 0x3f800000, 0x0007002c,
    0x00000007, 0x00000012, 0x00000010, 0x00000010,
    0x00000010, 0x00000011, 0x00040020, 0x00000013,
    0x00000003, 0x00000007, 0x00050036, 0x00000002,
    0x00000004, 0x00000000, 0x00000003, 0x000200f8,
    0x00000005, 0x00050041, 0x00000013, 0x00000014,
    0x0000000d, 0x0000000f, 0x0003003e, 0x00000014,
    0x00000012, 0x000100fd, 0x00010038,
};

// A three-vertex oversized triangle generated from gl_VertexIndex.  This is
// kept separate from the GPL fixture because the latter deliberately emits a
// constant position and does not rasterize fragments.
constexpr std::array<uint32_t, 190> fullscreen_vertex_spirv{
    0x07230203, 0x00010000, 0x000d000b, 0x00000028,
    0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000000, 0x00000004, 0x6e69616d,
    0x00000000, 0x0000000b, 0x00000020, 0x00040047,
    0x0000000b, 0x0000000b, 0x0000002a, 0x00030047,
    0x0000001e, 0x00000002, 0x00050048, 0x0000001e,
    0x00000000, 0x0000000b, 0x00000000, 0x00050048,
    0x0000001e, 0x00000001, 0x0000000b, 0x00000001,
    0x00050048, 0x0000001e, 0x00000002, 0x0000000b,
    0x00000003, 0x00050048, 0x0000001e, 0x00000003,
    0x0000000b, 0x00000004, 0x00020013, 0x00000002,
    0x00030021, 0x00000003, 0x00000002, 0x00030016,
    0x00000006, 0x00000020, 0x00040015, 0x00000009,
    0x00000020, 0x00000001, 0x00040020, 0x0000000a,
    0x00000001, 0x00000009, 0x0004003b, 0x0000000a,
    0x0000000b, 0x00000001, 0x0004002b, 0x00000009,
    0x0000000d, 0x00000001, 0x0004002b, 0x00000009,
    0x0000000f, 0x00000002, 0x0004002b, 0x00000006,
    0x00000012, 0x3f800000, 0x00040017, 0x0000001a,
    0x00000006, 0x00000004, 0x00040015, 0x0000001b,
    0x00000020, 0x00000000, 0x0004002b, 0x0000001b,
    0x0000001c, 0x00000001, 0x0004001c, 0x0000001d,
    0x00000006, 0x0000001c, 0x0006001e, 0x0000001e,
    0x0000001a, 0x00000006, 0x0000001d, 0x0000001d,
    0x00040020, 0x0000001f, 0x00000003, 0x0000001e,
    0x0004003b, 0x0000001f, 0x00000020, 0x00000003,
    0x0004002b, 0x00000009, 0x00000021, 0x00000000,
    0x0004002b, 0x00000006, 0x00000024, 0x00000000,
    0x00040020, 0x00000026, 0x00000003, 0x0000001a,
    0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x0004003d,
    0x00000009, 0x0000000c, 0x0000000b, 0x000500c7,
    0x00000009, 0x0000000e, 0x0000000c, 0x0000000d,
    0x000500c4, 0x00000009, 0x00000010, 0x0000000e,
    0x0000000f, 0x0004006f, 0x00000006, 0x00000011,
    0x00000010, 0x00050083, 0x00000006, 0x00000013,
    0x00000011, 0x00000012, 0x000500c7, 0x00000009,
    0x00000016, 0x0000000c, 0x0000000f, 0x000500c4,
    0x00000009, 0x00000017, 0x00000016, 0x0000000d,
    0x0004006f, 0x00000006, 0x00000018, 0x00000017,
    0x00050083, 0x00000006, 0x00000019, 0x00000018,
    0x00000012, 0x00070050, 0x0000001a, 0x00000025,
    0x00000013, 0x00000019, 0x00000024, 0x00000012,
    0x00050041, 0x00000026, 0x00000027, 0x00000020,
    0x00000021, 0x0003003e, 0x00000027, 0x00000025,
    0x000100fd, 0x00010038,
};

// A fullscreen triangle whose visibility depends on a scalar uint vertex
// input.  The probe binds R8_UNORM and R16_UNORM data to this input so the
// provider exercises both its stable numeric-mismatch policy and two distinct
// dynamic vertex-input variants.  Generated with `glslc -O`.
constexpr std::array<uint32_t, 290> legacy_vertex_spirv{
    0x07230203, 0x00010000, 0x000d000b, 0x0000003a,
    0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0008000f, 0x00000000, 0x00000004, 0x6e69616d,
    0x00000000, 0x0000000b, 0x0000001c, 0x00000029,
    0x00040047, 0x0000000b, 0x0000001e, 0x00000000,
    0x00030047, 0x0000001a, 0x00000002, 0x00050048,
    0x0000001a, 0x00000000, 0x0000000b, 0x00000000,
    0x00050048, 0x0000001a, 0x00000001, 0x0000000b,
    0x00000001, 0x00050048, 0x0000001a, 0x00000002,
    0x0000000b, 0x00000003, 0x00050048, 0x0000001a,
    0x00000003, 0x0000000b, 0x00000004, 0x00040047,
    0x00000029, 0x0000000b, 0x0000002a, 0x00020013,
    0x00000002, 0x00030021, 0x00000003, 0x00000002,
    0x00020014, 0x00000006, 0x00040015, 0x00000009,
    0x00000020, 0x00000000, 0x00040020, 0x0000000a,
    0x00000001, 0x00000009, 0x0004003b, 0x0000000a,
    0x0000000b, 0x00000001, 0x0004002b, 0x00000009,
    0x0000000d, 0x3f800000, 0x0004002b, 0x00000009,
    0x00000013, 0x000000ff, 0x00030016, 0x00000016,
    0x00000020, 0x00040017, 0x00000017, 0x00000016,
    0x00000004, 0x0004002b, 0x00000009, 0x00000018,
    0x00000001, 0x0004001c, 0x00000019, 0x00000016,
    0x00000018, 0x0006001e, 0x0000001a, 0x00000017,
    0x00000016, 0x00000019, 0x00000019, 0x00040020,
    0x0000001b, 0x00000003, 0x0000001a, 0x0004003b,
    0x0000001b, 0x0000001c, 0x00000003, 0x00040015,
    0x0000001d, 0x00000020, 0x00000001, 0x0004002b,
    0x0000001d, 0x0000001e, 0x00000000, 0x00040017,
    0x0000001f, 0x00000016, 0x00000002, 0x0004002b,
    0x00000009, 0x00000020, 0x00000003, 0x0004001c,
    0x00000021, 0x0000001f, 0x00000020, 0x0004002b,
    0x00000016, 0x00000022, 0xbf800000, 0x0005002c,
    0x0000001f, 0x00000023, 0x00000022, 0x00000022,
    0x0004002b, 0x00000016, 0x00000024, 0x40400000,
    0x0005002c, 0x0000001f, 0x00000025, 0x00000024,
    0x00000022, 0x0005002c, 0x0000001f, 0x00000026,
    0x00000022, 0x00000024, 0x0006002c, 0x00000021,
    0x00000027, 0x00000023, 0x00000025, 0x00000026,
    0x00040020, 0x00000028, 0x00000001, 0x0000001d,
    0x0004003b, 0x00000028, 0x00000029, 0x00000001,
    0x00040020, 0x0000002b, 0x00000007, 0x00000021,
    0x00040020, 0x0000002d, 0x00000007, 0x0000001f,
    0x0004002b, 0x00000016, 0x00000031, 0x00000000,
    0x0004002b, 0x00000016, 0x00000032, 0x40000000,
    0x0004002b, 0x00000016, 0x00000034, 0x3f800000,
    0x00040020, 0x00000038, 0x00000003, 0x00000017,
    0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x0004003b,
    0x0000002b, 0x0000002c, 0x00000007, 0x0004003d,
    0x00000009, 0x0000000c, 0x0000000b, 0x000500aa,
    0x00000006, 0x0000000e, 0x0000000c, 0x0000000d,
    0x000400a8, 0x00000006, 0x0000000f, 0x0000000e,
    0x000300f7, 0x00000011, 0x00000000, 0x000400fa,
    0x0000000f, 0x00000010, 0x00000011, 0x000200f8,
    0x00000010, 0x000500aa, 0x00000006, 0x00000014,
    0x0000000c, 0x00000013, 0x000200f9, 0x00000011,
    0x000200f8, 0x00000011, 0x000700f5, 0x00000006,
    0x00000015, 0x0000000e, 0x00000005, 0x00000014,
    0x00000010, 0x0004003d, 0x0000001d, 0x0000002a,
    0x00000029, 0x0003003e, 0x0000002c, 0x00000027,
    0x00050041, 0x0000002d, 0x0000002e, 0x0000002c,
    0x0000002a, 0x0004003d, 0x0000001f, 0x0000002f,
    0x0000002e, 0x000600a9, 0x00000016, 0x00000033,
    0x00000015, 0x00000031, 0x00000032, 0x00050051,
    0x00000016, 0x00000035, 0x0000002f, 0x00000000,
    0x00050051, 0x00000016, 0x00000036, 0x0000002f,
    0x00000001, 0x00070050, 0x00000017, 0x00000037,
    0x00000035, 0x00000036, 0x00000033, 0x00000034,
    0x00050041, 0x00000038, 0x00000039, 0x0000001c,
    0x0000001e, 0x0003003e, 0x00000039, 0x00000037,
    0x000100fd, 0x00010038,
};

constexpr std::array<uint32_t, 76> gpl_fragment_spirv{
    0x07230203, 0x00010000, 0x000d000b, 0x0000000d,
    0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000004, 0x00000004, 0x6e69616d,
    0x00000000, 0x00000009, 0x00030010, 0x00000004,
    0x00000007, 0x00040047, 0x00000009, 0x0000001e,
    0x00000000, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00030016, 0x00000006,
    0x00000020, 0x00040017, 0x00000007, 0x00000006,
    0x00000004, 0x00040020, 0x00000008, 0x00000003,
    0x00000007, 0x0004003b, 0x00000008, 0x00000009,
    0x00000003, 0x0004002b, 0x00000006, 0x0000000a,
    0x3f800000, 0x0004002b, 0x00000006, 0x0000000b,
    0x00000000, 0x0007002c, 0x00000007, 0x0000000c,
    0x0000000a, 0x0000000b, 0x0000000b, 0x0000000a,
    0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x0003003e,
    0x00000009, 0x0000000c, 0x000100fd, 0x00010038,
};

template <typename Function>
Function load_function(VkInstance instance, const char *name)
{
    const auto function = reinterpret_cast<Function>(
        vkGetInstanceProcAddr(instance, name));
    if (function == nullptr) {
        throw std::runtime_error(std::string("missing Vulkan entry point: ") +
                                 name);
    }
    return function;
}

template <typename Function>
Function load_device_function(PFN_vkGetDeviceProcAddr get_device_proc_addr,
                              VkDevice device,
                              const char *name)
{
    const auto function = reinterpret_cast<Function>(
        get_device_proc_addr(device, name));
    if (function == nullptr) {
        throw std::runtime_error(std::string("missing Vulkan entry point: ") +
                                 name);
    }
    return function;
}

// Declares member as the device entry point vk<name>, loaded through the
// enclosing scope's device and get_device_proc_addr. A missing one throws.
#define UML_DEVICE_FUNCTION(member, name)                                     \
    const PFN_vk##name member = load_device_function<PFN_vk##name>(           \
        get_device_proc_addr, device, "vk" #name)

// Runs a verifier's cleanup when the verifier returns or throws.
template <typename Function>
class ScopeExit {
public:
    explicit ScopeExit(Function function) : function_(std::move(function)) {}
    ScopeExit(const ScopeExit &) = delete;
    ScopeExit &operator=(const ScopeExit &) = delete;
    ~ScopeExit() { function_(); }

private:
    Function function_;
};

void require_result(VkResult result, std::string_view operation)
{
    if (result != VK_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed: " +
                                 std::to_string(result));
    }
}

// The first memory type among those in compatible that has every required
// property, preferring one that also has every preferred property. Throws
// failure when none qualifies.
uint32_t find_memory_type(const VkPhysicalDeviceMemoryProperties &memory,
                          uint32_t compatible,
                          VkMemoryPropertyFlags required,
                          VkMemoryPropertyFlags preferred,
                          const std::string &failure)
{
    uint32_t fallback = memory.memoryTypeCount;
    for (uint32_t index = 0; index < memory.memoryTypeCount; ++index) {
        const VkMemoryPropertyFlags flags =
            memory.memoryTypes[index].propertyFlags;
        if ((compatible & (1U << index)) == 0 ||
            (flags & required) != required) {
            continue;
        }
        if ((flags & preferred) == preferred) {
            return index;
        }
        if (fallback == memory.memoryTypeCount) {
            fallback = index;
        }
    }
    if (fallback == memory.memoryTypeCount) {
        throw std::runtime_error(failure);
    }
    return fallback;
}

struct CubeAliasReport;
struct GraphicsPipelineLibraryReport;
struct DeviceAddressCommandsReport;
struct DynamicLogicOpReport;
struct LegacyVertexAttributesReport;

// A created device and the Vulkan 1.0 entry points its verifiers share, which
// every Vulkan device exposes, loaded once. Each verifier exercises one
// capability and loads the entry points its extension adds itself.
struct ProbeDevice {
    ProbeDevice(VkDevice device, PFN_vkGetDeviceProcAddr get_device_proc_addr)
        : device(device), get_device_proc_addr(get_device_proc_addr)
    {
    }

    CubeAliasReport verify_cube_alias_views(
        VkPhysicalDevice physical,
        const VkPhysicalDeviceMemoryProperties &memory,
        PFN_vkGetPhysicalDeviceImageFormatProperties
            get_image_format_properties) const;
    GraphicsPipelineLibraryReport verify_graphics_pipeline_library(
        uint32_t queue_family) const;
    DeviceAddressCommandsReport verify_device_address_commands(
        uint32_t api_version,
        uint32_t queue_family,
        const VkPhysicalDeviceMemoryProperties &memory,
        const std::vector<VkExtensionProperties> &extensions) const;
    DynamicLogicOpReport verify_dynamic_logic_ops(
        uint32_t queue_family,
        const VkPhysicalDeviceMemoryProperties &memory) const;
    LegacyVertexAttributesReport verify_legacy_vertex_attributes(
        uint32_t queue_family,
        const VkPhysicalDeviceMemoryProperties &memory) const;

    const VkDevice device;
    const PFN_vkGetDeviceProcAddr get_device_proc_addr;
    UML_DEVICE_FUNCTION(allocate_command_buffers, AllocateCommandBuffers);
    UML_DEVICE_FUNCTION(allocate_memory, AllocateMemory);
    UML_DEVICE_FUNCTION(begin_command_buffer, BeginCommandBuffer);
    UML_DEVICE_FUNCTION(bind_buffer_memory, BindBufferMemory);
    UML_DEVICE_FUNCTION(bind_image_memory, BindImageMemory);
    UML_DEVICE_FUNCTION(cmd_begin_render_pass, CmdBeginRenderPass);
    UML_DEVICE_FUNCTION(cmd_bind_pipeline, CmdBindPipeline);
    UML_DEVICE_FUNCTION(cmd_bind_vertex_buffers, CmdBindVertexBuffers);
    UML_DEVICE_FUNCTION(cmd_copy_image_to_buffer, CmdCopyImageToBuffer);
    UML_DEVICE_FUNCTION(cmd_draw, CmdDraw);
    UML_DEVICE_FUNCTION(cmd_end_render_pass, CmdEndRenderPass);
    UML_DEVICE_FUNCTION(cmd_pipeline_barrier, CmdPipelineBarrier);
    UML_DEVICE_FUNCTION(cmd_set_scissor, CmdSetScissor);
    UML_DEVICE_FUNCTION(cmd_set_viewport, CmdSetViewport);
    UML_DEVICE_FUNCTION(create_buffer, CreateBuffer);
    UML_DEVICE_FUNCTION(create_command_pool, CreateCommandPool);
    UML_DEVICE_FUNCTION(create_framebuffer, CreateFramebuffer);
    UML_DEVICE_FUNCTION(create_graphics_pipelines, CreateGraphicsPipelines);
    UML_DEVICE_FUNCTION(create_image, CreateImage);
    UML_DEVICE_FUNCTION(create_image_view, CreateImageView);
    UML_DEVICE_FUNCTION(create_pipeline_cache, CreatePipelineCache);
    UML_DEVICE_FUNCTION(create_pipeline_layout, CreatePipelineLayout);
    UML_DEVICE_FUNCTION(create_render_pass, CreateRenderPass);
    UML_DEVICE_FUNCTION(create_shader_module, CreateShaderModule);
    UML_DEVICE_FUNCTION(destroy_buffer, DestroyBuffer);
    UML_DEVICE_FUNCTION(destroy_command_pool, DestroyCommandPool);
    UML_DEVICE_FUNCTION(destroy_device, DestroyDevice);
    UML_DEVICE_FUNCTION(destroy_framebuffer, DestroyFramebuffer);
    UML_DEVICE_FUNCTION(destroy_image, DestroyImage);
    UML_DEVICE_FUNCTION(destroy_image_view, DestroyImageView);
    UML_DEVICE_FUNCTION(destroy_pipeline, DestroyPipeline);
    UML_DEVICE_FUNCTION(destroy_pipeline_cache, DestroyPipelineCache);
    UML_DEVICE_FUNCTION(destroy_pipeline_layout, DestroyPipelineLayout);
    UML_DEVICE_FUNCTION(destroy_render_pass, DestroyRenderPass);
    UML_DEVICE_FUNCTION(destroy_shader_module, DestroyShaderModule);
    UML_DEVICE_FUNCTION(end_command_buffer, EndCommandBuffer);
    UML_DEVICE_FUNCTION(flush_memory, FlushMappedMemoryRanges);
    UML_DEVICE_FUNCTION(free_memory, FreeMemory);
    UML_DEVICE_FUNCTION(
        get_buffer_memory_requirements, GetBufferMemoryRequirements);
    UML_DEVICE_FUNCTION(get_device_queue, GetDeviceQueue);
    UML_DEVICE_FUNCTION(
        get_image_memory_requirements, GetImageMemoryRequirements);
    UML_DEVICE_FUNCTION(invalidate_memory, InvalidateMappedMemoryRanges);
    UML_DEVICE_FUNCTION(map_memory, MapMemory);
    UML_DEVICE_FUNCTION(queue_submit, QueueSubmit);
    UML_DEVICE_FUNCTION(queue_wait_idle, QueueWaitIdle);
    UML_DEVICE_FUNCTION(unmap_memory, UnmapMemory);
};

const char *json_bool(bool value)
{
    return value ? "true" : "false";
}

std::string json_string(std::string_view value)
{
    std::ostringstream output;
    output << '"';
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (character < 0x20U) {
                output << "\\u" << std::hex << std::setw(4)
                       << std::setfill('0') << static_cast<unsigned>(character)
                       << std::dec << std::setfill(' ');
            } else {
                output << static_cast<char>(character);
            }
        }
    }
    output << '"';
    return output.str();
}

template <typename Property>
bool has_extension(const std::vector<Property> &properties,
                   std::string_view name)
{
    return std::any_of(properties.begin(), properties.end(),
                       [name](const Property &property) {
                           return name == property.extensionName;
                       });
}

template <typename Property>
void write_extensions(std::ostream &output,
                      std::vector<Property> properties)
{
    std::sort(properties.begin(), properties.end(),
              [](const Property &left, const Property &right) {
                  return std::strcmp(left.extensionName,
                                     right.extensionName) < 0;
              });
    output << '[';
    for (std::size_t index = 0; index < properties.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << "{\"name\":" << json_string(properties[index].extensionName)
               << ",\"spec_version\":" << properties[index].specVersion
               << '}';
    }
    output << ']';
}

template <typename Property, typename Enumerate>
std::vector<Property> enumerate_properties(Enumerate enumerate)
{
    uint32_t count = 0;
    VkResult result = enumerate(&count, nullptr);
    if (result != VK_SUCCESS) {
        throw std::runtime_error("Vulkan enumeration count failed");
    }
    std::vector<Property> properties(count);
    result = enumerate(&count, properties.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        throw std::runtime_error("Vulkan enumeration payload failed");
    }
    properties.resize(count);
    return properties;
}

std::vector<VkFormat> cube_format_candidates()
{
    std::vector<VkFormat> formats;
    formats.reserve(280);
    for (int value = VK_FORMAT_R4G4_UNORM_PACK8;
         value <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK; ++value) {
        formats.push_back(static_cast<VkFormat>(value));
    }

    const auto append_range = [&formats](int first, int last) {
        for (int value = first; value <= last; ++value) {
            formats.push_back(static_cast<VkFormat>(value));
        }
    };
    append_range(VK_FORMAT_PVRTC1_2BPP_UNORM_BLOCK_IMG,
                 VK_FORMAT_PVRTC2_4BPP_SRGB_BLOCK_IMG);
    append_range(VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK,
                 VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK);
    append_range(VK_FORMAT_G8B8G8R8_422_UNORM,
                 VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM);
    append_range(VK_FORMAT_ASTC_3x3x3_UNORM_BLOCK_EXT,
                 VK_FORMAT_ASTC_6x6x6_SFLOAT_BLOCK_EXT);
    append_range(VK_FORMAT_G8_B8R8_2PLANE_444_UNORM,
                 VK_FORMAT_G16_B16R16_2PLANE_444_UNORM);
    append_range(VK_FORMAT_A4R4G4B4_UNORM_PACK16,
                 VK_FORMAT_A4B4G4R4_UNORM_PACK16);
    append_range(VK_FORMAT_R8_BOOL_ARM,
                 VK_FORMAT_R8_SFLOAT_FPENCODING_FLOAT8E5M2_ARM);
    formats.push_back(VK_FORMAT_R16G16_SFIXED5_NV);
    append_range(VK_FORMAT_A1B5G5R5_UNORM_PACK16, VK_FORMAT_A8_UNORM);
    append_range(VK_FORMAT_R10X6_UINT_PACK16_ARM,
                 VK_FORMAT_G14X2_B14X2R14X2_2PLANE_422_UNORM_3PACK16_ARM);
    return formats;
}

VkImageAspectFlags format_aspects(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT:
        return VK_IMAGE_ASPECT_STENCIL_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

struct CubeAliasReport {
    uint32_t supported_format_count = 0;
    uint32_t cube_view_count = 0;
    uint32_t cube_array_view_count = 0;
};

CubeAliasReport ProbeDevice::verify_cube_alias_views(
    VkPhysicalDevice physical,
    const VkPhysicalDeviceMemoryProperties &memory,
    PFN_vkGetPhysicalDeviceImageFormatProperties get_image_format_properties)
    const
{
    CubeAliasReport report;
    for (const VkFormat format : cube_format_candidates()) {
        VkImageFormatProperties properties{};
        const VkResult format_result = get_image_format_properties(
            physical, format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
            &properties);
        if (format_result == VK_ERROR_FORMAT_NOT_SUPPORTED) {
            continue;
        }
        if (format_result != VK_SUCCESS) {
            throw std::runtime_error(
                "cube image format query failed for format " +
                std::to_string(format) + ": " +
                std::to_string(format_result));
        }
        if (properties.maxArrayLayers < 12 ||
            properties.maxExtent.width < 16 ||
            properties.maxExtent.height < 16) {
            throw std::runtime_error(
                "cube-array format reports insufficient image limits: " +
                std::to_string(format));
        }

        const VkImageCreateInfo image_info{
            VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            nullptr,
            VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
            VK_IMAGE_TYPE_2D,
            format,
            {16, 16, 1},
            1,
            12,
            VK_SAMPLE_COUNT_1_BIT,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_SHARING_MODE_EXCLUSIVE,
            0,
            nullptr,
            VK_IMAGE_LAYOUT_UNDEFINED,
        };
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory image_memory = VK_NULL_HANDLE;
        VkImageView cube_view = VK_NULL_HANDLE;
        VkImageView cube_array_view = VK_NULL_HANDLE;
        const ScopeExit cleanup{[&] {
            if (cube_array_view != VK_NULL_HANDLE) {
                destroy_image_view(device, cube_array_view, nullptr);
            }
            if (cube_view != VK_NULL_HANDLE) {
                destroy_image_view(device, cube_view, nullptr);
            }
            if (image != VK_NULL_HANDLE) {
                destroy_image(device, image, nullptr);
            }
            if (image_memory != VK_NULL_HANDLE) {
                free_memory(device, image_memory, nullptr);
            }
        }};
        const auto require_result = [&](VkResult result,
                                        std::string_view operation) {
            if (result == VK_SUCCESS) {
                return;
            }
            throw std::runtime_error(
                std::string(operation) + " failed for cube format " +
                std::to_string(format) + ": " + std::to_string(result));
        };

        require_result(create_image(device, &image_info, nullptr, &image),
                       "vkCreateImage");
        VkMemoryRequirements requirements{};
        get_image_memory_requirements(device, image, &requirements);
        const uint32_t memory_type = find_memory_type(
            memory, requirements.memoryTypeBits, 0, 0,
            "no memory type for cube format " + std::to_string(format));
        const VkMemoryAllocateInfo allocation_info{
            VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            nullptr,
            requirements.size,
            memory_type,
        };
        require_result(allocate_memory(device, &allocation_info, nullptr,
                                       &image_memory),
                       "vkAllocateMemory");
        require_result(bind_image_memory(device, image, image_memory, 0),
                       "vkBindImageMemory");

        VkImageViewCreateInfo view_info{
            VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            nullptr,
            0,
            image,
            VK_IMAGE_VIEW_TYPE_CUBE,
            format,
            {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
             VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
            {format_aspects(format), 0, 1, 0, 6},
        };
        require_result(
            create_image_view(device, &view_info, nullptr, &cube_view),
            "vkCreateImageView(CUBE)");
        ++report.cube_view_count;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        view_info.subresourceRange.layerCount = 12;
        require_result(create_image_view(device, &view_info, nullptr,
                                         &cube_array_view),
                       "vkCreateImageView(CUBE_ARRAY)");
        ++report.cube_array_view_count;
        ++report.supported_format_count;
    }
    return report;
}

void write_cube_alias_report(std::ostream &output,
                             const CubeAliasReport &report)
{
    output << "{\"supported_format_count\":" << report.supported_format_count
           << ",\"cube_view_count\":" << report.cube_view_count
           << ",\"cube_array_view_count\":"
           << report.cube_array_view_count << '}';
}

struct GraphicsPipelineLibraryReport {
    VkResult compile_required_result = VK_ERROR_UNKNOWN;
    VkResult first_link_result = VK_ERROR_UNKNOWN;
    VkResult cached_link_result = VK_ERROR_UNKNOWN;
    VkResult mixed_link_result = VK_ERROR_UNKNOWN;
    VkResult combined_link_result = VK_ERROR_UNKNOWN;
    VkResult lto_link_result = VK_ERROR_UNKNOWN;
    VkResult bind_result = VK_ERROR_UNKNOWN;
    uint64_t first_link_ns = 0;
    uint64_t cached_link_ns = 0;
    uint64_t mixed_link_ns = 0;
    uint64_t combined_link_ns = 0;
    uint64_t lto_link_ns = 0;
    uint64_t bind_record_ns = 0;
};

GraphicsPipelineLibraryReport ProbeDevice::verify_graphics_pipeline_library(
    uint32_t queue_family) const
{
    VkShaderModule vertex_module = VK_NULL_HANDLE;
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipelineCache cache = VK_NULL_HANDLE;
    std::array<VkPipeline, 4> libraries{};
    std::array<VkPipeline, 2> combined_libraries{};
    VkPipeline first_pipeline = VK_NULL_HANDLE;
    VkPipeline cached_pipeline = VK_NULL_HANDLE;
    VkPipeline mixed_pipeline = VK_NULL_HANDLE;
    VkPipeline combined_pipeline = VK_NULL_HANDLE;
    VkPipeline lto_pipeline = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;

    const ScopeExit cleanup{[&] {
        if (command_pool != VK_NULL_HANDLE) {
            destroy_command_pool(device, command_pool, nullptr);
        }
        if (lto_pipeline != VK_NULL_HANDLE) {
            destroy_pipeline(device, lto_pipeline, nullptr);
        }
        if (combined_pipeline != VK_NULL_HANDLE) {
            destroy_pipeline(device, combined_pipeline, nullptr);
        }
        if (mixed_pipeline != VK_NULL_HANDLE) {
            destroy_pipeline(device, mixed_pipeline, nullptr);
        }
        if (cached_pipeline != VK_NULL_HANDLE) {
            destroy_pipeline(device, cached_pipeline, nullptr);
        }
        if (first_pipeline != VK_NULL_HANDLE) {
            destroy_pipeline(device, first_pipeline, nullptr);
        }
        for (const auto library : libraries) {
            if (library != VK_NULL_HANDLE) {
                destroy_pipeline(device, library, nullptr);
            }
        }
        for (const auto library : combined_libraries) {
            if (library != VK_NULL_HANDLE) {
                destroy_pipeline(device, library, nullptr);
            }
        }
        if (cache != VK_NULL_HANDLE) {
            destroy_pipeline_cache(device, cache, nullptr);
        }
        if (layout != VK_NULL_HANDLE) {
            destroy_pipeline_layout(device, layout, nullptr);
        }
        if (fragment_module != VK_NULL_HANDLE) {
            destroy_shader_module(device, fragment_module, nullptr);
        }
        if (vertex_module != VK_NULL_HANDLE) {
            destroy_shader_module(device, vertex_module, nullptr);
        }
    }};

    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = gpl_vertex_spirv.size() * sizeof(uint32_t);
    module_info.pCode = gpl_vertex_spirv.data();
    require_result(create_shader_module(device, &module_info, nullptr,
                                         &vertex_module),
                    "GPL vertex shader module creation");
    module_info.codeSize = gpl_fragment_spirv.size() * sizeof(uint32_t);
    module_info.pCode = gpl_fragment_spirv.data();
    require_result(create_shader_module(device, &module_info, nullptr,
                                         &fragment_module),
                    "GPL fragment shader module creation");

    VkPipelineLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    require_result(create_pipeline_layout(device, &layout_info, nullptr,
                                           &layout),
                    "GPL pipeline layout creation");
    VkPipelineCacheCreateInfo cache_info{};
    cache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    require_result(create_pipeline_cache(device, &cache_info, nullptr,
                                          &cache),
                    "GPL pipeline cache creation");

    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_VERTEX_BIT, vertex_module, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_FRAGMENT_BIT, fragment_module, "main", nullptr},
    }};
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    depth_stencil.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    constexpr std::array<VkDynamicState, 2> dynamic_states{
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = dynamic_states.size();
    dynamic.pDynamicStates = dynamic_states.data();
    constexpr VkFormat color_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color_format;

    constexpr std::array<VkGraphicsPipelineLibraryFlagsEXT, 4> subset_flags{
        VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT,
        VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
        VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
        VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT,
    };
    std::array<VkGraphicsPipelineLibraryCreateInfoEXT, 4> subset_infos{};
    std::array<VkGraphicsPipelineCreateInfo, 4> library_infos{};
    for (std::size_t index = 0; index < library_infos.size(); ++index) {
        subset_infos[index].sType =
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT;
        subset_infos[index].flags = subset_flags[index];
        library_infos[index].sType =
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        library_infos[index].pNext = &subset_infos[index];
        library_infos[index].flags =
            VK_PIPELINE_CREATE_LIBRARY_BIT_KHR |
            VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
        library_infos[index].layout = layout;
    }
    library_infos[0].pVertexInputState = &vertex_input;
    library_infos[0].pInputAssemblyState = &input_assembly;
    subset_infos[1].pNext = &rendering;
    library_infos[1].stageCount = 1;
    library_infos[1].pStages = &stages[0];
    library_infos[1].pViewportState = &viewport;
    library_infos[1].pRasterizationState = &rasterization;
    library_infos[1].pDynamicState = &dynamic;
    subset_infos[2].pNext = &rendering;
    library_infos[2].stageCount = 1;
    library_infos[2].pStages = &stages[1];
    library_infos[2].pMultisampleState = &multisample;
    library_infos[2].pDepthStencilState = &depth_stencil;
    subset_infos[3].pNext = &rendering;
    library_infos[3].pMultisampleState = &multisample;
    library_infos[3].pColorBlendState = &blend;
    for (std::size_t index = 0; index < library_infos.size(); ++index) {
        require_result(create_graphics_pipelines(
                            device, cache, 1, &library_infos[index], nullptr,
                            &libraries[index]),
                        "GPL subset library creation");
    }

    VkPipelineLibraryCreateInfoKHR library_link{};
    library_link.sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR;
    library_link.libraryCount = libraries.size();
    library_link.pLibraries = libraries.data();
    VkGraphicsPipelineCreateInfo final_info{};
    final_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    final_info.pNext = &library_link;
    final_info.layout = layout;

    GraphicsPipelineLibraryReport report;
    final_info.flags = VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    VkPipeline unexpected_pipeline = VK_NULL_HANDLE;
    report.compile_required_result = create_graphics_pipelines(
        device, cache, 1, &final_info, nullptr, &unexpected_pipeline);
    if (unexpected_pipeline != VK_NULL_HANDLE) {
        destroy_pipeline(device, unexpected_pipeline, nullptr);
    }
    if (report.compile_required_result != VK_PIPELINE_COMPILE_REQUIRED) {
        throw std::runtime_error(
            "cold GPL fail-on-compile did not return PIPELINE_COMPILE_REQUIRED: " +
            std::to_string(report.compile_required_result));
    }

    const auto timed_link = [&](VkPipelineCreateFlags flags,
                                VkPipeline *pipeline,
                                uint64_t *elapsed_ns) {
        final_info.flags = flags;
        const auto begin = std::chrono::steady_clock::now();
        const VkResult link_result = create_graphics_pipelines(
            device, cache, 1, &final_info, nullptr, pipeline);
        const auto end = std::chrono::steady_clock::now();
        *elapsed_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
                .count());
        return link_result;
    };
    report.first_link_result =
        timed_link(0, &first_pipeline, &report.first_link_ns);
    require_result(report.first_link_result, "GPL final link");
    report.cached_link_result = timed_link(
        VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT,
        &cached_pipeline, &report.cached_link_ns);
    require_result(report.cached_link_result, "cached GPL final link");

    // A final pipeline may import shader and state subsets while supplying a
    // missing shader stage directly.  Both representations must participate
    // in the same deferred native link.
    const std::array<VkPipeline, 3> mixed_libraries{
        libraries[0], libraries[1], libraries[3]};
    VkPipelineLibraryCreateInfoKHR mixed_library_link{};
    mixed_library_link.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR;
    mixed_library_link.pNext = &rendering;
    mixed_library_link.libraryCount = mixed_libraries.size();
    mixed_library_link.pLibraries = mixed_libraries.data();
    VkGraphicsPipelineLibraryCreateInfoEXT mixed_subset_info{};
    mixed_subset_info.sType =
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT;
    mixed_subset_info.pNext = &mixed_library_link;
    mixed_subset_info.flags =
        VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
    VkGraphicsPipelineCreateInfo mixed_info{};
    mixed_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    mixed_info.pNext = &mixed_subset_info;
    mixed_info.stageCount = 1;
    mixed_info.pStages = &stages[1];
    mixed_info.pMultisampleState = &multisample;
    mixed_info.pDepthStencilState = &depth_stencil;
    mixed_info.layout = layout;
    const auto mixed_begin = std::chrono::steady_clock::now();
    report.mixed_link_result = create_graphics_pipelines(
        device, cache, 1, &mixed_info, nullptr, &mixed_pipeline);
    const auto mixed_end = std::chrono::steady_clock::now();
    report.mixed_link_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(mixed_end -
                                                             mixed_begin)
            .count());
    require_result(report.mixed_link_result,
                    "mixed imported/direct GPL final link");

    std::array<VkGraphicsPipelineLibraryCreateInfoEXT, 2>
        combined_subset_infos{};
    std::array<VkGraphicsPipelineCreateInfo, 2> combined_library_infos{};
    for (std::size_t index = 0; index < combined_library_infos.size();
         ++index) {
        combined_subset_infos[index].sType =
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT;
        combined_subset_infos[index].pNext = &rendering;
        combined_library_infos[index].sType =
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        combined_library_infos[index].pNext =
            &combined_subset_infos[index];
        combined_library_infos[index].flags =
            VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
        combined_library_infos[index].layout = layout;
    }
    combined_subset_infos[0].flags =
        VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT |
        VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT |
        VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
    combined_library_infos[0].stageCount = 1;
    combined_library_infos[0].pStages = &stages[0];
    combined_library_infos[0].pVertexInputState = &vertex_input;
    combined_library_infos[0].pInputAssemblyState = &input_assembly;
    combined_library_infos[0].pViewportState = &viewport;
    combined_library_infos[0].pRasterizationState = &rasterization;
    combined_library_infos[0].pMultisampleState = &multisample;
    combined_library_infos[0].pColorBlendState = &blend;
    combined_library_infos[0].pDynamicState = &dynamic;
    combined_subset_infos[1].flags =
        VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
    combined_library_infos[1].stageCount = 1;
    combined_library_infos[1].pStages = &stages[1];
    combined_library_infos[1].pMultisampleState = &multisample;
    combined_library_infos[1].pDepthStencilState = &depth_stencil;
    for (std::size_t index = 0; index < combined_library_infos.size();
         ++index) {
        require_result(create_graphics_pipelines(
                            device, cache, 1,
                            &combined_library_infos[index], nullptr,
                            &combined_libraries[index]),
                        "combined GPL subset library creation");
    }
    VkPipelineLibraryCreateInfoKHR combined_library_link{};
    combined_library_link.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR;
    combined_library_link.libraryCount = combined_libraries.size();
    combined_library_link.pLibraries = combined_libraries.data();
    final_info.pNext = &combined_library_link;
    report.combined_link_result =
        timed_link(0, &combined_pipeline, &report.combined_link_ns);
    require_result(report.combined_link_result,
                    "combined-subset GPL final link");

    final_info.pNext = &library_link;
    report.lto_link_result = timed_link(
        VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT,
        &lto_pipeline, &report.lto_link_ns);
    require_result(report.lto_link_result, "GPL LTO final link");

    // A linked pipeline must retain everything it needs after its source
    // libraries have been destroyed.
    for (auto &library : libraries) {
        destroy_pipeline(device, library, nullptr);
        library = VK_NULL_HANDLE;
    }
    for (auto &library : combined_libraries) {
        destroy_pipeline(device, library, nullptr);
        library = VK_NULL_HANDLE;
    }

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = queue_family;
    require_result(create_command_pool(device, &pool_info, nullptr,
                                        &command_pool),
                    "GPL command pool creation");
    VkCommandBufferAllocateInfo allocation_info{};
    allocation_info.sType =
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocation_info.commandPool = command_pool;
    allocation_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation_info.commandBufferCount = 1;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    require_result(allocate_command_buffers(device, &allocation_info,
                                             &command_buffer),
                    "GPL command buffer allocation");
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    const auto bind_begin = std::chrono::steady_clock::now();
    require_result(begin_command_buffer(command_buffer, &begin_info),
                    "GPL command buffer begin");
    cmd_bind_pipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      first_pipeline);
    cmd_bind_pipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      cached_pipeline);
    cmd_bind_pipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      mixed_pipeline);
    cmd_bind_pipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      combined_pipeline);
    cmd_bind_pipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      lto_pipeline);
    report.bind_result = end_command_buffer(command_buffer);
    const auto bind_end = std::chrono::steady_clock::now();
    report.bind_record_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(bind_end -
                                                             bind_begin)
            .count());
    require_result(report.bind_result, "GPL command buffer bind recording");

    return report;
}

void write_graphics_pipeline_library_report(
    std::ostream &output,
    const GraphicsPipelineLibraryReport &report)
{
    output << "{\"subset_library_count\":4"
           << ",\"compile_required_result\":"
           << report.compile_required_result
           << ",\"first_link_result\":" << report.first_link_result
           << ",\"cached_link_result\":" << report.cached_link_result
           << ",\"mixed_link_result\":" << report.mixed_link_result
           << ",\"combined_link_result\":" << report.combined_link_result
           << ",\"lto_link_result\":" << report.lto_link_result
           << ",\"bind_result\":" << report.bind_result
           << ",\"first_link_ns\":" << report.first_link_ns
           << ",\"cached_link_ns\":" << report.cached_link_ns
           << ",\"mixed_link_ns\":" << report.mixed_link_ns
           << ",\"combined_link_ns\":" << report.combined_link_ns
           << ",\"lto_link_ns\":" << report.lto_link_ns
           << ",\"bind_record_ns\":" << report.bind_record_ns << '}';
}

struct DeviceAddressCommandsReport {
    uint32_t entrypoint_count = 0;
    bool state_commands_recorded = false;
    bool transfer_verified = false;
};

DeviceAddressCommandsReport ProbeDevice::verify_device_address_commands(
    uint32_t api_version,
    uint32_t queue_family,
    const VkPhysicalDeviceMemoryProperties &memory,
    const std::vector<VkExtensionProperties> &extensions) const
{
    DeviceAddressCommandsReport report;

    // Every entry point the extension adds, and those it adds to the
    // extensions this device has, must resolve. The report counts them.
    const auto count_entry_points =
        [&](std::initializer_list<const char *> names) {
            for (const char *name : names) {
                (void)load_device_function<PFN_vkVoidFunction>(
                    get_device_proc_addr, device, name);
                ++report.entrypoint_count;
            }
        };
    count_entry_points({
        "vkCmdBindIndexBuffer3KHR",
        "vkCmdBindVertexBuffers3KHR",
        "vkCmdDrawIndirect2KHR",
        "vkCmdDrawIndexedIndirect2KHR",
        "vkCmdDispatchIndirect2KHR",
        "vkCmdCopyMemoryKHR",
        "vkCmdCopyMemoryToImageKHR",
        "vkCmdCopyImageToMemoryKHR",
        "vkCmdUpdateMemoryKHR",
        "vkCmdFillMemoryKHR",
        "vkCmdCopyQueryPoolResultsToMemoryKHR",
    });
    if (api_version >= VK_API_VERSION_1_2 ||
        has_extension(extensions,
                      VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME)) {
        count_entry_points({
            "vkCmdDrawIndirectCount2KHR",
            "vkCmdDrawIndexedIndirectCount2KHR",
        });
    }
    if (has_extension(extensions,
                      VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME)) {
        count_entry_points({"vkCmdBeginConditionalRendering2EXT"});
    }
    if (has_extension(extensions, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME)) {
        count_entry_points({
            "vkCmdBindTransformFeedbackBuffers2EXT",
            "vkCmdBeginTransformFeedback2EXT",
            "vkCmdEndTransformFeedback2EXT",
            "vkCmdDrawIndirectByteCount2EXT",
        });
    }
    if (has_extension(extensions, VK_EXT_MESH_SHADER_EXTENSION_NAME)) {
        count_entry_points({"vkCmdDrawMeshTasksIndirect2EXT"});
        if (api_version >= VK_API_VERSION_1_2 ||
            has_extension(extensions,
                          VK_KHR_DRAW_INDIRECT_COUNT_EXTENSION_NAME)) {
            count_entry_points({"vkCmdDrawMeshTasksIndirectCount2EXT"});
        }
    }
    if (has_extension(extensions, VK_AMD_BUFFER_MARKER_EXTENSION_NAME)) {
        count_entry_points({"vkCmdWriteMarkerToMemoryAMD"});
    }
    if (has_extension(extensions,
                      VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME)) {
        count_entry_points({"vkCreateAccelerationStructure2KHR"});
    }
    UML_DEVICE_FUNCTION(cmd_bind_index_buffer, CmdBindIndexBuffer3KHR);
    UML_DEVICE_FUNCTION(cmd_bind_vertex_buffers3, CmdBindVertexBuffers3KHR);
    UML_DEVICE_FUNCTION(cmd_copy_memory, CmdCopyMemoryKHR);
    UML_DEVICE_FUNCTION(cmd_update_memory, CmdUpdateMemoryKHR);
    UML_DEVICE_FUNCTION(cmd_fill_memory, CmdFillMemoryKHR);

    UML_DEVICE_FUNCTION(get_buffer_device_address, GetBufferDeviceAddress);
    UML_DEVICE_FUNCTION(cmd_pipeline_barrier2, CmdPipelineBarrier2);

    VkBuffer source = VK_NULL_HANDLE;
    VkBuffer destination = VK_NULL_HANDLE;
    VkDeviceMemory source_memory = VK_NULL_HANDLE;
    VkDeviceMemory destination_memory = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    void *destination_map = nullptr;
    const ScopeExit cleanup{[&] {
        if (destination_map != nullptr) {
            unmap_memory(device, destination_memory);
            destination_map = nullptr;
        }
        if (command_pool != VK_NULL_HANDLE) {
            destroy_command_pool(device, command_pool, nullptr);
            command_pool = VK_NULL_HANDLE;
        }
        if (destination != VK_NULL_HANDLE) {
            destroy_buffer(device, destination, nullptr);
            destination = VK_NULL_HANDLE;
        }
        if (source != VK_NULL_HANDLE) {
            destroy_buffer(device, source, nullptr);
            source = VK_NULL_HANDLE;
        }
        if (destination_memory != VK_NULL_HANDLE) {
            free_memory(device, destination_memory, nullptr);
            destination_memory = VK_NULL_HANDLE;
        }
        if (source_memory != VK_NULL_HANDLE) {
            free_memory(device, source_memory, nullptr);
            source_memory = VK_NULL_HANDLE;
        }
    }};

    constexpr VkDeviceSize buffer_size = 256;
    const VkBufferCreateInfo buffer_info{
        VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        nullptr,
        0,
        buffer_size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        VK_SHARING_MODE_EXCLUSIVE,
        0,
        nullptr,
    };
    require_result(create_buffer(device, &buffer_info, nullptr, &source),
                   "vkCreateBuffer(source)");
    require_result(
        create_buffer(device, &buffer_info, nullptr, &destination),
        "vkCreateBuffer(destination)");

    VkMemoryRequirements source_requirements{};
    VkMemoryRequirements destination_requirements{};
    get_buffer_memory_requirements(device, source, &source_requirements);
    get_buffer_memory_requirements(device, destination,
                                   &destination_requirements);
    const uint32_t compatible_memory_types =
        source_requirements.memoryTypeBits &
        destination_requirements.memoryTypeBits;
    const uint32_t memory_type = find_memory_type(
        memory, compatible_memory_types, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        "no host-visible buffer-device-address memory type");

    const VkMemoryAllocateFlagsInfo address_allocation{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        nullptr,
        VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
        0,
    };
    const VkMemoryAllocateInfo source_allocation{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        &address_allocation,
        source_requirements.size,
        memory_type,
    };
    const VkMemoryAllocateInfo destination_allocation{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        &address_allocation,
        destination_requirements.size,
        memory_type,
    };
    require_result(
        allocate_memory(device, &source_allocation, nullptr, &source_memory),
        "vkAllocateMemory(source)");
    require_result(allocate_memory(device, &destination_allocation, nullptr,
                                   &destination_memory),
                   "vkAllocateMemory(destination)");
    require_result(bind_buffer_memory(device, source, source_memory, 0),
                   "vkBindBufferMemory(source)");
    require_result(
        bind_buffer_memory(device, destination, destination_memory, 0),
        "vkBindBufferMemory(destination)");

    const VkBufferDeviceAddressInfo source_address_info{
        VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        nullptr,
        source,
    };
    const VkBufferDeviceAddressInfo destination_address_info{
        VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        nullptr,
        destination,
    };
    const VkDeviceAddress source_address =
        get_buffer_device_address(device, &source_address_info);
    const VkDeviceAddress destination_address =
        get_buffer_device_address(device, &destination_address_info);
    if (source_address == 0 || destination_address == 0) {
        throw std::runtime_error("buffer device address query returned zero");
    }

    const VkCommandPoolCreateInfo pool_info{
        VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        nullptr,
        VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        queue_family,
    };
    require_result(
        create_command_pool(device, &pool_info, nullptr, &command_pool),
        "vkCreateCommandPool");
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    const VkCommandBufferAllocateInfo command_buffer_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        nullptr,
        command_pool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        1,
    };
    require_result(allocate_command_buffers(device, &command_buffer_info,
                                            &command_buffer),
                   "vkAllocateCommandBuffers");
    const VkCommandBufferBeginInfo begin_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        nullptr,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        nullptr,
    };
    require_result(begin_command_buffer(command_buffer, &begin_info),
                   "vkBeginCommandBuffer");

    constexpr VkAddressCommandFlagsKHR address_flags =
        VK_ADDRESS_COMMAND_FULLY_BOUND_BIT_KHR;
    constexpr std::array<uint32_t, 4> update_data{
        0x01020304U,
        0x11223344U,
        0x89abcdefU,
        0xfedcba98U,
    };
    constexpr uint32_t fill_data = 0xa1b2c3d4U;
    constexpr VkDeviceSize update_size = sizeof(update_data);
    constexpr VkDeviceSize fill_size = 16;
    constexpr VkDeviceSize copy_size = update_size + fill_size;
    const VkDeviceAddressRangeKHR update_range{
        source_address,
        update_size,
    };
    const VkDeviceAddressRangeKHR fill_range{
        source_address + update_size,
        fill_size,
    };
    cmd_update_memory(command_buffer, &update_range, address_flags,
                      update_size, update_data.data());
    cmd_fill_memory(command_buffer, &fill_range, address_flags, fill_data);

    VkMemoryRangeBarrierKHR memory_barrier{
        VK_STRUCTURE_TYPE_MEMORY_RANGE_BARRIER_KHR,
        nullptr,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        {source_address, copy_size},
        address_flags,
    };
    const VkMemoryRangeBarriersInfoKHR range_barriers{
        VK_STRUCTURE_TYPE_MEMORY_RANGE_BARRIERS_INFO_KHR,
        nullptr,
        1,
        &memory_barrier,
    };
    VkDependencyInfo dependency{
        VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        &range_barriers,
        0,
        0,
        nullptr,
        0,
        nullptr,
        0,
        nullptr,
    };
    cmd_pipeline_barrier2(command_buffer, &dependency);

    const VkDeviceMemoryCopyKHR copy_region{
        VK_STRUCTURE_TYPE_DEVICE_MEMORY_COPY_KHR,
        nullptr,
        {source_address, copy_size},
        address_flags,
        {destination_address, copy_size},
        address_flags,
    };
    const VkCopyDeviceMemoryInfoKHR copy_info{
        VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_INFO_KHR,
        nullptr,
        1,
        &copy_region,
    };
    cmd_copy_memory(command_buffer, &copy_info);

    memory_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    memory_barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    memory_barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    memory_barrier.addressRange = {destination_address, copy_size};
    cmd_pipeline_barrier2(command_buffer, &dependency);

    const VkBindIndexBuffer3InfoKHR index_binding{
        VK_STRUCTURE_TYPE_BIND_INDEX_BUFFER_3_INFO_KHR,
        nullptr,
        {destination_address, copy_size},
        address_flags,
        VK_INDEX_TYPE_UINT32,
    };
    cmd_bind_index_buffer(command_buffer, &index_binding);
    const VkBindVertexBuffer3InfoKHR vertex_binding{
        VK_STRUCTURE_TYPE_BIND_VERTEX_BUFFER_3_INFO_KHR,
        nullptr,
        VK_TRUE,
        {destination_address, copy_size, 16},
        address_flags,
    };
    cmd_bind_vertex_buffers3(command_buffer, 0, 1, &vertex_binding);
    report.state_commands_recorded = true;

    require_result(end_command_buffer(command_buffer), "vkEndCommandBuffer");
    VkQueue queue = VK_NULL_HANDLE;
    get_device_queue(device, queue_family, 0, &queue);
    if (queue == VK_NULL_HANDLE) {
        throw std::runtime_error("vkGetDeviceQueue returned null");
    }
    const VkSubmitInfo submit_info{
        VK_STRUCTURE_TYPE_SUBMIT_INFO,
        nullptr,
        0,
        nullptr,
        nullptr,
        1,
        &command_buffer,
        0,
        nullptr,
    };
    require_result(queue_submit(queue, 1, &submit_info, VK_NULL_HANDLE),
                   "vkQueueSubmit");
    require_result(queue_wait_idle(queue), "vkQueueWaitIdle");

    require_result(map_memory(device, destination_memory, 0, VK_WHOLE_SIZE, 0,
                              &destination_map),
                   "vkMapMemory(destination)");
    const VkMappedMemoryRange mapped_range{
        VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        nullptr,
        destination_memory,
        0,
        VK_WHOLE_SIZE,
    };
    require_result(invalidate_memory(device, 1, &mapped_range),
                   "vkInvalidateMappedMemoryRanges");
    std::array<uint32_t, 8> expected{};
    std::copy(update_data.begin(), update_data.end(), expected.begin());
    std::fill(expected.begin() + update_data.size(), expected.end(),
              fill_data);
    if (std::memcmp(destination_map, expected.data(), copy_size) != 0) {
        throw std::runtime_error(
            "device-address update/fill/copy verification mismatch");
    }
    report.transfer_verified = true;
    return report;
}

void write_device_address_commands_report(
    std::ostream &output,
    const DeviceAddressCommandsReport &report)
{
    output << "{\"entrypoint_count\":" << report.entrypoint_count
           << ",\"state_commands_recorded\":"
           << json_bool(report.state_commands_recorded)
           << ",\"transfer_verified\":"
           << json_bool(report.transfer_verified) << '}';
}

struct DynamicLogicOpReport {
    uint32_t entrypoint_count = 0;
    uint32_t operation_count = 0;
    bool results_verified = false;
    bool pipeline_reuse_verified = false;
};

uint8_t apply_logic_op(VkLogicOp operation, uint8_t source, uint8_t destination)
{
    switch (operation) {
    case VK_LOGIC_OP_CLEAR:
        return 0;
    case VK_LOGIC_OP_AND:
        return source & destination;
    case VK_LOGIC_OP_AND_REVERSE:
        return source & static_cast<uint8_t>(~destination);
    case VK_LOGIC_OP_COPY:
        return source;
    case VK_LOGIC_OP_AND_INVERTED:
        return static_cast<uint8_t>(~source) & destination;
    case VK_LOGIC_OP_NO_OP:
        return destination;
    case VK_LOGIC_OP_XOR:
        return source ^ destination;
    case VK_LOGIC_OP_OR:
        return source | destination;
    case VK_LOGIC_OP_NOR:
        return static_cast<uint8_t>(~(source | destination));
    case VK_LOGIC_OP_EQUIVALENT:
        return static_cast<uint8_t>(~(source ^ destination));
    case VK_LOGIC_OP_INVERT:
        return static_cast<uint8_t>(~destination);
    case VK_LOGIC_OP_OR_REVERSE:
        return source | static_cast<uint8_t>(~destination);
    case VK_LOGIC_OP_COPY_INVERTED:
        return static_cast<uint8_t>(~source);
    case VK_LOGIC_OP_OR_INVERTED:
        return static_cast<uint8_t>(~source) | destination;
    case VK_LOGIC_OP_NAND:
        return static_cast<uint8_t>(~(source & destination));
    case VK_LOGIC_OP_SET:
        return UINT8_MAX;
    default:
        throw std::runtime_error("unknown Vulkan logic operation");
    }
}

DynamicLogicOpReport ProbeDevice::verify_dynamic_logic_ops(
    uint32_t queue_family,
    const VkPhysicalDeviceMemoryProperties &memory) const
{
    DynamicLogicOpReport report;
    UML_DEVICE_FUNCTION(cmd_set_logic_op, CmdSetLogicOpEXT);
    ++report.entrypoint_count;

    VkShaderModule vertex_module = VK_NULL_HANDLE;
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkImageView image_view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    void *readback_map = nullptr;
    const ScopeExit cleanup{[&] {
        if (readback_map != nullptr) {
            unmap_memory(device, readback_memory);
            readback_map = nullptr;
        }
        if (command_pool != VK_NULL_HANDLE) {
            destroy_command_pool(device, command_pool, nullptr);
        }
        if (pipeline != VK_NULL_HANDLE) {
            destroy_pipeline(device, pipeline, nullptr);
        }
        if (framebuffer != VK_NULL_HANDLE) {
            destroy_framebuffer(device, framebuffer, nullptr);
        }
        if (image_view != VK_NULL_HANDLE) {
            destroy_image_view(device, image_view, nullptr);
        }
        if (image != VK_NULL_HANDLE) {
            destroy_image(device, image, nullptr);
        }
        if (readback != VK_NULL_HANDLE) {
            destroy_buffer(device, readback, nullptr);
        }
        if (image_memory != VK_NULL_HANDLE) {
            free_memory(device, image_memory, nullptr);
        }
        if (readback_memory != VK_NULL_HANDLE) {
            free_memory(device, readback_memory, nullptr);
        }
        if (render_pass != VK_NULL_HANDLE) {
            destroy_render_pass(device, render_pass, nullptr);
        }
        if (pipeline_layout != VK_NULL_HANDLE) {
            destroy_pipeline_layout(device, pipeline_layout, nullptr);
        }
        if (fragment_module != VK_NULL_HANDLE) {
            destroy_shader_module(device, fragment_module, nullptr);
        }
        if (vertex_module != VK_NULL_HANDLE) {
            destroy_shader_module(device, vertex_module, nullptr);
        }
    }};

    VkShaderModuleCreateInfo shader_info{};
    shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_info.codeSize = fullscreen_vertex_spirv.size() * sizeof(uint32_t);
    shader_info.pCode = fullscreen_vertex_spirv.data();
    require_result(create_shader_module(device, &shader_info, nullptr,
                                        &vertex_module),
                   "dynamic logic-op vertex shader creation");
    shader_info.codeSize = gpl_fragment_spirv.size() * sizeof(uint32_t);
    shader_info.pCode = gpl_fragment_spirv.data();
    require_result(create_shader_module(device, &shader_info, nullptr,
                                        &fragment_module),
                   "dynamic logic-op fragment shader creation");

    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    require_result(create_pipeline_layout(device, &pipeline_layout_info,
                                          nullptr, &pipeline_layout),
                   "dynamic logic-op pipeline layout creation");

    constexpr VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
    VkAttachmentDescription attachment{};
    attachment.format = color_format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const VkAttachmentReference attachment_reference{
        0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &attachment_reference;
    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments = &attachment;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    require_result(create_render_pass(device, &render_pass_info, nullptr,
                                      &render_pass),
                   "dynamic logic-op render pass creation");

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = color_format;
    image_info.extent = {1, 1, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    require_result(create_image(device, &image_info, nullptr, &image),
                   "dynamic logic-op image creation");
    VkMemoryRequirements image_requirements{};
    get_image_memory_requirements(device, image, &image_requirements);
    const uint32_t image_memory_type = find_memory_type(
        memory, image_requirements.memoryTypeBits, 0,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        "no compatible logic-op image memory type");
    const VkMemoryAllocateInfo image_allocation{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        nullptr,
        image_requirements.size,
        image_memory_type,
    };
    require_result(allocate_memory(device, &image_allocation, nullptr,
                                   &image_memory),
                   "dynamic logic-op image memory allocation");
    require_result(bind_image_memory(device, image, image_memory, 0),
                   "dynamic logic-op image memory bind");

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = color_format;
    view_info.components = {
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
    };
    view_info.subresourceRange = {
        VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    require_result(create_image_view(device, &view_info, nullptr, &image_view),
                   "dynamic logic-op image view creation");
    VkFramebufferCreateInfo framebuffer_info{};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = render_pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &image_view;
    framebuffer_info.width = 1;
    framebuffer_info.height = 1;
    framebuffer_info.layers = 1;
    require_result(create_framebuffer(device, &framebuffer_info, nullptr,
                                      &framebuffer),
                   "dynamic logic-op framebuffer creation");

    constexpr std::array<VkLogicOp, 16> logic_ops{
        VK_LOGIC_OP_CLEAR,
        VK_LOGIC_OP_AND,
        VK_LOGIC_OP_AND_REVERSE,
        VK_LOGIC_OP_COPY,
        VK_LOGIC_OP_AND_INVERTED,
        VK_LOGIC_OP_NO_OP,
        VK_LOGIC_OP_XOR,
        VK_LOGIC_OP_OR,
        VK_LOGIC_OP_NOR,
        VK_LOGIC_OP_EQUIVALENT,
        VK_LOGIC_OP_INVERT,
        VK_LOGIC_OP_OR_REVERSE,
        VK_LOGIC_OP_COPY_INVERTED,
        VK_LOGIC_OP_OR_INVERTED,
        VK_LOGIC_OP_NAND,
        VK_LOGIC_OP_SET,
    };
    constexpr VkDeviceSize readback_size = logic_ops.size() * 4;
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = readback_size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    require_result(create_buffer(device, &buffer_info, nullptr, &readback),
                   "dynamic logic-op readback buffer creation");
    VkMemoryRequirements buffer_requirements{};
    get_buffer_memory_requirements(device, readback, &buffer_requirements);
    const uint32_t readback_memory_type = find_memory_type(
        memory, buffer_requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        "no host-visible logic-op readback memory type");
    const VkMemoryAllocateInfo buffer_allocation{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        nullptr,
        buffer_requirements.size,
        readback_memory_type,
    };
    require_result(allocate_memory(device, &buffer_allocation, nullptr,
                                   &readback_memory),
                   "dynamic logic-op readback memory allocation");
    require_result(bind_buffer_memory(device, readback, readback_memory, 0),
                   "dynamic logic-op readback memory bind");

    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_VERTEX_BIT, vertex_module, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_FRAGMENT_BIT, fragment_module, "main", nullptr},
    }};
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.logicOpEnable = VK_TRUE;
    blend.logicOp = VK_LOGIC_OP_COPY;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    constexpr std::array<VkDynamicState, 3> dynamic_states{
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_LOGIC_OP_EXT,
    };
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = dynamic_states.size();
    dynamic_state.pDynamicStates = dynamic_states.data();
    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = stages.size();
    pipeline_info.pStages = stages.data();
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = pipeline_layout;
    pipeline_info.renderPass = render_pass;
    require_result(create_graphics_pipelines(device, VK_NULL_HANDLE, 1,
                                             &pipeline_info, nullptr,
                                             &pipeline),
                   "dynamic logic-op graphics pipeline creation");

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = queue_family;
    require_result(create_command_pool(device, &pool_info, nullptr,
                                       &command_pool),
                   "dynamic logic-op command pool creation");
    VkCommandBufferAllocateInfo command_buffer_info{};
    command_buffer_info.sType =
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_buffer_info.commandPool = command_pool;
    command_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_buffer_info.commandBufferCount = 1;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    require_result(allocate_command_buffers(device, &command_buffer_info,
                                            &command_buffer),
                   "dynamic logic-op command buffer allocation");
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    require_result(begin_command_buffer(command_buffer, &begin_info),
                   "dynamic logic-op command buffer begin");

    const VkImageSubresourceRange color_range{
        VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier image_barrier{};
    image_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    image_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    image_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.image = image;
    image_barrier.subresourceRange = color_range;
    cmd_pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &image_barrier);
    cmd_bind_pipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      pipeline);
    const VkViewport viewport{0.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F};
    const VkRect2D scissor{{0, 0}, {1, 1}};
    cmd_set_viewport(command_buffer, 0, 1, &viewport);
    cmd_set_scissor(command_buffer, 0, 1, &scissor);
    const VkClearValue clear_value{{{0.0F, 0.0F, 1.0F, 1.0F}}};
    VkRenderPassBeginInfo render_begin{};
    render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_begin.renderPass = render_pass;
    render_begin.framebuffer = framebuffer;
    render_begin.renderArea = scissor;
    render_begin.clearValueCount = 1;
    render_begin.pClearValues = &clear_value;

    for (std::size_t index = 0; index < logic_ops.size(); ++index) {
        cmd_begin_render_pass(command_buffer, &render_begin,
                              VK_SUBPASS_CONTENTS_INLINE);
        cmd_set_logic_op(command_buffer, logic_ops[index]);
        cmd_draw(command_buffer, 3, 1, 0, 0);
        cmd_end_render_pass(command_buffer);

        image_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        image_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        cmd_pipeline_barrier(
            command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
            &image_barrier);
        VkBufferImageCopy copy{};
        copy.bufferOffset = index * 4;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {1, 1, 1};
        cmd_copy_image_to_buffer(command_buffer, image,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 readback, 1, &copy);
        if (index + 1 != logic_ops.size()) {
            image_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            image_barrier.dstAccessMask =
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            image_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            image_barrier.newLayout =
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            cmd_pipeline_barrier(
                command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                0, nullptr, 0, nullptr, 1, &image_barrier);
        }
    }
    VkMemoryBarrier host_barrier{};
    host_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    cmd_pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_barrier,
                         0, nullptr, 0, nullptr);
    require_result(end_command_buffer(command_buffer),
                   "dynamic logic-op command buffer end");

    VkQueue queue = VK_NULL_HANDLE;
    get_device_queue(device, queue_family, 0, &queue);
    if (queue == VK_NULL_HANDLE) {
        throw std::runtime_error("dynamic logic-op queue is null");
    }
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer;
    require_result(queue_submit(queue, 1, &submit_info, VK_NULL_HANDLE),
                   "dynamic logic-op queue submit");
    require_result(queue_wait_idle(queue), "dynamic logic-op queue wait");

    require_result(map_memory(device, readback_memory, 0, VK_WHOLE_SIZE, 0,
                              &readback_map),
                   "dynamic logic-op readback map");
    const VkMappedMemoryRange mapped_range{
        VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        nullptr,
        readback_memory,
        0,
        VK_WHOLE_SIZE,
    };
    require_result(invalidate_memory(device, 1, &mapped_range),
                   "dynamic logic-op readback invalidate");
    constexpr std::array<uint8_t, 4> source{255, 0, 0, 255};
    constexpr std::array<uint8_t, 4> destination{0, 0, 255, 255};
    const auto *pixels = static_cast<const uint8_t *>(readback_map);
    for (std::size_t operation = 0; operation < logic_ops.size(); ++operation) {
        for (std::size_t component = 0; component < source.size(); ++component) {
            const uint8_t expected = apply_logic_op(
                logic_ops[operation], source[component],
                destination[component]);
            const uint8_t actual = pixels[operation * source.size() + component];
            if (actual != expected) {
                throw std::runtime_error(
                    "dynamic logic-op result mismatch for operation " +
                    std::to_string(operation) + ", component " +
                    std::to_string(component) + ": expected " +
                    std::to_string(expected) + ", got " +
                    std::to_string(actual));
            }
        }
    }
    report.operation_count = logic_ops.size();
    report.results_verified = true;
    report.pipeline_reuse_verified = true;
    return report;
}

void write_dynamic_logic_op_report(
    std::ostream &output,
    const DynamicLogicOpReport &report)
{
    output << "{\"entrypoint_count\":" << report.entrypoint_count
           << ",\"operation_count\":" << report.operation_count
           << ",\"results_verified\":"
           << json_bool(report.results_verified)
           << ",\"pipeline_reuse_verified\":"
           << json_bool(report.pipeline_reuse_verified) << '}';
}

struct LegacyVertexAttributesReport {
    uint32_t entrypoint_count = 0;
    VkResult compile_required_result = VK_ERROR_UNKNOWN;
    uint32_t variant_count = 0;
    uint64_t cold_draw_ns = 0;
    uint64_t cached_draw_ns = 0;
    uint64_t cold_switch_ns = 0;
    uint64_t cached_switch_ns = 0;
    std::array<uint64_t, 2> concurrent_draw_ns{};
    bool same_key_race_recorded = false;
    bool numeric_mismatch_verified = false;
    bool unaligned_offset_stride_verified = false;
    bool parent_destroy_verified = false;
};

LegacyVertexAttributesReport ProbeDevice::verify_legacy_vertex_attributes(
    uint32_t queue_family,
    const VkPhysicalDeviceMemoryProperties &memory) const
{
    LegacyVertexAttributesReport report;
    UML_DEVICE_FUNCTION(cmd_set_vertex_input, CmdSetVertexInputEXT);
    ++report.entrypoint_count;

    VkShaderModule vertex_module = VK_NULL_HANDLE;
    VkShaderModule fragment_module = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipeline race_pipeline = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    VkImageView image_view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkBuffer vertex_buffer = VK_NULL_HANDLE;
    VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readback_memory = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    std::array<VkCommandPool, 2> race_pools{};
    void *readback_map = nullptr;
    const ScopeExit cleanup{[&] {
        if (readback_map != nullptr) {
            unmap_memory(device, readback_memory);
            readback_map = nullptr;
        }
        for (const auto pool : race_pools) {
            if (pool != VK_NULL_HANDLE) {
                destroy_command_pool(device, pool, nullptr);
            }
        }
        if (command_pool != VK_NULL_HANDLE) {
            destroy_command_pool(device, command_pool, nullptr);
        }
        if (race_pipeline != VK_NULL_HANDLE) {
            destroy_pipeline(device, race_pipeline, nullptr);
        }
        if (pipeline != VK_NULL_HANDLE) {
            destroy_pipeline(device, pipeline, nullptr);
        }
        if (framebuffer != VK_NULL_HANDLE) {
            destroy_framebuffer(device, framebuffer, nullptr);
        }
        if (image_view != VK_NULL_HANDLE) {
            destroy_image_view(device, image_view, nullptr);
        }
        if (image != VK_NULL_HANDLE) {
            destroy_image(device, image, nullptr);
        }
        if (vertex_buffer != VK_NULL_HANDLE) {
            destroy_buffer(device, vertex_buffer, nullptr);
        }
        if (readback != VK_NULL_HANDLE) {
            destroy_buffer(device, readback, nullptr);
        }
        if (image_memory != VK_NULL_HANDLE) {
            free_memory(device, image_memory, nullptr);
        }
        if (vertex_memory != VK_NULL_HANDLE) {
            free_memory(device, vertex_memory, nullptr);
        }
        if (readback_memory != VK_NULL_HANDLE) {
            free_memory(device, readback_memory, nullptr);
        }
        if (render_pass != VK_NULL_HANDLE) {
            destroy_render_pass(device, render_pass, nullptr);
        }
        if (pipeline_layout != VK_NULL_HANDLE) {
            destroy_pipeline_layout(device, pipeline_layout, nullptr);
        }
        if (fragment_module != VK_NULL_HANDLE) {
            destroy_shader_module(device, fragment_module, nullptr);
        }
        if (vertex_module != VK_NULL_HANDLE) {
            destroy_shader_module(device, vertex_module, nullptr);
        }
    }};

    VkShaderModuleCreateInfo shader_info{};
    shader_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    shader_info.codeSize = legacy_vertex_spirv.size() * sizeof(uint32_t);
    shader_info.pCode = legacy_vertex_spirv.data();
    require_result(create_shader_module(device, &shader_info, nullptr,
                                        &vertex_module),
                   "legacy vertex shader creation");
    shader_info.codeSize = gpl_fragment_spirv.size() * sizeof(uint32_t);
    shader_info.pCode = gpl_fragment_spirv.data();
    require_result(create_shader_module(device, &shader_info, nullptr,
                                        &fragment_module),
                   "legacy fragment shader creation");

    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType =
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    require_result(create_pipeline_layout(device, &pipeline_layout_info,
                                          nullptr, &pipeline_layout),
                   "legacy pipeline layout creation");

    constexpr VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
    VkAttachmentDescription attachment{};
    attachment.format = color_format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const VkAttachmentReference attachment_reference{
        0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &attachment_reference;
    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments = &attachment;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    require_result(create_render_pass(device, &render_pass_info, nullptr,
                                      &render_pass),
                   "legacy render pass creation");

    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_VERTEX_BIT, vertex_module, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_FRAGMENT_BIT, fragment_module, "main", nullptr},
    }};
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    constexpr std::array<VkDynamicState, 3> dynamic_states{
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_VERTEX_INPUT_EXT,
    };
    VkPipelineDynamicStateCreateInfo dynamic_state{};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = dynamic_states.size();
    dynamic_state.pDynamicStates = dynamic_states.data();
    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = stages.size();
    pipeline_info.pStages = stages.data();
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = pipeline_layout;
    pipeline_info.renderPass = render_pass;

    pipeline_info.flags =
        VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    VkPipeline unexpected_pipeline = VK_NULL_HANDLE;
    report.compile_required_result = create_graphics_pipelines(
        device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
        &unexpected_pipeline);
    if (unexpected_pipeline != VK_NULL_HANDLE) {
        destroy_pipeline(device, unexpected_pipeline, nullptr);
    }
    if (report.compile_required_result != VK_PIPELINE_COMPILE_REQUIRED) {
        throw std::runtime_error(
            "dynamic vertex-input fail-on-compile did not return "
            "PIPELINE_COMPILE_REQUIRED: " +
            std::to_string(report.compile_required_result));
    }

    pipeline_info.flags = 0;
    require_result(create_graphics_pipelines(device, VK_NULL_HANDLE, 1,
                                             &pipeline_info, nullptr,
                                             &pipeline),
                   "legacy graphics pipeline creation");
    require_result(create_graphics_pipelines(device, VK_NULL_HANDLE, 1,
                                             &pipeline_info, nullptr,
                                             &race_pipeline),
                   "legacy race pipeline creation");

    constexpr uint32_t legacy_draw_count = 4;
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = color_format;
    image_info.extent = {legacy_draw_count, 1, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    require_result(create_image(device, &image_info, nullptr, &image),
                   "legacy image creation");
    VkMemoryRequirements image_requirements{};
    get_image_memory_requirements(device, image, &image_requirements);
    const uint32_t image_memory_type = find_memory_type(
        memory, image_requirements.memoryTypeBits, 0,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        "no compatible legacy image memory type");
    const VkMemoryAllocateInfo image_allocation{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        nullptr,
        image_requirements.size,
        image_memory_type,
    };
    require_result(allocate_memory(device, &image_allocation, nullptr,
                                   &image_memory),
                   "legacy image memory allocation");
    require_result(bind_image_memory(device, image, image_memory, 0),
                   "legacy image memory bind");

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = color_format;
    view_info.components = {
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
    };
    view_info.subresourceRange = {
        VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    require_result(create_image_view(device, &view_info, nullptr, &image_view),
                   "legacy image view creation");
    VkFramebufferCreateInfo framebuffer_info{};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = render_pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &image_view;
    framebuffer_info.width = legacy_draw_count;
    framebuffer_info.height = 1;
    framebuffer_info.layers = 1;
    require_result(create_framebuffer(device, &framebuffer_info, nullptr,
                                      &framebuffer),
                   "legacy framebuffer creation");

    constexpr VkDeviceSize vertex_buffer_size = 20;
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = vertex_buffer_size;
    buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    require_result(create_buffer(device, &buffer_info, nullptr,
                                 &vertex_buffer),
                   "legacy vertex buffer creation");
    VkMemoryRequirements vertex_requirements{};
    get_buffer_memory_requirements(device, vertex_buffer,
                                   &vertex_requirements);
    const uint32_t vertex_memory_type = find_memory_type(
        memory, vertex_requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        "no host-visible legacy vertex memory type");
    const VkMemoryAllocateInfo vertex_allocation{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        nullptr,
        vertex_requirements.size,
        vertex_memory_type,
    };
    require_result(allocate_memory(device, &vertex_allocation, nullptr,
                                   &vertex_memory),
                   "legacy vertex memory allocation");
    require_result(bind_buffer_memory(device, vertex_buffer, vertex_memory, 0),
                   "legacy vertex memory bind");
    void *vertex_map = nullptr;
    require_result(map_memory(device, vertex_memory, 0, VK_WHOLE_SIZE, 0,
                              &vertex_map),
                   "legacy vertex memory map");
    std::memset(vertex_map, UINT8_MAX, vertex_buffer_size);
    const VkMappedMemoryRange vertex_range{
        VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        nullptr,
        vertex_memory,
        0,
        VK_WHOLE_SIZE,
    };
    require_result(flush_memory(device, 1, &vertex_range),
                   "legacy vertex memory flush");
    unmap_memory(device, vertex_memory);

    buffer_info.size = legacy_draw_count * 4;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    require_result(create_buffer(device, &buffer_info, nullptr, &readback),
                   "legacy readback buffer creation");
    VkMemoryRequirements readback_requirements{};
    get_buffer_memory_requirements(device, readback, &readback_requirements);
    const uint32_t readback_memory_type = find_memory_type(
        memory, readback_requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        "no host-visible legacy readback memory type");
    const VkMemoryAllocateInfo readback_allocation{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        nullptr,
        readback_requirements.size,
        readback_memory_type,
    };
    require_result(allocate_memory(device, &readback_allocation, nullptr,
                                   &readback_memory),
                   "legacy readback memory allocation");
    require_result(bind_buffer_memory(device, readback, readback_memory, 0),
                   "legacy readback memory bind");

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = queue_family;
    require_result(create_command_pool(device, &pool_info, nullptr,
                                       &command_pool),
                   "legacy command pool creation");
    for (auto &pool : race_pools) {
        require_result(create_command_pool(device, &pool_info, nullptr, &pool),
                       "legacy race command pool creation");
    }

    VkCommandBufferAllocateInfo command_buffer_info{};
    command_buffer_info.sType =
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    command_buffer_info.commandPool = command_pool;
    command_buffer_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_buffer_info.commandBufferCount = 1;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    require_result(allocate_command_buffers(device, &command_buffer_info,
                                            &command_buffer),
                   "legacy command buffer allocation");
    std::array<VkCommandBuffer, 2> race_command_buffers{};
    for (std::size_t index = 0; index < race_command_buffers.size(); ++index) {
        command_buffer_info.commandPool = race_pools[index];
        require_result(allocate_command_buffers(
                           device, &command_buffer_info,
                           &race_command_buffers[index]),
                       "legacy race command buffer allocation");
    }

    const VkViewport viewport{0.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F};
    const VkRect2D scissor{{0, 0}, {legacy_draw_count, 1}};
    const VkClearValue clear_value{{{0.0F, 0.0F, 1.0F, 1.0F}}};
    VkRenderPassBeginInfo render_begin{};
    render_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_begin.renderPass = render_pass;
    render_begin.framebuffer = framebuffer;
    render_begin.renderArea = scissor;
    render_begin.clearValueCount = 1;
    render_begin.pClearValues = &clear_value;
    VkVertexInputBindingDescription2EXT binding{};
    binding.sType =
        VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT;
    binding.binding = 0;
    binding.stride = 3;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    binding.divisor = 1;
    VkVertexInputAttributeDescription2EXT attribute{};
    attribute.sType =
        VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT;
    attribute.location = 0;
    attribute.binding = 0;
    attribute.format = VK_FORMAT_R8_UNORM;
    attribute.offset = 1;
    constexpr VkDeviceSize vertex_binding_offset = 1;
    VkVertexInputAttributeDescription2EXT race_attribute = attribute;
    race_attribute.format = VK_FORMAT_R16_UINT;
    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    std::atomic<uint32_t> race_ready{0};
    std::atomic<bool> race_go{false};
    std::array<VkResult, 2> race_results{
        VK_ERROR_UNKNOWN, VK_ERROR_UNKNOWN};
    std::array<std::thread, 2> race_threads;
    for (std::size_t index = 0; index < race_threads.size(); ++index) {
        race_threads[index] = std::thread([&, index] {
            const VkCommandBuffer race_command_buffer =
                race_command_buffers[index];
            race_results[index] =
                begin_command_buffer(race_command_buffer, &begin_info);
            if (race_results[index] == VK_SUCCESS) {
                cmd_bind_pipeline(race_command_buffer,
                                  VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  race_pipeline);
                cmd_set_viewport(race_command_buffer, 0, 1, &viewport);
                cmd_set_scissor(race_command_buffer, 0, 1, &scissor);
                cmd_bind_vertex_buffers(race_command_buffer, 0, 1,
                                        &vertex_buffer,
                                        &vertex_binding_offset);
                cmd_set_vertex_input(race_command_buffer, 1, &binding, 1,
                                     &race_attribute);
                cmd_begin_render_pass(race_command_buffer, &render_begin,
                                      VK_SUBPASS_CONTENTS_INLINE);
            }
            race_ready.fetch_add(1, std::memory_order_release);
            while (!race_go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            if (race_results[index] == VK_SUCCESS) {
                const auto begin = std::chrono::steady_clock::now();
                cmd_draw(race_command_buffer, 3, 1, 0, 0);
                const auto end = std::chrono::steady_clock::now();
                report.concurrent_draw_ns[index] =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        end - begin)
                        .count();
                cmd_end_render_pass(race_command_buffer);
                race_results[index] = end_command_buffer(race_command_buffer);
            }
        });
    }
    while (race_ready.load(std::memory_order_acquire) != race_threads.size()) {
        std::this_thread::yield();
    }
    race_go.store(true, std::memory_order_release);
    for (auto &thread : race_threads) {
        thread.join();
    }
    for (const VkResult result : race_results) {
        if (result != VK_SUCCESS) {
            throw std::runtime_error(
                "concurrent same-key vertex-input recording failed: " +
                std::to_string(result));
        }
    }
    report.same_key_race_recorded = true;
    for (auto &pool : race_pools) {
        destroy_command_pool(device, pool, nullptr);
        pool = VK_NULL_HANDLE;
    }
    destroy_pipeline(device, race_pipeline, nullptr);
    race_pipeline = VK_NULL_HANDLE;

    require_result(begin_command_buffer(command_buffer, &begin_info),
                   "legacy command buffer begin");
    const VkImageSubresourceRange color_range{
        VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier image_barrier{};
    image_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    image_barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    image_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.image = image;
    image_barrier.subresourceRange = color_range;
    cmd_pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &image_barrier);
    cmd_bind_pipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      pipeline);
    cmd_set_viewport(command_buffer, 0, 1, &viewport);
    cmd_set_scissor(command_buffer, 0, 1, &scissor);
    cmd_bind_vertex_buffers(command_buffer, 0, 1, &vertex_buffer,
                            &vertex_binding_offset);
    cmd_begin_render_pass(command_buffer, &render_begin,
                          VK_SUBPASS_CONTENTS_INLINE);

    uint32_t draw_index = 0;
    const auto timed_draw = [&](uint64_t &elapsed_ns) {
        const VkViewport draw_viewport{
            static_cast<float>(draw_index), 0.0F, 1.0F, 1.0F, 0.0F, 1.0F};
        const VkRect2D draw_scissor{
            {static_cast<int32_t>(draw_index), 0}, {1, 1}};
        cmd_set_viewport(command_buffer, 0, 1, &draw_viewport);
        cmd_set_scissor(command_buffer, 0, 1, &draw_scissor);
        ++draw_index;
        const auto begin = std::chrono::steady_clock::now();
        cmd_draw(command_buffer, 3, 1, 0, 0);
        const auto end = std::chrono::steady_clock::now();
        elapsed_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
                .count();
    };
    cmd_set_vertex_input(command_buffer, 1, &binding, 1, &attribute);
    timed_draw(report.cold_draw_ns);
    timed_draw(report.cached_draw_ns);
    attribute.format = VK_FORMAT_R16_UNORM;
    cmd_set_vertex_input(command_buffer, 1, &binding, 1, &attribute);
    timed_draw(report.cold_switch_ns);
    binding.stride = 5;
    attribute.offset = 2;
    cmd_set_vertex_input(command_buffer, 1, &binding, 1, &attribute);
    timed_draw(report.cached_switch_ns);
    report.variant_count = 2;
    cmd_end_render_pass(command_buffer);

    image_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    image_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    image_barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    image_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    cmd_pipeline_barrier(
        command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
        &image_barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {legacy_draw_count, 1, 1};
    cmd_copy_image_to_buffer(command_buffer, image,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             readback, 1, &copy);
    VkMemoryBarrier host_barrier{};
    host_barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    cmd_pipeline_barrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_barrier,
                         0, nullptr, 0, nullptr);
    require_result(end_command_buffer(command_buffer),
                   "legacy command buffer end");

    VkQueue queue = VK_NULL_HANDLE;
    get_device_queue(device, queue_family, 0, &queue);
    if (queue == VK_NULL_HANDLE) {
        throw std::runtime_error("legacy queue is null");
    }
    VkSubmitInfo submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer;
    require_result(queue_submit(queue, 1, &submit_info, VK_NULL_HANDLE),
                   "legacy queue submit");
    require_result(queue_wait_idle(queue), "legacy queue wait");

    require_result(map_memory(device, readback_memory, 0, VK_WHOLE_SIZE, 0,
                              &readback_map),
                   "legacy readback map");
    const VkMappedMemoryRange readback_range{
        VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        nullptr,
        readback_memory,
        0,
        VK_WHOLE_SIZE,
    };
    require_result(invalidate_memory(device, 1, &readback_range),
                   "legacy readback invalidate");
    constexpr std::array<uint8_t, 4> expected{255, 0, 0, 255};
    for (uint32_t index = 0; index < legacy_draw_count; ++index) {
        const auto *actual = static_cast<const uint8_t *>(readback_map) +
                             index * expected.size();
        if (std::memcmp(actual, expected.data(), expected.size()) != 0) {
            const std::string actual_string =
                std::to_string(actual[0]) + "," + std::to_string(actual[1]) +
                "," + std::to_string(actual[2]) + "," +
                std::to_string(actual[3]);
            throw std::runtime_error(
                "legacy vertex draw " + std::to_string(index) +
                " mismatch: expected red, got " + actual_string);
        }
    }
    report.numeric_mismatch_verified = true;
    report.unaligned_offset_stride_verified = true;
    unmap_memory(device, readback_memory);
    readback_map = nullptr;

    destroy_pipeline(device, pipeline, nullptr);
    pipeline = VK_NULL_HANDLE;
    report.parent_destroy_verified = true;

    return report;
}

void write_legacy_vertex_attributes_report(
    std::ostream &output,
    const LegacyVertexAttributesReport &report)
{
    output << "{\"entrypoint_count\":" << report.entrypoint_count
           << ",\"compile_required_result\":"
           << report.compile_required_result
           << ",\"variant_count\":" << report.variant_count
           << ",\"cold_draw_ns\":" << report.cold_draw_ns
           << ",\"cached_draw_ns\":" << report.cached_draw_ns
           << ",\"cold_switch_ns\":" << report.cold_switch_ns
           << ",\"cached_switch_ns\":" << report.cached_switch_ns
           << ",\"concurrent_draw_ns\":["
           << report.concurrent_draw_ns[0] << ','
           << report.concurrent_draw_ns[1]
           << "],\"same_key_race_recorded\":"
           << json_bool(report.same_key_race_recorded)
           << ",\"numeric_mismatch_verified\":"
           << json_bool(report.numeric_mismatch_verified)
           << ",\"unaligned_offset_stride_verified\":"
           << json_bool(report.unaligned_offset_stride_verified)
           << ",\"parent_destroy_verified\":"
           << json_bool(report.parent_destroy_verified) << '}';
}

}  // namespace

int run_kosmickrisp_capability_probe(std::ostream &output)
{
    @autoreleasepool {
        try {
            const auto enumerate_instance_version =
                load_function<PFN_vkEnumerateInstanceVersion>(
                    VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
            const auto enumerate_instance_extensions =
                load_function<PFN_vkEnumerateInstanceExtensionProperties>(
                    VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
            const auto create_instance = load_function<PFN_vkCreateInstance>(
                VK_NULL_HANDLE, "vkCreateInstance");

            uint32_t loader_version = VK_API_VERSION_1_0;
            if (enumerate_instance_version(&loader_version) != VK_SUCCESS) {
                throw std::runtime_error("vkEnumerateInstanceVersion failed");
            }
            const auto instance_extensions =
                enumerate_properties<VkExtensionProperties>(
                    [&](uint32_t *count, VkExtensionProperties *data) {
                        return enumerate_instance_extensions(nullptr, count,
                                                             data);
                    });
            const bool has_portability_enumeration = has_extension(
                instance_extensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);

            const char *instance_extension =
                VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
            const VkApplicationInfo application{
                VK_STRUCTURE_TYPE_APPLICATION_INFO,
                nullptr,
                "linux-um-darwin-kosmickrisp-probe",
                1,
                "linux-um-darwin",
                1,
                loader_version,
            };
            const VkInstanceCreateInfo instance_info{
                VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                nullptr,
                has_portability_enumeration
                    ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
                    : 0U,
                &application,
                0,
                nullptr,
                has_portability_enumeration ? 1U : 0U,
                has_portability_enumeration ? &instance_extension : nullptr,
            };
            VkInstance instance = VK_NULL_HANDLE;
            const VkResult instance_result =
                create_instance(&instance_info, nullptr, &instance);
            if (instance_result != VK_SUCCESS) {
                throw std::runtime_error("vkCreateInstance failed: " +
                                         std::to_string(instance_result));
            }

            const auto destroy_instance =
                load_function<PFN_vkDestroyInstance>(instance,
                                                     "vkDestroyInstance");
            const auto enumerate_physical_devices =
                load_function<PFN_vkEnumeratePhysicalDevices>(
                    instance, "vkEnumeratePhysicalDevices");
            const auto enumerate_device_extensions =
                load_function<PFN_vkEnumerateDeviceExtensionProperties>(
                    instance, "vkEnumerateDeviceExtensionProperties");
            const auto get_properties2 =
                load_function<PFN_vkGetPhysicalDeviceProperties2>(
                    instance, "vkGetPhysicalDeviceProperties2");
            const auto get_features2 =
                load_function<PFN_vkGetPhysicalDeviceFeatures2>(
                    instance, "vkGetPhysicalDeviceFeatures2");
            const auto get_memory =
                load_function<PFN_vkGetPhysicalDeviceMemoryProperties>(
                    instance, "vkGetPhysicalDeviceMemoryProperties");
            const auto get_queues =
                load_function<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
                    instance, "vkGetPhysicalDeviceQueueFamilyProperties");
            const auto get_format =
                load_function<PFN_vkGetPhysicalDeviceFormatProperties>(
                    instance, "vkGetPhysicalDeviceFormatProperties");
            const auto get_image_format_properties =
                load_function<PFN_vkGetPhysicalDeviceImageFormatProperties>(
                    instance, "vkGetPhysicalDeviceImageFormatProperties");
            const auto create_device = load_function<PFN_vkCreateDevice>(
                instance, "vkCreateDevice");

            uint32_t physical_count = 0;
            VkResult result =
                enumerate_physical_devices(instance, &physical_count, nullptr);
            if (result != VK_SUCCESS || physical_count == 0) {
                destroy_instance(instance, nullptr);
                throw std::runtime_error(
                    "KosmicKrisp exposed no physical device");
            }
            std::vector<VkPhysicalDevice> physical_devices(physical_count);
            result = enumerate_physical_devices(instance, &physical_count,
                                                physical_devices.data());
            if (result != VK_SUCCESS) {
                destroy_instance(instance, nullptr);
                throw std::runtime_error(
                    "vkEnumeratePhysicalDevices payload failed");
            }

            output << "{\"devices\":[";

            for (uint32_t device_index = 0; device_index < physical_count;
                 ++device_index) {
                if (device_index != 0) {
                    output << ',';
                }
                const VkPhysicalDevice physical =
                    physical_devices[device_index];
                const auto device_extensions =
                    enumerate_properties<VkExtensionProperties>(
                        [&](uint32_t *count, VkExtensionProperties *data) {
                            return enumerate_device_extensions(
                                physical, nullptr, count, data);
                        });
                const bool has_portability_subset = has_extension(
                    device_extensions, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
                const bool has_dynamic_rendering_unused_attachments =
                    has_extension(
                        device_extensions,
                        VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME);
                const bool has_swapchain = has_extension(
                    device_extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
                const bool has_incremental_present = has_extension(
                    device_extensions,
                    VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME);
                const bool has_non_seamless_cube_map = has_extension(
                    device_extensions,
                    VK_EXT_NON_SEAMLESS_CUBE_MAP_EXTENSION_NAME);
                const bool has_pipeline_library = has_extension(
                    device_extensions,
                    VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
                const bool has_graphics_pipeline_library = has_extension(
                    device_extensions,
                    VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
                const bool has_device_address_commands = has_extension(
                    device_extensions,
                    VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME);
                const bool has_extended_dynamic_state2 = has_extension(
                    device_extensions,
                    VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
                const bool has_vertex_input_dynamic_state = has_extension(
                    device_extensions,
                    VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
                const bool has_legacy_vertex_attributes = has_extension(
                    device_extensions,
                    VK_EXT_LEGACY_VERTEX_ATTRIBUTES_EXTENSION_NAME);

                VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT
                    graphics_pipeline_library_properties{};
                graphics_pipeline_library_properties.sType =
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT;
                VkPhysicalDeviceLegacyVertexAttributesPropertiesEXT
                    legacy_vertex_attributes_properties{};
                legacy_vertex_attributes_properties.sType =
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LEGACY_VERTEX_ATTRIBUTES_PROPERTIES_EXT;
                void *properties_next = nullptr;
                const auto prepend_property = [&properties_next](auto &value) {
                    value.pNext = properties_next;
                    properties_next = &value;
                };
                if (has_legacy_vertex_attributes)
                    prepend_property(legacy_vertex_attributes_properties);
                if (has_graphics_pipeline_library)
                    prepend_property(graphics_pipeline_library_properties);
                VkPhysicalDeviceProperties2 properties{};
                properties.sType =
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
                properties.pNext = properties_next;
                get_properties2(physical, &properties);

                VkPhysicalDevicePortabilitySubsetFeaturesKHR subset_features{};
                subset_features.sType =
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR;
                VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT
                    unused_attachments_features{};
                unused_attachments_features.sType =
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT;
                VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT
                    non_seamless_cube_map_features{};
                non_seamless_cube_map_features.sType =
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT;
                VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT
                    graphics_pipeline_library_features{};
                graphics_pipeline_library_features.sType =
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT;
                VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR
                    device_address_commands_features{};
                device_address_commands_features.sType =
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_ADDRESS_COMMANDS_FEATURES_KHR;
                VkPhysicalDeviceBufferDeviceAddressFeatures
                    buffer_device_address_features{};
                buffer_device_address_features.sType =
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
                VkPhysicalDeviceExtendedDynamicState2FeaturesEXT
                    extended_dynamic_state2_features{};
                extended_dynamic_state2_features.sType =
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT;
                VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT
                    vertex_input_dynamic_state_features{};
                vertex_input_dynamic_state_features.sType =
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT;
                VkPhysicalDeviceLegacyVertexAttributesFeaturesEXT
                    legacy_vertex_attributes_features{};
                legacy_vertex_attributes_features.sType =
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LEGACY_VERTEX_ATTRIBUTES_FEATURES_EXT;

                void *features_next = nullptr;
                const auto prepend_feature = [&features_next](auto &value) {
                    value.pNext = features_next;
                    features_next = &value;
                };
                if (has_device_address_commands) {
                    prepend_feature(buffer_device_address_features);
                    prepend_feature(device_address_commands_features);
                }
                if (has_graphics_pipeline_library)
                    prepend_feature(graphics_pipeline_library_features);
                if (has_non_seamless_cube_map)
                    prepend_feature(non_seamless_cube_map_features);
                if (has_dynamic_rendering_unused_attachments)
                    prepend_feature(unused_attachments_features);
                if (has_portability_subset)
                    prepend_feature(subset_features);
                if (has_extended_dynamic_state2)
                    prepend_feature(extended_dynamic_state2_features);
                if (has_vertex_input_dynamic_state)
                    prepend_feature(vertex_input_dynamic_state_features);
                if (has_legacy_vertex_attributes)
                    prepend_feature(legacy_vertex_attributes_features);
                VkPhysicalDeviceFeatures2 features{};
                features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                features.pNext = features_next;
                get_features2(physical, &features);

                VkPhysicalDeviceMemoryProperties memory{};
                get_memory(physical, &memory);
                uint32_t queue_count = 0;
                get_queues(physical, &queue_count, nullptr);
                std::vector<VkQueueFamilyProperties> queues(queue_count);
                get_queues(physical, &queue_count, queues.data());

                output << "{\"extensions\":";
                write_extensions(output, device_extensions);
                output << ",\"dynamic_rendering_unused_attachments\":"
                          "{\"extension\":"
                       << json_bool(has_dynamic_rendering_unused_attachments)
                       << ",\"feature\":"
                       << json_bool(unused_attachments_features
                                        .dynamicRenderingUnusedAttachments)
                       << '}';
                output << ",\"incremental_present\":{\"extension\":"
                       << json_bool(has_incremental_present)
                       << ",\"swapchain_extension\":"
                       << json_bool(has_swapchain) << '}';
                output << ",\"non_seamless_cube_map\":{\"extension\":"
                       << json_bool(has_non_seamless_cube_map)
                       << ",\"feature\":"
                       << json_bool(non_seamless_cube_map_features
                                        .nonSeamlessCubeMap)
                       << '}';
                output << ",\"graphics_pipeline_library\":"
                          "{\"pipeline_library_extension\":"
                       << json_bool(has_pipeline_library)
                       << ",\"graphics_pipeline_library_extension\":"
                       << json_bool(has_graphics_pipeline_library)
                       << ",\"feature\":"
                       << json_bool(graphics_pipeline_library_features
                                        .graphicsPipelineLibrary)
                       << ",\"fast_linking\":"
                       << json_bool(graphics_pipeline_library_properties
                                        .graphicsPipelineLibraryFastLinking)
                       << ",\"independent_interpolation_decoration\":"
                       << json_bool(
                              graphics_pipeline_library_properties
                                  .graphicsPipelineLibraryIndependentInterpolationDecoration)
                       << '}';
                output << ",\"device_address_commands\":{\"extension\":"
                       << json_bool(has_device_address_commands)
                       << ",\"feature\":"
                       << json_bool(device_address_commands_features
                                        .deviceAddressCommands)
                       << ",\"buffer_device_address\":"
                       << json_bool(buffer_device_address_features
                                        .bufferDeviceAddress)
                       << '}';
                output << ",\"extended_dynamic_state2\":{\"extension\":"
                       << json_bool(has_extended_dynamic_state2)
                       << ",\"feature\":"
                       << json_bool(extended_dynamic_state2_features
                                        .extendedDynamicState2)
                       << ",\"logic_op\":"
                       << json_bool(extended_dynamic_state2_features
                                        .extendedDynamicState2LogicOp)
                       << ",\"patch_control_points\":"
                       << json_bool(
                              extended_dynamic_state2_features
                                  .extendedDynamicState2PatchControlPoints)
                       << '}';
                output << ",\"vertex_input_dynamic_state\":{\"extension\":"
                       << json_bool(has_vertex_input_dynamic_state)
                       << ",\"feature\":"
                       << json_bool(vertex_input_dynamic_state_features
                                        .vertexInputDynamicState)
                       << '}';
                output << ",\"legacy_vertex_attributes\":{\"extension\":"
                       << json_bool(has_legacy_vertex_attributes)
                       << ",\"feature\":"
                       << json_bool(legacy_vertex_attributes_features
                                        .legacyVertexAttributes)
                       << ",\"native_unaligned_performance\":"
                       << json_bool(legacy_vertex_attributes_properties
                                        .nativeUnalignedPerformance)
                       << '}';
                output << ",\"supported_core_formats\":[";
                bool first_format = true;
                for (int format = VK_FORMAT_R4G4_UNORM_PACK8;
                     format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK; ++format) {
                    VkFormatProperties format_properties{};
                    get_format(physical, static_cast<VkFormat>(format),
                               &format_properties);
                    if (format_properties.linearTilingFeatures == 0 &&
                        format_properties.optimalTilingFeatures == 0 &&
                        format_properties.bufferFeatures == 0) {
                        continue;
                    }
                    if (!first_format) {
                        output << ',';
                    }
                    first_format = false;
                    output << "{\"format\":" << format
                           << ",\"linear\":"
                           << format_properties.linearTilingFeatures
                           << ",\"optimal\":"
                           << format_properties.optimalTilingFeatures
                           << ",\"buffer\":"
                           << format_properties.bufferFeatures << '}';
                }
                output << "],\"device_create_result\":";

                // The first queue family that can run every verifier this
                // device will get, or else the first that can run its
                // graphics ones.
                const bool uses_graphics =
                    graphics_pipeline_library_features
                        .graphicsPipelineLibrary ||
                    extended_dynamic_state2_features
                        .extendedDynamicState2LogicOp;
                const bool uses_transfer =
                    device_address_commands_features.deviceAddressCommands &&
                    buffer_device_address_features.bufferDeviceAddress;
                const auto find_queue = [&](bool need_transfer)
                    -> std::optional<uint32_t> {
                    for (uint32_t queue_index = 0; queue_index < queue_count;
                         ++queue_index) {
                        const VkQueueFlags flags =
                            queues[queue_index].queueFlags;
                        if (queues[queue_index].queueCount != 0 &&
                            (!uses_graphics ||
                             (flags & VK_QUEUE_GRAPHICS_BIT) != 0) &&
                            (!need_transfer ||
                             (flags & VK_QUEUE_TRANSFER_BIT) != 0)) {
                            return queue_index;
                        }
                    }
                    return std::nullopt;
                };
                std::optional<uint32_t> selected_queue =
                    find_queue(uses_transfer);
                if (!selected_queue) {
                    selected_queue = find_queue(false);
                }

                const bool use_graphics_pipeline_library =
                    has_pipeline_library && has_graphics_pipeline_library &&
                    graphics_pipeline_library_features
                        .graphicsPipelineLibrary;
                int32_t device_result = VK_ERROR_FEATURE_NOT_PRESENT;
                VkDevice device = VK_NULL_HANDLE;
                if (selected_queue) {
                    constexpr float priority = 1.0F;
                    const VkDeviceQueueCreateInfo queue_info{
                        VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                        nullptr,
                        0,
                        *selected_queue,
                        1,
                        &priority,
                    };
                    const std::array<std::pair<bool, const char *>, 11>
                        candidate_extensions{{
                            {has_portability_subset,
                             VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME},
                            {has_dynamic_rendering_unused_attachments,
                             VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME},
                            {has_swapchain, VK_KHR_SWAPCHAIN_EXTENSION_NAME},
                            {has_incremental_present,
                             VK_KHR_INCREMENTAL_PRESENT_EXTENSION_NAME},
                            {has_non_seamless_cube_map,
                             VK_EXT_NON_SEAMLESS_CUBE_MAP_EXTENSION_NAME},
                            {use_graphics_pipeline_library,
                             VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME},
                            {use_graphics_pipeline_library,
                             VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME},
                            {has_device_address_commands,
                             VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME},
                            {has_extended_dynamic_state2,
                             VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME},
                            {has_vertex_input_dynamic_state,
                             VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME},
                            {has_legacy_vertex_attributes,
                             VK_EXT_LEGACY_VERTEX_ATTRIBUTES_EXTENSION_NAME},
                        }};
                    std::array<const char *, candidate_extensions.size()>
                        device_extensions_to_enable{};
                    uint32_t device_extension_count = 0;
                    for (const auto &[wanted, name] : candidate_extensions) {
                        if (wanted) {
                            device_extensions_to_enable
                                [device_extension_count++] = name;
                        }
                    }
                    VkPhysicalDeviceFeatures enabled_core_features{};
                    enabled_core_features.imageCubeArray =
                        features.features.imageCubeArray;
                    enabled_core_features.logicOp = features.features.logicOp;
                    const VkDeviceCreateInfo device_info{
                        VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                        features.pNext,
                        0,
                        1,
                        &queue_info,
                        0,
                        nullptr,
                        device_extension_count,
                        device_extensions_to_enable.data(),
                        &enabled_core_features,
                    };
                    device_result =
                        create_device(physical, &device_info, nullptr, &device);
                }

                if (device_result == VK_SUCCESS) {
                    const uint32_t queue_family = *selected_queue;
                    const ProbeDevice probe{
                        device,
                        load_function<PFN_vkGetDeviceProcAddr>(
                            instance, "vkGetDeviceProcAddr")};
                    DeviceAddressCommandsReport address_commands_report;
                    const bool verify_address_commands =
                        has_device_address_commands && uses_transfer &&
                        (queues[queue_family].queueFlags &
                         VK_QUEUE_TRANSFER_BIT) != 0;
                    if (verify_address_commands) {
                        address_commands_report =
                            probe.verify_device_address_commands(
                                properties.properties.apiVersion,
                                queue_family, memory, device_extensions);
                    }
                    DynamicLogicOpReport dynamic_logic_op_report;
                    const bool verify_dynamic_logic_op =
                        has_extended_dynamic_state2 &&
                        extended_dynamic_state2_features
                            .extendedDynamicState2LogicOp &&
                        features.features.logicOp &&
                        (queues[queue_family].queueFlags &
                         VK_QUEUE_GRAPHICS_BIT) != 0;
                    if (verify_dynamic_logic_op) {
                        dynamic_logic_op_report =
                            probe.verify_dynamic_logic_ops(queue_family,
                                                           memory);
                    }
                    LegacyVertexAttributesReport
                        legacy_vertex_attributes_report;
                    const bool run_legacy_vertex_attributes =
                        has_vertex_input_dynamic_state &&
                        vertex_input_dynamic_state_features
                            .vertexInputDynamicState &&
                        has_legacy_vertex_attributes &&
                        legacy_vertex_attributes_features
                            .legacyVertexAttributes &&
                        (queues[queue_family].queueFlags &
                         VK_QUEUE_GRAPHICS_BIT) != 0;
                    if (run_legacy_vertex_attributes) {
                        legacy_vertex_attributes_report =
                            probe.verify_legacy_vertex_attributes(
                                queue_family, memory);
                    }
                    CubeAliasReport alias_report;
                    GraphicsPipelineLibraryReport
                        graphics_pipeline_library_report;
                    if (has_non_seamless_cube_map &&
                        non_seamless_cube_map_features.nonSeamlessCubeMap) {
                        alias_report = probe.verify_cube_alias_views(
                            physical, memory, get_image_format_properties);
                    }
                    if (use_graphics_pipeline_library) {
                        graphics_pipeline_library_report =
                            probe.verify_graphics_pipeline_library(
                                queue_family);
                    }
                    output << device_result
                           << ",\"device_address_commands_execution\":";
                    if (verify_address_commands) {
                        write_device_address_commands_report(
                            output, address_commands_report);
                    } else {
                        output << "null";
                    }
                    output << ",\"dynamic_logic_op_execution\":";
                    if (verify_dynamic_logic_op) {
                        write_dynamic_logic_op_report(
                            output, dynamic_logic_op_report);
                    } else {
                        output << "null";
                    }
                    output << ",\"cube_alias_views\":";
                    write_cube_alias_report(output, alias_report);
                    output << ",\"graphics_pipeline_library_probe\":";
                    if (use_graphics_pipeline_library) {
                        write_graphics_pipeline_library_report(
                            output, graphics_pipeline_library_report);
                    } else {
                        output << "null";
                    }
                    output << ",\"legacy_vertex_attributes_probe\":";
                    if (run_legacy_vertex_attributes) {
                        write_legacy_vertex_attributes_report(
                            output, legacy_vertex_attributes_report);
                    } else {
                        output << "null";
                    }
                    probe.destroy_device(device, nullptr);
                } else {
                    output << device_result
                           << ",\"device_address_commands_execution\":null"
                           << ",\"dynamic_logic_op_execution\":null"
                           << ",\"cube_alias_views\":null"
                           << ",\"graphics_pipeline_library_probe\":null"
                           << ",\"legacy_vertex_attributes_probe\":null";
                }
                output << '}';
            }

            output << "]}\n";
            destroy_instance(instance, nullptr);
            return 0;
        } catch (const std::exception &error) {
            std::cerr << "kosmickrisp-capability-probe: " << error.what()
                      << '\n';
            return 1;
        }
    }
}

}  // namespace uml::gpu
