#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import "app/LinuxRunner/RFBClient.h"
int main(int argc,char **argv){@autoreleasepool{
 if(argc!=5)return 2;NSString *out=@(argv[3]);LRRFBClient *c=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil]];
 __block NSData *last=nil;__block unsigned w=0,h=0;__block BOOL ended=NO;c.frameHandler=^(NSData *d,uint16_t x,uint16_t y,uint64_t f,uint64_t b){last=d;w=x;h=y;};c.statusHandler=^(NSString *s){fprintf(stderr,"%s\n",s.UTF8String);if([s containsString:@"Display error:"])ended=YES;};[c start];NSDate *start=NSDate.date;
 BOOL interacted=NO;
 while(!ended&&-[start timeIntervalSinceNow]<35){[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];
 if(last&&!interacted&&-[start timeIntervalSinceNow]>20){interacted=YES;for(int i=0;i<30;i++){[c pointerX:450 y:350 buttons:0];[c pointerX:100 y:100 buttons:0];}[c pointerX:100 y:100 buttons:1];[c pointerX:100 y:100 buttons:0];[c pointerX:100 y:100 buttons:4];[c pointerX:100 y:100 buttons:0];}}

 if(last){CGColorSpaceRef sp=CGColorSpaceCreateDeviceRGB();CGDataProviderRef p=CGDataProviderCreateWithCFData((__bridge CFDataRef)last);CGImageRef img=CGImageCreate(w,h,8,32,w*4,sp,kCGBitmapByteOrder32Little|kCGImageAlphaNoneSkipFirst,p,NULL,NO,kCGRenderingIntentDefault);CGImageDestinationRef dst=CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:[out stringByAppendingPathComponent:@"macos-host-linux-x11.png"]],CFSTR("public.png"),1,NULL);CGImageDestinationAddImage(dst,img,NULL);CGImageDestinationFinalize(dst);CFRelease(dst);CGImageRelease(img);CGDataProviderRelease(p);CGColorSpaceRelease(sp);}
 [c pointerX:120 y:120 buttons:1];[c pointerX:120 y:120 buttons:0];[c key:0xff1b down:YES];[c key:0xff1b down:NO];NSDate *wait=NSDate.date;while(-[wait timeIntervalSinceNow]<3)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];[c stop];return last?0:1;
}}
