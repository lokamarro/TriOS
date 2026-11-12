#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
#include <time.h>
#include <errno.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
int main(int argc,char **argv){@autoreleasepool{
 if(argc!=5)return 2;
 unsigned target=30;const char *requested=getenv("LR_RFB_LATENCY_SAMPLES");
 if(requested){char *end=NULL;errno=0;unsigned long value=strtoul(requested,&end,10);
  if(errno||end==requested||*end||value<30||value>300)return 2;target=(unsigned)value;}
 NSString *password=[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil];NSString *dir=@(argv[3]);
 LRRFBClient*c=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:password];
 __block BOOL ended=NO,started=NO,pending=NO;__block unsigned n=0;__block double sent=0;__block uint64_t frames=0,bytes=0;NSMutableArray *latencies=[NSMutableArray new], *inputTimes=[NSMutableArray new], *inputWrites=[NSMutableArray new];double began=now();
 c.transportMetricsHandler=^(NSDictionary *event){ if([event[@"stage"] isEqual:@"rfb_input_write"] && [event[@"packet_type"] unsignedIntValue]==5) @synchronized(inputWrites){[inputWrites addObject:event];} };
 c.statusHandler=^(NSString*s){dispatch_async(dispatch_get_main_queue(),^{if([s hasPrefix:@"Linux display ended"]||[s hasPrefix:@"Display error:"])ended=YES;});};
 __weak LRRFBClient *weak=c;
 c.frameHandler=^(NSData*d,uint16_t w,uint16_t h,uint64_t f,uint64_t b){double received=now();dispatch_async(dispatch_get_main_queue(),^{
  if(w!=640||h!=480)return;const uint8_t*p=d.bytes;size_t pattern=(100*w+50)*4,counter=(220*w+100)*4;frames=f;bytes=b;
  if(p[pattern]!=0x40||p[pattern+2]!=0xd8)return;
  BOOL green=p[counter]==0x56&&p[counter+1]==0x6c&&p[counter+2]==0x23;
  if(!started){started=YES;}
  else if(pending&&green==(n%2==0)){[latencies addObject:@((received-sent)*1000)];pending=NO;n++;}
  if(!pending&&n<target){sent=now();[inputTimes addObject:@(sent*1000)];pending=YES;[weak pointerX:100 y:230 buttons:1];[weak pointerX:100 y:230 buttons:0];}
  else if(n==target){n++;[weak key:0xff1b down:YES];[weak key:0xff1b down:NO];}
 });};[c start];
 while(!ended&&now()-began<45)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];
 [c stop];NSArray<NSNumber *> *sorted=[latencies sortedArrayUsingSelector:@selector(compare:)];double total=0;for(NSNumber *v in sorted)total+=v.doubleValue;
 NSArray *writes;@synchronized(inputWrites){writes=[inputWrites copy];}
 NSDictionary*r=@{@"scope":@"host Linux process to native RFB decode; excludes UIKit/device",@"samples":@(sorted.count),@"requested_samples":@(target),@"latency_ms":latencies,@"sent_monotonic_ms":inputTimes,@"pointer_writes":writes,@"median_ms":sorted.count?@((sorted[(sorted.count-1)/2].doubleValue+sorted[sorted.count/2].doubleValue)/2):@0,@"p50_ms":sorted.count?sorted[(sorted.count+1)/2-1]:@0,@"p95_ms":sorted.count?sorted[(sorted.count*95+99)/100-1]:@0,@"p99_ms":sorted.count?sorted[(sorted.count*99+99)/100-1]:@0,@"mean_ms":@(sorted.count?total/sorted.count:0),@"frames":@(frames),@"wire_bytes":@(bytes),@"display_ended":@(ended),@"seconds":@(now()-began)};
 [[NSJSONSerialization dataWithJSONObject:r options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[dir stringByAppendingPathComponent:@"probe.json"] atomically:YES];return sorted.count==target&&ended?0:1;
}}
