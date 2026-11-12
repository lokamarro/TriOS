// SPDX-License-Identifier: GPL-2.0-or-later

#include "in_process_service.hpp"

#include "capture_source.hpp"
#include "microphone_capture_source.hpp"
#include "period_clock.hpp"
#include "playback_sink.hpp"
#include "vhost_user_server.hpp"
#include "virtio_sound_device.hpp"

namespace uml::audio {

std::unique_ptr<uml::gpu::InProcessVhostUserService> makeInProcessService(
    const ServiceOptions& options)
{
    return std::make_unique<uml::gpu::InProcessVhostUserService>(
        "Virtio Sound", "uml-audio", [options] {
            auto clock = makePeriodClock();
            auto sink = options.output == OutputMode::null
                ? makeNullPlaybackSink()
                : makeApplePlaybackSink(clock);
            auto capture = options.microphone
                ? makeMicrophoneCaptureSource(
                      options.microphone, makeAppleMicrophoneInput(), clock)
                : makeSilentCaptureSource();
            return std::make_unique<VirtioSoundDevice>(
                std::move(sink), std::move(capture), std::move(clock));
        });
}

} // namespace uml::audio
