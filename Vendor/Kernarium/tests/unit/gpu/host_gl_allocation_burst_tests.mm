// SPDX-License-Identifier: GPL-2.0-or-later

// The budget holds a guest that creates textures back to back with no work
// between them (docs/graphics.md, "Device memory budget"). The pool counts
// what the host GPU holds by the larger of Metal's allocation counter and the
// graphics ledger, and the ledger also holds command storage that Metal's
// counter omits, so it usually leads the counter. The ledger counts a heap's
// pages only once they are resident, which the driver's residency set makes
// them at the next submission. A heap that is not resident when its
// reservation ends therefore moves neither figure while the ledger leads, and
// a burst of creations was admitted against a usage that stood still until the
// next submission put the process past the budget by as much as the lead.
//
// The test holds a lead of its own, Metal 4 command storage that only the
// ledger counts, sets the budget a little above what the process holds, and
// creates textures until the budget refuses one, with no transfer or draw in
// between. A transfer and a fence then submit work, and the host must hold no
// more than the budget. The renderer submits once before the burst: what a
// first submission records (command storage, the driver's own pipelines) is
// counted and never refused, so a renderer that had not yet submitted would
// pass the budget by it at the refusal, whose reclaim submits.

#include "host_gl_test_support.hpp"
#include "host_gpu_memory.hpp"
#include "require.hpp"
#include "virgl_renderer.hpp"

#import <Metal/Metal.h>

#include <sys/uio.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <thread>
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
// The command storage the test holds past Metal's counter, and the room the
// budget leaves above what the process holds: a burst that hid under the
// lead would pass the budget by far more than the slack.
constexpr std::uint64_t leadBytes = 48 * mebibyte;
constexpr std::uint64_t roomBytes = 64 * mebibyte;
constexpr std::uint32_t attempts = 64;
// What the budget does not bound: the driver's own small allocations and
// the command storage a submission records.
constexpr std::uint64_t slackBytes = 8 * mebibyte;

// What the pool counts, the test importing no host memory.
std::uint64_t used() noexcept
{
    return std::max(uml::platform::hostGpuAllocatedBytes(),
        uml::platform::hostGraphicsFootprintBytes());
}

std::uint64_t ledgerLead() noexcept
{
    const std::uint64_t metal = uml::platform::hostGpuAllocatedBytes();
    const std::uint64_t ledger =
        uml::platform::hostGraphicsFootprintBytes();
    return ledger > metal ? ledger - metal : 0;
}

// Command storage only the ledger counts: a Metal 4 command buffer recorded
// and kept, never committed, until the test ends.
class LedgerLead final {
public:
    explicit LedgerLead(std::uint64_t bytes)
    {
        device_ = MTLCreateSystemDefaultDevice();
        if (device_ == nil)
            return;
        allocator_ = [device_ newCommandAllocator];
        buffer_ = [device_ newCommandBuffer];
        scratch_ = [device_ newBufferWithLength:65536
                                        options:MTLResourceStorageModeShared];
        if (allocator_ == nil || buffer_ == nil || scratch_ == nil)
            return;
        [buffer_ beginCommandBufferWithAllocator:allocator_];
        // About 22 KiB of touched storage a pass; bounded however the
        // driver grows.
        for (std::uint32_t pass = 0;
             pass < 20000 && ledgerLead() < bytes; ++pass) {
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
    id<MTLDevice> device_ = nil;
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

    // Uploads the guest's pages: work the renderer submits.
    [[nodiscard]] int upload()
    {
        const uml::gpu::VirglRenderer::Box box {
            .width = textureSize,
            .height = textureSize,
            .depth = 1,
        };
        return renderer_.transferResource(true, resourceId_, 0, 0,
            textureSize * bytesPerPixel, 0, box, 0);
    }

private:
    uml::gpu::VirglRenderer& renderer_;
    std::uint32_t resourceId_;
    int result_ {};
    bool created_ {};
    std::vector<std::uint8_t> storage_;
    std::array<iovec, 1> vectors_ {};
};

} // namespace

int main()
{
    @autoreleasepool {
        LedgerLead lead(leadBytes);
        const std::uint64_t heldLead = ledgerLead();
        if (heldLead < leadBytes / 2)
            return skip("the host would not hold command storage past "
                        "Metal's counter");

        const std::uint64_t before = used();
        const std::uint64_t limit = before + roomBytes;
        uml::gpu::VirglRenderer::setDeviceMemoryLimit(limit);
        std::unique_ptr<uml::gpu::VirglRenderer> renderer;
        try {
            renderer = std::make_unique<uml::gpu::VirglRenderer>();
        } catch (const std::exception& error) {
            return skipWithoutRenderer(error);
        }

        std::uint32_t nextResourceId = 1;
        auto warmUp = std::make_unique<Texture>(*renderer, nextResourceId++);
        std::uint64_t fenceId = 0;
        require(warmUp->result() == 0 && warmUp->upload() == 0 &&
                renderer->createFenceAndWait(0, 0, ++fenceId, nullptr) == 0,
            "the renderer could not make, upload and submit a first texture");
        const std::uint64_t warm = used();

        // A burst: creations only, nothing the renderer has to submit.
        std::vector<std::unique_ptr<Texture>> held;
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
            held.push_back(std::move(texture));
        }
        const std::uint64_t afterBurst = used();

        // Work submitted now makes resident whatever was not yet.
        require(!held.empty() && held.front()->upload() == 0 &&
                renderer->createFenceAndWait(0, 0, ++fenceId, nullptr) == 0,
            "a texture made within the budget could not be uploaded");
        std::uint64_t peak = used();
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            peak = std::max(peak, used());
        }
        std::fprintf(stderr,
            "lead %.1f MiB; %u textures of %.0f MiB made in a burst against a "
            "limit of %.1f MiB (%.1f MiB before the renderer, %.1f MiB after "
            "its first submission); %.1f MiB held after the burst, %.1f MiB "
            "at most once work was submitted\n",
            static_cast<double>(heldLead) / mebibyte, made,
            static_cast<double>(textureBytes) / mebibyte,
            static_cast<double>(limit) / mebibyte,
            static_cast<double>(before) / mebibyte,
            static_cast<double>(warm) / mebibyte,
            static_cast<double>(afterBurst) / mebibyte,
            static_cast<double>(peak) / mebibyte);

        require(made < attempts,
            "the budget refused no texture of the burst");
        require(peak <= limit + slackBytes,
            "a burst of creations took the host past its budget once work "
            "was submitted");
        held.clear();
        warmUp.reset();
        renderer.reset();
        std::fputs("Allocation burst: ok\n", stderr);
    }
    return 0;
}
