// SPDX-License-Identifier: GPL-2.0-or-later

#import <UIKit/UIKit.h>
#import <XCTest/XCTest.h>

#include "video_encoder.hpp"

#include <poll.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

/* The hardware video encoder of the device the tests run on. The encoder is
 * linked from HostVideoEncoder.xcframework, the archive the runtime links, so
 * the tested object is the one that ships.
 *
 * The capability case discovers the device the way a guest's first probe does
 * and attaches what it found, as JSON, to the test result: it is the record of
 * what a given iPad's encoder can do. The remaining cases check, on the
 * device, the behaviours the Mac unit test established and an iPad could
 * differ in: that every advertised profile and layout encodes, and that a
 * bitrate change reaches a stream that is already running. */
@interface HostVideoEncoderTests : XCTestCase
@end

namespace {

using namespace uml::platform;

const char* codecName(VideoCodec codec)
{
    switch (codec) {
    case VideoCodec::h264: return "h264";
    case VideoCodec::hevc: return "hevc";
    case VideoCodec::jpeg: return "jpeg";
    case VideoCodec::vp9: return "vp9";
    }
    return "unknown";
}

NSArray<NSNumber*>* numbers(const std::vector<int>& values)
{
    NSMutableArray<NSNumber*>* array = [NSMutableArray array];
    for (const int value : values) [array addObject:@(value)];
    return array;
}

NSDictionary* describe(const VideoEncodeCapability& c)
{
    NSMutableArray* profiles = [NSMutableArray array];
    for (const auto& profile : c.profiles) {
        std::vector<int> inputs;
        for (const auto layout : profile.inputs) inputs.push_back(int(layout));
        [profiles addObject:@{@"profile": @(int(profile.profile)), @"inputs": numbers(inputs),
                              @"maximumLevel": @(profile.maximumLevel), @"levelSelectable": @(profile.levelSelectable)}];
    }
    std::vector<int> modes;
    for (const auto mode : c.rateControls) modes.push_back(int(mode));
    return @{
        @"codec": @(codecName(c.codec)), @"profiles": profiles,
        @"minimumWidth": @(c.minimumWidth), @"minimumHeight": @(c.minimumHeight),
        @"maximumWidth": @(c.maximumWidth), @"maximumHeight": @(c.maximumHeight), @"sizeAlignment": @(c.sizeAlignment),
        @"rateControls": numbers(modes), @"maximumBitrate": @(c.maximumBitrate),
        @"minimumQp": @(c.minimumQp), @"maximumQp": @(c.maximumQp), @"maximumBFrames": @(c.maximumBFrames),
        @"maximumTemporalLayers": @(c.maximumTemporalLayers), @"entropyModeSelectable": @(c.entropyModeSelectable),
        @"hdrStaticMetadata": @(c.hdrStaticMetadata), @"concurrentSessions": @(c.concurrentSessions),
        @"framesPerSecond720p": @(c.measuredFramesPerSecond720p), @"framesPerSecond1080p": @(c.measuredFramesPerSecond1080p),
    };
}

/* Pseudo-random planes: busy enough that a rate target binds. */
void fillNoise(WritableVideoFrame& frame, unsigned index)
{
    std::uint32_t state = 0x1234567u + index * 7919u;
    for (std::size_t plane = 0; plane < frame.planeCount(); ++plane) {
        auto bytes = frame.plane(plane);
        for (auto& byte : bytes) {
            state = state * 1664525u + 1013904223u;
            byte = std::uint8_t(64 + (state >> 26) + (plane ? 32 : 0));
        }
    }
}

struct Output {
    std::vector<EncodedVideoFrame> frames;
    unsigned errors {};
    bool drained {};
};

void collect(VideoEncoder& encoder, Output& output, int waitMilliseconds)
{
    pollfd readable {encoder.readinessDescriptor(), POLLIN, 0};
    (void)::poll(&readable, 1, waitMilliseconds);
    for (auto& item : encoder.takeFrames()) {
        switch (item.kind) {
        case EncodedVideoFrameKind::frame: output.frames.push_back(std::move(item)); break;
        case EncodedVideoFrameKind::parameterSets: break;
        case EncodedVideoFrameKind::error: ++output.errors; break;
        case EncodedVideoFrameKind::drained: output.drained = true; break;
        }
    }
}

/* Encodes `count` noise frames and drains, with every wait bounded. `before`
 * runs ahead of each frame's submission. */
template<class Before>
Output encode(VideoEncoder& encoder, unsigned count, Before&& before)
{
    Output output;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    unsigned submitted = 0;
    bool draining = false;
    while (std::chrono::steady_clock::now() < deadline) {
        while (submitted < count) {
            before(encoder, submitted);
            auto frame = encoder.acquireFrame();
            if (!frame) break;
            fillNoise(*frame, submitted);
            encoder.submit(std::move(frame), submitted, false);
            ++submitted;
        }
        if (submitted == count && !draining) {
            encoder.drain();
            draining = true;
        }
        collect(encoder, output, 20);
        if (output.drained) break;
    }
    return output;
}

/* profile_idc of the first SPS in an Annex B H.264 frame, or 0. */
unsigned h264ProfileIdc(const std::vector<std::uint8_t>& bytes)
{
    for (std::size_t i = 0; i + 4 < bytes.size(); ++i)
        if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1 && (bytes[i + 3] & 0x1f) == 7) return bytes[i + 4];
    return 0;
}

const VideoEncodeCapability* find(const std::vector<VideoEncodeCapability>& capabilities, VideoCodec codec)
{
    for (const auto& capability : capabilities)
        if (capability.codec == codec) return &capability;
    return nullptr;
}

} // namespace

@implementation HostVideoEncoderTests

- (void)setUp
{
    [super setUp];
    self.continueAfterFailure = NO;
#if TARGET_OS_SIMULATOR
    /* The guest's encoder is the device's hardware encoder; the Simulator has
     * none. */
    XCTSkip(@"the Simulator has no hardware video encoder");
#endif
}

/* Discovery on this device, recorded. */
- (void)testCapabilitiesOfThisDevice
{
    const auto start = std::chrono::steady_clock::now();
    const auto capabilities = videoEncodeCapabilities();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    XCTAssertFalse(capabilities.empty(), @"this device reports no hardware video encoder");
    NSMutableArray* codecs = [NSMutableArray array];
    for (const auto& capability : capabilities) {
        XCTAssertFalse(capability.profiles.empty());
        XCTAssertGreaterThan(capability.minimumWidth, 0u);
        XCTAssertLessThanOrEqual(capability.minimumWidth, capability.maximumWidth);
        XCTAssertLessThanOrEqual(capability.minimumHeight, capability.maximumHeight);
        XCTAssertFalse(capability.rateControls.empty());
        XCTAssertGreaterThanOrEqual(capability.concurrentSessions, 1u);
        [codecs addObject:describe(capability)];
    }
    XCTAssertNotEqual(find(capabilities, VideoCodec::h264), nullptr, @"no hardware H.264 encoder");

    NSDictionary* report = @{@"discoverySeconds": @(seconds), @"model": UIDevice.currentDevice.model,
                             @"system": UIDevice.currentDevice.systemVersion, @"codecs": codecs};
    NSData* json = [NSJSONSerialization dataWithJSONObject:report
                                                   options:NSJSONWritingPrettyPrinted | NSJSONWritingSortedKeys
                                                     error:nil];
    XCTAssertNotNil(json);
    XCTAttachment* attachment = [XCTAttachment attachmentWithString:[[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding]];
    attachment.name = @"video-encode-capabilities.json";
    attachment.lifetime = XCTAttachmentLifetimeKeepAlways;
    [self addAttachment:attachment];
    NSLog(@"video encode capabilities (%.1f s): %@", seconds, [[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding]);
}

/* Every profile and layout the device lists encodes a short stream with no
 * error and starts it with a key frame. */
- (void)testEveryAdvertisedProfileEncodes
{
    const auto capabilities = videoEncodeCapabilities();
    unsigned cases = 0;
    for (const auto& capability : capabilities) {
        for (const auto& profile : capability.profiles) {
            for (const auto layout : profile.inputs) {
                VideoEncodeSettings settings;
                settings.codec = capability.codec;
                settings.profile = profile.profile;
                settings.input = layout;
                settings.width = 128;
                settings.height = 96;
                settings.bitrate = 2'000'000;
                settings.jpegQuality = 85;
                settings.rateControl = capability.codec == VideoCodec::jpeg ? VideoRateControl::constantQuality
                                                                              : VideoRateControl::variableBitrate;
                settings.quality = 60;
                auto encoder = makeVideoEncoder();
                XCTAssertEqual(encoder->configure(settings), VideoEncodeStatus::ok,
                               @"profile %d layout %d", int(profile.profile), int(layout));
                const unsigned count = capability.codec == VideoCodec::jpeg ? 3 : 6;
                const auto output = encode(*encoder, count, [](VideoEncoder&, unsigned) {});
                XCTAssertTrue(output.drained, @"profile %d layout %d did not drain", int(profile.profile), int(layout));
                XCTAssertEqual(output.errors, 0u);
                XCTAssertEqual(output.frames.size(), std::size_t(count));
                if (!output.frames.empty()) {
                    XCTAssertEqual(output.frames.front().type, VideoFrameType::key);
                    if (capability.codec == VideoCodec::h264)
                        XCTAssertNotEqual(h264ProfileIdc(output.frames.front().bytes), 0u, @"the first frame carries an SPS");
                }
                ++cases;
            }
        }
    }
    XCTAssertGreaterThan(cases, 0u);
}

/* A bitrate change reaches a stream already running: the standard encoder
 * ignores it unless the session is real-time, which the encoder sets. */
- (void)testBitrateChangeTakesEffectOnARunningStream
{
    const auto capabilities = videoEncodeCapabilities();
    const auto* h264 = find(capabilities, VideoCodec::h264);
    XCTAssertNotEqual(h264, nullptr);
    VideoEncodeSettings settings;
    settings.codec = VideoCodec::h264;
    settings.profile = h264->profiles.front().profile;
    settings.input = h264->profiles.front().inputs.front();
    settings.width = 320;
    settings.height = 240;
    settings.bitrate = 1'500'000;
    auto encoder = makeVideoEncoder();
    XCTAssertEqual(encoder->configure(settings), VideoEncodeStatus::ok);
    const auto output = encode(*encoder, 90, [](VideoEncoder& encoder, unsigned index) {
        VideoEncodeUpdate update;
        if (index == 30) update.bitrate = 300'000;
        else if (index == 60) update.bitrate = 3'000'000;
        else return;
        (void)encoder.update(update);
    });
    XCTAssertEqual(output.errors, 0u);
    XCTAssertEqual(output.frames.size(), std::size_t(90));
    const auto window = [&](std::size_t from) {
        double total = 0;
        for (std::size_t i = from; i < from + 15; ++i) total += double(output.frames[i].bytes.size());
        return total / 15.0;
    };
    const double before = window(15), low = window(45), after = window(75);
    NSLog(@"bytes per frame: %.0f -> %.0f -> %.0f", before, low, after);
    XCTAssertLessThan(low, before * 0.7, @"a lower bitrate did not reach the running stream");
    XCTAssertGreaterThan(after, low * 1.5, @"a higher bitrate did not reach the running stream");
}

@end
