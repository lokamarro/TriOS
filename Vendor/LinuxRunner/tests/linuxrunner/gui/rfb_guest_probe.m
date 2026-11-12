#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
#import "app/LinuxRunner/FramebufferPatch.h"
int main(int argc,char **argv){@autoreleasepool{
    if(argc!=4&&argc!=5)return 2;
    BOOL xterm=argc==5&&strcmp(argv[4],"xterm")==0;
    NSString *password=[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil];
    LRRFBClient *client=[[LRRFBClient alloc] initWithPort:(uint16_t)atoi(argv[1]) password:password];
    NSString *directory=@(argv[3]);[[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:nil];
    __block BOOL rendered=NO,clicked=NO,ended=NO;__block uint64_t frames=0,bytes=0;NSDate *begin=NSDate.date;
    client.statusHandler=^(NSString *status){fprintf(stderr,"%s\n",status.UTF8String);if([status hasPrefix:@"Linux display ended"]||[status hasPrefix:@"Display error:"])ended=YES;};
    __weak LRRFBClient *weak=client;
    client.frameHandler=^(NSData *data,uint16_t width,uint16_t height,uint64_t n,uint64_t wire){
        const uint8_t *p=data.bytes;frames=n;bytes=wire;
        if(width!=640||height!=480)return;
        if(xterm){
            [data writeToFile:[directory stringByAppendingPathComponent:@"xterm-frame.bgra"] atomically:YES];
            if(!rendered){rendered=YES;
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW,8*NSEC_PER_SEC),dispatch_get_global_queue(0,0),^{
                    [weak pointerX:150 y:100 buttons:1];[weak pointerX:150 y:100 buttons:0];
                    const char *command="uname -m; echo xterm-real\n";
                    for(const char *c=command;*c;c++){uint32_t symbol=*c=='\n'?0xff0d:(uint8_t)*c;[weak key:symbol down:YES];[weak key:symbol down:NO];}
                });
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW,15*NSEC_PER_SEC),dispatch_get_global_queue(0,0),^{
                    const char *command="exit\n";
                    for(const char *c=command;*c;c++){uint32_t symbol=*c=='\n'?0xff0d:(uint8_t)*c;[weak key:symbol down:YES];[weak key:symbol down:NO];}
                });
            }return;
        }
        size_t red=(100*width+50)*4,counter=(220*width+100)*4;
        if(!rendered&&p[red]==0x40&&p[red+1]==0x40&&p[red+2]==0xd8){
            rendered=YES;[data writeToFile:[directory stringByAppendingPathComponent:@"first-frame.bgra"] atomically:YES];
            [weak pointerX:100 y:230 buttons:1];[weak pointerX:100 y:230 buttons:0];
            for(uint32_t c='A';c<='C';c++){[weak key:c down:YES];[weak key:c down:NO];}
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,3*NSEC_PER_SEC),dispatch_get_global_queue(0,0),^{[weak key:0xff1b down:YES];[weak key:0xff1b down:NO];});
        }
        if(rendered&&p[counter]==0x56&&p[counter+1]==0x6c&&p[counter+2]==0x23){clicked=YES;[data writeToFile:[directory stringByAppendingPathComponent:@"interactive-frame.bgra"] atomically:YES];}
    };
    if(getenv("LR_RFB_PATCH_TEST")) {
        __block NSMutableData *assembled=nil;__block uint16_t fw=0,fh=0;
        client.patchFrameHandler=^(NSData *patch,uint16_t w,uint16_t h,uint64_t count,uint64_t bytes,LRFrameDamage damage){
            if(fw!=w||fh!=h){assembled=[NSMutableData dataWithLength:(NSUInteger)w*h*4];fw=w;fh=h;}
            assert(LRApplyFramebufferPatch(assembled,patch,w,h,damage));
            weak.frameHandler([assembled copy],w,h,count,bytes);
        };
    }
    [client start];
    // Run-loop only supports this host probe; UIKit owns its loop in the app.
    while(!ended&&-[begin timeIntervalSinceNow]<60)[NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];
    [client stop];NSDictionary *result=@{@"platform":@"Darwin ARM64 host executing Linux ARM64 guest",@"rendered":@(rendered),@"pointer_repaint":@(clicked),@"display_ended":@(ended),@"frames":@(frames),@"wire_bytes":@(bytes),@"seconds":@(-[begin timeIntervalSinceNow]),@"width":@640,@"height":@480};
    [[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:nil] writeToFile:[directory stringByAppendingPathComponent:@"probe.json"] atomically:YES];
    printf("%s\n",result.description.UTF8String);return rendered&&(clicked||xterm)&&ended?0:1;
}}
