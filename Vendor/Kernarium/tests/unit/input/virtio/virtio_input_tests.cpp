// SPDX-License-Identifier: GPL-2.0-or-later

#include "require.hpp"
#include "touchscreen.hpp"
#include "virtio_input_device.hpp"
#include "wire_bytes.hpp"

#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <poll.h>
#include <vector>

namespace {

using uml::input::virtio::DeviceKind;
using uml::input::virtio::EventQueue;
using uml::input::virtio::VirtioInputDevice;
namespace protocol = uml::input::virtio::protocol;

using uml::testing::require;

protocol::InputConfig configuration(VirtioInputDevice& device)
{
    return uml::testing::loadExact<protocol::InputConfig>(
        device.configuration());
}

void select(
    VirtioInputDevice& device,
    std::uint8_t selected,
    std::uint8_t subselected)
{
    device.writeConfiguration(0, 0, std::span {&selected, 1U});
    device.writeConfiguration(1, 0, std::span {&subselected, 1U});
}

bool bit(const protocol::InputConfig& config, std::uint16_t index)
{
    return (config.value.bitmap[index / 8U] & (1U << (index % 8U))) != 0;
}

void touchscreenConfigurationIsDirectWithAFixedRange()
{
    auto events = std::make_shared<EventQueue>();
    VirtioInputDevice device(DeviceKind::touchscreen, events);
    require(device.queueCount() == 2);
    require(device.features() == 0);

    select(device, protocol::configIdName, 0);
    const auto name = configuration(device);
    require(name.size == std::strlen("Kernarium Touchscreen"));
    require(std::memcmp(
                name.value.string.data(),
                "Kernarium Touchscreen",
                name.size) == 0);

    select(device, protocol::configPropertyBits, 0);
    const auto properties = configuration(device);
    require(properties.size == 1);
    require(bit(properties, protocol::propertyDirect));

    select(device, protocol::configEventBits, protocol::eventKey);
    const auto keys = configuration(device);
    require(bit(keys, protocol::buttonTouch));
    require(!bit(keys, 0x110)); // BTN_LEFT
    require(!bit(keys, 0x111)); // BTN_RIGHT
    require(!bit(keys, 0x112)); // BTN_MIDDLE
    select(device, protocol::configEventBits, 0x02); // EV_REL
    require(configuration(device).size == 0);
    select(device, protocol::configEventBits, protocol::eventAbsolute);
    const auto absoluteBits = configuration(device);
    require(bit(absoluteBits, protocol::absoluteX));
    require(bit(absoluteBits, protocol::absoluteY));
    require(bit(absoluteBits, protocol::absoluteMtSlot));
    require(bit(absoluteBits, protocol::absoluteMtTrackingId));
    require(bit(absoluteBits, protocol::absoluteMtPositionX));
    require(bit(absoluteBits, protocol::absoluteMtPositionY));
    require(bit(absoluteBits, protocol::absoluteMtToolType));

    select(device, protocol::configAbsoluteInfo, protocol::absoluteX);
    const auto x = configuration(device);
    require(x.size == sizeof(protocol::InputAbsoluteInfo));
    require(x.value.absolute.minimum == 0);
    require(x.value.absolute.maximum == protocol::touchscreenAbsoluteMaximum);
    select(device, protocol::configAbsoluteInfo, protocol::absoluteY);
    const auto y = configuration(device);
    require(y.value.absolute.minimum == 0);
    require(y.value.absolute.maximum == protocol::touchscreenAbsoluteMaximum);
    for (const auto axis : {protocol::absoluteMtPositionX,
                            protocol::absoluteMtPositionY}) {
        select(device, protocol::configAbsoluteInfo,
               static_cast<std::uint8_t>(axis));
        const auto info = configuration(device);
        require(info.size == sizeof(protocol::InputAbsoluteInfo));
        require(info.value.absolute.minimum == 0);
        require(info.value.absolute.maximum == 32767);
    }
    select(device, protocol::configAbsoluteInfo, protocol::absoluteMtSlot);
    require(configuration(device).value.absolute.maximum == 9);
    select(device, protocol::configAbsoluteInfo, protocol::absoluteMtTrackingId);
    require(configuration(device).value.absolute.maximum == 65535);
    // The tool type spans MT_TOOL_FINGER through MT_TOOL_PALM.
    select(device, protocol::configAbsoluteInfo, protocol::absoluteMtToolType);
    const auto tool = configuration(device);
    require(tool.size == sizeof(protocol::InputAbsoluteInfo));
    require(tool.value.absolute.minimum == 0);
    require(tool.value.absolute.maximum == 2);
}

std::int32_t value(
    const uml::input::virtio::TouchscreenReport& report,
    std::uint16_t type,
    std::uint16_t code)
{
    for (const auto& event : report.span()) {
        if (event.type == type && event.code == code)
            return event.value;
    }
    require(false);
    return -1;
}

void multipleContactsMoveAndLiftIndependently()
{
    uml::input::virtio::Touchscreen screen;
    const auto first = screen.touch(3, 100, 200, true);
    require(first.events[0].type == protocol::eventKey);
    require(first.events[0].code == protocol::buttonTouch);
    require(first.events[first.count - 1].type == protocol::eventSynchronize);
    const auto firstId = value(first, 3, 0x39); // ABS_MT_TRACKING_ID
    const auto second = screen.touch(1, 300, 400, true);
    const auto secondId = value(second, 3, 0x39);
    require(firstId >= 0 && secondId >= 0 && firstId != secondId);
    require(value(second, 3, 0x2f) == 1); // ABS_MT_SLOT
    require(value(second, 3, 0x35) == 300);
    require(value(second, 3, 0x36) == 400);
    require(value(second, 3, 0) == 100); // ABS_X keeps the original finger
    require(value(second, 3, 1) == 200);

    const auto move = screen.touch(3, 150, 250, true);
    require(value(move, 3, 0x2f) == 3);
    require(value(move, 3, 0x35) == 150);
    require(value(move, 3, 0x36) == 250);
    for (const auto& event : move.span())
        require(event.code != 0x39 || event.type != 3);

    const auto lift = screen.touch(3, 150, 250, false);
    require(value(lift, 3, 0x39) == -1);
    require(value(lift, 1, 0x14a) == 1); // BTN_TOUCH stays down
    require(value(lift, 3, 0) == 300); // primary moves to the remaining finger
    require(value(lift, 3, 1) == 400);
    const auto last = screen.touch(1, 300, 400, false);
    require(value(last, 1, 0x14a) == 0);
    require(value(last, 3, 0x39) == -1);
    require(screen.touch(1, 300, 400, false).count == 0);
    require(value(screen.touch(3, 100, 200, true), 3, 0x39) != firstId);
}

void tenContactsAndTrackingIdWrapKeepLiveIdsUnique()
{
    uml::input::virtio::Touchscreen screen;
    std::array<std::int32_t, 10> ids {};
    for (std::uint32_t slot = 0; slot < ids.size(); ++slot) {
        ids[slot] = value(screen.touch(slot, slot, slot, true), 3, 0x39);
        for (std::uint32_t prior = 0; prior < slot; ++prior)
            require(ids[slot] != ids[prior]);
    }
    require(screen.touch(10, 0, 0, true).count == 0);
    require(screen.touch(0, 32768, 0, true).count == 0);
    require(screen.touch(0, 0, 32768, true).count == 0);
    for (std::uint32_t index = 0; index < 65536; ++index) {
        (void)screen.touch(9, 0, 0, false);
        const auto id = value(screen.touch(9, 0, 0, true), 3, 0x39);
        require(id >= 0 && id <= 65535);
        for (std::size_t slot = 0; slot < 9; ++slot)
            require(id != ids[slot]);
    }
    for (std::uint32_t slot = 0; slot < 10; ++slot) {
        const auto lift = screen.touch(slot, 0, 0, false);
        require(value(lift, 1, 0x14a) == (slot == 9 ? 0 : 1));
    }
}

void liftingAtTheViewportEdgeDeliversTheFinalMovementBeforeUp()
{
    uml::input::virtio::Touchscreen screen;
    (void)screen.touch(0, 100, 200, true);
    const auto lift = screen.touch(0, 32767, 200, false);
    unsigned reports = 0;
    bool moved = false;
    bool released = false;
    for (const auto& event : lift.span()) {
        if (event.type == 3 && event.code == 0x35) {
            require(!released);
            require(event.value == 32767);
            moved = true;
        }
        if (event.type == 3 && event.code == 0x39) {
            require(moved && reports == 1);
            require(event.value == -1);
            released = true;
        }
        if (event.type == 0 && event.code == 0)
            ++reports;
    }
    require(moved && released && reports == 2);
    require(lift.events[lift.count - 1].type == protocol::eventSynchronize);
}

using Event = protocol::InputEvent;
using Frame = std::vector<Event>;

constexpr Event event(
    std::uint16_t type,
    std::uint16_t code,
    std::int32_t value) noexcept
{
    return {type, code, value};
}

constexpr Event touchDown(std::int32_t down) noexcept
{
    return event(protocol::eventKey, protocol::buttonTouch, down);
}

constexpr Event slotEvent(std::int32_t slot) noexcept
{
    return event(protocol::eventAbsolute, protocol::absoluteMtSlot, slot);
}

constexpr Event toolEvent(std::int32_t tool) noexcept
{
    return event(protocol::eventAbsolute, protocol::absoluteMtToolType, tool);
}

constexpr Event trackingEvent(std::int32_t id) noexcept
{
    return event(protocol::eventAbsolute, protocol::absoluteMtTrackingId, id);
}

constexpr Event absolute(std::uint16_t axis, std::int32_t value) noexcept
{
    return event(protocol::eventAbsolute, axis, value);
}

constexpr Event endOfReport() noexcept
{
    return event(
        protocol::eventSynchronize, protocol::synchronizeReport, 0);
}

bool equal(const Event& left, const Event& right) noexcept
{
    return left.type == right.type && left.code == right.code &&
        left.value == right.value;
}

// A report's SYN_REPORT-terminated frames, which the guest handles one by
// one.
std::vector<Frame> frames(std::span<const Event> events)
{
    std::vector<Frame> result;
    Frame current;
    for (const auto& one : events) {
        current.push_back(one);
        if (one.type == protocol::eventSynchronize) {
            result.push_back(std::move(current));
            current.clear();
        }
    }
    require(current.empty()); // every report ends in a SYN_REPORT
    return result;
}

std::vector<Frame> frames(const uml::input::virtio::TouchscreenReport& report)
{
    return frames(report.span());
}

bool matches(const Frame& frame, std::initializer_list<Event> expected)
{
    if (frame.size() != expected.size())
        return false;
    std::size_t index = 0;
    for (const auto& one : expected) {
        if (!equal(frame[index++], one))
            return false;
    }
    return true;
}

std::size_t position(const Frame& frame, std::uint16_t code)
{
    for (std::size_t index = 0; index < frame.size(); ++index) {
        if (frame[index].type == protocol::eventAbsolute &&
            frame[index].code == code)
            return index;
    }
    return frame.size();
}

bool has(const Frame& frame, std::uint16_t code)
{
    return position(frame, code) != frame.size();
}

std::int32_t trackingId(const Frame& frame)
{
    const auto index = position(frame, protocol::absoluteMtTrackingId);
    require(index != frame.size());
    return frame[index].value;
}

constexpr auto absoluteX = protocol::absoluteX;
constexpr auto absoluteY = protocol::absoluteY;
constexpr auto absoluteMtX = protocol::absoluteMtPositionX;
constexpr auto absoluteMtY = protocol::absoluteMtPositionY;

void aNewContactIsAFingerBeforeItsTrackingId()
{
    uml::input::virtio::Touchscreen screen;
    const auto down = frames(screen.touch(2, 100, 200, true));
    require(down.size() == 1);
    const auto id = trackingId(down[0]);
    require(id >= 0);
    // The tool type is set first, and in the same report as the contact.
    require(matches(down[0], {
        touchDown(1), slotEvent(2), toolEvent(protocol::toolTypeFinger),
        trackingEvent(id),
        absolute(absoluteMtX, 100), absolute(absoluteMtY, 200),
        absolute(absoluteX, 100), absolute(absoluteY, 200),
        endOfReport(),
    }));
    // A move or a lift states no tool type.
    const auto move = frames(screen.touch(2, 150, 250, true));
    require(move.size() == 1 && !has(move[0], protocol::absoluteMtToolType));
    const auto lift = frames(screen.touch(2, 150, 250, false));
    require(lift.size() == 1 && !has(lift[0], protocol::absoluteMtToolType));
    // A re-entry after a lift is a new contact, and says so the same way.
    const auto again = frames(screen.touch(2, 150, 250, true));
    require(again.size() == 1);
    require(position(again[0], protocol::absoluteMtToolType) <
        position(again[0], protocol::absoluteMtTrackingId));
}

void cancellingAContactIsAPalmReportThenItsLift()
{
    uml::input::virtio::Touchscreen screen;
    (void)screen.touch(0, 100, 200, true);
    const auto cancelled = frames(screen.cancel(0));
    require(cancelled.size() == 2);
    // The slot turns palm while it keeps its tracking ID and BTN_TOUCH stays
    // down, so a reader still sees the contact when it learns it is a palm.
    require(matches(cancelled[0], {
        touchDown(1), slotEvent(0), toolEvent(protocol::toolTypePalm),
        absolute(absoluteX, 100), absolute(absoluteY, 200),
        endOfReport(),
    }));
    // The ordinary lift, where the contact was.
    require(matches(cancelled[1], {
        touchDown(0), slotEvent(0), trackingEvent(-1),
        absolute(absoluteX, 100), absolute(absoluteY, 200),
        endOfReport(),
    }));
    // Nothing is left to lift or cancel.
    require(screen.touch(0, 100, 200, false).count == 0);
    require(screen.cancel(0).count == 0);
}

void cancellingAMovedContactSynthesizesNoMotion()
{
    uml::input::virtio::Touchscreen screen;
    (void)screen.touch(4, 100, 200, true);
    (void)screen.touch(4, 150, 250, true);
    const auto cancelled = frames(screen.cancel(4));
    require(cancelled.size() == 2);
    for (const auto& frame : cancelled) {
        require(!has(frame, absoluteMtX) && !has(frame, absoluteMtY));
    }
    require(matches(cancelled[0], {
        touchDown(1), slotEvent(4), toolEvent(protocol::toolTypePalm),
        absolute(absoluteX, 150), absolute(absoluteY, 250),
        endOfReport(),
    }));
    require(matches(cancelled[1], {
        touchDown(0), slotEvent(4), trackingEvent(-1),
        absolute(absoluteX, 150), absolute(absoluteY, 250),
        endOfReport(),
    }));
}

void cancellingOneOfTwoContactsKeepsTheOtherDown()
{
    uml::input::virtio::Touchscreen screen;
    const auto firstId = trackingId(frames(
        screen.touch(3, 100, 200, true))[0]);
    const auto secondId = trackingId(frames(
        screen.touch(1, 300, 400, true))[0]);
    require(firstId != secondId);

    // The first contact is the single-touch primary: the palm report states
    // its position, and its lift hands ABS_X/Y to the contact that remains.
    const auto first = frames(screen.cancel(3));
    require(first.size() == 2);
    require(matches(first[0], {
        touchDown(1), slotEvent(3), toolEvent(protocol::toolTypePalm),
        absolute(absoluteX, 100), absolute(absoluteY, 200),
        endOfReport(),
    }));
    require(matches(first[1], {
        touchDown(1), slotEvent(3), trackingEvent(-1),
        absolute(absoluteX, 300), absolute(absoluteY, 400),
        endOfReport(),
    }));

    // The survivor moves on and lifts as an ordinary contact.
    const auto move = frames(screen.touch(1, 350, 450, true));
    require(move.size() == 1 && !has(move[0], protocol::absoluteMtTrackingId));
    const auto last = frames(screen.touch(1, 350, 450, false));
    require(matches(last[0], {
        touchDown(0), slotEvent(1), trackingEvent(-1),
        absolute(absoluteX, 350), absolute(absoluteY, 450),
        endOfReport(),
    }));
}

void cancellingASecondaryContactLeavesTheSingleTouchCoordinates()
{
    uml::input::virtio::Touchscreen screen;
    (void)screen.touch(3, 100, 200, true);
    (void)screen.touch(1, 300, 400, true);
    const auto cancelled = frames(screen.cancel(1));
    require(cancelled.size() == 2);
    require(matches(cancelled[0], {
        touchDown(1), slotEvent(1), toolEvent(protocol::toolTypePalm),
        absolute(absoluteX, 100), absolute(absoluteY, 200),
        endOfReport(),
    }));
    require(matches(cancelled[1], {
        touchDown(1), slotEvent(1), trackingEvent(-1),
        absolute(absoluteX, 100), absolute(absoluteY, 200),
        endOfReport(),
    }));
    // The primary is still the first contact, and it lifts last.
    const auto last = frames(screen.touch(3, 100, 200, false));
    require(matches(last[0], {
        touchDown(0), slotEvent(3), trackingEvent(-1),
        absolute(absoluteX, 100), absolute(absoluteY, 200),
        endOfReport(),
    }));
}

void cancellingAnInactiveOrInvalidSlotReportsNothing()
{
    uml::input::virtio::Touchscreen screen;
    require(screen.cancel(0).count == 0);
    require(screen.cancel(protocol::touchscreenContactCount).count == 0);
    require(screen.cancel(0xffffffffU).count == 0);

    (void)screen.touch(5, 10, 20, true);
    require(screen.cancel(4).count == 0);
    require(screen.cancel(protocol::touchscreenContactCount).count == 0);
    // The contact that is down is undisturbed by the refused cancels.
    const auto move = frames(screen.touch(5, 11, 21, true));
    require(move.size() == 1 && !has(move[0], protocol::absoluteMtTrackingId));

    // A released slot and an already cancelled one hold no contact.
    (void)screen.touch(5, 11, 21, false);
    require(screen.cancel(5).count == 0);
    (void)screen.touch(6, 10, 20, true);
    require(screen.cancel(6).count == 12);
    require(screen.cancel(6).count == 0);
}

void aSlotIsAFingerAgainAfterACancel()
{
    uml::input::virtio::Touchscreen screen;
    const auto first = trackingId(frames(screen.touch(0, 100, 200, true))[0]);
    (void)screen.cancel(0);
    const auto again = frames(screen.touch(0, 500, 600, true));
    require(again.size() == 1);
    require(matches(again[0], {
        touchDown(1), slotEvent(0), toolEvent(protocol::toolTypeFinger),
        trackingEvent(trackingId(again[0])),
        absolute(absoluteMtX, 500), absolute(absoluteMtY, 600),
        absolute(absoluteX, 500), absolute(absoluteY, 600),
        endOfReport(),
    }));
    require(trackingId(again[0]) >= 0 && trackingId(again[0]) != first);
    // And it is an ordinary contact: it lifts with one report.
    const auto lift = frames(screen.touch(0, 500, 600, false));
    require(lift.size() == 1);
    require(matches(lift[0], {
        touchDown(0), slotEvent(0), trackingEvent(-1),
        absolute(absoluteX, 500), absolute(absoluteY, 600),
        endOfReport(),
    }));
}

void everyContactSlotCanBeCancelled()
{
    uml::input::virtio::Touchscreen screen;
    for (int round = 0; round < 2; ++round) {
        for (std::uint32_t slot = 0; slot < 10; ++slot)
            (void)screen.touch(slot, slot * 100, slot * 50, true);
        for (std::uint32_t slot = 0; slot < 10; ++slot) {
            const auto cancelled = frames(screen.cancel(slot));
            require(cancelled.size() == 2);
            require(cancelled[0][1].value == static_cast<std::int32_t>(slot));
            require(cancelled[0][2].value == protocol::toolTypePalm);
            require(!has(cancelled[0], protocol::absoluteMtTrackingId));
            require(trackingId(cancelled[1]) == -1);
            // BTN_TOUCH stays down until the last contact is gone.
            require(cancelled[0][0].value == 1);
            require(cancelled[1][0].value == (slot == 9 ? 0 : 1));
            // ABS_X/Y follow the first contact still down.
            if (slot != 9) {
                require(cancelled[1][3].value ==
                    static_cast<std::int32_t>((slot + 1) * 100));
            }
        }
        for (std::uint32_t slot = 0; slot < 10; ++slot)
            require(screen.cancel(slot).count == 0);
    }
}

void aReportFitsItsBuffer()
{
    uml::input::virtio::Touchscreen screen;
    // The longest sequences: a lift at a new point, and a cancel.
    require(screen.touch(0, 100, 200, true).count == 9);
    require(screen.touch(0, 150, 250, false).count == 13);
    (void)screen.touch(0, 100, 200, true);
    require(screen.cancel(0).count == 12);
    (void)screen.touch(0, 100, 200, true);
    (void)screen.touch(1, 100, 200, true);
    require(screen.cancel(0).count == 12);
}

// What a guest reads from the event queue, in order.
std::vector<Event> deliver(
    VirtioInputDevice& device,
    EventQueue& events,
    std::span<const Event> sent)
{
    events.enqueue(sent);
    pollfd readiness {
        .fd = device.eventDescriptor(),
        .events = POLLIN,
        .revents = 0,
    };
    require(::poll(&readiness, 1, 1000) == 1);
    device.drainEventSource();
    std::vector<Event> received;
    for (std::size_t index = 0; index < sent.size(); ++index) {
        const auto result =
            device.processQueue(protocol::eventQueue, 10 + index, {}, 8);
        require(result.action == uml::virtio::VhostUserQueueAction::complete);
        received.push_back(
            uml::testing::loadExact<protocol::InputEvent>(result.response));
    }
    require(device.processQueue(protocol::eventQueue, 99, {}, 8).action ==
        uml::virtio::VhostUserQueueAction::retry);
    return received;
}

void aCancelReachesTheGuestInOrderAsTwoReports()
{
    auto events = std::make_shared<EventQueue>();
    VirtioInputDevice device(DeviceKind::touchscreen, events);
    uml::input::virtio::Touchscreen screen;

    // Down, move, cancel, down again, in the order the host sends them.
    std::vector<Event> sent;
    for (const auto& report : {
             screen.touch(0, 100, 200, true),
             screen.touch(0, 150, 250, true),
             screen.cancel(0),
             screen.touch(0, 300, 400, true),
         }) {
        sent.insert(sent.end(), report.span().begin(), report.span().end());
    }
    const auto received = deliver(device, *events, sent);
    require(received.size() == sent.size());
    for (std::size_t index = 0; index < sent.size(); ++index)
        require(equal(received[index], sent[index]));

    const auto delivered = frames(received);
    require(delivered.size() == 5);
    // The palm frame precedes the lift frame, and the lift is the first frame
    // that ends the tracking ID.
    require(matches(delivered[2], {
        touchDown(1), slotEvent(0), toolEvent(protocol::toolTypePalm),
        absolute(absoluteX, 150), absolute(absoluteY, 250),
        endOfReport(),
    }));
    require(!has(delivered[2], protocol::absoluteMtTrackingId));
    require(trackingId(delivered[3]) == -1);
    require(!has(delivered[3], protocol::absoluteMtToolType));
    // The next contact in the slot is a finger before it has a tracking ID.
    require(position(delivered[4], protocol::absoluteMtToolType) <
        position(delivered[4], protocol::absoluteMtTrackingId));
    require(trackingId(delivered[4]) >= 0);
}

void keyboardAdvertisesEvdevKeysAndLeds()
{
    auto events = std::make_shared<EventQueue>();
    VirtioInputDevice device(DeviceKind::keyboard, events);
    select(device, protocol::configEventBits, protocol::eventKey);
    const auto keys = configuration(device);
    require(bit(keys, 1));
    require(bit(keys, 28));
    require(bit(keys, 57));
    require(bit(keys, protocol::keyboardKeyMaximum));

    select(device, protocol::configEventBits, protocol::eventLed);
    const auto leds = configuration(device);
    require(bit(leds, 0));
    require(bit(leds, 1));
    require(bit(leds, 2));
}

// Android's Generic key layout maps KEY_BACK and KEY_HOMEPAGE to BACK and
// HOME, and the guest's input core delivers only advertised keys.
void keyboardAdvertisesBackAndHomeAndNothingElseAbove127()
{
    require(protocol::keyBack == 158);
    require(protocol::keyHomepage == 172);

    auto events = std::make_shared<EventQueue>();
    VirtioInputDevice device(DeviceKind::keyboard, events);
    select(device, protocol::configEventBits, protocol::eventKey);
    const auto keys = configuration(device);
    require(bit(keys, protocol::keyBack));
    require(bit(keys, protocol::keyHomepage));
    // The map ends at the highest advertised key.
    require(keys.size == protocol::keyHomepage / 8U + 1U);

    constexpr std::uint16_t bitmapBits = 128U * 8U;
    for (std::uint16_t code = 0; code < bitmapBits; ++code) {
        const bool expected = (code >= 1U && code <= 127U) ||
            code == 158U || code == 172U;
        require(bit(keys, code) == expected);
        require(protocol::keyboardAdvertisesKey(code) == expected);
    }
    // A key beyond the bitmap is never advertised either.
    require(!protocol::keyboardAdvertisesKey(bitmapBits));
    require(!protocol::keyboardAdvertisesKey(0xffff));

    // The touchscreen's key map stays BTN_TOUCH alone.
    VirtioInputDevice touchscreen(DeviceKind::touchscreen, events);
    select(touchscreen, protocol::configEventBits, protocol::eventKey);
    const auto touchKeys = configuration(touchscreen);
    require(!bit(touchKeys, protocol::keyBack));
    require(!bit(touchKeys, protocol::keyHomepage));
}

void keyboardDeliversBackAndHomeAsKeyEvents()
{
    auto events = std::make_shared<EventQueue>();
    VirtioInputDevice device(DeviceKind::keyboard, events);

    // The runtime forwards a key as the key event and a report.
    std::array<protocol::InputEvent, 8> input {};
    std::size_t count = 0;
    const std::array keyCodes {protocol::keyBack, protocol::keyHomepage};
    for (const std::uint16_t code : keyCodes) {
        for (const std::int32_t value : {1, 0}) {
            input[count++] = {
                .type = protocol::eventKey,
                .code = code,
                .value = value,
            };
            input[count++] = {
                .type = protocol::eventSynchronize,
                .code = protocol::synchronizeReport,
                .value = 0,
            };
        }
    }
    events->enqueue(input);
    pollfd readiness {
        .fd = device.eventDescriptor(),
        .events = POLLIN,
        .revents = 0,
    };
    require(::poll(&readiness, 1, 1000) == 1);
    device.drainEventSource();
    for (const auto& sent : input) {
        const auto result =
            device.processQueue(protocol::eventQueue, 3, {}, 8);
        require(result.action == uml::virtio::VhostUserQueueAction::complete);
        const auto returned =
            uml::testing::loadExact<protocol::InputEvent>(result.response);
        require(returned.type == sent.type);
        require(returned.code == sent.code);
        require(returned.value == sent.value);
    }
    require(device.processQueue(protocol::eventQueue, 4, {}, 8).action ==
        uml::virtio::VhostUserQueueAction::retry);
}

void eventQueueDefersAndThenReturnsOneWireEvent()
{
    auto events = std::make_shared<EventQueue>();
    VirtioInputDevice device(DeviceKind::touchscreen, events);
    const auto empty = device.processQueue(protocol::eventQueue, 1, {}, 8);
    require(empty.action == uml::virtio::VhostUserQueueAction::retry);

    const std::array input {
        protocol::InputEvent {
            .type = protocol::eventAbsolute,
            .code = protocol::absoluteX,
            .value = 123,
        },
    };
    events->enqueue(input);
    pollfd readiness {
        .fd = device.eventDescriptor(),
        .events = POLLIN,
        .revents = 0,
    };
    require(::poll(&readiness, 1, 1000) == 1);
    device.drainEventSource();
    const auto result = device.processQueue(protocol::eventQueue, 2, {}, 8);
    require(result.action == uml::virtio::VhostUserQueueAction::complete);
    const auto returned =
        uml::testing::loadExact<protocol::InputEvent>(result.response);
    require(returned.type == protocol::eventAbsolute);
    require(returned.code == protocol::absoluteX);
    require(returned.value == 123);
}

} // namespace

int main()
{
    touchscreenConfigurationIsDirectWithAFixedRange();
    multipleContactsMoveAndLiftIndependently();
    tenContactsAndTrackingIdWrapKeepLiveIdsUnique();
    liftingAtTheViewportEdgeDeliversTheFinalMovementBeforeUp();
    aNewContactIsAFingerBeforeItsTrackingId();
    cancellingAContactIsAPalmReportThenItsLift();
    cancellingAMovedContactSynthesizesNoMotion();
    cancellingOneOfTwoContactsKeepsTheOtherDown();
    cancellingASecondaryContactLeavesTheSingleTouchCoordinates();
    cancellingAnInactiveOrInvalidSlotReportsNothing();
    aSlotIsAFingerAgainAfterACancel();
    everyContactSlotCanBeCancelled();
    aReportFitsItsBuffer();
    aCancelReachesTheGuestInOrderAsTwoReports();
    keyboardAdvertisesEvdevKeysAndLeds();
    keyboardAdvertisesBackAndHomeAndNothingElseAbove127();
    keyboardDeliversBackAndHomeAsKeyEvents();
    eventQueueDefersAndThenReturnsOneWireEvent();
    return EXIT_SUCCESS;
}
