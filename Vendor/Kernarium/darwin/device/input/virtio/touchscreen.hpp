// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "protocol.hpp"

#include <array>
#include <span>

namespace uml::input::virtio {

struct TouchscreenReport final {
    // A lift at a new point first reports the final active position, then
    // ends the slot. Type-B readers otherwise lose that last movement. A
    // cancel is two reports: the palm, then the lift. Every report ends in a
    // SYN_REPORT; the longest sequence, a lift at a new point, is thirteen
    // events.
    std::array<protocol::InputEvent, 16> events {};
    std::size_t count {};

    [[nodiscard]] std::span<const protocol::InputEvent> span() const noexcept
    {
        return {events.data(), count};
    }
};

/* Linux type-B contacts. One host contact keeps its slot until it ends;
 * leaving and re-entering the visible display starts a new tracking ID, and
 * every new contact is an MT_TOOL_FINGER, set before its tracking ID. A slot
 * that held a palm keeps that tool type until told otherwise, and a reader
 * that sees the tracking ID first (libinput before 1.30) would drop the
 * whole next touch.
 * ABS_X/Y follow the first contact until it lifts, for single-touch readers. */
class Touchscreen final {
public:
    [[nodiscard]] TouchscreenReport touch(
        std::uint32_t slot,
        std::uint32_t x,
        std::uint32_t y,
        bool pressed) noexcept;

    /* Withdraws a contact the host did not end itself, such as a touch the
     * system took over. The slot turns MT_TOOL_PALM while it keeps its
     * tracking ID, then a second report lifts it where it was. Android's
     * input reader cancels the contact on the first (a palm and a lift in
     * one report are a plain lift to it), and libinput reports a touch
     * cancel and no touch up, for one finger of several as well. A slot that
     * holds no contact, or does not exist, gives an empty report. */
    [[nodiscard]] TouchscreenReport cancel(std::uint32_t slot) noexcept;

private:
    struct Contact final {
        std::int32_t trackingId {-1};
        std::uint32_t x {};
        std::uint32_t y {};
    };

    [[nodiscard]] std::int32_t nextTrackingId() noexcept;

    std::array<Contact, protocol::touchscreenContactCount> contacts_ {};
    std::uint32_t primary_ {protocol::touchscreenContactCount};
    std::int32_t nextId_ {};
};

} // namespace uml::input::virtio
