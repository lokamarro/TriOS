#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import "app/LinuxRunner/RFBClient.h"
int main(int argc,char **argv){@autoreleasepool{
 if(argc!=5)return 2;NSString *out=@(argv[3]);LRRFBClient *c=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil]];
 __block NSData *last=nil;__block unsigned w=0,h=0;__block BOOL ended=NO;c.frameHandler=^(NSData *d,uint16_t x,uint16_t y,uint64_t f,uint64_t b){last=d;w=x;h=y;};c.statusHandler=^(NSString *s){fprintf(stderr,"%s\n",s.UTF8String);if([s containsString:@"Display error:"])ended=YES;};[c start];NSDate *start=NSDate.date;
 BOOL interacted=NO;
 while(!ended&&-[start timeIntervalSinceNow]<35){[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];
 if(last&&!interacted&&-[start timeIntervalSinceNow]>20){interacted=YES;for(int i=0;i<30;i++){[c pointerX:450 y:350 buttons:0];[c pointerX:100 y:100 buttons:0];}[c pointerX:100 y:100 buttons:1];[c pointerX:100 y:100 buttons:0];[c pointerX:100 y:100 buttons:4];[c pointerX:100 y:100 buttons:0];}}

 if(last){CGColorSpaceRef sp=CGColorSpaceCreateDeviceRGB();CGDataProviderRef p=CGDataProviderCreateWithCFData((__bridge CFDataRef)last);CGImageRef img=CGImageCreate(w,h,8,32,w*4,sp,kCGBitmapByteOrder32Little|kCGImageAlphaNoneSkipFirst,p,NULL,NO,kCGRenderingIntentDefault);CGImageDestinationRef dst=CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:[out stringByAppendingPathComponent:@"macos-host-glx-metal.png"]],CFSTR("public.png"),1,NULL);CGImageDestinationAddImage(dst,img,NULL);CGImageDestinationFinalize(dst);CFRelease(dst);CGImageRelease(img);CGDataProviderRelease(p);CGColorSpaceRelease(sp);}
 NSData *captured=last;
 [c pointerX:120 y:120 buttons:1];[c pointerX:120 y:120 buttons:0];[c key:0xff1b down:YES];[c key:0xff1b down:NO];NSDate *wait=NSDate.date;while(-[wait timeIntervalSinceNow]<3)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];[c stop];unsigned green=0;const unsigned char *pixels=captured.bytes;for(NSUInteger i=0;i+3<captured.length;i+=4)if(pixels[i+1]>200&&pixels[i]<20&&pixels[i+2]<20)green++;
 NSString *log=[NSString stringWithContentsOfFile:@(getenv("LR_GUI_APPLICATION_LOG")) encoding:NSUTF8StringEncoding error:nil];BOOL contract=[log containsString:@"GLX_METAL_CONTRACT_PASS"];BOOL hardware=[log containsString:@"Metal_submissions="];
 NSDictionary *result=@{@"environment":@"Mac ARM64 interpreter + Metal; not iPhone",@"contract":@(contract),@"hardware_render_counters":@(hardware),@"green_pixels":@(green),@"pointer_enter_leave_clicks":@(interacted),@"display_transport_error":@(ended)};
 [[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[out stringByAppendingPathComponent:@"glx-metal-result.json"] atomically:YES];return last&&contract&&hardware&&green>1000&&!ended?0:1;
}}
