#import <Foundation/Foundation.h>
#import <time.h>
#import "app/LinuxRunner/RFBClient.h"
#import "tests/linuxrunner/gui/BlenderFrameObservation.h"
static double monotonicSeconds(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
int main(int argc, char **argv) {
 @autoreleasepool {
    if (argc != 5 || !getenv("LR_GUI_APPLICATION_LOG")) return 2;
    NSString *password = [NSString stringWithContentsOfFile:@(argv[2]) encoding:NSASCIIStringEncoding error:nil];
    LRRFBClient *c = [[LRRFBClient alloc] initWithPort:atoi(argv[1]) password:password];
    LRBlenderFrameObservation *observation = [LRBlenderFrameObservation new];
    c.frameHandler = ^(NSData *data,uint16_t w,uint16_t h,uint64_t f,uint64_t b) {
        [observation observe:data width:w height:h frames:f bytes:b at:monotonicSeconds()];
    };
    c.transportMetricsHandler = ^(NSDictionary *event) { [observation observeTransport:event]; };
    c.statusHandler = ^(NSString *status) {
        fprintf(stderr,"%s\n",status.UTF8String);
        if ([status hasPrefix:@"Display error:"]) [observation markFailed];
    };
    double start = monotonicSeconds(); [c start];
    NSString *log = nil; BOOL passed = NO;
    while (![[observation snapshot][@"failed"] boolValue] && monotonicSeconds()-start < 45) {
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:.1]];
        log = [NSString stringWithContentsOfFile:@(getenv("LR_GUI_APPLICATION_LOG")) encoding:NSUTF8StringEncoding error:nil];
        if ([log containsString:@"MESA_METAL_CONTRACT_PASS"]) { passed = YES; break; }
        if ([log containsString:@"Assertion"] || [log containsString:@"Aborted"]) break;
    }
    BOOL hardware = [log containsString:@"MESA_METAL_GPU_PRESENT"];
    NSDictionary *snapshot = [observation snapshot]; BOOL failed = [snapshot[@"failed"] boolValue];
    NSDictionary *result = @{
        @"environment":@"Mac ARM64 interpreter + Mesa context + Metal draw replacement; not iPhone",
        @"contract":@(passed), @"GPU_present":@(hardware), @"transport_error":@(failed),
        @"RFB_updates":snapshot[@"frames"], @"transport_metrics":[observation metricsSince:start]
    };
    [[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:nil]
        writeToFile:[@(argv[3]) stringByAppendingPathComponent:@"mesa-metal-result.json"] atomically:YES];
    [c stop]; return !passed || !hardware || failed;
 }
}
