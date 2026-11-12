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
static BOOL tabReady(NSString*text,int n){NSString*pattern=[NSString stringWithFormat:@"TAB READY\\s+%d(?![0-9])",n];return [text rangeOfString:pattern options:NSRegularExpressionSearch].location!=NSNotFound;}
static void navigate(LRRFBClient*c,NSString*url){chord(c,'l');dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),dispatch_get_main_queue(),^{type(c,url.UTF8String);key(c,0xff0d);});}
int main(int argc,char**argv){@autoreleasepool{
 if(argc!=5)return 2;NSString*out=@(argv[3]);NSString*kind=@(getenv("LR_BROWSER_KIND")?:"brave");
 LRRFBClient*c=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil]];
 NSLock*lock=[NSLock new];__block NSData*last;__block uint16_t width=0,height=0;__block BOOL ended=NO;
 c.frameHandler=^(NSData*p,uint16_t w,uint16_t h,uint64_t n,uint64_t b){(void)n;(void)b;[lock lock];last=p;width=w;height=h;[lock unlock];};
 c.statusHandler=^(NSString*s){fprintf(stderr,"%s\n",s.UTF8String);if([s hasPrefix:@"Display error:"]||[s hasPrefix:@"Linux display ended"])dispatch_async(dispatch_get_main_queue(),^{ended=YES;});};
 [c start];NSDate*start=NSDate.date,*scan=start,*changed=start;int phase=0,tab=1,verified=0;BOOL https=NO,search=NO,scroll=NO,closed=NO,finished=NO;NSMutableArray*events=[NSMutableArray new];NSString*text=@"";
 while(!ended&&-[start timeIntervalSinceNow]<360&&!finished){[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];[lock lock];NSData*p=last;uint16_t w=width,h=height;[lock unlock];if(!p||-[scan timeIntervalSinceNow]<.6)continue;scan=NSDate.date;CGPoint a,b,d;text=ocr(p,w,h,&a,&b,&d);[text writeToFile:[out stringByAppendingPathComponent:@"latest-ocr.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
 if(phase==0&&[text containsString:@"BENCH READY"]){[p writeToFile:[out stringByAppendingPathComponent:@"initial.bgra"] atomically:YES];navigate(c,@"https://example.com/");phase=1;changed=NSDate.date;}
 else if(phase==1&&[text containsString:@"Example Domain"]&&[text containsString:@"This domain is for use"]){https=YES;[p writeToFile:[out stringByAppendingPathComponent:@"https.bgra"] atomically:YES];navigate(c,@"example domain");phase=2;changed=NSDate.date;}
 else if(phase==2){search=([text containsString:@"IANA"]||[text containsString:@"iana.org"])&&([text containsString:@"Search"]||[text containsString:@"search"]);if(search||-[changed timeIntervalSinceNow]>45){[p writeToFile:[out stringByAppendingPathComponent:@"search.bgra"] atomically:YES];[events addObject:@{@"search_results_verified":@(search),@"seconds_including_OCR":@(-[changed timeIntervalSinceNow])}];navigate(c,@"file:///home/mobile/Downloads/browser_multitab.html?tab=1");phase=3;changed=NSDate.date;}}
 else if(phase==3&&tabReady(text,tab)&&[text containsString:@"JS VERIFIED"]){verified=tab;[events addObject:@{@"active_tab_content_verified":@(tab),@"seconds_including_OCR":@(-[start timeIntervalSinceNow])}];fprintf(stderr,"MULTITAB %s tab=%d CONTENT PASS\n",kind.UTF8String,tab);if(tab==1||tab==5||tab==10||tab==20)[p writeToFile:[out stringByAppendingPathComponent:[NSString stringWithFormat:@"tabs-%d.bgra",tab]] atomically:YES];if(tab<20){tab++;chord(c,'t');NSString*url=[NSString stringWithFormat:@"file:///home/mobile/Downloads/browser_multitab.html?tab=%d",tab];dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),dispatch_get_main_queue(),^{navigate(c,url);});}else{for(int i=0;i<35;i++){[c pointerX:650 y:430 buttons:16];[c pointerX:650 y:430 buttons:0];}phase=4;}changed=NSDate.date;}
 else if(phase==4&&[text containsString:@"SCROLL SUCCESS"]){scroll=YES;[p writeToFile:[out stringByAppendingPathComponent:@"scroll.bgra"] atomically:YES];for(int i=0;i<19;i++)dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(i*.25*NSEC_PER_SEC)),dispatch_get_main_queue(),^{chord(c,'w');});phase=5;changed=NSDate.date;}
 else if(phase==5&&-[changed timeIntervalSinceNow]>7&&tabReady(text,1)&&[text containsString:@"JS VERIFIED"]){closed=YES;[p writeToFile:[out stringByAppendingPathComponent:@"first-tab-restored.bgra"] atomically:YES];[c key:0xffe3 down:YES];[c key:0xffe1 down:YES];key(c,'w');[c key:0xffe1 down:NO];[c key:0xffe3 down:NO];phase=6;changed=NSDate.date;}
 else if(phase==6&&-[changed timeIntervalSinceNow]>7){finished=YES;}
 }
 [c stop];NSDictionary*r=@{@"environment":@"Mac ARM64 interpreter; not physical iPhone",@"application":kind,@"https_page_pixels_verified":@(https),@"search_results_verified":@(search),@"tabs_with_individually_verified_JS_content":@(verified),@"scroll_pixels_verified":@(scroll),@"rapid_close_restored_first_tab":@(closed),@"finished":@(finished),@"seconds_including_OCR":@(-[start timeIntervalSinceNow]),@"events":events,@"last_ocr":text,@"guest_GPU_acceleration":@"NOT measured"};[[NSJSONSerialization dataWithJSONObject:r options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[out stringByAppendingPathComponent:@"browser-multitab.json"] atomically:YES];return finished&&https&&verified==20&&scroll&&closed?0:1;
}}
