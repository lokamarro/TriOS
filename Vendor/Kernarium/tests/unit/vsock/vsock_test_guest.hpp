// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "endpoint.hpp"
#include "protocol.hpp"
#include "require.hpp"
#include "virtio_vsock_device.hpp"
#include "wait_until.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace uml::vsock::testing {

namespace protocol = uml::vsock::protocol;
using uml::testing::require;
using uml::virtio::VhostUserQueueAction;

struct Packet final {
    protocol::Header header {};
    std::vector<std::uint8_t> payload;
};

/* A header as the guest's driver sends it to the host. */
inline protocol::Header guestHeader(
    std::uint16_t op,
    std::uint32_t guestPort,
    std::uint32_t hostPort,
    std::uint32_t length = 0,
    std::uint32_t flags = 0,
    std::uint32_t bufferAllocation = protocol::bufferAllocation,
    std::uint32_t forwardCount = 0)
{
    return {
        .sourceCid = protocol::guestCid,
        .destinationCid = protocol::hostCid,
        .sourcePort = guestPort,
        .destinationPort = hostPort,
        .length = length,
        .type = protocol::typeStream,
        .op = op,
        .flags = flags,
        .bufferAllocation = bufferAllocation,
        .forwardCount = forwardCount,
    };
}

/* The guest's driver, played against a device through its queues alone. */
class Guest final {
public:
    Guest()
        : gateway_(std::make_shared<Gateway>())
        , endpoint_(gateway_)
        , device_(gateway_)
    {
    }

    [[nodiscard]] Endpoint& endpoint() noexcept { return endpoint_; }
    [[nodiscard]] VirtioVsockDevice& device() noexcept { return device_; }

    void pump() { device_.drainEventSource(); }

    /* One transmit buffer; the device must complete it. */
    void transmit(
        const protocol::Header& header,
        std::span<const std::uint8_t> payload = {})
    {
        std::vector<std::uint8_t> bytes(sizeof(header) + payload.size());
        std::memcpy(bytes.data(), &header, sizeof(header));
        if (!payload.empty())
            std::memcpy(bytes.data() + sizeof(header), payload.data(), payload.size());
        const auto result = device_.processQueue(
            protocol::transmitQueue, ++requestId_, bytes, 0);
        require(result.action == VhostUserQueueAction::complete);
        require(result.response.empty());
    }

    /* One receive buffer of `capacity` bytes; nothing when the device has no
     * packet for it. The device never defers. */
    [[nodiscard]] std::optional<Packet> receive(std::size_t capacity = 4096)
    {
        const auto result = device_.processQueue(
            protocol::receiveQueue, ++requestId_, {}, capacity);
        require(result.action != VhostUserQueueAction::defer);
        if (result.action == VhostUserQueueAction::retry) {
            require(result.response.empty());
            return std::nullopt;
        }
        require(result.response.size() >= sizeof(protocol::Header));
        require(result.response.size() <= capacity);
        Packet packet;
        std::memcpy(&packet.header, result.response.data(), sizeof(packet.header));
        packet.payload.assign(
            result.response.begin() + sizeof(protocol::Header),
            result.response.end());
        require(packet.header.length == packet.payload.size());
        require(packet.header.destinationCid == protocol::guestCid);
        return packet;
    }

    /* The next packet, once the device's events have been drained. */
    [[nodiscard]] Packet expect(std::size_t capacity = 4096)
    {
        std::optional<Packet> packet;
        require(uml::testing::waitUntil(
                    [&] {
                        pump();
                        packet = receive(capacity);
                        return packet.has_value();
                    },
                    std::chrono::seconds(5)),
            "the device sent no packet");
        return std::move(*packet);
    }

    /* Whether the device has nothing for the guest after its events drain. */
    [[nodiscard]] bool quiet()
    {
        pump();
        return !receive().has_value();
    }

    /* The next packet, skipping credit updates. */
    [[nodiscard]] Packet expectNotCredit(std::size_t capacity = 4096)
    {
        for (;;) {
            auto packet = expect(capacity);
            if (packet.header.op != protocol::opCreditUpdate)
                return packet;
        }
    }

private:
    std::shared_ptr<Gateway> gateway_;
    Endpoint endpoint_;
    VirtioVsockDevice device_;
    std::uint64_t requestId_ {};
};

/* A connection opened and answered by the guest. */
struct Established final {
    int app {-1};
    std::uint32_t hostPort {};
    std::uint32_t guestPort {};

    Established() = default;
    Established(const Established&) = delete;
    Established& operator=(const Established&) = delete;
    Established(Established&& other) noexcept
        : app(other.app)
        , hostPort(other.hostPort)
        , guestPort(other.guestPort)
    {
        other.app = -1;
    }
    ~Established()
    {
        if (app >= 0)
            ::close(app);
    }

    void closeApp()
    {
        if (app >= 0)
            ::close(app);
        app = -1;
    }
};

inline Established establish(
    Guest& guest,
    std::uint32_t guestPort,
    std::uint32_t bufferAllocation = protocol::bufferAllocation)
{
    std::shared_ptr<Opening> opening;
    require(guest.endpoint().begin(guestPort, opening) == 0);
    const auto request = guest.expect();
    require(request.header.op == protocol::opRequest);
    require(request.header.type == protocol::typeStream);
    require(request.header.destinationPort == guestPort);
    require(request.header.bufferAllocation == protocol::bufferAllocation);
    Established connection;
    connection.hostPort = request.header.sourcePort;
    connection.guestPort = guestPort;
    guest.transmit(guestHeader(
        protocol::opResponse, guestPort, connection.hostPort, 0, 0,
        bufferAllocation, 0));
    connection.app = opening->wait(std::chrono::milliseconds(0));
    require(connection.app >= 0, "the open did not complete");
    return connection;
}

/* All that is readable from `descriptor` now, without blocking. */
inline std::vector<std::uint8_t> drain(int descriptor)
{
    std::vector<std::uint8_t> bytes;
    std::array<std::uint8_t, 65536> buffer {};
    for (;;) {
        const auto count = ::recv(descriptor, buffer.data(), buffer.size(), MSG_DONTWAIT);
        if (count <= 0)
            break;
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + count);
    }
    return bytes;
}

/* Whether `descriptor` reports end of stream. */
inline bool atEnd(int descriptor)
{
    std::uint8_t byte = 0;
    return ::recv(descriptor, &byte, 1, MSG_DONTWAIT) == 0;
}

} // namespace uml::vsock::testing
