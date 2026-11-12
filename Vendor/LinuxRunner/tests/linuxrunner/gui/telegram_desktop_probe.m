// macOS guest-fixture test, never physical-device evidence. Does not authenticate.
#import <Foundation/Foundation.h>
#import "app/LinuxRunner/RFBClient.h"
static NSData *pixels; static uint16_t width,height; static uint64_t frames,bytes;
static void pauseLoop(double seconds) {
    NSDate *until=[NSDate dateWithTimeIntervalSinceNow:seconds];
    while(until.timeIntervalSinceNow>0) [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];
}
static void capture(NSString *out, NSString *name) {
    [pixels writeToFile:[out stringByAppendingPathComponent:[name stringByAppendingString:@".bgra"]] atomically:YES];
    NSDictionary *info=@{@"width":@(width),@"height":@(height),@"environment":@"macOS host ARM64 interpreter"};
    [[NSJSONSerialization dataWithJSONObject:info options:0 error:nil] writeToFile:[out stringByAppendingPathComponent:[name stringByAppendingString:@".json"]] atomically:YES];
}
static void click(LRRFBClient *client, unsigned x, unsigned y) {
    [client pointerX:x y:y buttons:1];pauseLoop(.1);[client pointerX:x y:y buttons:0];
}
int main(int argc,char **argv) { @autoreleasepool {
    if(argc!=5)return 2;
    NSString *out=@(argv[3]), *session=[@(getenv("LR_GUI_APPLICATION_LOG")) stringByDeletingLastPathComponent];
    LRRFBClient *client=[[LRRFBClient alloc] initWithPort:atoi(argv[1]) password:[NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil]];
    client.statusHandler=^(NSString *s){fprintf(stderr,"%s\n",s.UTF8String);};
    client.frameHandler=^(NSData *d,uint16_t w,uint16_t h,uint64_t f,uint64_t n){dispatch_async(dispatch_get_main_queue(),^{pixels=d;width=w;height=h;frames=f;bytes=n;});};
    [client start]; BOOL ready=NO, menu=getenv("LR_TELEGRAM_MENU_TEST")!=NULL;
    for(int i=0;i<500;i++) {
        pauseLoop(.2);
        NSString *state=[NSString stringWithContentsOfFile:[session stringByAppendingPathComponent:@"desktop-state"] encoding:NSUTF8StringEncoding error:nil];
        if([state containsString:@"ready=1"]&&pixels){ready=YES;break;}
    }
    if(!ready){[client stop];return 1;}
    if(menu) {
        pauseLoop(3);click(client,30,height-21);pauseLoop(3);
        [client pointerX:70 y:height-110 buttons:0];pauseLoop(4);capture(out,@"internet-menu");
        // The caller inspects the captured real menu before providing an item
        // coordinate. Never guess a location that could request authentication.
        NSDictionary *coordinate=nil;
        for(int i=0;i<900;i++) {
            pauseLoop(.1);
            NSData *json=[NSData dataWithContentsOfFile:[out stringByAppendingPathComponent:@"menu-click.json"]];
            if(json) { coordinate=[NSJSONSerialization JSONObjectWithData:json options:0 error:nil];break; }
        }
        if(![coordinate isKindOfClass:NSDictionary.class] || ![coordinate[@"item"] isEqual:@"Telegram Desktop"] || [coordinate[@"x"] unsignedIntegerValue]>=width || [coordinate[@"y"] unsignedIntegerValue]>=height){[client stop];return 2;}
        click(client,[coordinate[@"x"] unsignedIntValue],[coordinate[@"y"] unsignedIntValue]);
    }
    NSString *launch=[NSString stringWithFormat:@"#!/bin/sh\nset -u\necho DESKTOP_REPRODUCE_BEGIN\nfor n in HOME PATH PWD DISPLAY XAUTHORITY XDG_RUNTIME_DIR XDG_CONFIG_HOME XDG_DATA_HOME; do printf '%%s=' \"$n\"; printenv \"$n\" || true; done\n%@\nsleep 12\nfor pid in $(pidof telegram-desktop | tr ' ' '\\n' | sort -n | head -n 1); do echo TELEGRAM_PROCESS=$pid; head -n 2048 /proc/$pid/maps; tr '\\0' '\\n' < /proc/$pid/environ | grep -E '^(HOME|PATH|DISPLAY|XAUTHORITY|XDG_RUNTIME_DIR|XDG_CONFIG_HOME|XDG_DATA_HOME|DESKTOP_SESSION|XDG_CURRENT_DESKTOP)='; done\necho SNAPSHOT_ONE\nhead -n 256 /proc/ish/tasks\nsleep 8\necho SNAPSHOT_TWO\nhead -n 256 /proc/ish/tasks\n",menu?@"echo LAUNCH_SOURCE=actual-panel-menu":@"gio launch /usr/share/applications/org.telegram.desktop.desktop\necho DESKTOP_GIO_EXIT=$?"];
    [launch writeToFile:[session stringByAppendingPathComponent:@"requests/launch-gio-reproduce.sh"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
    pauseLoop(45);capture(out,@"host-desktop-telegram");
    NSDictionary *result=@{@"ready":@(ready),@"width":@(width),@"height":@(height),@"frames":@(frames),@"bytes":@(bytes),@"launch_source":menu?@"actual LXPanel menu click":@"GIO desktop-entry activation",@"application":@"NOT ACCEPTED: inspect actual pixels/logs",@"environment":@"macOS host ARM64 interpreter",@"authentication":@"not attempted"};
    [[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:nil] writeToFile:[out stringByAppendingPathComponent:@"capture.json"] atomically:YES];
    [client stop];return 0;
} }
