// SPDX-License-Identifier: GPL-2.0-or-later
/* Test support: the Virtio touchscreen as the guest sees it, as JSON.
 *
 *   uml-touchscreen-reports
 *
 * prints the device the host attaches (its properties, keys and absolute axes,
 * read from the device's own configuration space) and the reports its
 * Touchscreen model sends for a few contact sequences. The guest's
 * touch_cancel_probe.py builds an input device with those capabilities and
 * writes those reports to it, so the probe and the host's model have one
 * source of truth: a change to the model reaches the guest check with no
 * second copy to edit.
 *
 * The steps run in order on one Touchscreen, as a session does, so a step
 * starts from the slot state the steps before it left. Each report is a list
 * of [type, code, value] triples ending in a SYN_REPORT.
 */

#include "protocol.hpp"
#include "touchscreen.hpp"
#include "virtio_input_device.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <span>
#include <vector>

namespace {

using uml::input::virtio::DeviceKind;
using uml::input::virtio::EventQueue;
using uml::input::virtio::Touchscreen;
using uml::input::virtio::TouchscreenReport;
using uml::input::virtio::VirtioInputDevice;
namespace protocol = uml::input::virtio::protocol;

[[noreturn]] void fail(const char* message)
{
    std::fprintf(stderr, "uml-touchscreen-reports: %s\n", message);
    std::exit(EXIT_FAILURE);
}

protocol::InputConfig configuration(
    VirtioInputDevice& device,
    std::uint8_t select,
    std::uint8_t subselect)
{
    device.writeConfiguration(0, 0, std::span {&select, 1U});
    device.writeConfiguration(1, 0, std::span {&subselect, 1U});
    const auto bytes = device.configuration();
    protocol::InputConfig config {};
    if (bytes.size() != sizeof(config))
        fail("unexpected configuration size");
    std::memcpy(&config, bytes.data(), sizeof(config));
    return config;
}

// The set bits of a configuration's bitmap, in ascending order.
std::vector<unsigned> setBits(const protocol::InputConfig& config)
{
    std::vector<unsigned> bits;
    for (unsigned bit = 0; bit < config.size * 8U; ++bit) {
        if ((config.value.bitmap[bit / 8U] & (1U << (bit % 8U))) != 0)
            bits.push_back(bit);
    }
    return bits;
}

void printList(const std::vector<unsigned>& values)
{
    std::printf("[");
    for (std::size_t index = 0; index < values.size(); ++index)
        std::printf("%s%u", index == 0 ? "" : ", ", values[index]);
    std::printf("]");
}

void printDevice()
{
    VirtioInputDevice device(
        DeviceKind::touchscreen, std::make_shared<EventQueue>());
    std::printf("  \"properties\": ");
    printList(setBits(configuration(device, protocol::configPropertyBits, 0)));
    std::printf(",\n  \"keys\": ");
    printList(setBits(configuration(
        device, protocol::configEventBits, protocol::eventKey)));
    std::printf(",\n  \"absolute\": [");
    bool first = true;
    for (const auto code : setBits(configuration(
             device, protocol::configEventBits, protocol::eventAbsolute))) {
        const auto info = configuration(
            device, protocol::configAbsoluteInfo,
            static_cast<std::uint8_t>(code));
        if (info.size != sizeof(protocol::InputAbsoluteInfo))
            fail("an advertised axis has no range");
        std::printf(
            "%s\n    {\"code\": %u, \"minimum\": %u, \"maximum\": %u}",
            first ? "" : ",",
            code,
            info.value.absolute.minimum,
            info.value.absolute.maximum);
        first = false;
    }
    std::printf("\n  ],\n");
}

// A report's SYN_REPORT-terminated frames, as JSON.
void printReports(const std::vector<TouchscreenReport>& reports)
{
    std::printf("[");
    bool firstFrame = true;
    for (const auto& report : reports) {
        bool open = false;
        for (const auto& event : report.span()) {
            if (!open) {
                std::printf("%s\n      [", firstFrame ? "" : ",");
                firstFrame = false;
                open = true;
            } else {
                std::printf(", ");
            }
            std::printf("[%u, %u, %d]", event.type, event.code, event.value);
            if (event.type == protocol::eventSynchronize) {
                std::printf("]");
                open = false;
            }
        }
        if (open)
            fail("a report does not end in a SYN_REPORT");
    }
    std::printf("\n    ]");
}

bool firstStep = true;

void step(const char* name, const std::vector<TouchscreenReport>& reports)
{
    std::printf("%s\n    {\"name\": \"%s\", \"reports\":", firstStep ? "" : ",",
                name);
    firstStep = false;
    // Indented one level deeper than the step's key.
    std::printf(" ");
    printReports(reports);
    std::printf("}");
}

} // namespace

int main()
{
    std::printf("{\n");
    printDevice();
    std::printf("  \"steps\": [");

    Touchscreen screen;
    const auto down = [&](std::uint32_t slot, std::uint32_t x,
                          std::uint32_t y) {
        return screen.touch(slot, x, y, true);
    };
    const auto up = [&](std::uint32_t slot, std::uint32_t x,
                        std::uint32_t y) {
        return screen.touch(slot, x, y, false);
    };
    const auto move = down;
    const auto cancel = [&](std::uint32_t slot) {
        return screen.cancel(slot);
    };

    // The positions are well apart, so a reader that reports motion shows it.
    step("tap", {down(0, 1000, 1000), up(0, 1000, 1000)});
    step("cancel-one-finger", {
        down(0, 1000, 1000), move(0, 2000, 1000), cancel(0)});
    // A slot that held a palm: its next contact is a finger again.
    step("touch-after-cancel", {down(0, 3000, 3000), up(0, 3000, 3000)});
    step("cancel-one-of-two", {
        down(0, 1000, 1000), down(1, 5000, 5000), cancel(0),
        move(1, 6000, 5000), up(1, 6000, 5000)});
    step("cancel-the-second-of-two", {
        down(0, 1000, 1000), down(1, 5000, 5000), cancel(1),
        up(0, 1000, 1000)});
    step("tap-in-the-cancelled-slot", {down(1, 4000, 4000), up(1, 4000, 4000)});
    // A slot with no contact: nothing reaches the guest.
    step("cancel-nothing", {cancel(0), cancel(7)});

    std::printf("\n  ]\n}\n");
    return EXIT_SUCCESS;
}
