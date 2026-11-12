#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
#include <stdatomic.h>
void LRMakeAutoreleasedFrameLease(void);
unsigned LRLiveFrameLeases(void),LRPeakFrameLeases(void);
int main(int argc,char **argv){@autoreleasepool{
 if(argc!=4)return 2;
 NSString *password=[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil];
 LRRFBClient *client=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:password];
 __weak LRRFBClient *weakClient=client;
 NSMutableArray *retained=[NSMutableArray new];NSLock *lock=[NSLock new];
 __block BOOL failed=NO;__block unsigned received=0;__block NSUInteger peakRawBytes=0;
 client.statusHandler=^(NSString *status){if([status hasPrefix:@"Display error:"]){[lock lock];failed=YES;[lock unlock];}};
 client.frameHandler=^(NSData *pixels,uint16_t w,uint16_t h,uint64_t frame,uint64_t bytes){
  LRMakeAutoreleasedFrameLease();[lock lock];
  if(w!=32||h!=16||pixels.length!=2048||frame!=received+1)failed=YES;
  peakRawBytes=MAX(peakRawBytes,[[weakClient valueForKey:@"raw"]length]);
  [retained addObject:pixels];received++;[lock unlock];
 };
 [client start];double start=NSProcessInfo.processInfo.systemUptime;
 while(NSProcessInfo.processInfo.systemUptime-start<5){
  [lock lock];BOOL done=received==32||failed;[lock unlock];if(done)break;
  [NSThread sleepForTimeInterval:.005];
 }
 // No new frames: let the final callback/pool boundary finish before sampling.
 [NSThread sleepForTimeInterval:.04];unsigned beforeStop=LRLiveFrameLeases();
 [client stop];start=NSProcessInfo.processInfo.systemUptime;
 while(LRLiveFrameLeases()&&NSProcessInfo.processInfo.systemUptime-start<1)[NSThread sleepForTimeInterval:.005];
 [lock lock];
 for(NSUInteger i=0;i<retained.count;i++){
  const uint8_t *p=[retained[i]bytes];for(NSUInteger j=0;j<2048;j++)if(p[j]!=(uint8_t)(i+1)){failed=YES;break;}
 }
 NSDictionary *report=@{@"frames":@(received),@"peak_intermediate_raw_bytes":@(peakRawBytes),@"live_before_stop":@(beforeStop),@"peak_autoreleased_objects":@(LRPeakFrameLeases()),@"live_after_stop":@(LRLiveFrameLeases()),@"retained_pixels_correct":@(!failed&&received==32)};
 [[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil]writeToFile:@(argv[3]) atomically:YES];
 [lock unlock];return failed||received!=32||LRLiveFrameLeases()!=0?1:0;
}}
