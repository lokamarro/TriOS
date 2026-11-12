// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Imports CPU-written hardware buffers into the product's Vulkan HAL.
 *
 * Each case allocates a buffer of one format and width, fills it through the
 * CPU mapping, rewrites a rectangle of it through a second lock of only that
 * rectangle, imports it into Vulkan as a dedicated allocation and copies the
 * image into a host-visible buffer, which must hold what the CPU wrote. The
 * widths include the ones whose tightly packed rows are not a multiple of the
 * 16 bytes Metal reads a texture over a buffer at. A run that reports every
 * case imported and every byte read back is a proof that the row pitch gralloc
 * reports is a pitch the driver can import and the renderer reads the rows at.
 *
 * Usage: kernarium_vk_ahb_import [width...]
 * With no argument it runs a set of widths for each format. The exit status is
 * 0 when every case that could be allocated passed.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <android/hardware_buffer.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>

namespace {

struct FormatCase {
    uint32_t ahbFormat;
    const char* name;
    uint32_t bytesPerPixel;
};

const FormatCase kFormats[] = {
        {AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8", 4},
        {AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM, "R5G6B5", 2},
        {AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16F", 8},
        {AHARDWAREBUFFER_FORMAT_R8_UNORM, "R8", 1},
};

const uint32_t kDefaultWidths[] = {640, 642, 333, 100, 17, 1};
constexpr uint32_t kHeight = 48;

// What the CPU writes at a byte of a row.
uint8_t pattern(uint32_t x, uint32_t y, uint32_t byte) {
    return static_cast<uint8_t>(x * 7 + y * 13 + byte * 31 + 1);
}

// The part of a buffer a second, partial lock rewrites.
struct Update {
    uint32_t x, y, width, height;
    bool contains(uint32_t px, uint32_t py) const {
        return px >= x && px < x + width && py >= y && py < y + height;
    }
};

// What a byte holds once the update has been written over the pattern.
uint8_t expected(const Update& update, uint32_t x, uint32_t y, uint32_t byte) {
    return update.contains(x, y) ? static_cast<uint8_t>(pattern(x, y, byte) ^ 0x5a) : pattern(x, y, byte);
}

struct Device {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory = {};
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID getProperties = nullptr;
};

bool check(VkResult result, const char* what) {
    if (result == VK_SUCCESS) return true;
    printf("    %s -> %d\n", what, static_cast<int>(result));
    return false;
}

bool createDevice(Device* d) {
    VkApplicationInfo app = {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    if (!check(vkCreateInstance(&ici, nullptr, &d->instance), "vkCreateInstance")) return false;

    uint32_t count = 1;
    VkResult r = vkEnumeratePhysicalDevices(d->instance, &count, &d->physical);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || count == 0) {
        printf("    no physical device (%d)\n", static_cast<int>(r));
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties(d->physical, &d->memory);

    const char* extensions[] = {
            "VK_ANDROID_external_memory_android_hardware_buffer",
            "VK_KHR_sampler_ycbcr_conversion",
            "VK_KHR_external_memory",
            "VK_KHR_dedicated_allocation",
            "VK_KHR_get_memory_requirements2",
            "VK_KHR_bind_memory2",
            "VK_KHR_maintenance1",
            "VK_EXT_queue_family_foreign",
    };
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcr = {};
    ycbcr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    ycbcr.samplerYcbcrConversion = VK_TRUE;
    VkDeviceCreateInfo dci = {};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &ycbcr;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]);
    dci.ppEnabledExtensionNames = extensions;
    if (!check(vkCreateDevice(d->physical, &dci, nullptr, &d->device), "vkCreateDevice")) return false;

    vkGetDeviceQueue(d->device, 0, 0, &d->queue);
    d->getProperties = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
            vkGetDeviceProcAddr(d->device, "vkGetAndroidHardwareBufferPropertiesANDROID"));
    if (!d->getProperties) {
        printf("    vkGetAndroidHardwareBufferPropertiesANDROID is not exposed\n");
        return false;
    }

    VkCommandPoolCreateInfo pci = {};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = 0;
    return check(vkCreateCommandPool(d->device, &pci, nullptr, &d->pool), "vkCreateCommandPool");
}

bool findMemoryType(const Device& d, uint32_t bits, VkMemoryPropertyFlags want, uint32_t* index) {
    for (uint32_t i = 0; i < d.memory.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (d.memory.memoryTypes[i].propertyFlags & want) == want) {
            *index = i;
            return true;
        }
    }
    return false;
}

// Writes the pattern through the CPU mapping, row by row at the stride gralloc reports.
bool fill(AHardwareBuffer* buffer, const AHardwareBuffer_Desc& desc, uint32_t bytesPerPixel) {
    void* mapped = nullptr;
    if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &mapped) != 0 ||
        !mapped) {
        printf("    AHardwareBuffer_lock failed\n");
        return false;
    }
    uint8_t* bytes = static_cast<uint8_t*>(mapped);
    memset(bytes, 0xee, static_cast<size_t>(desc.stride) * desc.height * bytesPerPixel);
    for (uint32_t y = 0; y < desc.height; y++) {
        for (uint32_t x = 0; x < desc.width; x++) {
            for (uint32_t b = 0; b < bytesPerPixel; b++) {
                bytes[(static_cast<size_t>(y) * desc.stride + x) * bytesPerPixel + b] = pattern(x, y, b);
            }
        }
    }
    AHardwareBuffer_unlock(buffer, nullptr);
    return true;
}

// Rewrites a rectangle through a second CPU lock of just that rectangle.
bool rewrite(AHardwareBuffer* buffer, const AHardwareBuffer_Desc& desc, uint32_t bytesPerPixel,
             const Update& update) {
    const ARect rect = {static_cast<int32_t>(update.x), static_cast<int32_t>(update.y),
                        static_cast<int32_t>(update.x + update.width),
                        static_cast<int32_t>(update.y + update.height)};
    void* mapped = nullptr;
    if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, &rect, &mapped) != 0 || !mapped) {
        printf("    AHardwareBuffer_lock of a rectangle failed\n");
        return false;
    }
    uint8_t* bytes = static_cast<uint8_t*>(mapped);
    for (uint32_t y = update.y; y < update.y + update.height; y++) {
        for (uint32_t x = update.x; x < update.x + update.width; x++) {
            for (uint32_t b = 0; b < bytesPerPixel; b++) {
                bytes[(static_cast<size_t>(y) * desc.stride + x) * bytesPerPixel + b] =
                        expected(update, x, y, b);
            }
        }
    }
    AHardwareBuffer_unlock(buffer, nullptr);
    return true;
}

// Imports the buffer, copies its image into a host-visible buffer and compares it with the pattern
// as the update left it.
bool importAndRead(const Device& d, AHardwareBuffer* buffer, const AHardwareBuffer_Desc& desc,
                   uint32_t bytesPerPixel, const Update& update) {
    VkAndroidHardwareBufferFormatPropertiesANDROID format = {};
    format.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
    VkAndroidHardwareBufferPropertiesANDROID props = {};
    props.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    props.pNext = &format;
    if (!check(d.getProperties(d.device, buffer, &props), "vkGetAndroidHardwareBufferPropertiesANDROID")) {
        return false;
    }
    if (format.format == VK_FORMAT_UNDEFINED) {
        printf("    the buffer has only an external format\n");
        return false;
    }

    VkExternalMemoryImageCreateInfo external = {};
    external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &external;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format.format;
    ici.extent = {desc.width, desc.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool ok = false;

    do {
        if (!check(vkCreateImage(d.device, &ici, nullptr, &image), "vkCreateImage")) break;

        VkImportAndroidHardwareBufferInfoANDROID import = {};
        import.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
        import.buffer = buffer;
        VkMemoryDedicatedAllocateInfo dedicated = {};
        dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.pNext = &import;
        dedicated.image = image;
        uint32_t type = 0;
        if (!findMemoryType(d, props.memoryTypeBits, 0, &type)) {
            printf("    no memory type for the buffer\n");
            break;
        }
        VkMemoryAllocateInfo mai = {};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.pNext = &dedicated;
        mai.allocationSize = props.allocationSize;
        mai.memoryTypeIndex = type;
        if (!check(vkAllocateMemory(d.device, &mai, nullptr, &imageMemory), "vkAllocateMemory")) break;
        if (!check(vkBindImageMemory(d.device, image, imageMemory, 0), "vkBindImageMemory")) break;

        const VkDeviceSize readbackSize = static_cast<VkDeviceSize>(desc.width) * desc.height * bytesPerPixel;
        VkBufferCreateInfo bci = {};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = readbackSize;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!check(vkCreateBuffer(d.device, &bci, nullptr, &readback), "vkCreateBuffer")) break;
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(d.device, readback, &requirements);
        VkMemoryAllocateInfo rai = {};
        rai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        rai.allocationSize = requirements.size;
        if (!findMemoryType(d, requirements.memoryTypeBits,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            &rai.memoryTypeIndex)) {
            printf("    no host-visible memory type\n");
            break;
        }
        if (!check(vkAllocateMemory(d.device, &rai, nullptr, &readbackMemory), "vkAllocateMemory (readback)")) break;
        if (!check(vkBindBufferMemory(d.device, readback, readbackMemory, 0), "vkBindBufferMemory")) break;

        VkCommandBufferAllocateInfo cai = {};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = d.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (!check(vkAllocateCommandBuffers(d.device, &cai, &commands), "vkAllocateCommandBuffers")) break;
        VkCommandBufferBeginInfo begin = {};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!check(vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer")) break;

        // Take the image from the foreign owner without discarding what the CPU wrote.
        VkImageMemoryBarrier acquire = {};
        acquire.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        acquire.srcAccessMask = 0;
        acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        acquire.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        acquire.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        acquire.dstQueueFamilyIndex = 0;
        acquire.image = image;
        acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &acquire);
        VkBufferImageCopy region = {};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {desc.width, desc.height, 1};
        vkCmdCopyImageToBuffer(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &region);
        VkBufferMemoryBarrier visible = {};
        visible.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        visible.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        visible.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        visible.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        visible.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        visible.buffer = readback;
        visible.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                             &visible, 0, nullptr);
        if (!check(vkEndCommandBuffer(commands), "vkEndCommandBuffer")) break;

        VkFenceCreateInfo fci = {};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (!check(vkCreateFence(d.device, &fci, nullptr, &fence), "vkCreateFence")) break;
        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        if (!check(vkQueueSubmit(d.queue, 1, &submit, fence), "vkQueueSubmit")) break;
        if (!check(vkWaitForFences(d.device, 1, &fence, VK_TRUE, 30ull * 1000 * 1000 * 1000), "vkWaitForFences")) {
            break;
        }

        void* mapped = nullptr;
        if (!check(vkMapMemory(d.device, readbackMemory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory")) break;
        const uint8_t* bytes = static_cast<const uint8_t*>(mapped);
        uint64_t wrong = 0;
        uint32_t firstX = 0, firstY = 0;
        uint8_t firstGot = 0, firstWant = 0;
        for (uint32_t y = 0; y < desc.height; y++) {
            for (uint32_t x = 0; x < desc.width; x++) {
                for (uint32_t b = 0; b < bytesPerPixel; b++) {
                    if (bytes[(static_cast<size_t>(y) * desc.width + x) * bytesPerPixel + b] != expected(update, x, y, b)) {
                        if (!wrong) {
                            firstX = x;
                            firstY = y;
                            firstGot = bytes[(static_cast<size_t>(y) * desc.width + x) * bytesPerPixel + b];
                            firstWant = expected(update, x, y, b);
                        }
                        wrong++;
                    }
                }
            }
        }
        vkUnmapMemory(d.device, readbackMemory);
        if (wrong) {
            printf("    %" PRIu64 " bytes read back differ from what the CPU wrote, first at (%u,%u): read %u, wrote %u\n",
                   wrong, firstX, firstY, firstGot, firstWant);
            break;
        }
        ok = true;
    } while (false);

    if (fence) vkDestroyFence(d.device, fence, nullptr);
    if (commands) vkFreeCommandBuffers(d.device, d.pool, 1, &commands);
    if (readback) vkDestroyBuffer(d.device, readback, nullptr);
    if (readbackMemory) vkFreeMemory(d.device, readbackMemory, nullptr);
    if (image) vkDestroyImage(d.device, image, nullptr);
    if (imageMemory) vkFreeMemory(d.device, imageMemory, nullptr);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    uint32_t widths[32];
    size_t widthCount = 0;
    for (int i = 1; i < argc && widthCount < sizeof(widths) / sizeof(widths[0]); i++) {
        widths[widthCount++] = static_cast<uint32_t>(strtoul(argv[i], nullptr, 0));
    }
    if (widthCount == 0) {
        for (uint32_t width : kDefaultWidths) widths[widthCount++] = width;
    }

    Device device;
    if (!createDevice(&device)) {
        printf("FAIL: no Vulkan device to import into\n");
        return 2;
    }

    int run = 0, failed = 0, skipped = 0;
    for (const FormatCase& format : kFormats) {
        for (size_t w = 0; w < widthCount; w++) {
            const uint32_t width = widths[w];
            AHardwareBuffer_Desc want = {};
            want.width = width;
            want.height = kHeight;
            want.layers = 1;
            want.format = format.ahbFormat;
            want.usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                         AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
            AHardwareBuffer* buffer = nullptr;
            if (AHardwareBuffer_allocate(&want, &buffer) != 0 || !buffer) {
                printf("%-14s %4ux%-3u SKIP: the buffer cannot be allocated\n", format.name, width, kHeight);
                skipped++;
                continue;
            }
            AHardwareBuffer_Desc got;
            AHardwareBuffer_describe(buffer, &got);
            printf("%-14s %4ux%-3u stride %u (%u bytes)\n", format.name, width, kHeight, got.stride,
                   got.stride * format.bytesPerPixel);
            run++;
            // A rectangle off the left and top edges, so that a partial lock has to find where its first
            // row starts and where each row after it does.
            Update update = {0, 0, 0, 0};
            if (got.width > 4 && got.height > 6) {
                update.x = got.width / 5 + 1;
                update.y = 3;
                update.width = got.width - update.x - 1;
                update.height = got.height - 6;
            }
            bool ok = fill(buffer, got, format.bytesPerPixel) &&
                      (update.width == 0 || rewrite(buffer, got, format.bytesPerPixel, update)) &&
                      importAndRead(device, buffer, got, format.bytesPerPixel, update);
            printf("  %s\n", ok ? "PASS" : "FAIL");
            if (!ok) failed++;
            AHardwareBuffer_release(buffer);
        }
    }

    printf("%s: %d cases imported, %d failed, %d not allocated\n", failed ? "FAIL" : "PASS", run, failed, skipped);
    return failed ? 1 : 0;
}
