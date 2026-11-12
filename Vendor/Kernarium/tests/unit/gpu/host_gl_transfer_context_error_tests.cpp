// SPDX-License-Identifier: GPL-2.0-or-later

// A transfer names its resource through a guest context, and one that names a
// resource the context never attached fails. That failure must cost the guest
// the transfer and nothing else. Android's gralloc mapper creates a context
// that submits nothing but transfers, to lock buffers for reading (a buffer
// with padded rows by a command buffer that holds one transfer, any other by a
// control-queue transfer). When a failed transfer left the context in error,
// every later transfer through it failed as well, each one reaching the guest
// as VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER and a readback of nothing.
//
// The contract under test: after a refused transfer, the same context
// transfers normally once the resource is attached to it.

#include "host_gl_test_support.hpp"
#include "protocol.hpp"
#include "require.hpp"
#include "virgl_renderer.hpp"

#include <sys/uio.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

constexpr std::uint32_t contextId = 1;
constexpr std::uint32_t resourceId = 1;
constexpr std::uint32_t size = 16;

} // namespace

int main()
{
    std::unique_ptr<uml::gpu::VirglRenderer> renderer;
    try {
        renderer = std::make_unique<uml::gpu::VirglRenderer>();
    } catch (const std::exception& error) {
        return skipWithoutRenderer(error);
    }

    require(renderer->createContext(contextId, virgl2CapsetId, "mapper") == 0,
        "the guest context could not be created");
    require(renderer->createResource3d(uml::gpu::VirglRenderer::Resource3d {
                .resourceId = resourceId,
                .target = pipeTexture2d,
                .format = virglFormatR8g8b8a8Unorm,
                .bind = virglBindRenderTarget | virglBindSamplerView,
                .width = size,
                .height = size,
                .depth = 1,
                .arraySize = 1,
            }) == 0,
        "the texture resource could not be created");
    std::vector<std::uint8_t> backing(
        static_cast<std::size_t>(size) * size * 4U, 0x5aU);
    std::array<iovec, 1> vectors {
        iovec {.iov_base = backing.data(), .iov_len = backing.size()},
    };
    require(renderer->attachBacking(resourceId, vectors) == 0,
        "the resource's guest backing could not be attached");

    const uml::gpu::VirglRenderer::Box box {
        .width = size,
        .height = size,
        .depth = 1,
    };
    require(renderer->transferResource(
                false, resourceId, contextId, 0, 0, 0, box, 0) != 0,
        "a context read a resource that was never attached to it");

    // Make another context current, so the next transfer through the guest
    // context has to switch back to it: that switch is what refused every
    // transfer while the refused one's error outlived it.
    std::vector<std::uint8_t> pixels(backing.size());
    require(renderer->readResourceRectangle(
                resourceId, pixels, 0, 0, size, size) == 0,
        "the host could not read the resource back");

    require(renderer->attachResource(contextId, resourceId) == 0,
        "the resource could not be attached to the guest context");
    require(renderer->transferResource(
                true, resourceId, contextId, 0, 0, 0, box, 0) == 0,
        "an attached resource could not be written after a refused transfer");
    std::fill(backing.begin(), backing.end(), std::uint8_t {});
    require(renderer->transferResource(
                false, resourceId, contextId, 0, 0, 0, box, 0) == 0,
        "an attached resource could not be read after a refused transfer");
    require(backing[0] == 0x5aU && backing[backing.size() - 1U] == 0x5aU,
        "the read did not return what the write stored");

    require(renderer->detachResource(contextId, resourceId) == 0,
        "the resource could not be detached");
    renderer->detachBacking(resourceId);
    renderer->unrefResource(resourceId);
    renderer->destroyContext(contextId);
    std::fputs("Transfer context error: ok\n", stderr);
    return 0;
}
