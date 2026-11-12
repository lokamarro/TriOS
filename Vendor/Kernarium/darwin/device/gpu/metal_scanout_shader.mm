// SPDX-License-Identifier: GPL-2.0-or-later

#include "metal_scanout_shader.hpp"

#include <stdexcept>
#include <string>

namespace uml::gpu {
namespace {

NSString* const metalShaderSource = @R"metal(
#include <metal_stdlib>
using namespace metal;

struct RasterData {
    float4 position [[position]];
    float2 textureCoordinate;
};

vertex RasterData displayVertex(unsigned int vertexId [[vertex_id]])
{
    const float2 positions[] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0),
    };
    const float2 coordinates[] = {
        float2(0.0,  1.0),
        float2(2.0,  1.0),
        float2(0.0, -1.0),
    };
    RasterData output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.textureCoordinate = coordinates[vertexId];
    return output;
}

// Where the presenter samples the scanout: origin + point * extent, clamped
// to the centres of the scanout rectangle's edge texels, maps the guest's
// top-left-origin unit square onto the rectangle in its texture, with a
// bottom-up texture's flip folded into a negative vertical extent.
struct ScanoutSampling {
    float2 origin;
    float2 extent;
    float2 minimum;
    float2 maximum;
    uint rotation;
};

// Rotation turns the guest image onto the display, so a display point maps
// back through the inverse turn into the guest's unit square.
float2 scanoutCoordinate(float2 coordinate, constant ScanoutSampling& sampling)
{
    switch (sampling.rotation & 3u) {
    case 1u:
        coordinate = float2(coordinate.y, 1.0 - coordinate.x);
        break;
    case 2u:
        coordinate = float2(1.0 - coordinate.x, 1.0 - coordinate.y);
        break;
    case 3u:
        coordinate = float2(1.0 - coordinate.y, coordinate.x);
        break;
    default:
        break;
    }
    return clamp(
        sampling.origin + coordinate * sampling.extent,
        sampling.minimum,
        sampling.maximum);
}

// A guest pixel magnified onto the display stays a sharp block, so text and
// pixel art keep their edges at Retina and integer scales. A scanout larger
// than its viewport is minified bilinearly, which blends each display pixel's
// nearest 2x2 texels instead of dropping whole rows and columns. The guest's
// textures have no mip chain, so a reduction beyond 2:1 still skips texels.
constexpr sampler scanoutSampler(
    mag_filter::nearest,
    min_filter::linear,
    address::clamp_to_edge);

fragment float4 displayFragment(
    RasterData input [[stage_in]],
    texture2d<float> scanout [[texture(0)]],
    constant ScanoutSampling& sampling [[buffer(0)]])
{
    const float4 color = scanout.sample(
        scanoutSampler,
        scanoutCoordinate(input.textureCoordinate, sampling));
    return float4(color.rgb, 1.0);
}

fragment float4 displayArrayFragment(
    RasterData input [[stage_in]],
    texture2d_array<float> scanout [[texture(0)]],
    constant ScanoutSampling& sampling [[buffer(0)]])
{
    const float4 color = scanout.sample(
        scanoutSampler,
        scanoutCoordinate(input.textureCoordinate, sampling),
        0);
    return float4(color.rgb, 1.0);
}
)metal";

std::string description(NSError* error)
{
    if (error == nil || error.localizedDescription == nil)
        return "unknown Metal error";
    const char* text = error.localizedDescription.UTF8String;
    return text == nullptr ? "unknown Metal error" : std::string(text);
}

} // namespace

ShaderScanoutSampling shaderSampling(
    const ScanoutSampling& sampling,
    DisplayRotation rotation) noexcept
{
    return {
        .origin = {
            static_cast<float>(sampling.origin.x),
            static_cast<float>(sampling.origin.y)},
        .extent = {
            static_cast<float>(sampling.extent.x),
            static_cast<float>(sampling.extent.y)},
        .minimum = {
            static_cast<float>(sampling.minimum.x),
            static_cast<float>(sampling.minimum.y)},
        .maximum = {
            static_cast<float>(sampling.maximum.x),
            static_cast<float>(sampling.maximum.y)},
        .rotation = static_cast<std::uint32_t>(rotation),
        .padding = 0,
    };
}

bool scanoutPassSamples(id<MTLTexture> texture) noexcept
{
    return texture != nil &&
        (texture.textureType == MTLTextureType2D ||
         (texture.textureType == MTLTextureType2DArray &&
          texture.arrayLength == 1));
}

std::optional<ScanoutSampling> scanoutPassSampling(
    id<MTLTexture> texture,
    const MetalScanout& scanout) noexcept
{
    return scanoutSampling({
        .texture = {
            .width = static_cast<std::uint32_t>(texture.width),
            .height = static_cast<std::uint32_t>(texture.height)},
        .source = {
            .width = scanout.sourceWidth,
            .height = scanout.sourceHeight},
        .x = scanout.sourceX,
        .y = scanout.sourceY,
        .width = scanout.width,
        .height = scanout.height,
        .flipped = scanout.flipped,
    });
}

id<MTLRenderPipelineState> ScanoutPipelines::forTexture(
    id<MTLTexture> texture) const noexcept
{
    return texture.textureType == MTLTextureType2DArray ?
        textureArray : texture2D;
}

ScanoutPipelines makeScanoutPipelines(
    id<MTLDevice> device,
    MTLPixelFormat pixelFormat)
{
    NSError* error = nil;
    id<MTLLibrary> library = [device
        newLibraryWithSource:metalShaderSource
        options:nil
        error:&error];
    if (library == nil) {
        throw std::runtime_error(
            "Metal display shader compilation failed: " +
            description(error));
    }
    id<MTLFunction> vertex = [library newFunctionWithName:@"displayVertex"];
    id<MTLFunction> fragment =
        [library newFunctionWithName:@"displayFragment"];
    id<MTLFunction> arrayFragment =
        [library newFunctionWithName:@"displayArrayFragment"];
    if (vertex == nil || fragment == nil || arrayFragment == nil)
        throw std::runtime_error("Metal display shader entry is missing");
    const auto makePipeline = [&](id<MTLFunction> selectedFragment) {
        MTLRenderPipelineDescriptor* descriptor =
            [[MTLRenderPipelineDescriptor alloc] init];
        descriptor.vertexFunction = vertex;
        descriptor.fragmentFunction = selectedFragment;
        descriptor.colorAttachments[0].pixelFormat = pixelFormat;
        return [device
            newRenderPipelineStateWithDescriptor:descriptor
            error:&error];
    };
    ScanoutPipelines pipelines {
        .texture2D = makePipeline(fragment),
        .textureArray = makePipeline(arrayFragment),
    };
    if (pipelines.texture2D == nil || pipelines.textureArray == nil)
        throw std::runtime_error(
            "Metal display pipeline creation failed: " +
            description(error));
    return pipelines;
}

} // namespace uml::gpu
