// GPU-resident textures for the Mesa bridge (LR_GPU_RESIDENT), included by
// GuestMetalBackend.h. A resident texture is a Metal render texture in the
// backend's top-left orientation; when sampled after rendering, a GPU copy in GL
// row order (bottom row first) is refreshed, matching LR_GPU_TEXTURE storage.
// Pixels cross the CPU only for explicit UPLOAD/READ requests from the bridge.
@interface LRGuestResident : NSObject
@property(nonatomic,strong) id<MTLTexture> render,sampled,renderSrgb,sampledSrgb;
@property(nonatomic) unsigned format,width,height;
@property(nonatomic) NSUInteger bytes;
@property(nonatomic) BOOL sampledStale;
@end
@implementation LRGuestResident
@end

static MTLPixelFormat LRResidentPixelFormat(unsigned format){
 if(format==LR_GPU_RES_FORMAT_DEPTH)return MTLPixelFormatDepth32Float;
 if(format==LR_GPU_RES_FORMAT_STENCIL)return MTLPixelFormatStencil8;
 if(format==LR_GPU_RES_FORMAT_R8)return MTLPixelFormatR8Unorm;
 if(format==LR_GPU_RES_FORMAT_RG8)return MTLPixelFormatRG8Unorm;
 if(format==LR_GPU_RES_FORMAT_SRGB8_A8)return MTLPixelFormatRGBA8Unorm;   // raw bytes; sRGB through views
 return LRGuestInteropFormat(format);
}
static BOOL LRResidentIsColor(unsigned format){return format!=LR_GPU_RES_FORMAT_DEPTH&&format!=LR_GPU_RES_FORMAT_STENCIL;}
// sRGB view of a resident's storage (render or sampled), created once.
static id<MTLTexture> LRResidentSrgbView(id<MTLTexture> texture,id<MTLTexture> __strong *cache){
 if(!*cache||(*cache).parentTexture!=texture)*cache=[texture newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB];
 return *cache;
}
static BOOL LRResidentIsInteger(MTLPixelFormat f){return f==MTLPixelFormatRGBA16Uint||f==MTLPixelFormatRGBA16Sint||f==MTLPixelFormatRGBA32Uint||f==MTLPixelFormatRGBA32Sint;}

static id<MTLCommandBuffer> LRResidentCommand(LRGuestMetalContext *c){
 // Separate passes share the pending command buffer; no CPU wait between them.
 if(c.encoder){[c.encoder endEncoding];c.encoder=nil;}
 if(!c.pending)c.pending=[c.queue commandBuffer];
 return c.pending;
}

// Whole-image clear as its own render pass in the pending command buffer.
static int LRResidentClearTexture(LRGuestMetalContext *c,id<MTLTexture> texture,unsigned format,const float color[4],const uint32_t bits[4],float depth,unsigned stencil){
 id<MTLCommandBuffer> command=LRResidentCommand(c);if(!command)return -5;
 MTLRenderPassDescriptor *d=[MTLRenderPassDescriptor renderPassDescriptor];
 if(format==LR_GPU_RES_FORMAT_DEPTH){d.depthAttachment.texture=texture;d.depthAttachment.loadAction=MTLLoadActionClear;d.depthAttachment.clearDepth=depth;d.depthAttachment.storeAction=MTLStoreActionStore;}
 else if(format==LR_GPU_RES_FORMAT_STENCIL){d.stencilAttachment.texture=texture;d.stencilAttachment.loadAction=MTLLoadActionClear;d.stencilAttachment.clearStencil=stencil&255;d.stencilAttachment.storeAction=MTLStoreActionStore;}
 else{d.colorAttachments[0].texture=texture;d.colorAttachments[0].loadAction=MTLLoadActionClear;d.colorAttachments[0].storeAction=MTLStoreActionStore;
  if(LRResidentIsInteger(texture.pixelFormat))d.colorAttachments[0].clearColor=MTLClearColorMake(bits[0],bits[1],bits[2],bits[3]);
  else d.colorAttachments[0].clearColor=MTLClearColorMake(color[0],color[1],color[2],color[3]);}
 id<MTLRenderCommandEncoder> e=[command renderCommandEncoderWithDescriptor:d];if(!e)return -5;[e endEncoding];c.residentClears++;return 0;
}

// Scissored / write-masked clear (glClear with GL_SCISSOR_TEST or partial
// masks): a full-target triangle in its own pass, clipped by the scissor rect.
// Colour uses a constant fragment (float/uint/int by format), depth writes the
// vertex z with an always-pass test, stencil replaces through the write mask.
static int LRResidentClearRegion(LRGuestMetalContext *c,id<MTLTexture> texture,unsigned format,const struct lr_gpu_resident *r){
 unsigned W=(unsigned)texture.width,H=(unsigned)texture.height;
 if(r->x>=W||r->y>=H||!r->width||!r->height)return 0;   // empty scissor: nothing to clear
 unsigned w=MIN(r->width,W-r->x),h=MIN(r->height,H-r->y);
 if(!c.residentClearLibrary){NSError *error=nil;
  NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\nstruct V{float4 p [[position]];};\n"
   "vertex V lr_clear_v(uint i [[vertex_id]],constant float &z [[buffer(0)]]){V o;float2 q=float2((i<<1)&2,i&2);o.p=float4(q*2.0-1.0,z,1.0);return o;}\n"
   "fragment float4 lr_clear_f(constant float4 &c [[buffer(0)]]){return c;}\nfragment uint4 lr_clear_u(constant uint4 &c [[buffer(0)]]){return c;}\nfragment int4 lr_clear_i(constant int4 &c [[buffer(0)]]){return c;}\n";
  c.residentClearLibrary=[c.device newLibraryWithSource:source options:nil error:&error];if(!c.residentClearLibrary){fprintf(stderr,"GUEST_METAL_RESIDENT_CLEAR_LIBRARY %s\n",error.localizedDescription.UTF8String);return -5;}
  c.residentClearStates=[NSMutableDictionary new];}
 BOOL depth=format==LR_GPU_RES_FORMAT_DEPTH,stencil=format==LR_GPU_RES_FORMAT_STENCIL;MTLPixelFormat pf=texture.pixelFormat;
 unsigned mask=depth||stencil?0:(r->masks[0]&15);if(!depth&&!stencil&&!mask)return 0;
 NSString *key=[NSString stringWithFormat:@"%lu-%u",(unsigned long)pf,mask];id<MTLRenderPipelineState> pipeline=c.residentClearStates[key];
 if(!pipeline){MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new];d.vertexFunction=[c.residentClearLibrary newFunctionWithName:@"lr_clear_v"];
  if(depth)d.depthAttachmentPixelFormat=pf;else if(stencil)d.stencilAttachmentPixelFormat=pf;
  else{d.colorAttachments[0].pixelFormat=pf;d.fragmentFunction=[c.residentClearLibrary newFunctionWithName:LRResidentIsInteger(pf)?(pf==MTLPixelFormatRGBA16Uint||pf==MTLPixelFormatRGBA32Uint?@"lr_clear_u":@"lr_clear_i"):@"lr_clear_f"];
   d.colorAttachments[0].writeMask=((mask&1)?MTLColorWriteMaskRed:0)|((mask&2)?MTLColorWriteMaskGreen:0)|((mask&4)?MTLColorWriteMaskBlue:0)|((mask&8)?MTLColorWriteMaskAlpha:0);}
  NSError *error=nil;pipeline=[c.device newRenderPipelineStateWithDescriptor:d error:&error];if(!pipeline){fprintf(stderr,"GUEST_METAL_RESIDENT_CLEAR_PIPELINE %s\n",error.localizedDescription.UTF8String);return -5;}
  c.residentClearStates[key]=pipeline;}
 id<MTLDepthStencilState> state=nil;
 if(depth||stencil){NSString *dkey=[NSString stringWithFormat:@"ds-%d-%u",depth,r->masks[1]&255];state=c.residentClearStates[dkey];
  if(!state){MTLDepthStencilDescriptor *ds=[MTLDepthStencilDescriptor new];ds.depthCompareFunction=MTLCompareFunctionAlways;ds.depthWriteEnabled=depth;
   if(stencil){MTLStencilDescriptor *sd=[MTLStencilDescriptor new];sd.stencilCompareFunction=MTLCompareFunctionAlways;sd.depthStencilPassOperation=MTLStencilOperationReplace;sd.writeMask=r->masks[1]&255;sd.readMask=255;ds.frontFaceStencil=sd;ds.backFaceStencil=sd;}
   state=[c.device newDepthStencilStateWithDescriptor:ds];if(!state)return -5;c.residentClearStates[dkey]=state;}}
 id<MTLCommandBuffer> command=LRResidentCommand(c);if(!command)return -5;
 MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
 if(depth){pass.depthAttachment.texture=texture;pass.depthAttachment.loadAction=MTLLoadActionLoad;pass.depthAttachment.storeAction=MTLStoreActionStore;}
 else if(stencil){pass.stencilAttachment.texture=texture;pass.stencilAttachment.loadAction=MTLLoadActionLoad;pass.stencilAttachment.storeAction=MTLStoreActionStore;}
 else{pass.colorAttachments[0].texture=texture;pass.colorAttachments[0].loadAction=MTLLoadActionLoad;pass.colorAttachments[0].storeAction=MTLStoreActionStore;}
 id<MTLRenderCommandEncoder> e=[command renderCommandEncoderWithDescriptor:pass];if(!e)return -5;
 [e setRenderPipelineState:pipeline];if(state){[e setDepthStencilState:state];[e setStencilReferenceValue:r->clear_stencil&255];}
 [e setScissorRect:(MTLScissorRect){r->x,H-(r->y+h),w,h}];   // GL rows count from the bottom
 float z=r->clear_depth<0?0:r->clear_depth>1?1:r->clear_depth;[e setVertexBytes:&z length:sizeof z atIndex:0];
 if(LRResidentIsInteger(pf))[e setFragmentBytes:r->clear_bits length:sizeof r->clear_bits atIndex:0];else if(!depth&&!stencil)[e setFragmentBytes:r->clear length:sizeof r->clear atIndex:0];
 [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[e endEncoding];c.residentClears++;return 0;
}

// glBlitFramebuffer between resident images: a pass over the destination
// (scissored to the destination rectangle and GL scissor) whose fragments map
// back to source coordinates; handles scaling, mirrored rectangles, NEAREST /
// LINEAR, and depth (NEAREST). Per-fragment state other than the scissor does
// not apply to blits, so there is no blending or masking.
struct LRBlitParams {float dst0[2],dst1[2],src0[2],src1[2],src_size[2],dst_height,pad;};
static int LRResidentBlit(LRGuestMetalContext *c,LRGuestResident *src,id<MTLTexture> source,LRGuestResident *dst,id<MTLTexture> target,const struct lr_gpu_resident *r){
 BOOL depth=dst.format==LR_GPU_RES_FORMAT_DEPTH;
 if(depth!=(src.format==LR_GPU_RES_FORMAT_DEPTH)||src.format==LR_GPU_RES_FORMAT_STENCIL||dst.format==LR_GPU_RES_FORMAT_STENCIL)return -22;
 if(LRResidentIsInteger(source.pixelFormat)||LRResidentIsInteger(target.pixelFormat)||source==target||src.render==dst.render)return -22;
 if(r->min_filter!=0x2600&&(r->min_filter!=0x2601||depth))return -22;
 const int32_t *q=r->rect;unsigned W=(unsigned)target.width,H=(unsigned)target.height;
 long x0=MIN(q[4],q[6]),x1=MAX(q[4],q[6]),y0=MIN(q[5],q[7]),y1=MAX(q[5],q[7]);
 if(r->flags&LR_GPU_RES_FLAG_REGION){x0=MAX(x0,(long)r->x);y0=MAX(y0,(long)r->y);x1=MIN(x1,(long)r->x+(long)r->width);y1=MIN(y1,(long)r->y+(long)r->height);}
 x0=MAX(x0,0);y0=MAX(y0,0);x1=MIN(x1,(long)W);y1=MIN(y1,(long)H);if(x1<=x0||y1<=y0||q[0]==q[2]||q[1]==q[3])return 0;   // nothing to write
 if(!c.residentBlitLibrary){NSError *error=nil;
  NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\nstruct V{float4 p [[position]];};\n"
   "struct P{float2 d0,d1,s0,s1,size;float dh,pad;};\n"
   "vertex V lr_blit_v(uint i [[vertex_id]]){V o;float2 q=float2((i<<1)&2,i&2);o.p=float4(q*2.0-1.0,0,1);return o;}\n"
   "static float2 src_at(float4 p,constant P &k){float2 g=float2(p.x,k.dh-p.y);return k.s0+(g-k.d0)*(k.s1-k.s0)/(k.d1-k.d0);}\n"
   "fragment float4 lr_blit_nearest(V v [[stage_in]],constant P &k [[buffer(0)]],texture2d<float,access::read> s [[texture(0)]]){float2 g=src_at(v.p,k);int2 t=int2(floor(g));t=clamp(t,int2(0),int2(k.size)-1);return s.read(uint2(t.x,uint(k.size.y)-1-uint(t.y)));}\n"
   "fragment float4 lr_blit_linear(V v [[stage_in]],constant P &k [[buffer(0)]],texture2d<float> s [[texture(0)]],sampler f [[sampler(0)]]){float2 g=src_at(v.p,k);return s.sample(f,float2(g.x,k.size.y-g.y)/k.size);}\n"
   "struct D{float d [[depth(any)]];};\nfragment D lr_blit_depth(V v [[stage_in]],constant P &k [[buffer(0)]],depth2d<float,access::read> s [[texture(0)]]){float2 g=src_at(v.p,k);int2 t=clamp(int2(floor(g)),int2(0),int2(k.size)-1);D o;o.d=s.read(uint2(t.x,uint(k.size.y)-1-uint(t.y)));return o;}\n";
  c.residentBlitLibrary=[c.device newLibraryWithSource:source options:nil error:&error];if(!c.residentBlitLibrary){fprintf(stderr,"GUEST_METAL_RESIDENT_BLIT_LIBRARY %s\n",error.localizedDescription.UTF8String);return -5;}}
 if(!c.residentClearStates)c.residentClearStates=[NSMutableDictionary new];
 NSString *function=depth?@"lr_blit_depth":r->min_filter==0x2601?@"lr_blit_linear":@"lr_blit_nearest";
 NSString *key=[NSString stringWithFormat:@"blit-%@-%lu",function,(unsigned long)target.pixelFormat];id<MTLRenderPipelineState> pipeline=c.residentClearStates[key];
 if(!pipeline){MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new];d.vertexFunction=[c.residentBlitLibrary newFunctionWithName:@"lr_blit_v"];d.fragmentFunction=[c.residentBlitLibrary newFunctionWithName:function];
  if(depth)d.depthAttachmentPixelFormat=target.pixelFormat;else d.colorAttachments[0].pixelFormat=target.pixelFormat;
  NSError *error=nil;pipeline=[c.device newRenderPipelineStateWithDescriptor:d error:&error];if(!pipeline){fprintf(stderr,"GUEST_METAL_RESIDENT_BLIT_PIPELINE %s\n",error.localizedDescription.UTF8String);return -5;}c.residentClearStates[key]=pipeline;}
 id<MTLDepthStencilState> state=nil;if(depth){state=c.residentClearStates[@"blit-depth-state"];if(!state){MTLDepthStencilDescriptor *ds=[MTLDepthStencilDescriptor new];ds.depthCompareFunction=MTLCompareFunctionAlways;ds.depthWriteEnabled=YES;state=[c.device newDepthStencilStateWithDescriptor:ds];if(!state)return -5;c.residentClearStates[@"blit-depth-state"]=state;}}
 id<MTLSamplerState> sampler=nil;if(r->min_filter==0x2601){sampler=c.residentClearStates[@"blit-linear"];if(!sampler){MTLSamplerDescriptor *sd=[MTLSamplerDescriptor new];sd.minFilter=sd.magFilter=MTLSamplerMinMagFilterLinear;sd.sAddressMode=sd.tAddressMode=MTLSamplerAddressModeClampToEdge;sampler=[c.device newSamplerStateWithDescriptor:sd];if(!sampler)return -5;c.residentClearStates[@"blit-linear"]=sampler;}}
 id<MTLCommandBuffer> command=LRResidentCommand(c);if(!command)return -5;
 MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
 if(depth){pass.depthAttachment.texture=target;pass.depthAttachment.loadAction=MTLLoadActionLoad;pass.depthAttachment.storeAction=MTLStoreActionStore;}
 else{pass.colorAttachments[0].texture=target;pass.colorAttachments[0].loadAction=MTLLoadActionLoad;pass.colorAttachments[0].storeAction=MTLStoreActionStore;}
 id<MTLRenderCommandEncoder> e=[command renderCommandEncoderWithDescriptor:pass];if(!e)return -5;
 [e setRenderPipelineState:pipeline];if(state)[e setDepthStencilState:state];
 [e setScissorRect:(MTLScissorRect){(NSUInteger)x0,(NSUInteger)(H-y1),(NSUInteger)(x1-x0),(NSUInteger)(y1-y0)}];
 struct LRBlitParams k={{q[4],q[5]},{q[6],q[7]},{q[0],q[1]},{q[2],q[3]},{(float)source.width,(float)source.height},(float)H,0};
 [e setFragmentBytes:&k length:sizeof k atIndex:0];[e setFragmentTexture:source atIndex:0];if(sampler)[e setFragmentSamplerState:sampler atIndex:0];
 [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];[e endEncoding];dst.sampledStale=YES;c.residentBlits++;return 0;
}

// Depth32Float render image -> R32Float sampled copy in GL row order.
static int LRResidentDepthFlip(LRGuestMetalContext *c,id<MTLTexture> source,id<MTLTexture> dest){
 if(!source||!dest||source.width!=dest.width||source.height!=dest.height)return -22;
 if(!c.residentClearStates)c.residentClearStates=[NSMutableDictionary new];
 id<MTLComputePipelineState> pipeline=c.residentClearStates[@"depth-flip"];
 if(!pipeline){NSError *error=nil;
  id<MTLLibrary> library=[c.device newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nkernel void lr_depth_flip(depth2d<float,access::read> s [[texture(0)]],texture2d<float,access::write> d [[texture(1)]],uint2 p [[thread_position_in_grid]]){if(p.x<s.get_width()&&p.y<s.get_height())d.write(float4(s.read(p),0,0,1),uint2(p.x,s.get_height()-1-p.y));}\n" options:nil error:&error];
  pipeline=library?[c.device newComputePipelineStateWithFunction:[library newFunctionWithName:@"lr_depth_flip"] error:&error]:nil;
  if(!pipeline){fprintf(stderr,"GUEST_METAL_RESIDENT_DEPTH_FLIP %s\n",error.localizedDescription.UTF8String);return -5;}
  c.residentClearStates[@"depth-flip"]=pipeline;}
 id<MTLCommandBuffer> command=LRResidentCommand(c);if(!command)return -5;
 id<MTLComputeCommandEncoder> e=[command computeCommandEncoder];if(!e)return -5;
 [e setComputePipelineState:pipeline];[e setTexture:source atIndex:0];[e setTexture:dest atIndex:1];
 NSUInteger rows=MIN((NSUInteger)16,pipeline.maxTotalThreadsPerThreadgroup/16);if(!rows){[e endEncoding];return -5;}
 [e dispatchThreadgroups:MTLSizeMake((source.width+15)/16,(source.height+rows-1)/rows,1) threadsPerThreadgroup:MTLSizeMake(16,rows,1)];[e endEncoding];return 0;
}

// Copy rows bottom-up between the two orientations (also valid for depth,
// stencil and integer formats, which the float compute flip cannot read).
static int LRResidentFlipCopy(LRGuestMetalContext *c,id<MTLTexture> source,id<MTLTexture> dest){
 if(!source||!dest||source.width!=dest.width||source.height!=dest.height||source.pixelFormat!=dest.pixelFormat)return -22;
 id<MTLCommandBuffer> command=LRResidentCommand(c);if(!command)return -5;
 MTLPixelFormat f=source.pixelFormat;
 if(c.flipPipeline&&(f==MTLPixelFormatRGBA8Unorm||f==MTLPixelFormatRGBA16Float||f==MTLPixelFormatR8Unorm||f==MTLPixelFormatRG8Unorm))return LRGuestFlipTexture(c,command,source,dest);
 id<MTLBlitCommandEncoder> b=[command blitCommandEncoder];if(!b)return -5;
 for(NSUInteger y=0;y<source.height;y++)[b copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,y,0) sourceSize:MTLSizeMake(source.width,1,1) toTexture:dest destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,source.height-1-y,0)];
 [b endEncoding];c.rowCopies+=source.height;return 0;
}

// CPU transfer of a whole image in GL row order. Colour textures are shared;
// depth/stencil textures are private and go through a staging buffer.
// CPU transfer of a whole image, or of the rectangle x,y,w,h (GL lower-left
// origin), in GL row order. Colour textures are shared; depth/stencil textures
// are private and go through a staging buffer.
static int LRResidentTransfer(LRGuestMetalContext *c,LRGuestResident *t,void *pixels,size_t bytes,BOOL read,unsigned x,unsigned y,unsigned w,unsigned h){
 unsigned bpp=lr_gpu_resident_pixel_bytes(t.format),H=t.height;
 if(!w||!h||x>t.width||y>t.height||w>t.width-x||h>t.height-y||bytes!=(size_t)w*h*bpp)return -22;
 int error=LRGuestMetalFinish(c);if(error)return error;
 if(LRResidentIsColor(t.format)){
  for(unsigned row=0;row<h;row++){char *line=(char *)pixels+(size_t)row*w*bpp;MTLRegion region=MTLRegionMake2D(x,H-(y+row)-1,w,1);
   if(read)[t.render getBytes:line bytesPerRow:w*bpp fromRegion:region mipmapLevel:0];else [t.render replaceRegion:region mipmapLevel:0 withBytes:line bytesPerRow:w*bpp];}
  if(read)c.residentReads++;else{c.residentUploads++;t.sampledStale=YES;}
  return 0;
 }
 NSUInteger pitch=((NSUInteger)w*bpp+255)&~(NSUInteger)255,size=pitch*h;
 id<MTLBuffer> staging=[c.device newBufferWithLength:size options:MTLResourceStorageModeShared];if(!staging)return -12;
 if(!read)for(unsigned row=0;row<h;row++)memcpy((char *)staging.contents+(size_t)(h-row-1)*pitch,(char *)pixels+(size_t)row*w*bpp,(size_t)w*bpp);
 id<MTLCommandBuffer> command=[c.queue commandBuffer];id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];if(!blit)return -5;
 MTLOrigin origin=MTLOriginMake(x,H-(y+h),0);
 if(read)[blit copyFromTexture:t.render sourceSlice:0 sourceLevel:0 sourceOrigin:origin sourceSize:MTLSizeMake(w,h,1) toBuffer:staging destinationOffset:0 destinationBytesPerRow:pitch destinationBytesPerImage:size];
 else [blit copyFromBuffer:staging sourceOffset:0 sourceBytesPerRow:pitch sourceBytesPerImage:size sourceSize:MTLSizeMake(w,h,1) toTexture:t.render destinationSlice:0 destinationLevel:0 destinationOrigin:origin];
 [blit endEncoding];[command commit];[command waitUntilCompleted];
 if(command.status!=MTLCommandBufferStatusCompleted){fprintf(stderr,"GUEST_METAL_RESIDENT_TRANSFER_ERROR %s\n",command.error.localizedDescription.UTF8String);return -5;}
 if(read){for(unsigned row=0;row<h;row++)memcpy((char *)pixels+(size_t)row*w*bpp,(char *)staging.contents+(size_t)(h-row-1)*pitch,(size_t)w*bpp);c.residentReads++;}
 else{c.residentUploads++;t.sampledStale=YES;}
 return 0;
}

static id<MTLTexture> LRResidentScratch(LRGuestMetalContext *c,MTLPixelFormat format,unsigned w,unsigned h,id<MTLTexture> current){
 if(current&&current.width==w&&current.height==h)return current;
 MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageRenderTarget;
 return [c.device newTextureWithDescriptor:d];
}

static int LRGuestMetalResident(void *context,const struct lr_gpu_resident *r,void *pixels,size_t pixel_bytes){
 @autoreleasepool {
 LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;
 if(!c.residents)c.residents=[NSMutableDictionary new];
 if(r->command!=LR_GPU_RES_BIND&&(r->id<1||r->id>LR_GPU_MAX_RESIDENT))return -22;
 LRGuestResident *t=r->command==LR_GPU_RES_BIND?nil:c.residents[@(r->id)];
 switch(r->command){
 case LR_GPU_RES_DEFINE:{
  MTLPixelFormat format=LRResidentPixelFormat(r->format);unsigned bpp=lr_gpu_resident_pixel_bytes(r->format);
  if(format==MTLPixelFormatInvalid||!bpp||r->width<1||r->height<1||r->width>LR_GPU_RESIDENT_MAX_DIMENSION||r->height>LR_GPU_RESIDENT_MAX_DIMENSION||(size_t)r->width*r->height*bpp>LR_GPU_RESIDENT_MAX_IMAGE)return -22;
  NSUInteger bytes=(NSUInteger)r->width*r->height*bpp*2;   // render + sampled copies
  NSUInteger existing=t?t.bytes:0;if(bytes>LR_GPU_RESIDENT_BUDGET-(c.residentBytes-existing))return -12;
  if(t&&c.residentBound){int e=LRGuestResidentRelease(c);if(e)return e;}   // never reallocate a bound target
  MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:r->width height:r->height mipmapped:NO];
  BOOL color=LRResidentIsColor(r->format);d.storageMode=color?MTLStorageModeShared:MTLStorageModePrivate;
  d.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead|(color?MTLTextureUsageShaderWrite|MTLTextureUsagePixelFormatView:0);
  id<MTLTexture> render=[c.device newTextureWithDescriptor:d];if(!render)return -12;
  LRGuestResident *n=[LRGuestResident new];n.render=render;n.format=r->format;n.width=r->width;n.height=r->height;n.bytes=bytes;n.sampledStale=YES;
  c.residentBytes=c.residentBytes-existing+bytes;c.residents[@(r->id)]=n;[c.textures removeObjectForKey:@(LR_GPU_RESIDENT_SAMPLER_BASE+r->id)];
  const float color0[4]={0,0,0,1};const uint32_t bits0[4]={0,0,0,1};
  return LRResidentClearTexture(c,render,r->format,color0,bits0,1,0);
 }
 case LR_GPU_RES_UPLOAD:case LR_GPU_RES_READ:
  if(!t)return -22;
  if(r->flags&LR_GPU_RES_FLAG_REGION)return LRResidentTransfer(c,t,pixels,pixel_bytes,r->command==LR_GPU_RES_READ,r->x,r->y,r->width,r->height);
  return LRResidentTransfer(c,t,pixels,pixel_bytes,r->command==LR_GPU_RES_READ,0,0,t.width,t.height);
 case LR_GPU_RES_CLEAR:
  if(!t)return -22;t.sampledStale=YES;{id<MTLTexture> target=t.render;if((r->flags&LR_GPU_RES_FLAG_SRGB)&&t.format==LR_GPU_RES_FORMAT_SRGB8_A8){id<MTLTexture> view=t.renderSrgb;target=LRResidentSrgbView(t.render,&view);t.renderSrgb=view;if(!target)return -12;}
  if(r->flags&LR_GPU_RES_FLAG_REGION)return LRResidentClearRegion(c,target,t.format,r);
  return LRResidentClearTexture(c,target,t.format,r->clear,r->clear_bits,r->clear_depth,r->clear_stencil);}
 case LR_GPU_RES_DELETE:{
  if(!t)return 0;
  if(c.residentBound&&(c.target==t.render||c.depth==t.render||c.stencil==t.render||[c.mrtColors containsObject:t.render])){int e=LRGuestResidentRelease(c);if(e)return e;}
  c.residentBytes-=t.bytes;[c.residents removeObjectForKey:@(r->id)];[c.textures removeObjectForKey:@(LR_GPU_RESIDENT_SAMPLER_BASE+r->id)];return 0;
 }
 case LR_GPU_RES_BIND:{
  if(r->count==0)return LRGuestResidentRelease(c);
  if(r->count>LR_GPU_MAX_COLOR_TARGETS)return -22;
  NSMutableArray<id<MTLTexture>> *colors=[NSMutableArray new];unsigned w=0,h=0;uint32_t masks[4]={0};BOOL srgb=NO;
  for(unsigned i=0;i<r->count;i++){LRGuestResident *ct=c.residents[@(r->color[i])];if(!ct||!LRResidentIsColor(ct.format)||r->masks[i]>15)return -22;
   if(i&&(ct.width!=w||ct.height!=h))return -22;w=ct.width;h=ct.height;masks[i]=r->masks[i];
   id<MTLTexture> render=ct.render;if((r->flags&LR_GPU_RES_FLAG_SRGB)&&ct.format==LR_GPU_RES_FORMAT_SRGB8_A8){id<MTLTexture> view=ct.renderSrgb;render=LRResidentSrgbView(ct.render,&view);ct.renderSrgb=view;if(!render)return -12;srgb=YES;}
   [colors addObject:render];}
  LRGuestResident *dt=r->depth?c.residents[@(r->depth)]:nil,*st=r->stencil?c.residents[@(r->stencil)]:nil;
  if((r->depth&&(!dt||dt.format!=LR_GPU_RES_FORMAT_DEPTH||dt.width!=w||dt.height!=h))||(r->stencil&&(!st||st.format!=LR_GPU_RES_FORMAT_STENCIL||st.width!=w||st.height!=h)))return -22;
  id<MTLTexture> wantDepth=dt?dt.render:nil,wantStencil=st?st.render:nil;
  // Same attachments as the open pass: keep the encoder (consecutive draws
  // share one render pass); only the per-draw write masks change.
  if(c.residentBound&&[c.mrtColors isEqualToArray:colors]&&(wantDepth?c.depth==wantDepth:c.depth==c.residentScratchDepth)&&(wantStencil?c.stencil==wantStencil:c.stencil==c.residentScratchStencil)){
   c.mrtMasks=[NSData dataWithBytes:masks length:sizeof masks];c.residentBinds++;
   for(unsigned i=0;i<r->count;i++)((LRGuestResident *)c.residents[@(r->color[i])]).sampledStale=YES;dt.sampledStale=YES;st.sampledStale=YES;   // this draw writes them
   return 0;}
  if(c.encoder){[c.encoder endEncoding];c.encoder=nil;}
  if(!c.residentBound){c.companionMrt=c.mrtColors;c.companionMasks=c.mrtMasks;c.defaultPainted=c.painted;}
  id<MTLTexture> depth=dt?dt.render:LRResidentScratch(c,MTLPixelFormatDepth32Float,w,h,c.residentScratchDepth);if(!dt){if(!depth)return -12;c.residentScratchDepth=depth;}
  id<MTLTexture> stencil=st?st.render:LRResidentScratch(c,MTLPixelFormatStencil8,w,h,c.residentScratchStencil);if(!st){if(!stencil)return -12;c.residentScratchStencil=stencil;}
  c.target=colors[0];c.mrtColors=colors;c.mrtMasks=[NSData dataWithBytes:masks length:sizeof masks];c.depth=depth;c.stencil=stencil;c.painted=YES;c.residentBound=YES;c.residentSrgb=srgb;c.residentBinds++;
  for(unsigned i=0;i<r->count;i++)((LRGuestResident *)c.residents[@(r->color[i])]).sampledStale=YES;dt.sampledStale=YES;st.sampledStale=YES;
  return 0;
 }
 case LR_GPU_RES_BLIT:{
  LRGuestResident *d=c.residents[@(r->color[0])];if(!t||!d)return -22;
  id<MTLTexture> source=t.render,target=d.render;
  // sRGB images: with GL_FRAMEBUFFER_SRGB both sides convert (decode reads, encode writes); otherwise raw bytes.
  if(r->flags&LR_GPU_RES_FLAG_SRGB){if(t.format==LR_GPU_RES_FORMAT_SRGB8_A8){id<MTLTexture> v=t.renderSrgb;source=LRResidentSrgbView(t.render,&v);t.renderSrgb=v;}
   if(d.format==LR_GPU_RES_FORMAT_SRGB8_A8){id<MTLTexture> v=d.renderSrgb;target=LRResidentSrgbView(d.render,&v);d.renderSrgb=v;}if(!source||!target)return -12;}
  return LRResidentBlit(c,t,source,d,target,r);
 }
 case LR_GPU_RES_SAMPLER:{
  // Translated GLSL samples through texture2d<float>, which Metal does not
  // allow for depth formats: depth is sampled from an R32Float copy, which
  // reads as GL's (d,0,0,1). Stencil is not sampled here.
  if(!t||t.format==LR_GPU_RES_FORMAT_STENCIL)return -22;
  BOOL depth=t.format==LR_GPU_RES_FORMAT_DEPTH;
  if(depth&&(r->min_filter==0x2601||r->mag_filter==0x2601)&&!c.device.supports32BitFloatFiltering)return -22;
  MTLPixelFormat sampledFormat=depth?MTLPixelFormatR32Float:t.render.pixelFormat;
  // Mipmapped minification (GL *_MIPMAP_* filters): the sampled copy carries a
  // full chain generated on the GPU from level 0 whenever level 0 changes.
  BOOL mip=!depth&&r->min_filter>=0x2700&&r->min_filter<=0x2703;
  NSUInteger levels=mip?(NSUInteger)floor(log2((double)MAX(t.width,t.height)))+1:1;
  if(!t.sampled||t.sampled.width!=t.width||t.sampled.height!=t.height||t.sampled.pixelFormat!=sampledFormat||t.sampled.mipmapLevelCount!=levels){
   MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:sampledFormat width:t.width height:t.height mipmapped:mip];
   d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite|(t.format==LR_GPU_RES_FORMAT_SRGB8_A8?MTLTextureUsagePixelFormatView:0);
   t.sampled=[c.device newTextureWithDescriptor:d];if(!t.sampled)return -12;t.sampledStale=YES;}
  if(t.sampledStale){int e=depth?LRResidentDepthFlip(c,t.render,t.sampled):LRResidentFlipCopy(c,t.render,t.sampled);if(e)return e;
   if(levels>1){id<MTLCommandBuffer> command=LRResidentCommand(c);id<MTLBlitCommandEncoder> b=command?[command blitCommandEncoder]:nil;if(!b)return -5;[b generateMipmapsForTexture:t.sampled];[b endEncoding];}
   t.sampledStale=NO;c.residentFlips++;}
  struct lr_gpu_texture p={.name=LR_GPU_RESIDENT_SAMPLER_BASE+r->id,.width=t.width,.height=t.height,.min_filter=r->min_filter,.mag_filter=r->mag_filter,.wrap_s=r->wrap_s,.wrap_t=r->wrap_t};
  if((p.min_filter!=0x2600&&p.min_filter!=0x2601&&!mip)||(p.mag_filter!=0x2600&&p.mag_filter!=0x2601))return -22;
  id<MTLTexture> view=t.sampled;
  if(t.format==LR_GPU_RES_FORMAT_SRGB8_A8&&!(r->flags&LR_GPU_RES_FLAG_RAW)){id<MTLTexture> cached=t.sampledSrgb;view=LRResidentSrgbView(t.sampled,&cached);t.sampledSrgb=cached;if(!view)return -12;}
  LRGuestMetalTexture *entry=c.textures[@(p.name)];if(!entry)entry=[LRGuestMetalTexture new];
  // Sampler states are immutable; rebuild only when the GL parameters change.
  if(!entry.sampler||entry.texture!=view||entry.minFilter!=p.min_filter||entry.magFilter!=p.mag_filter||entry.wrapS!=p.wrap_s||entry.wrapT!=p.wrap_t){entry.sampler=LRTextureSampler(c.device,&p);if(!entry.sampler)return -12;}
  entry.texture=view;entry.magFilter=p.mag_filter;
  entry.minFilter=p.min_filter;entry.wrapS=p.wrap_s;entry.wrapT=p.wrap_t;entry.mipmaps=mip;entry.painted=YES;c.textures[@(p.name)]=entry;
  return 0;
 }
 }
 return -22;
 }
}
