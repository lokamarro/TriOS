// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace uml::console::protocol {

inline constexpr std::uint32_t inputQueue = 0;
inline constexpr std::uint32_t outputQueue = 1;
inline constexpr std::size_t queueCount = 2;
inline constexpr std::uint32_t controlInputQueue = 2;
inline constexpr std::uint32_t controlOutputQueue = 3;
inline constexpr std::size_t namedSerialQueueCount = 4;
inline constexpr std::uint32_t deviceId = 3;
inline constexpr std::uint64_t featureMultiport = 1ULL << 1U;
inline constexpr std::uint32_t badPortId = ~std::uint32_t {};
inline constexpr std::uint32_t hostSerialPortId = 0;
inline constexpr std::string_view hostSerialPortName {
    "org.uml.host-serial.0"};

enum class ControlEvent : std::uint16_t {
    deviceReady = 0,
    portAdd = 1,
    portRemove = 2,
    portReady = 3,
    consolePort = 4,
    resize = 5,
    portOpen = 6,
    portName = 7,
};

struct Configuration final {
    std::uint16_t columns {};
    std::uint16_t rows {};
    std::uint32_t maximumPorts {};
    std::uint32_t emergencyWrite {};
};

static_assert(sizeof(Configuration) == 12);

struct [[gnu::packed]] Control final {
    std::uint32_t id {};
    std::uint16_t event {};
    std::uint16_t value {};
};

static_assert(sizeof(Control) == 8);

} // namespace uml::console::protocol
