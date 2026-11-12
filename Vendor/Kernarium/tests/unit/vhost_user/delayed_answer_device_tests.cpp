// SPDX-License-Identifier: GPL-2.0-or-later
// The delayed-answer test device holds back the answers its policy chooses,
// wakes the transport when one falls due, drops the rest at a reset, and
// answers in the wrapped device's place when its answerer chooses to.
#include "delayed_answer_device.hpp"
#include "require.hpp"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using uml::testing::DelayedAnswerDevice;
using uml::testing::require;
using uml::virtio::VhostUserQueueAction;
using uml::virtio::VhostUserQueueCompletion;
using uml::virtio::VhostUserQueueResult;
using Clock = std::chrono::steady_clock;

/* Answers each request with its first byte plus one, or defers it when that
 * byte is zero; its own event source is a pipe the test writes. */
class EchoDevice final : public uml::virtio::VhostUserDevice {
public:
    explicit EchoDevice(std::atomic_int& resets) : resets_(resets)
    {
        require(::pipe(events_) == 0);
        require(::fcntl(events_[0], F_SETFL, O_NONBLOCK) == 0);
    }
    ~EchoDevice() override
    {
        ::close(events_[0]);
        ::close(events_[1]);
    }

    const char* name() const noexcept override { return "echo"; }
    std::uint64_t features() const noexcept override { return 0; }
    std::size_t queueCount() const noexcept override { return 1; }
    std::vector<std::uint8_t> configuration() const override { return {}; }
    void writeConfiguration(std::uint32_t, std::uint32_t,
                            std::span<const std::uint8_t>) override {}
    VhostUserQueueResult processQueue(std::uint32_t, std::uint64_t,
                                      std::span<const std::uint8_t> request,
                                      std::size_t) override
    {
        if (request[0] == 0)
            return {VhostUserQueueAction::defer, {}};
        return {VhostUserQueueAction::complete,
                {static_cast<std::uint8_t>(request[0] + 1)}};
    }
    void reset() noexcept override { ++resets_; }
    int eventDescriptor() const noexcept override { return events_[0]; }
    void drainEventSource() override
    {
        char byte;
        while (::read(events_[0], &byte, 1) > 0)
            ++drains;
    }

    void signal() const { require(::write(events_[1], "x", 1) == 1); }

    int drains {};

private:
    std::atomic_int& resets_;
    int events_[2] {-1, -1};
};

bool readable(const DelayedAnswerDevice& device, std::chrono::milliseconds wait)
{
    pollfd descriptor {device.eventDescriptor(), POLLIN, 0};
    return ::poll(&descriptor, 1, static_cast<int>(wait.count())) == 1 &&
           (descriptor.revents & POLLIN) != 0;
}

struct Fixture {
    std::atomic_int resets {};
    EchoDevice* echo {};
    std::vector<std::string> log;
    std::unique_ptr<DelayedAnswerDevice> device;

    explicit Fixture(DelayedAnswerDevice::Policy policy,
                     DelayedAnswerDevice::Answerer answerer = {})
    {
        auto inner = std::make_unique<EchoDevice>(resets);
        echo = inner.get();
        device = std::make_unique<DelayedAnswerDevice>(
            std::move(inner), std::move(policy),
            [this](std::string_view line) { log.emplace_back(line); },
            std::move(answerer));
    }
};

VhostUserQueueResult send(DelayedAnswerDevice& device, std::uint64_t id, std::uint8_t byte)
{
    const std::uint8_t request[] {byte};
    return device.processQueue(0, id, request, 16);
}

void passesUnchosenRequestsThrough()
{
    Fixture fixture([](std::uint32_t, std::span<const std::uint8_t>) {
        return std::optional<std::chrono::milliseconds> {};
    });
    const auto result = send(*fixture.device, 1, 4);
    require(result.action == VhostUserQueueAction::complete);
    require(result.response == std::vector<std::uint8_t> {5});
    require(!readable(*fixture.device, 0ms), "nothing is held, so nothing wakes");
    require(fixture.log.empty());
}

void deliversAHeldAnswerWhenItFallsDue()
{
    Fixture fixture([](std::uint32_t queue, std::span<const std::uint8_t> request) {
        return queue == 0 && request[0] == 7 ?
            std::optional {std::chrono::milliseconds {150}} : std::nullopt;
    });
    const auto sent = Clock::now();
    const auto result = send(*fixture.device, 42, 7);
    require(result.action == VhostUserQueueAction::defer, "a chosen answer is held");
    VhostUserQueueCompletion completion;
    require(!fixture.device->takeQueueCompletion(completion), "not due yet");
    require(send(*fixture.device, 43, 1).action == VhostUserQueueAction::complete,
            "other requests are still answered at once");

    require(readable(*fixture.device, 5000ms), "the transport is woken when it falls due");
    require(Clock::now() - sent >= 150ms, "the answer is not woken early");
    fixture.device->drainEventSource();
    require(fixture.device->takeQueueCompletion(completion));
    require(completion.requestId == 42);
    require(completion.response == std::vector<std::uint8_t> {8});
    require(!fixture.device->takeQueueCompletion(completion), "it is delivered once");
    require(!readable(*fixture.device, 0ms), "draining leaves nothing readable");
    require(fixture.log.size() == 2 && fixture.log[1] == "delivered a held answer");
}

void dropsWhatItHoldsAtAReset()
{
    Fixture fixture([](std::uint32_t, std::span<const std::uint8_t>) {
        return std::optional {DelayedAnswerDevice::never};
    });
    require(send(*fixture.device, 1, 3).action == VhostUserQueueAction::defer);
    require(!readable(*fixture.device, 300ms), "an answer held for good never falls due");
    fixture.device->reset();
    require(fixture.resets == 1, "the device itself is reset");
    VhostUserQueueCompletion completion;
    require(!fixture.device->takeQueueCompletion(completion), "the reset dropped it");
    require(fixture.log.back() == "reset dropped 1 held answer(s)");
}

void neverHoldsWhatTheDeviceDefers()
{
    Fixture fixture([](std::uint32_t, std::span<const std::uint8_t>) {
        return std::optional {std::chrono::milliseconds {10}};
    });
    require(send(*fixture.device, 1, 0).action == VhostUserQueueAction::defer);
    require(!readable(*fixture.device, 200ms), "nothing was held");
    require(fixture.log.empty());
}

void answersInTheDevicesPlace()
{
    Fixture fixture(
        [](std::uint32_t, std::span<const std::uint8_t>) {
            return std::optional<std::chrono::milliseconds> {};
        },
        [](std::uint32_t queue, std::span<const std::uint8_t> request) {
            return queue == 0 && request[0] == 9 ?
                std::optional {std::vector<std::uint8_t> {0x90}} : std::nullopt;
        });
    const auto answered = send(*fixture.device, 1, 9);
    require(answered.action == VhostUserQueueAction::complete);
    require(answered.response == std::vector<std::uint8_t> {0x90},
            "the answerer's answer goes back, not the device's");
    require(fixture.log.size() == 1 &&
                fixture.log[0] == "answered a request in the device's place");
    require(send(*fixture.device, 2, 4).response == std::vector<std::uint8_t> {5},
            "a request the answerer leaves goes to the device");
    require(fixture.log.size() == 1);
}

void forwardsTheDevicesOwnEvents()
{
    Fixture fixture([](std::uint32_t, std::span<const std::uint8_t>) {
        return std::optional<std::chrono::milliseconds> {};
    });
    fixture.echo->signal();
    require(readable(*fixture.device, 5000ms), "the device's event wakes the transport");
    fixture.device->drainEventSource();
    require(fixture.echo->drains == 1, "draining reaches the device");
    require(!readable(*fixture.device, 0ms));
}

} // namespace

int main()
{
    passesUnchosenRequestsThrough();
    deliversAHeldAnswerWhenItFallsDue();
    dropsWhatItHoldsAtAReset();
    neverHoldsWhatTheDeviceDefers();
    answersInTheDevicesPlace();
    forwardsTheDevicesOwnEvents();
    std::puts("delayed-answer device tests passed");
    return 0;
}
