#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Plays Linux session sound on iOS. The session's PulseAudio server offers its
// output (null sink monitor) as 48 kHz stereo s16le PCM on a loopback port the
// app chose (session file audio-port); this reads it and plays it through
// AVAudioEngine. Latency is kept bounded: on a backlog the oldest audio is
// dropped rather than letting the delay grow.
@interface LRAudioBridge : NSObject
- (instancetype)initWithPort:(uint16_t)port;
- (void)start;
- (void)stop;
// state, connected, bytes, played_ms, underruns, dropped_ms, last_error
- (NSDictionary *)statistics;
+ (BOOL)enabled; // Settings › Audio › Linux Sound (default on)
@end

NS_ASSUME_NONNULL_END
