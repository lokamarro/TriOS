// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>

namespace uml::gpu {

/* What to do with a deferred fence the renderer has not retired yet.
 *
 * A renderer fence can stay busy because the guest command carrying the
 * signal it waits for is itself held back by fence ordering. Dropping the
 * connection resolves nothing there: it removes the GPU device from a guest
 * that is still able to finish its workload and power off, and turns one
 * wedged submission into a dead session. The device therefore escapes the
 * deadlock first and only then gives up on the renderer, so a stuck fence
 * costs the workload that owns it rather than every workload sharing the
 * session. */
enum class OverdueFenceAction {
    /* Still inside its window. */
    wait,
    /* Release queue ordering so a held-back signal can reach the renderer,
     * and grant the fence a second window. */
    releaseOrdering,
    /* The second window expired: complete the fence without its renderer
     * work and keep serving the connection. */
    retire,
};

[[nodiscard]] constexpr OverdueFenceAction classifyOverdueFence(
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point deadline,
    bool orderingAlreadyReleased) noexcept
{
    if (now < deadline)
        return OverdueFenceAction::wait;
    return orderingAlreadyReleased
        ? OverdueFenceAction::retire
        : OverdueFenceAction::releaseOrdering;
}

/* How many fences in a row one ring may complete without the renderer before
 * the ring is no longer carrying a stuck submission but is simply not
 * retiring anything.
 *
 * The escape above pays two windows for each fence, so it answers a workload
 * that wedges once. A ring that needs it again for the fence after, and the
 * one after that, has had six windows and three ordering releases without the
 * renderer moving, and nothing about the fourth fence is different. Waiting
 * for it buys the same nothing at the same price, for as long as the guest
 * keeps submitting: a Vulkan CTS fraction spent 2,700 seconds paying thirty
 * per test case and said nothing at all.
 *
 * Three is the smallest count that cannot be reached by one wedged submission
 * and the ordering release meant to free it. */
inline constexpr unsigned maximumRingEscapes = 3;

/* Whether a ring has stopped retiring rather than stumbled over one fence.
 * A ring that dies is reported once and then costs nothing per fence; any
 * retirement the renderer does make clears the count and revives it. */
[[nodiscard]] constexpr bool ringHasStoppedRetiring(
    unsigned consecutiveEscapes) noexcept
{
    return consecutiveEscapes >= maximumRingEscapes;
}

} // namespace uml::gpu
