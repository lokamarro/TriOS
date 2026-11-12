// SPDX-License-Identifier: GPL-2.0-or-later

#include "audio_session.hpp"

#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>
#import <TargetConditionals.h>

#include <mutex>
#include <utility>

namespace uml::audio::session {

#if TARGET_OS_IPHONE

namespace {

std::mutex lock;
bool playbackNeeds {};
bool captureNeeds {};
double playbackCycle {};
double captureCycle {};

/* The shortest IO cycle a client that needs the session asks for, or 0. */
double preferredCycle() noexcept
{
    double cycle = 0.0;
    for (const auto [needs, asked] :
         {std::pair {playbackNeeds, playbackCycle},
          std::pair {captureNeeds, captureCycle}}) {
        if (needs && asked > 0.0 && (cycle == 0.0 || asked < cycle))
            cycle = asked;
    }
    return cycle;
}

/* Sets the category the current clients need and activates the session. */
bool apply() noexcept
{
    @try {
        AVAudioSession* session = [AVAudioSession sharedInstance];
        NSError* error = nil;
        const bool recording = captureNeeds;
        if (![session setCategory:recording ? AVAudioSessionCategoryPlayAndRecord
                                            : AVAudioSessionCategoryPlayback
                             mode:AVAudioSessionModeDefault
                          options:recording
                              ? (AVAudioSessionCategoryOptionMixWithOthers |
                                 AVAudioSessionCategoryOptionDefaultToSpeaker |
                                 AVAudioSessionCategoryOptionAllowBluetoothA2DP)
                              : 0
                            error:&error]) {
            return false;
        }
        // Only a preference: a session that keeps another cycle still plays.
        if (const double cycle = preferredCycle(); cycle > 0.0)
            (void)[session setPreferredIOBufferDuration:cycle error:&error];
        return [session setActive:YES error:&error];
    } @catch (NSException*) {
        return false;
    }
}

} // namespace

bool otherAudioPlaying() noexcept
{
    @try {
        return [AVAudioSession sharedInstance].secondaryAudioShouldBeSilencedHint;
    } @catch (NSException*) {
        return false;
    }
}

bool acquire(Client client) noexcept
{
    const std::lock_guard guard(lock);
    bool& needs = client == Client::capture ? captureNeeds : playbackNeeds;
    const bool before = needs;
    needs = true;
    if (apply())
        return true;
    needs = before;
    return false;
}

void release(Client client) noexcept
{
    const std::lock_guard guard(lock);
    (client == Client::capture ? captureNeeds : playbackNeeds) = false;
    if (playbackNeeds || captureNeeds) {
        (void)apply();
        return;
    }
    @try {
        NSError* error = nil;
        [[AVAudioSession sharedInstance]
            setActive:NO
          withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                error:&error];
    } @catch (NSException*) {
    }
}

void preferIoCycle(Client client, double seconds) noexcept
{
    const std::lock_guard guard(lock);
    (client == Client::capture ? captureCycle : playbackCycle) = seconds;
}

#else

bool otherAudioPlaying() noexcept
{
    return false;
}

void preferIoCycle(Client, double) noexcept {}

bool acquire(Client) noexcept
{
    return true;
}

void release(Client) noexcept {}

#endif

} // namespace uml::audio::session
