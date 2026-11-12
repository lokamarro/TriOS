#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
#include <time.h>

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static int marker(NSData *pixels,uint16_t w,uint16_t h) {
 if(pixels.length!=(NSUInteger)w*h*4||w<100)return -1;
 const unsigned char *p=pixels.bytes;
 const unsigned char colors[2][2][3]={{{70,30,230},{190,220,10}},{{230,60,15},{10,200,230}}};
 for(unsigned y=0;y<h;y+=4)for(unsigned x=0;x+64<w;x+=4)for(int state=0;state<2;state++) {
  const unsigned char *a=p+(y*w+x)*4,*b=a+32*4,*c=a+64*4;
  if(!memcmp(a,colors[state][0],3)&&!memcmp(b,colors[state][1],3)&&!memcmp(c,colors[state][0],3))return state;
 }
 return -1;
}
static NSDictionary *summary(NSArray<NSNumber *> *samples) {
 NSArray *s=[samples sortedArrayUsingSelector:@selector(compare:)];NSUInteger n=s.count;
 if(!n)return @{@"count":@0};
 double median=n%2?[s[n/2] doubleValue]:([s[n/2-1] doubleValue]+[s[n/2] doubleValue])/2;
 return @{@"count":@(n),@"median_ms":@(median),@"p95_ms":s[(NSUInteger)ceil(.95*n)-1],@"p99_ms":s[(NSUInteger)ceil(.99*n)-1],@"max_ms":s.lastObject};
}
int main(int argc,char **argv){@autoreleasepool{
 if(argc!=5)return 2;NSString *output=@(argv[3]);
 LRRFBClient *client=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil]];
 __weak LRRFBClient *input=client;
 const NSUInteger target=100;double started=now();
 __block double sent=0,ready=0;__block NSUInteger delivered=0;__block int expected=0;
 __block BOOL waiting=NO,ended=NO,finished=NO;
 NSMutableArray *keys=[NSMutableArray new],*wheels=[NSMutableArray new],*events=[NSMutableArray new];
 void (^send)(void)=^{
  expected^=1;sent=now();waiting=YES;
  if(delivered%2==0){[input key:'q' down:YES];[input key:'q' down:NO];}
  else{uint8_t button=((delivered/2)%2)?8:16;[input pointerX:450 y:380 buttons:button];[input pointerX:450 y:380 buttons:0];}
 };
 client.statusHandler=^(NSString *s){fprintf(stderr,"%s\n",s.UTF8String);if([s hasPrefix:@"Display error:"]||[s hasPrefix:@"Linux display ended"])dispatch_async(dispatch_get_main_queue(),^{ended=YES;});};
 client.frameHandler=^(NSData *pixels,uint16_t w,uint16_t h,uint64_t frames,uint64_t bytes){
  (void)frames;(void)bytes;int state=marker(pixels,w,h);double received=now();if(state<0)return;
  dispatch_async(dispatch_get_main_queue(),^{
   if(finished||ended)return;
   if(!ready){if(state!=0)return;ready=received-started;[input pointerX:450 y:380 buttons:1];[input pointerX:450 y:380 buttons:0];send();return;}
   if(!waiting||state!=expected||received<sent)return;
   double ms=(received-sent)*1000;NSString *kind=delivered%2?@"wheel":@"keyboard";
   [(delivered%2?wheels:keys) addObject:@(ms)];[events addObject:@{@"kind":kind,@"latency_ms":@(ms),@"state":@(state),@"event":@(delivered+1)}];
   waiting=NO;delivered++;if(delivered==target*2)finished=YES;else send();
  });
 };
 [client start];
 while(!ended&&!finished&&now()-started<300){[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.01]];if(waiting&&now()-sent>15){ended=YES;fprintf(stderr,"FIREFOX_INTERACTION_TIMEOUT event=%lu\n",(unsigned long)delivered+1);}}
 [client stop];NSDictionary *report=@{@"environment":@"Mac host interpreter; physical iPhone NOT TESTED",@"scope":@"native input enqueue to Firefox DOM event and corresponding RFB pixel receipt; excludes UIKit/GPU presentation",@"page_ready_seconds":@(ready),@"expected_events":@(target*2),@"delivered_events":@(delivered),@"keyboard":summary(keys),@"wheel":summary(wheels),@"events":events,@"passed":@(finished&&!ended)};
 [[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[output stringByAppendingPathComponent:@"firefox-interaction.json"] atomically:YES];
 fprintf(stderr,"FIREFOX_INTERACTION_%s delivered=%lu\n",finished&&!ended?"PASS":"FAIL",(unsigned long)delivered);return finished&&!ended?0:1;
}}
