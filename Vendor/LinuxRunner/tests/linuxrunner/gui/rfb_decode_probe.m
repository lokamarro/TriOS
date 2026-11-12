#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
#import "app/LinuxRunner/FramebufferPatch.h"
#import <CommonCrypto/CommonDigest.h>
static NSString *SHA(NSData *data){unsigned char hash[CC_SHA256_DIGEST_LENGTH];CC_SHA256(data.bytes,(CC_LONG)data.length,hash);NSMutableString *s=[NSMutableString new];for(unsigned i=0;i<sizeof hash;i++)[s appendFormat:@"%02x",hash[i]];return s;}
int main(int argc,char **argv){@autoreleasepool{
 if(argc!=5)return 2;
 BOOL patches=strcmp(argv[4],"patch")==0;
 LRRFBClient *client=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil]];
 __weak LRRFBClient *weak=client;NSLock *lock=[NSLock new];NSMutableArray *frames=[NSMutableArray new],*snapshots=[NSMutableArray new];
 __block BOOL ended=NO,failed=NO;__block NSUInteger rawBytes=0;
 void (^observe)(NSData *,uint16_t,uint16_t,uint64_t)=^(NSData *pixels,uint16_t w,uint16_t h,uint64_t count){
  [lock lock];rawBytes=MAX(rawBytes,[[weak valueForKey:@"raw"]length]);
  [frames addObject:@{@"width":@(w),@"height":@(h),@"frame":@(count),@"sha256":SHA(pixels)}];[snapshots addObject:[pixels copy]];[lock unlock];
 };
 client.statusHandler=^(NSString *status){[lock lock];if([status hasPrefix:@"Display error:"])failed=YES;if([status hasPrefix:@"Linux display ended"]||failed)ended=YES;[lock unlock];};
 if(patches){
  __block NSMutableData *assembled=nil;__block uint16_t previousW=0,previousH=0;
  client.patchFrameHandler=^(NSData *patch,uint16_t w,uint16_t h,uint64_t count,uint64_t wire,LRFrameDamage damage){
   if(!assembled||w!=previousW||h!=previousH){assembled=[NSMutableData dataWithLength:(size_t)w*h*4];previousW=w;previousH=h;}
   if(!LRApplyFramebufferPatch(assembled,patch,w,h,damage)){[lock lock];failed=YES;[lock unlock];return;}
   observe(assembled,w,h,count);
  };
 }else client.frameHandler=^(NSData *pixels,uint16_t w,uint16_t h,uint64_t count,uint64_t wire){observe(pixels,w,h,count);};
 [client start];double start=NSProcessInfo.processInfo.systemUptime;
 while(NSProcessInfo.processInfo.systemUptime-start<6){[lock lock];BOOL done=ended;[lock unlock];if(done)break;[NSThread sleepForTimeInterval:.005];}
 [client stop];[lock lock];NSMutableArray *retainedHashes=[NSMutableArray new];for(NSData *data in snapshots)[retainedHashes addObject:SHA(data)];
 NSDictionary *result=@{@"frames":[frames copy],@"retained_sha256":[retainedHashes copy],@"peak_intermediate_raw_bytes":@(rawBytes),@"ended":@(ended),@"failed":@(failed),@"delivery":patches?@"patch":@"snapshot"};
 [[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:nil]writeToFile:@(argv[3]) atomically:YES];[lock unlock];return ended&&!failed&&frames.count==5?0:1;
}}
