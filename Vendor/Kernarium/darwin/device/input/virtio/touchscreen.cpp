// SPDX-License-Identifier: GPL-2.0-or-later

#include "touchscreen.hpp"

#include <algorithm>

namespace uml::input::virtio {

std::int32_t Touchscreen::nextTrackingId() noexcept
{
    for (;;) {
        const auto id = nextId_;
        nextId_ = id == protocol::touchscreenTrackingIdMaximum ? 0 : id + 1;
        if (std::none_of(contacts_.begin(), contacts_.end(), [id](const auto& c) {
                return c.trackingId == id;
            })) {
            return id;
        }
    }
}

TouchscreenReport Touchscreen::touch(
    std::uint32_t slot,
    std::uint32_t x,
    std::uint32_t y,
    bool pressed) noexcept
{
    TouchscreenReport report;
    if (slot >= contacts_.size() ||
        x > protocol::touchscreenAbsoluteMaximum ||
        y > protocol::touchscreenAbsoluteMaximum) {
        return report;
    }
    auto& contact = contacts_[slot];
    const bool wasPressed = contact.trackingId >= 0;
    if (!pressed && !wasPressed)
        return report;

    if (!pressed && (x != contact.x || y != contact.y))
        report = touch(slot, x, y, true);

    const bool changed = pressed != wasPressed;
    if (changed)
        contact.trackingId = pressed ? nextTrackingId() : -1;
    contact.x = x;
    contact.y = y;
    if (primary_ == contacts_.size() ||
        contacts_[primary_].trackingId < 0) {
        primary_ = protocol::touchscreenContactCount;
        for (std::uint32_t index = 0; index < contacts_.size(); ++index) {
            if (contacts_[index].trackingId >= 0) {
                primary_ = index;
                break;
            }
        }
    }
    const bool anyPressed = primary_ < contacts_.size();
    const auto append = [&](std::uint16_t type, std::uint16_t code,
                            std::int32_t value) {
        report.events[report.count++] = {type, code, value};
    };
    // BTN_TOUCH must be first in a report for Linux's legacy emulation.
    append(protocol::eventKey, protocol::buttonTouch, anyPressed ? 1 : 0);
    append(protocol::eventAbsolute, protocol::absoluteMtSlot,
           static_cast<std::int32_t>(slot));
    if (changed) {
        if (pressed) {
            // The tool type comes first: a slot that last held a palm still
            // has that tool type, and a reader that sees the tracking ID
            // before it treats the new contact as the palm.
            append(protocol::eventAbsolute, protocol::absoluteMtToolType,
                   protocol::toolTypeFinger);
        }
        append(protocol::eventAbsolute, protocol::absoluteMtTrackingId,
               contact.trackingId);
    }
    if (pressed) {
        append(protocol::eventAbsolute, protocol::absoluteMtPositionX,
               static_cast<std::int32_t>(x));
        append(protocol::eventAbsolute, protocol::absoluteMtPositionY,
               static_cast<std::int32_t>(y));
    }
    const auto& primary = anyPressed ? contacts_[primary_] : contact;
    append(protocol::eventAbsolute, protocol::absoluteX,
           static_cast<std::int32_t>(primary.x));
    append(protocol::eventAbsolute, protocol::absoluteY,
           static_cast<std::int32_t>(primary.y));
    append(protocol::eventSynchronize, protocol::synchronizeReport, 0);
    return report;
}

TouchscreenReport Touchscreen::cancel(std::uint32_t slot) noexcept
{
    TouchscreenReport report;
    if (slot >= contacts_.size() || contacts_[slot].trackingId < 0)
        return report;
    const auto contact = contacts_[slot];
    // The slot keeps its tracking ID and BTN_TOUCH stays down: Android's
    // input reader skips a slot that is not in use, and takes a device whose
    // BTN_TOUCH is up for hovering.
    const auto& primary = contacts_[primary_];
    const auto append = [&](std::uint16_t type, std::uint16_t code,
                            std::int32_t value) {
        report.events[report.count++] = {type, code, value};
    };
    append(protocol::eventKey, protocol::buttonTouch, 1);
    append(protocol::eventAbsolute, protocol::absoluteMtSlot,
           static_cast<std::int32_t>(slot));
    append(protocol::eventAbsolute, protocol::absoluteMtToolType,
           protocol::toolTypePalm);
    append(protocol::eventAbsolute, protocol::absoluteX,
           static_cast<std::int32_t>(primary.x));
    append(protocol::eventAbsolute, protocol::absoluteY,
           static_cast<std::int32_t>(primary.y));
    append(protocol::eventSynchronize, protocol::synchronizeReport, 0);

    // The ordinary lift, where the contact was, in a report of its own.
    const auto lift = touch(slot, contact.x, contact.y, false);
    for (const auto& event : lift.span())
        report.events[report.count++] = event;
    return report;
}

} // namespace uml::input::virtio
