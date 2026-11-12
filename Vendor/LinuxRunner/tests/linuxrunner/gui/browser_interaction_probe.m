#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
static void click(LRRFBClient*c,int x,int y){[c pointerX:x y:y buttons:1];[c pointerX:x y:y buttons:0];}
static void key(LRRFBClient*c,uint32_t k){[c key:k down:YES];[c key:k down:NO];}
static void type(LRRFBClient*c,const char*t){for(;*t;t++)key(c,*t=='\n'?0xff0d:(unsigned char)*t);}
int main(int argc,char **argv){@autoreleasepool{
 if(argc!=5)return 2;NSString *password=[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil],*dir=@(argv[3]);LRRFBClient *c=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:password];
 __block BOOL ended=NO,exerciseStarted=NO;__block void (^exercise)(void);__block uint64_t frames=0,bytes=0;__block NSData *last;NSDate *start=NSDate.date;
 c.statusHandler=^(NSString*s){dispatch_async(dispatch_get_main_queue(),^{fprintf(stderr,"%s\n",s.UTF8String);if([s hasPrefix:@"Linux display ended"]||[s hasPrefix:@"Display error:"])ended=YES;});};
 c.frameHandler=^(NSData*d,uint16_t w,uint16_t h,uint64_t n,uint64_t b){dispatch_async(dispatch_get_main_queue(),^{frames=n;bytes=b;last=d;const uint8_t*p=d.bytes;size_t i=(330*640+40)*4;if(!exerciseStarted&&w==640&&h==480&&d.length>i+3&&p[i]==0x9d&&p[i+1]==0xf5&&p[i+2]==0xff){exerciseStarted=YES;exercise();}if(n==1||n%10==0)[d writeToFile:[dir stringByAppendingPathComponent:@"latest.bgra"] atomically:YES];});};
 exercise=^{
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC),dispatch_get_main_queue(),^{click(c,100,275);type(c,"Browser iOS27 AaZz09 !?");});
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,7*NSEC_PER_SEC),dispatch_get_main_queue(),^{click(c,300,275);});
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,12*NSEC_PER_SEC),dispatch_get_main_queue(),^{[last writeToFile:[dir stringByAppendingPathComponent:@"form.bgra"] atomically:YES];for(int i=0;i<16;i++){[c pointerX:450 y:400 buttons:16];[c pointerX:450 y:400 buttons:0];}});
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,18*NSEC_PER_SEC),dispatch_get_main_queue(),^{[last writeToFile:[dir stringByAppendingPathComponent:@"scroll.bgra"] atomically:YES];for(int i=0;i<16;i++){[c pointerX:450 y:400 buttons:8];[c pointerX:450 y:400 buttons:0];}});
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,25*NSEC_PER_SEC),dispatch_get_main_queue(),^{click(c,300,385);});
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,32*NSEC_PER_SEC),dispatch_get_main_queue(),^{click(c,390,25);[c key:0xffe3 down:YES];key(c,'a');[c key:0xffe3 down:NO];type(c,"https://example.com\n");});
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,57*NSEC_PER_SEC),dispatch_get_main_queue(),^{[last writeToFile:[dir stringByAppendingPathComponent:@"https.bgra"] atomically:YES];click(c,45,25);});
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,72*NSEC_PER_SEC),dispatch_get_main_queue(),^{click(c,540,25);});
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,79*NSEC_PER_SEC),dispatch_get_main_queue(),^{[c key:0xffe3 down:YES];key(c,'l');[c key:0xffe3 down:NO];type(c,"http://127.0.0.1:18766/download.txt\n");});
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,90*NSEC_PER_SEC),dispatch_get_main_queue(),^{[c key:0xffe3 down:YES];key(c,'q');[c key:0xffe3 down:NO];});
 };[c start];
 while(!ended&&-[start timeIntervalSinceNow]<110)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];
 [last writeToFile:[dir stringByAppendingPathComponent:@"final.bgra"] atomically:YES];[c stop];NSDictionary *r=@{@"transport_only_not_browser_success":@YES,@"exercise_started_after_guest_js_render":@(exerciseStarted),@"frames":@(frames),@"wire_bytes":@(bytes),@"ended":@(ended),@"seconds":@(-[start timeIntervalSinceNow])};[[NSJSONSerialization dataWithJSONObject:r options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[dir stringByAppendingPathComponent:@"transport.json"] atomically:YES];return frames?0:1;
}}
