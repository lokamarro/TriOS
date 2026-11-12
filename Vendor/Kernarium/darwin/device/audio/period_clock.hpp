// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <memory>

namespace uml::audio {

using ClockTime = std::chrono::steady_clock::time_point;

/* Signals the device's event descriptor from any thread. The playback sink
 * calls it from audio-engine callbacks, so it must not allocate or lock. */
class Waker {
public:
    virtual ~Waker() = default;
    virtual void notify() const noexcept = 0;
};

/* The time base of a Virtio Sound device and the one descriptor its
 * transport polls.
 *
 * The device paces its capture stream and any playback it must discard by
 * this clock alone, so a guest sees the same steady device whatever the
 * host's audio hardware does. The descriptor becomes readable when notify()
 * is called or when the deadline given to wakeAt() has passed; drain() takes
 * both signals back. A wake may be early, late or spurious: the device
 * recomputes what is due from now() every time it runs.
 *
 * Only the transport worker calls everything but notify(). A test injects a
 * clock whose time it advances by hand. */
class PeriodClock : public Waker {
public:
    [[nodiscard]] virtual ClockTime now() const noexcept = 0;
    /* Makes the descriptor readable at `deadline`, or at once if it has
     * passed, replacing any earlier deadline. */
    virtual void wakeAt(ClockTime deadline) noexcept = 0;
    /* Withdraws the deadline. */
    virtual void cancel() noexcept = 0;
    [[nodiscard]] virtual int descriptor() const noexcept = 0;
    virtual void drain() noexcept = 0;
};

/* The production clock: steady_clock time and a kqueue that watches a wake
 * socket and one timer, so the transport worker serves the deadline from its
 * own poll and no thread is added. */
[[nodiscard]] std::shared_ptr<PeriodClock> makePeriodClock();

} // namespace uml::audio
