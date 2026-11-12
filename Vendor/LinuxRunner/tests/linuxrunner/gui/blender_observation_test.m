#import "BlenderFrameObservation.h"
#import <assert.h>
int main(void) { @autoreleasepool {
    LRBlenderFrameObservation *state = [LRBlenderFrameObservation new];
    NSData *pixels = [[NSMutableData dataWithLength:16] copy];
    [state observe:pixels width:2 height:2 frames:1 bytes:16 at:10];
    [state observe:pixels width:2 height:2 frames:2 bytes:32 at:10.04];
    [state observe:pixels width:2 height:2 frames:3 bytes:48 at:10.10];
    NSDictionary *metrics = [state metricsSince:9];
    assert([metrics[@"delivered_updates"] unsignedLongLongValue] == 3);
    assert([metrics[@"first_decoded_update_seconds"] doubleValue] == 1);
    assert([metrics[@"interval_samples"] unsignedIntegerValue] == 2);
    assert(fabs([metrics[@"recent_interval_p95_ms"] doubleValue] - 60) < .001);
    assert(fabs([metrics[@"recent_interval_median_ms"] doubleValue] - 60) < .001);
    [state observe:[NSData data] width:2 height:2 frames:99 bytes:0 at:11];
    assert([[state snapshot][@"frames"] unsignedLongLongValue] == 3);
    dispatch_group_t group = dispatch_group_create();
    dispatch_group_async(group, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
        @autoreleasepool { for (uint64_t i=4; i<20004; i++) {
            uint64_t payload[2] = {i,i};
            [state observe:[NSData dataWithBytes:payload length:sizeof(payload)]
                     width:2 height:2 frames:i bytes:i*16 at:11+i*.001];
        }}
    });
    for (unsigned i=0; i<20000; i++) { @autoreleasepool {
        NSDictionary *s = [state snapshot];
        NSData *d = s[@"pixels"];
        assert(d.length == [s[@"width"] unsignedIntegerValue]*[s[@"height"] unsignedIntegerValue]*4);
        if ([s[@"frames"] unsignedLongLongValue] > 3)
            assert(((const uint64_t *)d.bytes)[0] == [s[@"frames"] unsignedLongLongValue]);
    }}
    dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
    assert([[state metricsSince:9][@"interval_samples"] unsignedIntegerValue] == 256);
    for(unsigned i=0;i<100;i++) [state observeTransport:@{@"sequence":@(i)}];
    NSDictionary *transport = [state metricsSince:9];
    assert([transport[@"transport_events_observed"] unsignedLongLongValue] == 100);
    assert([transport[@"recent_transport_events"] count] == 64);
    assert([transport[@"recent_transport_events"][0][@"sequence"] unsignedIntValue] == 36);
    for(unsigned i=0;i<40;i++) [state observeTransport:@{@"stage":@"rfb_input_write", @"sequence":@(i)}];
    for(unsigned i=0;i<100;i++) [state observeTransport:@{@"stage":@"rfb_decode_delivery"}];
    NSDictionary *retained = [state metricsSince:9];
    assert([retained[@"recent_input_transport_events"] count] == 32);
    assert([retained[@"input_transport_events_observed"] unsignedIntValue] == 40);
    assert([retained[@"recent_input_transport_events"][0][@"sequence"] unsignedIntValue] == 8);
    assert([retained[@"recent_transport_events"][0][@"stage"] isEqual:@"rfb_decode_delivery"]);
    [state markFailed]; assert([[state snapshot][@"failed"] boolValue]);
    puts("BLENDER_OBSERVATION_PASS coherent frame snapshots, malformed frame rejection, bounded intervals, concurrent delivery");
} return 0; }
