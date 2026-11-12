#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
typedef void (^LRFrameCallback)(NSData *,uint16_t,uint16_t,uint64_t,uint64_t);
typedef void (^LRPatchCallback)(NSData *,uint16_t,uint16_t,uint64_t,uint64_t,LRFrameDamage);
typedef void (^LRStatusCallback)(NSString *);
// Deterministically clear the property after its first getter. On a subsequent
// nil read return a sentinel instead of crashing the test process at NULL.
@interface LRCallbackRaceClient : LRRFBClient
@property NSString *race;
@property BOOL injected,sentinelInvoked,realInvoked;
@property NSUInteger clearedReads;
@end
@implementation LRCallbackRaceClient
- (LRStatusCallback)statusHandler {
 LRStatusCallback h=[super statusHandler];if(![_race isEqual:@"status"])return h;
 if(h&&!self.injected){self.injected=YES;[super setStatusHandler:nil];return h;}
 if(!h&&self.injected&&!self.realInvoked){self.clearedReads++;return ^(NSString *s){self.sentinelInvoked=YES;};}return h;
}
- (LRFrameCallback)frameHandler {
 LRFrameCallback h=[super frameHandler];if(![_race isEqual:@"frame"])return h;
 if(h&&!self.injected){self.injected=YES;[super setFrameHandler:nil];return h;}
 if(!h&&self.injected&&!self.realInvoked){self.clearedReads++;return ^(NSData *p,uint16_t w,uint16_t h,uint64_t f,uint64_t b){self.sentinelInvoked=YES;};}return h;
}
- (LRPatchCallback)damageFrameHandler {
 LRPatchCallback h=[super damageFrameHandler];if(![_race isEqual:@"damage"])return h;
 if(h&&!self.injected){self.injected=YES;[super setDamageFrameHandler:nil];return h;}
 if(!h&&self.injected&&!self.realInvoked){self.clearedReads++;return ^(NSData *p,uint16_t w,uint16_t h,uint64_t f,uint64_t b,LRFrameDamage d){self.sentinelInvoked=YES;};}return h;
}
- (LRPatchCallback)patchFrameHandler {
 LRPatchCallback h=[super patchFrameHandler];if(![_race isEqual:@"patch"])return h;
 if(h&&!self.injected){self.injected=YES;[super setPatchFrameHandler:nil];return h;}
 if(!h&&self.injected&&!self.realInvoked){self.clearedReads++;return ^(NSData *p,uint16_t w,uint16_t h,uint64_t f,uint64_t b,LRFrameDamage d){self.sentinelInvoked=YES;};}return h;
}
@end
int main(int argc,char **argv){@autoreleasepool{
 if(argc!=5)return 2;
 LRCallbackRaceClient *client=[[LRCallbackRaceClient alloc]initWithPort:atoi(argv[1]) password:[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil]];
 __weak LRCallbackRaceClient *weak=client;
 client.race=@(argv[4]);NSLock *lock=[NSLock new];__block NSUInteger frames=0,statuses=0;__block BOOL valid=YES;
 client.statusHandler=^(NSString *s){if([weak.race isEqual:@"status"])weak.realInvoked=YES;[lock lock];statuses++;[lock unlock];};
 LRFrameCallback observe=^(NSData *pixels,uint16_t w,uint16_t h,uint64_t n,uint64_t bytes){if(![weak.race isEqual:@"status"])weak.realInvoked=YES;[lock lock];frames++;valid&=w==32&&h==16&&n==1&&pixels.length==2048;const uint8_t *p=pixels.bytes;for(NSUInteger i=0;i<pixels.length;i++)valid&=p[i]==0x6d;[lock unlock];};
 if([client.race isEqual:@"patch"])client.patchFrameHandler=^(NSData *p,uint16_t w,uint16_t h,uint64_t n,uint64_t b,LRFrameDamage d){observe(p,w,h,n,b);};
 else if([client.race isEqual:@"damage"])client.damageFrameHandler=^(NSData *p,uint16_t w,uint16_t h,uint64_t n,uint64_t b,LRFrameDamage d){observe(p,w,h,n,b);};
 else client.frameHandler=observe;
 [client start];double began=NSProcessInfo.processInfo.systemUptime;
 while(NSProcessInfo.processInfo.systemUptime-began<4){[lock lock];BOOL done=frames>0;[lock unlock];if(done||client.sentinelInvoked)break;[NSThread sleepForTimeInterval:.005];}
 [client stop];[NSThread sleepForTimeInterval:.02];[lock lock];
 NSDictionary *result=@{@"case":client.race,@"getter_clear_injected":@(client.injected),@"cleared_callback_reads":@(client.clearedReads),@"sentinel_invoked":@(client.sentinelInvoked),@"frames":@(frames),@"statuses":@(statuses),@"valid_pixels":@(valid&&frames==1)};
 [[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:nil]writeToFile:@(argv[3]) atomically:YES];[lock unlock];
 client.frameHandler=nil;client.damageFrameHandler=nil;client.patchFrameHandler=nil;client.statusHandler=nil;return client.injected?0:1;
}}
