// SPDX-License-Identifier: GPL-2.0-or-later
#include "shaders/rgb32_texel_spirv.hpp"
#include "metal_submit_timing.hpp"

#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using uml::gpu::test::SubmitTiming;
using uml::gpu::test::SubmitTimingCollector;
constexpr uint32_t guard = 0xa5a5a5a5;
constexpr uint32_t guard_vectors = 16;
constexpr uint32_t max_elements = 1048579;
constexpr uint32_t view_batch_size = 32;
enum class TimingMode { vulkan_queries, metal_whole_submit, correctness_only };

const char *timing_name(TimingMode mode)
{
    switch (mode) {
    case TimingMode::vulkan_queries: return "vulkan-queries";
    case TimingMode::metal_whole_submit: return "metal-whole-submit";
    case TimingMode::correctness_only: return "correctness-only";
    }
    throw std::runtime_error("invalid timing mode");
}

void require(bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void check(VkResult result, const char *operation)
{
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}

template <typename T>
T structure(VkStructureType type)
{
    T value{};
    value.sType = type;
    return value;
}

double nanoseconds(Clock::time_point start)
{
    return std::chrono::duration<double, std::nano>(Clock::now() - start).count();
}

double median(std::vector<double> samples)
{
    require(!samples.empty(), "empty measurement series");
    std::sort(samples.begin(), samples.end());
    const auto middle = samples.size() / 2;
    return samples.size() % 2 ? samples[middle] : (samples[middle - 1] + samples[middle]) / 2;
}

constexpr uint64_t timestamp_delta(uint64_t first, uint64_t last, uint32_t bits)
{
    if (bits == 0 || bits > 64) return 0;
    const uint64_t mask = bits == 64 ? UINT64_MAX : (uint64_t(1) << bits) - 1;
    first &= mask;
    last &= mask;
    return first != 0 && last > first ? last - first : 0;
}

static_assert(timestamp_delta(7, 9, 64) == 2);
static_assert(timestamp_delta(0, 9, 64) == 0);
static_assert(timestamp_delta(7, 7, 64) == 0);
static_assert(timestamp_delta(9, 7, 64) == 0);
static_assert(timestamp_delta(9, 0, 64) == 0);
static_assert(timestamp_delta(0xfd, 0x02, 8) == 0);
static_assert(timestamp_delta(0x107, 0x109, 8) == 2);
static_assert(timestamp_delta(7, 9, 0) == 0 && timestamp_delta(7, 9, 65) == 0);

#define DEVICE_FUNCTIONS(X) \
    X(DestroyDevice) X(GetDeviceQueue) X(CreateBuffer) X(DestroyBuffer) \
    X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) \
    X(MapMemory) X(UnmapMemory) X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges) \
    X(CreateBufferView) X(DestroyBufferView) X(CreateShaderModule) X(DestroyShaderModule) \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) \
    X(DestroyPipelineLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool) \
    X(AllocateDescriptorSets) X(FreeDescriptorSets) X(UpdateDescriptorSets) \
    X(CreateComputePipelines) X(DestroyPipeline) X(CreateCommandPool) X(DestroyCommandPool) \
    X(AllocateCommandBuffers) X(ResetCommandBuffer) X(BeginCommandBuffer) X(EndCommandBuffer) \
    X(CmdPipelineBarrier) X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) \
    X(CmdDispatch) X(CmdResetQueryPool) X(CmdWriteTimestamp) X(CreateQueryPool) \
    X(DestroyQueryPool) X(GetQueryPoolResults) X(QueueSubmit) X(QueueWaitIdle) \
    X(CreateFence) X(DestroyFence) X(ResetFences) X(WaitForFences)

struct Context {
    const TimingMode timing_mode;
    std::unique_ptr<SubmitTimingCollector> submit_timing;
    unsigned submissions = 0;
    unsigned invalid_gpu = 0;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::array<VkPipeline, 3> pipelines{};
    uint32_t timestamp_bits = 0;
    PFN_vkDestroyInstance destroy_instance = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties format_properties = nullptr;
#define DECLARE(name) PFN_vk##name name = nullptr;
    DEVICE_FUNCTIONS(DECLARE)
#undef DECLARE

    template <typename Function>
    Function instance_function(const char *name)
    {
        auto function = reinterpret_cast<Function>(vkGetInstanceProcAddr(instance, name));
        require(function != nullptr, std::string("missing ") + name);
        return function;
    }

    explicit Context(TimingMode mode) : timing_mode(mode)
    {
        try {
#if defined(__linux__)
            require(mode == TimingMode::correctness_only, "Linux probe supports correctness-only, not performance");
            const char *venus = std::getenv("UML_VULKAN_REQUIRE_VENUS");
            require(venus && std::string_view(venus) == "1", "Linux probe requires UML_VULKAN_REQUIRE_VENUS=1");
#endif
            if (mode == TimingMode::metal_whole_submit) {
                const char *debug = std::getenv("MESA_KK_DEBUG");
                require(debug && std::string_view(debug).find("submit_timing") != std::string_view::npos,
                        "metal-whole-submit requires MESA_KK_DEBUG=submit_timing and a diagnostic-capable provider");
            }
            initialize();
            if (mode == TimingMode::metal_whole_submit)
                submit_timing = std::make_unique<SubmitTimingCollector>();
        } catch (...) {
            release();
            throw;
        }
    }
    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;
    ~Context() { release(); }

    void release()
    {
        if (queue && QueueWaitIdle)
            (void)QueueWaitIdle(queue);
        if (device) {
            for (auto pipeline : pipelines)
                if (pipeline) DestroyPipeline(device, pipeline, nullptr);
            if (fence) DestroyFence(device, fence, nullptr);
            if (queries) DestroyQueryPool(device, queries, nullptr);
            if (command_pool) DestroyCommandPool(device, command_pool, nullptr);
            if (descriptor_pool) DestroyDescriptorPool(device, descriptor_pool, nullptr);
            if (pipeline_layout) DestroyPipelineLayout(device, pipeline_layout, nullptr);
            if (set_layout) DestroyDescriptorSetLayout(device, set_layout, nullptr);
            if (DestroyDevice) DestroyDevice(device, nullptr);
        }
        if (instance && destroy_instance)
            destroy_instance(instance, nullptr);
    }

    void initialize()
    {
        auto application = structure<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
        application.pApplicationName = "RGB32 texel-buffer benchmark";
        application.apiVersion = VK_API_VERSION_1_1;
        auto instance_info = structure<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        instance_info.pApplicationInfo = &application;
        check(instance_function<PFN_vkCreateInstance>("vkCreateInstance")(
                  &instance_info, nullptr, &instance), "vkCreateInstance");
        destroy_instance = instance_function<PFN_vkDestroyInstance>("vkDestroyInstance");
        auto enumerate = instance_function<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
        uint32_t count = 0;
        check(enumerate(instance, &count, nullptr), "physical count");
        require(count == 1, "benchmark requires exactly one directly linked native device");
        check(enumerate(instance, &count, &physical), "physical device");
        auto driver = structure<VkPhysicalDeviceDriverProperties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES);
        auto identity = structure<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
        identity.pNext = &driver;
        instance_function<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2")(
            physical, &identity);
        properties = identity.properties;
        const char *venus = std::getenv("UML_VULKAN_REQUIRE_VENUS");
        if (venus && std::string_view(venus) == "1") {
            require(timing_mode == TimingMode::correctness_only && driver.driverID == VK_DRIVER_ID_MESA_VENUS,
                    "supplemental guest probe requires the Venus Vulkan provider and correctness-only mode");
            std::puts("UML_VULKAN_PROBE_DEVICE: Venus");
        } else {
            require(driver.driverID == VK_DRIVER_ID_MESA_KOSMICKRISP, "unexpected Vulkan provider");
        }
        if (timing_mode == TimingMode::vulkan_queries)
            require(properties.limits.timestampComputeAndGraphics && properties.limits.timestampPeriod > 0 &&
                    std::isfinite(properties.limits.timestampPeriod), "compute timestamps unavailable");
        require(properties.limits.minTexelBufferOffsetAlignment > 0 &&
                properties.limits.minTexelBufferOffsetAlignment <= 1048576 &&
                properties.limits.maxComputeWorkGroupSize[0] >= 64 &&
                properties.limits.maxComputeWorkGroupInvocations >= 64 &&
                properties.limits.maxComputeWorkGroupCount[0] >= (max_elements + 63) / 64,
                "benchmark geometry exceeds native limits");
        instance_function<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(
            physical, &memory);
        format_properties = instance_function<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties");
        auto family_properties = instance_function<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
        family_properties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        family_properties(physical, &count, families.data());
        uint32_t family = 0;
        for (; family < count; ++family) {
            if ((families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
                (timing_mode != TimingMode::vulkan_queries || families[family].timestampValidBits))
                break;
        }
        require(family < count && families[family].timestampValidBits <= 64, "suitable compute queue unavailable");
        timestamp_bits = families[family].timestampValidBits;
        const float priority = 1;
        auto queue_info = structure<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        auto enumerate_extensions = instance_function<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
        check(enumerate_extensions(physical, nullptr, &count, nullptr), "extension count");
        std::vector<VkExtensionProperties> extensions(count);
        check(enumerate_extensions(physical, nullptr, &count, extensions.data()), "extensions");
        std::vector<const char *> enabled;
        for (const auto &extension : extensions)
            if (std::strcmp(extension.extensionName, "VK_KHR_portability_subset") == 0)
                enabled.push_back("VK_KHR_portability_subset");
        auto device_info = structure<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
        device_info.ppEnabledExtensionNames = enabled.data();
        check(instance_function<PFN_vkCreateDevice>("vkCreateDevice")(
                  physical, &device_info, nullptr, &device), "vkCreateDevice");
        auto get_device = instance_function<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
#define LOAD(name) name = reinterpret_cast<PFN_vk##name>(get_device(device, "vk" #name)); require(name != nullptr, "missing vk" #name);
        DEVICE_FUNCTIONS(LOAD)
#undef LOAD
        GetDeviceQueue(device, family, 0, &queue);
        const VkDescriptorSetLayoutBinding bindings[] = {
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        };
        auto layout = structure<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
        layout.bindingCount = 2;
        layout.pBindings = bindings;
        check(CreateDescriptorSetLayout(device, &layout, nullptr, &set_layout), "descriptor layout");
        const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t)};
        auto pipeline_info = structure<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        pipeline_info.setLayoutCount = 1;
        pipeline_info.pSetLayouts = &set_layout;
        pipeline_info.pushConstantRangeCount = 1;
        pipeline_info.pPushConstantRanges = &range;
        check(CreatePipelineLayout(device, &pipeline_info, nullptr, &pipeline_layout), "pipeline layout");
        const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
        };
        auto pool = structure<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
        pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool.maxSets = 2;
        pool.poolSizeCount = 2;
        pool.pPoolSizes = sizes;
        check(CreateDescriptorPool(device, &pool, nullptr, &descriptor_pool), "descriptor pool");
        auto command_info = structure<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        command_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        command_info.queueFamilyIndex = family;
        check(CreateCommandPool(device, &command_info, nullptr, &command_pool), "command pool");
        auto allocate = structure<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        allocate.commandPool = command_pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        check(AllocateCommandBuffers(device, &allocate, &command), "command buffer");
        if (timing_mode == TimingMode::vulkan_queries) {
            auto query = structure<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
            query.queryType = VK_QUERY_TYPE_TIMESTAMP;
            query.queryCount = 2;
            check(CreateQueryPool(device, &query, nullptr, &queries), "query pool");
        }
        auto fence_info = structure<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        check(CreateFence(device, &fence_info, nullptr, &fence), "fence");
        const std::array<std::span<const uint32_t>, 3> shaders{
            rgb32_texel::floating, rgb32_texel::signed_integer, rgb32_texel::unsigned_integer};
        for (uint32_t kind = 0; kind < shaders.size(); ++kind) {
            auto shader_info = structure<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
            shader_info.codeSize = shaders[kind].size_bytes();
            shader_info.pCode = shaders[kind].data();
            VkShaderModule shader = VK_NULL_HANDLE;
            check(CreateShaderModule(device, &shader_info, nullptr, &shader), "shader module");
            auto compute = structure<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
            compute.stage = structure<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
            compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            compute.stage.module = shader;
            compute.stage.pName = "main";
            compute.layout = pipeline_layout;
            const auto result = CreateComputePipelines(device, VK_NULL_HANDLE, 1, &compute, nullptr, &pipelines[kind]);
            DestroyShaderModule(device, shader, nullptr);
            check(result, "compute pipeline");
        }
        std::printf("provider=%s device=%s timing=%s", driver.driverName, properties.deviceName, timing_name(timing_mode));
        if (timing_mode == TimingMode::vulkan_queries)
            std::printf(" timestamp_bits=%u timestamp_period_ns=%.9g", timestamp_bits, properties.limits.timestampPeriod);
        std::putchar('\n');
    }
};
#undef DEVICE_FUNCTIONS

struct Buffer {
    Context &api;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size;
    uint32_t *mapped = nullptr;
    bool coherent = false;

    Buffer(Context &context, VkDeviceSize bytes, VkBufferUsageFlags usage) : api(context), size(bytes)
    {
        try {
            auto info = structure<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
            info.size = size;
            info.usage = usage;
            check(api.CreateBuffer(api.device, &info, nullptr, &buffer), "buffer");
            VkMemoryRequirements requirements{};
            api.GetBufferMemoryRequirements(api.device, buffer, &requirements);
            uint32_t type = 0;
            for (; type < api.memory.memoryTypeCount; ++type)
                if ((requirements.memoryTypeBits & (1u << type)) &&
                    (api.memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
                    break;
            require(type < api.memory.memoryTypeCount, "host-visible memory unavailable");
            coherent = api.memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            auto allocate = structure<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
            allocate.allocationSize = requirements.size;
            allocate.memoryTypeIndex = type;
            check(api.AllocateMemory(api.device, &allocate, nullptr, &memory), "memory");
            check(api.BindBufferMemory(api.device, buffer, memory, 0), "bind memory");
            void *address = nullptr;
            check(api.MapMemory(api.device, memory, 0, VK_WHOLE_SIZE, 0, &address), "map memory");
            mapped = static_cast<uint32_t *>(address);
        } catch (...) {
            release();
            throw;
        }
    }
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    ~Buffer() { release(); }
    void release()
    {
        if (mapped) api.UnmapMemory(api.device, memory);
        if (buffer) api.DestroyBuffer(api.device, buffer, nullptr);
        if (memory) api.FreeMemory(api.device, memory, nullptr);
    }
    void synchronize(bool invalidate)
    {
        if (coherent) return;
        auto range = structure<VkMappedMemoryRange>(VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);
        range.memory = memory;
        range.size = VK_WHOLE_SIZE;
        check(invalidate ? api.InvalidateMappedMemoryRanges(api.device, 1, &range)
                         : api.FlushMappedMemoryRanges(api.device, 1, &range), "mapped memory synchronization");
    }
};

uint32_t component(uint32_t kind, uint32_t index, uint32_t channel)
{
    if (channel == 3) return kind == 0 ? std::bit_cast<uint32_t>(1.0f) : 1u;
    if (kind == 0) {
        const int32_t value = static_cast<int32_t>((index * 13u + channel * 7u) % 4093u) - 2046;
        return std::bit_cast<uint32_t>(static_cast<float>(value) * 0.125f);
    }
    return (index * 747796405u + channel * 2891336453u) ^ 0x800012abu;
}

struct Samples {
    std::vector<double> gpu, record, view_batch, submit_wall;
    std::vector<std::array<uint64_t, 2>> ticks;
    std::vector<SubmitTiming> submits;
    uint32_t invalid_gpu = 0;
};

struct Workload {
    Context &api;
    uint32_t kind, count, channels;
    VkDeviceSize offset, input_bytes;
    Buffer input, output;
    VkFormat format;
    VkBufferView view = VK_NULL_HANDLE;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
    Samples samples;

    Workload(Context &context, uint32_t type, uint32_t elements, uint32_t components)
        : api(context), kind(type), count(elements), channels(components),
          offset(std::lcm(std::max<VkDeviceSize>(16, api.properties.limits.minTexelBufferOffsetAlignment), VkDeviceSize(channels * 4))),
          input_bytes(VkDeviceSize(count) * channels * 4),
          input(api, offset + input_bytes + 64, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT),
          output(api, VkDeviceSize(count + 2 * guard_vectors) * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT),
          format(channels == 3 ? std::array{VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32_UINT}[kind]
                               : std::array{VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32A32_UINT}[kind])
    {
        require(count <= api.properties.limits.maxTexelBufferElements &&
                output.size <= api.properties.limits.maxStorageBufferRange, "resource exceeds native limits");
        VkFormatProperties features{};
        api.format_properties(api.physical, format, &features);
        require(features.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT, "texel format unavailable");
        std::fill_n(input.mapped, input.size / 4, guard);
        for (uint32_t index = 0; index < count; ++index)
            for (uint32_t channel = 0; channel < channels; ++channel)
                input.mapped[offset / 4 + index * channels + channel] = component(kind, index, channel);
        input.synchronize(false);
        try {
            view = create_view();
            auto allocate = structure<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
            allocate.descriptorPool = api.descriptor_pool;
            allocate.descriptorSetCount = 1;
            allocate.pSetLayouts = &api.set_layout;
            check(api.AllocateDescriptorSets(api.device, &allocate, &descriptor), "descriptor set");
            VkDescriptorBufferInfo output_info{output.buffer, 0, output.size};
            std::array<VkWriteDescriptorSet, 2> writes{};
            for (uint32_t binding = 0; binding < writes.size(); ++binding) {
                writes[binding] = structure<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
                writes[binding].dstSet = descriptor;
                writes[binding].dstBinding = binding;
                writes[binding].descriptorCount = 1;
            }
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            writes[0].pTexelBufferView = &view;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[1].pBufferInfo = &output_info;
            api.UpdateDescriptorSets(api.device, 2, writes.data(), 0, nullptr);
        } catch (...) {
            release();
            throw;
        }
    }
    Workload(const Workload &) = delete;
    Workload &operator=(const Workload &) = delete;
    ~Workload() { release(); }
    void release()
    {
        // Also keep exception-path destruction safe if a bounded fence wait failed.
        (void)api.QueueWaitIdle(api.queue);
        if (descriptor) (void)api.FreeDescriptorSets(api.device, api.descriptor_pool, 1, &descriptor);
        if (view) api.DestroyBufferView(api.device, view, nullptr);
    }
    VkBufferView create_view()
    {
        auto info = structure<VkBufferViewCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO);
        info.buffer = input.buffer;
        info.format = format;
        info.offset = offset;
        info.range = input_bytes;
        VkBufferView result = VK_NULL_HANDLE;
        check(api.CreateBufferView(api.device, &info, nullptr, &result), "buffer view");
        return result;
    }
    double measure_view_batch()
    {
        // A single raw RGB32 view can finish within one host clock tick.
        // Measure a fixed batch, never clamp zero or replace a failed sample.
        std::array<VkBufferView, view_batch_size> views{};
        const auto start = Clock::now();
        try {
            for (auto &temporary : views) temporary = create_view();
        } catch (...) {
            for (auto temporary : views)
                if (temporary) api.DestroyBufferView(api.device, temporary, nullptr);
            throw;
        }
        const double elapsed = nanoseconds(start);
        // All destruction is outside the creation-only measurement.
        for (auto temporary : views) api.DestroyBufferView(api.device, temporary, nullptr);
        return elapsed;
    }
    void execute(bool retain, bool empty = false)
    {
        const bool timed = api.timing_mode != TimingMode::correctness_only;
        require(timed || !retain, "correctness-only does not retain performance samples");
        const double view_batch_ns = timed ? measure_view_batch() : 0;
        std::fill_n(output.mapped, output.size / 4, guard);
        output.synchronize(false);
        const uint32_t active = empty ? 0 : count;
        const auto record_start = timed ? Clock::now() : Clock::time_point{};
        check(api.ResetCommandBuffer(api.command, 0), "reset command");
        auto begin = structure<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(api.BeginCommandBuffer(api.command, &begin), "begin command");
        std::array<VkBufferMemoryBarrier, 2> barriers{};
        for (uint32_t index = 0; index < barriers.size(); ++index) {
            barriers[index] = structure<VkBufferMemoryBarrier>(VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER);
            barriers[index].srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
            barriers[index].dstAccessMask = index ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
            barriers[index].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[index].buffer = index ? output.buffer : input.buffer;
            barriers[index].size = VK_WHOLE_SIZE;
        }
        api.CmdPipelineBarrier(api.command, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               0, 0, nullptr, 2, barriers.data(), 0, nullptr);
        if (api.queries) api.CmdResetQueryPool(api.command, api.queries, 0, 2);
        api.CmdBindPipeline(api.command, VK_PIPELINE_BIND_POINT_COMPUTE, api.pipelines[kind]);
        api.CmdBindDescriptorSets(api.command, VK_PIPELINE_BIND_POINT_COMPUTE, api.pipeline_layout,
                                  0, 1, &descriptor, 0, nullptr);
        api.CmdPushConstants(api.command, api.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(active), &active);
        if (api.queries)
            api.CmdWriteTimestamp(api.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, api.queries, 0);
        api.CmdDispatch(api.command, (count + 63) / 64, 1, 1);
        if (api.queries)
            api.CmdWriteTimestamp(api.command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, api.queries, 1);
        auto readback = barriers[1];
        readback.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        readback.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        api.CmdPipelineBarrier(api.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                               0, 0, nullptr, 1, &readback, 0, nullptr);
        check(api.EndCommandBuffer(api.command), "end command");
        const double record_ns = timed ? nanoseconds(record_start) : 0;
        check(api.ResetFences(api.device, 1, &api.fence), "reset fence");
        auto submit = structure<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &api.command;
        // Exactly one command buffer and one outstanding submission. Include
        // controls and warmups in callback coverage, not just retained samples.
        if (api.submit_timing) api.submit_timing->begin();
        ++api.submissions;
        const auto submit_start = timed ? Clock::now() : Clock::time_point{};
        check(api.QueueSubmit(api.queue, 1, &submit, api.fence), "submit");
        check(api.WaitForFences(api.device, 1, &api.fence, VK_TRUE, 5'000'000'000ULL), "bounded fence wait");
        const double submit_wall = timed ? std::chrono::duration<double>(Clock::now() - submit_start).count() : 0;
        uint64_t ticks[2]{};
        SubmitTiming completed{};
        double gpu_ns = std::numeric_limits<double>::quiet_NaN();
        if (api.submit_timing) {
            completed = api.submit_timing->complete(submit_wall);
            gpu_ns = (completed.end_seconds - completed.start_seconds) * 1e9;
        } else if (api.queries) {
            check(api.GetQueryPoolResults(api.device, api.queries, 0, 2, sizeof(ticks), ticks, sizeof(uint64_t),
                                          VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "timestamp results");
            // Preserve raw values. A missing, equal, reversed, or wrapped pair
            // is not timing evidence; never manufacture an unsigned delta.
            const uint64_t delta = timestamp_delta(ticks[0], ticks[1], api.timestamp_bits);
            if (delta) gpu_ns = double(delta) * api.properties.limits.timestampPeriod;
        }
        output.synchronize(true);
        for (uint32_t index = 0; index < count + 2 * guard_vectors; ++index) {
            for (uint32_t channel = 0; channel < 4; ++channel) {
                const bool data = !empty && index >= guard_vectors && index < count + guard_vectors;
                const uint32_t expected = data ? component(kind, index - guard_vectors, channel) : guard;
                if (output.mapped[index * 4 + channel] != expected)
                    throw std::runtime_error("exact output/guard mismatch: format=" + std::to_string(format) +
                                             " index=" + std::to_string(index) + " channel=" + std::to_string(channel) +
                                             " expected=" + std::to_string(expected) +
                                             " actual=" + std::to_string(output.mapped[index * 4 + channel]));
            }
        }
        input.synchronize(true);
        for (VkDeviceSize index = 0; index < offset / 4; ++index)
            require(input.mapped[index] == guard, "input prefix guard modified");
        for (VkDeviceSize index = (offset + input_bytes) / 4; index < input.size / 4; ++index)
            require(input.mapped[index] == guard, "input suffix guard modified");
        if (!timed) {
            std::printf("RGB32_CORRECTNESS submission=%u phase=%s type=%s channels=%u texels=%u "
                        "view_offset=%llu guards=pass result=pass\n",
                        api.submissions, empty ? "zero-active-control" : "full-data",
                        std::array{"float", "sint", "uint"}[kind], channels, count,
                        static_cast<unsigned long long>(offset));
            return;
        }
        require(std::isfinite(record_ns) && record_ns > 0 &&
                std::isfinite(view_batch_ns) && view_batch_ns > 0,
                "invalid CPU timing sample: record_ns=" + std::to_string(record_ns) +
                " view_batch_ns=" + std::to_string(view_batch_ns));
        const bool invalid_gpu = !std::isfinite(gpu_ns) || gpu_ns <= 0;
        api.invalid_gpu += invalid_gpu;
        std::printf("RGB32_SAMPLE submission=%u phase=%s type=%s channels=%u texels=%u "
                    "correctness=pass timing=%s synchronous_wall_s=%.17g "
                    "cpu_record_ns=%.3f cpu_view_batch_ns=%.3f cpu_view_batch_count=%u ",
                    api.submissions, empty ? "control" : retain ? "retained" : "warmup",
                    std::array{"float", "sint", "uint"}[kind], channels, count,
                    timing_name(api.timing_mode), submit_wall, record_ns, view_batch_ns, view_batch_size);
        if (api.submit_timing)
            std::printf("device=0x%llx gpu_start_s=%.17g gpu_end_s=%.17g error=%u\n",
                        static_cast<unsigned long long>(completed.device), completed.start_seconds,
                        completed.end_seconds, completed.error);
        else
            std::printf("start_tick=%llu end_tick=%llu gpu_valid=%s\n",
                        static_cast<unsigned long long>(ticks[0]), static_cast<unsigned long long>(ticks[1]),
                        invalid_gpu ? "false" : "true");
        if (retain) {
            samples.view_batch.push_back(view_batch_ns);
            samples.record.push_back(record_ns);
            samples.gpu.push_back(gpu_ns);
            samples.ticks.push_back({ticks[0], ticks[1]});
            samples.submits.push_back(completed);
            samples.submit_wall.push_back(submit_wall);
            samples.invalid_gpu += invalid_gpu;
        }
    }
    bool report(uint32_t warmup) const
    {
        const bool valid = samples.invalid_gpu == 0;
        const double gpu = valid ? median(samples.gpu) : std::numeric_limits<double>::quiet_NaN();
        std::printf("{\"type\":\"%s\",\"channels\":%u,\"texels\":%u,\"view_bytes\":%llu,"
                    "\"view_offset\":%llu,\"output_bytes\":%llu,\"samples\":%zu,\"warmup\":%u,"
                    "\"timing\":\"%s\",\"cpu_timing\":\"instrumented\","
                    "\"correctness\":true,\"invalid_gpu_samples\":%u,\"gpu_samples_valid\":%s,"
                    "\"cpu_record_ns_median\":%.3f,\"cpu_view_batch_count\":%u,"
                    "\"cpu_view_create_batch_ns_median\":%.3f,"
                    "\"cpu_view_create_amortized_ns_median\":%.3f,",
                    std::array{"float", "sint", "uint"}[kind], channels, count,
                    static_cast<unsigned long long>(input_bytes), static_cast<unsigned long long>(offset),
                    static_cast<unsigned long long>(output.size), samples.gpu.size(), warmup,
                    timing_name(api.timing_mode),
                    samples.invalid_gpu, valid ? "true" : "false", median(samples.record), view_batch_size,
                    median(samples.view_batch), median(samples.view_batch) / view_batch_size);
        std::printf("\"cpu_record_samples_ns\":[");
        for (std::size_t index = 0; index < samples.record.size(); ++index)
            std::printf("%s%.3f", index ? "," : "", samples.record[index]);
        std::printf("],\"cpu_view_create_batch_samples_ns\":[");
        for (std::size_t index = 0; index < samples.view_batch.size(); ++index)
            std::printf("%s%.3f", index ? "," : "", samples.view_batch[index]);
        std::printf("],");
        if (api.submit_timing) {
            std::printf("\"gpu_raw_submit_seconds\":[");
            for (std::size_t index = 0; index < samples.submits.size(); ++index)
                std::printf("%s[%.17g,%.17g]", index ? "," : "",
                            samples.submits[index].start_seconds, samples.submits[index].end_seconds);
        } else {
            std::printf("\"gpu_raw_ticks\":[");
            for (std::size_t index = 0; index < samples.ticks.size(); ++index)
                std::printf("%s[%llu,%llu]", index ? "," : "",
                            static_cast<unsigned long long>(samples.ticks[index][0]),
                            static_cast<unsigned long long>(samples.ticks[index][1]));
        }
        std::printf("],\"synchronous_submit_wall_seconds\":[");
        for (std::size_t index = 0; index < samples.submit_wall.size(); ++index)
            std::printf("%s%.17g", index ? "," : "", samples.submit_wall[index]);
        std::printf("],");
        std::printf("\"gpu_samples_ns\":[");
        for (std::size_t index = 0; index < samples.gpu.size(); ++index) {
            std::printf("%s", index ? "," : "");
            if (std::isfinite(samples.gpu[index]) && samples.gpu[index] > 0)
                std::printf("%.3f", samples.gpu[index]);
            else
                std::printf("null");
        }
        std::printf("],");
        const char *metric = api.submit_timing ? "gpu_whole_submit_ns_median" : "gpu_dispatch_ns_median";
        if (valid)
            std::printf("\"%s\":%.3f,\"logical_texels_per_ns\":%.9f}\n", metric, gpu, count / gpu);
        else
            std::printf("\"%s\":null,\"logical_texels_per_ns\":null}\n", metric);
        return valid;
    }
};

uint32_t number(std::string_view value, uint32_t minimum, uint32_t maximum)
{
    uint32_t parsed = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    require(result.ec == std::errc{} && result.ptr == value.data() + value.size() &&
            parsed >= minimum && parsed <= maximum, "option value outside its bounded range");
    return parsed;
}

} // namespace

int main(int argc, char **argv)
{
    try {
        // Finish each sample/JSON line before a later callback can write to
        // the same captured log; all reporting is outside measured regions.
        require(std::setvbuf(stdout, nullptr, _IOLBF, 0) == 0, "cannot line-buffer benchmark output");
        uint32_t samples = 9, warmup = 3, limit = max_elements;
        TimingMode timing_mode = TimingMode::vulkan_queries;
        bool correctness_only = false, timing_options = false;
        for (int index = 1; index < argc; ++index) {
            const std::string_view option(argv[index]);
            if (option == "--help") {
                std::puts("Opt-in native RGB32/RGBA32 compute benchmark, with a separate correctness-only probe mode.\n"
                          "Options: --samples 3..31 (9), --warmup 1..8 (3), --max-elements 4099..1048579 (1048579).\n"
                          "--timing vulkan-queries (default) or metal-whole-submit (requires MESA_KK_DEBUG=submit_timing).\n"
                          "--correctness-only runs one zero-active control and one full-data dispatch per format/size, without timing.\n"
                          "Do not combine correctness-only with --timing/--samples/--warmup. Linux requires verified Venus.\n"
                          "Query spans cover dispatch; Metal spans cover the entire GPU submit, without query commands.\n"
                          "Controls, warmups, and retained samples must all be valid; exact callback coverage is required.\n"
                          "Invalid query ticks return 2; invalid callbacks/correctness/API return 1. No speed thresholds.\n"
                          "CPU view creation uses fixed batches of 32; total and amortized costs exclude destruction.\n"
                          "CPU spans are instrumented; amortized creation is not single-call latency or physical bandwidth.");
                return 0;
            }
            if (option == "--correctness-only") {
                correctness_only = true;
                continue;
            }
            require(index + 1 < argc, "missing option value");
            const std::string_view value(argv[++index]);
            timing_options |= option == "--samples" || option == "--warmup" || option == "--timing";
            if (option == "--samples") samples = number(value, 3, 31);
            else if (option == "--warmup") warmup = number(value, 1, 8);
            else if (option == "--max-elements") limit = number(value, 4099, max_elements);
            else if (option == "--timing") {
                require(value == "vulkan-queries" || value == "metal-whole-submit", "unknown timing mode");
                timing_mode = value == "metal-whole-submit" ? TimingMode::metal_whole_submit : TimingMode::vulkan_queries;
            }
            else throw std::runtime_error("unknown option: " + std::string(option));
        }
        require(!correctness_only || !timing_options, "correctness-only cannot use timing or repetition options");
        if (correctness_only) timing_mode = TimingMode::correctness_only;
        Context api(timing_mode);
        bool valid = true;
        unsigned expected_submissions = 0;
        for (uint32_t count : {4099u, 16387u, 262147u, max_elements}) {
            if (count > limit) continue;
            expected_submissions += 6 * (correctness_only ? 2 : 1 + warmup + samples);
            for (uint32_t kind = 0; kind < 3; ++kind) {
                Workload rgb(api, kind, count, 3), rgba(api, kind, count, 4);
                rgb.execute(false, true);
                rgba.execute(false, true);
                if (correctness_only) {
                    rgb.execute(false);
                    rgba.execute(false);
                    continue;
                }
                for (uint32_t iteration = 0; iteration < warmup + samples; ++iteration) {
                    const bool retain = iteration >= warmup;
                    if (iteration % 2) { rgba.execute(retain); rgb.execute(retain); }
                    else { rgb.execute(retain); rgba.execute(retain); }
                }
                valid = rgb.report(warmup) && valid;
                valid = rgba.report(warmup) && valid;
            }
        }
        require(api.submissions == expected_submissions, "workload submission coverage mismatch");
        if (api.submit_timing)
            require(api.submit_timing->finish() == expected_submissions, "whole-submit callback coverage mismatch");
        if (correctness_only) {
            std::printf("RGB32_CORRECTNESS_PASS submissions=%u timing=not-measured\n", api.submissions);
            return 0;
        }
        valid = valid && api.invalid_gpu == 0;
        std::printf("%s correctness=pass timing=%s mode=%s submissions=%u invalid_gpu_samples=%u\n",
                    valid ? "RGB32_BENCH_PASS" : "RGB32_BENCH_INVALID", valid ? "valid" : "invalid",
                    timing_name(timing_mode), api.submissions, api.invalid_gpu);
        return valid ? 0 : 2;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "RGB32_BENCH_FAIL %s\n", error.what());
        return 1;
    }
}
