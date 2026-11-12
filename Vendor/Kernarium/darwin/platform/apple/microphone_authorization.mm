// SPDX-License-Identifier: GPL-2.0-or-later

#include "microphone_authorization.hpp"

#import <AVFAudio/AVFAudio.h>
#import <Foundation/Foundation.h>

#include <memory>
#include <utility>

namespace uml::platform {
namespace {

MicrophonePermission current() noexcept
{
    @try {
        switch (AVAudioApplication.sharedInstance.recordPermission) {
        case AVAudioApplicationRecordPermissionGranted:
            return MicrophonePermission::granted;
        case AVAudioApplicationRecordPermissionUndetermined:
            return MicrophonePermission::undetermined;
        default:
            return MicrophonePermission::denied;
        }
    } @catch (NSException*) {
        return MicrophonePermission::denied;
    }
}

class AppleMicrophoneAuthorization final : public MicrophoneAuthorization {
public:
    [[nodiscard]] MicrophonePermission permission() const noexcept override
    {
        return current();
    }

    void request(std::function<void(MicrophonePermission)> completion) override
    {
        // An answer that is already given is never asked for again, so no
        // prompt can appear for a setting that was turned on before.
        if (const auto answered = current();
            answered != MicrophonePermission::undetermined) {
            completion(answered);
            return;
        }
        const auto shared = std::make_shared<
            std::function<void(MicrophonePermission)>>(std::move(completion));
        [AVAudioApplication requestRecordPermissionWithCompletionHandler:^(
                                BOOL granted) {
            (*shared)(granted ? MicrophonePermission::granted
                              : MicrophonePermission::denied);
        }];
    }
};

} // namespace

std::shared_ptr<MicrophoneAuthorization> makeAppleMicrophoneAuthorization()
{
    return std::make_shared<AppleMicrophoneAuthorization>();
}

} // namespace uml::platform
