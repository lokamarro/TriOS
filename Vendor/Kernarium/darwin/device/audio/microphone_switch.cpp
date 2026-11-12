// SPDX-License-Identifier: GPL-2.0-or-later

#include "microphone_switch.hpp"

#include <stdexcept>
#include <utility>

namespace uml::audio {

using platform::MicrophonePermission;

MicrophoneSwitch::MicrophoneSwitch(
    std::shared_ptr<platform::MicrophoneAuthorization> authorization)
    : authorization_(std::move(authorization))
{
    if (!authorization_)
        throw std::invalid_argument("the microphone switch needs an authorization");
}

MicrophoneSwitch::State MicrophoneSwitch::enable(
    std::function<void(State)> answered)
{
    {
        const std::lock_guard lock(lock_);
        wanted_ = true;
    }
    if (authorization_->permission() == MicrophonePermission::undetermined) {
        bool alreadyAsking {};
        {
            // One question at a time: the user answers the prompt on screen.
            const std::lock_guard lock(lock_);
            alreadyAsking = std::exchange(asking_, true);
        }
        if (alreadyAsking) {
            changed();
            return state();
        }
        // The answer can arrive before request() returns, or on any thread.
        authorization_->request(
            [weak = weak_from_this(), answered = std::move(answered)](
                MicrophonePermission) {
                if (const auto self = weak.lock()) {
                    {
                        const std::lock_guard lock(self->lock_);
                        self->asking_ = false;
                    }
                    self->changed();
                    if (answered)
                        answered(self->state());
                }
            });
    }
    changed();
    return state();
}

MicrophoneSwitch::State MicrophoneSwitch::restore()
{
    {
        const std::lock_guard lock(lock_);
        wanted_ = true;
    }
    // Asks nothing, but wakes the source: a recording that could not reach
    // the microphone in the background tries again when the app returns.
    changed();
    return state();
}

void MicrophoneSwitch::disable()
{
    {
        const std::lock_guard lock(lock_);
        wanted_ = false;
    }
    changed();
}

MicrophoneSwitch::State MicrophoneSwitch::state() const
{
    {
        // Off asks the system nothing.
        const std::lock_guard lock(lock_);
        if (!wanted_)
            return State::off;
    }
    const auto permission = authorization_->permission();
    const std::lock_guard lock(lock_);
    if (!wanted_)
        return State::off;
    switch (permission) {
    case MicrophonePermission::granted:
        return State::on;
    case MicrophonePermission::denied:
        return State::denied;
    case MicrophonePermission::undetermined:
        break;
    }
    return asking_ ? State::asking : State::unasked;
}

bool MicrophoneSwitch::active() const
{
    {
        const std::lock_guard lock(lock_);
        if (!wanted_)
            return false;
    }
    return authorization_->permission() == MicrophonePermission::granted;
}

void MicrophoneSwitch::setWaker(std::shared_ptr<const Waker> waker)
{
    const std::lock_guard lock(lock_);
    waker_ = std::move(waker);
}

void MicrophoneSwitch::clearWaker(const Waker* expected)
{
    const std::lock_guard lock(lock_);
    if (waker_.get() == expected)
        waker_.reset();
}

void MicrophoneSwitch::changed()
{
    std::shared_ptr<const Waker> waker;
    {
        const std::lock_guard lock(lock_);
        waker = waker_;
    }
    if (waker)
        waker->notify();
}

const char* describe(MicrophoneSwitch::State state) noexcept
{
    switch (state) {
    case MicrophoneSwitch::State::off:
        return "off";
    case MicrophoneSwitch::State::unasked:
        return "on, but the system has not been asked for access";
    case MicrophoneSwitch::State::asking:
        return "waiting for the system's permission";
    case MicrophoneSwitch::State::denied:
        return "denied by the system; the guest records silence";
    case MicrophoneSwitch::State::on:
        return "on";
    }
    return "unknown";
}

} // namespace uml::audio
