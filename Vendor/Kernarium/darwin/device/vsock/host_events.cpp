// SPDX-License-Identifier: GPL-2.0-or-later

#include "host_events.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

namespace uml::vsock {
namespace {

constexpr std::uintptr_t wakeIdentifier = 1;

} // namespace

HostEvents::HostEvents()
    : queue_(::kqueue())
{
    if (queue_.get() < 0)
        throw std::runtime_error("creating the Virtio vsock event queue failed");
    struct kevent wake {};
    EV_SET(&wake, wakeIdentifier, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (::kevent(queue_.get(), &wake, 1, nullptr, 0, nullptr) != 0)
        throw std::runtime_error("registering the Virtio vsock wake event failed");
}

void HostEvents::trigger() noexcept
{
    struct kevent wake {};
    EV_SET(&wake, wakeIdentifier, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    /* Nothing to do about a failure: the poller then runs on its next event
     * or kick, and the descriptor is only closed with the device. */
    (void)::kevent(queue_.get(), &wake, 1, nullptr, 0, nullptr);
}

void HostEvents::change(
    int descriptor,
    std::uint64_t id,
    std::int16_t filter,
    std::uint16_t flags)
{
    struct kevent request {};
    EV_SET(
        &request,
        static_cast<std::uintptr_t>(descriptor),
        filter,
        flags,
        0,
        0,
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(id)));
    /* No event list: a change must not collect a readiness, and in
     * particular not the wake, which only the poller may consume. */
    while (::kevent(queue_.get(), &request, 1, nullptr, 0, nullptr) != 0) {
        if (errno == EINTR)
            continue;
        /* A descriptor closed before the change reports EBADF or ENOENT,
         * which is the same as the filter being gone. */
        if (errno == EBADF || errno == ENOENT)
            return;
        throw std::runtime_error(
            std::string("changing a Virtio vsock event failed: ") +
            std::strerror(errno));
    }
}

void HostEvents::watch(int descriptor, std::uint64_t id)
{
    change(descriptor, id, EVFILT_READ, EV_ADD | EV_DISABLE | EV_DISPATCH);
    change(descriptor, id, EVFILT_WRITE, EV_ADD | EV_DISABLE | EV_DISPATCH);
}

void HostEvents::setReadEnabled(int descriptor, std::uint64_t id, bool enabled)
{
    change(descriptor, id, EVFILT_READ, enabled ? EV_ENABLE : EV_DISABLE);
}

void HostEvents::setWriteEnabled(int descriptor, std::uint64_t id, bool enabled)
{
    change(descriptor, id, EVFILT_WRITE, enabled ? EV_ENABLE : EV_DISABLE);
}

void HostEvents::collect(std::vector<Event>& events)
{
    constexpr timespec noWait {};
    std::array<struct kevent, 64> raw {};
    for (;;) {
        const int count = ::kevent(
            queue_.get(),
            nullptr,
            0,
            raw.data(),
            static_cast<int>(raw.size()),
            &noWait);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error("draining Virtio vsock events failed");
        }
        for (int index = 0; index < count; ++index) {
            const auto& event = raw[static_cast<std::size_t>(index)];
            if ((event.flags & EV_ERROR) != 0)
                continue;
            const auto id =
                static_cast<std::uint64_t>(
                    reinterpret_cast<std::uintptr_t>(event.udata));
            switch (event.filter) {
            case EVFILT_USER:
                events.push_back({Kind::wake, 0});
                break;
            case EVFILT_READ:
                events.push_back({Kind::readable, id});
                break;
            case EVFILT_WRITE:
                events.push_back({Kind::writable, id});
                break;
            default:
                break;
            }
        }
        if (count < static_cast<int>(raw.size()))
            break;
    }
}

} // namespace uml::vsock
