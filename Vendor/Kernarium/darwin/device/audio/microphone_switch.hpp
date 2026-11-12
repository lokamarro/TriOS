// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "period_clock.hpp"
#include "platform/microphone_authorization.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace uml::audio {

/* The user's Microphone setting and the system's permission behind it.
 *
 * A guest records from the real microphone only while the switch is on and
 * the system has granted access. The switch is off until the user turns it
 * on, and nothing here reads, opens or asks about the microphone until then:
 * enable() is the one call that can show the system's permission prompt, and
 * it belongs to the user's act of turning the setting on. restore() carries an
 * earlier choice into a new run and never asks.
 *
 * Own it with std::make_shared. Any thread may call any member. */
class MicrophoneSwitch final
    : public std::enable_shared_from_this<MicrophoneSwitch> {
public:
    enum class State : std::uint8_t {
        // The user has not turned it on.
        off,
        // On, but the system has not been asked yet (an earlier choice was
        // restored; the user turns the setting off and on to be asked).
        unasked,
        // Waiting for the user's answer to the system prompt.
        asking,
        // On, but the system refuses access. The guest records silence.
        denied,
        // On and permitted: a recording guest hears the microphone.
        on,
    };

    explicit MicrophoneSwitch(
        std::shared_ptr<platform::MicrophoneAuthorization> authorization);

    /* The user turned the setting on. Asks the system if it has never been
     * asked, and then runs `answered`, if given, with the state the answer
     * leaves, on whichever thread the system answers on. */
    State enable(std::function<void(State)> answered = {});
    /* An earlier "on" carried into this run; asks nothing. */
    State restore();
    void disable();

    [[nodiscard]] State state() const;
    /* Whether captured audio may flow: on, and permitted right now. */
    [[nodiscard]] bool active() const;

    /* The capture source's wake-up for a change. */
    void setWaker(std::shared_ptr<const Waker> waker);
    void clearWaker(const Waker* expected);

private:
    void changed();

    std::shared_ptr<platform::MicrophoneAuthorization> authorization_;
    mutable std::mutex lock_;
    std::shared_ptr<const Waker> waker_;
    bool wanted_ {};
    bool asking_ {};
};

[[nodiscard]] const char* describe(MicrophoneSwitch::State state) noexcept;

} // namespace uml::audio
