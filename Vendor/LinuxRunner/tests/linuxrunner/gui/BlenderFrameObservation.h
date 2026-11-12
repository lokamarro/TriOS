// Thread-safe, bounded observation of the real RFB decoder for Blender tests.
// These intervals measure decoded updates, not GPU/render/display frame rate.
#import <Foundation/Foundation.h>
@interface LRBlenderFrameObservation : NSObject {
    NSLock *_lock;
    NSData *_pixels;
    uint16_t _width, _height;
    uint64_t _frames, _bytes, _updates;
    double _first, _last, _intervals[256];
    NSUInteger _intervalCount, _intervalNext;
    BOOL _failed;
    NSMutableArray *_transport, *_inputTransport;
    uint64_t _transportEvents, _inputTransportEvents;
}
- (void)observe:(NSData *)pixels width:(uint16_t)width height:(uint16_t)height
         frames:(uint64_t)frames bytes:(uint64_t)bytes at:(double)received;
- (void)markFailed;
- (void)observeTransport:(NSDictionary *)event;
- (NSDictionary *)snapshot;
- (NSDictionary *)metricsSince:(double)start;
@end
@implementation LRBlenderFrameObservation
- (instancetype)init {
    if ((self = [super init])) { _lock = [NSLock new]; _transport = [NSMutableArray new]; _inputTransport = [NSMutableArray new]; }
    return self;
}
- (void)observe:(NSData *)pixels width:(uint16_t)width height:(uint16_t)height
         frames:(uint64_t)frames bytes:(uint64_t)bytes at:(double)received {
    if (!width || !height || pixels.length != (size_t)width * height * 4) return;
    [_lock lock];
    if (_updates && received >= _last) {
        _intervals[_intervalNext] = (received - _last) * 1000;
        _intervalNext = (_intervalNext + 1) % 256;
        _intervalCount = MIN(_intervalCount + 1, (NSUInteger)256);
    }
    if (!_updates) _first = received;
    _last = received;
    _updates++;
    _pixels = pixels; // The decoder delivers an immutable snapshot.
    _width = width; _height = height; _frames = frames; _bytes = bytes;
    [_lock unlock];
}
- (void)markFailed { [_lock lock]; _failed = YES; [_lock unlock]; }
- (void)observeTransport:(NSDictionary *)event {
    [_lock lock];
    if (_transport.count == 64) [_transport removeObjectAtIndex:0];
    [_transport addObject:[event copy]]; _transportEvents++;
    // Retain input-write observations separately so frequent decoded updates
    // cannot evict every event from the actual interaction sequence.
    if ([event[@"stage"] isEqual:@"rfb_input_write"]) {
        if (_inputTransport.count == 32) [_inputTransport removeObjectAtIndex:0];
        [_inputTransport addObject:[event copy]]; _inputTransportEvents++;
    }
    [_lock unlock];
}
- (NSDictionary *)snapshot {
    [_lock lock];
    NSDictionary *result = @{@"pixels":_pixels ?: NSNull.null, @"width":@(_width),
        @"height":@(_height), @"frames":@(_frames), @"wire_bytes":@(_bytes),
        @"received_seconds":@(_last), @"failed":@(_failed)};
    [_lock unlock];
    return result;
}
- (NSDictionary *)metricsSince:(double)start {
    [_lock lock];
    NSMutableArray *samples = [NSMutableArray arrayWithCapacity:_intervalCount];
    for (NSUInteger i=0; i<_intervalCount; i++) [samples addObject:@(_intervals[i])];
    uint64_t updates = _updates, bytes = _bytes;
    double first = _first;
    NSArray *transport = [_transport copy], *inputs = [_inputTransport copy]; uint64_t transportEvents = _transportEvents, inputEvents = _inputTransportEvents;
    [_lock unlock];
    NSArray *ordered = [samples sortedArrayUsingSelector:@selector(compare:)];
    return @{@"scope":@"RFB decoder callback arrival; excludes GPU render rate and device presentation",
        @"recent_transport_events":transport, @"transport_events_observed":@(transportEvents),
        @"transport_window_capacity":@64, @"recent_input_transport_events":inputs,
        @"input_transport_window_capacity":@32, @"input_transport_events_observed":@(inputEvents), @"delivered_updates":@(updates), @"wire_bytes":@(bytes),
        @"first_decoded_update_seconds":updates ? @(first-start) : NSNull.null,
        @"interval_window_capacity":@256, @"interval_samples":@(ordered.count),
        @"recent_interval_median_ms":ordered.count ? ordered[ordered.count/2] : NSNull.null,
        @"recent_interval_p95_ms":ordered.count ? ordered[(ordered.count*95+99)/100-1] : NSNull.null};
}
@end
