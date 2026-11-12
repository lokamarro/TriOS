// SPDX-License-Identifier: GPL-2.0-or-later

// The budget counts the IOSurfaces of presentation and the driver's command
// storage both (docs/graphics.md, "Device memory budget"). Metal's allocation
// counter holds an IOSurface and not the command storage, the graphics ledger
// the command storage and not an IOSurface, and both hold the heaps the
// driver made resident. A pool that took the larger of the two would leave
// the smaller of the IOSurfaces and the command storage out, and grant that
// much past the budget.
//
// The test holds an IOSurface texture and Metal 4 command storage of its own,
// sets the budget a little above the two together, and creates textures until
// the budget refuses one. The pool must report both, beside the heaps the
// renderer made, from its start, and admit no more than the room above them.

// The resident heaps are read through the renderer's unstable API.
#define VIRGL_RENDERER_UNSTABLE_APIS

#include "host_gl_test_support.hpp"
#include "host_gpu_memory.hpp"
#include "require.hpp"
#include "virgl_renderer.hpp"

#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#include <virglrenderer.h>

#include <sys/uio.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <vector>

namespace {

using uml::gpu::testing::pipeTexture2d;
using uml::gpu::testing::skip;
using uml::gpu::testing::skipWithoutRenderer;
using uml::gpu::testing::virglBindRenderTarget;
using uml::gpu::testing::virglBindSamplerView;
using uml::gpu::testing::virglFormatR8g8b8a8Unorm;
using uml::testing::require;

constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;
constexpr std::uint32_t textureSize = 1024;
constexpr std::uint32_t bytesPerPixel = 4;
constexpr std::uint64_t textureBytes =
    std::uint64_t {textureSize} * textureSize * bytesPerPixel;
// A 4096 x 4096 BGRA surface, and half as much command storage beside it.
constexpr std::uint32_t surfaceSize = 4096;
constexpr std::uint64_t surfaceBytes =
    std::uint64_t {surfaceSize} * surfaceSize * bytesPerPixel;
constexpr std::uint64_t leadBytes = 32 * mebibyte;
// The room above what the process holds before the renderer starts; the
// textures the budget admits must fit in it less the renderer's own heaps,
// the surface's share not added.
constexpr std::uint64_t roomBytes = 64 * mebibyte;
constexpr std::uint32_t attempts = 64;
// What the renderer makes besides its heaps (queues, pipelines, the storage
// of a first submission), and what the ledger gives back of the test's own
// storage while it starts.
constexpr std::uint64_t slackBytes = 8 * mebibyte;

std::uint64_t ledgerBytes() noexcept
{
    return uml::platform::hostGraphicsFootprintBytes();
}

// What only the counter holds: an IOSurface the GPU has drawn to.
class Surface final {
public:
    explicit Surface(id<MTLDevice> device)
    {
        NSDictionary* properties = @{
            (id)kIOSurfaceWidth : @(surfaceSize),
            (id)kIOSurfaceHeight : @(surfaceSize),
            (id)kIOSurfaceBytesPerElement : @(bytesPerPixel),
            (id)kIOSurfacePixelFormat : @((std::uint32_t)'BGRA'),
        };
        surface_ = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
        if (surface_ == nullptr)
            return;
        MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                         width:surfaceSize
                                        height:surfaceSize
                                     mipmapped:NO];
        descriptor.usage = MTLTextureUsageRenderTarget;
        descriptor.storageMode = MTLStorageModeShared;
        texture_ = [device newTextureWithDescriptor:descriptor
                                          iosurface:surface_
                                              plane:0];
        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (texture_ == nil || queue == nil)
            return;
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        MTLRenderPassDescriptor* pass =
            [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = texture_;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        [[commands renderCommandEncoderWithDescriptor:pass] endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        drawn_ = commands.status == MTLCommandBufferStatusCompleted;
    }

    ~Surface()
    {
        texture_ = nil;
        if (surface_ != nullptr)
            CFRelease(surface_);
    }

    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    [[nodiscard]] bool drawn() const noexcept { return drawn_; }

private:
    IOSurfaceRef surface_ = nullptr;
    id<MTLTexture> texture_ = nil;
    bool drawn_ {};
};

// What only the ledger holds: a Metal 4 command buffer recorded and kept,
// never committed, until the test ends.
class CommandStorage final {
public:
    CommandStorage(id<MTLDevice> device, std::uint64_t bytes)
    {
        allocator_ = [device newCommandAllocator];
        buffer_ = [device newCommandBuffer];
        scratch_ = [device newBufferWithLength:65536
                                       options:MTLResourceStorageModeShared];
        if (allocator_ == nil || buffer_ == nil || scratch_ == nil)
            return;
        const std::uint64_t start = ledgerBytes();
        [buffer_ beginCommandBufferWithAllocator:allocator_];
        for (std::uint32_t pass = 0;
             pass < 20000 && ledgerBytes() < start + bytes; ++pass) {
            @autoreleasepool {
                id<MTL4ComputeCommandEncoder> encoder =
                    [buffer_ computeCommandEncoder];
                [encoder fillBuffer:scratch_
                              range:NSMakeRange(0, 64)
                              value:static_cast<std::uint8_t>(pass)];
                [encoder endEncoding];
            }
        }
        [buffer_ endCommandBuffer];
    }

private:
    id<MTL4CommandAllocator> allocator_ = nil;
    id<MTL4CommandBuffer> buffer_ = nil;
    id<MTLBuffer> scratch_ = nil;
};

class Texture final {
public:
    Texture(uml::gpu::VirglRenderer& renderer, std::uint32_t resourceId)
        : renderer_ {renderer}
        , resourceId_ {resourceId}
        , storage_(static_cast<std::size_t>(textureBytes))
    {
        result_ = renderer_.createResource3d(
            uml::gpu::VirglRenderer::Resource3d {
                .resourceId = resourceId_,
                .target = pipeTexture2d,
                .format = virglFormatR8g8b8a8Unorm,
                .bind = virglBindRenderTarget | virglBindSamplerView,
                .width = textureSize,
                .height = textureSize,
                .depth = 1,
                .arraySize = 1,
                .lastLevel = 0,
            });
        created_ = result_ == 0;
        if (!created_)
            return;
        // The renderer keeps this array, not a copy of it.
        vectors_[0] = {.iov_base = storage_.data(), .iov_len = storage_.size()};
        require(renderer_.attachBacking(resourceId_, vectors_) == 0,
            "the texture's guest storage could not be attached");
    }

    ~Texture()
    {
        if (!created_)
            return;
        renderer_.detachBacking(resourceId_);
        renderer_.unrefResource(resourceId_);
    }

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    [[nodiscard]] int result() const noexcept { return result_; }

private:
    uml::gpu::VirglRenderer& renderer_;
    std::uint32_t resourceId_;
    int result_ {};
    bool created_ {};
    std::vector<std::uint8_t> storage_;
    std::array<iovec, 1> vectors_ {};
};

[[nodiscard]] double mib(std::uint64_t bytes) noexcept
{
    return static_cast<double>(bytes) / mebibyte;
}

} // namespace

int main()
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil)
            return skip("the host has no Metal device");
        const std::uint64_t metalStart =
            uml::platform::hostGpuAllocatedBytes();
        Surface surface(device);
        if (!surface.drawn())
            return skip("the host could not draw to an IOSurface");
        const std::uint64_t surfaceCounted =
            uml::platform::hostGpuAllocatedBytes() - metalStart;
        const std::uint64_t ledgerStart = ledgerBytes();
        const std::uint64_t metalBefore =
            uml::platform::hostGpuAllocatedBytes();
        CommandStorage storage(device, leadBytes);
        // Only the ledger grows: the storage is not in Metal's counter.
        const std::uint64_t ledgerGrowth = ledgerBytes() - ledgerStart;
        const std::uint64_t metalGrowth =
            uml::platform::hostGpuAllocatedBytes() - metalBefore;
        const std::uint64_t lead =
            ledgerGrowth > metalGrowth ? ledgerGrowth - metalGrowth : 0;
        if (surfaceCounted < surfaceBytes / 2 || lead < leadBytes / 2)
            return skip("the host counts the surface or the command storage "
                        "differently");

        // Nothing the two hold overlaps, so the process holds their sum.
        const std::uint64_t metal = uml::platform::hostGpuAllocatedBytes();
        const std::uint64_t ledger =
            uml::platform::hostGraphicsFootprintBytes();
        const std::uint64_t held = metal + ledger;
        const std::uint64_t limit = held + roomBytes;
        uml::gpu::VirglRenderer::setDeviceMemoryLimit(limit);
        std::unique_ptr<uml::gpu::VirglRenderer> renderer;
        try {
            renderer = std::make_unique<uml::gpu::VirglRenderer>();
        } catch (const std::exception& error) {
            return skipWithoutRenderer(error);
        }
        const auto start = uml::gpu::VirglRenderer::deviceMemory();
        require(start.has_value() && start->budgetBytes == limit,
            "the renderer did not report its device memory budget");
        const std::uint64_t heaps = virgl_renderer_get_resident_heap_memory();

        std::uint32_t nextResourceId = 1;
        std::vector<std::unique_ptr<Texture>> textures;
        std::uint32_t made = 0;
        for (std::uint32_t index = 0; index < attempts; ++index) {
            auto texture =
                std::make_unique<Texture>(*renderer, nextResourceId++);
            if (texture->result() != 0) {
                require(texture->result() == ENOMEM ||
                        texture->result() == -ENOMEM,
                    "a texture the budget refused was not reported as out "
                    "of memory");
                break;
            }
            ++made;
            textures.push_back(std::move(texture));
        }
        std::fprintf(stderr,
            "surface %.1f MiB in Metal's counter, command storage %.1f MiB "
            "in the ledger alone; %.1f MiB held, limit %.1f MiB; the pool "
            "started at %.1f MiB with the renderer's heaps at %.1f MiB, and "
            "admitted %u textures of %.0f MiB\n",
            mib(surfaceCounted), mib(lead), mib(held), mib(limit),
            mib(start->usageBytes), mib(heaps), made, mib(textureBytes));

        require(start->usageBytes + slackBytes >= held + heaps,
            "the pool left the surface or the command storage out");
        require(made < attempts, "the budget refused no texture");
        require(static_cast<std::uint64_t>(made) * textureBytes + heaps <=
                roomBytes + slackBytes,
            "the budget admitted textures into the room the surface or the "
            "command storage takes");
        textures.clear();
        renderer.reset();
        std::fputs("Surface and command storage: ok\n", stderr);
    }
    return 0;
}
