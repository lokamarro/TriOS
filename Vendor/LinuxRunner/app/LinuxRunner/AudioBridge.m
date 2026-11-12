#import "AudioBridge.h"
#import <AVFoundation/AVFoundation.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <unistd.h>

// 48 kHz, 2 channels, signed 16-bit little endian: the session's
// module-simple-protocol-tcp format (session.sh).
enum { LRAudioRate = 48000, LRAudioChannels = 2, LRAudioFrameBytes = 4 };
enum { LRAudioCapacity = LRAudioRate };             // 1 s ring (frames)
enum { LRAudioMaxBacklog = LRAudioRate / 4 };       // above 250 ms queued, drop
enum { LRAudioTargetBacklog = LRAudioRate / 10 };   // ...down to 100 ms
enum { LRAudioPrebuffer = LRAudioRate * 6 / 100 };  // after running dry, resume at 60 ms

@implementation LRAudioBridge {
    uint16_t _port;
    int16_t *_ring;                 // LRAudioCapacity frames, interleaved
    _Atomic uint64_t _written, _read; // frame counters (single producer/consumer)
    _Atomic bool _stopped, _connected;
    _Atomic int _socket;
    _Atomic uint64_t _bytes, _underruns, _dropped, _played;
    AVAudioEngine *_engine;
    AVAudioSourceNode *_source;
    NSThread *_reader;
    NSString *_lastError;
    BOOL _engineRunning;
    bool _buffering; // render thread only
    id _interruptionObserver;
}

+ (BOOL)enabled {
    id value = [NSUserDefaults.standardUserDefaults objectForKey:@"LinuxRunnerAudio.enabled"];
    return value ? [value boolValue] : YES;
}

- (instancetype)initWithPort:(uint16_t)port {
    if ((self = [super init])) {
        _port = port;
        _ring = calloc(LRAudioCapacity * LRAudioChannels, sizeof(int16_t));
        atomic_store(&_socket, -1);
    }
    return self;
}

// The reader thread retains the bridge until -stop, so this runs after it.
- (void)dealloc { [_engine stop]; free(_ring); }

- (void)start {
    if (_reader || !_ring) return;
    atomic_store(&_stopped, false);
    _reader = [[NSThread alloc] initWithTarget:self selector:@selector(readLoop) object:nil];
    _reader.name = @"LinuxRunner audio";
    _reader.qualityOfService = NSQualityOfServiceUserInteractive;
    [_reader start];
}

- (void)stop {
    if (atomic_exchange(&_stopped, true)) return;
    int fd = atomic_exchange(&_socket, -1);
    if (fd >= 0) { shutdown(fd, SHUT_RDWR); close(fd); }
    _reader = nil;
    void (^teardown)(void) = ^{
        if (self->_interruptionObserver) [NSNotificationCenter.defaultCenter removeObserver:self->_interruptionObserver];
        self->_interruptionObserver = nil;
        [self->_engine stop];
#if TARGET_OS_IPHONE
        if (self->_engineRunning)
            [AVAudioSession.sharedInstance setActive:NO withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation error:nil];
#endif
        self->_engineRunning = NO; self->_engine = nil; self->_source = nil;
    };
    if (NSThread.isMainThread) teardown(); else dispatch_sync(dispatch_get_main_queue(), teardown);
}

- (NSDictionary *)statistics {
    return @{@"state": atomic_load(&_stopped) ? @"stopped" : (atomic_load(&_connected) ? (_engineRunning ? @"playing" : @"connected") : @"waiting for Linux sound server"),
             @"connected": @(atomic_load(&_connected)), @"bytes": @(atomic_load(&_bytes)),
             @"played_ms": @(atomic_load(&_played) * 1000 / LRAudioRate), @"underruns": @(atomic_load(&_underruns)),
             @"dropped_ms": @(atomic_load(&_dropped) * 1000 / LRAudioRate), @"last_error": _lastError ?: @""};
}

// Network side: connect (retrying while the session starts PulseAudio), then
// append whole frames to the ring. A full ring drops the newest data.
- (void)readLoop {
    uint8_t buffer[16384]; size_t pending = 0;
    while (!atomic_load(&_stopped)) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { usleep(500000); continue; }
        int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 500000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        struct sockaddr_in address = {.sin_len = sizeof address, .sin_family = AF_INET, .sin_port = htons(_port)};
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(fd, (struct sockaddr *)&address, sizeof address) != 0) { close(fd); usleep(500000); continue; }
        atomic_store(&_socket, fd);
        if (atomic_load(&_stopped)) break;
        atomic_store(&_connected, true); pending = 0;
        for (;;) {
            ssize_t n = recv(fd, buffer + pending, sizeof buffer - pending, 0);
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) break;
            if (n < 0) { if (atomic_load(&_stopped)) break; continue; }
            atomic_fetch_add(&_bytes, (uint64_t)n);
            pending += (size_t)n;
            size_t frames = pending / LRAudioFrameBytes;
            [self appendFrames:(const int16_t *)buffer count:frames];
            memmove(buffer, buffer + frames * LRAudioFrameBytes, pending - frames * LRAudioFrameBytes);
            pending -= frames * LRAudioFrameBytes;
            if (frames && !_engineRunning) dispatch_async(dispatch_get_main_queue(), ^{ [self startEngine]; });
        }
        atomic_store(&_connected, false);
        int current = atomic_exchange(&_socket, -1);
        if (current >= 0) close(current);
        if (!atomic_load(&_stopped)) usleep(500000);
    }
}

- (void)appendFrames:(const int16_t *)frames count:(size_t)count {
    uint64_t written = atomic_load_explicit(&_written, memory_order_relaxed), read = atomic_load_explicit(&_read, memory_order_acquire);
    size_t space = LRAudioCapacity - (size_t)(written - read);
    if (count > space) { atomic_fetch_add(&_dropped, count - space); count = space; }
    for (size_t i = 0; i < count; i++) {
        size_t slot = (size_t)((written + i) % LRAudioCapacity) * LRAudioChannels;
        _ring[slot] = frames[i * 2]; _ring[slot + 1] = frames[i * 2 + 1];
    }
    atomic_store_explicit(&_written, written + count, memory_order_release);
}

// Render side (audio thread): converts to float; silence on underrun.
- (OSStatus)render:(AVAudioFrameCount)frameCount into:(AudioBufferList *)output {
    float *left = output->mBuffers[0].mData, *right = output->mNumberBuffers > 1 ? output->mBuffers[1].mData : NULL;
    uint64_t written = atomic_load_explicit(&_written, memory_order_acquire), read = atomic_load_explicit(&_read, memory_order_relaxed);
    uint64_t queued = written - read;
    if (queued > LRAudioMaxBacklog) {   // clocks drift or the app paused: catch up
        uint64_t skip = queued - LRAudioTargetBacklog;
        read += skip; queued -= skip; atomic_fetch_add(&_dropped, skip);
    }
    if (_buffering && queued < LRAudioPrebuffer) queued = 0; else _buffering = false;
    AVAudioFrameCount available = (AVAudioFrameCount)MIN((uint64_t)frameCount, queued);
    for (AVAudioFrameCount i = 0; i < available; i++) {
        size_t slot = (size_t)((read + i) % LRAudioCapacity) * LRAudioChannels;
        left[i] = _ring[slot] / 32768.0f;
        if (right) right[i] = _ring[slot + 1] / 32768.0f;
    }
    for (AVAudioFrameCount i = available; i < frameCount; i++) { left[i] = 0; if (right) right[i] = 0; }
    if (available < frameCount) {
        if (available) atomic_fetch_add(&_underruns, 1);
        _buffering = true;
    }
    atomic_fetch_add(&_played, available);
    atomic_store_explicit(&_read, read + available, memory_order_release);
    return noErr;
}

- (void)startEngine {
    if (_engineRunning || atomic_load(&_stopped) || !LRAudioBridge.enabled) return;
    NSError *error = nil;
#if TARGET_OS_IPHONE
    AVAudioSession *session = AVAudioSession.sharedInstance;
    // Mix with other apps' audio, like a game; never takes over the user's music.
    if (![session setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:AVAudioSessionCategoryOptionMixWithOthers error:&error] ||
        ![session setActive:YES error:&error]) { _lastError = error.localizedDescription; return; }
#endif
    if (!_engine) {
        _engine = [AVAudioEngine new];
        AVAudioFormat *format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:LRAudioRate channels:LRAudioChannels];
        __unsafe_unretained LRAudioBridge *weak = self; // the engine never outlives -stop
        _source = [[AVAudioSourceNode alloc] initWithFormat:format renderBlock:^OSStatus(BOOL *silence, const AudioTimeStamp *time, AVAudioFrameCount frames, AudioBufferList *output) {
            (void)silence; (void)time; return [weak render:frames into:output];
        }];
        [_engine attachNode:_source];
        [_engine connect:_source to:_engine.mainMixerNode format:format];
#if TARGET_OS_IPHONE
        __weak typeof(self) weakSelf = self;
        _interruptionObserver = [NSNotificationCenter.defaultCenter addObserverForName:AVAudioSessionInterruptionNotification object:session queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note) {
            typeof(self) strong = weakSelf; if (!strong) return;
            if ([note.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue] == AVAudioSessionInterruptionTypeBegan) {
                [strong->_engine pause]; strong->_engineRunning = NO;   // a call, Siri, an alarm
            } else [strong startEngine];                                // resume when iOS allows it
        }];
#endif
    }
    if (![_engine startAndReturnError:&error]) { _lastError = error.localizedDescription; return; }
    _engineRunning = YES; _lastError = nil;
}
@end
