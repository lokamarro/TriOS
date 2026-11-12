// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "capture_ring.hpp"
#include "capture_source.hpp"
#include "microphone_switch.hpp"
#include "period_clock.hpp"

#include <cstdint>
#include <memory>

namespace uml::audio {

/* The part of microphone capture that touches the system. */
class MicrophoneInput {
public:
    virtual ~MicrophoneInput() = default;

    /* Starts the microphone and delivers its audio, as interleaved 48 kHz
     * signed 16-bit frames of `channels` channels, to `ring.write()` from an
     * audio thread of its own. Returns false if the microphone could not be
     * started; nothing is left running then. */
    [[nodiscard]] virtual bool start(
        std::uint8_t channels,
        CaptureRing& ring) noexcept = 0;
    /* Stops the microphone, so that the system's recording indicator goes
     * out. No delivery is running when it returns. */
    virtual void stop() noexcept = 0;
};

/* A capture source that records from the real microphone while the guest
 * records and the user's `MicrophoneSwitch` is on and permitted, and delivers
 * nothing otherwise, so the guest records silence. The input is started only
 * then, and stopped when the guest stops, the switch turns off, or the device
 * resets. The switch's wake-up reaches the transport worker through `waker`,
 * which calls handleEvents(). */
[[nodiscard]] std::unique_ptr<CaptureSource> makeMicrophoneCaptureSource(
    std::shared_ptr<MicrophoneSwitch> microphone,
    std::unique_ptr<MicrophoneInput> input,
    std::shared_ptr<const Waker> waker);

/* The same over the Apple microphone (AVAudioEngine's input node). */
[[nodiscard]] std::unique_ptr<MicrophoneInput> makeAppleMicrophoneInput();

} // namespace uml::audio
