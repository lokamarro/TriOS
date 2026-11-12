#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
// Passive probe for guest-driven sessions (e.g. vscode_session_driver): holds
// the RFB display connection, which keeps the session alive, without sending
// input. Returns when the guest creates $LR_PASSIVE_MARKER (relative to the
// rootfs data directory), when the display ends, or after $LR_PASSIVE_SECONDS
// (default 900). Saves the last frame as final.bgra with its geometry.
// Arguments as for the other probes: port password output-dir mode.
int main(int argc,char**argv){@autoreleasepool{
 if(argc!=5)return 2;
 NSString*out=@(argv[3]);
 NSString*marker=[[@(getenv("LR_GUI_ROOTFS")?:"") stringByAppendingPathComponent:@"data"] stringByAppendingPathComponent:@(getenv("LR_PASSIVE_MARKER")?:"")];
 double limit=getenv("LR_PASSIVE_SECONDS")?atof(getenv("LR_PASSIVE_SECONDS")):900;
 LRRFBClient*c=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil]];
 NSLock*lock=[NSLock new];__block NSData*last;__block uint16_t width=0,height=0;__block uint64_t frames=0;__block BOOL ended=NO;
 c.frameHandler=^(NSData*p,uint16_t w,uint16_t h,uint64_t n,uint64_t b){(void)b;[lock lock];last=p;width=w;height=h;frames=n;[lock unlock];};
 c.statusHandler=^(NSString*s){fprintf(stderr,"%s\n",s.UTF8String);if([s hasPrefix:@"Linux display ended"]||[s hasPrefix:@"Display error:"])dispatch_async(dispatch_get_main_queue(),^{ended=YES;});};
 [c start];NSDate*start=NSDate.date;BOOL found=NO;
 while(!ended&&-[start timeIntervalSinceNow]<limit){
  [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1]];
  if([NSFileManager.defaultManager fileExistsAtPath:marker]){found=YES;break;}
 }
 [lock lock];NSData*p=last;uint16_t w=width,h=height;uint64_t n=frames;[lock unlock];
 if(p)[p writeToFile:[out stringByAppendingPathComponent:@"final.bgra"] atomically:YES];
 NSString*json=[NSString stringWithFormat:@"{\"probe\":\"passive\",\"marker_found\":%d,\"display_ended\":%d,\"seconds\":%.1f,\"frames\":%llu,\"width\":%u,\"height\":%u}\n",found,ended,-[start timeIntervalSinceNow],(unsigned long long)n,w,h];
 [json writeToFile:[out stringByAppendingPathComponent:@"passive-probe.json"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
 fprintf(stderr,"PASSIVE_PROBE %s",json.UTF8String);
 return found?0:1;
}}
