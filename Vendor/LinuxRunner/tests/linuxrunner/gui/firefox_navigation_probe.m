#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
static void key(LRRFBClient *c,uint32_t k){[c key:k down:YES];[c key:k down:NO];}
static void navigate(LRRFBClient *c,const char *url){[c key:0xffe3 down:YES];key(c,'l');[c key:0xffe3 down:NO];size_t n=strlen(url);uint32_t symbols[n];for(size_t i=0;i<n;i++)symbols[i]=(unsigned char)url[i];[c typeKeysyms:symbols count:n];key(c,0xff0d);}
int main(int argc,char **argv){@autoreleasepool{
 if(argc!=5)return 2;NSString *dir=@(argv[3]);LRRFBClient *c=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil]];
 __block NSData *last;__block BOOL started=NO,ended=NO;__block uint64_t frames=0,bytes=0;NSDate *begin=NSDate.date;
 c.statusHandler=^(NSString*s){dispatch_async(dispatch_get_main_queue(),^{fprintf(stderr,"%s\n",s.UTF8String);if([s hasPrefix:@"Display error:"])ended=YES;});};
 c.frameHandler=^(NSData*d,uint16_t w,uint16_t h,uint64_t n,uint64_t b){dispatch_async(dispatch_get_main_queue(),^{last=d;frames=n;bytes=b;const uint8_t*p=d.bytes;NSUInteger colored=0;for(NSUInteger i=0;i+3<d.length;i+=4)if(p[i]||p[i+1]||p[i+2])colored++;
 if(!started&&w==640&&h==480&&colored>10000){started=YES;fprintf(stderr,"FIREFOX_PROBE: chrome_pixels_observed=%.3f transport_not_page_acceptance\n",-[begin timeIntervalSinceNow]);
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC),dispatch_get_main_queue(),^{navigate(c,"file:///home/mobile/Downloads/.linuxrunner/gui/firefox-test.html");});
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,90*NSEC_PER_SEC),dispatch_get_main_queue(),^{[last writeToFile:[dir stringByAppendingPathComponent:@"local.bgra"] atomically:YES];navigate(c,"https://example.com");});
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,170*NSEC_PER_SEC),dispatch_get_main_queue(),^{[last writeToFile:[dir stringByAppendingPathComponent:@"https.bgra"] atomically:YES];[c key:0xffe3 down:YES];key(c,'q');[c key:0xffe3 down:NO];});
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,185*NSEC_PER_SEC),dispatch_get_main_queue(),^{ended=YES;});
 }});};[c start];while(!ended&&-[begin timeIntervalSinceNow]<300)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];
 [last writeToFile:[dir stringByAppendingPathComponent:@"final.bgra"] atomically:YES];[c stop];c.frameHandler=nil;c.statusHandler=nil;NSDictionary*r=@{@"transport_only_not_browser_success":@YES,@"actions_started":@(started),@"frames":@(frames),@"wire_bytes":@(bytes),@"seconds":@(-[begin timeIntervalSinceNow])};[[NSJSONSerialization dataWithJSONObject:r options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[dir stringByAppendingPathComponent:@"transport.json"] atomically:YES];return started?0:1;
}}
