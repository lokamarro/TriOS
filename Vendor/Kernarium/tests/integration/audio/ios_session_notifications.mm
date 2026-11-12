// SPDX-License-Identifier: GPL-2.0-or-later
//
// The audio-session notifications iOS 27 posts when another app takes the
// session and gives it back, for the Simulator tests of the Apple sink and
// the Apple microphone. The Simulator never interrupts a session, so the tests
// post what an interruption would.

#include "ios_session_notifications.hpp"

#import <Foundation/Foundation.h>
#import <objc/runtime.h>

// iOS creates the contexts its notifications carry and offers no initializer,
// so each is a subclass that answers the reads the sink and the
// microphone make.
@interface InterruptionContextDouble : AVAudioSessionInterruptionContext
@end

@implementation InterruptionContextDouble
- (AVAudioSessionInterruptionReason)reason
{
    return AVAudioSessionInterruptionReasonDefault;
}
@end

@interface DeactivationContextDouble : AVAudioSessionDeactivationContext
@property(nonatomic) AVAudioSessionDeactivationSource sourceDouble;
@property(nonatomic, strong) AVAudioSessionInterruptionContext* interruptionDouble;
@end

@implementation DeactivationContextDouble
- (AVAudioSessionDeactivationSource)source
{
    return self.sourceDouble;
}

- (AVAudioSessionInterruptionContext*)interruptionContext
{
    return self.interruptionDouble;
}
@end

@interface ResumptionContextDouble : AVAudioSessionResumptionContext
@property(nonatomic) AVAudioSessionResumptionRecommendation recommendationDouble;
@end

@implementation ResumptionContextDouble
- (AVAudioSessionResumptionRecommendation)recommendation
{
    return self.recommendationDouble;
}
@end

namespace {

template <class Context> Context* make()
{
    return static_cast<Context*>(class_createInstance([Context class], 0));
}

} // namespace

void postDeactivation(
    AVAudioSessionDeactivationSource source, bool interrupted)
{
    auto* context = make<DeactivationContextDouble>();
    context.sourceDouble = source;
    context.interruptionDouble =
        interrupted ? make<InterruptionContextDouble>() : nil;
    [[NSNotificationCenter defaultCenter]
        postNotificationName:AVAudioSessionDidBecomeInactiveNotification
                      object:[AVAudioSession sharedInstance]
                    userInfo:@{
                        AVAudioSessionDeactivationContextKey : context
                    }];
}

void postInterruptionBegan()
{
    postDeactivation(AVAudioSessionDeactivationSourceSystem, true);
}

void postInterruptionEnded(bool shouldResume)
{
    auto* context = make<ResumptionContextDouble>();
    context.recommendationDouble = shouldResume ?
        AVAudioSessionResumptionRecommendationShouldResume :
        AVAudioSessionResumptionRecommendationShouldNotResume;
    [[NSNotificationCenter defaultCenter]
        postNotificationName:AVAudioSessionResumptionRecommendationNotification
                      object:[AVAudioSession sharedInstance]
                    userInfo:@{
                        AVAudioSessionResumptionContextKey : context
                    }];
}
