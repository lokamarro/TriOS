#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
#import "BlenderFrameObservation.h"
int main(int argc,char **argv){@autoreleasepool{
 if(argc!=4)return 2;
 NSString *password=[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil];
 LRBlenderFrameObservation *observation=[LRBlenderFrameObservation new];
 LRRFBClient *client=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:password];
 double start=NSProcessInfo.processInfo.systemUptime;
 __weak LRRFBClient *weak=client;
 client.transportMetricsHandler=^(NSDictionary *event){[observation observeTransport:event];};
 client.statusHandler=^(NSString *status){if([status hasPrefix:@"Display error:"])[observation markFailed];};
 client.frameHandler=^(NSData *pixels,uint16_t w,uint16_t h,uint64_t f,uint64_t bytes){
  [observation observe:pixels width:w height:h frames:f bytes:bytes at:NSProcessInfo.processInfo.systemUptime];
  [weak pointerX:3 y:4 buttons:1];[weak key:'A' down:YES];[weak key:'A' down:NO];
 };
 [client start];BOOL complete=NO;
 while(NSProcessInfo.processInfo.systemUptime-start<5){
  [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
  NSDictionary *metrics=[observation metricsSince:start];
  if([metrics[@"transport_events_observed"] unsignedIntegerValue]>=4){complete=YES;break;}
 }
 [client stop];
 NSDictionary *report=[observation metricsSince:start];
 [[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil]writeToFile:@(argv[3]) atomically:YES];
 return complete&&![[observation snapshot][@"failed"] boolValue]?0:1;
}}
