// SPDX-License-Identifier: GPL-2.0-or-later

#include "period_clock.hpp"

#include "platform/descriptor.hpp"

#include <sys/event.h>
#include <sys/types.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace uml::audio {
namespace {

constexpr std::uintptr_t timerIdentifier = 1;

class KqueuePeriodClock final : public PeriodClock {
public:
    KqueuePeriodClock()
        : wake_("audio readiness")
    {
        queue_.reset(::kqueue());
        if (queue_.get() < 0) {
            const int error = errno;
            throw std::runtime_error(
                std::string("creating the audio event queue failed: ") +
                std::strerror(error));
        }
        uml::platform::configureDescriptorOrThrow(
            queue_.get(), {.closeOnExec = true}, "audio event queue");
        struct kevent change {};
        EV_SET(
            &change, static_cast<std::uintptr_t>(wake_.descriptor()),
            EVFILT_READ, EV_ADD, 0, 0, nullptr);
        if (::kevent(queue_.get(), &change, 1, nullptr, 0, nullptr) != 0) {
            const int error = errno;
            throw std::runtime_error(
                std::string("watching the audio wake socket failed: ") +
                std::strerror(error));
        }
    }

    [[nodiscard]] ClockTime now() const noexcept override
    {
        return std::chrono::steady_clock::now();
    }

    void notify() const noexcept override { (void)wake_.notify(); }

    void wakeAt(ClockTime deadline) noexcept override
    {
        const auto remaining = std::chrono::duration_cast<
            std::chrono::nanoseconds>(deadline - now());
        // A kqueue timer of zero length never fires, so ask for one
        // nanosecond when the deadline has already passed.
        const auto nanoseconds = remaining.count() > 0 ? remaining.count() : 1;
        struct kevent change {};
        EV_SET(
            &change, timerIdentifier, EVFILT_TIMER,
            EV_ADD | EV_ONESHOT, NOTE_NSECONDS | NOTE_CRITICAL,
            static_cast<intptr_t>(nanoseconds), nullptr);
        (void)::kevent(queue_.get(), &change, 1, nullptr, 0, nullptr);
    }

    void cancel() noexcept override
    {
        struct kevent change {};
        EV_SET(
            &change, timerIdentifier, EVFILT_TIMER, EV_DELETE, 0, 0, nullptr);
        // The timer may have fired and been consumed already.
        (void)::kevent(queue_.get(), &change, 1, nullptr, 0, nullptr);
    }

    [[nodiscard]] int descriptor() const noexcept override
    {
        return queue_.get();
    }

    void drain() noexcept override
    {
        (void)wake_.drain();
        // The wake socket reads ready until drained and the timer is
        // retrieved here; a retrieved event of either kind is never kept.
        std::array<struct kevent, 4> events {};
        const timespec immediately {};
        while (::kevent(
                   queue_.get(), nullptr, 0, events.data(),
                   static_cast<int>(events.size()), &immediately) ==
               static_cast<int>(events.size())) {
        }
    }

private:
    uml::platform::UniqueDescriptor queue_;
    uml::platform::WakeSocket wake_;
};

} // namespace

std::shared_ptr<PeriodClock> makePeriodClock()
{
    return std::make_shared<KqueuePeriodClock>();
}

} // namespace uml::audio
