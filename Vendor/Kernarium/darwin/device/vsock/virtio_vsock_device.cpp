// SPDX-License-Identifier: GPL-2.0-or-later

#include "virtio_vsock_device.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace uml::vsock {
namespace {

constexpr std::size_t headerSize = sizeof(protocol::Header);

/* A guest that never supplies a receive buffer cannot make the refusals of
 * its own packets grow without bound. */
constexpr std::size_t maximumRefusals = 256;

[[nodiscard]] bool wouldBlock() noexcept
{
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

} // namespace

VirtioVsockDevice::VirtioVsockDevice(std::shared_ptr<Gateway> gateway)
    : gateway_(std::move(gateway))
    , table_(Gateway::maximumConnections)
{
    if (!gateway_)
        throw std::invalid_argument("Virtio vsock requires a gateway");
}

VirtioVsockDevice::~VirtioVsockDevice()
{
    /* Opens not yet taken in fail as an open on a stopped service does; the
     * connections that were open end as a reset's do. */
    gateway_->close(ENODEV);
    reset();
}

const char* VirtioVsockDevice::name() const noexcept
{
    return "Virtio vsock";
}

std::uint64_t VirtioVsockDevice::features() const noexcept
{
    return protocol::features;
}

std::size_t VirtioVsockDevice::queueCount() const noexcept
{
    return protocol::queueCount;
}

std::vector<std::uint8_t> VirtioVsockDevice::configuration() const
{
    return uml::virtio::toBytes(
        protocol::Configuration {.guestCid = protocol::guestCid});
}

void VirtioVsockDevice::writeConfiguration(
    std::uint32_t,
    std::uint32_t,
    std::span<const std::uint8_t> bytes)
{
    if (!bytes.empty())
        throw std::runtime_error("Virtio vsock configuration is read-only");
}

int VirtioVsockDevice::eventDescriptor() const noexcept
{
    return gateway_->events().descriptor();
}

uml::virtio::VhostUserQueueResult VirtioVsockDevice::processQueue(
    std::uint32_t index,
    std::uint64_t,
    std::span<const std::uint8_t> request,
    std::size_t responseCapacity)
{
    switch (index) {
    case protocol::receiveQueue: {
        if (!request.empty())
            throw std::runtime_error("invalid Virtio vsock receive descriptor");
        auto result = receive(responseCapacity);
        if (table_.size() != publishedSize_) {
            publishedSize_ = table_.size();
            gateway_->publishLive(publishedSize_);
        }
        return result;
    }
    case protocol::transmitQueue:
        if (responseCapacity != 0)
            throw std::runtime_error("invalid Virtio vsock transmit descriptor");
        transmit(request);
        if (table_.size() != publishedSize_) {
            publishedSize_ = table_.size();
            gateway_->publishLive(publishedSize_);
        }
        /* Never deferred: a deferred request would reset the device, and
         * every connection with it, when the guest disables the ring to
         * suspend. */
        return {
            .action = uml::virtio::VhostUserQueueAction::complete,
            .response = {},
        };
    case protocol::eventQueue:
        /* The device sends no event: a device lives for one boot, so the
         * transport is never reset under the guest. The buffers stay. */
        return {};
    default:
        break;
    }
    throw std::runtime_error("invalid Virtio vsock queue index");
}

void VirtioVsockDevice::reset() noexcept
{
    try {
        std::vector<Gateway::Request> requests;
        std::vector<std::shared_ptr<Opening>> abandoned;
        gateway_->take(requests, abandoned);
        for (auto& request : requests)
            request.opening->fail(ECONNRESET);
    } catch (...) {
    }
    table_.forEach([](Connection& connection) {
        if (connection.opening)
            connection.opening->fail(ECONNRESET);
    });
    /* Closing the host ends is what the applications see as the stream
     * ending. */
    table_.clear();
    controlQueue_.clear();
    refusals_.clear();
    dataCursor_ = 0;
    wakeQueued_ = false;
    publishedSize_ = 0;
    gateway_->publishLive(0);
}

void VirtioVsockDevice::drainEventSource()
{
    wakeQueued_ = false;
    std::vector<HostEvents::Event> events;
    gateway_->events().collect(events);
    for (const auto& event : events) {
        if (event.kind == HostEvents::Kind::wake)
            continue;
        Connection* connection = table_.find(event.id);
        if (connection == nullptr || connection->stream.get() < 0)
            continue;
        if (event.kind == HostEvents::Kind::readable) {
            connection->readEnabled = false;
            connection->readable = true;
        } else {
            connection->writeEnabled = false;
            flushPending(*connection);
        }
        if (table_.find(event.id) != nullptr)
            refresh(*connection);
    }

    std::vector<Gateway::Request> requests;
    std::vector<std::shared_ptr<Opening>> abandoned;
    gateway_->take(requests, abandoned);
    for (auto& request : requests)
        open(std::move(request));
    for (const auto& opening : abandoned)
        abandon(opening);
    if (table_.size() != publishedSize_) {
        publishedSize_ = table_.size();
        gateway_->publishLive(publishedSize_);
    }
}

uml::virtio::VhostUserQueueResult VirtioVsockDevice::receive(
    std::size_t capacity)
{
    /* A buffer too small for a header cannot carry any packet; it waits. */
    if (capacity < headerSize)
        return {};
    for (;;) {
        std::optional<protocol::Header> header;
        if (!refusals_.empty()) {
            header = refusals_.front();
            refusals_.pop_front();
        } else {
            header = nextControl();
        }
        if (header) {
            return {
                .action = uml::virtio::VhostUserQueueAction::complete,
                .response = uml::virtio::toBytes(*header),
            };
        }
        if (auto packet = nextData(capacity)) {
            return {
                .action = uml::virtio::VhostUserQueueAction::complete,
                .response = std::move(*packet),
            };
        }
        /* Reading a connection can owe the guest a packet (end of stream, a
         * broken stream); that one goes out in this buffer. */
        if (refusals_.empty() && controlQueue_.empty())
            break;
    }
    return {};
}

protocol::Header VirtioVsockDevice::headerFor(
    const Connection& connection,
    std::uint16_t op,
    std::uint32_t flags) const noexcept
{
    return {
        .sourceCid = protocol::hostCid,
        .destinationCid = protocol::guestCid,
        .sourcePort = connection.hostPort,
        .destinationPort = connection.guestPort,
        .length = 0,
        .type = protocol::typeStream,
        .op = op,
        .flags = flags,
        .bufferAllocation = protocol::bufferAllocation,
        .forwardCount = connection.credit.forwarded,
    };
}

void VirtioVsockDevice::queueControl(Connection& connection)
{
    if (connection.queuedForControl || !connection.wantsControl())
        return;
    connection.queuedForControl = true;
    controlQueue_.push_back(connection.id);
}

std::optional<protocol::Header> VirtioVsockDevice::nextControl()
{
    while (!controlQueue_.empty()) {
        const ConnectionId id = controlQueue_.front();
        controlQueue_.pop_front();
        Connection* connection = table_.find(id);
        if (connection == nullptr)
            continue;
        connection->queuedForControl = false;
        protocol::Header header {};
        if (connection->wantRequest) {
            connection->wantRequest = false;
            connection->requestSent = true;
            header = headerFor(*connection, protocol::opRequest, 0);
        } else if (connection->wantShutdown != 0) {
            header = headerFor(
                *connection, protocol::opShutdown, connection->wantShutdown);
            connection->wantShutdown = 0;
        } else if (connection->wantCreditUpdate) {
            connection->wantCreditUpdate = false;
            header = headerFor(*connection, protocol::opCreditUpdate, 0);
        } else if (connection->wantReset) {
            header = headerFor(*connection, protocol::opReset, 0);
            table_.erase(id);
            return header;
        } else {
            continue;
        }
        connection->credit.advertised = connection->credit.forwarded;
        queueControl(*connection);
        return header;
    }
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> VirtioVsockDevice::nextData(
    std::size_t capacity)
{
    const std::size_t maximumPayload = capacity - headerSize;
    std::optional<std::vector<std::uint8_t>> packet;
    table_.forEachFrom(dataCursor_, [&](Connection& connection) {
        if (!connection.canSend())
            return false;
        const std::size_t room = std::min<std::size_t>(
            maximumPayload, connection.credit.peerFree());
        if (room == 0)
            return false;
        std::vector<std::uint8_t> bytes(headerSize + room);
        ssize_t count = 0;
        do {
            count = ::read(
                connection.stream.get(), bytes.data() + headerSize, room);
        } while (count < 0 && errno == EINTR);
        if (count > 0) {
            const auto length = static_cast<std::size_t>(count);
            bytes.resize(headerSize + length);
            /* A read that filled the buffer may have left more behind; a
             * short one drained the socket, and the filter is armed again to
             * hear when more comes. */
            connection.readable = length == room;
            auto header = headerFor(connection, protocol::opReadWrite, 0);
            header.length = static_cast<std::uint32_t>(length);
            connection.credit.sent += static_cast<std::uint32_t>(length);
            connection.credit.advertised = connection.credit.forwarded;
            std::memcpy(bytes.data(), &header, headerSize);
            dataCursor_ = connection.id;
            refresh(connection);
            packet = std::move(bytes);
            return true;
        }
        if (count == 0) {
            connection.readable = false;
            connection.hostEof = true;
            connection.wantShutdown |= protocol::shutdownSend;
            queueControl(connection);
            finishIfDone(connection);
            refresh(connection);
            return false;
        }
        if (wouldBlock()) {
            connection.readable = false;
            refresh(connection);
            return false;
        }
        scheduleReset(connection, false);
        return false;
    });
    return packet;
}

void VirtioVsockDevice::transmit(std::span<const std::uint8_t> packet)
{
    if (packet.size() < headerSize)
        return;
    protocol::Header header {};
    std::memcpy(&header, packet.data(), headerSize);
    const auto payload = packet.subspan(headerSize);
    if (header.length != payload.size())
        return;
    Connection* connection = nullptr;
    if (header.type == protocol::typeStream &&
        header.destinationCid == protocol::hostCid &&
        header.sourceCid == protocol::guestCid) {
        connection =
            table_.find(header.destinationPort, header.sourcePort);
    }
    if (connection == nullptr) {
        /* No listener, no such connection, another CID or another socket
         * type: all refused. An RST is never answered. */
        if (header.op != protocol::opReset)
            refuse(header);
    } else {
        handleKnown(*connection, header, payload);
    }
    wakeIfReceiveWork();
}

void VirtioVsockDevice::refuse(const protocol::Header& offending)
{
    if (refusals_.size() >= maximumRefusals)
        return;
    refusals_.push_back({
        .sourceCid = offending.destinationCid,
        .destinationCid = offending.sourceCid,
        .sourcePort = offending.destinationPort,
        .destinationPort = offending.sourcePort,
        .length = 0,
        .type = offending.type,
        .op = protocol::opReset,
        .flags = 0,
        .bufferAllocation = 0,
        .forwardCount = 0,
    });
}

void VirtioVsockDevice::handleKnown(
    Connection& connection,
    const protocol::Header& header,
    std::span<const std::uint8_t> payload)
{
    if (connection.wantReset) {
        /* The connection is only waiting to tell the guest it is over. */
        if (header.op == protocol::opReset)
            terminate(connection, ECONNRESET);
        return;
    }
    connection.credit.peerBufferAllocation = header.bufferAllocation;
    connection.credit.peerForwardCount = header.forwardCount;
    if (connection.phase == Connection::Phase::requesting) {
        if (header.op == protocol::opResponse)
            establish(connection);
        else if (header.op == protocol::opReset)
            terminate(connection, ECONNREFUSED);
        return;
    }
    switch (header.op) {
    case protocol::opReset:
        terminate(connection, ECONNRESET);
        return;
    case protocol::opShutdown:
        if ((header.flags & protocol::shutdownReceive) != 0)
            connection.guestShutReceive = true;
        if ((header.flags & protocol::shutdownSend) != 0)
            connection.guestShutSend = true;
        afterWrite(connection);
        return;
    case protocol::opReadWrite:
        receiveStream(connection, payload);
        return;
    case protocol::opCreditRequest:
        connection.wantCreditUpdate = true;
        queueControl(connection);
        break;
    default:
        /* CREDIT_UPDATE says nothing more than the fields every packet
         * carries. A REQUEST or RESPONSE for a connection that exists is a
         * stray the guest repeated. */
        break;
    }
    refresh(connection);
}

void VirtioVsockDevice::establish(Connection& connection)
{
    connection.phase = Connection::Phase::established;
    connection.readable = false;
    const auto opening = std::move(connection.opening);
    connection.opening.reset();
    if (!opening || !opening->succeed()) {
        /* The opener gave up before the guest answered. */
        scheduleReset(connection, false);
        return;
    }
    refresh(connection);
}

void VirtioVsockDevice::receiveStream(
    Connection& connection,
    std::span<const std::uint8_t> payload)
{
    const auto length = static_cast<std::uint32_t>(payload.size());
    /* More than the credit the guest was given, or data after it said it
     * would send no more, is a violation: the connection ends. */
    if (connection.guestShutSend || !connection.credit.accepts(length)) {
        scheduleReset(connection, false);
        return;
    }
    connection.credit.received += length;
    std::size_t written = 0;
    if (connection.pending.empty()) {
        while (written < payload.size()) {
            const ssize_t count = ::write(
                connection.stream.get(),
                payload.data() + written,
                payload.size() - written);
            if (count > 0) {
                written += static_cast<std::size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0 && wouldBlock())
                break;
            scheduleReset(connection, false);
            return;
        }
    }
    connection.credit.forwarded += static_cast<std::uint32_t>(written);
    if (written < payload.size()) {
        connection.pending.emplace_back(
            payload.begin() + static_cast<std::ptrdiff_t>(written),
            payload.end());
    }
    afterWrite(connection);
}

void VirtioVsockDevice::flushPending(Connection& connection)
{
    while (!connection.pending.empty() && connection.stream.get() >= 0) {
        const auto& chunk = connection.pending.front();
        const ssize_t count = ::write(
            connection.stream.get(),
            chunk.data() + connection.pendingOffset,
            chunk.size() - connection.pendingOffset);
        if (count > 0) {
            connection.credit.forwarded += static_cast<std::uint32_t>(count);
            connection.pendingOffset += static_cast<std::size_t>(count);
            if (connection.pendingOffset == chunk.size()) {
                connection.pending.pop_front();
                connection.pendingOffset = 0;
            }
            continue;
        }
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && wouldBlock())
            break;
        scheduleReset(connection, false);
        return;
    }
    afterWrite(connection);
}

void VirtioVsockDevice::afterWrite(Connection& connection)
{
    if (connection.wantReset)
        return;
    if (connection.stream.get() >= 0 && connection.pending.empty() &&
        connection.guestShutSend && !connection.hostWriteShut) {
        (void)::shutdown(connection.stream.get(), SHUT_WR);
        connection.hostWriteShut = true;
    }
    if (connection.credit.updateDue()) {
        connection.wantCreditUpdate = true;
        queueControl(connection);
    }
    finishIfDone(connection);
    refresh(connection);
}

void VirtioVsockDevice::finishIfDone(Connection& connection)
{
    if (connection.wantReset ||
        connection.phase != Connection::Phase::established) {
        return;
    }
    /* The guest will send nothing more and everything it sent has reached
     * the host, and it either closed (no receive either) or has been told
     * the host sends nothing more: both directions are over. */
    if (connection.guestShutSend && connection.pending.empty() &&
        (connection.guestShutReceive || connection.hostEof)) {
        scheduleReset(connection, true);
    }
}

void VirtioVsockDevice::scheduleReset(Connection& connection, bool keepShutdown)
{
    if (connection.wantReset)
        return;
    closeStream(connection);
    connection.pending.clear();
    connection.pendingOffset = 0;
    connection.wantCreditUpdate = false;
    connection.wantRequest = false;
    if (!keepShutdown)
        connection.wantShutdown = 0;
    connection.wantReset = true;
    queueControl(connection);
}

void VirtioVsockDevice::terminate(Connection& connection, int openError)
{
    if (connection.opening)
        connection.opening->fail(openError != 0 ? openError : ECONNRESET);
    table_.erase(connection.id);
}

void VirtioVsockDevice::closeStream(Connection& connection) noexcept
{
    connection.stream.reset();
    connection.readEnabled = false;
    connection.writeEnabled = false;
    connection.readable = false;
}

void VirtioVsockDevice::refresh(Connection& connection)
{
    if (connection.stream.get() < 0)
        return;
    auto& events = gateway_->events();
    const bool wantRead =
        connection.phase == Connection::Phase::established &&
        !connection.readable && !connection.hostEof &&
        !connection.guestShutReceive && !connection.wantReset &&
        connection.credit.peerFree() != 0;
    if (wantRead != connection.readEnabled) {
        events.setReadEnabled(connection.stream.get(), connection.id, wantRead);
        connection.readEnabled = wantRead;
    }
    const bool wantWrite = !connection.pending.empty() && !connection.wantReset;
    if (wantWrite != connection.writeEnabled) {
        events.setWriteEnabled(
            connection.stream.get(), connection.id, wantWrite);
        connection.writeEnabled = wantWrite;
    }
}

void VirtioVsockDevice::open(Gateway::Request request)
{
    Connection* connection = table_.create(
        request.guestPort, std::move(request.hostEnd), request.opening);
    if (connection == nullptr) {
        request.opening->fail(EMFILE);
        return;
    }
    try {
        gateway_->events().watch(connection->stream.get(), connection->id);
    } catch (...) {
        request.opening->fail(EIO);
        table_.erase(connection->id);
        return;
    }
    connection->wantRequest = true;
    queueControl(*connection);
}

void VirtioVsockDevice::abandon(const std::shared_ptr<Opening>& opening)
{
    Connection* connection = table_.findOpening(opening.get());
    if (connection == nullptr)
        return;
    if (!connection->requestSent) {
        /* The guest never heard of it. */
        terminate(*connection, ECANCELED);
        return;
    }
    scheduleReset(*connection, false);
}

bool VirtioVsockDevice::hasReceiveWork() const
{
    if (!refusals_.empty() || !controlQueue_.empty())
        return true;
    bool sendable = false;
    table_.forEach([&](const Connection& connection) {
        sendable = sendable || connection.canSend();
    });
    return sendable;
}

void VirtioVsockDevice::wakeIfReceiveWork()
{
    if (wakeQueued_ || !hasReceiveWork())
        return;
    wakeQueued_ = true;
    gateway_->events().trigger();
}

} // namespace uml::vsock
