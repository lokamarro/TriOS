// RFB typing integrity: types a known text through the native RFB client into
// a guest `cat > ~/typed.txt` (xterm), then ends input with Ctrl-D. The guest
// script compares the file; this probe only drives input and waits for the
// display to end. Usage: rfb-typing-probe PORT PASSWORD_FILE OUTPUT_DIR ...
#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static NSString *typedText(void){
    // Shaped like the browser gate's typed URLs, plus capitals for Shift.
    NSMutableString *s=[NSMutableString string];
    for(int i=0;i<8;i++)[s appendFormat:@"file:///home/mobile/Downloads/browser_multitab.html?tab=%d LinuxRunner Typing %d.\n",i,i*37];
    return s;
}
int main(int argc,char **argv){@autoreleasepool{
    if(argc<4)return 2;   // the session harness appends further arguments
    NSString *password=[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil];
    NSString *dir=@(argv[3]);[[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    LRRFBClient *c=[[LRRFBClient alloc] initWithPort:(uint16_t)atoi(argv[1]) password:password];
    __block BOOL started=NO,ended=NO;__block uint16_t width=0,height=0;double began=now();
    c.statusHandler=^(NSString *s){if([s hasPrefix:@"Linux display ended"]||[s hasPrefix:@"Display error:"])ended=YES;};
    __weak LRRFBClient *weak=c;NSString *text=typedText();
    c.frameHandler=^(NSData *d,uint16_t w,uint16_t h,uint64_t f,uint64_t b){(void)d;(void)f;(void)b;width=w;height=h;
        if(started)return;started=YES;
        // The client is driven from the main run loop; send input from it too.
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(8*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
            [weak pointerX:width/2 y:height/2 buttons:1];[weak pointerX:width/2 y:height/2 buttons:0];});
        // 20 characters every 150 ms: fast typing, within the client's
        // bounded input queue (128 jobs).
        for(NSUInteger start=0;start<text.length;start+=20){
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(9*NSEC_PER_SEC+start/20*150*NSEC_PER_MSEC)),dispatch_get_main_queue(),^{
                for(NSUInteger i=start;i<text.length&&i<start+20;i++){unichar ch=[text characterAtIndex:i];uint32_t sym=ch=='\n'?0xff0d:ch;[weak key:sym down:YES];[weak key:sym down:NO];}});
        }
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(25*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
            [weak key:0xffe3 down:YES];[weak key:'d' down:YES];[weak key:'d' down:NO];[weak key:0xffe3 down:NO];});
    };
    [c start];
    while(!ended&&now()-began<120)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.05]];
    [c stop];
    [text writeToFile:[dir stringByAppendingPathComponent:@"expected.txt"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
    printf("RFB_TYPING_PROBE started=%d ended=%d seconds=%.1f chars=%lu\n",started,ended,now()-began,(unsigned long)text.length);
    return started&&ended?0:1;
}}
