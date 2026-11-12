// SPDX-License-Identifier: GPL-2.0-or-later
#include "vulkan_draw_test.hpp"

#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_metal.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using uml::gpu::test::check;
using uml::gpu::test::instance_function;
using uml::gpu::test::memory_type;
using uml::gpu::test::structure;
struct Context {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    VkCommandPool pool{};
    VkCommandBuffer command{};
    VkBuffer buffer{};
    VkRenderPass render_pass{};
    VkFramebuffer framebuffer{};
    VkImageView producer_view{};
    std::vector<VkImage> images;
    std::vector<VkDeviceMemory> allocations;
    PFN_vkGetDeviceProcAddr get_device{};
    VkPhysicalDeviceMemoryProperties memory_properties{};

    template <typename T> T function(const char *name)
    {
        auto result = reinterpret_cast<T>(get_device(device, name));
        if (!result) throw std::runtime_error(name);
        return result;
    }
#define CALL(name, ...) function<PFN_vk##name>("vk" #name)(__VA_ARGS__)
#define CHECK(name, ...) check(CALL(name, __VA_ARGS__), "vk" #name)
    ~Context()
    {
        if (device) {
            if (queue) (void)CALL(QueueWaitIdle, queue);
            if (pool) CALL(DestroyCommandPool, device, pool, nullptr);
            if (framebuffer) CALL(DestroyFramebuffer, device, framebuffer, nullptr);
            if (render_pass) CALL(DestroyRenderPass, device, render_pass, nullptr);
            if (producer_view) CALL(DestroyImageView, device, producer_view, nullptr);
            for (auto image : images) CALL(DestroyImage, device, image, nullptr);
            if (buffer) CALL(DestroyBuffer, device, buffer, nullptr);
            for (auto memory : allocations) CALL(FreeMemory, device, memory, nullptr);
            CALL(DestroyDevice, device, nullptr);
        }
        if (instance)
            instance_function<PFN_vkDestroyInstance>(instance, "vkDestroyInstance")(instance, nullptr);
    }
    void initialize()
    {
        auto app = structure<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
        app.apiVersion = VK_API_VERSION_1_3;
        auto info = structure<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        info.pApplicationInfo = &app;
        check(instance_function<PFN_vkCreateInstance>(instance, "vkCreateInstance")(&info, nullptr, &instance), "create instance");
        uint32_t count = 1;
        check(instance_function<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices")(instance, &count, &physical), "physical device");
        instance_function<PFN_vkGetPhysicalDeviceMemoryProperties>(instance, "vkGetPhysicalDeviceMemoryProperties")(physical, &memory_properties);
        float priority = 1;
        auto qi = structure<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        const char *extension = VK_EXT_EXTERNAL_MEMORY_METAL_EXTENSION_NAME;
        auto di = structure<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &qi;
        di.enabledExtensionCount = 1;
        di.ppEnabledExtensionNames = &extension;
        check(instance_function<PFN_vkCreateDevice>(instance, "vkCreateDevice")(physical, &di, nullptr, &device), "create device");
        get_device = instance_function<PFN_vkGetDeviceProcAddr>(instance, "vkGetDeviceProcAddr");
        CALL(GetDeviceQueue, device, 0, 0, &queue);
        auto pi = structure<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        CHECK(CreateCommandPool, device, &pi, nullptr, &pool);
        auto ci = structure<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        ci.commandPool = pool;
        ci.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ci.commandBufferCount = 1;
        CHECK(AllocateCommandBuffers, device, &ci, &command);
    }
    VkDeviceMemory allocate(VkMemoryRequirements requirements, VkMemoryPropertyFlags flags, const void *next = nullptr)
    {
        auto info = structure<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        info.pNext = next;
        info.allocationSize = requirements.size;
        const auto type = memory_type(memory_properties, requirements.memoryTypeBits, flags);
        if (!type) throw std::runtime_error("memory type unavailable");
        info.memoryTypeIndex = *type;
        VkDeviceMemory memory{};
        CHECK(AllocateMemory, device, &info, nullptr, &memory);
        allocations.push_back(memory);
        return memory;
    }
    void run(VkFormat format, uint32_t width, uint32_t height)
    {
        const VkDeviceSize bytes = VkDeviceSize(width) * height * 4;
        auto bi = structure<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        bi.size = bytes * 2;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        CHECK(CreateBuffer, device, &bi, nullptr, &buffer);
        VkMemoryRequirements br{};
        CALL(GetBufferMemoryRequirements, device, buffer, &br);
        auto buffer_memory = allocate(br, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        CHECK(BindBufferMemory, device, buffer, buffer_memory, 0);
        void *mapped{};
        CHECK(MapMemory, device, buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
        auto *pixels = static_cast<uint32_t *>(mapped);
        std::fill(pixels, pixels + width * height, 0u);
        std::fill(pixels + width * height, pixels + 2 * width * height, 0xa5a5a5a5u);

        auto external = structure<VkExternalMemoryImageCreateInfo>(VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT;
        auto ii = structure<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
        ii.pNext = &external;
        ii.flags = VK_IMAGE_CREATE_ALIAS_BIT;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = format;
        ii.extent = {width, height, 1};
        ii.mipLevels = ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        VkImage producer{}, consumer{};
        CHECK(CreateImage, device, &ii, nullptr, &producer);
        images.push_back(producer);
        // A compositor imports the same storage for sampling and may add views.
        ii.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        CHECK(CreateImage, device, &ii, nullptr, &consumer);
        images.push_back(consumer);
        VkMemoryRequirements pr{}, cr{};
        CALL(GetImageMemoryRequirements, device, producer, &pr);
        CALL(GetImageMemoryRequirements, device, consumer, &cr);
        pr.size = std::max(pr.size, cr.size);
        pr.memoryTypeBits &= cr.memoryTypeBits;
        auto export_info = structure<VkExportMemoryAllocateInfo>(VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
        export_info.handleTypes = external.handleTypes;
        auto producer_memory = allocate(pr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &export_info);
        auto handle_info = structure<VkMemoryGetMetalHandleInfoEXT>(VK_STRUCTURE_TYPE_MEMORY_GET_METAL_HANDLE_INFO_EXT);
        handle_info.memory = producer_memory;
        handle_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT;
        void *handle{};
        CHECK(GetMemoryMetalHandleEXT, device, &handle_info, &handle);
        auto import_info = structure<VkImportMemoryMetalHandleInfoEXT>(VK_STRUCTURE_TYPE_IMPORT_MEMORY_METAL_HANDLE_INFO_EXT);
        import_info.handleType = handle_info.handleType;
        import_info.handle = handle;
        auto consumer_memory = allocate(pr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &import_info);
        CHECK(BindImageMemory, device, producer, producer_memory, 0);
        CHECK(BindImageMemory, device, consumer, consumer_memory, 0);

        auto vi = structure<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        vi.image = producer;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        CHECK(CreateImageView, device, &vi, nullptr, &producer_view);
        VkAttachmentDescription attachment{};
        attachment.format = format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkAttachmentReference color{0, VK_IMAGE_LAYOUT_GENERAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color;
        auto ri = structure<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
        ri.attachmentCount = 1;
        ri.pAttachments = &attachment;
        ri.subpassCount = 1;
        ri.pSubpasses = &subpass;
        CHECK(CreateRenderPass, device, &ri, nullptr, &render_pass);
        auto fi = structure<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
        fi.renderPass = render_pass;
        fi.attachmentCount = 1;
        fi.pAttachments = &producer_view;
        fi.width = width;
        fi.height = height;
        fi.layers = 1;
        CHECK(CreateFramebuffer, device, &fi, nullptr, &framebuffer);
        auto begin = structure<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        CHECK(BeginCommandBuffer, command, &begin);
        std::array<VkImageMemoryBarrier, 2> barriers{};
        for (unsigned i = 0; i < barriers.size(); ++i) {
            auto &barrier = barriers[i];
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = i ? consumer : producer;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        CALL(CmdPipelineBarrier, command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
             0, 0, nullptr, 0, nullptr, uint32_t(barriers.size()), barriers.data());
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        VkClearValue initial{};
        auto render = structure<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
        render.renderPass = render_pass;
        render.framebuffer = framebuffer;
        render.renderArea.extent = {width, height};
        render.clearValueCount = 1;
        render.pClearValues = &initial;
        CALL(CmdBeginRenderPass, command, &render, VK_SUBPASS_CONTENTS_INLINE);
        for (uint32_t y = 0; y < height; y += 16) {
            for (uint32_t x = 0; x < width; x += 16) {
                const uint32_t red = (x / 16) & 1;
                const uint32_t green = (y / 16) & 1;
                const uint32_t blue = red ^ green;
                VkClearAttachment clear{};
                clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                clear.clearValue.color = {{float(red), float(green), float(blue), 1.0f}};
                VkClearRect rect{{{int32_t(x), int32_t(y)}, {std::min(16u, width - x), std::min(16u, height - y)}}, 0, 1};
                CALL(CmdClearAttachments, command, 1, &clear, 1, &rect);
                const uint32_t value = 0xff000000u | (green * 255 << 8) |
                    (format == VK_FORMAT_B8G8R8A8_UNORM ? (red * 255 << 16) | blue * 255 : (blue * 255 << 16) | red * 255);
                for (uint32_t py = y; py < y + rect.rect.extent.height; ++py)
                    for (uint32_t px = x; px < x + rect.rect.extent.width; ++px)
                        pixels[py * width + px] = value;
            }
        }
        CALL(CmdEndRenderPass, command);
        auto memory_barrier = structure<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
        memory_barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        memory_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        CALL(CmdPipelineBarrier, command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
             0, 1, &memory_barrier, 0, nullptr, 0, nullptr);
        copy.bufferOffset = bytes;
        CALL(CmdCopyImageToBuffer, command, consumer, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &copy);
        memory_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        memory_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        CALL(CmdPipelineBarrier, command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
             0, 1, &memory_barrier, 0, nullptr, 0, nullptr);
        CHECK(EndCommandBuffer, command);
        auto submit = structure<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        CHECK(QueueSubmit, queue, 1, &submit, VK_NULL_HANDLE);
        CHECK(QueueWaitIdle, queue);
        uint32_t mismatches = 0;
        for (uint32_t i = 0; i < width * height; ++i)
            mismatches += pixels[i] != pixels[width * height + i];
        CALL(UnmapMemory, device, buffer_memory);
        if (mismatches)
            throw std::runtime_error("external image alias corrupted " + std::to_string(mismatches) + " pixels");
    }
};
}
int main()
{
    try {
        for (auto format : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
            for (auto extent : {std::array{256u, 256u}, std::array{257u, 131u}}) {
                Context context;
                context.initialize();
                context.run(format, extent[0], extent[1]);
            }
        }
        std::puts("External Metal heap images preserve pixels across compositor usage changes");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
