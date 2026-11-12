// SPDX-License-Identifier: GPL-2.0-or-later

// A guest that makes a resource inside a command buffer, as Android's gralloc
// does for a graphics buffer, sends PIPE_RESOURCE_CREATE with a blob id and
// then asks for the blob that names it. When the host has no memory for the
// resource, the creation inside the buffer fails where the guest cannot see
// it, and the guest's next step is the one that learns of the refusal: the
// blob it names must be answered out of memory, the way a refused 3D resource
// is, and not as an id the host never saw, which tells the guest its own
// request was wrong. The contract under test:
//
//  - the blob of a creation the host refused for want of memory is answered
//    with ENOMEM, once;
//  - an id the host never saw, and the same refused id asked for again, are
//    answered as a request the guest got wrong; and
//  - once the guest has released memory, a resource and its blob are made.

#include "host_gl_stream_support.hpp"
#include "host_gpu_memory.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <vector>

namespace {

using namespace uml::gpu::testing::stream;
using uml::gpu::testing::skipWithoutRenderer;

constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;
constexpr std::uint64_t roomBytes = 96 * mebibyte;
constexpr std::uint32_t fillSize = 1024;
constexpr std::uint32_t refusedSize = 2048;
constexpr std::uint32_t smallSize = 128;
constexpr std::uint32_t bytesPerPixel = 4;
constexpr std::uint32_t commandPipeResourceCreate = 48;
constexpr std::uint32_t blobMemoryHost3d = 2;
constexpr std::uint32_t firstResource = 1;

[[nodiscard]] bool isOutOfMemory(int result) noexcept
{
    return result == ENOMEM || result == -ENOMEM;
}

// What the pool counts: the larger of Metal's allocation and the graphics
// footprint, since nothing here is imported.
[[nodiscard]] std::uint64_t used() noexcept
{
    return std::max(uml::platform::hostGpuAllocatedBytes(),
        uml::platform::hostGraphicsFootprintBytes());
}

// The guest's PIPE_RESOURCE_CREATE: a 2D render target that a blob names.
[[nodiscard]] int createInBuffer(
    VirglRenderer& renderer, std::uint32_t blobId, std::uint32_t size)
{
    Commands commands;
    commands.add(commandPipeResourceCreate, 0,
        {pipeTexture2d, virglFormatR8g8b8a8Unorm,
            virglBindRenderTarget | virglBindSamplerView, size, size, 1, 1, 0,
            0, 0, blobId});
    return commands.submit(renderer);
}

[[nodiscard]] int createBlob(VirglRenderer& renderer,
    std::uint32_t resourceId, std::uint32_t blobId, std::uint32_t size)
{
    return renderer.createResourceBlob(VirglRenderer::ResourceBlob {
        .resourceId = resourceId,
        .contextId = contextId,
        .memory = blobMemoryHost3d,
        .flags = 0,
        .blobId = blobId,
        .size = static_cast<std::uint64_t>(size) * size * bytesPerPixel,
    });
}

} // namespace

int main()
{
    // The pool takes its limit when the renderer starts, and counts everything
    // the process holds, so the limit is what is held now plus the room.
    uml::gpu::VirglRenderer::setDeviceMemoryLimit(used() + roomBytes);
    std::unique_ptr<uml::gpu::VirglRenderer> renderer;
    try {
        renderer = std::make_unique<uml::gpu::VirglRenderer>();
    } catch (const std::exception& error) {
        return skipWithoutRenderer(error);
    }
    require(renderer->createContext(contextId, virgl2CapsetId, "blob") == 0,
        "the context could not be created");

    // The guest fills the room with textures it keeps.
    std::vector<std::uint32_t> held;
    std::uint32_t nextResource = firstResource;
    for (;;) {
        const std::uint32_t resourceId = nextResource++;
        const int result = Image::create(*renderer, resourceId, pipeTexture2d,
            virglFormatR8g8b8a8Unorm,
            virglBindRenderTarget | virglBindSamplerView, fillSize, fillSize,
            0);
        if (result != 0) {
            require(isOutOfMemory(result),
                "a texture was refused for a reason other than memory");
            break;
        }
        held.push_back(resourceId);
        require(held.size() < 4096, "the budget never refused a texture");
    }
    require(held.size() > 4, "the budget left no room for a few textures");

    // A resource that cannot fit is refused inside the buffer, and the blob
    // that names it learns why.
    constexpr std::uint32_t refusedBlobId = 41;
    const int submit = createInBuffer(*renderer, refusedBlobId, refusedSize);
    std::fprintf(stderr,
        "%zu textures held; the creation in the buffer was answered %d\n",
        held.size(), submit);
    require(submit != 0, "the budget accepted a resource that cannot fit");
    const int refused =
        createBlob(*renderer, nextResource++, refusedBlobId, refusedSize);
    std::fprintf(stderr, "the blob of the refused creation was answered %d\n",
        refused);
    require(isOutOfMemory(refused),
        "the blob of a creation the host refused for want of memory was not "
        "answered out of memory");

    // The refusal is told once; an id the host never saw is the guest's error.
    const int again =
        createBlob(*renderer, nextResource++, refusedBlobId, refusedSize);
    require(again != 0 && !isOutOfMemory(again),
        "a refusal was told twice");
    const int unknown = createBlob(*renderer, nextResource++, 77, smallSize);
    require(unknown != 0 && !isOutOfMemory(unknown),
        "an id the host never saw was answered out of memory");

    // Released, the room is whole again, and a blob is made.
    for (const std::uint32_t resourceId : held) {
        renderer->unrefResource(resourceId);
    }
    held.clear();
    constexpr std::uint32_t goodBlobId = 42;
    require(createInBuffer(*renderer, goodBlobId, smallSize) == 0,
        "a resource that fits was not made");
    const std::uint32_t blobResource = nextResource++;
    require(createBlob(*renderer, blobResource, goodBlobId, smallSize) == 0,
        "the blob of a resource that was made was refused");
    renderer->unrefResource(blobResource);

    std::fputs("Refused blob: ok\n", stderr);
    return 0;
}
