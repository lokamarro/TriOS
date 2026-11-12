// Observe real guest X11 window mapping and pixels, without claiming app behavior.
#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
int main(int argc,char **argv){@autoreleasepool{
 if(argc!=5||!getenv("LR_GUI_APPLICATION_LOG"))return 2;
 double timeout=getenv("LR_GUI_CAPTURE_SECONDS")?atof(getenv("LR_GUI_CAPTURE_SECONDS")):40;
 if(timeout<5||timeout>600)return 2;
 double settle=getenv("LR_GUI_CAPTURE_SETTLE_SECONDS")?atof(getenv("LR_GUI_CAPTURE_SETTLE_SECONDS")):2;
 if(settle<2||settle>120)return 2;
 NSString *directory=@(argv[3]);NSString *session=[@(getenv("LR_GUI_APPLICATION_LOG")) stringByDeletingLastPathComponent];
 NSString *marker=getenv("LR_GUI_READY_TEXT")?@(getenv("LR_GUI_READY_TEXT")):nil;
 NSString *password=[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil];
 LRRFBClient *c=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:password];
 __block NSData *pixels=nil;__block uint64_t frames=0,bytes=0;__block uint16_t width=0,height=0;__block BOOL failed=NO;
 c.statusHandler=^(NSString *s){fprintf(stderr,"%s\n",s.UTF8String);dispatch_async(dispatch_get_main_queue(),^{if([s hasPrefix:@"Display error:"])failed=YES;});};
 c.frameHandler=^(NSData *d,uint16_t w,uint16_t h,uint64_t n,uint64_t b){dispatch_async(dispatch_get_main_queue(),^{pixels=d;width=w;height=h;frames=n;bytes=b;});};
 [c start];NSDate *begin=NSDate.date,*readySince=nil;BOOL mapped=NO,markerSeen=marker==nil;NSUInteger colored=0;
 while(!failed&&-[begin timeIntervalSinceNow]<timeout){
  [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];
  NSString *state=[NSString stringWithContentsOfFile:[session stringByAppendingPathComponent:@"window-state"] encoding:NSUTF8StringEncoding error:nil];
  for(NSString *line in [state componentsSeparatedByString:@"\n"]){if([line hasPrefix:@"mapped="])mapped=[[line substringFromIndex:7] integerValue]>0;}
  colored=0;const uint8_t *p=pixels.bytes;for(NSUInteger i=0;i+3<pixels.length;i+=4)if(p[i]||p[i+1]||p[i+2])colored++;
  if(marker)markerSeen=[[NSString stringWithContentsOfFile:@(getenv("LR_GUI_APPLICATION_LOG")) encoding:NSUTF8StringEncoding error:nil] containsString:marker];
  if(mapped&&colored>100&&markerSeen){if(!readySince)readySince=NSDate.date;if(-[readySince timeIntervalSinceNow]>=settle&&-[begin timeIntervalSinceNow]>5)break;}
  else readySince=nil;
 }
 [pixels writeToFile:[directory stringByAppendingPathComponent:@"window.bgra"] atomically:YES];
 NSDictionary *report=@{@"mapped":@(mapped),@"nonblack_pixels":@(colored),@"diagnostic_marker_seen":@(markerSeen),@"width":@(width),@"height":@(height),@"frames":@(frames),@"wire_bytes":@(bytes),@"seconds":@(-[begin timeIntervalSinceNow]),@"application_behavior":@"not tested by this snapshot"};
 [[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[directory stringByAppendingPathComponent:@"snapshot.json"] atomically:YES];[c stop];return mapped&&colored>100&&markerSeen&&!failed?0:1;
}}
