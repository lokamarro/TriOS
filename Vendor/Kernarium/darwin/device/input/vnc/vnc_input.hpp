// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace uml::input::vnc {

enum class PointerMode {
    mouse,
    touchscreen,
};

struct [[gnu::packed]] VncInputEvent {
    std::uint16_t type;
    std::uint16_t code;
    std::int32_t value;
};

using VncInputSink =
    std::function<void(std::span<const VncInputEvent> events)>;

constexpr std::uint16_t eventSync = 0;
constexpr std::uint16_t eventKey = 1;
constexpr std::uint16_t eventRelative = 2;
constexpr std::uint16_t eventAbsolute = 3;

constexpr std::uint16_t syncReport = 0;
constexpr std::uint16_t relativeHorizontalWheel = 6;
constexpr std::uint16_t relativeWheel = 8;
constexpr std::uint16_t absoluteX = 0;
constexpr std::uint16_t absoluteY = 1;

constexpr std::uint16_t buttonLeft = 0x110;
constexpr std::uint16_t buttonRight = 0x111;
constexpr std::uint16_t buttonMiddle = 0x112;
constexpr std::uint16_t buttonTouch = 0x14a;

constexpr std::int32_t absoluteMaximum = 32767;

/* Native windows use a stable absolute tablet range. Mapping through the
 * current host extent keeps the same visual location after either side
 * changes resolution. */
[[nodiscard]] std::int32_t normalizedAbsoluteCoordinate(
    double coordinate,
    double extent) noexcept;

[[nodiscard]] std::optional<std::uint16_t> keyCode(
    std::uint32_t keySymbol) noexcept;

/* LibVNCServer invokes input handlers through a C callback boundary. A custom
 * host sink must therefore never be allowed to unwind through that boundary.
 * The boolean result lets the display report a failed delivery without
 * conflating it with an intentionally absent sink. */
[[nodiscard]] bool tryDeliver(
    const VncInputSink& sink,
    std::span<const VncInputEvent> events) noexcept;

static_assert(sizeof(VncInputEvent) == 8);

} // namespace uml::input::vnc
