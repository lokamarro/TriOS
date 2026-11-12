#import <Foundation/Foundation.h>
#import "app/LinuxRunner/FramebufferMetalPipeline.h"
int main(){@autoreleasepool{
 id<MTLDevice>d=MTLCreateSystemDefaultDevice();if(!d)return 77;NSError*e=nil;id<MTLRenderPipelineState>p=LRFramebufferPipeline(d,&e);if(!p)return 1;id<MTLCommandQueue>q=[d newCommandQueue];
 NSMutableData*data=[NSMutableData dataWithLength:960*540*4];memset(data.mutableBytes,42,data.length);NSUInteger bytes[2]={0};double cpu[2]={0};
 for(unsigned mode=0;mode<2;mode++){
  id<MTLTexture> input[3]={nil},output[3];id<MTLCommandBuffer>commands[3];unsigned readers[3]={0};NSUInteger last=0;
  for(unsigned i=0;i<3;i++){
   BOOL reuse=mode&&i;NSUInteger slot=mode?LRFramebufferSlot(readers,reuse,last):i;if(slot==NSNotFound)return 1;
   if(!input[slot])input[slot]=LRFramebufferTexture(d,960,540);
   double start=NSProcessInfo.processInfo.systemUptime;
   if(!reuse)bytes[mode]+=LRUploadFramebuffer(input[slot],data.bytes,(LRFrameDamage){0,0,960,540});
   cpu[mode]+=NSProcessInfo.processInfo.systemUptime-start;
   MTLTextureDescriptor*desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:960 height:540 mipmapped:NO];desc.storageMode=MTLStorageModeShared;desc.usage=MTLTextureUsageRenderTarget;output[i]=[d newTextureWithDescriptor:desc];
   MTLRenderPassDescriptor*pass=[MTLRenderPassDescriptor renderPassDescriptor];pass.colorAttachments[0].texture=output[i];pass.colorAttachments[0].loadAction=MTLLoadActionClear;pass.colorAttachments[0].storeAction=MTLStoreActionStore;
   commands[i]=[q commandBuffer];id<MTLRenderCommandEncoder>en=[commands[i]renderCommandEncoderWithDescriptor:pass];LREncodeFramebuffer(en,p,input[slot]);[en endEncoding];readers[slot]++;last=slot;[commands[i]commit];
  }
  for(unsigned i=0;i<3;i++){[commands[i]waitUntilCompleted];if(commands[i].status!=MTLCommandBufferStatusCompleted)return 1;NSMutableData*r=[NSMutableData dataWithLength:data.length];[output[i]getBytes:r.mutableBytes bytesPerRow:960*4 fromRegion:MTLRegionMake2D(0,0,960,540) mipmapLevel:0];const uint8_t*b=r.bytes;for(NSUInteger j=0;j<r.length;j+=4)if(b[j]!=42||b[j+1]!=42||b[j+2]!=42||b[j+3]!=255)return 1;}
 }
 printf("METAL_REDRAW_PASS host GPU, 6 pixel-verified submissions; upload bytes baseline=%lu optimized=%lu; upload CPU ms baseline=%.3f optimized=%.3f; unchanged-frame redraw only, not device FPS\n",(unsigned long)bytes[0],(unsigned long)bytes[1],cpu[0]*1000,cpu[1]*1000);return 0;
}}
