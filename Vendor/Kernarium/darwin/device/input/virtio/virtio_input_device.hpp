// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "platform/descriptor.hpp"
#include "protocol.hpp"
#include "vhost_user_device.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>

namespace uml::input::virtio {

enum class DeviceKind {
    touchscreen,
    keyboard,
};

class EventQueue final {
public:
    EventQueue();

    EventQueue(const EventQueue&) = delete;
    EventQueue& operator=(const EventQueue&) = delete;

    void enqueue(std::span<const protocol::InputEvent> events) noexcept;
    [[nodiscard]] bool take(protocol::InputEvent& event) noexcept;
    [[nodiscard]] int descriptor() const noexcept;
    void drain();
    void clear() noexcept;

private:
    uml::platform::WakeSocket wake_;
    std::mutex mutex_;
    std::deque<protocol::InputEvent> events_;
    bool reportedOverflow_ {};
};

class VirtioInputDevice final : public uml::virtio::VhostUserDevice {
public:
    VirtioInputDevice(DeviceKind kind, std::shared_ptr<EventQueue> events);

    [[nodiscard]] const char* name() const noexcept override;
    [[nodiscard]] std::uint64_t features() const noexcept override;
    [[nodiscard]] std::size_t queueCount() const noexcept override;
    [[nodiscard]] std::vector<std::uint8_t> configuration() const override;
    void writeConfiguration(
        std::uint32_t offset,
        std::uint32_t flags,
        std::span<const std::uint8_t> bytes) override;
    [[nodiscard]] uml::virtio::VhostUserQueueResult processQueue(
        std::uint32_t index,
        std::uint64_t requestId,
        std::span<const std::uint8_t> request,
        std::size_t responseCapacity) override;
    void reset() noexcept override;
    [[nodiscard]] int eventDescriptor() const noexcept override;
    void drainEventSource() override;

private:
    [[nodiscard]] protocol::InputConfig selectedConfiguration() const;

    DeviceKind kind_;
    std::shared_ptr<EventQueue> events_;
    std::uint8_t select_ {};
    std::uint8_t subselect_ {};
    bool reportedDelivery_ {};
};

} // namespace uml::input::virtio
