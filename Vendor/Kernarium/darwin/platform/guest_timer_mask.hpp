// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <pthread.h>
#include <signal.h>

#include <system_error>

namespace uml::platform {

/* Blocks SIGALRM on the calling thread until the end of the scope.
 *
 * ITIMER_REAL is UML's guest clock and must reach only the guest's own CPU
 * threads. Darwin may deliver it to any thread that leaves it unblocked, which
 * would enter the Linux scheduler on a foreign host stack. A thread created
 * inside the scope inherits the blocked mask, so host service threads are
 * started inside one. Throws std::system_error, naming `purpose`, when the
 * mask cannot be changed.
 */
class ScopedGuestTimerBlock final {
public:
    explicit ScopedGuestTimerBlock(const char* purpose)
    {
        sigset_t blocked;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGALRM);
        const int error = ::pthread_sigmask(SIG_BLOCK, &blocked, &previous_);
        if (error != 0)
            throw std::system_error(error, std::generic_category(), purpose);
    }

    ~ScopedGuestTimerBlock()
    {
        (void)::pthread_sigmask(SIG_SETMASK, &previous_, nullptr);
    }

    ScopedGuestTimerBlock(const ScopedGuestTimerBlock&) = delete;
    ScopedGuestTimerBlock& operator=(const ScopedGuestTimerBlock&) = delete;

private:
    sigset_t previous_ {};
};

} // namespace uml::platform
