// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan_draw_test.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace uml::gpu::test {
namespace {

using uml::testing::require;

struct Cleanup {
    std::vector<std::function<void()>> actions;
    ~Cleanup()
    {
        for (auto action = actions.rbegin(); action != actions.rend(); ++action)
            (*action)();
    }
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkDeviceAddress address = 0;
};

} // namespace

struct DrawContext::Impl {
    DrawCapabilities capabilities{};
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    uint32_t timestamp_bits = 0;
    float timestamp_period = 0;
    VkPhysicalDeviceMemoryProperties memory{};
    PFN_vkGetDeviceProcAddr get_device_proc_addr = nullptr;
    VkPhysicalDeviceFeatures2 features = structure<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    VkPhysicalDeviceVulkan11Features vulkan11 = structure<VkPhysicalDeviceVulkan11Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
    VkPhysicalDeviceVulkan12Features vulkan12 = structure<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
    VkPhysicalDeviceVulkan13Features vulkan13 = structure<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR address_commands = structure<VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_ADDRESS_COMMANDS_FEATURES_KHR);
    VkPhysicalDeviceMaintenance8FeaturesKHR maintenance8 = structure<VkPhysicalDeviceMaintenance8FeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR);
    VkPhysicalDeviceMaintenance10FeaturesKHR maintenance10 = structure<VkPhysicalDeviceMaintenance10FeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_10_FEATURES_KHR);
    VkPhysicalDeviceLineRasterizationFeaturesKHR lines = structure<VkPhysicalDeviceLineRasterizationFeaturesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_KHR);
    VkPhysicalDeviceProvokingVertexFeaturesEXT provoking = structure<VkPhysicalDeviceProvokingVertexFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT);
    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb = structure<VkPhysicalDeviceTransformFeedbackFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT);
    VkPhysicalDeviceConditionalRenderingFeaturesEXT conditional = structure<VkPhysicalDeviceConditionalRenderingFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT);
    VkPhysicalDeviceLegacyVertexAttributesFeaturesEXT legacy = structure<VkPhysicalDeviceLegacyVertexAttributesFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LEGACY_VERTEX_ATTRIBUTES_FEATURES_EXT);
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vertex_input = structure<VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT);
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dynamic3 = structure<VkPhysicalDeviceExtendedDynamicState3FeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT);
    bool legacy_vertex_input = false;
    VkPhysicalDeviceDepthClipEnableFeaturesEXT depth_clip = structure<VkPhysicalDeviceDepthClipEnableFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT);
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT library = structure<VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT);

    template <typename Function>
    Function function(const char *name) const
    {
        auto result = reinterpret_cast<Function>(get_device_proc_addr(device, name));
        require(result != nullptr, std::string("missing ") + name);
        return result;
    }

    uint32_t memory_type(uint32_t mask, VkMemoryPropertyFlags flags) const
    {
        const auto type = uml::gpu::test::memory_type(memory, mask, flags);
        if (!type)
            throw std::runtime_error("required Vulkan memory type unavailable");
        return *type;
    }
};

#define VK_FUNCTION(name) api.function<PFN_vk##name>("vk" #name)
#define VK_CALL(name, ...) VK_FUNCTION(name)(__VA_ARGS__)
#define VK_CHECK(name, ...) check(VK_CALL(name, __VA_ARGS__), "vk" #name)

DrawContext::DrawContext(bool legacy_vertex_input, bool pipeline_statistics,
                         bool conditional_rendering, DrawExtensions extra_extensions)
    : impl_(std::make_unique<Impl>())
{
    auto &api = *impl_;
    VkApplicationInfo application = structure<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
    application.pApplicationName = "portable Vulkan draw regression";
    application.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info = structure<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
    instance_info.pApplicationInfo = &application;
    check(instance_function<PFN_vkCreateInstance>(nullptr, "vkCreateInstance")(
              &instance_info, nullptr, &api.instance), "vkCreateInstance");
    uint32_t physical_count = 0;
    auto enumerate = instance_function<PFN_vkEnumeratePhysicalDevices>(
        api.instance, "vkEnumeratePhysicalDevices");
    check(enumerate(api.instance, &physical_count, nullptr), "physical device count");
    require(physical_count != 0, "no Vulkan physical device");
    std::vector<VkPhysicalDevice> physicals(physical_count);
    check(enumerate(api.instance, &physical_count, physicals.data()), "physical devices");
    api.physical = physicals.front();
    auto properties = instance_function<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        api.instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    uint32_t family_count = 0;
    properties(api.physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    properties(api.physical, &family_count, families.data());
    for (; api.queue_family < family_count; ++api.queue_family) {
        if (families[api.queue_family].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            break;
    }
    require(api.queue_family < family_count, "no graphics queue");
    api.timestamp_bits = families[api.queue_family].timestampValidBits;
    api.features.pNext = &api.vulkan11;
    api.vulkan11.pNext = &api.vulkan12;
    api.vulkan12.pNext = &api.vulkan13;
    void **next = &api.vulkan13.pNext;
    const auto append = [&](auto &feature) {
        *next = &feature;
        next = &feature.pNext;
    };
    if (extra_extensions.device_address_commands)
        append(api.address_commands);
    if (extra_extensions.maintenance8)
        append(api.maintenance8);
    if (extra_extensions.maintenance10)
        append(api.maintenance10);
    *next = &api.lines;
    api.lines.pNext = &api.provoking;
    api.provoking.pNext = &api.xfb;
    api.xfb.pNext = &api.depth_clip;
    api.depth_clip.pNext = &api.dynamic3;
    api.dynamic3.pNext = &api.library;
    api.library.pNext = conditional_rendering ? static_cast<void *>(&api.conditional)
                                             : legacy_vertex_input ? &api.legacy : nullptr;
    api.conditional.pNext = legacy_vertex_input ? &api.legacy : nullptr;
    api.legacy.pNext = &api.vertex_input;
    instance_function<PFN_vkGetPhysicalDeviceFeatures2>(
        api.instance, "vkGetPhysicalDeviceFeatures2")(api.physical, &api.features);
    require(!extra_extensions.device_address_commands ||
                (api.address_commands.deviceAddressCommands && api.vulkan12.bufferDeviceAddress),
            "device address commands unavailable");
    require(!extra_extensions.maintenance8 || api.maintenance8.maintenance8,
            "maintenance8 unavailable");
    require(!extra_extensions.maintenance10 || api.maintenance10.maintenance10,
            "maintenance10 unavailable");
    if (legacy_vertex_input) {
        require(api.legacy.legacyVertexAttributes && api.vertex_input.vertexInputDynamicState &&
                    api.dynamic3.extendedDynamicState3ProvokingVertexMode &&
                    api.dynamic3.extendedDynamicState3LineStippleEnable,
                "legacy vertex input and dynamic line selection unavailable");
        api.legacy_vertex_input = true;
    }
    VkPhysicalDeviceDriverProperties driver = structure<VkPhysicalDeviceDriverProperties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES);
    VkPhysicalDeviceTransformFeedbackPropertiesEXT xfb_properties = structure<VkPhysicalDeviceTransformFeedbackPropertiesEXT>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT);
    driver.pNext = &xfb_properties;
    auto maintenance10_properties = structure<VkPhysicalDeviceMaintenance10PropertiesKHR>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_10_PROPERTIES_KHR);
    if (extra_extensions.maintenance10)
        xfb_properties.pNext = &maintenance10_properties;
    VkPhysicalDeviceProperties2 identity = structure<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
    identity.pNext = &driver;
    instance_function<PFN_vkGetPhysicalDeviceProperties2>(
        api.instance, "vkGetPhysicalDeviceProperties2")(api.physical, &identity);
    api.timestamp_period = identity.properties.limits.timestampPeriod;
    api.capabilities = {identity.properties.deviceName, driver.driverName, driver.driverID,
                        bool(api.features.features.geometryShader),
                        bool(api.xfb.transformFeedback), bool(api.vulkan11.multiview),
                        bool(api.xfb.geometryStreams),
                        bool(xfb_properties.transformFeedbackStreamsLinesTriangles),
                        bool(xfb_properties.transformFeedbackQueries),
                        bool(api.provoking.transformFeedbackPreservesProvokingVertex),
                        bool(api.features.features.pipelineStatisticsQuery),
                        bool(api.vulkan11.multiviewGeometryShader),
                        maintenance10_properties};
    if (!pipeline_statistics)
        api.features.features.pipelineStatisticsQuery = VK_FALSE;
    require(!conditional_rendering || api.conditional.conditionalRendering,
            "conditional rendering unavailable");
    api.conditional.conditionalRendering = conditional_rendering;
    api.conditional.inheritedConditionalRendering = VK_FALSE;
    require(api.vulkan13.dynamicRendering, "dynamic rendering unavailable");
    instance_function<PFN_vkGetPhysicalDeviceMemoryProperties>(
        api.instance, "vkGetPhysicalDeviceMemoryProperties")(api.physical, &api.memory);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info = structure<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
    queue_info.queueFamilyIndex = api.queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    std::vector<const char *> extensions{VK_KHR_LINE_RASTERIZATION_EXTENSION_NAME,
        VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME,
        VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
        VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
        VK_EXT_SAMPLE_LOCATIONS_EXTENSION_NAME};
    if (conditional_rendering)
        extensions.push_back(VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME);
    if (extra_extensions.device_address_commands)
        extensions.push_back(VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME);
    if (extra_extensions.maintenance8)
        extensions.push_back(VK_KHR_MAINTENANCE_8_EXTENSION_NAME);
    if (extra_extensions.maintenance10)
        extensions.push_back(VK_KHR_MAINTENANCE_10_EXTENSION_NAME);
    if (legacy_vertex_input) {
        extensions.push_back(VK_EXT_LEGACY_VERTEX_ATTRIBUTES_EXTENSION_NAME);
        extensions.push_back(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    }
    VkDeviceCreateInfo device_info = structure<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    device_info.pNext = &api.features;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = std::size(extensions);
    device_info.ppEnabledExtensionNames = extensions.data();
    check(instance_function<PFN_vkCreateDevice>(api.instance, "vkCreateDevice")(
              api.physical, &device_info, nullptr, &api.device), "vkCreateDevice");
    api.get_device_proc_addr = instance_function<PFN_vkGetDeviceProcAddr>(
        api.instance, "vkGetDeviceProcAddr");
    VK_CALL(GetDeviceQueue, api.device, api.queue_family, 0, &api.queue);
}

DrawContext::~DrawContext()
{
    auto &api = *impl_;
    if (api.device)
        VK_CALL(DestroyDevice, api.device, nullptr);
    if (api.instance)
        instance_function<PFN_vkDestroyInstance>(api.instance, "vkDestroyInstance")(
            api.instance, nullptr);
}

const DrawCapabilities &DrawContext::capabilities() const
{
    return impl_->capabilities;
}

void require_venus_when_asked(const DrawContext &context, const std::source_location location)
{
    const char *venus = std::getenv("UML_VULKAN_REQUIRE_VENUS");
    if (!venus || std::string_view(venus) != "1")
        return;
    require(context.capabilities().driver_id == VK_DRIVER_ID_MESA_VENUS,
            "guest probe did not select the Venus driver", location);
    std::puts("UML_VULKAN_PROBE_DEVICE: Venus");
    std::fflush(stdout);
}

SubmitStormResult DrawContext::submit_storm(const SubmitStormConfig &config)
{
    auto &api = *impl_;
    require(config.submissions != 0, "submit storm needs a submission");
    const uint32_t outstanding = config.outstanding ? config.outstanding : 1;

    Cleanup cleanup;
    SubmitStormResult result;
    result.submissions = config.submissions;

    const VkDeviceSize size = VkDeviceSize(config.submissions) * sizeof(uint32_t);
    VkBufferCreateInfo buffer_info = structure<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer slots;
    VK_CHECK(CreateBuffer, api.device, &buffer_info, nullptr, &slots);
    cleanup.actions.emplace_back([&, slots] { VK_CALL(DestroyBuffer, api.device, slots, nullptr); });
    VkMemoryRequirements requirements;
    VK_CALL(GetBufferMemoryRequirements, api.device, slots, &requirements);
    VkMemoryAllocateInfo allocate = structure<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = api.memory_type(requirements.memoryTypeBits,
                                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    VkDeviceMemory memory;
    VK_CHECK(AllocateMemory, api.device, &allocate, nullptr, &memory);
    cleanup.actions.emplace_back([&, memory] { VK_CALL(FreeMemory, api.device, memory, nullptr); });
    VK_CHECK(BindBufferMemory, api.device, slots, memory, 0);
    void *mapping = nullptr;
    VK_CHECK(MapMemory, api.device, memory, 0, VK_WHOLE_SIZE, 0, &mapping);
    std::memset(mapping, 0, size_t(size));

    VkCommandPoolCreateInfo pool_info = structure<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    pool_info.queueFamilyIndex = api.queue_family;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool;
    VK_CHECK(CreateCommandPool, api.device, &pool_info, nullptr, &pool);
    cleanup.actions.emplace_back([&, pool] { VK_CALL(DestroyCommandPool, api.device, pool, nullptr); });

    /* One command buffer and one fence per outstanding slot, each recycled
     * once its own submission has completed: the driver sees a steady stream
     * of one-time recordings, which is where per-submission command storage
     * is created and destroyed. */
    std::vector<VkCommandBuffer> commands(outstanding);
    VkCommandBufferAllocateInfo command_info = structure<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    command_info.commandPool = pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = outstanding;
    VK_CHECK(AllocateCommandBuffers, api.device, &command_info, commands.data());
    std::vector<VkFence> fences(outstanding);
    std::vector<bool> pending(outstanding, false);
    for (uint32_t i = 0; i < outstanding; ++i) {
        VkFenceCreateInfo fence_info = structure<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        VK_CHECK(CreateFence, api.device, &fence_info, nullptr, &fences[i]);
    }
    if (!config.abandon) {
        // The cleanup runs after `fences`, declared later, is destroyed.
        cleanup.actions.emplace_back([&, fences] {
            for (VkFence fence : fences)
                VK_CALL(DestroyFence, api.device, fence, nullptr);
        });
    }

    const auto started = std::chrono::steady_clock::now();
    for (uint32_t index = 0; index < config.submissions; ++index) {
        const uint32_t slot = index % outstanding;
        if (pending[slot]) {
            VK_CHECK(WaitForFences, api.device, 1, &fences[slot], VK_TRUE, 30'000'000'000ull);
            VK_CHECK(ResetFences, api.device, 1, &fences[slot]);
            pending[slot] = false;
            if (config.completed)
                config.completed(index - outstanding);
        }
        VkCommandBufferBeginInfo begin = structure<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(BeginCommandBuffer, commands[slot], &begin);
        /* A value no other submission writes, at an offset no other one
         * touches, so a recording that lands in the wrong storage shows up as
         * a wrong or missing word rather than as a race. */
        VK_CALL(CmdFillBuffer, commands[slot], slots, VkDeviceSize(index) * sizeof(uint32_t),
                sizeof(uint32_t), index + 1u);
        VkMemoryBarrier host = structure<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        VK_CALL(CmdPipelineBarrier, commands[slot], VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
        VK_CHECK(EndCommandBuffer, commands[slot]);
        VkSubmitInfo submit = structure<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands[slot];
        VK_CHECK(QueueSubmit, api.queue, 1, &submit, fences[slot]);
        pending[slot] = true;
    }

    if (config.abandon) {
        /* Deliberately leave every submission in flight: the caller destroys
         * the context next, and the driver's completion handling must survive
         * the device going away underneath it. */
        result.wall_nanoseconds = double(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started).count());
        cleanup.actions.clear();
        return result;
    }

    for (uint32_t slot = 0; slot < outstanding; ++slot) {
        if (!pending[slot])
            continue;
        VK_CHECK(WaitForFences, api.device, 1, &fences[slot], VK_TRUE, 30'000'000'000ull);
        if (config.completed) {
            // The slot's last submission: the largest index below `submissions`
            // that is congruent to the slot modulo `outstanding`.
            const uint32_t last = slot + (config.submissions - 1 - slot) / outstanding * outstanding;
            config.completed(last);
        }
    }
    result.wall_nanoseconds = double(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count());

    VkMappedMemoryRange range = structure<VkMappedMemoryRange>(VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);
    range.memory = memory;
    range.size = VK_WHOLE_SIZE;
    VK_CHECK(InvalidateMappedMemoryRanges, api.device, 1, &range);
    result.values.resize(config.submissions);
    std::memcpy(result.values.data(), mapping, size_t(size));
    return result;
}

DrawResult DrawContext::draw(const DrawConfig &config,
                              std::span<const uint8_t> initial_cache)
{
    auto &api = *impl_;
    const bool dynamic_input = !config.vertex_input_draws.empty();
    require(!dynamic_input || (api.legacy_vertex_input && !config.vertex_data.empty() &&
                               config.height >= config.vertex_input_draws.size() &&
                               config.height % config.vertex_input_draws.size() == 0),
            "dynamic vertex-input sequence requires enabled features, bytes and equal bands");
    const bool has_depth_stencil = config.stencil || config.depth;
    const VkFormat depth_format = config.stencil ? VK_FORMAT_D32_SFLOAT_S8_UINT
                                                 : config.depth_format;
    const bool depth_unorm = depth_format == VK_FORMAT_D16_UNORM;
    const bool depth_has_stencil = depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    const VkImageAspectFlags depth_aspects = VK_IMAGE_ASPECT_DEPTH_BIT |
        (depth_has_stencil ? VkImageAspectFlags(VK_IMAGE_ASPECT_STENCIL_BIT) : 0u);
    require(!config.pipeline_library || api.library.graphicsPipelineLibrary,
            "graphics pipeline libraries unavailable");
    require(!config.depth_clamp || api.features.features.depthClamp,
            "depth clamping unavailable");
    require(!config.view_mask || api.vulkan11.multiview, "multiview unavailable");
    require(config.geometry_shader.empty() || api.features.features.geometryShader,
            "geometry shader unavailable");
    require(config.geometry_shader.empty() || !config.view_mask ||
                api.vulkan11.multiviewGeometryShader,
            "geometry shaders with multiview unavailable");
    require(config.counters.empty() || api.features.features.vertexPipelineStoresAndAtomics,
            "vertex pipeline atomics unavailable");
    require(config.provoking_vertex != VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT ||
                api.provoking.provokingVertexLast, "last provoking vertex unavailable");
    require(config.line_mode != VK_LINE_RASTERIZATION_MODE_RECTANGULAR_KHR ||
                api.lines.rectangularLines, "rectangular lines unavailable");
    require(config.line_mode != VK_LINE_RASTERIZATION_MODE_BRESENHAM_KHR ||
                api.lines.bresenhamLines, "Bresenham lines unavailable");
    require(config.line_mode != VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_KHR ||
                api.lines.smoothLines, "smooth lines unavailable");
    const bool stipple_available =
        config.line_mode == VK_LINE_RASTERIZATION_MODE_BRESENHAM_KHR
            ? api.lines.stippledBresenhamLines
        : config.line_mode == VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_KHR
            ? api.lines.stippledSmoothLines : api.lines.stippledRectangularLines;
    require(!config.stipple || stipple_available, "requested stippled line mode unavailable");
    const bool has_feedback = config.transform_feedback_words ||
                              !config.transform_feedback_bindings.empty();
    require(config.transform_feedback_bindings.empty() ||
                (!config.transform_feedback_words && !config.device_address_commands &&
                 !config.secondary_replays && config.transform_feedback_bindings.size() <= 4),
            "explicit XFB bindings require ordinary primary-command capture");
    require(!has_feedback || api.xfb.transformFeedback,
            "transform feedback unavailable");
    require(!config.transform_feedback_query ||
                (api.capabilities.transform_feedback_queries &&
                 has_feedback && !config.secondary_replays &&
                 !config.device_address_commands),
            "XFB query/counter requires ordinary primary-command capture");
    require(!config.pipeline_statistics || api.features.features.pipelineStatisticsQuery,
            "pipeline statistics unavailable");
    require(!config.indirect_count || (config.indirect && api.vulkan12.drawIndirectCount &&
                                       config.max_indirect_count <= 1),
            "indirect count requires its feature and at most one fixture command");
    require(!config.conditional_value || api.conditional.conditionalRendering,
            "conditional rendering not enabled");
    require(!config.secondary_replays || !config.pipeline_statistics ||
                api.features.features.inheritedQueries,
            "inherited statistics queries unavailable");
    require(!config.secondary_query_transitions ||
                (config.secondary_replays && config.pipeline_statistics),
            "query transitions require a queried secondary replay");
    require(!config.device_address_commands || api.address_commands.deviceAddressCommands,
            "device address commands not enabled");
    require(!config.end_rendering2 || api.maintenance10.maintenance10,
            "maintenance10 not enabled");
    require(!config.query_after_rendering || config.pipeline_statistics,
            "post-render query requires pipeline statistics");
    require(config.depth_clip || api.depth_clip.depthClipEnable,
            "depth clipping control unavailable");
    require(!config.dynamic_raster ||
                (api.dynamic3.extendedDynamicState3PolygonMode &&
                 api.dynamic3.extendedDynamicState3LineRasterizationMode &&
                 api.dynamic3.extendedDynamicState3ProvokingVertexMode &&
                 api.dynamic3.extendedDynamicState3DepthClipEnable &&
                 api.dynamic3.extendedDynamicState3LineStippleEnable),
            "complete requested dynamic raster features unavailable");
    require(config.dynamic_raster || config.raster_warmup.empty(),
            "raster warmup draws require dynamic raster states");
    require(!config.fail_on_compile_required || api.vulkan13.pipelineCreationCacheControl,
            "pipeline compile-required control unavailable");
    require(!config.timestamp || api.timestamp_bits, "graphics timestamps unavailable");
    require(config.repeat_count != 0, "draw repeat count must be nonzero");
    require(!config.query_restoration || (config.pipeline_statistics && !config.secondary_replays && !dynamic_input),
            "query restoration requires the direct raster fixture");
    Cleanup cleanup;
    const auto allocation = [&](VkMemoryRequirements requirements,
                                VkMemoryPropertyFlags flags, bool address = false) {
        VkMemoryAllocateInfo info = structure<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        auto address_info = structure<VkMemoryAllocateFlagsInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO);
        address_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        if (address)
            info.pNext = &address_info;
        info.allocationSize = requirements.size;
        info.memoryTypeIndex = api.memory_type(requirements.memoryTypeBits, flags);
        VkDeviceMemory result;
        VK_CHECK(AllocateMemory, api.device, &info, nullptr, &result);
        cleanup.actions.emplace_back([&, result] { VK_CALL(FreeMemory, api.device, result, nullptr); });
        return result;
    };
    const auto buffer = [&](VkDeviceSize size, VkBufferUsageFlags usage,
                            const void *initial = nullptr) {
        Buffer result{};
        result.size = size;
        VkBufferCreateInfo info = structure<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        info.size = size;
        info.usage = usage | (config.device_address_commands
            ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
        VK_CHECK(CreateBuffer, api.device, &info, nullptr, &result.buffer);
        VkMemoryRequirements requirements;
        VK_CALL(GetBufferMemoryRequirements, api.device, result.buffer, &requirements);
        result.memory = allocation(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                    config.device_address_commands);
        cleanup.actions.emplace_back([&, handle = result.buffer] {
            VK_CALL(DestroyBuffer, api.device, handle, nullptr);
        });
        VK_CHECK(BindBufferMemory, api.device, result.buffer, result.memory, 0);
        if (config.device_address_commands) {
            auto address_info = structure<VkBufferDeviceAddressInfo>(VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO);
            address_info.buffer = result.buffer;
            result.address = VK_CALL(GetBufferDeviceAddress, api.device, &address_info);
            require(result.address != 0, "buffer has no device address");
        }
        void *mapping;
        VK_CHECK(MapMemory, api.device, result.memory, 0, VK_WHOLE_SIZE, 0, &mapping);
        if (initial)
            std::memcpy(mapping, initial, size);
        else
            std::memset(mapping, 0, size);
        VkMappedMemoryRange range = structure<VkMappedMemoryRange>(VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);
        range.memory = result.memory;
        range.size = VK_WHOLE_SIZE;
        VK_CHECK(FlushMappedMemoryRanges, api.device, 1, &range);
        VK_CALL(UnmapMemory, api.device, result.memory);
        return result;
    };
    const auto read = [&](const Buffer &source, void *destination) {
        void *mapping;
        VK_CHECK(MapMemory, api.device, source.memory, 0, VK_WHOLE_SIZE, 0, &mapping);
        VkMappedMemoryRange range = structure<VkMappedMemoryRange>(VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);
        range.memory = source.memory;
        range.size = VK_WHOLE_SIZE;
        VK_CHECK(InvalidateMappedMemoryRanges, api.device, 1, &range);
        std::memcpy(destination, mapping, source.size);
        VK_CALL(UnmapMemory, api.device, source.memory);
    };
    const uint32_t render_query_count = std::max(1, std::popcount(config.view_mask));
    const uint32_t query_count = render_query_count + config.query_after_rendering;
    const uint32_t query_words = std::popcount(config.pipeline_statistics) + 1;
    VkQueryPool queries = VK_NULL_HANDLE;
    Buffer query_copy{};
    if (config.pipeline_statistics) {
        VkQueryPoolCreateInfo info = structure<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
        info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        info.queryCount = query_count;
        info.pipelineStatistics = config.pipeline_statistics;
        VK_CHECK(CreateQueryPool, api.device, &info, nullptr, &queries);
        cleanup.actions.emplace_back([&, queries] {
            VK_CALL(DestroyQueryPool, api.device, queries, nullptr);
        });
        query_copy = buffer(query_count * query_words * sizeof(uint64_t),
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }
    const uint32_t zero = 0;
    const Buffer counters = buffer(config.counters.empty() ? 4 : config.counters.size_bytes(),
                                   VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                   config.counters.empty() ? &zero : config.counters.data());
    const Buffer pixels = buffer(VkDeviceSize(config.width) * config.height * config.layers * 4,
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Buffer stencil_pixels{};
    if (config.stencil)
        stencil_pixels = buffer(VkDeviceSize(config.width) * config.height * config.layers,
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Buffer depth_pixels{};
    if (config.depth)
        depth_pixels = buffer(VkDeviceSize(config.width) * config.height * config.layers * (depth_unorm ? 2 : 4),
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    std::vector<Buffer> feedback_buffers;
    std::vector<VkBuffer> feedback_handles;
    std::vector<VkDeviceSize> feedback_offsets, feedback_sizes;
    if (config.transform_feedback_words) {
        feedback_buffers.push_back(buffer(VkDeviceSize(config.transform_feedback_words) * 4,
                                           VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT));
        feedback_offsets.push_back(0);
        feedback_sizes.push_back(feedback_buffers.back().size);
    }
    for (const auto &binding : config.transform_feedback_bindings) {
        require(binding.offset_words < binding.words && binding.size_words &&
                    binding.size_words <= binding.words - binding.offset_words,
                "invalid guarded XFB buffer range");
        const std::vector<uint32_t> initial(binding.words, binding.initial_word);
        feedback_buffers.push_back(buffer(VkDeviceSize(binding.words) * 4,
                                           VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT,
                                           initial.data()));
        feedback_offsets.push_back(VkDeviceSize(binding.offset_words) * 4);
        feedback_sizes.push_back(VkDeviceSize(binding.size_words) * 4);
    }
    for (const auto &entry : feedback_buffers)
        feedback_handles.push_back(entry.buffer);
    const Buffer feedback = feedback_buffers.empty() ? Buffer{} : feedback_buffers.front();
    Buffer feedback_counter{};
    if (config.transform_feedback_query)
        feedback_counter = buffer(feedback_buffers.size() * sizeof(uint32_t),
                                   VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT);
    const std::vector<VkBuffer> feedback_counter_handles(feedback_buffers.size(), feedback_counter.buffer);
    std::vector<VkDeviceSize> feedback_counter_offsets;
    for (unsigned i = 0; i < feedback_buffers.size(); ++i)
        feedback_counter_offsets.push_back(i * sizeof(uint32_t));
    Buffer indices{};
    if (!config.indices.empty())
        indices = buffer(config.indices.size_bytes(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                          config.indices.data());
    Buffer vertices{};
    if (!config.vertex_data.empty())
        vertices = buffer(config.vertex_data.size_bytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                           config.vertex_data.data());
    Buffer indirect{};
    if (config.indirect) {
        if (config.indices.empty()) {
            const VkDrawIndirectCommand command{config.vertex_count, config.instance_count,
                                                 config.first_vertex, config.first_instance};
            indirect = buffer(sizeof(command), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, &command);
        } else {
            const VkDrawIndexedIndirectCommand command{
                config.vertex_count, config.instance_count, 0,
                static_cast<int32_t>(config.first_vertex), config.first_instance};
            indirect = buffer(sizeof(command), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, &command);
        }
    }
    Buffer indirect_count{};
    if (config.indirect_count)
        indirect_count = buffer(sizeof(uint32_t), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                                 &*config.indirect_count);
    Buffer predicate{};
    if (config.conditional_value)
        predicate = buffer(sizeof(uint32_t), VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT,
                            &*config.conditional_value);
    VkImageCreateInfo image_info = structure<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {config.width, config.height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = config.layers;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage image;
    VK_CHECK(CreateImage, api.device, &image_info, nullptr, &image);
    VkMemoryRequirements requirements;
    VK_CALL(GetImageMemoryRequirements, api.device, image, &requirements);
    VkDeviceMemory image_memory = allocation(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    cleanup.actions.emplace_back([&, image] { VK_CALL(DestroyImage, api.device, image, nullptr); });
    VK_CHECK(BindImageMemory, api.device, image, image_memory, 0);
    VkImageViewCreateInfo view_info = structure<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view_info.format = image_info.format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, config.layers};
    VkImageView view;
    VK_CHECK(CreateImageView, api.device, &view_info, nullptr, &view);
    cleanup.actions.emplace_back([&, view] { VK_CALL(DestroyImageView, api.device, view, nullptr); });

    VkImage multisample_image = VK_NULL_HANDLE;
    VkImageView multisample_view = VK_NULL_HANDLE;
    if (config.samples != VK_SAMPLE_COUNT_1_BIT) {
        require(!has_depth_stencil, "multisample depth/stencil readback requires a separate resolve fixture");
        VkImageCreateInfo ms_info = image_info;
        ms_info.samples = config.samples;
        ms_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        VK_CHECK(CreateImage, api.device, &ms_info, nullptr, &multisample_image);
        VK_CALL(GetImageMemoryRequirements, api.device, multisample_image, &requirements);
        VkDeviceMemory memory = allocation(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        cleanup.actions.emplace_back([&, multisample_image] {
            VK_CALL(DestroyImage, api.device, multisample_image, nullptr);
        });
        VK_CHECK(BindImageMemory, api.device, multisample_image, memory, 0);
        VkImageViewCreateInfo ms_view_info = view_info;
        ms_view_info.image = multisample_image;
        VK_CHECK(CreateImageView, api.device, &ms_view_info, nullptr, &multisample_view);
        cleanup.actions.emplace_back([&, multisample_view] {
            VK_CALL(DestroyImageView, api.device, multisample_view, nullptr);
        });
    }

    VkImage stencil_image = VK_NULL_HANDLE;
    VkImageView stencil_view = VK_NULL_HANDLE;
    if (has_depth_stencil) {
        VkImageCreateInfo stencil_info = image_info;
        stencil_info.format = depth_format;
        stencil_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VK_CHECK(CreateImage, api.device, &stencil_info, nullptr, &stencil_image);
        VK_CALL(GetImageMemoryRequirements, api.device, stencil_image, &requirements);
        VkDeviceMemory memory = allocation(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        cleanup.actions.emplace_back([&, stencil_image] {
            VK_CALL(DestroyImage, api.device, stencil_image, nullptr);
        });
        VK_CHECK(BindImageMemory, api.device, stencil_image, memory, 0);
        VkImageViewCreateInfo stencil_view_info = view_info;
        stencil_view_info.image = stencil_image;
        stencil_view_info.format = stencil_info.format;
        stencil_view_info.subresourceRange.aspectMask = depth_aspects;
        VK_CHECK(CreateImageView, api.device, &stencil_view_info, nullptr, &stencil_view);
        cleanup.actions.emplace_back([&, stencil_view] {
            VK_CALL(DestroyImageView, api.device, stencil_view, nullptr);
        });
    }

    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_ALL_GRAPHICS, nullptr};
    VkDescriptorSetLayoutCreateInfo set_info = structure<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    set_info.bindingCount = 1;
    set_info.pBindings = &binding;
    VkDescriptorSetLayout set_layout;
    VK_CHECK(CreateDescriptorSetLayout, api.device, &set_info, nullptr, &set_layout);
    cleanup.actions.emplace_back([&, set_layout] {
        VK_CALL(DestroyDescriptorSetLayout, api.device, set_layout, nullptr);
    });
    VkPipelineLayoutCreateInfo layout_info = structure<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout;
    VkPipelineLayout layout;
    VK_CHECK(CreatePipelineLayout, api.device, &layout_info, nullptr, &layout);
    cleanup.actions.emplace_back([&, layout] { VK_CALL(DestroyPipelineLayout, api.device, layout, nullptr); });
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pool_info = structure<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    VkDescriptorPool descriptor_pool;
    VK_CHECK(CreateDescriptorPool, api.device, &pool_info, nullptr, &descriptor_pool);
    cleanup.actions.emplace_back([&, descriptor_pool] {
        VK_CALL(DestroyDescriptorPool, api.device, descriptor_pool, nullptr);
    });
    VkDescriptorSetAllocateInfo set_allocate = structure<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    set_allocate.descriptorPool = descriptor_pool;
    set_allocate.descriptorSetCount = 1;
    set_allocate.pSetLayouts = &set_layout;
    VkDescriptorSet set;
    VK_CHECK(AllocateDescriptorSets, api.device, &set_allocate, &set);
    VkDescriptorBufferInfo counter_info{counters.buffer, 0, counters.size};
    VkWriteDescriptorSet write = structure<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    write.dstSet = set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &counter_info;
    VK_CALL(UpdateDescriptorSets, api.device, 1, &write, 0, nullptr);

    std::vector<VkPipelineShaderStageCreateInfo> stages;
    const auto shader = [&](std::span<const uint32_t> words, VkShaderStageFlagBits stage) {
        if (words.empty())
            return;
        VkShaderModuleCreateInfo info = structure<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        info.codeSize = words.size_bytes();
        info.pCode = words.data();
        VkShaderModule module;
        VK_CHECK(CreateShaderModule, api.device, &info, nullptr, &module);
        cleanup.actions.emplace_back([&, module] { VK_CALL(DestroyShaderModule, api.device, module, nullptr); });
        VkPipelineShaderStageCreateInfo entry = structure<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        entry.stage = stage;
        entry.module = module;
        entry.pName = "main";
        stages.push_back(entry);
    };
    shader(config.vertex_shader, VK_SHADER_STAGE_VERTEX_BIT);
    shader(config.tess_control_shader, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
    shader(config.tess_eval_shader, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
    shader(config.geometry_shader, VK_SHADER_STAGE_GEOMETRY_BIT);
    shader(config.fragment_shader, VK_SHADER_STAGE_FRAGMENT_BIT);
    VkPipelineVertexInputStateCreateInfo vertex = structure<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
    VkPipelineInputAssemblyStateCreateInfo assembly = structure<VkPipelineInputAssemblyStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
    assembly.topology = config.topology;
    assembly.primitiveRestartEnable = config.primitive_restart;
    VkViewport viewport{0, 0, float(config.width), float(config.height), config.min_depth, config.max_depth};
    if (config.negative_viewport) {
        viewport.y = float(config.height);
        viewport.height = -float(config.height);
    }
    VkRect2D scissor{{0, 0}, {config.width, config.height}};
    VkPipelineViewportStateCreateInfo viewport_info = structure<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
    viewport_info.viewportCount = 1;
    viewport_info.pViewports = &viewport;
    viewport_info.scissorCount = 1;
    viewport_info.pScissors = &scissor;
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provoking = structure<VkPipelineRasterizationProvokingVertexStateCreateInfoEXT>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT);
    const RasterState live_raster{config.polygon_mode, config.line_mode,
                                 config.provoking_vertex, config.stipple, config.depth_clip};
    const RasterState create_raster = config.dynamic_raster ? config.ignored_raster_create : live_raster;
    VkPipelineRasterizationDepthClipStateCreateInfoEXT depth_clip = structure<VkPipelineRasterizationDepthClipStateCreateInfoEXT>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT);
    depth_clip.depthClipEnable = create_raster.depth_clip;
    provoking.pNext = &depth_clip;
    provoking.provokingVertexMode = create_raster.provoking_vertex;
    VkPipelineRasterizationLineStateCreateInfoKHR line = structure<VkPipelineRasterizationLineStateCreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_KHR);
    line.pNext = &provoking;
    line.lineRasterizationMode = create_raster.line_mode;
    line.stippledLineEnable = create_raster.stipple;
    line.lineStippleFactor = config.stipple_factor;
    line.lineStipplePattern = config.stipple_pattern;
    VkPipelineRasterizationStateCreateInfo raster = structure<VkPipelineRasterizationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
    raster.pNext = &line;
    raster.polygonMode = create_raster.polygon_mode;
    raster.cullMode = config.cull_mode;
    raster.frontFace = config.front_face;
    raster.lineWidth = 1.0f;
    raster.rasterizerDiscardEnable = config.rasterizer_discard;
    raster.depthClampEnable = config.depth_clamp;
    raster.depthBiasEnable = config.depth_bias;
    raster.depthBiasConstantFactor = config.depth_bias_constant;
    raster.depthBiasSlopeFactor = config.depth_bias_slope;
    raster.depthBiasClamp = config.depth_bias_clamp;
    VkPipelineMultisampleStateCreateInfo multisample = structure<VkPipelineMultisampleStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
    multisample.rasterizationSamples = config.samples;
    VkPipelineSampleLocationsStateCreateInfoEXT locations = structure<VkPipelineSampleLocationsStateCreateInfoEXT>(VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT);
    if (!config.sample_locations.empty()) {
        require(config.sample_locations.size() == uint32_t(config.samples),
                "sample-location count must match rasterization samples");
        locations.sampleLocationsEnable = VK_TRUE;
        locations.sampleLocationsInfo = structure<VkSampleLocationsInfoEXT>(VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT);
        locations.sampleLocationsInfo.sampleLocationsPerPixel = config.samples;
        locations.sampleLocationsInfo.sampleLocationGridSize = {1, 1};
        locations.sampleLocationsInfo.sampleLocationsCount = config.sample_locations.size();
        locations.sampleLocationsInfo.pSampleLocations = config.sample_locations.data();
        multisample.pNext = &locations;
    }
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo blend = structure<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    VkPipelineRenderingCreateInfo rendering = structure<VkPipelineRenderingCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
    rendering.viewMask = config.view_mask;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &image_info.format;
    VkPipelineDepthStencilStateCreateInfo depth_stencil = structure<VkPipelineDepthStencilStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
    if (has_depth_stencil) {
        rendering.depthAttachmentFormat = depth_format;
        rendering.stencilAttachmentFormat = depth_has_stencil ? depth_format : VK_FORMAT_UNDEFINED;
        depth_stencil.stencilTestEnable = config.stencil;
        depth_stencil.depthTestEnable = config.depth;
        depth_stencil.depthWriteEnable = config.depth;
        depth_stencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        depth_stencil.front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE,
                              VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 255, 255, 1};
        depth_stencil.back = depth_stencil.front;
        depth_stencil.back.reference = 2;
    }
    VkGraphicsPipelineCreateInfo pipeline_info = structure<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
    if (config.fail_on_compile_required)
        pipeline_info.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    pipeline_info.pNext = &rendering;
    pipeline_info.stageCount = stages.size();
    pipeline_info.pStages = stages.data();
    pipeline_info.pVertexInputState = &vertex;
    pipeline_info.pInputAssemblyState = &assembly;
    VkPipelineTessellationStateCreateInfo tessellation = structure<VkPipelineTessellationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO);
    tessellation.patchControlPoints = 3;
    auto tessellation_origin = structure<VkPipelineTessellationDomainOriginStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO);
    tessellation_origin.domainOrigin = config.tessellation_origin;
    tessellation.pNext = &tessellation_origin;
    if (!config.tess_eval_shader.empty())
        pipeline_info.pTessellationState = &tessellation;
    pipeline_info.pViewportState = &viewport_info;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend;
    if (has_depth_stencil)
        pipeline_info.pDepthStencilState = &depth_stencil;
    std::vector<VkDynamicState> dynamic_states;
    if (dynamic_input)
        dynamic_states = {VK_DYNAMIC_STATE_VERTEX_INPUT_EXT, VK_DYNAMIC_STATE_PROVOKING_VERTEX_MODE_EXT,
            VK_DYNAMIC_STATE_LINE_STIPPLE_ENABLE_EXT, VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    if (config.dynamic_raster) {
        const VkDynamicState raster_states[] = {VK_DYNAMIC_STATE_POLYGON_MODE_EXT,
            VK_DYNAMIC_STATE_LINE_RASTERIZATION_MODE_EXT, VK_DYNAMIC_STATE_PROVOKING_VERTEX_MODE_EXT,
            VK_DYNAMIC_STATE_DEPTH_CLIP_ENABLE_EXT, VK_DYNAMIC_STATE_LINE_STIPPLE_ENABLE_EXT};
        for (auto state : raster_states)
            if (std::find(dynamic_states.begin(), dynamic_states.end(), state) == dynamic_states.end())
                dynamic_states.push_back(state);
    }
    VkPipelineDynamicStateCreateInfo dynamic = structure<VkPipelineDynamicStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
    if (!dynamic_states.empty()) {
        dynamic.dynamicStateCount = dynamic_states.size();
        dynamic.pDynamicStates = dynamic_states.data();
        pipeline_info.pDynamicState = &dynamic;
    }
    pipeline_info.layout = layout;
    VkPipelineCacheCreateInfo cache_info = structure<VkPipelineCacheCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO);
    cache_info.initialDataSize = initial_cache.size();
    cache_info.pInitialData = initial_cache.data();
    VkPipelineCache cache;
    VK_CHECK(CreatePipelineCache, api.device, &cache_info, nullptr, &cache);
    cleanup.actions.emplace_back([&, cache] { VK_CALL(DestroyPipelineCache, api.device, cache, nullptr); });
    VkPipeline pipeline;
    if (config.pipeline_library) {
        std::array<VkPipeline, 2> libraries;
        for (unsigned subset = 0; subset < libraries.size(); ++subset) {
            VkGraphicsPipelineLibraryCreateInfoEXT parts = structure<VkGraphicsPipelineLibraryCreateInfoEXT>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT);
            parts.pNext = &rendering;
            parts.flags = subset == 0
                ? VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT |
                  VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT
                : VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT |
                  VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
            auto info = pipeline_info;
            info.pNext = &parts;
            info.flags |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
            info.stageCount = subset == 0 ? stages.size() - 1 : 1;
            info.pStages = subset == 0 ? stages.data() : &stages.back();
            VK_CHECK(CreateGraphicsPipelines, api.device, cache, 1, &info, nullptr, &libraries[subset]);
            cleanup.actions.emplace_back([&, library = libraries[subset]] {
                VK_CALL(DestroyPipeline, api.device, library, nullptr);
            });
        }
        VkPipelineLibraryCreateInfoKHR link = structure<VkPipelineLibraryCreateInfoKHR>(VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR);
        link.libraryCount = libraries.size();
        link.pLibraries = libraries.data();
        VkGraphicsPipelineCreateInfo final = structure<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
        final.pNext = &link;
        final.layout = layout;
        // Deferred library NIR is serialized. Final native links have a
        // device-lifetime cache, so a fresh device may compile the final link.
        VK_CHECK(CreateGraphicsPipelines, api.device, cache, 1, &final, nullptr, &pipeline);
        final.flags = VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
        VkPipeline warm;
        VK_CHECK(CreateGraphicsPipelines, api.device, cache, 1, &final, nullptr, &warm);
        VK_CALL(DestroyPipeline, api.device, warm, nullptr);
    } else {
        VK_CHECK(CreateGraphicsPipelines, api.device, cache, 1, &pipeline_info, nullptr, &pipeline);
    }
    cleanup.actions.emplace_back([&, pipeline] { VK_CALL(DestroyPipeline, api.device, pipeline, nullptr); });

    VkCommandPoolCreateInfo command_pool_info = structure<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    const auto query_pool = [&](VkQueryType type, uint32_t count) {
        VkQueryPoolCreateInfo info = structure<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
        info.queryType = type;
        info.queryCount = count;
        VkQueryPool result;
        VK_CHECK(CreateQueryPool, api.device, &info, nullptr, &result);
        cleanup.actions.emplace_back([&, result] { VK_CALL(DestroyQueryPool, api.device, result, nullptr); });
        return result;
    };
    const VkQueryPool timestamp = config.timestamp ? query_pool(VK_QUERY_TYPE_TIMESTAMP, 2)
                                                   : VK_NULL_HANDLE;
    const VkQueryPool feedback_query = config.transform_feedback_query
        ? query_pool(VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT, 1) : VK_NULL_HANDLE;
    Buffer feedback_query_copy{};
    if (feedback_query)
        feedback_query_copy = buffer(3 * sizeof(uint64_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    command_pool_info.queueFamilyIndex = api.queue_family;
    VkCommandPool command_pool;
    VK_CHECK(CreateCommandPool, api.device, &command_pool_info, nullptr, &command_pool);
    cleanup.actions.emplace_back([&, command_pool] { VK_CALL(DestroyCommandPool, api.device, command_pool, nullptr); });
    VkCommandBufferAllocateInfo command_info = structure<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    command_info.commandPool = command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    VkCommandBuffer command;
    VK_CHECK(AllocateCommandBuffers, api.device, &command_info, &command);
    VkCommandBufferBeginInfo begin = structure<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    VK_CHECK(BeginCommandBuffer, command, &begin);
    if (queries)
        VK_CALL(CmdResetQueryPool, command, queries, 0, query_count);
    if (timestamp)
        VK_CALL(CmdResetQueryPool, command, timestamp, 0, 2);
    if (feedback_query)
        VK_CALL(CmdResetQueryPool, command, feedback_query, 0, 1);
    const auto transition = [&](VkImageLayout old_layout, VkImageLayout new_layout,
                                 VkAccessFlags src_access, VkAccessFlags dst_access) {
        VkImageMemoryBarrier barrier = structure<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
        barrier.srcAccessMask = src_access;
        barrier.dstAccessMask = dst_access;
        barrier.oldLayout = old_layout;
        barrier.newLayout = new_layout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = view_info.subresourceRange;
        VK_CALL(CmdPipelineBarrier, command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    };
    transition(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue clear{};
    VK_CALL(CmdClearColorImage, command, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            &clear, 1, &view_info.subresourceRange);
    transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    if (multisample_image) {
        VkImageMemoryBarrier barrier = structure<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = multisample_image;
        barrier.subresourceRange = view_info.subresourceRange;
        VK_CALL(CmdPipelineBarrier, command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    const VkImageSubresourceRange stencil_range{
        depth_aspects, 0, 1, 0, config.layers};
    const auto transition_stencil = [&](VkImageLayout old_layout, VkImageLayout new_layout,
                                         VkAccessFlags src_access, VkAccessFlags dst_access) {
        VkImageMemoryBarrier barrier = structure<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
        barrier.srcAccessMask = src_access;
        barrier.dstAccessMask = dst_access;
        barrier.oldLayout = old_layout;
        barrier.newLayout = new_layout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = stencil_image;
        barrier.subresourceRange = stencil_range;
        VK_CALL(CmdPipelineBarrier, command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    };
    if (has_depth_stencil) {
        transition_stencil(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           0, VK_ACCESS_TRANSFER_WRITE_BIT);
        const VkClearDepthStencilValue clear_stencil{1.0f, 0};
        VK_CALL(CmdClearDepthStencilImage, command, stencil_image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_stencil, 1, &stencil_range);
        transition_stencil(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                           VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    }
    VkRenderingAttachmentInfo color = structure<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    color.imageView = view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (multisample_view) {
        color.imageView = multisample_view;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        color.resolveImageView = view;
        color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkRenderingInfo render = structure<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
    render.renderArea = scissor;
    render.layerCount = config.view_mask ? 1 : config.layers;
    render.viewMask = config.view_mask;
    render.colorAttachmentCount = 1;
    render.pColorAttachments = &color;
    if (config.secondary_replays)
        render.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
    VkRenderingAttachmentInfo stencil = structure<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    if (has_depth_stencil) {
        stencil.imageView = stencil_view;
        stencil.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        stencil.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        stencil.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        render.pDepthAttachment = &stencil;
        if (depth_has_stencil)
            render.pStencilAttachment = &stencil;
    }
    VK_CALL(CmdBeginRendering, command, &render);
    VkCommandBuffer drawing = command;
    if (config.secondary_replays) {
        command_info.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        VK_CHECK(AllocateCommandBuffers, api.device, &command_info, &drawing);
        VkCommandBufferInheritanceRenderingInfo inherited_rendering = structure<VkCommandBufferInheritanceRenderingInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO);
        inherited_rendering.viewMask = config.view_mask;
        inherited_rendering.colorAttachmentCount = 1;
        inherited_rendering.pColorAttachmentFormats = &image_info.format;
        inherited_rendering.rasterizationSamples = config.samples;
        inherited_rendering.depthAttachmentFormat = rendering.depthAttachmentFormat;
        inherited_rendering.stencilAttachmentFormat = rendering.stencilAttachmentFormat;
        VkCommandBufferInheritanceInfo inheritance = structure<VkCommandBufferInheritanceInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO);
        inheritance.pNext = &inherited_rendering;
        inheritance.pipelineStatistics = config.pipeline_statistics;
        begin.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT |
                      VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
        begin.pInheritanceInfo = &inheritance;
        VK_CHECK(BeginCommandBuffer, drawing, &begin);
    }
    VK_CALL(CmdBindPipeline, drawing, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VK_CALL(CmdBindDescriptorSets, drawing, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
            0, 1, &set, 0, nullptr);
    if (predicate.buffer) {
        if (config.device_address_commands) {
            auto condition = structure<VkConditionalRenderingBeginInfo2EXT>(VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_2_EXT);
            condition.addressRange = {predicate.address, predicate.size};
            condition.addressFlags = VK_ADDRESS_COMMAND_FULLY_BOUND_BIT_KHR;
            condition.flags = config.conditional_inverted ? VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT : 0;
            VK_CALL(CmdBeginConditionalRendering2EXT, drawing, &condition);
        } else {
            auto condition = structure<VkConditionalRenderingBeginInfoEXT>(VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT);
            condition.buffer = predicate.buffer;
            condition.flags = config.conditional_inverted ? VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT : 0;
            VK_CALL(CmdBeginConditionalRenderingEXT, drawing, &condition);
        }
    }
    const bool feedback_per_draw = config.query_restoration || !config.raster_warmup.empty();
    const auto begin_feedback = [&] {
        if (config.device_address_commands) {
            auto binding = structure<VkBindTransformFeedbackBuffer2InfoEXT>(VK_STRUCTURE_TYPE_BIND_TRANSFORM_FEEDBACK_BUFFER_2_INFO_EXT);
            binding.addressRange = {feedback.address, feedback.size};
            binding.addressFlags = VK_ADDRESS_COMMAND_FULLY_BOUND_BIT_KHR |
                                   VK_ADDRESS_COMMAND_TRANSFORM_FEEDBACK_BUFFER_USAGE_BIT_KHR;
            VK_CALL(CmdBindTransformFeedbackBuffers2EXT, drawing, 0, 1, &binding);
            VK_CALL(CmdBeginTransformFeedback2EXT, drawing, 0, 0, nullptr);
        } else {
            VK_CALL(CmdBindTransformFeedbackBuffersEXT, drawing, 0, feedback_handles.size(),
                    feedback_handles.data(), feedback_offsets.data(), feedback_sizes.data());
            VK_CALL(CmdBeginTransformFeedbackEXT, drawing, 0, 0, nullptr, nullptr);
        }
    };
    const auto end_feedback = [&] {
        if (config.device_address_commands)
            VK_CALL(CmdEndTransformFeedback2EXT, drawing, 0, 0, nullptr);
        else if (feedback_counter.buffer) {
            VK_CALL(CmdEndTransformFeedbackEXT, drawing, 0, feedback_counter_handles.size(),
                    feedback_counter_handles.data(), feedback_counter_offsets.data());
        } else
            VK_CALL(CmdEndTransformFeedbackEXT, drawing, 0, 0, nullptr, nullptr);
    };
    if (feedback.buffer && !feedback_per_draw)
        begin_feedback();
    const auto issue_draw = [&] {
        if (feedback.buffer && feedback_per_draw)
            begin_feedback();
        if (config.device_address_commands) {
            if (indices.buffer) {
                auto binding = structure<VkBindIndexBuffer3InfoKHR>(VK_STRUCTURE_TYPE_BIND_INDEX_BUFFER_3_INFO_KHR);
                binding.addressRange = {indices.address, indices.size};
                binding.addressFlags = VK_ADDRESS_COMMAND_FULLY_BOUND_BIT_KHR;
                binding.indexType = VK_INDEX_TYPE_UINT32;
                VK_CALL(CmdBindIndexBuffer3KHR, drawing, &binding);
            }
            const VkDeviceSize stride = indices.buffer ? sizeof(VkDrawIndexedIndirectCommand)
                                                         : sizeof(VkDrawIndirectCommand);
            if (indirect_count.buffer) {
                auto info = structure<VkDrawIndirectCount2InfoKHR>(VK_STRUCTURE_TYPE_DRAW_INDIRECT_COUNT_2_INFO_KHR);
                info.addressRange = {indirect.address, indirect.size, stride};
                info.addressFlags = VK_ADDRESS_COMMAND_FULLY_BOUND_BIT_KHR;
                info.countAddressRange = {indirect_count.address, indirect_count.size};
                info.countAddressFlags = VK_ADDRESS_COMMAND_FULLY_BOUND_BIT_KHR;
                info.maxDrawCount = config.max_indirect_count;
                if (indices.buffer)
                    VK_CALL(CmdDrawIndexedIndirectCount2KHR, drawing, &info);
                else
                    VK_CALL(CmdDrawIndirectCount2KHR, drawing, &info);
            } else if (config.indirect) {
                auto info = structure<VkDrawIndirect2InfoKHR>(VK_STRUCTURE_TYPE_DRAW_INDIRECT_2_INFO_KHR);
                info.addressRange = {indirect.address, indirect.size, stride};
                info.addressFlags = VK_ADDRESS_COMMAND_FULLY_BOUND_BIT_KHR;
                info.drawCount = 1;
                if (indices.buffer)
                    VK_CALL(CmdDrawIndexedIndirect2KHR, drawing, &info);
                else
                    VK_CALL(CmdDrawIndirect2KHR, drawing, &info);
            } else if (indices.buffer) {
                VK_CALL(CmdDrawIndexed, drawing, config.vertex_count, config.instance_count,
                        0, static_cast<int32_t>(config.first_vertex), config.first_instance);
            } else {
                VK_CALL(CmdDraw, drawing, config.vertex_count, config.instance_count,
                        config.first_vertex, config.first_instance);
            }
        } else if (indices.buffer) {
            VK_CALL(CmdBindIndexBuffer, drawing, indices.buffer, 0, VK_INDEX_TYPE_UINT32);
            if (indirect_count.buffer)
                VK_CALL(CmdDrawIndexedIndirectCount, drawing, indirect.buffer, 0,
                        indirect_count.buffer, 0, config.max_indirect_count,
                        sizeof(VkDrawIndexedIndirectCommand));
            else if (config.indirect)
                VK_CALL(CmdDrawIndexedIndirect, drawing, indirect.buffer, 0, 1, sizeof(VkDrawIndexedIndirectCommand));
            else
                VK_CALL(CmdDrawIndexed, drawing, config.vertex_count, config.instance_count,
                        0, static_cast<int32_t>(config.first_vertex), config.first_instance);
        } else if (indirect_count.buffer) {
            VK_CALL(CmdDrawIndirectCount, drawing, indirect.buffer, 0,
                    indirect_count.buffer, 0, config.max_indirect_count,
                    sizeof(VkDrawIndirectCommand));
        } else if (config.indirect) {
            VK_CALL(CmdDrawIndirect, drawing, indirect.buffer, 0, 1, sizeof(VkDrawIndirectCommand));
        } else {
            VK_CALL(CmdDraw, drawing, config.vertex_count, config.instance_count,
                    config.first_vertex, config.first_instance);
        }
        if (feedback.buffer && feedback_per_draw)
            end_feedback();
    };
    const auto set_raster = [&](const RasterState &state) {
        VK_CALL(CmdSetPolygonModeEXT, drawing, state.polygon_mode);
        VK_CALL(CmdSetLineRasterizationModeEXT, drawing, state.line_mode);
        VK_CALL(CmdSetProvokingVertexModeEXT, drawing, state.provoking_vertex);
        VK_CALL(CmdSetDepthClipEnableEXT, drawing, state.depth_clip);
        VK_CALL(CmdSetLineStippleEnableEXT, drawing, state.stipple);
    };
    if (config.dynamic_raster)
        set_raster(live_raster);
    if (config.query_restoration)
        issue_draw();
    if (queries && !config.secondary_replays)
        VK_CALL(CmdBeginQuery, command, queries, 0, 0);
    if (feedback_query)
        VK_CALL(CmdBeginQueryIndexedEXT, command, feedback_query, 0, 0, 0);
    for (const auto &warmup : config.raster_warmup) {
        set_raster(warmup);
        issue_draw();
        VkClearAttachment attachments[2]{};
        attachments[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        attachments[1].aspectMask = depth_aspects;
        attachments[1].clearValue.depthStencil = {1.0f, 0};
        const VkClearRect rectangle{scissor, 0, config.view_mask ? 1u : config.layers};
        VK_CALL(CmdClearAttachments, drawing, has_depth_stencil ? 2 : 1, attachments, 1, &rectangle);
    }
    if (config.dynamic_raster)
        set_raster(live_raster);
    if (timestamp)
        VK_CALL(CmdWriteTimestamp, drawing, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamp, 0);
    const auto record_start = std::chrono::steady_clock::now();
    if (dynamic_input) {
        const uint32_t band_height = config.height / config.vertex_input_draws.size();
        for (uint32_t index = 0; index < config.vertex_input_draws.size(); ++index) {
            const auto &input = config.vertex_input_draws[index];
            require(!input.stipple || stipple_available, "dynamic stippled line mode unavailable");
            require(input.provoking_vertex != VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT ||
                        api.provoking.provokingVertexLast, "dynamic last provoking vertex unavailable");
            VkVertexInputBindingDescription2EXT binding = structure<VkVertexInputBindingDescription2EXT>(VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT);
            binding.stride = input.stride;
            binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            binding.divisor = 1;
            VkVertexInputAttributeDescription2EXT attribute = structure<VkVertexInputAttributeDescription2EXT>(VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT);
            attribute.format = input.format;
            attribute.offset = input.attribute_offset;
            VK_CALL(CmdSetVertexInputEXT, drawing, 1, &binding, 1, &attribute);
            if (config.device_address_commands) {
                auto binding_info = structure<VkBindVertexBuffer3InfoKHR>(VK_STRUCTURE_TYPE_BIND_VERTEX_BUFFER_3_INFO_KHR);
                binding_info.addressRange = {vertices.address + input.binding_offset,
                                              vertices.size - input.binding_offset, input.stride};
                binding_info.addressFlags = VK_ADDRESS_COMMAND_FULLY_BOUND_BIT_KHR;
                VK_CALL(CmdBindVertexBuffers3KHR, drawing, 0, 1, &binding_info);
            } else {
                VK_CALL(CmdBindVertexBuffers, drawing, 0, 1, &vertices.buffer, &input.binding_offset);
            }
            VK_CALL(CmdSetProvokingVertexModeEXT, drawing, input.provoking_vertex);
            VK_CALL(CmdSetLineStippleEnableEXT, drawing, input.stipple);
            const VkViewport band_viewport{0, float(index * band_height),
                                            float(config.width), float(band_height), 0, 1};
            const VkRect2D band_scissor{{0, int32_t(index * band_height)},
                                         {config.width, band_height}};
            VK_CALL(CmdSetViewport, drawing, 0, 1, &band_viewport);
            VK_CALL(CmdSetScissor, drawing, 0, 1, &band_scissor);
            issue_draw();
        }
    } else {
        for (uint32_t repeat = 0; repeat < config.repeat_count; ++repeat)
            issue_draw();
    }
    const double record_nanoseconds = std::chrono::duration<double, std::nano>(
        std::chrono::steady_clock::now() - record_start).count();
    if (timestamp)
        VK_CALL(CmdWriteTimestamp, drawing, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamp, 1);
    if (feedback.buffer && !feedback_per_draw)
        end_feedback();
    if (feedback_query)
        VK_CALL(CmdEndQueryIndexedEXT, command, feedback_query, 0, 0);
    if (predicate.buffer)
        VK_CALL(CmdEndConditionalRenderingEXT, drawing);
    if (config.secondary_replays) {
        VK_CHECK(EndCommandBuffer, drawing);
        if (config.secondary_query_transitions)
            VK_CALL(CmdExecuteCommands, command, 1, &drawing);
        if (queries)
            VK_CALL(CmdBeginQuery, command, queries, 0, 0);
        for (uint32_t replay = 0; replay < config.secondary_replays; ++replay)
            VK_CALL(CmdExecuteCommands, command, 1, &drawing);
    }
    if (queries)
        VK_CALL(CmdEndQuery, command, queries, 0);
    if (config.secondary_query_transitions)
        VK_CALL(CmdExecuteCommands, command, 1, &drawing);
    if (config.query_restoration)
        issue_draw();
    if (config.end_rendering2) {
        auto end = structure<VkRenderingEndInfoKHR>(VK_STRUCTURE_TYPE_RENDERING_END_INFO_KHR);
        VK_CALL(CmdEndRendering2KHR, command, &end);
    } else {
        VK_CALL(CmdEndRendering, command);
    }
    if (config.query_after_rendering) {
        VK_CALL(CmdBeginQuery, command, queries, render_query_count, 0);
        VK_CALL(CmdEndQuery, command, queries, render_query_count);
    }
    if (queries) {
        const VkQueryResultFlags flags = VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT |
                                         VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;
        if (config.device_address_commands) {
            const VkStridedDeviceAddressRangeKHR destination{
                query_copy.address, query_copy.size, query_words * sizeof(uint64_t)};
            VK_CALL(CmdCopyQueryPoolResultsToMemoryKHR, command, queries, 0, query_count,
                    &destination, VK_ADDRESS_COMMAND_FULLY_BOUND_BIT_KHR, flags);
        } else {
            VK_CALL(CmdCopyQueryPoolResults, command, queries, 0, query_count,
                    query_copy.buffer, 0, query_words * sizeof(uint64_t), flags);
        }
    }
    if (feedback_query)
        VK_CALL(CmdCopyQueryPoolResults, command, feedback_query, 0, 1,
                feedback_query_copy.buffer, 0, 3 * sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT |
                    VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    transition(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, config.layers};
    copy.imageExtent = image_info.extent;
    if (config.device_address_commands) {
        auto region = structure<VkDeviceMemoryImageCopyKHR>(VK_STRUCTURE_TYPE_DEVICE_MEMORY_IMAGE_COPY_KHR);
        region.addressRange = {pixels.address, pixels.size};
        region.addressFlags = VK_ADDRESS_COMMAND_FULLY_BOUND_BIT_KHR;
        region.imageSubresource = copy.imageSubresource;
        region.imageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        region.imageExtent = copy.imageExtent;
        auto info = structure<VkCopyDeviceMemoryImageInfoKHR>(VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_IMAGE_INFO_KHR);
        info.image = image;
        info.regionCount = 1;
        info.pRegions = &region;
        VK_CALL(CmdCopyImageToMemoryKHR, command, &info);
    } else {
        VK_CALL(CmdCopyImageToBuffer, command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                pixels.buffer, 1, &copy);
    }
    if (has_depth_stencil) {
        transition_stencil(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy stencil_copy = copy;
        if (config.stencil) {
            stencil_copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
            VK_CALL(CmdCopyImageToBuffer, command, stencil_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    stencil_pixels.buffer, 1, &stencil_copy);
        }
        if (config.depth) {
            stencil_copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            VK_CALL(CmdCopyImageToBuffer, command, stencil_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    depth_pixels.buffer, 1, &stencil_copy);
        }
    }
    VkMemoryBarrier host = structure<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
    host.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    VK_CALL(CmdPipelineBarrier, command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
    VK_CHECK(EndCommandBuffer, command);
    VkFenceCreateInfo fence_info = structure<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    VkFence fence;
    VK_CHECK(CreateFence, api.device, &fence_info, nullptr, &fence);
    cleanup.actions.emplace_back([&, fence] { VK_CALL(DestroyFence, api.device, fence, nullptr); });
    VkSubmitInfo submit = structure<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    VK_CHECK(QueueSubmit, api.queue, 1, &submit, fence);
    VK_CHECK(WaitForFences, api.device, 1, &fence, VK_TRUE, 30'000'000'000ull);

    DrawResult result;
    result.record_nanoseconds = record_nanoseconds;
    if (queries) {
        result.statistics.resize(query_count * query_words);
        VK_CHECK(GetQueryPoolResults, api.device, queries, 0, query_count,
                 result.statistics.size() * sizeof(uint64_t), result.statistics.data(),
                 query_words * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT |
                     VK_QUERY_RESULT_WAIT_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        result.copied_statistics.resize(query_count * query_words);
        read(query_copy, result.copied_statistics.data());
    }
    if (timestamp) {
        uint64_t ticks[2];
        VK_CHECK(GetQueryPoolResults, api.device, timestamp, 0, 2, sizeof(ticks), ticks,
                 sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        const uint64_t mask = api.timestamp_bits == 64 ? UINT64_MAX : (uint64_t(1) << api.timestamp_bits) - 1;
        result.gpu_nanoseconds = double((ticks[1] - ticks[0]) & mask) * api.timestamp_period;
    }
    result.pixels.resize(pixels.size / 4);
    read(pixels, result.pixels.data());
    if (config.stencil) {
        result.stencil.resize(stencil_pixels.size);
        read(stencil_pixels, result.stencil.data());
    }
    if (config.depth) {
        result.depth.resize(depth_pixels.size / (depth_unorm ? 2 : 4));
        if (depth_unorm) {
            std::vector<uint16_t> words(result.depth.size());
            read(depth_pixels, words.data());
            for (unsigned i = 0; i < words.size(); ++i)
                result.depth[i] = float(words[i]) / 65535.0f;
        } else {
            read(depth_pixels, result.depth.data());
        }
    }
    if (!config.counters.empty()) {
        result.counters.resize(config.counters.size());
        read(counters, result.counters.data());
    }
    if (feedback.buffer) {
        result.transform_feedback.resize(feedback.size / sizeof(uint32_t));
        read(feedback, result.transform_feedback.data());
        if (!config.transform_feedback_bindings.empty()) {
            for (const auto &entry : feedback_buffers) {
                result.transform_feedback_buffers.emplace_back(entry.size / sizeof(uint32_t));
                read(entry, result.transform_feedback_buffers.back().data());
            }
        }
    }
    if (feedback_query) {
        result.transform_feedback_query.resize(3);
        VK_CHECK(GetQueryPoolResults, api.device, feedback_query, 0, 1,
                 3 * sizeof(uint64_t), result.transform_feedback_query.data(),
                 3 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT |
                     VK_QUERY_RESULT_WAIT_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        result.copied_transform_feedback_query.resize(3);
        read(feedback_query_copy, result.copied_transform_feedback_query.data());
        result.transform_feedback_bytes_written_per_buffer.resize(feedback_buffers.size());
        read(feedback_counter, result.transform_feedback_bytes_written_per_buffer.data());
        result.transform_feedback_bytes_written = result.transform_feedback_bytes_written_per_buffer.front();
    }
    size_t cache_size = 0;
    VK_CHECK(GetPipelineCacheData, api.device, cache, &cache_size, nullptr);
    result.pipeline_cache.resize(cache_size);
    VK_CHECK(GetPipelineCacheData, api.device, cache, &cache_size, result.pipeline_cache.data());
    result.pipeline_cache.resize(cache_size);
    return result;
}

#undef VK_CHECK
#undef VK_CALL
#undef VK_FUNCTION

} // namespace uml::gpu::test
