#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
#include <unistd.h>
static void key(LRRFBClient*c,uint32_t k){[c key:k down:YES];[c key:k down:NO];}
static void click(LRRFBClient*c,int x,int y){[c pointerX:x y:y buttons:1];[c pointerX:x y:y buttons:0];}
static void type(LRRFBClient*c,const char*t){for(;*t;t++){key(c,*t=='\n'?0xff0d:(unsigned char)*t);usleep(10000);}}
static void navigate(LRRFBClient*c,const char*url){NSString*u=@(url);[c key:0xffe3 down:YES];key(c,'l');[c key:0xffe3 down:NO];dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),dispatch_get_main_queue(),^{type(c,u.UTF8String);key(c,0xff0d);});}
int main(int argc,char**argv){@autoreleasepool{
 if(argc!=5||!getenv("LR_GUI_APPLICATION_LOG"))return 2;
 NSString*dir=@(argv[3]),*logPath=@(getenv("LR_GUI_APPLICATION_LOG"));
 NSString*password=[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil];LRRFBClient*c=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:password];
 __block NSData*last=nil;__block BOOL ended=NO;__block uint64_t frames=0,bytes=0;c.statusHandler=^(NSString*s){fprintf(stderr,"%s\n",s.UTF8String);if([s hasPrefix:@"Linux display ended"]||[s hasPrefix:@"Display error:"])ended=YES;};
 c.frameHandler=^(NSData*d,uint16_t w,uint16_t h,uint64_t n,uint64_t b){if(w==640&&h==480){last=d;frames=n;bytes=b;[d writeToFile:[dir stringByAppendingPathComponent:@"latest.bgra"] atomically:YES];}};
 [c start];NSDate*start=NSDate.date,*step=start;int stage=0;NSString*log=@"";BOOL previousStorage=NO;
 while(!ended&&-[start timeIntervalSinceNow]<350){[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];if(!last)continue;log=[NSString stringWithContentsOfFile:logPath encoding:NSUTF8StringEncoding error:nil]?:@"";
  previousStorage=[log containsString:@"LR_PREVIOUS_STORAGE storage-ok linuxrunner=cookie-ok"];BOOL next=NO;const uint8_t*p=last.bytes;size_t i=(330*640+40)*4;
  if(stage==0&&[log containsString:@"LR_JS_PASS"]&&p[i]==0x9d&&p[i+1]==0xf5&&p[i+2]==0xff){click(c,100,275);type(c,"Browser iOS27 AaZz09 !?");next=YES;}
  else if(stage==1&&[log containsString:@"LR_INPUT Browser iOS27 AaZz09 !?"]){click(c,300,275);next=YES;}
  else if(stage==2&&[log containsString:@"LR_CLICK clicked 1 Browser iOS27 AaZz09 !?"]){[last writeToFile:[dir stringByAppendingPathComponent:@"form.bgra"] atomically:YES];for(int n=0;n<16;n++){[c pointerX:450 y:400 buttons:16];[c pointerX:450 y:400 buttons:0];}next=YES;}
  else if(stage==3&&[log containsString:@"LR_SCROLL_PASS"]&&-[step timeIntervalSinceNow]>2){[last writeToFile:[dir stringByAppendingPathComponent:@"scroll.bgra"] atomically:YES];for(int n=0;n<16;n++){[c pointerX:450 y:400 buttons:8];[c pointerX:450 y:400 buttons:0];}next=YES;}
  else if(stage==4&&-[step timeIntervalSinceNow]>3){click(c,300,385);next=YES;}
  else if(stage==5&&[log containsString:@"loadFinished ok=1 url=http://127.0.0.1:18766/browser-test.html?navigation=1"]&&[log containsString:@"JavaScript result="]){navigate(c,"https://example.com/");next=YES;}
  else if(stage==6&&[log containsString:@"loadFinished ok=1 url=https://example.com/"]&&[log containsString:@"\"title\":\"Example Domain\""]){[last writeToFile:[dir stringByAppendingPathComponent:@"https.bgra"] atomically:YES];click(c,45,25);next=YES;}
  else if(stage==7&&-[step timeIntervalSinceNow]>4&&[log containsString:@"LR_PREVIOUS_STORAGE storage-ok"]&&p[i]==0x9d&&p[i+1]==0xf5&&p[i+2]==0xff){click(c,540,25);next=YES;}
  else if(stage==8&&[log containsString:@"loadFinished ok=1 url=about:blank"]){navigate(c,"http://127.0.0.1:18766/download.txt");next=YES;}
  else if(stage==9&&[log containsString:@"download finished state=2 bytes=40"]){[c key:0xffe3 down:YES];key(c,'q');[c key:0xffe3 down:NO];next=YES;}
  if(next){fprintf(stderr,"CHROMIUM_RESTART: stage=%d PASS\n",stage++);step=NSDate.date;}
 }
 log=[NSString stringWithContentsOfFile:logPath encoding:NSUTF8StringEncoding error:nil]?:@"";BOOL pass=stage==10&&ended&&[log containsString:@"BROWSER: exit=0"]&&previousStorage;
 [last writeToFile:[dir stringByAppendingPathComponent:@"final.bgra"] atomically:YES];[c stop];NSDictionary*r=@{@"result":pass?@"PASS":@"FAIL",@"stages":@(stage),@"previous_storage_and_cookie":@(previousStorage),@"frames":@(frames),@"wire_bytes":@(bytes),@"ended":@(ended),@"seconds":@(-[start timeIntervalSinceNow])};[[NSJSONSerialization dataWithJSONObject:r options:NSJSONWritingPrettyPrinted error:nil]writeToFile:[dir stringByAppendingPathComponent:@"restart.json"] atomically:YES];return pass?0:1;
}}
