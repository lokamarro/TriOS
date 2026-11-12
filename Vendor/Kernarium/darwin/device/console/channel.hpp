// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace uml::console {

class ConsoleChannel {
public:
    virtual ~ConsoleChannel() = default;

    [[nodiscard]] virtual std::vector<std::uint8_t> read(
        std::size_t maximumBytes) = 0;
    virtual void write(std::span<const std::uint8_t> bytes) noexcept = 0;
    // Whether output the channel took earlier still waits for its peer to
    // make room. It sends what it can first, so false means the channel takes
    // more. While output waits the device leaves the guest's output queued,
    // and the channel's event descriptor reports writable once there is
    // room. A channel that never keeps output back keeps this default.
    [[nodiscard]] virtual bool outputWaiting() noexcept { return false; }
    [[nodiscard]] virtual int eventDescriptor() const noexcept = 0;
    virtual void drainEventSource() = 0;
    virtual void reset() noexcept = 0;
};

} // namespace uml::console
