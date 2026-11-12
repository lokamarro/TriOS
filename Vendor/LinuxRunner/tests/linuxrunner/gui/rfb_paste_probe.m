#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
// A deliberately held writer exposes queue overflow without depending on
// kernel socket-buffer size or host scheduling. This is a transport contract,
// not a claim about physical-device paste/display latency.
int main(int argc,char **argv){@autoreleasepool{
 if(argc!=6)return 2;
 NSUInteger count=strtoul(argv[4],NULL,10);if(!count||count>16384)return 2;
 BOOL hold=atoi(argv[5]);
 LRRFBClient *client=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil]];
 NSLock *lock=[NSLock new];dispatch_semaphore_t writer=dispatch_semaphore_create(0);
 __block BOOL failed=NO,done=NO,started=NO;__block NSUInteger writes=0;__block double enqueueMs=0;
 NSMutableArray *statuses=[NSMutableArray new];
 client.statusHandler=^(NSString *status){[lock lock];[statuses addObject:status];if([status hasPrefix:@"Display error:"])failed=YES;[lock unlock];};
 client.transportMetricsHandler=^(NSDictionary *e){if(![e[@"stage"] isEqual:@"rfb_input_write"])return;
  [lock lock];NSUInteger n=++writes;if(![e[@"written"] boolValue])failed=YES;[lock unlock];
  if(n==1){dispatch_semaphore_signal(writer);if(hold)[NSThread sleepForTimeInterval:.2];}
  if([e[@"packet_type"] intValue]==5&&n>1){[lock lock];done=YES;[lock unlock];}
 };
 __weak LRRFBClient *weakClient=client;
 client.frameHandler=^(NSData *pixels,uint16_t w,uint16_t h,uint64_t frames,uint64_t bytes){
  LRRFBClient *client=weakClient;if(!client)return;
  [lock lock];BOOL duplicate=started;started=YES;[lock unlock];if(duplicate)return;
  [client pointerX:0 y:0 buttons:0];
  if(dispatch_semaphore_wait(writer,dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC)))return;
  NSMutableData *keys=[NSMutableData dataWithLength:count*4];uint32_t *symbols=keys.mutableBytes;
  for(NSUInteger i=0;i<count;i++)symbols[i]=(i%11==0)?0x010003bb:(uint32_t)('a'+i%26);
  double began=NSProcessInfo.processInfo.systemUptime;
  [client typeKeysyms:symbols count:count];
  [client key:0xff0d down:YES];[client key:0xff0d down:NO];[client pointerX:31 y:31 buttons:0];
  [lock lock];enqueueMs=(NSProcessInfo.processInfo.systemUptime-began)*1000;[lock unlock];
 };
 double began=NSProcessInfo.processInfo.systemUptime;[client start];
 while(NSProcessInfo.processInfo.systemUptime-began<4){
  [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.005]];
  [lock lock];BOOL finished=failed||done;[lock unlock];if(finished)break;
 }
 [client stop];[lock lock];NSDictionary *report=@{@"count":@(count),@"hold_writer":@(hold),@"failed":@(failed),@"completed":@(done),@"write_events":@(writes),@"enqueue_ms":@(enqueueMs),@"statuses":[statuses copy]};[lock unlock];
 [[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil]writeToFile:@(argv[3]) atomically:YES];
 client.frameHandler=nil;client.transportMetricsHandler=nil;client.statusHandler=nil;
 return done&&!failed?0:1;
}}
