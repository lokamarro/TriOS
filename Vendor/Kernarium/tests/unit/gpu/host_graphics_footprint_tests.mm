// SPDX-License-Identifier: GPL-2.0-or-later
/* A Metal 4 command allocator keeps what its command buffers encoded until it
 * is released, and that storage never shows in the device's allocated size.
 * One large recording once held gigabytes of it and pushed an iOS app past
 * its memory limit, while the device memory budget, counting the allocated
 * size alone, did not see it. The graphics footprint charges the storage that
 * recorded commands wrote, which is why the budget's usage takes the larger of
 * the two. */

#include "host_gpu_memory.hpp"
#include "require.hpp"

#import <Metal/Metal.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

using uml::testing::require;

constexpr double mib = 1024.0 * 1024.0;
/* Dense recordings write nearly all of the storage they grow; an encoder
 * with no commands reserves far more than it writes. */
constexpr unsigned encoders = 16;
constexpr unsigned copiesPerEncoder = 65536;

/* Grows one allocator by recording many copy commands and returns its
 * storage, checking what the graphics footprint charges while it is held. */
std::uint64_t recordAndCheck(id<MTLDevice> device,
    std::uint64_t footprintBefore, std::uint64_t allocatedBefore)
{
    @autoreleasepool {
        id<MTLBuffer> source = [device newBufferWithLength:4096
                                                   options:MTLResourceStorageModeShared];
        id<MTLBuffer> destination = [device newBufferWithLength:4096
                                                        options:MTLResourceStorageModeShared];
        id<MTL4CommandAllocator> allocator = [device newCommandAllocator];
        id<MTL4CommandBuffer> commands = [device newCommandBuffer];
        require(source != nil && destination != nil && allocator != nil &&
                commands != nil,
            "no buffers, command allocator or command buffer");
        [commands beginCommandBufferWithAllocator:allocator];
        for (unsigned index = 0; index < encoders; ++index) {
            id<MTL4ComputeCommandEncoder> encoder = [commands computeCommandEncoder];
            require(encoder != nil, "no compute encoder");
            for (unsigned copy = 0; copy < copiesPerEncoder; ++copy) {
                const NSUInteger offset = (copy % 256) * 16;
                [encoder copyFromBuffer:source
                           sourceOffset:offset
                               toBuffer:destination
                      destinationOffset:offset
                                   size:16];
            }
            [encoder endEncoding];
        }
        [commands endCommandBuffer];
        const std::uint64_t storage = allocator.allocatedSize;

        const std::uint64_t allocated = uml::platform::hostGpuAllocatedBytes();
        const std::uint64_t footprint = uml::platform::hostGraphicsFootprintBytes();
        std::printf("%u encoders of %u copies: allocator %.1f MiB, allocated size %+.1f MiB, "
                    "graphics footprint %+.1f MiB\n",
            encoders, copiesPerEncoder, storage / mib,
            (static_cast<double>(allocated) - allocatedBefore) / mib,
            (static_cast<double>(footprint) - footprintBefore) / mib);
        require(storage >= 64 * 1024 * 1024,
            "the recording grew its allocator by less than 64 MiB");
        require(footprint >= footprintBefore + storage * 9 / 10,
            "the graphics footprint omits the allocator's storage");
        return storage;
    }
}

} // namespace

int main()
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (device == nil) {
            std::puts("SKIP host graphics footprint: no Metal device");
            return 77;
        }
        const std::uint64_t allocatedBefore =
            uml::platform::hostGpuAllocatedBytes();
        const std::uint64_t footprintBefore =
            uml::platform::hostGraphicsFootprintBytes();
        const std::uint64_t storage = recordAndCheck(device,
            footprintBefore, allocatedBefore);

        /* Releasing the allocator returns its storage. */
        const std::uint64_t released =
            uml::platform::hostGraphicsFootprintBytes();
        std::printf("released: graphics footprint %+.1f MiB against the start\n",
            (static_cast<double>(released) - footprintBefore) / mib);
        require(released < footprintBefore + storage / 2,
            "the released allocator stays in the graphics footprint");
    }
    std::puts("PASS host graphics footprint");
    return 0;
}
