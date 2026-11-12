// SPDX-License-Identifier: GPL-2.0-or-later

#include "vnc_input.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <utility>

namespace uml::input::vnc {
namespace {

using Key = std::pair<std::uint32_t, std::uint16_t>;

constexpr std::array asciiKeys {
    Key {'1', 2}, Key {'2', 3}, Key {'3', 4}, Key {'4', 5},
    Key {'5', 6}, Key {'6', 7}, Key {'7', 8}, Key {'8', 9},
    Key {'9', 10}, Key {'0', 11}, Key {'-', 12}, Key {'=', 13},
    Key {'q', 16}, Key {'w', 17}, Key {'e', 18}, Key {'r', 19},
    Key {'t', 20}, Key {'y', 21}, Key {'u', 22}, Key {'i', 23},
    Key {'o', 24}, Key {'p', 25}, Key {'[', 26}, Key {']', 27},
    Key {'a', 30}, Key {'s', 31}, Key {'d', 32}, Key {'f', 33},
    Key {'g', 34}, Key {'h', 35}, Key {'j', 36}, Key {'k', 37},
    Key {'l', 38}, Key {';', 39}, Key {'\'', 40}, Key {'`', 41},
    Key {'\\', 43}, Key {'z', 44}, Key {'x', 45}, Key {'c', 46},
    Key {'v', 47}, Key {'b', 48}, Key {'n', 49}, Key {'m', 50},
    Key {',', 51}, Key {'.', 52}, Key {'/', 53}, Key {' ', 57},
};

constexpr std::array shiftedAsciiKeys {
    Key {'!', 2}, Key {'@', 3}, Key {'#', 4}, Key {'$', 5},
    Key {'%', 6}, Key {'^', 7}, Key {'&', 8}, Key {'*', 9},
    Key {'(', 10}, Key {')', 11}, Key {'_', 12}, Key {'+', 13},
    Key {'{', 26}, Key {'}', 27}, Key {':', 39}, Key {'"', 40},
    Key {'~', 41}, Key {'|', 43}, Key {'<', 51}, Key {'>', 52},
    Key {'?', 53},
};

constexpr std::array specialKeys {
    Key {0xff08, 14},  // BackSpace
    Key {0xff09, 15},  // Tab
    Key {0xff0d, 28},  // Return
    Key {0xff13, 119}, // Pause
    Key {0xff14, 70},  // Scroll_Lock
    Key {0xff1b, 1},   // Escape
    Key {0xff50, 102}, // Home
    Key {0xff51, 105}, // Left
    Key {0xff52, 103}, // Up
    Key {0xff53, 106}, // Right
    Key {0xff54, 108}, // Down
    Key {0xff55, 104}, // Page_Up
    Key {0xff56, 109}, // Page_Down
    Key {0xff57, 107}, // End
    Key {0xff61, 99},  // Print
    Key {0xff63, 110}, // Insert
    Key {0xff7f, 69},  // Num_Lock
    Key {0xff8d, 96},  // KP_Enter
    Key {0xffaa, 55},  // KP_Multiply
    Key {0xffab, 78},  // KP_Add
    Key {0xffad, 74},  // KP_Subtract
    Key {0xffae, 83},  // KP_Decimal
    Key {0xffaf, 98},  // KP_Divide
    Key {0xffb0, 82},  // KP_0
    Key {0xffb1, 79},  // KP_1
    Key {0xffb2, 80},  // KP_2
    Key {0xffb3, 81},  // KP_3
    Key {0xffb4, 75},  // KP_4
    Key {0xffb5, 76},  // KP_5
    Key {0xffb6, 77},  // KP_6
    Key {0xffb7, 71},  // KP_7
    Key {0xffb8, 72},  // KP_8
    Key {0xffb9, 73},  // KP_9
    Key {0xffbe, 59},  // F1
    Key {0xffbf, 60},  // F2
    Key {0xffc0, 61},  // F3
    Key {0xffc1, 62},  // F4
    Key {0xffc2, 63},  // F5
    Key {0xffc3, 64},  // F6
    Key {0xffc4, 65},  // F7
    Key {0xffc5, 66},  // F8
    Key {0xffc6, 67},  // F9
    Key {0xffc7, 68},  // F10
    Key {0xffc8, 87},  // F11
    Key {0xffc9, 88},  // F12
    Key {0xffe1, 42},  // Shift_L
    Key {0xffe2, 54},  // Shift_R
    Key {0xffe3, 29},  // Control_L
    Key {0xffe4, 97},  // Control_R
    Key {0xffe5, 58},  // Caps_Lock
    Key {0xffe9, 56},  // Alt_L
    Key {0xffea, 100}, // Alt_R
    Key {0xffeb, 125}, // Super_L
    Key {0xffec, 126}, // Super_R
    Key {0xff67, 127}, // Menu
    Key {0xffff, 111}, // Delete
    /* A phone's power button, which also wakes a guest that suspended itself. */
    Key {0x1008ff2a, 116}, // XF86PowerOff
};

template<std::size_t Size>
std::optional<std::uint16_t> find(
    const std::array<Key, Size>& keys,
    std::uint32_t symbol) noexcept
{
    for (const auto& [candidate, code] : keys)
        if (candidate == symbol)
            return code;
    return std::nullopt;
}

} // namespace

std::optional<std::uint16_t> keyCode(std::uint32_t keySymbol) noexcept
{
    if (keySymbol >= 'A' && keySymbol <= 'Z')
        keySymbol = static_cast<std::uint32_t>(
            std::tolower(static_cast<unsigned char>(keySymbol)));
    if (auto code = find(asciiKeys, keySymbol))
        return code;
    if (auto code = find(shiftedAsciiKeys, keySymbol))
        return code;
    return find(specialKeys, keySymbol);
}

std::int32_t normalizedAbsoluteCoordinate(
    double coordinate,
    double extent) noexcept
{
    if (!std::isfinite(coordinate) || !std::isfinite(extent) || extent <= 0.0)
        return 0;
    const double normalized = std::clamp(coordinate / extent, 0.0, 1.0);
    return static_cast<std::int32_t>(
        std::lround(normalized * absoluteMaximum));
}

bool tryDeliver(
    const VncInputSink& sink,
    std::span<const VncInputEvent> events) noexcept
{
    if (!sink)
        return true;
    try {
        sink(events);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace uml::input::vnc
