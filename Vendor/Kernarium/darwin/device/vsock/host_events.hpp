// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "unique_descriptor.hpp"

#include <cstdint>
#include <vector>

namespace uml::vsock {

/* The readiness of the host ends of the connections, as one kqueue the
 * vhost-user transport polls (VhostUserDevice::eventDescriptor). Each host
 * descriptor has a read filter and a write filter, both dispatched: a filter
 * that fired is disabled until the device enables it again, so the device
 * sees a readiness once, can decline to act on it while the guest has no
 * credit or no buffer, and is not woken again until it asks. A filter only
 * fires while enabled and the condition holds, which is level-triggered, so
 * data that arrived while it was disabled is reported when it is enabled.
 *
 * Everything but trigger() belongs to the device's thread. */
class HostEvents final {
public:
    enum class Kind : std::uint8_t {
        wake,
        readable,
        writable,
    };

    struct Event final {
        Kind kind {};
        std::uint64_t id {};
    };

    /* Throws std::runtime_error when the kqueue cannot be made. */
    HostEvents();

    [[nodiscard]] int descriptor() const noexcept { return queue_.get(); }

    /* Wakes the poller from any thread. */
    void trigger() noexcept;

    /* Registers both filters of `descriptor` for connection `id`, disabled.
     * Closing the descriptor drops them. */
    void watch(int descriptor, std::uint64_t id);
    void setReadEnabled(int descriptor, std::uint64_t id, bool enabled);
    void setWriteEnabled(int descriptor, std::uint64_t id, bool enabled);

    /* Appends everything that is ready now and clears the wake. Never
     * blocks. */
    void collect(std::vector<Event>& events);

private:
    void change(
        int descriptor,
        std::uint64_t id,
        std::int16_t filter,
        std::uint16_t flags);

    uml::gpu::UniqueDescriptor queue_;
};

} // namespace uml::vsock
