#import "app/LinuxRunner/FramebufferPatch.h"
#include <assert.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(void){@autoreleasepool{
 unsigned w=960,h=540;NSMutableData *source=[NSMutableData dataWithLength:w*h*4],*receiver=[NSMutableData dataWithLength:w*h*4];
 uint8_t *p=source.mutableBytes;for(unsigned i=0;i<source.length;i++)p[i]=(uint8_t)(i*17+i/7);
 LRFrameDamage full={0,0,w,h};assert(LRApplyFramebufferPatch(receiver,LRFramebufferPatch(source,w,h,full),w,h,full));assert([source isEqual:receiver]);
 uint64_t random=1721;for(unsigned n=0;n<500;n++){
  random=random*6364136223846793005ULL+1;unsigned x=random%w,y=(random>>16)%h;
  LRFrameDamage d={x,y,MIN(23,w-x),MIN(19,h-y)};uint8_t color[4]={n,n>>1,255-n,0};assert(lr_rfb_fill(source.mutableBytes,w,h,d.x,d.y,d.width,d.height,color));
  NSData *patch=LRFramebufferPatch(source,w,h,d);assert(patch.length==d.width*d.height*4);
  assert(LRApplyFramebufferPatch(receiver,patch,w,h,d));assert([source isEqual:receiver]);
 }
 assert(!LRFramebufferPatch(source,w,h,(LRFrameDamage){959,539,2,2}));
 assert(!LRApplyFramebufferPatch(receiver,[NSMutableData dataWithLength:3],w,h,(LRFrameDamage){0,0,1,1}));
 // Resize/reconnect: the first full delivery reproduces all pixels, and a
 // coalesced bounding region preserves changes in two disjoint rectangles.
 LRFrameDamage a={2,3,3,4},b={200,300,2,2},combined=lr_damage_union(a,b);
 uint8_t c[4]={7,11,19,0};lr_rfb_fill(source.mutableBytes,w,h,a.x,a.y,a.width,a.height,c);lr_rfb_fill(source.mutableBytes,w,h,b.x,b.y,b.width,b.height,c);
 assert(LRApplyFramebufferPatch(receiver,LRFramebufferPatch(source,w,h,combined),w,h,combined));assert([source isEqual:receiver]);
 LRFrameDamage small={20,30,32,24};NSUInteger copied=0;double start=now();
 for(unsigned i=0;i<2000;i++){@autoreleasepool{NSData *d=[source copy];copied+=d.length;}}
 double baseline=now()-start;NSUInteger patchBytes=0;start=now();
 for(unsigned i=0;i<2000;i++){@autoreleasepool{NSData *d=LRFramebufferPatch(source,w,h,small);patchBytes+=d.length;}}
 printf("FRAMEBUFFER_PATCH_PASS random=500 baseline_bytes=%lu patch_bytes=%lu baseline_cpu_seconds=%.6f patch_cpu_seconds=%.6f\n",(unsigned long)copied,(unsigned long)patchBytes,baseline,now()-start);return 0;
}}
