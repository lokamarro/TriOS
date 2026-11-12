// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "display.hpp"
#include "display_geometry.hpp"

#import <Metal/Metal.h>

#include <cstdint>
#include <optional>

namespace uml::gpu {

/* The Metal pass that draws a guest scanout: its vertex stage covers the
 * viewport with the guest's top-left-origin unit square, and its fragment
 * stage samples the scanout rectangle through a ScanoutSampling, turned by a
 * presentation rotation. Magnification is nearest and minification bilinear.
 * Every presenter of a scanout texture draws it with this pass, so they all
 * sample it alike. */

/* The shader's ScanoutSampling: float2 members are 8-byte aligned, and the
 * trailing uint pads the struct to a multiple of that alignment. */
struct ShaderScanoutSampling final {
    float origin[2];
    float extent[2];
    float minimum[2];
    float maximum[2];
    std::uint32_t rotation;
    std::uint32_t padding;
};
static_assert(sizeof(ShaderScanoutSampling) == 40);

[[nodiscard]] ShaderScanoutSampling shaderSampling(
    const ScanoutSampling& sampling,
    DisplayRotation rotation) noexcept;

/* Whether the pass can sample this texture: a 2D texture, or a 2D array of
 * one layer. */
[[nodiscard]] bool scanoutPassSamples(id<MTLTexture> texture) noexcept;

/* How the pass samples a scanout stored in this texture, or nothing when the
 * scanout's rectangle cannot be sampled from it. */
[[nodiscard]] std::optional<ScanoutSampling> scanoutPassSampling(
    id<MTLTexture> texture,
    const MetalScanout& scanout) noexcept;

/* Draws a scanout in the pass: its pipeline for the texture's kind, the
 * viewport the guest's unit square covers, the texture, how it is sampled and
 * turned, and the covering triangle. Any scissor is set beforehand. The
 * objects are borrowed, which keeps ARC from retaining each around the pass. */
[[gnu::always_inline]] inline void encodeScanoutPass(
    __unsafe_unretained id<MTLRenderCommandEncoder> encoder,
    __unsafe_unretained id<MTLRenderPipelineState> pipeline,
    __unsafe_unretained id<MTLTexture> texture,
    const ScanoutSampling& sampling,
    DisplayRotation rotation,
    const MTLViewport& viewport) noexcept
{
    [encoder setRenderPipelineState:pipeline];
    [encoder setViewport:viewport];
    [encoder setFragmentTexture:texture atIndex:0];
    const auto shader = shaderSampling(sampling, rotation);
    [encoder setFragmentBytes:&shader length:sizeof(shader) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0
                vertexCount:3];
}

/* The pass for one colour attachment format. A one-layer 2D-array texture
 * needs its own fragment stage; forTexture() picks the one that reads it. */
struct ScanoutPipelines final {
    id<MTLRenderPipelineState> texture2D;
    id<MTLRenderPipelineState> textureArray;

    [[nodiscard]] id<MTLRenderPipelineState> forTexture(
        id<MTLTexture> texture) const noexcept;
};

/* Throws std::runtime_error when Metal cannot build the pass. */
[[nodiscard]] ScanoutPipelines makeScanoutPipelines(
    id<MTLDevice> device,
    MTLPixelFormat pixelFormat);

} // namespace uml::gpu
