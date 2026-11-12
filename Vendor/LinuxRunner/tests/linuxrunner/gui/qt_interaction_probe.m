#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
#include <unistd.h>
static void key(LRRFBClient*c,uint32_t k){[c key:k down:YES];[c key:k down:NO];}
static void type(LRRFBClient*c,const char*t){size_t n=strlen(t);uint32_t syms[n];for(size_t i=0;i<n;i++)syms[i]=t[i]=='\n'?0xff0d:(unsigned char)t[i];[c typeKeysyms:syms count:n];}
static void click(LRRFBClient*c,int x,int y){[c pointerX:x y:y buttons:1];[c pointerX:x y:y buttons:0];}
static BOOL grey(NSData*d,int x,int y){const uint8_t*p=d.bytes;size_t i=(y*640+x)*4;return p[i]>230&&p[i]<245&&p[i+1]>230&&p[i+1]<245&&p[i+2]>230&&p[i+2]<245;}
static BOOL icon(NSData*d){const uint8_t*p=d.bytes;size_t i=(185*640+200)*4;return p[i]>150&&p[i+1]>100&&p[i+2]<100;}
int main(int argc,char**argv){@autoreleasepool{
 if(argc!=5||!getenv("LR_GUI_APPLICATION_LOG"))return 2;
 NSString*dir=@(argv[3]),*logPath=@(getenv("LR_GUI_APPLICATION_LOG"));
 NSString*password=[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil];
 const char*ascii="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 !@#$%^&*()-_=+[]{};:\"',.<>/?|`~";
 NSMutableString*hex=[NSMutableString new];for(const unsigned char*p=(const void*)ascii;*p;p++)[hex appendFormat:@"%02x",*p];
 NSString*typed=[@"QT_TEST: text_utf8_hex=" stringByAppendingString:hex];
 LRRFBClient*c=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:password];__block NSData*last=nil;__block BOOL ended=NO;__block uint64_t frames=0,bytes=0;
 c.statusHandler=^(NSString*s){dispatch_async(dispatch_get_main_queue(),^{fprintf(stderr,"%s\n",s.UTF8String);if([s hasPrefix:@"Linux display ended"]||[s hasPrefix:@"Display error:"])ended=YES;});};
 c.frameHandler=^(NSData*d,uint16_t w,uint16_t h,uint64_t n,uint64_t b){dispatch_async(dispatch_get_main_queue(),^{if(w==640&&h==480){last=d;frames=n;bytes=b;}});};
 [c start];NSDate*start=NSDate.date,*step=start;int stage=0;
 while(!ended&&-[start timeIntervalSinceNow]<150){
  [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];if(!last)continue;
  NSString*log=[NSString stringWithContentsOfFile:logPath encoding:NSUTF8StringEncoding error:nil]?:@"";BOOL next=NO;
  if(stage==0&&grey(last,300,300)&&[log containsString:@"QT_TEST: resize=620x460"]){click(c,100,102);type(c,ascii);next=YES;}
  else if(stage==1&&[log containsString:typed]){[last writeToFile:[dir stringByAppendingPathComponent:@"typed.bgra"] atomically:YES];click(c,65,208);click(c,26,232);click(c,500,253);next=YES;}
  else if(stage==2&&[log containsString:@"QT_TEST: clicks=1"]&&[log containsString:@"QT_TEST: checkbox=1"]&&[log containsString:@"QT_TEST: slider="]){click(c,166,208);next=YES;}
  else if(stage==3&&icon(last)&&[log containsString:@"QT_TEST: modal_open"]){[last writeToFile:[dir stringByAppendingPathComponent:@"modal.bgra"] atomically:YES];click(c,408,227);next=YES;}
  else if(stage==4&&!icon(last)&&[log containsString:@"QT_TEST: modal_closed"]){click(c,270,208);next=YES;}
  else if(stage==5&&grey(last,300,180)&&[log containsString:@"QT_TEST: child_open"]&&-[step timeIntervalSinceNow]>2){[last writeToFile:[dir stringByAppendingPathComponent:@"child.bgra"] atomically:YES];click(c,300,285);next=YES;}
  else if(stage==6&&!grey(last,300,180)&&-[step timeIntervalSinceNow]>2){click(c,375,208);click(c,475,208);next=YES;}
  else if(stage==7&&[log containsString:@"QT_TEST: guest_clipboard_copy"]&&[log containsString:@"QT_TEST: guest_clipboard_paste"]){click(c,180,350);[c pointerX:180 y:350 buttons:16];[c pointerX:180 y:350 buttons:0];[c pointerX:180 y:350 buttons:4];[c pointerX:180 y:350 buttons:0];next=YES;}
  else if(stage==8&&-[step timeIntervalSinceNow]>3){[last writeToFile:[dir stringByAppendingPathComponent:@"context.bgra"] atomically:YES];key(c,0xff1b);next=YES;}
  else if(stage==9&&[log containsString:@"QT_TEST: context_closed"]){click(c,100,102);key(c,0xff09);type(c,"Multiline\nsecond line");key(c,0xff08);key(c,0xff51);next=YES;}
  else if(stage==10&&-[step timeIntervalSinceNow]>3){[last writeToFile:[dir stringByAppendingPathComponent:@"interactive.bgra"] atomically:YES];click(c,20,20);next=YES;}
  else if(stage==11&&-[step timeIntervalSinceNow]>2){[last writeToFile:[dir stringByAppendingPathComponent:@"menu.bgra"] atomically:YES];click(c,75,60);next=YES;}
  else if(stage==12&&[log containsString:@"QT_TEST: resize=518x380"]){[last writeToFile:[dir stringByAppendingPathComponent:@"resized.bgra"] atomically:YES];click(c,20,20);next=YES;}
  else if(stage==13&&-[step timeIntervalSinceNow]>2){click(c,75,82);next=YES;}
  if(next){fprintf(stderr,"QT_INTERACTION: stage=%d PASS\n",stage++);step=NSDate.date;}
 }
 [c stop];NSDictionary*r=@{@"frames":@(frames),@"wire_bytes":@(bytes),@"display_ended":@(ended),@"stages":@(stage),@"seconds":@(-[start timeIntervalSinceNow])};[[NSJSONSerialization dataWithJSONObject:r options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[dir stringByAppendingPathComponent:@"probe.json"] atomically:YES];return stage==14&&ended?0:1;
}}
