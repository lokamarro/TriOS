// SPDX-License-Identifier: GPL-2.0-or-later

// A guest reaches a texture it has no pages for through a staging buffer: a
// copy transfer moves the pixels between the texture and the buffer's guest
// storage, in either direction. Android's gralloc makes every buffer a guest
// never touches itself a blob wrapping a texture the renderer created, so the
// texture has no guest storage at all, and an upload and a readback of a
// window, a hardware bitmap or an EGL image both have to go through the
// staging buffer.
//
// The renderer used to look for guest storage on the wrong side of a read. It
// asked the texture, which a blob never has, instead of the staging buffer
// that receives the data, so it refused every read of a blob and the guest
// read zeros.
//
// The contract under test: a copy transfer writes a blob texture from a
// staging buffer and reads the same pixels back into it.

#include "host_gl_test_support.hpp"
#include "protocol.hpp"
#include "require.hpp"
#include "virgl_renderer.hpp"

#include <sys/uio.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <span>
#include <vector>

namespace {

using uml::gpu::protocol::virgl2CapsetId;
using uml::gpu::testing::pipeTexture2d;
using uml::gpu::testing::skipWithoutRenderer;
using uml::gpu::testing::virglBindRenderTarget;
using uml::gpu::testing::virglBindSamplerView;
using uml::gpu::testing::virglFormatR8g8b8a8Unorm;
using uml::testing::require;

// virgl_protocol.h and virgl_hw.h values the guest sends.
constexpr std::uint32_t virglCommandCopyTransfer3d = 45;
constexpr std::uint32_t virglCommandPipeResourceCreate = 48;
constexpr std::uint32_t pipeBuffer = 0;
constexpr std::uint32_t virglFormatR8Unorm = 64;
constexpr std::uint32_t virglBindStaging = 1U << 19U;
constexpr std::uint32_t copyTransferReadFromHost = 1U << 1U;
// virglrenderer.h
constexpr std::uint32_t blobMemoryHost3d = 2;

constexpr std::uint32_t contextId = 1;
constexpr std::uint32_t textureBlobId = 7;
constexpr std::uint32_t textureResourceId = 10;
constexpr std::uint32_t stagingResourceId = 11;
constexpr std::uint32_t size = 16;
constexpr std::uint32_t bytesPerPixel = 4;
constexpr std::uint32_t stride = size * bytesPerPixel;
constexpr std::uint32_t imageBytes = stride * size;

[[nodiscard]] int submit(
    uml::gpu::VirglRenderer& renderer,
    std::span<const std::uint32_t> command)
{
    return renderer.submit(
        contextId,
        {reinterpret_cast<const std::uint8_t*>(command.data()),
            command.size_bytes()});
}

// PIPE_RESOURCE_CREATE: a texture the context owns, named by `blobId` until a
// blob resource wraps it.
[[nodiscard]] int createContextTexture(
    uml::gpu::VirglRenderer& renderer,
    std::uint32_t blobId)
{
    constexpr std::uint32_t length = 11;
    const std::array<std::uint32_t, length + 1> command {
        (length << 16U) | virglCommandPipeResourceCreate,
        pipeTexture2d,
        virglFormatR8g8b8a8Unorm,
        virglBindRenderTarget | virglBindSamplerView,
        size,
        size,
        1,
        1,
        0,
        0,
        0,
        blobId,
    };
    return submit(renderer, command);
}

// COPY_TRANSFER3D between the texture and the staging buffer. A read names
// the texture first, as the guest's driver encodes it.
[[nodiscard]] int copyTransfer(
    uml::gpu::VirglRenderer& renderer,
    bool readFromHost)
{
    constexpr std::uint32_t length = 14;
    const std::array<std::uint32_t, length + 1> command {
        (length << 16U) | virglCommandCopyTransfer3d,
        textureResourceId,
        0,
        0,
        stride,
        0,
        0,
        0,
        0,
        size,
        size,
        1,
        stagingResourceId,
        0,
        readFromHost ? copyTransferReadFromHost : 0U,
    };
    return submit(renderer, command);
}

} // namespace

int main()
{
    std::unique_ptr<uml::gpu::VirglRenderer> renderer;
    try {
        renderer = std::make_unique<uml::gpu::VirglRenderer>();
    } catch (const std::exception& error) {
        return skipWithoutRenderer(error);
    }

    require(renderer->createContext(contextId, virgl2CapsetId, "gralloc") == 0,
        "the guest context could not be created");

    // The texture, as a blob: renderer storage, no guest pages.
    require(createContextTexture(*renderer, textureBlobId) == 0,
        "the context could not create its texture");
    require(renderer->createResourceBlob(uml::gpu::VirglRenderer::ResourceBlob {
                .resourceId = textureResourceId,
                .contextId = contextId,
                .memory = blobMemoryHost3d,
                .blobId = textureBlobId,
                .size = imageBytes,
            }) == 0,
        "the texture could not be wrapped in a blob resource");
    require(renderer->attachResource(contextId, textureResourceId) == 0,
        "the blob could not be attached to the guest context");

    // The staging buffer: guest pages and nothing else.
    require(renderer->createResource3d(uml::gpu::VirglRenderer::Resource3d {
                .resourceId = stagingResourceId,
                .target = pipeBuffer,
                .format = virglFormatR8Unorm,
                .bind = virglBindStaging,
                .width = imageBytes,
                .height = 1,
                .depth = 1,
                .arraySize = 1,
            }) == 0,
        "the staging buffer could not be created");
    std::vector<std::uint8_t> staging(imageBytes);
    std::array<iovec, 1> vectors {
        iovec {.iov_base = staging.data(), .iov_len = staging.size()},
    };
    require(renderer->attachBacking(stagingResourceId, vectors) == 0,
        "the staging buffer's guest storage could not be attached");
    require(renderer->attachResource(contextId, stagingResourceId) == 0,
        "the staging buffer could not be attached to the guest context");

    for (std::size_t offset = 0; offset < staging.size(); ++offset)
        staging[offset] = static_cast<std::uint8_t>(offset * 7U + 3U);
    const std::vector<std::uint8_t> pattern = staging;

    require(copyTransfer(*renderer, false) == 0,
        "the staging buffer could not be copied into the blob texture");

    std::fill(staging.begin(), staging.end(), std::uint8_t {});
    require(copyTransfer(*renderer, true) == 0,
        "a copy transfer could not read a blob texture into the staging "
        "buffer");
    require(staging == pattern,
        "the read did not return what the copy transfer stored");

    require(renderer->detachResource(contextId, stagingResourceId) == 0,
        "the staging buffer could not be detached");
    renderer->detachBacking(stagingResourceId);
    renderer->unrefResource(stagingResourceId);
    require(renderer->detachResource(contextId, textureResourceId) == 0,
        "the blob could not be detached");
    renderer->unrefResource(textureResourceId);
    renderer->destroyContext(contextId);
    std::fputs("Blob copy transfer: ok\n", stderr);
    return 0;
}
