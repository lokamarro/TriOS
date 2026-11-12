// SPDX-License-Identifier: GPL-2.0-or-later

// A guest that wants a stencil buffer and no depth buffer used to be given a
// depth-stencil one: virglrenderer had no image format of eight stencil bits
// alone, so every window surface and renderbuffer that needed only stencil
// held a 32-bit float depth plane or a 24-bit one beside it. Android's
// hardware renderer asks for exactly that, a stencil of eight bits and a depth
// of none, for every window it draws.
//
// The contract under test: the renderer advertises VIRGL_FORMAT_S8_UINT, an
// image of it is a depth-stencil target the guest can transfer into and out of,
// clear, draw stencil operations into and copy, and each of those changes
// exactly the stencil bytes the guest named. A framebuffer that held a
// depth-stencil image and is then given a stencil-only one has no depth plane
// left: the host used to keep the first image's depth plane beside the new
// stencil, a combination Zink cannot draw to, so every draw after the switch was
// lost. That is the order a window's depth-stencil buffer followed by a
// renderbuffer of stencil alone makes in the guest.

#include "host_gl_stream_support.hpp"
#include "host_gl_test_support.hpp"

#include <virgl_hw.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <vector>

namespace {

using namespace uml::gpu::testing::stream;
using uml::gpu::testing::skipWithoutRenderer;

// virgl_protocol.h values the guest sends.
constexpr std::uint32_t commandClear = 7;
constexpr std::uint32_t commandSetStencilRef = 13;
constexpr std::uint32_t commandResourceCopyRegion = 17;
constexpr std::uint32_t clearStencil = 1U << 1U;
constexpr std::uint32_t pipeFuncAlways = 7;
constexpr std::uint32_t pipeStencilOpReplace = 2;

constexpr std::uint32_t stencilFormat = VIRGL_FORMAT_S8_UINT;
constexpr std::uint32_t stencilBind = VIRGL_BIND_DEPTH_STENCIL | VIRGL_BIND_SAMPLER_VIEW;
constexpr std::uint32_t imageId = 20;
constexpr std::uint32_t copyId = 21;
constexpr std::uint32_t colorId = 22;
constexpr std::uint32_t packedId = 23;
constexpr std::uint32_t clearDepth = 1U << 0U;
constexpr std::uint32_t clearColor0 = 1U << 2U;
constexpr std::uint32_t pipeFuncLess = 1;

struct Size {
    std::uint32_t width;
    std::uint32_t height;
};

[[nodiscard]] bool advertisesFormat(const virgl_caps_v2& caps, std::uint32_t format)
{
    return ((caps.v1.sampler.bitmask[format / 32U] >> (format % 32U)) & 1U) != 0U;
}

// One byte per texel, one row after another: the image is its own stride.
[[nodiscard]] std::vector<std::uint8_t> read(
    uml::gpu::VirglRenderer& renderer,
    Image& image,
    std::uint32_t resourceId,
    Size size)
{
    std::fill(image.bytes().begin(), image.bytes().end(), std::uint8_t {0xee});
    const uml::gpu::VirglRenderer::Box box {
        .width = size.width,
        .height = size.height,
        .depth = 1,
    };
    require(renderer.transferResource(
                false, resourceId, contextId, 0, size.width, 0, box, 0) == 0,
        "the stencil image could not be read back");
    return image.bytes();
}

void expectAll(
    const std::vector<std::uint8_t>& texels,
    std::uint8_t value,
    const char* what)
{
    for (std::size_t index = 0; index < texels.size(); ++index) {
        if (texels[index] == value)
            continue;
        std::fprintf(stderr,
            "Stencil only target: %s: texel %zu is %#x, not %#x\n", what, index,
            texels[index], value);
        std::exit(1);
    }
}

void finish(uml::gpu::VirglRenderer& renderer, std::uint64_t& fence)
{
    require(renderer.createFenceAndWait(contextId, 0, ++fence, nullptr) == 0,
        "the commands did not complete");
}

void checkImage(
    uml::gpu::VirglRenderer& renderer,
    Pipeline& pipeline,
    std::uint64_t& fence,
    Size size)
{
    const std::size_t bytes = static_cast<std::size_t>(size.width) * size.height;
    Image image(renderer, imageId, pipeTexture2d, stencilFormat, stencilBind,
        size.width, size.height, 0, bytes);

    // The guest's bytes make the trip into the image and back unchanged.
    for (std::size_t index = 0; index < bytes; ++index)
        image.bytes()[index] = static_cast<std::uint8_t>(index * 7U + 3U);
    const std::vector<std::uint8_t> pattern = image.bytes();
    image.write(size.width, 0, size.height);
    require(read(renderer, image, imageId, size) == pattern,
        "a stencil image does not return the bytes written into it");

    // A framebuffer of the stencil image alone.
    const std::uint32_t surface = pipeline.newHandle();
    Commands commands;
    commands.add(commandCreateObject, objectSurface,
        {surface, imageId, stencilFormat, 0, 0});
    commands.add(commandSetFramebufferState, 0, {0, surface});
    commands.add(commandClear, 0, {clearStencil, 0, 0, 0, 0, 0, 0, 0x5a});
    require(commands.submit(renderer) == 0, "the stencil clear failed");
    finish(renderer, fence);
    expectAll(read(renderer, image, imageId, size), 0x5a, "the clear");

    // A stencil operation that replaces the value under the left half of the
    // image, drawn through a viewport of that half with no colour attachment,
    // leaves the right half alone.
    const std::uint32_t state = pipeline.newHandle();
    const std::uint32_t stencil = 1U | (pipeFuncAlways << 1U) |
        (pipeStencilOpReplace << 7U) | (0xffU << 13U) | (0xffU << 21U);
    const std::uint32_t quarterWidth = size.width / 4U;
    const float halfHeight = static_cast<float>(size.height) / 2.0F;
    commands.add(commandCreateObject, objectDepthStencilAlpha,
        {state, 0, stencil, 0, 0});
    commands.add(commandBindObject, objectDepthStencilAlpha, {state});
    commands.add(commandSetStencilRef, 0, {0xa5U});
    commands.add(commandSetViewportState, 0,
        {0, bits(static_cast<float>(quarterWidth)), bits(halfHeight),
            bits(0.5F), bits(static_cast<float>(quarterWidth)), bits(halfHeight),
            bits(0.5F)});
    commands.add(commandDrawVbo, 0,
        {0, 4, primitiveTriangleStrip, 0, 1, 0, 0, 0, 0, 0, 3, 0});
    require(commands.submit(renderer) == 0, "the stencil draw failed");
    finish(renderer, fence);
    const std::vector<std::uint8_t> drawn = read(renderer, image, imageId, size);
    for (std::uint32_t y = 0; y < size.height; ++y) {
        for (std::uint32_t x = 0; x < size.width; ++x) {
            const std::uint8_t expected = x < quarterWidth * 2U ? 0xa5 : 0x5a;
            if (drawn[static_cast<std::size_t>(y) * size.width + x] == expected)
                continue;
            std::fprintf(stderr,
                "Stencil only target: the draw: texel (%u, %u) is %#x, not %#x\n",
                x, y, drawn[static_cast<std::size_t>(y) * size.width + x],
                expected);
            std::exit(1);
        }
    }

    // A copy between two stencil images moves the stencil bytes as they are.
    Image copy(renderer, copyId, pipeTexture2d, stencilFormat, stencilBind,
        size.width, size.height, 0, bytes);
    commands.add(commandResourceCopyRegion, 0,
        {copyId, 0, 0, 0, 0, imageId, 0, 0, 0, 0, size.width, size.height, 1});
    require(commands.submit(renderer) == 0, "the stencil copy failed");
    finish(renderer, fence);
    require(read(renderer, copy, copyId, size) == drawn,
        "a copy of a stencil image changed its bytes");

    // Leave the bindings the next image starts from: nothing stencilled.
    commands.add(commandBindObject, objectDepthStencilAlpha, {depthHandle});
    require(commands.submit(renderer) == 0, "the state could not be reset");
}

// Draws a viewport-filling strip, whose fragments are all at depth 0.5, into a
// 16 by 16 colour image and the depth-stencil image `zsId`, with the depth test
// on and the depth plane (if the image has one) cleared to 0.0, which nothing
// can pass. Returns how many texels were drawn.
[[nodiscard]] std::size_t drawnThrough(
    uml::gpu::VirglRenderer& renderer,
    Pipeline& pipeline,
    std::uint64_t& fence,
    std::uint32_t zsId,
    std::uint32_t zsFormat,
    bool hasDepth)
{
    constexpr std::uint32_t side = 16;
    constexpr std::uint32_t rgba = uml::gpu::testing::virglFormatR8g8b8a8Unorm;
    Image color(renderer, colorId, pipeTexture2d, rgba,
        uml::gpu::testing::virglBindRenderTarget, side, side, 0,
        static_cast<std::size_t>(side) * side * 4U);
    const std::uint32_t colorSurface = pipeline.newHandle();
    const std::uint32_t zsSurface = pipeline.newHandle();
    const std::uint32_t depthTest = pipeline.newHandle();
    const float half = static_cast<float>(side) / 2.0F;
    Commands commands;
    commands.add(commandCreateObject, objectSurface,
        {colorSurface, colorId, rgba, 0, 0});
    commands.add(commandCreateObject, objectSurface,
        {zsSurface, zsId, zsFormat, 0, 0});
    commands.add(commandSetFramebufferState, 0, {1, zsSurface, colorSurface});
    commands.add(commandSetViewportState, 0,
        {0, bits(half), bits(half), bits(0.5F), bits(half), bits(half),
            bits(0.5F)});
    const std::uint32_t cleared = clearColor0 | clearStencil |
        (hasDepth ? clearDepth : 0U);
    commands.add(commandClear, 0, {cleared, 0, 0, 0, 0, 0, 0, 0});
    // depth_enabled, depth_writemask and depth_func LESS.
    commands.add(commandCreateObject, objectDepthStencilAlpha,
        {depthTest, 1U | (1U << 1U) | (pipeFuncLess << 2U), 0, 0, 0});
    commands.add(commandBindObject, objectDepthStencilAlpha, {depthTest});
    commands.add(commandDrawVbo, 0,
        {0, 4, primitiveTriangleStrip, 0, 1, 0, 0, 0, 0, 0, 3, 0});
    require(commands.submit(renderer) == 0, "the draw failed");
    finish(renderer, fence);
    std::vector<std::uint8_t> texels(static_cast<std::size_t>(side) * side * 4U);
    require(renderer.readResourceRectangle(colorId, texels, 0, 0, side, side) == 0,
        "the colour image could not be read");
    commands.add(commandBindObject, objectDepthStencilAlpha, {depthHandle});
    require(commands.submit(renderer) == 0, "the state could not be reset");
    std::size_t drawn = 0;
    for (std::size_t index = 0; index < texels.size(); index += 4)
        drawn += (texels[index] != 0 || texels[index + 1] != 0 ||
                     texels[index + 2] != 0) ? 1U : 0U;
    return drawn;
}

void checkAfterDepthStencil(
    uml::gpu::VirglRenderer& renderer,
    Pipeline& pipeline,
    std::uint64_t& fence)
{
    constexpr std::uint32_t side = 16;
    Image packed(renderer, packedId, pipeTexture2d, VIRGL_FORMAT_S8_UINT_Z24_UNORM,
        stencilBind, side, side, 0, static_cast<std::size_t>(side) * side * 4U);
    Image stencil(renderer, imageId, pipeTexture2d, stencilFormat, stencilBind,
        side, side, 0, static_cast<std::size_t>(side) * side);

    // The depth-stencil image does test depth: a depth of zero stops every fragment.
    require(drawnThrough(renderer, pipeline, fence, packedId,
                VIRGL_FORMAT_S8_UINT_Z24_UNORM, true) == 0,
        "a depth-stencil image does not apply the depth test");
    // A stencil-only image has no depth to test against, so the same draw lands
    // everywhere, however often the framebuffer changes between the two.
    for (int round = 0; round < 2; ++round) {
        require(drawnThrough(renderer, pipeline, fence, imageId, stencilFormat, false) ==
                static_cast<std::size_t>(side) * side,
            "a stencil-only image after a depth-stencil one did not take the draw");
        require(drawnThrough(renderer, pipeline, fence, packedId,
                    VIRGL_FORMAT_S8_UINT_Z24_UNORM, true) == 0,
            "a depth-stencil image after a stencil-only one does not apply the depth test");
    }
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

    const auto capset = renderer->capset(virgl2CapsetId).data;
    virgl_caps_v2 caps {};
    require(capset.size() >= sizeof(caps.v1),
        "the capset is too short to hold the version 1 capabilities");
    std::memcpy(&caps, capset.data(), std::min(capset.size(), sizeof(caps)));
    require(advertisesFormat(caps, stencilFormat),
        "the renderer does not advertise an eight bit stencil format");

    require(renderer->createContext(contextId, virgl2CapsetId, "stencil") == 0,
        "the guest context could not be created");

    Pipeline pipeline(*renderer,
        "FRAG\n"
        "DCL OUT[0], COLOR\n"
        "IMM[0] FLT32 { 1.0, 1.0, 1.0, 1.0 }\n"
        "  0: MOV OUT[0], IMM[0]\n"
        "  1: END\n");
    std::uint64_t fence = 0;
    // A square, a size whose rows are not a multiple of the host's alignment,
    // and one taller than it is wide.
    for (const Size size : {Size {16, 16}, Size {17, 9}, Size {64, 33}})
        checkImage(*renderer, pipeline, fence, size);
    checkAfterDepthStencil(*renderer, pipeline, fence);

    std::fputs("Stencil only target: ok\n", stderr);
    return 0;
}
