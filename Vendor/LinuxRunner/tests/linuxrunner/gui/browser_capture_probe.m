#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
int main(int argc,char **argv){@autoreleasepool{
 if(argc!=5)return 2;NSString *password=[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil],*dir=@(argv[3]);LRRFBClient *c=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:password];
 __block BOOL ended=NO;__block uint64_t frames=0,bytes=0;__block NSData *last;NSDate *start=NSDate.date;
 c.statusHandler=^(NSString*s){dispatch_async(dispatch_get_main_queue(),^{fprintf(stderr,"%s\n",s.UTF8String);if([s hasPrefix:@"Linux display ended"]||[s hasPrefix:@"Display error:"])ended=YES;});};
 c.frameHandler=^(NSData*d,uint16_t w,uint16_t h,uint64_t n,uint64_t b){dispatch_async(dispatch_get_main_queue(),^{frames=n;bytes=b;last=d;if(n==1||n%10==0)[d writeToFile:[dir stringByAppendingPathComponent:@"latest.bgra"] atomically:YES];});};
 [c start];while(!ended&&-[start timeIntervalSinceNow]<110)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];
 [last writeToFile:[dir stringByAppendingPathComponent:@"final.bgra"] atomically:YES];[c stop];NSDictionary *r=@{@"transport_only_not_browser_success":@YES,@"frames":@(frames),@"wire_bytes":@(bytes),@"ended":@(ended),@"seconds":@(-[start timeIntervalSinceNow])};[[NSJSONSerialization dataWithJSONObject:r options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[dir stringByAppendingPathComponent:@"transport.json"] atomically:YES];return frames?0:1;
}}
