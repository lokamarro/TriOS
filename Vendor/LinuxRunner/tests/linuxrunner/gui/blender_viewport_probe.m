// Real guest Blender framebuffer/input evidence, not a native graphics demo.
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>
#import <ImageIO/ImageIO.h>
#import "app/LinuxRunner/RFBClient.h"
#import "BlenderFrameObservation.h"
static double monotonic(void){return NSProcessInfo.processInfo.systemUptime;}
static void refresh(LRBlenderFrameObservation *state,NSData *__strong *data,unsigned *w,unsigned *h,uint64_t *frames,BOOL *failed){NSDictionary *s=[state snapshot];*data=s[@"pixels"]==NSNull.null?nil:s[@"pixels"];*w=[s[@"width"] unsignedIntValue];*h=[s[@"height"] unsignedIntValue];*frames=[s[@"frames"] unsignedLongLongValue];*failed=[s[@"failed"] boolValue];}
static void png(NSData *data,unsigned w,unsigned h,NSString *path){if(!data)return;CGColorSpaceRef s=CGColorSpaceCreateDeviceRGB();CGDataProviderRef p=CGDataProviderCreateWithCFData((__bridge CFDataRef)data);CGImageRef i=CGImageCreate(w,h,8,32,w*4,s,kCGBitmapByteOrder32Little|kCGImageAlphaNoneSkipFirst,p,NULL,NO,kCGRenderingIntentDefault);CGImageDestinationRef d=CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path],CFSTR("public.png"),1,NULL);if(d){CGImageDestinationAddImage(d,i,NULL);CGImageDestinationFinalize(d);CFRelease(d);}CGImageRelease(i);CGDataProviderRelease(p);CGColorSpaceRelease(s);}
static NSString *ocr(NSData *data,unsigned w,unsigned h){if(!data)return @"";CGColorSpaceRef s=CGColorSpaceCreateDeviceRGB();CGDataProviderRef p=CGDataProviderCreateWithCFData((__bridge CFDataRef)data);CGImageRef i=CGImageCreate(w,h,8,32,w*4,s,kCGBitmapByteOrder32Little|kCGImageAlphaNoneSkipFirst,p,NULL,NO,kCGRenderingIntentDefault);VNRecognizeTextRequest *r=[VNRecognizeTextRequest new];r.recognitionLevel=VNRequestTextRecognitionLevelAccurate;r.usesLanguageCorrection=NO;[[[VNImageRequestHandler alloc]initWithCGImage:i options:@{}]performRequests:@[r] error:nil];NSMutableArray *a=[NSMutableArray new];for(VNRecognizedTextObservation *v in r.results)[a addObject:[v topCandidates:1].firstObject.string?:@""];CGImageRelease(i);CGDataProviderRelease(p);CGColorSpaceRelease(s);return [a componentsJoinedByString:@" "];}
static void pump(double seconds){double begin=monotonic();while(monotonic()-begin<seconds)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.05]];}
static void key(LRRFBClient *c,uint32_t k){[c key:k down:YES];[c key:k down:NO];pump(.2);}
int main(int argc,char **argv){@autoreleasepool{
 if(argc!=5)return 2;NSString *out=@(argv[3]);double timeout=getenv("LR_BLENDER_SECONDS")?atof(getenv("LR_BLENDER_SECONDS")):240;if(timeout<10||timeout>600)return 2;
 LRRFBClient *c=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil]];
 NSData *last=nil;unsigned w=0,h=0;uint64_t frames=0;BOOL failed=NO;LRBlenderFrameObservation *observation=[LRBlenderFrameObservation new];double began=monotonic();c.frameHandler=^(NSData *d,uint16_t x,uint16_t y,uint64_t f,uint64_t b){[observation observe:d width:x height:y frames:f bytes:b at:monotonic()];};c.transportMetricsHandler=^(NSDictionary *event){[observation observeTransport:event];};c.statusHandler=^(NSString *s){fprintf(stderr,"%s\n",s.UTF8String);if([s containsString:@"Display error:"]||[s containsString:@"Linux display ended"])[observation markFailed];};[c start];double start=monotonic();double scan=0;BOOL ui=NO;NSString *text=@"";
 while(!failed&&(monotonic()-start)<timeout){pump(.1);refresh(observation,&last,&w,&h,&frames,&failed);if(failed)break;double elapsed=(monotonic()-start);if(!last||elapsed-scan<5)continue;scan=elapsed;text=ocr(last,w,h);fprintf(stderr,"BLENDER_FRAME seconds=%.1f frames=%llu text=%.512s\n",elapsed,(unsigned long long)frames,text.UTF8String);
  png(last,w,h,[out stringByAppendingPathComponent:@"mac-host-blender-latest.png"]);
  if([text containsString:@"Modeling"]&&([text containsString:@"Object"]||[text containsString:@"Layout"])){ui=YES;break;}
 }
 BOOL saved=NO;double first=ui?(monotonic()-start):0;
 if(ui){png(last,w,h,[out stringByAppendingPathComponent:@"mac-host-blender-first-ui.png"]);
  // Escape dismisses splash, then normal RFB input focuses the 3D viewport.
  key(c,0xff1b);[c pointerX:w*0.42 y:h*0.47 buttons:1];[c pointerX:w*0.42 y:h*0.47 buttons:0];pump(3);
  key(c,0xffb2);[c pointerX:w/3 y:h/3 buttons:8];[c pointerX:w/3 y:h/3 buttons:0];pump(2);
  key(c,'g');pump(2);key(c,'x');key(c,'1');key(c,0xff0d);pump(4);
  key(c,'s');pump(2);key(c,'2');key(c,0xff0d);pump(4);
  key(c,0xff09);pump(3);key(c,'3');pump(2);key(c,'a');pump(2);key(c,'e');pump(2);key(c,'z');key(c,'1');key(c,0xff0d);pump(5);key(c,0xff09);pump(15);
  double saveStart=monotonic();NSString *applicationLog=@(getenv("LR_GUI_APPLICATION_LOG")?:"");
  while((monotonic()-saveStart)<60){NSString *log=[NSString stringWithContentsOfFile:applicationLog encoding:NSUTF8StringEncoding error:nil];if([log containsString:@"BLENDER_UI_MODEL_SAVED"]){saved=YES;break;}pump(.2);}
  refresh(observation,&last,&w,&h,&frames,&failed);
  png(last,w,h,[out stringByAppendingPathComponent:@"mac-host-blender-after-input.png"]);
 }
 refresh(observation,&last,&w,&h,&frames,&failed);
 png(last,w,h,[out stringByAppendingPathComponent:@"mac-host-blender-final.png"]);
 NSDictionary *r=@{@"environment":@"macOS host ARM64 guest/X11, not iPhone",@"first_ui_seconds":@(first),@"ui_text_observed":@(ui),@"input_sequence_sent":@(ui),@"model_saved_marker":@(saved),@"modeling_result":saved?@"guest scene modified through RFB input and save completed":@"requires guest scene assertion; input delivery alone is not success",@"frames":@(frames),@"rfb_observation":[observation metricsSince:began],@"last_text":text,@"transport_error":@(failed)};[[NSJSONSerialization dataWithJSONObject:r options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[out stringByAppendingPathComponent:@"blender-display.json"] atomically:YES];[c stop];return ui&&saved&&!failed?0:1;
}}
