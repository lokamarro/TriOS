#import <Foundation/Foundation.h>
#import "app/LinuxRunner/FramebufferMetalPipeline.h"
// Real host GPU; production partial-upload helper and damage union. A slot can
// miss many updates while in flight and must catch up before it is sampled.
int main(int argc,char **argv){@autoreleasepool{
 unsigned readers[3]={1,0,0};
 if(LRFramebufferSlot(readers,YES,0)!=0||LRFramebufferSlot(readers,NO,0)!=1)return 1;
 readers[0]=2;if(LRFramebufferSlot(readers,YES,0)!=0)return 1;
 readers[0]=3;if(LRFramebufferSlot(readers,YES,0)!=NSNotFound)return 1;
 readers[0]=0;if(LRFramebufferSlot(readers,NO,0)!=0)return 1;
 id<MTLDevice> device=MTLCreateSystemDefaultDevice();if(!device)return 77;
 id<MTLTexture> slots[3]; LRFrameDamage damage[3]={{0}};
 const unsigned w=960,h=540; NSMutableData *pixels=[NSMutableData dataWithLength:w*h*4];
 NSMutableData *readback=[NSMutableData dataWithLength:w*h*4];
 NSUInteger uploaded=0,full=0; double partialTime=0,fullTime=0;unsigned checks=0;
 for(unsigned i=0;i<3;i++){slots[i]=LRFramebufferTexture(device,w,h);damage[i]=(LRFrameDamage){0,0,w,h};}
 for(unsigned frame=0;frame<240;frame++){
  LRFrameDamage d={(frame*31)%(w-32),(frame*19)%(h-24),32,24};
  uint8_t *p=pixels.mutableBytes;
  for(unsigned y=d.y;y<d.y+d.height;y++)for(unsigned x=d.x;x<d.x+d.width;x++){
   unsigned n=(y*w+x)*4;p[n]=frame;p[n+1]=frame*3;p[n+2]=x^y;p[n+3]=255;
  }
  for(unsigned i=0;i<3;i++)damage[i]=lr_damage_union(damage[i],d);
  if(frame%7==0)continue; // coalesced frames / unavailable drawable
  unsigned slot=frame%3;double start=NSProcessInfo.processInfo.systemUptime;
  uploaded+=LRUploadFramebuffer(slots[slot],p,damage[slot]);partialTime+=NSProcessInfo.processInfo.systemUptime-start;
  damage[slot]=(LRFrameDamage){0};
  [slots[slot] getBytes:readback.mutableBytes bytesPerRow:w*4 fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
  if(![readback isEqual:pixels]){fprintf(stderr,"stale slot %u frame %u\n",slot,frame);return 1;}
  // Comparable upload-only operation, no drawable or browser timing claim.
  start=NSProcessInfo.processInfo.systemUptime;
  full+=LRUploadFramebuffer(slots[slot],p,(LRFrameDamage){0,0,w,h});fullTime+=NSProcessInfo.processInfo.systemUptime-start;checks++;
 }
 if(LRUploadFramebuffer(slots[0],pixels.bytes,(LRFrameDamage){0})!=0)return 1;
 for(unsigned n=0;n<12;n++){
  unsigned rw=17+n*7,rh=11+n*3;id<MTLTexture> texture=LRFramebufferTexture(device,rw,rh);
  NSMutableData *src=[NSMutableData dataWithLength:rw*rh*4],*dst=[NSMutableData dataWithLength:rw*rh*4];memset(src.mutableBytes,n+1,src.length);
  LRUploadFramebuffer(texture,src.bytes,(LRFrameDamage){0,0,rw,rh});
  [texture getBytes:dst.mutableBytes bytesPerRow:rw*4 fromRegion:MTLRegionMake2D(0,0,rw,rh) mipmapLevel:0];if(![src isEqual:dst])return 1;checks++;
 }
 NSDictionary *r=@{@"environment":@"macOS host GPU",@"result":@"PASS",@"checks":@(checks),@"partial_upload_bytes":@(uploaded),@"full_upload_bytes":@(full),@"partial_upload_cpu_ms":@(partialTime*1000),@"full_upload_cpu_ms":@(fullTime*1000),@"scope":@"Deterministic moving-rectangle workload; texture upload only, no device FPS/speedup claim",@"coverage":@"stale ring slots, coalescing, source offset/stride, zero damage, first full upload, resized texture"};
 NSData *json=[NSJSONSerialization dataWithJSONObject:r options:NSJSONWritingPrettyPrinted error:nil];if(argc==2)[json writeToFile:@(argv[1]) atomically:YES];puts([[NSString alloc]initWithData:json encoding:NSUTF8StringEncoding].UTF8String);return 0;
}}
