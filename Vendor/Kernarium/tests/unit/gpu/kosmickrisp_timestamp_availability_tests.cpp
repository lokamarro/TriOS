// SPDX-License-Identifier: GPL-2.0-or-later
#include "require.hpp"
#include "vulkan_draw_test.hpp"

#include <vulkan/vulkan_core.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using uml::gpu::test::check;
using uml::gpu::test::instance_function;
using uml::gpu::test::memory_type;
using uml::gpu::test::structure;
using uml::testing::require;

struct Result {
    uint64_t value, available;
    bool operator==(const Result &) const = default;
};

constexpr uint64_t poison = 0xa5a5a5a5a5a5a5a5ull;
constexpr VkDeviceSize buffer_bytes = 8 * 1024 * 1024;
constexpr uint32_t fills = 64;
constexpr uint32_t trials = 32;

struct Context {
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool commands = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::array<VkQueryPool, 2> pools{};
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    PFN_vkGetDeviceProcAddr get_device = nullptr;

    template <typename T>
    T device_function(const char *name) const
    {
        auto function = reinterpret_cast<T>(get_device(device, name));
        require(function != nullptr, name);
        return function;
    }

#define CALL(name, ...) device_function<PFN_vk##name>("vk" #name)(__VA_ARGS__)
#define CHECK(name, ...) check(CALL(name, __VA_ARGS__), "vk" #name)

    Context()
    {
        try { initialize(); }
        catch (...) { release(); throw; }
    }
    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;
    ~Context() { release(); }

    void release()
    {
        if (device) {
            if (queue) (void)CALL(QueueWaitIdle, queue);
            if (commands) CALL(DestroyCommandPool, device, commands, nullptr);
            if (fence) CALL(DestroyFence, device, fence, nullptr);
            for (auto pool : pools)
                if (pool) CALL(DestroyQueryPool, device, pool, nullptr);
            if (view) CALL(DestroyImageView, device, view, nullptr);
            if (image) CALL(DestroyImage, device, image, nullptr);
            if (image_memory) CALL(FreeMemory, device, image_memory, nullptr);
            if (mapped) CALL(UnmapMemory, device, memory);
            if (buffer) CALL(DestroyBuffer, device, buffer, nullptr);
            if (memory) CALL(FreeMemory, device, memory, nullptr);
            CALL(DestroyDevice, device, nullptr);
        }
        if (instance)
            instance_function<PFN_vkDestroyInstance>(instance, "vkDestroyInstance")(instance, nullptr);
    }

    void initialize()
    {
        auto application = structure<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
        application.pApplicationName = "timestamp availability regression";
        application.apiVersion = VK_API_VERSION_1_3;
        auto info = structure<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        info.pApplicationInfo = &application;
        check(instance_function<PFN_vkCreateInstance>(instance, "vkCreateInstance")(&info, nullptr, &instance),
              "vkCreateInstance");
        get_device = instance_function<PFN_vkGetDeviceProcAddr>(instance, "vkGetDeviceProcAddr");
        auto enumerate = instance_function<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices");
        uint32_t count = 0;
        check(enumerate(instance, &count, nullptr), "physical device count");
        require(count == 1, "expected one directly linked native provider");
        VkPhysicalDevice physical;
        check(enumerate(instance, &count, &physical), "physical device");
        auto driver = structure<VkPhysicalDeviceDriverProperties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES);
        auto properties = structure<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
        properties.pNext = &driver;
        instance_function<PFN_vkGetPhysicalDeviceProperties2>(instance, "vkGetPhysicalDeviceProperties2")(
            physical, &properties);
        require(driver.driverID == VK_DRIVER_ID_MESA_KOSMICKRISP, "unexpected native provider");

        auto v11 = structure<VkPhysicalDeviceVulkan11Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
        auto v12 = structure<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
        auto v13 = structure<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
        auto features = structure<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
        features.pNext = &v11; v11.pNext = &v12; v12.pNext = &v13;
        instance_function<PFN_vkGetPhysicalDeviceFeatures2>(instance, "vkGetPhysicalDeviceFeatures2")(physical, &features);
        require(v11.multiview && v12.hostQueryReset && v13.dynamicRendering,
                "native timestamp regression prerequisites unavailable");
        v11 = structure<VkPhysicalDeviceVulkan11Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES);
        v12 = structure<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
        v13 = structure<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
        v11.pNext = &v12; v12.pNext = &v13;
        v11.multiview = v12.hostQueryReset = v13.dynamicRendering = VK_TRUE;
        auto get_families = instance_function<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(instance, "vkGetPhysicalDeviceQueueFamilyProperties");
        get_families(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        get_families(physical, &count, families.data());
        uint32_t family = 0;
        for (; family < count; ++family)
            if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) && families[family].timestampValidBits)
                break;
        require(family < count, "timestamp graphics queue unavailable");
        auto enumerate_extensions = instance_function<PFN_vkEnumerateDeviceExtensionProperties>(instance, "vkEnumerateDeviceExtensionProperties");
        check(enumerate_extensions(physical, nullptr, &count, nullptr), "extension count");
        std::vector<VkExtensionProperties> extensions(count);
        check(enumerate_extensions(physical, nullptr, &count, extensions.data()), "extensions");
        std::vector<const char *> enabled;
        for (const auto &extension : extensions)
            if (std::strcmp(extension.extensionName, "VK_KHR_portability_subset") == 0)
                enabled.push_back("VK_KHR_portability_subset");
        const float priority = 1;
        auto queue_info = structure<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        auto device_info = structure<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
        device_info.pNext = &v11;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
        device_info.ppEnabledExtensionNames = enabled.data();
        check(instance_function<PFN_vkCreateDevice>(instance, "vkCreateDevice")(physical, &device_info, nullptr, &device),
              "vkCreateDevice");
        CALL(GetDeviceQueue, device, family, 0, &queue);
        auto command_info = structure<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        command_info.queueFamilyIndex = family;
        command_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        CHECK(CreateCommandPool, device, &command_info, nullptr, &commands);
        auto allocate = structure<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        allocate.commandPool = commands;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        CHECK(AllocateCommandBuffers, device, &allocate, &command);
        auto fence_info = structure<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        CHECK(CreateFence, device, &fence_info, nullptr, &fence);
        auto query_info = structure<VkQueryPoolCreateInfo>(VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
        query_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query_info.queryCount = 4;
        for (auto &pool : pools)
            CHECK(CreateQueryPool, device, &query_info, nullptr, &pool);
        auto buffer_info = structure<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        buffer_info.size = buffer_bytes;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        CHECK(CreateBuffer, device, &buffer_info, nullptr, &buffer);
        VkMemoryRequirements requirements;
        CALL(GetBufferMemoryRequirements, device, buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memory_properties;
        instance_function<PFN_vkGetPhysicalDeviceMemoryProperties>(instance, "vkGetPhysicalDeviceMemoryProperties")(
            physical, &memory_properties);
        constexpr auto flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        const auto host_type = memory_type(memory_properties, requirements.memoryTypeBits, flags);
        require(host_type.has_value(), "coherent native query-copy buffer unavailable");
        auto memory_info = structure<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        memory_info.allocationSize = requirements.size;
        memory_info.memoryTypeIndex = *host_type;
        CHECK(AllocateMemory, device, &memory_info, nullptr, &memory);
        CHECK(BindBufferMemory, device, buffer, memory, 0);
        CHECK(MapMemory, device, memory, 0, VK_WHOLE_SIZE, 0, &mapped);

        // A real color attachment starts a native render encoder, unlike an
        // empty attachmentless pass whose timestamps use the compute path.
        auto image_info = structure<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.extent = {16, 16, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 2;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        CHECK(CreateImage, device, &image_info, nullptr, &image);
        CALL(GetImageMemoryRequirements, device, image, &requirements);
        const auto device_type =
            memory_type(memory_properties, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        require(device_type.has_value(), "native color-attachment memory unavailable");
        memory_info.allocationSize = requirements.size;
        memory_info.memoryTypeIndex = *device_type;
        CHECK(AllocateMemory, device, &memory_info, nullptr, &image_memory);
        CHECK(BindImageMemory, device, image, image_memory, 0);
        auto view_info = structure<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        view_info.format = image_info.format;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
        CHECK(CreateImageView, device, &view_info, nullptr, &view);
        std::printf("provider=%s device=%s\n", driver.driverName, properties.properties.deviceName);
    }

    void begin()
    {
        // Host reset prevents an old submission's availability being mistaken
        // for this one before its GPU reset has executed.
        for (auto pool : pools) CALL(ResetQueryPool, device, pool, 0, 4);
        CHECK(ResetCommandBuffer, command, 0);
        auto info = structure<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        CHECK(BeginCommandBuffer, command, &info);
        for (auto pool : pools) CALL(CmdResetQueryPool, command, pool, 0, 4);
    }

    void busy(uint32_t seed)
    {
        auto barrier = structure<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
        barrier.srcAccessMask = barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        for (uint32_t i = 0; i < fills; ++i) {
            CALL(CmdFillBuffer, command, buffer, 0, buffer_bytes, seed + i);
            CALL(CmdPipelineBarrier, command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        }
    }

    void submit()
    {
        auto barrier = structure<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        CALL(CmdPipelineBarrier, command, VK_PIPELINE_STAGE_TRANSFER_BIT,
             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        CHECK(EndCommandBuffer, command);
        CHECK(ResetFences, device, 1, &fence);
        auto info = structure<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        info.commandBufferCount = 1;
        info.pCommandBuffers = &command;
        CHECK(QueueSubmit, queue, 1, &info, fence);
    }

    std::vector<Result> results(VkQueryPool pool, uint32_t count, bool wait)
    {
        std::vector<Result> values(count, {poison, poison});
        const VkResult status = CALL(GetQueryPoolResults, device, pool, 0, count,
            values.size() * sizeof(Result), values.data(), sizeof(Result),
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT |
                (wait ? VK_QUERY_RESULT_WAIT_BIT : 0));
        require(status == VK_SUCCESS || (!wait && status == VK_NOT_READY), "query retrieval failed");
        for (const auto &value : values) {
            require(value.available <= 1, "availability must be normalized");
            require(value.available || value.value == poison, "unavailable query modified result");
            require(!wait || value.available, "WAIT returned unavailable query");
        }
        return values;
    }

    void availability(uint32_t mode, uint32_t trial)
    {
        begin();
        const uint32_t pool_count = mode % 2 ? 2 : 1;
        const uint32_t query_count = mode >= 2 ? 4 : 2;
        if (mode >= 2) {
            auto rendering = structure<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
            rendering.renderArea.extent = {1, 1};
            rendering.layerCount = 1;
            rendering.viewMask = 3;
            auto attachment = structure<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
            if (mode == 3) {
                auto barrier = structure<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
                barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = image;
                barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
                CALL(CmdPipelineBarrier, command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
                attachment.imageView = view;
                attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                attachment.clearValue.color.float32[0] = 1;
                rendering.renderArea.extent = {16, 16};
                rendering.layerCount = 2;
                rendering.colorAttachmentCount = 1;
                rendering.pColorAttachments = &attachment;
            }
            CALL(CmdBeginRendering, command, &rendering);
            for (uint32_t p = 0; p < pool_count; ++p) {
                CALL(CmdWriteTimestamp, command, mode == 3 ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                                         : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pools[p], 0);
                CALL(CmdWriteTimestamp, command, mode == 3 ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                                         : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pools[p], 2);
            }
            CALL(CmdEndRendering, command);
            busy(trial);
        } else {
            for (uint32_t p = 0; p < pool_count; ++p)
                CALL(CmdWriteTimestamp, command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pools[p], 0);
            busy(trial);
            for (uint32_t p = 0; p < pool_count; ++p)
                CALL(CmdWriteTimestamp, command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pools[p], 1);
        }
        submit();
        std::array<std::vector<Result>, 2> early, waited, available;
        for (uint32_t p = 0; p < pool_count; ++p)
            early[p] = results(pools[p], query_count, false);
        // Crucially this WAIT is issued before waiting for submission completion.
        for (uint32_t p = 0; p < pool_count; ++p)
            waited[p] = results(pools[p], query_count, true);
        for (uint32_t p = 0; p < pool_count; ++p)
            available[p] = results(pools[p], query_count, false);
        CHECK(WaitForFences, device, 1, &fence, VK_TRUE, 10'000'000'000ull);
        require(static_cast<const uint32_t *>(mapped)[0] == trial + fills - 1,
                "GPU workload did not complete");
        for (uint32_t p = 0; p < pool_count; ++p) {
            const auto final = results(pools[p], query_count, false);
            for (uint32_t q = 0; q < query_count; ++q) {
                if (final[q].available != 1 || waited[p][q] != final[q] ||
                    available[p][q] != final[q] ||
                    (early[p][q].available && early[p][q] != final[q])) {
                    std::fprintf(stderr, "mode=%u trial=%u pool=%u query=%u early=[%llu,%llu] wait=[%llu,%llu] available=[%llu,%llu] final=[%llu,%llu]\n",
                        mode, trial, p, q,
                        static_cast<unsigned long long>(early[p][q].value),
                        static_cast<unsigned long long>(early[p][q].available),
                        static_cast<unsigned long long>(waited[p][q].value),
                        static_cast<unsigned long long>(waited[p][q].available),
                        static_cast<unsigned long long>(available[p][q].value),
                        static_cast<unsigned long long>(available[p][q].available),
                        static_cast<unsigned long long>(final[q].value),
                        static_cast<unsigned long long>(final[q].available));
                    throw std::runtime_error("availability exposed a stale timestamp result");
                }
                if (mode >= 2 && q % 2)
                    require(final[q].value == 0, "multiview extra query was not zeroed");
            }
        }
    }

    void reset_copy()
    {
        begin();
        CALL(CmdWriteTimestamp, command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pools[0], 0);
        CALL(CmdResetQueryPool, command, pools[0], 0, 4);
        CALL(CmdWriteTimestamp, command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pools[0], 0);
        CALL(CmdCopyQueryPoolResults, command, pools[0], 0, 1, buffer, 0, sizeof(Result),
             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        submit();
        CHECK(WaitForFences, device, 1, &fence, VK_TRUE, 10'000'000'000ull);
        const auto value = results(pools[0], 1, false);
        require(value[0].available == 1 && *static_cast<const Result *>(mapped) == value[0],
                "reset/copy result differs from post-fence host value");
        const auto all = results(pools[0], 4, false);
        for (uint32_t q = 1; q < all.size(); ++q)
            require(all[q].available == 0, "reset query was republished by an old resolve");
    }
};

#undef CHECK
#undef CALL
} // namespace

int main()
{
    try {
        Context context;
        for (uint32_t mode = 0; mode < 4; ++mode)
            for (uint32_t trial = 0; trial < trials; ++trial)
                context.availability(mode, trial);
        context.reset_copy();
        std::puts("TIMESTAMP_AVAILABILITY_PASS trials=128 modes=single,two-pools,multiview,render-multiview-two-pools reset-copy=pass sampling-accuracy=not-tested");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "TIMESTAMP_AVAILABILITY_FAIL %s\n", error.what());
        return 1;
    }
}
