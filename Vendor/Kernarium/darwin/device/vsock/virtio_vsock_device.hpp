// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "connection_table.hpp"
#include "endpoint.hpp"
#include "protocol.hpp"
#include "vhost_user_device.hpp"

#include <deque>
#include <memory>
#include <optional>
#include <vector>

namespace uml::vsock {

/* The host end of the guest's virtio-vsock device. The guest is CID 3 and the
 * host CID 2; the host only opens connections (Endpoint), and every
 * connection a guest tries to open is refused. README.md has the rules the
 * device keeps; how a connection ends:
 *
 *   - an RST before the answer fails the open with ECONNREFUSED; an opener
 *     that gives up before the REQUEST is sent drops it quietly, afterwards an
 *     RST goes at once and a late RESPONSE finds no connection and is reset;
 *   - more than the credit granted, or RW after SHUTDOWN SEND, is an RST;
 *   - SHUTDOWN SEND becomes shutdown(SHUT_WR) on the host's end after the held
 *     data, SHUTDOWN RCV stops reading it, and its end of stream sends
 *     SHUTDOWN SEND to the guest;
 *   - a broken host end (EPIPE, ECONNRESET) is an RST, as is a guest that is
 *     done sending and (done receiving or the host's end at end of stream),
 *     which also frees the entry;
 *   - a device reset, guest restart or service stop closes every host end and
 *     fails opens in flight with ECONNRESET. */
class VirtioVsockDevice final : public uml::virtio::VhostUserDevice {
public:
    explicit VirtioVsockDevice(std::shared_ptr<Gateway> gateway);
    ~VirtioVsockDevice() override;

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
    /* A transmit buffer is stream data the guest has handed over for good,
     * and it completes in the guest's own work queue with nothing waiting on
     * it, so it stays on a ring the guest disabled to suspend and runs after
     * the resume. */
    [[nodiscard]] bool keepsRequestsOnDisabledRing() const noexcept override
    {
        return true;
    }
    [[nodiscard]] int eventDescriptor() const noexcept override;
    void drainEventSource() override;

    /* The number of connections, open and opening, for tests. */
    [[nodiscard]] std::size_t connectionCount() const noexcept
    {
        return table_.size();
    }

private:
    using QueueResult = uml::virtio::VhostUserQueueResult;

    [[nodiscard]] QueueResult receive(std::size_t capacity);
    void transmit(std::span<const std::uint8_t> packet);

    void handleKnown(
        Connection& connection,
        const protocol::Header& header,
        std::span<const std::uint8_t> payload);
    void establish(Connection& connection);
    void receiveStream(
        Connection& connection,
        std::span<const std::uint8_t> payload);
    void refuse(const protocol::Header& offending);

    void open(Gateway::Request request);
    void abandon(const std::shared_ptr<Opening>& opening);

    void flushPending(Connection& connection);
    void afterWrite(Connection& connection);
    void finishIfDone(Connection& connection);
    /* Ends the connection toward the guest: the host's end closes at once
     * and an RST is owed. `keepShutdown` leaves an owed SHUTDOWN in front of
     * it. A connection whose REQUEST was never sent is dropped instead. */
    void scheduleReset(Connection& connection, bool keepShutdown);
    void terminate(Connection& connection, int openError);
    void closeStream(Connection& connection) noexcept;
    void refresh(Connection& connection);

    [[nodiscard]] protocol::Header headerFor(
        const Connection& connection,
        std::uint16_t op,
        std::uint32_t flags) const noexcept;
    void queueControl(Connection& connection);
    [[nodiscard]] std::optional<protocol::Header> nextControl();
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> nextData(
        std::size_t capacity);
    [[nodiscard]] bool hasReceiveWork() const;
    void wakeIfReceiveWork();

    std::shared_ptr<Gateway> gateway_;
    ConnectionTable table_;
    std::deque<ConnectionId> controlQueue_;
    std::deque<protocol::Header> refusals_;
    ConnectionId dataCursor_ {};
    std::size_t publishedSize_ {};
    bool wakeQueued_ {};
};

} // namespace uml::vsock
