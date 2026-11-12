// SPDX-License-Identifier: GPL-2.0-or-later

// A guest allocates the pages behind a new texture zeroed, and expects to
// read zeros from the texture until it writes it. The texture's own storage
// is memory the host driver hands out again after another resource releases
// it, with that resource's pixels still in it, so a texture the renderer
// leaves as it finds it shows the guest another resource's contents: a
// process that never wrote them sees what an earlier process drew, and a
// pair of fresh buffers differ although neither was written. Android's
// BitmapTest#testSameAs_wrappedHardwareBuffer compares exactly such a pair.
//
// The contract under test: after textures that held a pattern are released,
// a new texture of the same shape reads back as zeros on every level.

#include "host_gl_test_support.hpp"
#include "require.hpp"
#include "virgl_renderer.hpp"

#include <sys/uio.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <vector>

namespace {

using uml::gpu::testing::pipeTexture2d;
using uml::gpu::testing::skipWithoutRenderer;
using uml::gpu::testing::virglBindRenderTarget;
using uml::gpu::testing::virglBindSamplerView;
using uml::gpu::testing::virglFormatR8g8b8a8Unorm;
using uml::testing::require;

constexpr std::uint32_t bytesPerPixel = 4;
// Enough textures per round that one of them lands on memory an earlier
// round's textures released, and enough rounds that it does so whichever way
// the driver recycles.
constexpr std::uint32_t texturesPerRound = 6;
constexpr std::uint32_t rounds = 4;
constexpr std::uint8_t patternSeed = 0xa5;

struct Shape {
    std::uint32_t size;
    std::uint32_t lastLevel;
};

// A texture, the guest storage it reads into, and where each level starts in
// that storage.
class Texture final {
public:
    Texture(
        uml::gpu::VirglRenderer& renderer,
        std::uint32_t resourceId,
        Shape shape)
        : renderer_ {renderer}
        , resourceId_ {resourceId}
        , shape_ {shape}
    {
        std::size_t total = 0;
        for (std::uint32_t level = 0; level <= shape.lastLevel; ++level) {
            offsets_.push_back(total);
            total += static_cast<std::size_t>(levelSize(level)) *
                levelSize(level) * bytesPerPixel;
        }
        storage_.resize(total);
        require(renderer_.createResource3d(
                    uml::gpu::VirglRenderer::Resource3d {
                        .resourceId = resourceId_,
                        .target = pipeTexture2d,
                        .format = virglFormatR8g8b8a8Unorm,
                        .bind = virglBindRenderTarget | virglBindSamplerView,
                        .width = shape.size,
                        .height = shape.size,
                        .depth = 1,
                        .arraySize = 1,
                        .lastLevel = shape.lastLevel,
                    }) == 0,
            "the texture could not be created");
        // The renderer keeps this array, not a copy of it.
        vectors_[0] = {.iov_base = storage_.data(), .iov_len = storage_.size()};
        require(renderer_.attachBacking(resourceId_, vectors_) == 0,
            "the texture's guest storage could not be attached");
    }

    ~Texture()
    {
        renderer_.detachBacking(resourceId_);
        renderer_.unrefResource(resourceId_);
    }

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    // Fills every level with a pattern no zeroed texture can show.
    void write()
    {
        std::fill(storage_.begin(), storage_.end(), patternSeed);
        for (std::uint32_t level = 0; level <= shape_.lastLevel; ++level)
            require(transfer(true, level) == 0,
                "the pattern could not be written to the texture");
    }

    // True when every byte of every level reads back as zero. The guest
    // storage starts as the pattern, so a level the read leaves alone is
    // not mistaken for zeros.
    [[nodiscard]] bool readsZero()
    {
        std::fill(storage_.begin(), storage_.end(), patternSeed);
        for (std::uint32_t level = 0; level <= shape_.lastLevel; ++level)
            require(transfer(false, level) == 0,
                "the texture could not be read back");
        return std::all_of(storage_.begin(), storage_.end(),
            [](std::uint8_t byte) { return byte == 0; });
    }

private:
    [[nodiscard]] std::uint32_t levelSize(std::uint32_t level) const noexcept
    {
        return std::max(shape_.size >> level, 1U);
    }

    [[nodiscard]] int transfer(bool toHost, std::uint32_t level)
    {
        const std::uint32_t size = levelSize(level);
        const uml::gpu::VirglRenderer::Box box {
            .width = size,
            .height = size,
            .depth = 1,
        };
        return renderer_.transferResource(toHost, resourceId_, 0, level,
            size * bytesPerPixel, 0, box, offsets_[level]);
    }

    uml::gpu::VirglRenderer& renderer_;
    std::uint32_t resourceId_;
    Shape shape_;
    std::vector<std::size_t> offsets_;
    std::vector<std::uint8_t> storage_;
    std::array<iovec, 1> vectors_ {};
};

} // namespace

int main()
{
    std::unique_ptr<uml::gpu::VirglRenderer> renderer;
    try {
        renderer = std::make_unique<uml::gpu::VirglRenderer>();
    } catch (const std::exception& error) {
        return skipWithoutRenderer(error);
    }

    constexpr std::array<Shape, 3> shapes {
        Shape {64, 0},
        Shape {256, 0},
        Shape {256, 3},
    };
    std::uint32_t nextResourceId = 1;
    for (const Shape shape : shapes) {
        for (std::uint32_t round = 0; round < rounds; ++round) {
            {
                std::vector<std::unique_ptr<Texture>> written;
                for (std::uint32_t index = 0; index < texturesPerRound;
                     ++index) {
                    written.push_back(std::make_unique<Texture>(
                        *renderer, nextResourceId++, shape));
                    written.back()->write();
                }
            }
            std::vector<std::unique_ptr<Texture>> fresh;
            for (std::uint32_t index = 0; index < texturesPerRound; ++index) {
                fresh.push_back(std::make_unique<Texture>(
                    *renderer, nextResourceId++, shape));
                require(fresh.back()->readsZero(),
                    "a new texture showed what a released one held");
            }
        }
    }
    std::fputs("Texture zero fill: ok\n", stderr);
    return 0;
}
