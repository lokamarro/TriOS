#import <Foundation/Foundation.h>
#import <Vision/Vision.h>
#include <unistd.h>
#import "app/LinuxRunner/RFBClient.h"
static void key(LRRFBClient*c,uint32_t k){[c key:k down:YES];[c key:k down:NO];}
static void type(LRRFBClient*c,const char*t){for(;*t;t++){key(c,*t=='\n'?0xff0d:(unsigned char)*t);usleep(10000);}}
static void click(LRRFBClient*c,int x,int y){[c pointerX:x y:y buttons:1];[c pointerX:x y:y buttons:0];}
static void navigate(LRRFBClient*c,const char*url){
 // Focus changes travel through Gecko's event loop before text can be committed.
 // Use realistic pauses rather than sending the entire navigation in one burst.
 NSString *target=@(url);
 click(c,230,65);
 dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),dispatch_get_main_queue(),^{
  [c key:0xffe3 down:YES];key(c,'a');[c key:0xffe3 down:NO];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),dispatch_get_main_queue(),^{type(c,target.UTF8String);key(c,0xff0d);});
 });
}

static NSString *ocr(NSData*d){
 CGColorSpaceRef cs=CGColorSpaceCreateDeviceRGB();CGDataProviderRef provider=CGDataProviderCreateWithCFData((__bridge CFDataRef)d);
 CGImageRef im=CGImageCreate(640,480,8,32,2560,cs,kCGBitmapByteOrder32Little|kCGImageAlphaNoneSkipFirst,provider,NULL,NO,kCGRenderingIntentDefault);
 VNRecognizeTextRequest*r=[VNRecognizeTextRequest new];r.recognitionLevel=VNRequestTextRecognitionLevelAccurate;r.usesLanguageCorrection=NO;
 [[[VNImageRequestHandler alloc]initWithCGImage:im options:@{}] performRequests:@[r] error:nil];NSMutableArray*a=[NSMutableArray new];
 for(VNRecognizedTextObservation*o in r.results)[a addObject:[o topCandidates:1].firstObject.string?:@""];
 CGImageRelease(im);CGDataProviderRelease(provider);CGColorSpaceRelease(cs);return [a componentsJoinedByString:@" "];
}
int main(int argc,char**argv){@autoreleasepool{
 if(argc!=5)return 2;NSString*dir=@(argv[3]),*password=[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil];
 LRRFBClient*c=[[LRRFBClient alloc]initWithPort:atoi(argv[1]) password:password];__block NSData*last=nil;__block BOOL ended=NO;__block uint64_t frames=0,bytes=0;
 c.statusHandler=^(NSString*s){fprintf(stderr,"%s\n",s.UTF8String);if([s hasPrefix:@"Linux display ended"]||[s hasPrefix:@"Display error:"])ended=YES;};
 c.frameHandler=^(NSData*d,uint16_t w,uint16_t h,uint64_t n,uint64_t b){if(w==640&&h==480){last=d;frames=n;bytes=b;[d writeToFile:[dir stringByAppendingPathComponent:@"latest.bgra"] atomically:YES];}};
 [c start];NSDate*start=NSDate.date,*stepStart=start,*scan=start;int stage=0;BOOL sentCount=NO,siteScrolled=NO;NSDate *localReady=nil,*siteReady=nil;NSString *siteFirstText=nil;NSMutableDictionary*report=[NSMutableDictionary new];NSString*text=@"";
 while(!ended&&-[start timeIntervalSinceNow]<650&&stage<5){
  [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];
  if(!last||-[scan timeIntervalSinceNow]<1)continue;scan=NSDate.date;text=ocr(last);[text writeToFile:[dir stringByAppendingPathComponent:@"latest-ocr.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
  if([text containsString:@"Firefox automatically sends"]||[text containsString:@"Choose What I Share"]){
   // Dismiss the real first-run notice through ordinary pointer input. Do not
   // alter privacy/security prefs. The notice shifts the content coordinates.
   click(c,552,108);localReady=nil;continue;
  }
  if(stage==1&&!sentCount&&[text containsString:@"AaZz09"]){click(c,310,294);sentCount=YES;continue;}
  BOOL advance=NO;
  if(stage==0&&[text containsString:@"REAL LINUX ARM64"]&&[text containsString:@"storage-ok"]){
   if(!localReady){localReady=NSDate.date;continue;}if(-[localReady timeIntervalSinceNow]<5)continue;
   [last writeToFile:[dir stringByAppendingPathComponent:@"local.bgra"] atomically:YES];click(c,100,294);
   dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),dispatch_get_main_queue(),^{type(c,"Firefox iOS27 AaZz09 !?");});advance=YES;
  }else if(stage==1&&[text containsString:@"clicked 1"]&&[text containsString:@"AaZz09"]){
   [last writeToFile:[dir stringByAppendingPathComponent:@"form.bgra"] atomically:YES];for(int i=0;i<20;i++){[c pointerX:450 y:400 buttons:16];[c pointerX:450 y:400 buttons:0];}advance=YES;
  }else if(stage==2&&[text containsString:@"SCROLL BOTTOM"]){
   [last writeToFile:[dir stringByAppendingPathComponent:@"scroll.bgra"] atomically:YES];navigate(c,"https://etsbi.edu.ba/");advance=YES;
  }else if(stage==3&&([text.uppercaseString containsString:@"ELEKTROTEHNI"]||[text.uppercaseString containsString:@"SMJEROVI KOJE NUDIMO"]||[text.uppercaseString containsString:@"JU MJESOVITA"])){
   if(!siteReady){siteReady=NSDate.date;siteFirstText=text;[last writeToFile:[dir stringByAppendingPathComponent:@"etsbi.bgra"] atomically:YES];report[@"etsbi"]=@{@"URL":@"https://etsbi.edu.ba/",@"first_paint":@"PASS: real Gecko pixels",@"document_load":@"PASS: main HTML school content visible; subresource completion not measured",@"DNS_TCP_TLS_HTTP":@"PASS inferred from normal HTTPS content; certificates not disabled",@"ocr":text,@"elapsed_seconds":@(-[stepStart timeIntervalSinceNow])};continue;}
   if(!siteScrolled&&-[siteReady timeIntervalSinceNow]>=20){for(int i=0;i<12;i++){[c pointerX:450 y:400 buttons:16];[c pointerX:450 y:400 buttons:0];}siteScrolled=YES;siteReady=NSDate.date;continue;}
  }
  if(stage==3&&siteScrolled&&-[siteReady timeIntervalSinceNow]>=5&&![text isEqualToString:siteFirstText]&&([text containsString:@"Učenika"]||[text containsString:@"Zaposlenika"]||[text containsString:@"Razreda"]||[text.uppercaseString containsString:@"SMJEROVI"])){
   [last writeToFile:[dir stringByAppendingPathComponent:@"etsbi-scroll.bgra"] atomically:YES];NSMutableDictionary*site=[report[@"etsbi"] mutableCopy];site[@"page_interactive"]=@"PASS: real wheel input changed document viewport";site[@"scrolled_ocr"]=text;report[@"etsbi"]=site;navigate(c,"https://example.com/");advance=YES;
  }else if(stage==4&&[text containsString:@"Example Domain"]&&[text containsString:@"This domain is for use"]){
   [last writeToFile:[dir stringByAppendingPathComponent:@"example.bgra"] atomically:YES];report[@"example"]=@{@"URL":@"https://example.com/",@"document_load":@"PASS",@"first_paint":@"PASS",@"ocr":text,@"elapsed_seconds":@(-[stepStart timeIntervalSinceNow])};advance=YES;
  }
  if(advance){fprintf(stderr,"FIREFOX_WEB: stage=%d PASS %s\n",stage,text.UTF8String);stage++;stepStart=NSDate.date;}
 }
 report[@"physical_device"]=@"NOT TESTED";report[@"stage_count"]=@(stage);report[@"result"]=stage==5?@"PASS":@"FAIL";report[@"frames"]=@(frames);report[@"wire_bytes"]=@(bytes);report[@"seconds"]=@(-[start timeIntervalSinceNow]);report[@"last_ocr"]=text;
 [[NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil] writeToFile:[dir stringByAppendingPathComponent:@"firefox-web.json"] atomically:YES];[c stop];return stage==5?0:1;
}}
