#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import "RFBProtocol.h"

// Shared by the iOS presenter and the offscreen macOS regression. RFB's
// fourth byte is unused, so always emit opaque pixels rather than interpreting
// it as an alpha channel. The top row must remain the top row after sampling.
static id<MTLRenderPipelineState> LRFramebufferPipeline(id<MTLDevice> device, NSError **error) {
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 position [[position]]; float2 uv; };\n"
        "vertex V lr_frame_vertex(uint i [[vertex_id]]) {\n"
        " const float2 p[4] = {float2(-1,1),float2(-1,-1),float2(1,1),float2(1,-1)};\n"
        " const float2 t[4] = {float2(0,0),float2(0,1),float2(1,0),float2(1,1)};\n"
        " return V{float4(p[i],0,1),t[i]}; }\n"
        "fragment float4 lr_frame_fragment(V v [[stage_in]], texture2d<float> image [[texture(0)]]) {\n"
        " constexpr sampler s(filter::nearest,address::clamp_to_edge);\n"
        " return float4(image.sample(s,v.uv).rgb,1); }\n";
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:error];
    if (!library) return nil;
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"lr_frame_vertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"lr_frame_fragment"];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    return [device newRenderPipelineStateWithDescriptor:descriptor error:error];
}

static id<MTLTexture> LRFramebufferTexture(id<MTLDevice> device, NSUInteger width, NSUInteger height) {
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:width height:height mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageShaderRead;
    return [device newTextureWithDescriptor:descriptor];
}

static void LREncodeFramebuffer(id<MTLRenderCommandEncoder> encoder, id<MTLRenderPipelineState> pipeline, id<MTLTexture> texture) {
    [encoder setRenderPipelineState:pipeline];
    [encoder setFragmentTexture:texture atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
}

// Shared production/test upload path. Whole-row stride with a subregion source
// offset preserves correct pixels outside the dirty rectangle.
static NSUInteger LRUploadFramebuffer(id<MTLTexture> texture, const void *pixels, LRFrameDamage damage) {
    if (!damage.width || !damage.height) return 0;
    const uint8_t *source = pixels;
    [texture replaceRegion:MTLRegionMake2D(damage.x,damage.y,damage.width,damage.height)
        mipmapLevel:0 withBytes:source+((NSUInteger)damage.y*texture.width+damage.x)*4
        bytesPerRow:texture.width*4];
    return (NSUInteger)damage.width*damage.height*4;
}

// Repeated redraws can read the last texture concurrently, but new pixels
// require a slot with no GPU readers. Bound total outstanding submissions.
static NSUInteger LRFramebufferSlot(const unsigned readers[3], BOOL reuse, NSUInteger last) {
    if (readers[0]+readers[1]+readers[2]>=3) return NSNotFound;
    if (reuse && last<3) return last;
    for(NSUInteger i=0;i<3;i++) if(!readers[i]) return i;
    return NSNotFound;
}
