#import "MetalFramebufferView.h"
#import "FramebufferMetalPipeline.h"

@interface LRMetalFramebufferView () <MTKViewDelegate>
@property id<MTLCommandQueue> queue;
@property id<MTLRenderPipelineState> pipeline;
@property NSData *pixels;
@property uint16_t pixelWidth, pixelHeight;
@property uint64_t generation, submittedGeneration;
@property (copy) void (^failure)(NSString *);
@property BOOL failed;
@end

@implementation LRMetalFramebufferView {
    id<MTLTexture> _textures[3];
    unsigned _inFlight[3];
    NSUInteger _lastSlot;
    uint64_t _reusedRedraws;
    LRFrameDamage _damage[3];
    uint64_t _received,_submitted,_coalesced,_uploadBytes;
    double _uploadMs,_gpuMs;
}
- (instancetype)initWithFailure:(void (^)(NSString *))failure {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return nil;
    NSError *error = nil;
    id<MTLRenderPipelineState> pipeline = LRFramebufferPipeline(device, &error);
    if (!pipeline) return nil;
    if ((self = [super initWithFrame:CGRectZero device:device])) {
        _queue = [device newCommandQueue];
        if (!_queue) return nil;
        _pipeline = pipeline;
        _failure = [failure copy];
        self.delegate = self;
        self.paused = YES;
        self.enableSetNeedsDisplay = YES; // No idle display-link polling.
        self.userInteractionEnabled = NO; // Input belongs to the shared viewport.
        self.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        self.clearColor = MTLClearColorMake(0, 0, 0, 1);
        self.framebufferOnly = YES;
    }
    return self;
}
- (void)presentPixels:(NSData *)pixels width:(uint16_t)width height:(uint16_t)height {
    [self presentPixels:pixels width:width height:height damage:(LRFrameDamage){0,0,width,height}];
}
- (void)presentPixels:(NSData *)pixels width:(uint16_t)width height:(uint16_t)height damage:(LRFrameDamage)damage {
    NSAssert(NSThread.isMainThread, @"Presentation state belongs to the main thread");
    size_t bytes;
    if (_failed || !lr_rfb_dimensions(width,height,&bytes) || pixels.length!=bytes) return;
    BOOL resized=width!=_pixelWidth||height!=_pixelHeight;
    if(resized || !lr_rfb_rectangle(width,height,damage.x,damage.y,damage.width,damage.height,&bytes))
        damage=(LRFrameDamage){0,0,width,height};
    // Each reusable slot may contain a DIFFERENT older frame. Accumulate every
    // incoming update for every slot, even while its GPU read is in flight.
    for(unsigned i=0;i<3;i++)_damage[i]=resized?damage:lr_damage_union(_damage[i],damage);
    _received++; if(_generation!=_submittedGeneration && _pixels)_coalesced++;
    _pixels = pixels; _pixelWidth = width; _pixelHeight = height; _generation++;
    self.hidden = NO; [self setNeedsDisplay];
}
- (NSDictionary *)presentationStatistics {
    return @{@"received":@(_received),@"submitted":@(_submitted),@"coalesced":@(_coalesced),
        @"reused_redraws":@(_reusedRedraws),@"upload_bytes":@(_uploadBytes),@"upload_cpu_ms":@(_uploadMs),@"gpu_ms":@(_gpuMs)};
}
- (void)clearFrame {
    memset(_damage,0,sizeof _damage); _pixelWidth=_pixelHeight=0;
    _pixels = nil; _generation++; self.hidden = YES;
}
- (void)didMoveToWindow {
    [super didMoveToWindow];
    if (self.window && _pixels) [self setNeedsDisplay];
}
- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
    if (_pixels) [self setNeedsDisplay];
}
- (void)fail:(NSString *)reason {
    if (_failed) return;
    _failed = YES; self.hidden = YES;
    _failure(reason);
}
- (void)drawInMTKView:(MTKView *)view {
    if (_failed || !_pixels || !self.window || self.hidden || UIApplication.sharedApplication.applicationState != UIApplicationStateActive) return;
    BOOL reuse=_generation==_submittedGeneration && _textures[_lastSlot]!=nil;
    NSUInteger slot=LRFramebufferSlot(_inFlight,reuse,_lastSlot);
    if (slot == NSNotFound) return; // Keep the latest snapshot; completion schedules it.
    id<CAMetalDrawable> drawable = view.currentDrawable;
    MTLRenderPassDescriptor *pass = view.currentRenderPassDescriptor;
    if (!drawable || !pass) return; // Hidden/occluded views can lack a drawable.
    id<MTLTexture> texture = _textures[slot];
    if (!texture || texture.width != _pixelWidth || texture.height != _pixelHeight) {
        texture = LRFramebufferTexture(self.device, _pixelWidth, _pixelHeight);
        if (!texture) { [self fail:@"Framebuffer texture allocation failed"]; return; }
        _textures[slot] = texture;
        _damage[slot]=(LRFrameDamage){0,0,_pixelWidth,_pixelHeight};
    }
    // Never update a texture while its previous command buffer is reading it.
    if(!reuse) {
    double uploadStart=NSProcessInfo.processInfo.systemUptime;
    _uploadBytes+=LRUploadFramebuffer(texture,_pixels.bytes,_damage[slot]);
    _uploadMs+=(NSProcessInfo.processInfo.systemUptime-uploadStart)*1000;
    _damage[slot]=(LRFrameDamage){0};
    } else _reusedRedraws++;
    _submitted++;
    id<MTLCommandBuffer> command = [_queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!command || !encoder) { [self fail:@"Framebuffer command encoding failed"]; return; }
    LREncodeFramebuffer(encoder, _pipeline, texture);
    [encoder endEncoding];
    [command presentDrawable:drawable];
    _inFlight[slot]++; _lastSlot=slot; _submittedGeneration = _generation;
    __weak typeof(self) weak = self;
    [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        dispatch_async(dispatch_get_main_queue(), ^{
            typeof(self) self = weak; if (!self) return;
            self->_inFlight[slot]--;
            if(completed.GPUEndTime>=completed.GPUStartTime)
                self->_gpuMs+=(completed.GPUEndTime-completed.GPUStartTime)*1000;
            if (completed.status == MTLCommandBufferStatusError) {
                [self fail:completed.error.localizedDescription ?: @"Framebuffer GPU command failed"];
            } else if (self.generation != self.submittedGeneration && self.pixels) {
                [self setNeedsDisplay];
            }
        });
    }];
    [command commit];
}
@end
