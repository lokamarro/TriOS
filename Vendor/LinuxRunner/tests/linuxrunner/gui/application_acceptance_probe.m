#import <Foundation/Foundation.h>
#import <Vision/Vision.h>
#import "ProbeOCRDevice.h"
#import "app/LinuxRunner/RFBClient.h"
// Real original applications, fixed 960x540 fixture. A frame/window alone fails.
static NSString *ocr(NSData *pixels,uint16_t w,uint16_t h,CGPoint *button,CGPoint *field,CGPoint *block){
 CGColorSpaceRef s=CGColorSpaceCreateDeviceRGB();CGDataProviderRef p=CGDataProviderCreateWithCFData((__bridge CFDataRef)pixels);
 CGImageRef im=CGImageCreate(w,h,8,32,w*4,s,kCGBitmapByteOrder32Little|kCGImageAlphaNoneSkipFirst,p,NULL,NO,kCGRenderingIntentDefault);
 VNRecognizeTextRequest*r=[VNRecognizeTextRequest new];r.recognitionLevel=VNRequestTextRecognitionLevelAccurate;r.usesLanguageCorrection=NO;
 if(!LRConfigureProbeOCR(r)){CGImageRelease(im);CGDataProviderRelease(p);CGColorSpaceRelease(s);return @"";}
 NSError *error=nil;NSDate *started=NSDate.date;
 BOOL ok=[[[VNImageRequestHandler alloc]initWithCGImage:im options:@{}]performRequests:@[r] error:&error];
 if(!ok||error)fprintf(stderr,"APP_OCR_ERROR domain=%s code=%ld description=%s\n",error.domain.UTF8String?:"none",(long)error.code,error.localizedDescription.UTF8String?:"none");
 if(-[started timeIntervalSinceNow]>2)fprintf(stderr,"APP_OCR_SLOW seconds=%.3f observations=%lu\n",-[started timeIntervalSinceNow],(unsigned long)r.results.count);
 NSMutableArray*a=[NSMutableArray new];
 for(VNRecognizedTextObservation*v in r.results){NSString*t=[v topCandidates:1].firstObject.string?:@"";[a addObject:t];CGRect b=v.boundingBox;CGPoint center=CGPointMake(CGRectGetMidX(b)*w,(1-CGRectGetMidY(b))*h);
  if([t.lowercaseString containsString:@"est input"])*button=center;
  if([t.lowercaseString containsString:@"type here"]||[t.lowercaseString containsString:@"lype here"])*field=center;
  if([t isEqualToString:@"Block"])*block=center;
 }
 CGImageRelease(im);CGDataProviderRelease(p);CGColorSpaceRelease(s);return [a componentsJoinedByString:@" "];
}
static void key(LRRFBClient*c,uint32_t k){[c key:k down:YES];[c key:k down:NO];}
static void chord(LRRFBClient*c,uint32_t k){[c key:0xffe3 down:YES];key(c,k);[c key:0xffe3 down:NO];}
static void type(LRRFBClient*c,const char*s){for(;*s;s++)key(c,(unsigned char)*s);}
static void click(LRRFBClient*c,int x,int y){[c pointerX:x y:y buttons:0];[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.15]];[c pointerX:x y:y buttons:1];[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.15]];[c pointerX:x y:y buttons:0];}
int main(int argc,char**argv){@autoreleasepool{
 if(argc!=5)return 2;BOOL code=getenv("LR_PROBE_CODE")!=NULL;NSString*out=@(argv[3]);
 LRRFBClient*c=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil]];
 NSLock*lock=[NSLock new];__block NSData*last;__block uint16_t width=0,height=0;__block uint64_t frames=0,bytes=0;__block BOOL ended=NO;
 c.frameHandler=^(NSData*p,uint16_t w,uint16_t h,uint64_t n,uint64_t b){[lock lock];last=p;width=w;height=h;frames=n;bytes=b;[lock unlock];};
 c.statusHandler=^(NSString*s){fprintf(stderr,"%s\n",s.UTF8String);if([s hasPrefix:@"Linux display ended"]||[s hasPrefix:@"Display error:"])dispatch_async(dispatch_get_main_queue(),^{ended=YES;});};
 [c start];NSDate*start=NSDate.date,*scan=start;double ready=0,input=0,scroll=0;BOOL scrolled=NO,popupDismissed=NO,keyboardSent=NO,mouseConfirmed=NO,saved=NO,keyboardSnapshot=NO;NSDate *keyboardAt=nil;NSString*text=@"";
 while(!ended&&-[start timeIntervalSinceNow]<180){[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];
  [lock lock];NSData*p=last;uint16_t w=width,h=height;[lock unlock];if(!p||-[scan timeIntervalSinceNow]<1)continue;scan=NSDate.date;CGPoint button=CGPointMake(-1,-1),field=button,block=button;text=ocr(p,w,h,&button,&field,&block);
  [text writeToFile:[out stringByAppendingPathComponent:@"latest-ocr.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
  if(keyboardAt&&!keyboardSnapshot&&-[keyboardAt timeIntervalSinceNow]>2){[p writeToFile:[out stringByAppendingPathComponent:@"after-keyboard.bgra"] atomically:YES];keyboardSnapshot=YES;}
  if(!code&&[text containsString:@"quit unexpectedly"]){if(!popupDismissed){click(c,block.x>=0?block.x:550,block.y>=0?block.y:290);popupDismissed=YES;}continue;}
  if(!ready&&[text containsString:code?@"original ARM64 editor":@"BENCH READY"]&&(code||(button.x>=0&&field.x>=0))){
   ready=-[start timeIntervalSinceNow];[p writeToFile:[out stringByAppendingPathComponent:@"ready.bgra"] atomically:YES];fprintf(stderr,"APP_RENDER_READY %.3f %s\n",ready,text.UTF8String);
   if(code){
    // Fixed fixture: click the visible Manage gear, then require its menu pixels.
    click(c,25,485);
   }
   else {fprintf(stderr,"APP_INPUT_TARGET button=%.1f,%.1f field=%.1f,%.1f\n",button.x,button.y,field.x,field.y);click(c,button.x,button.y);}
  }else if(code&&ready&&!mouseConfirmed&&[text containsString:@"Keyboard Shortcuts"]){
   mouseConfirmed=YES;[p writeToFile:[out stringByAppendingPathComponent:@"mouse.bgra"] atomically:YES];
   key(c,0xff1b);click(c,300,100);chord(c,0xff57);key(c,0xff0d);type(c,"LINUXRUNNER INPUT VERIFIED 875");chord(c,'s');keyboardSent=YES;
  }else if(!code&&ready&&!keyboardSent&&[text containsString:@"INPUT VERIFIED"]&&field.x>=0){
   // The button grows after clicking; find the reflowed input before typing.
   fprintf(stderr,"APP_KEYBOARD_TARGET %.1f,%.1f\n",field.x,field.y);click(c,field.x,field.y);type(c,"975310875");[c pointerX:880 y:500 buttons:0];keyboardSent=YES;keyboardAt=NSDate.date;
  }else if(ready&&!input&&[text containsString:code?@"INPUT VERIFIED 875":@"INPUT VERIFIED"]&&(code||[text containsString:@"975310875"])){
   input=-[start timeIntervalSinceNow]-ready;[p writeToFile:[out stringByAppendingPathComponent:@"input.bgra"] atomically:YES];
   if(code&&mouseConfirmed){chord(c,'s');continue;}
   for(int i=0;i<18;i++){[c pointerX:650 y:430 buttons:16];[c pointerX:650 y:430 buttons:0];}scrolled=YES;
  }else if(code&&input&&mouseConfirmed){
   NSString*path=[@(getenv("LR_GUI_ROOTFS")?:"") stringByAppendingPathComponent:@"data/home/lrabi_gui/lr-code-875-test.txt"];
   NSString*contents=[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
   if([contents containsString:@"LINUXRUNNER INPUT VERIFIED 875"]){saved=YES;fprintf(stderr,"APP_FILE_SAVE_CONFIRMED\n");break;}
  }else if(scrolled&&[text containsString:@"SCROLL BOTTOM"]){scroll=-[start timeIntervalSinceNow]-ready-input;[p writeToFile:[out stringByAppendingPathComponent:@"scroll.bgra"] atomically:YES];break;}
 }
 [lock lock];NSData*p=last;uint64_t n=frames,b=bytes;[lock unlock];[p writeToFile:[out stringByAppendingPathComponent:@"final.bgra"] atomically:YES];
 if(ready&&input){chord(c,'q');NSDate*quit=NSDate.date;while(!ended&&-[quit timeIntervalSinceNow]<3)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];}[c stop];NSDictionary*r=@{@"environment":@"Mac ARM64 interpreter; not physical iPhone",@"application":code?@"original VS Code":@"original Brave",@"actual_content_visible":@(ready>0),@"keyboard_mouse_confirmed_in_pixels":@(input>0&&(!code||mouseConfirmed)),@"mouse_confirmed_in_pixels":@(code?mouseConfirmed:input>0),@"saved_file_independently_confirmed":@(saved),@"scroll_confirmed_in_pixels":@(scroll>0),@"ready_seconds_including_transport_ocr":@(ready),@"input_seconds_including_ocr":@(input),@"scroll_seconds_including_ocr":@(scroll),@"frames":@(n),@"wire_bytes":@(b),@"last_ocr":text};
 [[NSJSONSerialization dataWithJSONObject:r options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[out stringByAppendingPathComponent:@"application-acceptance.json"] atomically:YES];return ready&&input&&(code?(mouseConfirmed&&saved):scroll>0)?0:1;
}}
