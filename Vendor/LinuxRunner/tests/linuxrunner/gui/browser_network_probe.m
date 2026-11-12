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
  if([t.lowercaseString containsString:@"est input"]||[t isEqualToString:@"Quit Firefox"])*button=center;
  if([t.lowercaseString containsString:@"type here"]||[t.lowercaseString containsString:@"lype here"])*field=center;
  if([t isEqualToString:@"Block"])*block=center;
  else if([t hasPrefix:@"Block "]&&[t containsString:@"Allow"])*block=CGPointMake((b.origin.x+b.size.width*.25)*w,center.y);
 }
 CGImageRelease(im);CGDataProviderRelease(p);CGColorSpaceRelease(s);return [a componentsJoinedByString:@" "];
}
static void key(LRRFBClient*c,uint32_t k){[c key:k down:YES];[c key:k down:NO];}
static void chord(LRRFBClient*c,uint32_t k){[c key:0xffe3 down:YES];key(c,k);[c key:0xffe3 down:NO];}
// Firefox on Linux selects numbered tabs with Alt, unlike Chromium's Ctrl.
static void firstTab(LRRFBClient*c,NSString*kind){uint32_t modifier=[kind isEqualToString:@"firefox"]?0xffe9:0xffe3;[c key:modifier down:YES];key(c,'1');[c key:modifier down:NO];}
static void type(LRRFBClient*c,const char*s){for(;*s;s++)key(c,(unsigned char)*s);}
static void click(LRRFBClient*c,int x,int y){[c pointerX:x y:y buttons:0];[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.15]];[c pointerX:x y:y buttons:1];[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.15]];[c pointerX:x y:y buttons:0];}
static BOOL tabReady(NSString*text,int n){NSString*pattern=[NSString stringWithFormat:@"TAB READY\\s+%d(?![0-9])",n];return [text rangeOfString:pattern options:NSRegularExpressionSearch].location!=NSNotFound;}
// Acceptance input is deliberately paced; these waits are not latency measurements.
static BOOL searchReady(NSString*text){
 NSString*t=text.lowercaseString;
 // Snippets and UI labels can be translated independently. Require the
 // engine name, result title and HTTPS result URL; the direct example.com
 // page lacks the engine name and must not pass as a stale search frame.
 // Ranking changes (Wikipedia above example.com) may push the result URL out
 // of view; the engine's search URL plus an example.com result also counts.
 // Chromium's default engine is DuckDuckGo; Firefox and Brave use Google here.
 BOOL engine=[t containsString:@"google"]||[t containsString:@"duckduckgo"];
 BOOL resultsURL=[t containsString:@"google.com/search"]||[t containsString:@"duckduckgo.com/?q="];
 return engine&&[t containsString:@"example domain"]&&([t containsString:@"https://example.com"]||[t containsString:@"https://www.example.com"]||(resultsURL&&[t containsString:@"example.com"]));
}
static BOOL browserExitedCleanly(void){
 NSString*path=@(getenv("LR_GUI_APPLICATION_LOG")?:"");
 NSString*log=[NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
 return log&&[log rangeOfString:@"(?m)^BROWSER_EXIT status=0$" options:NSRegularExpressionSearch].location!=NSNotFound;
}
static void navigate(LRRFBClient*c,NSString*url){
 click(c,450,70);chord(c,'a');
 [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1]];
 type(c,url.UTF8String);
 [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1]];
 key(c,0xff0d);fprintf(stderr,"BROWSER_NAVIGATION_SENT\n");
}
int main(int argc,char**argv){@autoreleasepool{
 if(argc!=5)return 2;NSString*out=@(argv[3]);NSString*kind=@(getenv("LR_BROWSER_KIND")?:"brave");
 LRRFBClient*c=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil]];
 NSLock*lock=[NSLock new];__block NSData*last;__block uint16_t width=0,height=0;__block BOOL ended=NO;
 c.frameHandler=^(NSData*p,uint16_t w,uint16_t h,uint64_t n,uint64_t b){(void)n;(void)b;[lock lock];last=p;width=w;height=h;[lock unlock];};
 c.statusHandler=^(NSString*s){fprintf(stderr,"%s\n",s.UTF8String);if([s hasPrefix:@"Display error:"]||[s hasPrefix:@"Linux display ended"])dispatch_async(dispatch_get_main_queue(),^{ended=YES;});};
 [c start];NSDate*start=NSDate.date,*scan=start,*changed=start;int phase=0,tab=1,verified=0;unsigned permissionDismissals=0,quitConfirmations=0;BOOL wps=NO,https=NO,search=NO,scroll=NO,scrollRetried=NO,closed=NO,finished=NO;NSMutableArray*events=[NSMutableArray new];NSString*text=@"";
 while(!ended&&-[start timeIntervalSinceNow]<360&&!finished){[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];[lock lock];NSData*p=last;uint16_t w=width,h=height;[lock unlock];if(!p||-[scan timeIntervalSinceNow]<.6)continue;scan=NSDate.date;CGPoint a=CGPointZero,b=CGPointZero,d=CGPointZero;text=ocr(p,w,h,&a,&b,&d);[text writeToFile:[out stringByAppendingPathComponent:@"latest-ocr.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
 if(phase==6&&([text containsString:@"Quit Firefox or close current tab?"]||[text containsString:@"Close window and quit Firefox?"])&&!CGPointEqualToPoint(a,CGPointZero)&&quitConfirmations<1){
  [p writeToFile:[out stringByAppendingPathComponent:@"quit-confirmation.bgra"] atomically:YES];
  click(c,a.x,a.y);quitConfirmations++;fprintf(stderr,"QUIT_FIREFOX_CONFIRM_CLICK\n");continue;
 }
 if([text containsString:@"Show notifications"]&&!CGPointEqualToPoint(d,CGPointZero)&&permissionDismissals<2){
  [text writeToFile:[out stringByAppendingPathComponent:@"notification-prompt-ocr.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
  [p writeToFile:[out stringByAppendingPathComponent:@"notification-prompt.bgra"] atomically:YES];
  click(c,d.x,d.y);permissionDismissals++;fprintf(stderr,"NOTIFICATION_PROMPT_BLOCK_CLICK count=%u\n",permissionDismissals);
  [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1]];
  if(phase==1)navigate(c,@"https://example.com/");
  else if(phase==2)navigate(c,@"example domain");
  else if(phase==3)navigate(c,[NSString stringWithFormat:@"file:///home/mobile/Downloads/browser_multitab.html?tab=%d",tab]);
  changed=NSDate.date;continue;
 }
 if(phase==0&&[text containsString:@"BENCH READY"]){[p writeToFile:[out stringByAppendingPathComponent:@"initial.bgra"] atomically:YES];navigate(c,@"https://www.wps.com/");phase=7;changed=NSDate.date;}
 else if(phase==7&&[text containsString:@"WPS"]&&([text containsString:@"Office"]||[text containsString:@"OFFICE"])&&([text containsString:@"Download"]||[text containsString:@"DOWNLOAD"])) {wps=YES;[p writeToFile:[out stringByAppendingPathComponent:@"wps.bgra"] atomically:YES];fprintf(stderr,"WPS_CONTENT_VISIBLE\n");navigate(c,@"https://example.com/");phase=1;changed=NSDate.date;}
 else if((phase==7||phase==1)&&-[changed timeIntervalSinceNow]>90){[events addObject:@{@"stage_timeout":@(phase),@"last_visible_text":text}];navigate(c,@"file:///home/mobile/Downloads/browser_multitab.html?tab=1");phase=3;changed=NSDate.date;}
 else if(phase==1&&[text containsString:@"Example Domain"]&&[text containsString:@"This domain is for use"]){https=YES;[p writeToFile:[out stringByAppendingPathComponent:@"https.bgra"] atomically:YES];navigate(c,@"example domain");phase=2;changed=NSDate.date;}
 else if(phase==2){search=searchReady(text);if(search||-[changed timeIntervalSinceNow]>45){[text writeToFile:[out stringByAppendingPathComponent:@"search-ocr.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];[p writeToFile:[out stringByAppendingPathComponent:@"search.bgra"] atomically:YES];[events addObject:@{@"search_results_verified":@(search),@"seconds_including_OCR":@(-[changed timeIntervalSinceNow])}];navigate(c,@"file:///home/mobile/Downloads/browser_multitab.html?tab=1");phase=3;changed=NSDate.date;}}
 else if(phase==3&&tabReady(text,tab)&&[text containsString:@"JS VERIFIED"]){verified=tab;[events addObject:@{@"active_tab_content_verified":@(tab),@"seconds_including_OCR":@(-[start timeIntervalSinceNow])}];fprintf(stderr,"MULTITAB %s tab=%d CONTENT PASS\n",kind.UTF8String,tab);if(tab==1||tab==5||tab==10||tab==20)[p writeToFile:[out stringByAppendingPathComponent:[NSString stringWithFormat:@"tabs-%d.bgra",tab]] atomically:YES];if(tab<5){tab++;chord(c,'t');NSString*url=[NSString stringWithFormat:@"file:///home/mobile/Downloads/browser_multitab.html?tab=%d",tab];dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),dispatch_get_main_queue(),^{navigate(c,url);});}else{for(int i=0;i<35;i++){[c pointerX:650 y:430 buttons:16];[c pointerX:650 y:430 buttons:0];}phase=4;}changed=NSDate.date;}
 // Scroll stall diagnosis: keep the stalled frame, then send one more burst.
 // Recovery means wheel input was lost in the browser; no recovery means it
 // stopped presenting frames. A retried scroll never passes the gate.
 else if(phase==4&&!scrollRetried&&-[changed timeIntervalSinceNow]>25&&![text containsString:@"SCROLL SUCCESS"]){scrollRetried=YES;[p writeToFile:[out stringByAppendingPathComponent:@"scroll-stall.bgra"] atomically:YES];[events addObject:@{@"scroll_stall_retry":@YES,@"stall_ocr":text,@"seconds_including_OCR":@(-[start timeIntervalSinceNow])}];fprintf(stderr,"MULTITAB %s SCROLL STALL; retrying burst\n",kind.UTF8String);for(int i=0;i<35;i++){[c pointerX:650 y:430 buttons:16];[c pointerX:650 y:430 buttons:0];}changed=NSDate.date;}
 else if(phase==4&&[text containsString:@"SCROLL SUCCESS"]){scroll=YES;[p writeToFile:[out stringByAppendingPathComponent:@"scroll.bgra"] atomically:YES];for(int i=0;i<4;i++)dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(i*.25*NSEC_PER_SEC)),dispatch_get_main_queue(),^{chord(c,'w');});dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(1.5*NSEC_PER_SEC)),dispatch_get_main_queue(),^{firstTab(c,kind);});phase=5;changed=NSDate.date;}
 else if(phase==5&&-[changed timeIntervalSinceNow]>7&&tabReady(text,1)&&[text containsString:@"JS VERIFIED"]){closed=YES;[p writeToFile:[out stringByAppendingPathComponent:@"first-tab-restored.bgra"] atomically:YES];if([kind isEqualToString:@"firefox"])chord(c,'q');else{[c key:0xffe3 down:YES];[c key:0xffe1 down:YES];key(c,'w');[c key:0xffe1 down:NO];[c key:0xffe3 down:NO];}phase=6;changed=NSDate.date;}
 else if(phase==6&&-[changed timeIntervalSinceNow]>7&&browserExitedCleanly()){finished=YES;}
 }
 [c stop];NSDictionary*r=@{@"environment":@"Mac ARM64 interpreter; not physical iPhone",@"application":kind,@"wps_page_pixels_verified":@(wps),@"https_page_pixels_verified":@(https),@"search_results_verified":@(search),@"tabs_with_individually_verified_JS_content":@(verified),@"scroll_pixels_verified":@(scroll),@"scroll_needed_retry":@(scrollRetried),@"first_tab_verified_after_close_inputs":@(closed),@"close_requests_queued":@((phase==5||phase==6)?4:0),@"normal_browser_exit_verified":@(finished),@"finished":@(finished),@"seconds_including_OCR":@(-[start timeIntervalSinceNow]),@"events":events,@"last_ocr":text,@"quit_confirmation_clicks":@(quitConfirmations),@"permission_prompt_block_clicks":@(permissionDismissals),@"guest_GPU_acceleration":@"NOT measured"};[[NSJSONSerialization dataWithJSONObject:r options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[out stringByAppendingPathComponent:@"browser-multitab.json"] atomically:YES];return finished&&wps&&https&&search&&verified==5&&scroll&&!scrollRetried&&closed?0:1;
}}
