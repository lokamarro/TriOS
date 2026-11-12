// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <memory>

namespace uml::platform {

enum class HostBatteryStatus : std::uint8_t {
    unknown,
    charging,
    discharging,
    notCharging,
    full,
};

struct HostBatteryState final {
    std::uint8_t percentage {100};
    HostBatteryStatus status {HostBatteryStatus::charging};
    bool present {true};
    bool externalPower {true};
};

class HostBatteryProvider {
public:
    virtual ~HostBatteryProvider() = default;

    [[nodiscard]] virtual HostBatteryState state() noexcept = 0;
};

[[nodiscard]] std::unique_ptr<HostBatteryProvider>
makeAppleHostBatteryProvider();

} // namespace uml::platform
