#import <Foundation/Foundation.h>
#import "app/LinuxRunner/FramebufferMetalPipeline.h"

// Real Metal commands on the macOS GPU. Tests the exact iOS shader, not an
// imitation renderer; this does not establish physical-iPhone MTKView behavior.
int main(int argc, char **argv) { @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) { fprintf(stderr, "SKIP: no Metal device\n"); return 77; }
    NSError *error = nil;
    id<MTLRenderPipelineState> pipeline = LRFramebufferPipeline(device, &error);
    if (!pipeline) { fprintf(stderr, "%s\n", error.description.UTF8String); return 1; }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    NSMutableArray *timings = [NSMutableArray new];
    const unsigned resolutions[][2] = {{640,480},{800,600},{1024,768},{960,540},{17,11}};
    unsigned checks = 0;
    for (unsigned test = 0; test < 5; test++) {
        NSUInteger width = resolutions[test][0], height = resolutions[test][1];
        // Includes nonuniform nearest-neighbor scaling and odd dimensions.
        NSUInteger scale = test == 4 ? 2 : 1;
        id<MTLTexture> input = LRFramebufferTexture(device, width, height);
        MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:width*scale height:height*scale mipmapped:NO];
        desc.storageMode = MTLStorageModeShared; desc.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> output = [device newTextureWithDescriptor:desc];
        if (!input || !output || !queue) return 1;
        NSMutableData *pixels = [NSMutableData dataWithLength:width*height*4];
        NSMutableData *readback = [NSMutableData dataWithLength:width*height*scale*scale*4];
        uint8_t *src = pixels.mutableBytes;
        for (unsigned iteration = 0; iteration < 12; iteration++) {
            for (NSUInteger y=0;y<height;y++) for (NSUInteger x=0;x<width;x++) {
                NSUInteger i=(y*width+x)*4;
                src[i]=(uint8_t)(x+iteration);src[i+1]=(uint8_t)(y+iteration*3);src[i+2]=(uint8_t)(x^y);src[i+3]=(uint8_t)(x+y);
            }
            NSData *before = pixels.copy;
            double start = NSProcessInfo.processInfo.systemUptime;
            [input replaceRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0 withBytes:src bytesPerRow:width*4];
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture=output;pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
            LREncodeFramebuffer(encoder,pipeline,input);[encoder endEncoding];[command commit];[command waitUntilCompleted];
            if (command.status!=MTLCommandBufferStatusCompleted) { fprintf(stderr,"GPU failure: %s\n",command.error.description.UTF8String);return 1; }
            [output getBytes:readback.mutableBytes bytesPerRow:width*scale*4 fromRegion:MTLRegionMake2D(0,0,width*scale,height*scale) mipmapLevel:0];
            if (test==3 && iteration>=2) [timings addObject:@((NSProcessInfo.processInfo.systemUptime-start)*1000)];
            const uint8_t *dst=readback.bytes;
            for (NSUInteger y=0;y<height*scale;y++) for (NSUInteger x=0;x<width*scale;x++) {
                NSUInteger a=((y/scale)*width+x/scale)*4,b=(y*width*scale+x)*4;
                if (memcmp(src+a,dst+b,3)!=0 || dst[b+3]!=255) { fprintf(stderr,"FAIL: %lux%lu pixel %lu,%lu\n",width,height,x,y);return 1; }
            }
            if (![before isEqual:pixels]) { fprintf(stderr,"FAIL: source modified\n");return 1; }
            checks++;
        }
    }
    [timings sortUsingSelector:@selector(compare:)];
    NSDictionary *result=@{@"environment":@"macOS host GPU",@"device":device.name,@"checks":@(checks),@"result":@"PASS",@"coverage":@"BGRA channels, opaque alpha, row orientation, nearest scaling, source immutability, repeated dimensions",@"960x540_upload_render_sync_readback_median_ms":timings[timings.count/2],@"960x540_upload_render_sync_readback_max_ms":timings.lastObject,@"physical_device":@"NOT TESTED",@"performance_scope":@"Component timing only; includes synchronization/readback. No browser or display speedup claim."};
    NSData *json=[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:nil];
    if(argc==2)[json writeToFile:@(argv[1]) atomically:YES];
    puts([[NSString alloc]initWithData:json encoding:NSUTF8StringEncoding].UTF8String);
    return 0;
} }
