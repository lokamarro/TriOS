// SPDX-License-Identifier: GPL-2.0-or-later
//
// Starts the Apple microphone in the iOS Simulator and posts the audio-session
// notifications that iOS 27 posts when another app takes the session and gives
// it back. An interruption must stop the microphone and release the session.
// The system's advice to resume must start it again; without that advice it
// must start again only if no other app plays, because starting takes the
// session and the microphone must never take it from an app that is playing
// or recording, such as a call. The app's own deactivations interrupt
// nothing. run_ios_interruption.py builds this file with the microphone and
// the session for the Simulator and runs it.
//
// Nothing here opens a real input or can show a permission prompt: the
// engine's start and stop, its input node's format and tap, and the session's
// category and activation are hooked, as the sink test hooks the session. The
// real microphone is checked by hand (see the audio README).

#include "capture_ring.hpp"
#include "ios_session_notifications.hpp"
#include "microphone_capture_source.hpp"

#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>

namespace {

int failures = 0;

void check(bool passed, const char* name, const std::string& detail = {})
{
    failures += passed ? 0 : 1;
    std::printf("%s %s%s%s\n", passed ? "PASS" : "FAIL", name,
        detail.empty() ? "" : ": ", detail.c_str());
    std::fflush(stdout);
}

// What the hooks observe and drive.
std::atomic<bool> otherAudioPlaying {};
std::atomic<int> engineStarts {};
std::atomic<int> engineStops {};
std::atomic<int> sessionActivations {};
std::atomic<int> sessionDeactivations {};

struct Counts final {
    int starts {};
    int stops {};
    int activations {};
    int deactivations {};

    [[nodiscard]] static Counts now()
    {
        return {engineStarts.load(), engineStops.load(),
            sessionActivations.load(), sessionDeactivations.load()};
    }

    [[nodiscard]] std::string text() const
    {
        return "starts " + std::to_string(starts) + ", stops " +
            std::to_string(stops) + ", activations " +
            std::to_string(activations) + ", deactivations " +
            std::to_string(deactivations);
    }

    bool operator==(const Counts&) const = default;
};

[[nodiscard]] Counts since(const Counts& before)
{
    const auto now = Counts::now();
    return {now.starts - before.starts, now.stops - before.stops,
        now.activations - before.activations,
        now.deactivations - before.deactivations};
}

void installHooks()
{
    Method hint = class_getInstanceMethod([AVAudioSession class],
        @selector(secondaryAudioShouldBeSilencedHint));
    method_setImplementation(hint, imp_implementationWithBlock(^BOOL(id) {
        return otherAudioPlaying.load();
    }));

    Method category = class_getInstanceMethod([AVAudioSession class],
        @selector(setCategory:mode:options:error:));
    method_setImplementation(category, imp_implementationWithBlock(
        ^BOOL(id, NSString*, NSString*, NSUInteger, NSError* __autoreleasing*) {
          return YES;
        }));

    Method activate = class_getInstanceMethod(
        [AVAudioSession class], @selector(setActive:error:));
    method_setImplementation(activate, imp_implementationWithBlock(
        ^BOOL(id, BOOL active, NSError* __autoreleasing*) {
          if (active)
              ++sessionActivations;
          return YES;
        }));

    Method deactivate = class_getInstanceMethod(
        [AVAudioSession class], @selector(setActive:withOptions:error:));
    method_setImplementation(deactivate, imp_implementationWithBlock(
        ^BOOL(id, BOOL active, NSUInteger, NSError* __autoreleasing*) {
          ++(active ? sessionActivations : sessionDeactivations);
          return YES;
        }));

    Method start = class_getInstanceMethod(
        [AVAudioEngine class], @selector(startAndReturnError:));
    method_setImplementation(start, imp_implementationWithBlock(
        ^BOOL(id, NSError* __autoreleasing*) {
          ++engineStarts;
          return YES;
        }));

    Method stop =
        class_getInstanceMethod([AVAudioEngine class], @selector(stop));
    method_setImplementation(stop, imp_implementationWithBlock(^(id) {
        ++engineStops;
    }));

    Method prepare =
        class_getInstanceMethod([AVAudioEngine class], @selector(prepare));
    method_setImplementation(prepare, imp_implementationWithBlock(^(id) {}));

    // The input node answers a format and takes a tap without any hardware.
    static AVAudioFormat* const format =
        [[AVAudioFormat alloc] initStandardFormatWithSampleRate:48000
                                                       channels:1];
    Method inputFormat = class_getInstanceMethod(
        [AVAudioInputNode class], @selector(outputFormatForBus:));
    method_setImplementation(inputFormat,
        imp_implementationWithBlock(^AVAudioFormat*(id, AVAudioNodeBus) {
          return format;
        }));
    Method tap = class_getInstanceMethod([AVAudioInputNode class],
        @selector(installTapOnBus:bufferSize:format:error:block:));
    method_setImplementation(tap, imp_implementationWithBlock(
        ^BOOL(id, AVAudioNodeBus, AVAudioFrameCount, AVAudioFormat*,
            NSError* __autoreleasing*, AVAudioNodeTapBlock) { return YES; }));
    Method untap = class_getInstanceMethod(
        [AVAudioInputNode class], @selector(removeTapOnBus:));
    method_setImplementation(untap,
        imp_implementationWithBlock(^(id, AVAudioNodeBus) {}));
}

// A guest that records, as the capture source starts it.
struct Recording final {
    Recording()
        : input(uml::audio::makeAppleMicrophoneInput())
        , ring(1U << 16)
    {
    }

    [[nodiscard]] bool start() { return input->start(1, ring); }
    void stop() { input->stop(); }

    std::unique_ptr<uml::audio::MicrophoneInput> input;
    uml::audio::CaptureRing ring;
};

// The resumption notification with no context, as a recommendation the
// system does not give.
void postRecommendationWithoutContext()
{
    [[NSNotificationCenter defaultCenter]
        postNotificationName:AVAudioSessionResumptionRecommendationNotification
                      object:[AVAudioSession sharedInstance]
                    userInfo:nil];
}

void startsAndStops()
{
    otherAudioPlaying = false;
    Recording recording;
    auto before = Counts::now();
    check(recording.start(), "the microphone starts");
    auto seen = since(before);
    check(seen.starts == 1 && seen.activations == 1,
        "starting takes the session and starts the engine once", seen.text());
    before = Counts::now();
    recording.stop();
    seen = since(before);
    check(seen.stops == 1 && seen.deactivations == 1,
        "stopping stops the engine and releases the session", seen.text());
}

void deactivationsThatInterruptNothing()
{
    otherAudioPlaying = false;
    Recording recording;
    check(recording.start(), "the microphone starts before the deactivations");
    const auto before = Counts::now();
    postDeactivation(AVAudioSessionDeactivationSourceApp, false);
    postDeactivation(AVAudioSessionDeactivationSourceSystem, false);
    const auto seen = since(before);
    check(seen == Counts {},
        "the app's own deactivation and a system deactivation that "
        "interrupts nothing leave the microphone running",
        seen.text());
}

void interruptionStopsTheMicrophone()
{
    otherAudioPlaying = true;
    Recording recording;
    check(recording.start(), "the microphone starts before the interruption");
    auto before = Counts::now();
    postInterruptionBegan();
    auto seen = since(before);
    check(seen.stops == 1 && seen.deactivations == 1 && seen.starts == 0 &&
            seen.activations == 0,
        "an interruption stops the engine and releases the session",
        seen.text());
    before = Counts::now();
    postInterruptionBegan();
    seen = since(before);
    check(seen == Counts {}, "a second interruption does nothing more",
        seen.text());
    otherAudioPlaying = false;
}

void adviceToResumeStartsIt()
{
    otherAudioPlaying = false;
    Recording recording;
    check(recording.start(), "the microphone starts before the interruption");
    postInterruptionBegan();
    auto before = Counts::now();
    postInterruptionEnded(true);
    auto seen = since(before);
    check(seen.starts == 1 && seen.activations == 1,
        "a recommendation to resume starts the microphone again",
        seen.text());
    before = Counts::now();
    postInterruptionEnded(true);
    seen = since(before);
    check(seen == Counts {},
        "a recommendation while the microphone runs changes nothing",
        seen.text());

    // The system's advice settles it, whatever the hint says.
    postInterruptionBegan();
    otherAudioPlaying = true;
    before = Counts::now();
    postInterruptionEnded(true);
    seen = since(before);
    check(seen.starts == 1 && seen.activations == 1,
        "a recommendation to resume starts it while another app plays too",
        seen.text());
    otherAudioPlaying = false;
}

// The microphone never takes the session from an app that plays.
void noAdviceWhileAnotherAppPlays()
{
    otherAudioPlaying = false;
    Recording recording;
    check(recording.start(), "the microphone starts before the interruption");
    postInterruptionBegan();
    otherAudioPlaying = true;
    auto before = Counts::now();
    postInterruptionEnded(false);
    auto seen = since(before);
    check(seen == Counts {},
        "a recommendation not to resume while another app plays starts "
        "nothing and takes no session",
        seen.text());
    before = Counts::now();
    postRecommendationWithoutContext();
    seen = since(before);
    check(seen == Counts {},
        "so does a recommendation that carries no advice", seen.text());

    // It stays closed until the next recommendation to resume.
    otherAudioPlaying = false;
    before = Counts::now();
    postInterruptionEnded(true);
    seen = since(before);
    check(seen.starts == 1 && seen.activations == 1,
        "the next recommendation to resume starts it", seen.text());
}

void noAdviceWhileNoAppPlays()
{
    otherAudioPlaying = false;
    Recording recording;
    check(recording.start(), "the microphone starts before the interruption");
    postInterruptionBegan();
    auto before = Counts::now();
    postInterruptionEnded(false);
    auto seen = since(before);
    check(seen.starts == 1 && seen.activations == 1,
        "a recommendation not to resume starts the microphone when no other "
        "app plays",
        seen.text());

    postInterruptionBegan();
    before = Counts::now();
    postRecommendationWithoutContext();
    seen = since(before);
    check(seen.starts == 1 && seen.activations == 1,
        "so does a recommendation that carries no advice", seen.text());
}

void guestRestartsRecording()
{
    otherAudioPlaying = false;
    Recording recording;
    check(recording.start(), "the microphone starts before the interruption");
    postInterruptionBegan();
    otherAudioPlaying = true;
    postInterruptionEnded(false);
    const auto before = Counts::now();
    check(recording.start(), "the guest's next recording starts it again");
    const auto seen = since(before);
    check(seen.starts == 1 && seen.activations == 1,
        "the engine starts once for it", seen.text());
    otherAudioPlaying = false;
}

void stoppedAndDestroyedMicrophones()
{
    otherAudioPlaying = false;
    {
        Recording recording;
        check(recording.start(), "the microphone starts before it is stopped");
        postInterruptionBegan();
        recording.stop();
        const auto before = Counts::now();
        postInterruptionEnded(true);
        postInterruptionBegan();
        postInterruptionEnded(false);
        const auto seen = since(before);
        check(seen == Counts {},
            "a microphone the guest stopped stays stopped", seen.text());
    }
    {
        Recording recording;
        check(recording.start(), "the microphone starts before it is destroyed");
        postInterruptionBegan();
        recording.input.reset();
        const auto before = Counts::now();
        postInterruptionEnded(true);
        postInterruptionBegan();
        postInterruptionEnded(false);
        const auto seen = since(before);
        check(seen == Counts {},
            "notifications for a destroyed microphone do nothing",
            seen.text());
    }
}

// The notification iOS 27 deprecates is no longer observed.
void deprecatedNotificationIsIgnored()
{
    otherAudioPlaying = false;
    Recording recording;
    check(recording.start(), "the microphone starts before the old notification");
    const auto before = Counts::now();
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [[NSNotificationCenter defaultCenter]
        postNotificationName:AVAudioSessionInterruptionNotification
                      object:[AVAudioSession sharedInstance]
                    userInfo:@{
                        AVAudioSessionInterruptionTypeKey :
                            @(AVAudioSessionInterruptionTypeBegan)
                    }];
#pragma clang diagnostic pop
    const auto seen = since(before);
    check(seen == Counts {},
        "the deprecated interruption notification does nothing", seen.text());
}

} // namespace

int main()
{
    @autoreleasepool {
        installHooks();
        startsAndStops();
        deactivationsThatInterruptNothing();
        interruptionStopsTheMicrophone();
        adviceToResumeStartsIt();
        noAdviceWhileAnotherAppPlays();
        noAdviceWhileNoAppPlays();
        guestRestartsRecording();
        stoppedAndDestroyedMicrophones();
        deprecatedNotificationIsIgnored();
    }
    std::printf("%s: %d failure(s)\n", failures == 0 ? "PASS" : "FAIL",
        failures);
    return failures == 0 ? 0 : 1;
}
