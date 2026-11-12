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
 * Presents frames through the product's Vulkan HAL.
 *
 * The tool owns a SurfaceFlinger layer, wraps it in a VkSurfaceKHR through
 * VK_KHR_android_surface, and clears and presents a swapchain whose images
 * the platform loader allocates from gralloc and hands to the driver through
 * VK_ANDROID_native_buffer. A run that reports presented frames is a
 * window-system proof: the driver implements the Android image extension,
 * the gralloc buffers it imports are renderable, and the compositor accepts
 * what vkQueuePresentKHR queues.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vector>

#include <android/hardware_buffer.h>

#include <gui/ISurfaceComposer.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <ui/DisplayMode.h>
#include <ui/DisplayState.h>

#include <vulkan/vulkan.h>

using android::ISurfaceComposerClient;
using android::sp;
using android::String8;
using android::SurfaceComposerClient;
using android::SurfaceControl;

namespace {

/*
 * Applying a layer transaction makes libgui register its process-wide
 * TransactionCompletedListener and start a binder thread pool this tool
 * neither owns nor can stop, and SurfaceFlinger keeps calling that listener
 * back after the last present. Returning from main would let the C++ static
 * destructors free the listener singleton -- and its std::mutex -- under a
 * binder thread already inside onTransactionCompleted, which bionic reports
 * as "FORTIFY: pthread_mutex_lock called on a destroyed mutex". Leave with
 * the result instead; the layer, the client and the binder threads go away
 * with the process.
 */
[[noreturn]] void leave(int status) {
    fflush(nullptr);
    _exit(status);
}

bool check(VkResult result, const char* what) {
    if (result == VK_SUCCESS) {
        return true;
    }
    fprintf(stderr, "kernarium_vk_present: %s failed: %d\n", what, result);
    return false;
}

const char* presentModeName(VkPresentModeKHR mode) {
    switch (mode) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR:
            return "IMMEDIATE";
        case VK_PRESENT_MODE_MAILBOX_KHR:
            return "MAILBOX";
        case VK_PRESENT_MODE_FIFO_KHR:
            return "FIFO";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
            return "FIFO_RELAXED";
        default:
            return "OTHER";
    }
}

class Window {
public:
    bool create() {
        mClient = sp<SurfaceComposerClient>::make();
        if (mClient->initCheck() != android::NO_ERROR) {
            fprintf(stderr, "kernarium_vk_present: no SurfaceFlinger connection\n");
            return false;
        }
        const auto ids = SurfaceComposerClient::getPhysicalDisplayIds();
        if (ids.empty()) {
            fprintf(stderr, "kernarium_vk_present: no physical display\n");
            return false;
        }
        const auto token = SurfaceComposerClient::getPhysicalDisplayToken(ids.front());
        if (token == nullptr) {
            fprintf(stderr, "kernarium_vk_present: no display token\n");
            return false;
        }
        android::ui::DisplayMode mode;
        if (SurfaceComposerClient::getActiveDisplayMode(token, &mode) != android::NO_ERROR) {
            fprintf(stderr, "kernarium_vk_present: no active display mode\n");
            return false;
        }
        android::ui::DisplayState state;
        if (SurfaceComposerClient::getDisplayState(token, &state) != android::NO_ERROR) {
            fprintf(stderr, "kernarium_vk_present: no display state\n");
            return false;
        }
        int32_t width = mode.resolution.getWidth();
        int32_t height = mode.resolution.getHeight();
        if (state.orientation == android::ui::ROTATION_90 ||
            state.orientation == android::ui::ROTATION_270) {
            std::swap(width, height);
        }
        mControl = mClient->createSurface(String8("kernarium-vk-present"), width, height,
                                          android::PIXEL_FORMAT_RGBA_8888,
                                          ISurfaceComposerClient::eOpaque);
        if (mControl == nullptr || !mControl->isValid()) {
            fprintf(stderr, "kernarium_vk_present: cannot create a layer\n");
            return false;
        }
        SurfaceComposerClient::Transaction{}
                .setLayer(mControl, 0x7FFFFFFF)
                .show(mControl)
                .apply();
        mWindow = mControl->getSurface();
        printf("display %" PRId32 "x%" PRId32 ", layer shown\n", width, height);
        return mWindow != nullptr;
    }

    ANativeWindow* nativeWindow() const { return mWindow.get(); }

private:
    sp<SurfaceComposerClient> mClient;
    sp<SurfaceControl> mControl;
    sp<android::Surface> mWindow;
};

struct Vulkan {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkSemaphore rendered = VK_NULL_HANDLE;
    std::vector<VkImage> images;
};

bool createInstance(Vulkan* vk) {
    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app = {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "kernarium_vk_present";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = extensions;
    return check(vkCreateInstance(&info, nullptr, &vk->instance), "vkCreateInstance");
}

bool pickDevice(Vulkan* vk) {
    uint32_t count = 0;
    if (!check(vkEnumeratePhysicalDevices(vk->instance, &count, nullptr),
               "vkEnumeratePhysicalDevices")) {
        return false;
    }
    if (count == 0) {
        fprintf(stderr, "kernarium_vk_present: no physical device\n");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    if (!check(vkEnumeratePhysicalDevices(vk->instance, &count, devices.data()),
               "vkEnumeratePhysicalDevices")) {
        return false;
    }
    vk->physicalDevice = devices[0];

    VkPhysicalDeviceDriverProperties driver = {};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 properties = {};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &driver;
    vkGetPhysicalDeviceProperties2(vk->physicalDevice, &properties);
    printf("device %s, driverID %u (%s %s), api %u.%u.%u\n",
           properties.properties.deviceName, driver.driverID, driver.driverName,
           driver.driverInfo, VK_VERSION_MAJOR(properties.properties.apiVersion),
           VK_VERSION_MINOR(properties.properties.apiVersion),
           VK_VERSION_PATCH(properties.properties.apiVersion));

    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk->physicalDevice, &families, nullptr);
    std::vector<VkQueueFamilyProperties> family(families);
    vkGetPhysicalDeviceQueueFamilyProperties(vk->physicalDevice, &families, family.data());
    for (uint32_t i = 0; i < families; ++i) {
        VkBool32 supported = VK_FALSE;
        if (vkGetPhysicalDeviceSurfaceSupportKHR(vk->physicalDevice, i, vk->surface,
                                                 &supported) != VK_SUCCESS) {
            continue;
        }
        if (supported == VK_TRUE && (family[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            vk->queueFamily = i;
            return true;
        }
    }
    fprintf(stderr, "kernarium_vk_present: no queue family presents to the surface\n");
    return false;
}

bool createDevice(Vulkan* vk) {
    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = vk->queueFamily;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = extensions;
    if (!check(vkCreateDevice(vk->physicalDevice, &info, nullptr, &vk->device),
               "vkCreateDevice")) {
        return false;
    }
    vkGetDeviceQueue(vk->device, vk->queueFamily, 0, &vk->queue);
    return true;
}

uint32_t ahbFormatFor(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
            return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_R8G8B8_UNORM:
            return AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM;
        case VK_FORMAT_R5G6B5_UNORM_PACK16:
            return AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            return AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM;
        default:
            return 0;
    }
}

/*
 * Report, for one surface format, what the platform allocator and the driver
 * each say about the Android hardware buffer a swapchain image would be. The
 * loader asks the driver exactly this before it creates a swapchain, so a
 * format that fails here cannot become a presentable image.
 */
void reportSurfaceFormat(const Vulkan& vk, const VkSurfaceFormatKHR& format) {
    const uint32_t ahbFormat = ahbFormatFor(format.format);
    AHardwareBuffer_Desc desc = {};
    desc.width = 16;
    desc.height = 16;
    desc.layers = 1;
    desc.format = ahbFormat;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER;
    const bool framebuffer = ahbFormat != 0 && AHardwareBuffer_isSupported(&desc);
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    const bool sampled = ahbFormat != 0 && AHardwareBuffer_isSupported(&desc);

    VkPhysicalDeviceExternalImageFormatInfo external = {};
    external.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
    external.handleType =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkPhysicalDeviceImageFormatInfo2 info = {};
    info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    info.pNext = &external;
    info.format = format.format;
    info.type = VK_IMAGE_TYPE_2D;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkAndroidHardwareBufferUsageANDROID ahbUsage = {};
    ahbUsage.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID;
    VkImageFormatProperties2 props = {};
    props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    props.pNext = &ahbUsage;
    const VkResult result =
            vkGetPhysicalDeviceImageFormatProperties2(vk.physicalDevice, &info, &props);
    printf("  format %d colorSpace %d ahbFormat %u framebuffer %d sampled %d "
           "driver %d ahbUsage 0x%llx\n",
           format.format, format.colorSpace, ahbFormat, framebuffer, sampled, result,
           static_cast<unsigned long long>(ahbUsage.androidHardwareBufferUsage));
}

bool createSwapchain(Vulkan* vk, VkExtent2D* extent, VkFormat* format) {
    VkSurfaceCapabilitiesKHR caps = {};
    if (!check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk->physicalDevice, vk->surface, &caps),
               "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) {
        return false;
    }
    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk->physicalDevice, vk->surface, &formatCount, nullptr);
    if (formatCount == 0) {
        fprintf(stderr, "kernarium_vk_present: the surface reports no format\n");
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk->physicalDevice, vk->surface, &formatCount,
                                         formats.data());
    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk->physicalDevice, vk->surface, &modeCount,
                                              nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk->physicalDevice, vk->surface, &modeCount,
                                              modes.data());
    printf("surface formats %u, present modes %u, images %u..%u, usage 0x%x\n", formatCount,
           modeCount, caps.minImageCount, caps.maxImageCount, caps.supportedUsageFlags);
    for (uint32_t i = 0; i < formatCount; ++i) {
        reportSurfaceFormat(*vk, formats[i]);
    }
    for (uint32_t i = 0; i < modeCount; ++i) {
        printf("  present mode %s\n", presentModeName(modes[i]));
    }

    *extent = caps.currentExtent;
    *format = formats[0].format;
    VkSwapchainCreateInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = vk->surface;
    info.minImageCount = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount != 0 && info.minImageCount > caps.maxImageCount) {
        info.minImageCount = caps.maxImageCount;
    }
    info.imageFormat = formats[0].format;
    info.imageColorSpace = formats[0].colorSpace;
    info.imageExtent = caps.currentExtent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    if (!check(vkCreateSwapchainKHR(vk->device, &info, nullptr, &vk->swapchain),
               "vkCreateSwapchainKHR")) {
        return false;
    }
    uint32_t imageCount = 0;
    vkGetSwapchainImagesKHR(vk->device, vk->swapchain, &imageCount, nullptr);
    vk->images.resize(imageCount);
    if (!check(vkGetSwapchainImagesKHR(vk->device, vk->swapchain, &imageCount, vk->images.data()),
               "vkGetSwapchainImagesKHR")) {
        return false;
    }
    printf("swapchain %ux%u format %d images %u mode FIFO\n", info.imageExtent.width,
           info.imageExtent.height, info.imageFormat, imageCount);
    return true;
}

bool createCommandResources(Vulkan* vk) {
    VkCommandPoolCreateInfo pool = {};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = vk->queueFamily;
    if (!check(vkCreateCommandPool(vk->device, &pool, nullptr, &vk->commandPool),
               "vkCreateCommandPool")) {
        return false;
    }
    VkCommandBufferAllocateInfo allocate = {};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = vk->commandPool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    if (!check(vkAllocateCommandBuffers(vk->device, &allocate, &vk->commandBuffer),
               "vkAllocateCommandBuffers")) {
        return false;
    }
    VkSemaphoreCreateInfo semaphore = {};
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    return check(vkCreateSemaphore(vk->device, &semaphore, nullptr, &vk->acquired),
                 "vkCreateSemaphore") &&
           check(vkCreateSemaphore(vk->device, &semaphore, nullptr, &vk->rendered),
                 "vkCreateSemaphore");
}

void recordClear(Vulkan* vk, VkImage image, const VkClearColorValue& colour) {
    VkCommandBufferBeginInfo begin = {};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(vk->commandBuffer, &begin);

    VkImageSubresourceRange range = {};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;

    VkImageMemoryBarrier toTransfer = {};
    toTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = image;
    toTransfer.subresourceRange = range;
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(vk->commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toTransfer);

    vkCmdClearColorImage(vk->commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour,
                         1, &range);

    VkImageMemoryBarrier toPresent = toTransfer;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toPresent.dstAccessMask = 0;
    vkCmdPipelineBarrier(vk->commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toPresent);

    vkEndCommandBuffer(vk->commandBuffer);
}

}  // namespace

int main(int argc, char** argv) {
    uint32_t requested = 60;
    if (argc > 1) {
        requested = static_cast<uint32_t>(strtoul(argv[1], nullptr, 10));
    }

    Window window;
    if (!window.create()) {
        leave(1);
    }

    Vulkan vk;
    if (!createInstance(&vk)) {
        leave(1);
    }

    VkAndroidSurfaceCreateInfoKHR surfaceInfo = {};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.window = window.nativeWindow();
    if (!check(vkCreateAndroidSurfaceKHR(vk.instance, &surfaceInfo, nullptr, &vk.surface),
               "vkCreateAndroidSurfaceKHR")) {
        leave(1);
    }

    VkExtent2D extent = {};
    VkFormat format = VK_FORMAT_UNDEFINED;
    if (!pickDevice(&vk) || !createDevice(&vk) || !createSwapchain(&vk, &extent, &format) ||
        !createCommandResources(&vk)) {
        leave(1);
    }

    uint32_t presented = 0;
    for (uint32_t frame = 0; frame < requested; ++frame) {
        uint32_t index = 0;
        VkResult acquire = vkAcquireNextImageKHR(vk.device, vk.swapchain, UINT64_MAX,
                                                 vk.acquired, VK_NULL_HANDLE, &index);
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
            fprintf(stderr, "kernarium_vk_present: vkAcquireNextImageKHR failed: %d\n", acquire);
            break;
        }

        VkClearColorValue colour = {};
        colour.float32[0] = static_cast<float>(frame % 60) / 59.0f;
        colour.float32[1] = static_cast<float>((frame + 20) % 60) / 59.0f;
        colour.float32[2] = static_cast<float>((frame + 40) % 60) / 59.0f;
        colour.float32[3] = 1.0f;
        vkResetCommandBuffer(vk.commandBuffer, 0);
        recordClear(&vk, vk.images[index], colour);

        const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit = {};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &vk.acquired;
        submit.pWaitDstStageMask = &wait;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &vk.commandBuffer;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &vk.rendered;
        if (!check(vkQueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit")) {
            break;
        }

        VkPresentInfoKHR present = {};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &vk.rendered;
        present.swapchainCount = 1;
        present.pSwapchains = &vk.swapchain;
        present.pImageIndices = &index;
        VkResult result = vkQueuePresentKHR(vk.queue, &present);
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            fprintf(stderr, "kernarium_vk_present: vkQueuePresentKHR failed: %d\n", result);
            break;
        }
        ++presented;
        vkQueueWaitIdle(vk.queue);
    }

    printf("presented %u of %u frames\n", presented, requested);

    vkDeviceWaitIdle(vk.device);
    vkDestroySemaphore(vk.device, vk.rendered, nullptr);
    vkDestroySemaphore(vk.device, vk.acquired, nullptr);
    vkDestroyCommandPool(vk.device, vk.commandPool, nullptr);
    vkDestroySwapchainKHR(vk.device, vk.swapchain, nullptr);
    vkDestroyDevice(vk.device, nullptr);
    vkDestroySurfaceKHR(vk.instance, vk.surface, nullptr);
    vkDestroyInstance(vk.instance, nullptr);
    leave(presented == requested ? 0 : 1);
}
