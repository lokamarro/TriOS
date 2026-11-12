// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "display_geometry.hpp"
#include "platform/descriptor.hpp"
#include "protocol.hpp"

#include <atomic>
#include <cstdint>

namespace uml::gpu {

/* Apple UI callbacks publish only the newest usable geometry here. The GPU
 * transport thread drains the readiness descriptor and is the sole consumer
 * which mutates renderer or Virtio-GPU state. */
class DisplayResizeNotifier final {
public:
    static constexpr std::uint32_t minimumDimension = 32;

    [[nodiscard]] static std::uint32_t clampDimension(
        std::uint32_t dimension,
        std::uint32_t maximumDimension) noexcept;

    DisplayResizeNotifier(
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t maximumDimension =
            protocol::maximumDisplayDimension);

    DisplayResizeNotifier(const DisplayResizeNotifier&) = delete;
    DisplayResizeNotifier& operator=(const DisplayResizeNotifier&) = delete;

    [[nodiscard]] int descriptor() const noexcept;
    [[nodiscard]] std::uint32_t maximumDimension() const noexcept;
    void request(std::uint32_t width, std::uint32_t height) noexcept;
    void requestCommitted(std::uint32_t width, std::uint32_t height) noexcept;
    [[nodiscard]] DisplaySize drain() noexcept;

private:
    void publish(
        std::uint32_t width,
        std::uint32_t height,
        bool ensureNotification) noexcept;

    uml::platform::WakeSocket wake_;
    std::uint32_t maximumDimension_;
    std::atomic_uint64_t requested_;
};

} // namespace uml::gpu
