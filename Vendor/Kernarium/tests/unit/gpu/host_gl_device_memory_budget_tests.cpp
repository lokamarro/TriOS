// SPDX-License-Identifier: GPL-2.0-or-later

// The host GPU has one fixed budget (docs/graphics.md, "Device memory
// budget"), and the OpenGL path stays inside it: every buffer and texture a
// guest creates is memory the host's Zink allocates through KosmicKrisp, which
// refuses what the budget cannot hold. The contract is what the host does and
// what it tells the guest:
//
//  - a texture the budget cannot hold is not made: the renderer fails the
//    creation with ENOMEM, which the device answers as out of memory so that
//    the guest fails it too, and the host's GPU memory stays within the
//    budget while the guest tries;
//  - the renderer is not lost: once the guest releases some textures, the
//    ones it made before the budget ran out still hold their pixels (while
//    the budget is full even a transfer is refused, since it needs a staging
//    buffer), and
//  - once the guest releases them all, new textures store again, even though
//    the driver still holds the released memory for reuse, because a refused
//    allocation first drops what the driver caches and tries once more.

#include "host_gl_test_support.hpp"
#include "host_gpu_memory.hpp"
#include "require.hpp"
#include "virgl_renderer.hpp"

#include <sys/uio.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <thread>
#include <vector>

namespace {

using uml::gpu::testing::pipeTexture2d;
using uml::gpu::testing::skipWithoutRenderer;
using uml::gpu::testing::virglBindRenderTarget;
using uml::gpu::testing::virglBindSamplerView;
using uml::gpu::testing::virglFormatR8g8b8a8Unorm;
using uml::testing::require;

constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;
constexpr std::uint32_t textureSize = 1024;
constexpr std::uint32_t bytesPerPixel = 4;
// 4 MiB each; the room leaves space for some of them past what the renderer
// holds when it starts, and the guest tries to make far more than that.
constexpr std::uint64_t roomBytes = 192 * mebibyte;
constexpr std::uint32_t attempts = 120;
// What the budget does not bound: the driver's own small allocations (command
// pools, descriptor pools, upload buffers) are counted and never refused.
constexpr std::uint64_t slackBytes = 8 * mebibyte;

// What the pool counts: the larger of Metal's allocation and the graphics
// footprint.
std::uint64_t used() noexcept
{
    return std::max(uml::platform::hostGpuAllocatedBytes(),
        uml::platform::hostGraphicsFootprintBytes());
}

class Texture final {
public:
    Texture(uml::gpu::VirglRenderer& renderer, std::uint32_t resourceId)
        : renderer_ {renderer}
        , resourceId_ {resourceId}
        , storage_(static_cast<std::size_t>(textureSize) * textureSize *
              bytesPerPixel)
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

    // What the renderer answered the creation with: 0, or the reason.
    [[nodiscard]] int result() const noexcept { return result_; }

    // Writes a pattern of its own to the texture and reads it back: true when
    // the host gave the texture storage that holds it.
    [[nodiscard]] bool stores(std::uint8_t pattern)
    {
        if (!created_)
            return false;
        std::fill(storage_.begin(), storage_.end(), pattern);
        if (transfer(true) != 0)
            return false;
        return readsAs(pattern);
    }

    // True when the texture reads back as `pattern`.
    [[nodiscard]] bool readsAs(std::uint8_t pattern)
    {
        if (!created_)
            return false;
        std::fill(storage_.begin(), storage_.end(),
            static_cast<std::uint8_t>(~pattern));
        if (transfer(false) != 0)
            return false;
        return std::all_of(storage_.begin(), storage_.end(),
            [pattern](std::uint8_t byte) { return byte == pattern; });
    }

private:
    [[nodiscard]] int transfer(bool toHost)
    {
        const uml::gpu::VirglRenderer::Box box {
            .width = textureSize,
            .height = textureSize,
            .depth = 1,
        };
        return renderer_.transferResource(toHost, resourceId_, 0, 0,
            textureSize * bytesPerPixel, 0, box, 0);
    }

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
    // The pool takes its limit when the renderer starts, and counts everything
    // the process holds, so the limit is what is held now plus the room.
    const std::uint64_t before = used();
    const std::uint64_t limit = before + roomBytes;
    uml::gpu::VirglRenderer::setDeviceMemoryLimit(limit);
    std::unique_ptr<uml::gpu::VirglRenderer> renderer;
    try {
        renderer = std::make_unique<uml::gpu::VirglRenderer>();
    } catch (const std::exception& error) {
        return skipWithoutRenderer(error);
    }

    // The pool's figures are the applications': the budget, and the count
    // the budget refuses by.
    const auto start = uml::gpu::VirglRenderer::deviceMemory();
    require(start.has_value() && start->budgetBytes == limit,
        "the renderer did not report its device memory budget");
    require(start->ceilingBytes ==
            limit - uml::gpu::VirglRenderer::deviceMemoryReserveBytes,
        "the pool does not keep its reserve inside the budget");

    std::uint32_t nextResourceId = 1;
    std::vector<std::unique_ptr<Texture>> held;
    std::uint32_t stored = 0;
    std::uint32_t refusedAtCreation = 0;
    std::uint32_t firstRefused = attempts;
    std::uint64_t peak = 0;
    for (std::uint32_t index = 0; index < attempts; ++index) {
        held.push_back(
            std::make_unique<Texture>(*renderer, nextResourceId++));
        if (held.back()->result() != 0) {
            // The only reason a well described texture is not made.
            require(held.back()->result() == ENOMEM ||
                    held.back()->result() == -ENOMEM,
                "a texture the budget refused was not reported as out of "
                "memory");
            ++refusedAtCreation;
        }
        if (index == 0 && held.back()->result() == 0) {
            // The ledger credits a resource with what its storage takes.
            require(renderer->resourceStorageBytes(1) >=
                    static_cast<std::uint64_t>(textureSize) * textureSize *
                        bytesPerPixel,
                "a texture's host storage was not reported");
            require(renderer->resourceStorageBytes(nextResourceId + 1000) ==
                    0,
                "a resource nobody made reported storage");
        }
        if (held.back()->stores(static_cast<std::uint8_t>(index + 1)))
            ++stored;
        else
            firstRefused = std::min(firstRefused, index);
        peak = std::max(peak, used());
    }
    std::fprintf(stderr,
        "%u of %u textures of %u MiB stored, %u refused at creation; the "
        "first refusal at %u; "
        "%.1f MiB held at most against a limit of %.1f MiB (%.1f MiB "
        "before the renderer)\n",
        stored, attempts, static_cast<unsigned>(textureSize * textureSize *
            bytesPerPixel / mebibyte),
        refusedAtCreation, firstRefused, static_cast<double>(peak) / mebibyte,
        static_cast<double>(limit) / mebibyte,
        static_cast<double>(before) / mebibyte);

    require(stored > 0, "the budget left no room for a single texture");
    require(stored < attempts,
        "the budget did not refuse a texture past its limit");
    require(refusedAtCreation > 0,
        "the budget refused no creation: a texture past it was made without "
        "storage and the guest was not told");
    require(peak <= limit + slackBytes,
        "the host GPU held more than its budget while a guest asked for more");
    const auto full = uml::gpu::VirglRenderer::deviceMemory();
    require(full.has_value() && full->budgetBytes == limit,
        "the budget changed while the guest asked for more");
    require(full->usageBytes <= limit + slackBytes &&
            full->usageBytes >= start->usageBytes + (stored * 3ULL / 4ULL) *
                (textureSize * textureSize * bytesPerPixel),
        "the pool's usage did not follow the textures the host stored");

    // Release the later half of what the guest made; the textures it made
    // before the first refusal and kept still hold their pixels.
    const std::uint32_t kept = firstRefused / 2;
    held.resize(kept);
    for (std::uint32_t index = 0; index < kept; ++index)
        require(held[index]->readsAs(static_cast<std::uint8_t>(index + 1)),
            "a texture made within the budget lost its pixels");
    for (std::uint32_t index = 0; index < 4; ++index) {
        Texture texture(*renderer, nextResourceId++);
        require(texture.stores(static_cast<std::uint8_t>(index + 101)),
            "a texture made after the guest released some memory has no "
            "storage");
    }

    // Released, the memory serves new textures although the driver holds it
    // for reuse: a refusal drops what it caches and tries once more.
    held.clear();
    for (std::uint32_t index = 0; index < 4; ++index) {
        Texture texture(*renderer, nextResourceId++);
        require(texture.stores(static_cast<std::uint8_t>(index + 201)),
            "a texture made after the guest released its memory has no "
            "storage");
    }
    renderer.reset();
    require(!uml::gpu::VirglRenderer::deviceMemory().has_value(),
        "a renderer that is gone still reported a budget");
    std::fputs("Device memory budget: ok\n", stderr);
    return 0;
}
