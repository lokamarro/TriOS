// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "channel.hpp"
#include "vhost_user_device.hpp"

#include <deque>
#include <memory>
#include <string>
#include <string_view>

namespace uml::console {

class VirtioConsoleDevice final : public uml::virtio::VhostUserDevice {
public:
    explicit VirtioConsoleDevice(std::unique_ptr<ConsoleChannel> channel);
    VirtioConsoleDevice(
        std::unique_ptr<ConsoleChannel> channel,
        std::string portName);
    ~VirtioConsoleDevice() override;

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
    class EventMultiplexer;

    void validateNamedPort() const;
    void enqueueControl(
        std::uint32_t portId,
        std::uint16_t event,
        std::uint16_t value,
        std::string_view payload = {});
    void processControl(std::span<const std::uint8_t> request);

    std::unique_ptr<ConsoleChannel> channel_;
    std::string portName_;
    std::unique_ptr<EventMultiplexer> events_;
    std::deque<std::vector<std::uint8_t>> controlMessages_;
    bool deviceReady_ {};
    bool portReady_ {};
};

} // namespace uml::console
