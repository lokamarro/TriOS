// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "host_battery.hpp"
#include "vhost_user_device.hpp"

#include <memory>

namespace uml::battery {

class VirtioBatteryDevice final : public uml::virtio::VhostUserDevice {
public:
    explicit VirtioBatteryDevice(
        std::unique_ptr<uml::platform::HostBatteryProvider> provider);

    [[nodiscard]] const char* name() const noexcept override;
    [[nodiscard]] std::uint64_t features() const noexcept override;
    [[nodiscard]] std::size_t queueCount() const noexcept override;
    [[nodiscard]] std::vector<std::uint8_t> configuration() const override;
    void writeConfiguration(
        std::uint32_t offset,
        std::uint32_t flags,
        std::span<const std::uint8_t> bytes) override;
    [[nodiscard]] uml::virtio::VhostUserQueueResult processQueue(
        std::uint32_t index,
        std::uint64_t requestId,
        std::span<const std::uint8_t> request,
        std::size_t responseCapacity) override;
    void reset() noexcept override;

private:
    std::unique_ptr<uml::platform::HostBatteryProvider> provider_;
};

} // namespace uml::battery
