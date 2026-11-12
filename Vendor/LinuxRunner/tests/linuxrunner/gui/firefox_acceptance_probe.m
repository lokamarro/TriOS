#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
static void key(LRRFBClient*c,uint32_t k){[c key:k down:YES];[c key:k down:NO];}
static void click(LRRFBClient*c,int x,int y){[c pointerX:x y:y buttons:1];[c pointerX:x y:y buttons:0];}
static void text(LRRFBClient*c,const char*s){size_t n=strlen(s);uint32_t syms[n];for(size_t i=0;i<n;i++)syms[i]=(unsigned char)s[i];[c typeKeysyms:syms count:n];}
static void navigate(LRRFBClient*c,const char*s){NSString*url=@(s);[c key:0xffe3 down:YES];key(c,'l');[c key:0xffe3 down:NO];dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),dispatch_get_main_queue(),^{text(c,url.UTF8String);dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),dispatch_get_main_queue(),^{key(c,0xff0d);});});}
int main(int argc,char**argv){@autoreleasepool{
 if(argc!=5)return 2;NSString*dir=@(argv[3]);LRRFBClient*c=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil]];
 __block NSData*last;__block BOOL started=NO,ended=NO,failed=NO,closeObserved=NO,quitRequested=NO,quitConfirmed=NO;__block uint64_t frames=0,bytes=0;NSDate*begin=NSDate.date;
 c.statusHandler=^(NSString*s){dispatch_async(dispatch_get_main_queue(),^{fprintf(stderr,"%s\n",s.UTF8String);if([s hasPrefix:@"Display error:"]){failed=YES;ended=YES;}if([s hasPrefix:@"Linux display ended"]){closeObserved=YES;ended=YES;}});};
 c.frameHandler=^(NSData*d,uint16_t w,uint16_t h,uint64_t n,uint64_t b){dispatch_async(dispatch_get_main_queue(),^{last=d;frames=n;bytes=b;const uint8_t*p=d.bytes;
 if(quitRequested&&!quitConfirmed&&w==640&&h==480&&d.length>=640*480*4){
  // Firefox uses different actual dialogs for one tab and multiple tabs.
  // Observe the rendered primary button instead of guessing paint timing.
  for(int y=180;y<=232;y+=52){size_t button=(y*640+360)*4;
   if(p[button]>200&&p[button+1]>70&&p[button+1]<140&&p[button+2]<30){
    [last writeToFile:[dir stringByAppendingPathComponent:@"quit-dialog.bgra"] atomically:YES];
    quitConfirmed=YES;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),dispatch_get_main_queue(),^{
     [c pointerX:415 y:y buttons:1];
     dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC/10),dispatch_get_main_queue(),^{[c pointerX:415 y:y buttons:0];});
    });
    // Also exercise the genuine modal's keyboard confirmation, after giving
    // the ordinary press/release sequence time to reach the application.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,4*NSEC_PER_SEC),dispatch_get_main_queue(),^{key(c,0xff0d);});
    fprintf(stderr,"FIREFOX_PROBE: rendered_quit_confirmation_clicked y=%d\n",y);break;
   }
  }
 }
NSUInteger colored=0;for(NSUInteger i=0;i+3<d.length;i+=4)if(p[i]||p[i+1]||p[i+2])colored++;
 if(!started&&w==640&&h==480&&colored>10000){started=YES;fprintf(stderr,"FIREFOX_PROBE: chrome_pixels_observed=%.3f transport_not_page_acceptance\n",-[begin timeIntervalSinceNow]);
 void(^after)(int,dispatch_block_t)=^(int seconds,dispatch_block_t action){dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)seconds*NSEC_PER_SEC),dispatch_get_main_queue(),action);};
 if(getenv("LR_FIREFOX_CLOSE_ONLY")){
 after(5,^{navigate(c,"file:///home/mobile/Downloads/.linuxrunner/gui/firefox-test.html");});
 after(25,^{[last writeToFile:[dir stringByAppendingPathComponent:@"local.bgra"] atomically:YES];});
 after(30,^{quitRequested=YES;[c key:0xffe3 down:YES];key(c,'q');[c key:0xffe3 down:NO];});
 // Firefox genuinely asks whether Ctrl-Q means close tab or quit. Keep its
 // setting intact and confirm the actual dialog through native pointer input.
 after(120,^{ended=YES;});
 }else{
 after(5,^{navigate(c,"file:///home/mobile/Downloads/.linuxrunner/gui/firefox-test.html");});
 after(75,^{click(c,552,110);});
 after(80,^{[last writeToFile:[dir stringByAppendingPathComponent:@"local.bgra"] atomically:YES];});
 after(82,^{click(c,100,300);text(c,"Firefox iOS27 AaZz09 !?");});
 after(88,^{click(c,310,300);});
 after(98,^{[last writeToFile:[dir stringByAppendingPathComponent:@"form.bgra"] atomically:YES];});
 after(102,^{for(int i=0;i<16;i++){[c pointerX:450 y:400 buttons:16];[c pointerX:450 y:400 buttons:0];}});
 after(113,^{[last writeToFile:[dir stringByAppendingPathComponent:@"scroll.bgra"] atomically:YES];});
 after(118,^{navigate(c,"https://example.com");});
 after(190,^{[last writeToFile:[dir stringByAppendingPathComponent:@"https.bgra"] atomically:YES];navigate(c,"https://etsbi.edu.ba/");});
 after(270,^{[last writeToFile:[dir stringByAppendingPathComponent:@"etsbi.bgra"] atomically:YES];});
 after(275,^{[c key:0xffe9 down:YES];key(c,0xff51);[c key:0xffe9 down:NO];});
 after(295,^{[last writeToFile:[dir stringByAppendingPathComponent:@"back.bgra"] atomically:YES];});
 after(300,^{quitRequested=YES;[c key:0xffe3 down:YES];key(c,'q');[c key:0xffe3 down:NO];});
 after(345,^{ended=YES;});
 }
 }});};[c start];while(!ended&&-[begin timeIntervalSinceNow]<400)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];
 [last writeToFile:[dir stringByAppendingPathComponent:@"final.bgra"] atomically:YES];[c stop];c.frameHandler=nil;c.statusHandler=nil;
 NSDictionary*r=@{@"transport_only_not_browser_success":@YES,@"actions_started":@(started),@"display_close_observed":@(closeObserved),@"transport_failed":@(failed),@"quit_confirmation_clicked":@(quitConfirmed),@"frames":@(frames),@"wire_bytes":@(bytes),@"seconds":@(-[begin timeIntervalSinceNow])};[[NSJSONSerialization dataWithJSONObject:r options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[dir stringByAppendingPathComponent:@"transport.json"] atomically:YES];return started&&!failed&&(!getenv("LR_FIREFOX_CLOSE_ONLY")||closeObserved)?0:1;
}}
