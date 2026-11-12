// Host fixture only. Stops at authentication UI; never enters credentials.
#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
int main(int argc, char **argv) { @autoreleasepool {
 if(argc!=5)return 2;
 NSString *dir=@(argv[3]);
 LRRFBClient *c=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil]];
 __block NSData *pixels=nil; __block uint16_t w=0,h=0; __block uint64_t frames=0,bytes=0; __block BOOL failed=NO;
 c.statusHandler=^(NSString *s){fprintf(stderr,"%s\n",s.UTF8String);dispatch_async(dispatch_get_main_queue(),^{if([s hasPrefix:@"Display error:"])failed=YES;});};
 c.frameHandler=^(NSData *d,uint16_t width,uint16_t height,uint64_t f,uint64_t b){dispatch_async(dispatch_get_main_queue(),^{pixels=d;w=width;h=height;frames=f;bytes=b;});};
 [c start]; NSDate *begin=NSDate.date,*ready=nil,*clicked=nil; BOOL welcome=NO,phoneClicked=NO,keyboardTest=getenv("LR_TELEGRAM_TEST_KEYBOARD")!=NULL; int inputStage=0; NSDate *inputTime=nil;
 while(!failed&&-[begin timeIntervalSinceNow]<240){
  [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];
  const uint8_t *p=pixels.bytes;
  BOOL blue=w>=640&&h>=480&&pixels.length>=(NSUInteger)w*h*4;
  if(blue){NSUInteger i=((NSUInteger)410*w+180)*4;blue=p[i]>160&&p[i+1]>100&&p[i+2]<120;}
  if(!clicked&&blue){if(!ready)ready=NSDate.date;if(-[ready timeIntervalSinceNow]>8){
   [pixels writeToFile:[dir stringByAppendingPathComponent:@"welcome.bgra"] atomically:YES];welcome=YES;
   fprintf(stderr,"TELEGRAM_PROBE: clicking Start Messaging only at %.2fs\n",-[begin timeIntervalSinceNow]);
   [c pointerX:320 y:416 buttons:1];
   dispatch_after(dispatch_time(DISPATCH_TIME_NOW,100*NSEC_PER_MSEC),dispatch_get_main_queue(),^{[c pointerX:320 y:416 buttons:0];});clicked=NSDate.date;
  }}
  if(clicked&&-[clicked timeIntervalSinceNow]>30&&!phoneClicked){
   // Do not retain a QR-login token in test screenshots or exports.
   // Position confirmed from the prior real QR login screen. This changes
   // login method only; the phone field is left empty and Next is never used.
   [c pointerX:320 y:470 buttons:1];
   dispatch_after(dispatch_time(DISPATCH_TIME_NOW,100*NSEC_PER_MSEC),dispatch_get_main_queue(),^{[c pointerX:320 y:470 buttons:0];});
   phoneClicked=YES;
  }
  if(clicked&&-[clicked timeIntervalSinceNow]>65){
  if(!keyboardTest)break;
  if(inputStage==0){
   [pixels writeToFile:[dir stringByAppendingPathComponent:@"phone-entry.bgra"] atomically:YES];
   [c pointerX:320 y:232 buttons:1];[c pointerX:320 y:232 buttons:0];inputStage=1;inputTime=NSDate.date;
  }else if(inputStage==1&&-[inputTime timeIntervalSinceNow]>3){
   const char *text="Bosnia";for(;*text;text++){[c key:*text down:YES];[c key:*text down:NO];}
   inputStage=2;inputTime=NSDate.date;
  }else if(inputStage==2&&-[inputTime timeIntervalSinceNow]>4){
   [pixels writeToFile:[dir stringByAppendingPathComponent:@"country-search.bgra"] atomically:YES];
   [c key:0xff1b down:YES];[c key:0xff1b down:NO];inputStage=3;inputTime=NSDate.date;
  }else if(inputStage==3&&-[inputTime timeIntervalSinceNow]>2){
   [c key:0xffe3 down:YES];[c key:'q' down:YES];[c key:'q' down:NO];[c key:0xffe3 down:NO];inputStage=4;inputTime=NSDate.date;
  }else if(inputStage==4&&-[inputTime timeIntervalSinceNow]>3)break;
 }
 }
 if(!keyboardTest)[pixels writeToFile:[dir stringByAppendingPathComponent:@"phone-entry.bgra"] atomically:YES];
 NSDictionary *report=@{@"environment":@"macOS host interpreter",@"welcome_observed":@(welcome),@"start_clicked":@(clicked!=nil),@"keyboard_sequence_sent":@(inputStage==4),@"width":@(w),@"height":@(h),@"frames":@(frames),@"wire_bytes":@(bytes),@"seconds":@(-[begin timeIntervalSinceNow]),@"authentication":@"not attempted; inspect captured screen"};
 [[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[dir stringByAppendingPathComponent:@"snapshot.json"] atomically:YES];[c stop];return welcome&&!failed?0:1;
}}
