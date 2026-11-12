// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace uml::battery::protocol {

// The Virtio specification reserves 65535 for local experimental devices.
inline constexpr std::uint32_t deviceId = 65535;
inline constexpr std::uint32_t version = 1;
inline constexpr std::uint32_t statusQueue = 0;
inline constexpr std::size_t queueCount = 1;

enum class ChargeStatus : std::uint8_t {
    unknown,
    charging,
    discharging,
    notCharging,
    full,
};

struct [[gnu::packed]] Request final {
    std::uint32_t version;
};

struct [[gnu::packed]] State final {
    std::uint32_t version;
    std::uint8_t percentage;
    ChargeStatus status;
    std::uint8_t present;
    std::uint8_t externalPower;
};

static_assert(sizeof(Request) == 4);
static_assert(sizeof(State) == 8);

} // namespace uml::battery::protocol
