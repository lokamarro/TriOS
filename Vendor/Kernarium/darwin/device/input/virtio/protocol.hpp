// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::input::virtio::protocol {

// Device ID 18 is VIRTIO_ID_INPUT in the stable Virtio device registry.
inline constexpr std::uint32_t deviceId = 18;
inline constexpr std::size_t queueCount = 2;
inline constexpr std::uint32_t eventQueue = 0;
inline constexpr std::uint32_t statusQueue = 1;

inline constexpr std::uint8_t configUnset = 0x00;
inline constexpr std::uint8_t configIdName = 0x01;
inline constexpr std::uint8_t configIdSerial = 0x02;
inline constexpr std::uint8_t configIdDeviceIds = 0x03;
inline constexpr std::uint8_t configPropertyBits = 0x10;
inline constexpr std::uint8_t configEventBits = 0x11;
inline constexpr std::uint8_t configAbsoluteInfo = 0x12;

inline constexpr std::uint16_t eventSynchronize = 0x00;
inline constexpr std::uint16_t eventKey = 0x01;
inline constexpr std::uint16_t eventAbsolute = 0x03;
inline constexpr std::uint16_t eventLed = 0x11;
inline constexpr std::uint16_t synchronizeReport = 0x00;
inline constexpr std::uint16_t absoluteX = 0x00;
inline constexpr std::uint16_t absoluteY = 0x01;
inline constexpr std::uint16_t absoluteMtSlot = 0x2f;
inline constexpr std::uint16_t absoluteMtPositionX = 0x35;
inline constexpr std::uint16_t absoluteMtPositionY = 0x36;
inline constexpr std::uint16_t absoluteMtToolType = 0x37;
inline constexpr std::uint16_t absoluteMtTrackingId = 0x39;
/* ABS_MT_TOOL_TYPE values (MT_TOOL_FINGER, MT_TOOL_PEN, MT_TOOL_PALM). The
 * touchscreen sends a finger and, for a cancelled contact, a palm, which the
 * guest's input stack reads as a contact to disregard rather than lift. */
inline constexpr std::int32_t toolTypeFinger = 0;
inline constexpr std::int32_t toolTypePalm = 2;
inline constexpr std::uint32_t touchscreenContactCount = 10;
inline constexpr std::int32_t touchscreenTrackingIdMaximum = 65535;
inline constexpr std::uint16_t buttonTouch = 0x14a;
/* The touchscreen's absolute range is 0..touchscreenAbsoluteMaximum on both
 * axes, the VNC touchscreen's range too. It does not depend on the display, so
 * the guest's probe-time ABS_X/Y limits stay valid across every later mode
 * change, and a host maps its view onto it at the full resolution. */
inline constexpr std::uint32_t touchscreenAbsoluteMaximum = 32767;
/* Linux's KEY_POWER, which a host forwards as a phone's power button. */
inline constexpr std::uint16_t keyPower = 116;
/* Linux's KEY_BACK and KEY_HOMEPAGE, which Android's Generic key layout maps
 * to BACK and HOME, and a host forwards as a phone's Back and Home keys. */
inline constexpr std::uint16_t keyBack = 158;
inline constexpr std::uint16_t keyHomepage = 172;
/* The keyboard advertises the contiguous codes 1..keyboardKeyMaximum, then
 * Back and Homepage. A guest's input core delivers only advertised keys, so
 * the codes in between and above stay unadvertised, and a Linux guest sees two
 * more keys than before and nothing else. */
inline constexpr std::uint16_t keyboardKeyMaximum = 0x7f;
inline constexpr std::uint16_t keyboardKeyLast = keyHomepage;

[[nodiscard]] constexpr bool keyboardAdvertisesKey(
    std::uint16_t code) noexcept
{
    return (code >= 1U && code <= keyboardKeyMaximum) ||
        code == keyBack || code == keyHomepage;
}

inline constexpr std::uint8_t propertyDirect = 0x01;
inline constexpr std::uint16_t busVirtual = 0x06;

struct [[gnu::packed]] InputAbsoluteInfo final {
    std::uint32_t minimum;
    std::uint32_t maximum;
    std::uint32_t fuzz;
    std::uint32_t flat;
    std::uint32_t resolution;
};

struct [[gnu::packed]] InputDeviceIds final {
    std::uint16_t bus;
    std::uint16_t vendor;
    std::uint16_t product;
    std::uint16_t version;
};

union [[gnu::packed]] InputConfigValue final {
    std::array<char, 128> string;
    std::array<std::uint8_t, 128> bitmap;
    InputAbsoluteInfo absolute;
    InputDeviceIds ids;
};

struct [[gnu::packed]] InputConfig final {
    std::uint8_t select;
    std::uint8_t subselect;
    std::uint8_t size;
    std::array<std::uint8_t, 5> reserved;
    InputConfigValue value;
};

struct [[gnu::packed]] InputEvent final {
    std::uint16_t type;
    std::uint16_t code;
    std::int32_t value;
};

static_assert(sizeof(InputConfig) == 136);
static_assert(sizeof(InputEvent) == 8);
static_assert(keyboardKeyLast / 8U < sizeof(InputConfigValue::bitmap));

} // namespace uml::input::virtio::protocol
