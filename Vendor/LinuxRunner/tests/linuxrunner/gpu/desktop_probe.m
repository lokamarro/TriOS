#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import "app/LinuxRunner/RFBClient.h"
#include <unistd.h>
int main(int argc,char **argv){@autoreleasepool{
 if(argc!=5)return 2;NSString *output=@(argv[3]);
 NSDictionary *session=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:@"build/qt-tests/current-session.json"] options:0 error:nil];
 NSString *requests=[session[@"path"] stringByAppendingPathComponent:@"requests"];
 NSString *password=[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil];
 LRRFBClient *client=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:password];
 __block NSData *last=nil;__block BOOL ended=NO;__block uint16_t width=0,height=0;
 client.statusHandler=^(NSString *s){fprintf(stderr,"%s\n",s.UTF8String);if([s hasPrefix:@"Display error:"]||[s hasPrefix:@"Linux display ended"])ended=YES;};
 client.frameHandler=^(NSData *data,uint16_t w,uint16_t h,uint64_t f,uint64_t bytes){last=data;width=w;height=h;};
 [client start];NSDate *start=NSDate.date;BOOL submitted=NO,rendered=NO,clicked=NO;NSDate *ready=nil;unsigned clickX=0,clickY=0;
 while(!ended&&-[start timeIntervalSinceNow]<100){
  [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];
  if(!submitted&&last&&-[start timeIntervalSinceNow]>7){
   NSString *command=@"#!/bin/sh\nexport LD_LIBRARY_PATH=/home/mobile/Downloads/.linuxrunner/gui\nexec /bin/sh /home/mobile/Downloads/.linuxrunner/gui/gpu-demo.sh\n";
   [NSFileManager.defaultManager createDirectoryAtPath:requests withIntermediateDirectories:YES attributes:nil error:nil];
   [command writeToFile:[requests stringByAppendingPathComponent:@"launch-gpu.sh"] atomically:YES encoding:NSUTF8StringEncoding error:nil];submitted=YES;
  }
  if(!last)continue;
  if(!rendered){unsigned colorful=0;uint64_t sumX=0,sumY=0;const uint8_t *p=last.bytes;
   for(NSUInteger i=0;i+3<last.length;i+=4){unsigned b=p[i],g=p[i+1],r=p[i+2];if(r>40&&b>60&&g>30&&b>g+20&&r>g+20){colorful++;sumX+=(i/4)%width;sumY+=(i/4)/width;}}
   if(colorful>2500){rendered=YES;ready=NSDate.date;clickX=sumX/colorful;clickY=sumY/colorful;
    [last writeToFile:[output stringByAppendingPathComponent:@"linux-gpu-desktop.bgra"] atomically:YES];
    CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();CGDataProviderRef provider=CGDataProviderCreateWithCFData((__bridge CFDataRef)last);
    CGImageRef image=CGImageCreate(width,height,8,32,width*4,space,kCGBitmapByteOrder32Little|kCGImageAlphaNoneSkipFirst,provider,NULL,NO,kCGRenderingIntentDefault);
    CGImageDestinationRef destination=CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:[output stringByAppendingPathComponent:@"macos-host-linux-metal-desktop.png"]],CFSTR("public.png"),1,NULL);
    CGImageDestinationAddImage(destination,image,NULL);CGImageDestinationFinalize(destination);CFRelease(destination);CGImageRelease(image);CGDataProviderRelease(provider);CGColorSpaceRelease(space);
    fprintf(stderr,"GPU_DESKTOP_VISIBLE_PASS colored_pixels=%u resolution=%ux%u\n",colorful,width,height);
   }
  } else if(!clicked&&-[ready timeIntervalSinceNow]>2){
   // The mapped GPU demo has its own X11 window, activated by a normal click.
   for(unsigned i=0;i<20;i++){
    [client pointerX:(i%2?clickX:10) y:(i%2?clickY:10) buttons:0];
   }
   [client pointerX:clickX y:clickY buttons:1];[client pointerX:clickX y:clickY buttons:0];clicked=YES;
  }else if(clicked&&-[ready timeIntervalSinceNow]>5){
   NSString *liveLog=[NSString stringWithContentsOfFile:[session[@"path"] stringByAppendingPathComponent:@"application.log"] encoding:NSUTF8StringEncoding error:nil];
   if([liveLog containsString:@"GPU_BRIDGE_CLEANUP_PASS"]){fprintf(stderr,"POINTER_REGRESSION unexpected guest exit\n");break;}
   [client key:0xff1b down:YES];[client key:0xff1b down:NO];
   NSDate *wait=NSDate.date;while(-[wait timeIntervalSinceNow]<3)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];break;
  }
 }
 NSString *applicationLog=[NSString stringWithContentsOfFile:[session[@"path"] stringByAppendingPathComponent:@"application.log"] encoding:NSUTF8StringEncoding error:nil];
 BOOL guestPixels=[applicationLog containsString:@"GPU_BRIDGE_PIXEL_PASS"],cleanup=[applicationLog containsString:@"GPU_BRIDGE_CLEANUP_PASS"];
 [applicationLog writeToFile:[output stringByAppendingPathComponent:@"guest-gpu.log"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
 [client stop];NSDictionary *report=@{@"environment":@"macOS host ARM64 interpreter + Apple Metal; not iPhone",@"linux_gpu_window_visible":@(rendered),@"pointer_delivered":@(clicked),@"guest_pixel_checks":@(guestPixels),@"clean_exit_after_escape":@(cleanup),@"pointer_motion_and_click_survived":@([applicationLog containsString:@"GPU_EXIT reason=escape"]),@"seconds":@(-[start timeIntervalSinceNow]),@"width":@(width),@"height":@(height)};
 [[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[output stringByAppendingPathComponent:@"gpu-desktop.json"] atomically:YES];return rendered&&clicked&&guestPixels&&cleanup?0:1;
}}
