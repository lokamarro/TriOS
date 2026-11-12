// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// A guest's command stream for the renderer, built by hand: the objects a draw
// needs, the images the guest backs with storage it transfers into, and the
// draw that shows a sampled texture in a render target. Tests that check what
// the host does with a stream use it in place of a guest.

#include "host_gl_test_support.hpp"
#include "protocol.hpp"
#include "require.hpp"
#include "virgl_renderer.hpp"

#include <sys/uio.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace uml::gpu::testing::stream {

using uml::gpu::protocol::virgl2CapsetId;
using uml::gpu::testing::pipeTexture2d;
using uml::gpu::testing::virglBindRenderTarget;
using uml::gpu::testing::virglBindSamplerView;
using uml::gpu::testing::virglFormatR8g8b8a8Unorm;
using uml::gpu::VirglRenderer;
using uml::testing::require;

// virgl_protocol.h and virgl_hw.h values the guest sends.
inline constexpr std::uint32_t commandCreateObject = 1;
inline constexpr std::uint32_t commandBindObject = 2;
inline constexpr std::uint32_t commandSetViewportState = 4;
inline constexpr std::uint32_t commandSetFramebufferState = 5;
inline constexpr std::uint32_t commandSetVertexBuffers = 6;
inline constexpr std::uint32_t commandDrawVbo = 8;
inline constexpr std::uint32_t commandSetSamplerViews = 10;
inline constexpr std::uint32_t commandBindSamplerStates = 18;
inline constexpr std::uint32_t commandBindShader = 31;
inline constexpr std::uint32_t objectBlend = 1;
inline constexpr std::uint32_t objectRasterizer = 2;
inline constexpr std::uint32_t objectDepthStencilAlpha = 3;
inline constexpr std::uint32_t objectShader = 4;
inline constexpr std::uint32_t objectVertexElements = 5;
inline constexpr std::uint32_t objectSamplerView = 6;
inline constexpr std::uint32_t objectSamplerState = 7;
inline constexpr std::uint32_t objectSurface = 8;
inline constexpr std::uint32_t pipeBuffer = 0;
inline constexpr std::uint32_t shaderVertex = 0;
inline constexpr std::uint32_t shaderFragment = 1;
inline constexpr std::uint32_t primitiveTriangleStrip = 5;
inline constexpr std::uint32_t virglFormatR8Unorm = 64;
inline constexpr std::uint32_t virglFormatR8g8Unorm = 65;
inline constexpr std::uint32_t virglFormatR32g32b32a32Float = 31;
inline constexpr std::uint32_t virglBindVertexBuffer = 1U << 4U;

inline constexpr std::uint32_t contextId = 1;
inline constexpr std::uint32_t vertexBufferId = 2;
inline constexpr std::uint32_t vertexStride = 8 * sizeof(float);

// Wire handles of the objects the pipeline is made of. Those that come and go
// with a picture are numbered from firstTransientHandle and never reused.
inline constexpr std::uint32_t blendHandle = 1;
inline constexpr std::uint32_t depthHandle = 2;
inline constexpr std::uint32_t rasterizerHandle = 3;
inline constexpr std::uint32_t vertexShaderHandle = 4;
inline constexpr std::uint32_t fragmentShaderHandle = 5;
inline constexpr std::uint32_t elementsHandle = 6;
inline constexpr std::uint32_t samplerStateHandle = 7;
inline constexpr std::uint32_t firstTransientHandle = 16;

[[nodiscard]] inline std::uint32_t bits(float value) noexcept
{
    return std::bit_cast<std::uint32_t>(value);
}

[[nodiscard]] inline std::uint32_t alignUp(std::uint32_t value, std::uint32_t alignment)
{
    return (value + alignment - 1U) / alignment * alignment;
}

// One command buffer for the context, in 32-bit words.
class Commands final {
public:
    void add(
        std::uint32_t command,
        std::uint32_t object,
        std::initializer_list<std::uint32_t> payload)
    {
        words_.push_back(
            (static_cast<std::uint32_t>(payload.size()) << 16U) |
            (object << 8U) | command);
        words_.insert(words_.end(), payload.begin(), payload.end());
    }

    // CREATE_OBJECT of a shader from TGSI text, which is sent whole with its
    // terminator and padded to a word.
    void addShader(
        std::uint32_t handle, std::uint32_t type, std::string_view text)
    {
        std::vector<std::uint32_t> payload {
            handle, type, static_cast<std::uint32_t>(text.size() + 1), 300, 0};
        std::string padded(text);
        padded.resize(alignUp(static_cast<std::uint32_t>(text.size() + 1), 4U));
        for (std::size_t offset = 0; offset < padded.size(); offset += 4) {
            std::uint32_t word = 0;
            std::memcpy(&word, padded.data() + offset, sizeof(word));
            payload.push_back(word);
        }
        words_.push_back(
            (static_cast<std::uint32_t>(payload.size()) << 16U) |
            (objectShader << 8U) | commandCreateObject);
        words_.insert(words_.end(), payload.begin(), payload.end());
    }

    [[nodiscard]] int submit(VirglRenderer& renderer)
    {
        const int result = renderer.submit(
            contextId,
            {reinterpret_cast<const std::uint8_t*>(words_.data()),
                words_.size() * sizeof(std::uint32_t)});
        words_.clear();
        return result;
    }

private:
    std::vector<std::uint32_t> words_;
};

// A resource with guest storage the renderer reads and writes by transfer.
class Image final {
public:
    Image(
        VirglRenderer& renderer,
        std::uint32_t resourceId,
        std::uint32_t target,
        std::uint32_t format,
        std::uint32_t bind,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t flags,
        std::size_t bytes)
        : renderer_ {renderer}
        , resourceId_ {resourceId}
        , storage_(bytes)
    {
        require(create(renderer, resourceId, target, format, bind, width,
                    height, flags) == 0,
            "the image could not be created");
        vectors_[0] = {.iov_base = storage_.data(), .iov_len = storage_.size()};
        require(renderer_.attachBacking(resourceId_, vectors_) == 0,
            "the image's guest storage could not be attached");
        require(renderer_.attachResource(contextId, resourceId_) == 0,
            "the image could not be attached to the context");
    }

    ~Image()
    {
        (void)renderer_.detachResource(contextId, resourceId_);
        renderer_.detachBacking(resourceId_);
        renderer_.unrefResource(resourceId_);
    }

    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    [[nodiscard]] static int create(
        VirglRenderer& renderer,
        std::uint32_t resourceId,
        std::uint32_t target,
        std::uint32_t format,
        std::uint32_t bind,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t flags)
    {
        return renderer.createResource3d(VirglRenderer::Resource3d {
            .resourceId = resourceId,
            .target = target,
            .format = format,
            .bind = bind,
            .width = width,
            .height = height,
            .depth = 1,
            .arraySize = 1,
            .flags = flags,
        });
    }

    [[nodiscard]] std::vector<std::uint8_t>& bytes() noexcept
    {
        return storage_;
    }

    // Sends rows [first, first + count) of a `stride` byte wide image.
    void write(std::uint32_t stride, std::uint32_t first, std::uint32_t count)
    {
        writeBox(stride, 0, first, stride, count);
    }

    // Sends the bytes [x, x + width) of rows [y, y + height) of an image whose
    // guest rows are `stride` bytes apart. The transfer names that pitch
    // unless `name` is false, which leaves the pitch to the renderer.
    void writeBox(
        std::uint32_t stride,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t width,
        std::uint32_t height,
        bool name = true)
    {
        require(transferBox(true, stride, x, y, width, height, name) == 0,
            "the box could not be written");
    }

    // Fetches the same box from the host into the guest storage.
    [[nodiscard]] int readBox(
        std::uint32_t stride,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t width,
        std::uint32_t height,
        bool name = true)
    {
        return transferBox(false, stride, x, y, width, height, name);
    }

private:
    [[nodiscard]] int transferBox(
        bool toHost,
        std::uint32_t stride,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t width,
        std::uint32_t height,
        bool name)
    {
        const VirglRenderer::Box box {
            .x = x,
            .y = y,
            .z = 0,
            .width = width,
            .height = height,
            .depth = 1,
        };
        return renderer_.transferResource(
            toHost, resourceId_, contextId, 0, name ? stride : 0, 0, box,
            static_cast<std::uint64_t>(y) * stride + x);
    }


    VirglRenderer& renderer_;
    std::uint32_t resourceId_;
    std::vector<std::uint8_t> storage_;
    std::array<iovec, 1> vectors_ {};
};


// The fragment shader that shows a sampled texel unchanged.
inline constexpr std::string_view copyFragmentShader =
    "FRAG\n"
    "DCL IN[0], GENERIC[0], PERSPECTIVE\n"
    "DCL OUT[0], COLOR\n"
    "DCL SAMP[0]\n"
    "DCL SVIEW[0], 2D, FLOAT\n"
    "  0: TEX OUT[0], IN[0], SAMP[0], 2D\n"
    "  1: END\n";

// The draw that shows a texture's texels one for one in a render target of its
// own size: a viewport-filling strip that samples with nearest filtering.
class Pipeline final {
public:
    explicit Pipeline(
        VirglRenderer& renderer,
        std::string_view fragment = copyFragmentShader)
        : renderer_ {renderer}
        , vertices_(
              renderer, vertexBufferId, pipeBuffer, virglFormatR8Unorm,
              virglBindVertexBuffer, 4U * vertexStride, 1, 0, 4U * vertexStride)
    {
        // Positions in clip space with the texture coordinate at the same
        // corner of the unit square.
        const float corners[4][8] = {
            {-1, -1, 0, 1, 0, 0, 0, 1},
            {1, -1, 0, 1, 1, 0, 0, 1},
            {-1, 1, 0, 1, 0, 1, 0, 1},
            {1, 1, 0, 1, 1, 1, 0, 1},
        };
        std::memcpy(vertices_.bytes().data(), corners, sizeof(corners));
        const VirglRenderer::Box box {
            .width = 4U * vertexStride,
            .height = 1,
            .depth = 1,
        };
        require(renderer_.transferResource(
                    true, vertexBufferId, contextId, 0, 0, 0, box, 0) == 0,
            "the vertices could not be written");

        Commands commands;
        commands.add(commandCreateObject, objectBlend,
            {blendHandle, 0, 0, 0xfU << 27U, 0, 0, 0, 0, 0, 0, 0});
        commands.add(commandBindObject, objectBlend, {blendHandle});
        commands.add(commandCreateObject, objectDepthStencilAlpha,
            {depthHandle, 0, 0, 0, 0});
        commands.add(commandBindObject, objectDepthStencilAlpha, {depthHandle});
        // depth_clip, half_pixel_center and bottom_edge_rule, with unit point
        // size and line width.
        commands.add(commandCreateObject, objectRasterizer,
            {rasterizerHandle, (1U << 1U) | (1U << 29U) | (1U << 30U),
                bits(1.0F), 0, 0, bits(1.0F), 0, 0, 0});
        commands.add(commandBindObject, objectRasterizer, {rasterizerHandle});
        commands.addShader(vertexShaderHandle, shaderVertex,
            "VERT\n"
            "DCL IN[0]\n"
            "DCL IN[1]\n"
            "DCL OUT[0], POSITION\n"
            "DCL OUT[1], GENERIC[0]\n"
            "  0: MOV OUT[1], IN[1]\n"
            "  1: MOV OUT[0], IN[0]\n"
            "  2: END\n");
        commands.add(commandBindShader, 0, {vertexShaderHandle, shaderVertex});
        commands.addShader(fragmentShaderHandle, shaderFragment, fragment);
        commands.add(
            commandBindShader, 0, {fragmentShaderHandle, shaderFragment});
        commands.add(commandCreateObject, objectVertexElements,
            {elementsHandle, 0, 0, 0, virglFormatR32g32b32a32Float,
                4U * sizeof(float), 0, 0, virglFormatR32g32b32a32Float});
        commands.add(commandBindObject, objectVertexElements, {elementsHandle});
        commands.add(commandSetVertexBuffers, 0,
            {vertexStride, 0, vertexBufferId});
        // Clamp to the edge, nearest filtering, no mip levels.
        commands.add(commandCreateObject, objectSamplerState,
            {samplerStateHandle, 2U | (2U << 3U) | (2U << 6U) | (2U << 11U),
                0, 0, 0, 0, 0, 0, 0});
        commands.add(commandBindSamplerStates, 0,
            {shaderFragment, 0, samplerStateHandle});
        require(commands.submit(renderer_) == 0,
            "the pipeline could not be created");
    }

    // A view of `plane` of the image `resourceId`, in `format`.
    [[nodiscard]] std::uint32_t createView(
        std::uint32_t resourceId,
        std::uint32_t format,
        std::uint32_t plane)
    {
        const std::uint32_t handle = nextHandle_++;
        Commands commands;
        // The texture target rides in the top byte of the format word, and a
        // plane other than the first is sent where the layers go.
        commands.add(commandCreateObject, objectSamplerView,
            {handle, resourceId, format | (pipeTexture2d << 24U), plane, 0,
                0U | (1U << 3U) | (2U << 6U) | (3U << 9U)});
        require(commands.submit(renderer_) == 0,
            "the sampler view could not be created");
        return handle;
    }

    // Draws the view into an RGBA render target of the given size and returns
    // its texels.
    [[nodiscard]] std::vector<std::uint8_t> sample(
        std::uint32_t viewHandle,
        std::uint32_t width,
        std::uint32_t height)
    {
        const std::uint32_t targetId = 100;
        const std::uint32_t surfaceHandle = nextHandle_++;
        Image target(renderer_, targetId, pipeTexture2d,
            virglFormatR8g8b8a8Unorm,
            virglBindRenderTarget | virglBindSamplerView, width, height, 0,
            static_cast<std::size_t>(width) * height * 4U);
        Commands commands;
        commands.add(commandCreateObject, objectSurface,
            {surfaceHandle, targetId, virglFormatR8g8b8a8Unorm, 0, 0});
        commands.add(commandSetFramebufferState, 0, {1, 0, surfaceHandle});
        const float halfWidth = static_cast<float>(width) / 2.0F;
        const float halfHeight = static_cast<float>(height) / 2.0F;
        commands.add(commandSetViewportState, 0,
            {0, bits(halfWidth), bits(halfHeight), bits(0.5F), bits(halfWidth),
                bits(halfHeight), bits(0.5F)});
        commands.add(commandSetSamplerViews, 0, {shaderFragment, 0, viewHandle});
        commands.add(commandDrawVbo, 0,
            {0, 4, primitiveTriangleStrip, 0, 1, 0, 0, 0, 0, 0, 3, 0});
        require(commands.submit(renderer_) == 0, "the draw failed");
        require(renderer_.createFenceAndWait(contextId, 0, ++fence_, nullptr) ==
                0,
            "the draw did not complete");

        std::vector<std::uint8_t> texels(
            static_cast<std::size_t>(width) * height * 4U);
        require(renderer_.readResourceRectangle(
                    targetId, texels, 0, 0, width, height) == 0,
            "the render target could not be read");
        return texels;
    }

    // A wire handle for an object the caller makes; none is ever reused.
    [[nodiscard]] std::uint32_t newHandle() { return nextHandle_++; }

private:
    VirglRenderer& renderer_;
    Image vertices_;
    std::uint64_t fence_ {};
    std::uint32_t nextHandle_ = firstTransientHandle;
};

} // namespace uml::gpu::testing::stream
