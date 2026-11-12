#import <Foundation/Foundation.h>
#import <Vision/Vision.h>
#import "ProbeOCRDevice.h"
#import "app/LinuxRunner/RFBClient.h"
#import "app/LinuxRunner/FramebufferPatch.h"
#include <unistd.h>
#include <time.h>
static double clock_seconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static NSString *ocr(NSData *pixels,uint16_t w,uint16_t h){
 CGColorSpaceRef s=CGColorSpaceCreateDeviceRGB();CGDataProviderRef p=CGDataProviderCreateWithCFData((__bridge CFDataRef)pixels);
 CGImageRef image=CGImageCreate(w,h,8,32,w*4,s,kCGBitmapByteOrder32Little|kCGImageAlphaNoneSkipFirst,p,NULL,NO,kCGRenderingIntentDefault);
 VNRecognizeTextRequest *r=[VNRecognizeTextRequest new];r.recognitionLevel=VNRequestTextRecognitionLevelAccurate;r.usesLanguageCorrection=NO;
 if(!LRConfigureProbeOCR(r)){CGImageRelease(image);CGDataProviderRelease(p);CGColorSpaceRelease(s);return @"";}
 NSError *error=nil;NSDate *started=NSDate.date;
 BOOL ok=[[[VNImageRequestHandler alloc]initWithCGImage:image options:@{}]performRequests:@[r] error:&error];
 if(!ok||error)fprintf(stderr,"FIREFOX_OCR_ERROR %s\n",error.localizedDescription.UTF8String?:"unknown");
 if(-[started timeIntervalSinceNow]>2)fprintf(stderr,"FIREFOX_OCR_SLOW seconds=%.3f\n",-[started timeIntervalSinceNow]);
 NSMutableArray *a=[NSMutableArray new];
 for(VNRecognizedTextObservation *v in r.results)[a addObject:[v topCandidates:1].firstObject.string?:@""];
 CGImageRelease(image);CGDataProviderRelease(p);CGColorSpaceRelease(s);return [a componentsJoinedByString:@" "];
}
int main(int argc,char **argv){@autoreleasepool{
 if(argc!=5)return 2;NSString *output=@(argv[3]);
 LRRFBClient *client=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil]];
 NSLock *frameLock=[NSLock new];
 __block double lastAt=0;__block NSData *last=nil;__block uint16_t w=0,h=0;__block uint64_t frames=0,wire=0,patchBytes=0;__block BOOL ended=NO;
 client.statusHandler=^(NSString *s){fprintf(stderr,"%s\n",s.UTF8String);if([s hasPrefix:@"Display error:"]||[s hasPrefix:@"Linux display ended"])dispatch_async(dispatch_get_main_queue(),^{ended=YES;});};
 __block NSMutableData *assembled=nil;
 if(getenv("LR_RFB_PATCH_TEST"))client.patchFrameHandler=^(NSData *patch,uint16_t width,uint16_t height,uint64_t count,uint64_t bytes,LRFrameDamage damage){
  [frameLock lock];if(w!=width||h!=height)assembled=[NSMutableData dataWithLength:(NSUInteger)width*height*4];
  assert(LRApplyFramebufferPatch(assembled,patch,width,height,damage));last=[assembled copy];w=width;h=height;frames=count;wire=bytes;patchBytes+=patch.length;lastAt=clock_seconds();[frameLock unlock];
 };
 else client.frameHandler=^(NSData *p,uint16_t width,uint16_t height,uint64_t count,uint64_t bytes){[frameLock lock];last=p;w=width;h=height;frames=count;wire=bytes;patchBytes+=p.length;lastAt=clock_seconds();[frameLock unlock];};
 double start=clock_seconds(),scan=start,scrollSent=0,readyRecognitionDelay=0,scrollRecognitionDelay=0;[client start];double ready=0,scrollReady=0;NSString *text=@"";BOOL scrolled=NO;
 while(!ended&&clock_seconds()-start<180){[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];
  // Retain one coherent latest frame; OCR must not race the decoder's ARC
  // assignment or pair old pixels with a new DesktopSize. Do not queue stale
  // full-frame snapshots behind expensive OCR on the main thread.
  [frameLock lock];NSData *pixels=last;uint16_t width=w,height=h;double frameAt=lastAt;[frameLock unlock];
  if(!pixels||clock_seconds()-scan<.5)continue;scan=clock_seconds();text=ocr(pixels,width,height);
  if(!ready&&[text containsString:@"BENCH READY"]){ready=frameAt-start;readyRecognitionDelay=clock_seconds()-frameAt;[pixels writeToFile:[output stringByAppendingPathComponent:@"local.bgra"] atomically:YES];fprintf(stderr,"FIREFOX_LOCAL_READY seconds=%.3f %s\n",ready,text.UTF8String);
   scrollSent=clock_seconds();for(int i=0;i<18;i++){[client pointerX:450 y:380 buttons:16];[client pointerX:450 y:380 buttons:0];}scrolled=YES;
  } else if(scrolled&&frameAt>=scrollSent&&[text containsString:@"SCROLL BOTTOM"]){scrollReady=frameAt-scrollSent;scrollRecognitionDelay=clock_seconds()-frameAt;[pixels writeToFile:[output stringByAppendingPathComponent:@"scroll.bgra"] atomically:YES];break;}
 }
 [client stop];[frameLock lock];uint64_t finalFrames=frames,finalWire=wire,finalPatchBytes=patchBytes;[frameLock unlock];NSDictionary *report=@{@"environment":@"macOS host interpreter; not iPhone",@"local_js_rendered":@(ready>0),@"startup_to_local_js_frame_seconds":@(ready),@"scroll_to_visible_frame_seconds":@(scrollReady),@"ready_ocr_delay_seconds":@(readyRecognitionDelay),@"scroll_ocr_delay_seconds":@(scrollRecognitionDelay),@"timing_method":@"monotonic receive time of the recognized coherent RFB frame; not OCR completion",@"last_ocr":text,@"frames":@(finalFrames),@"wire_bytes":@(finalWire),@"decoder_handoff_bytes":@(finalPatchBytes),@"scope":@"X11+RFB startup included; latest recognized frame bounds readiness, OCR processing delay separate; not pure Firefox CPU or iPhone presentation time"};
 [[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[output stringByAppendingPathComponent:@"firefox-performance.json"] atomically:YES];return ready>0&&scrollReady>0?0:1;
}}
