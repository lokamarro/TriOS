// SPDX-License-Identifier: GPL-2.0-or-later

#include "named_tests.hpp"
#include "vsock_test_guest.hpp"
#include "watchdog.hpp"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <numeric>
#include <set>
#include <thread>

#include <poll.h>

namespace {

using namespace uml::vsock;
using namespace uml::vsock::testing;
using std::chrono::milliseconds;

constexpr std::uint32_t port = 5000;

std::vector<std::uint8_t> pattern(std::size_t size, std::uint8_t seed = 1)
{
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t index = 0; index < size; ++index)
        bytes[index] = static_cast<std::uint8_t>(seed + index * 7U + index / 251U);
    return bytes;
}

void writeAll(int descriptor, const std::vector<std::uint8_t>& bytes)
{
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto count = ::send(
            descriptor, bytes.data() + done, bytes.size() - done, MSG_DONTWAIT);
        if (count > 0) {
            done += static_cast<std::size_t>(count);
            continue;
        }
        require(count < 0 && (errno == EAGAIN || errno == EINTR), "app write failed");
        std::this_thread::sleep_for(milliseconds(1));
    }
}

void describesTheDevice()
{
    Guest guest;
    auto& device = guest.device();
    require(device.features() == 0);
    require(device.queueCount() == 3);
    const auto configuration = device.configuration();
    require(configuration.size() == 8);
    std::uint64_t cid = 0;
    std::memcpy(&cid, configuration.data(), sizeof(cid));
    require(cid == 3);
    /* Stream data on a ring the guest disabled to suspend is kept. */
    require(device.keepsRequestsOnDisabledRing());
    require(device.eventDescriptor() >= 0);
}

void opensAndCarriesDataBothWays()
{
    Guest guest;
    auto connection = establish(guest, port);
    require(connection.hostPort >= 49152 && connection.hostPort <= 65535);

    const auto toGuest = pattern(3000);
    writeAll(connection.app, toGuest);
    auto data = guest.expect();
    require(data.header.op == protocol::opReadWrite);
    require(data.header.sourcePort == connection.hostPort);
    require(data.header.destinationPort == port);
    require(data.header.bufferAllocation == protocol::bufferAllocation);
    require(data.payload == std::vector<std::uint8_t>(
        toGuest.begin(), toGuest.begin() + data.payload.size()));
    std::vector<std::uint8_t> received = data.payload;
    while (received.size() < toGuest.size()) {
        data = guest.expect();
        require(data.header.op == protocol::opReadWrite);
        received.insert(received.end(), data.payload.begin(), data.payload.end());
    }
    require(received == toGuest);

    const auto toHost = pattern(9000, 9);
    for (std::size_t offset = 0; offset < toHost.size(); offset += 4000) {
        const auto size = std::min<std::size_t>(4000, toHost.size() - offset);
        guest.transmit(
            guestHeader(
                protocol::opReadWrite, port, connection.hostPort,
                static_cast<std::uint32_t>(size)),
            std::span(toHost).subspan(offset, size));
    }
    std::vector<std::uint8_t> arrived;
    require(uml::testing::waitUntil(
        [&] {
            const auto more = drain(connection.app);
            arrived.insert(arrived.end(), more.begin(), more.end());
            return arrived.size() >= toHost.size();
        },
        std::chrono::seconds(5)));
    require(arrived == toHost);
}

void everyPacketCarriesTheBufferAndForwardCount()
{
    Guest guest;
    auto connection = establish(guest, port);
    const auto bytes = pattern(1000);
    guest.transmit(
        guestHeader(protocol::opReadWrite, port, connection.hostPort, 1000), bytes);
    guest.transmit(guestHeader(
        protocol::opCreditRequest, port, connection.hostPort));
    auto update = guest.expect();
    require(update.header.op == protocol::opCreditUpdate);
    require(update.header.bufferAllocation == protocol::bufferAllocation);
    require(update.header.forwardCount == 1000);
    writeAll(connection.app, pattern(10));
    const auto data = guest.expect();
    require(data.header.op == protocol::opReadWrite);
    require(data.header.forwardCount == 1000);
}

void refusalOfTheOpenIsConnectionRefused()
{
    Guest guest;
    std::shared_ptr<Opening> opening;
    require(guest.endpoint().begin(port, opening) == 0);
    const auto request = guest.expect();
    require(request.header.op == protocol::opRequest);
    guest.transmit(guestHeader(
        protocol::opReset, port, request.header.sourcePort));
    require(opening->wait(milliseconds(0)) == -ECONNREFUSED);
    require(guest.device().connectionCount() == 0);
    /* An RST is not answered. */
    require(guest.quiet());
}

void anOpenTimesOutAndALateResponseIsReset()
{
    Guest guest;
    std::shared_ptr<Opening> opening;
    require(guest.endpoint().begin(port, opening) == 0);
    const auto request = guest.expect();
    require(request.header.op == protocol::opRequest);
    require(opening->wait(milliseconds(0)) == -ETIMEDOUT);
    /* The guest has heard of the connection, so it is told it is over. */
    const auto reset = guest.expect();
    require(reset.header.op == protocol::opReset);
    require(reset.header.sourcePort == request.header.sourcePort);
    require(reset.header.destinationPort == port);
    /* The response that was already on its way finds no connection. */
    guest.transmit(guestHeader(
        protocol::opResponse, port, request.header.sourcePort));
    const auto refusal = guest.expect();
    require(refusal.header.op == protocol::opReset);
    require(refusal.header.sourcePort == request.header.sourcePort);
    require(guest.device().connectionCount() == 0);
    require(guest.quiet());
}

void anOpenAbandonedBeforeItIsSentIsDroppedQuietly()
{
    Guest guest;
    std::shared_ptr<Opening> opening;
    /* Taken in by the device, which has no receive buffer for the request
     * until the guest supplies one, and abandoned before it does. */
    require(guest.endpoint().begin(port, opening) == 0);
    guest.pump();
    require(guest.device().connectionCount() == 1);
    require(opening->wait(milliseconds(0)) == -ETIMEDOUT);
    guest.pump();
    require(guest.device().connectionCount() == 0);
    require(guest.quiet());
}

void cancellingAnOpenEndsTheWaitAndTheConnection()
{
    Guest guest;
    std::shared_ptr<Opening> opening;
    require(guest.endpoint().begin(port, opening) == 0);
    const auto request = guest.expect();
    require(request.header.op == protocol::opRequest);
    std::atomic_int result {0};
    std::thread waiter([&] { result = opening->wait(std::chrono::seconds(30)); });
    std::this_thread::sleep_for(milliseconds(20));
    opening->cancel();
    waiter.join();
    require(result.load() == -ECANCELED);
    /* The guest has heard of it, so it is reset. */
    const auto reset = guest.expect();
    require(reset.header.op == protocol::opReset);
    require(guest.device().connectionCount() == 0);
}

void anOpenWithoutAReceiveBufferTimesOut()
{
    Guest guest;
    std::shared_ptr<Opening> opening;
    require(guest.endpoint().begin(port, opening) == 0);
    guest.pump();
    /* The driver never supplied a receive buffer. */
    require(opening->wait(milliseconds(20)) == -ETIMEDOUT);
}

void theGuestStallsOnItsCreditAndResumes()
{
    Guest guest;
    auto connection = establish(guest, port);
    /* The application reads nothing, so the data waits in the host's socket
     * buffers and then in the device, whose buffer is all the guest has
     * credit for. */
    std::uint32_t sent = 0;
    std::uint32_t advertisedForward = 0;
    std::uint32_t advertisedAllocation = protocol::bufferAllocation;
    const auto chunk = pattern(65536, 3);
    std::vector<std::uint8_t> expected;
    const auto credit = [&] {
        const std::uint32_t inFlight = sent - advertisedForward;
        return inFlight >= advertisedAllocation ? 0U : advertisedAllocation - inFlight;
    };
    const auto absorb = [&](const Packet& packet) {
        require(packet.header.op == protocol::opCreditUpdate);
        advertisedForward = packet.header.forwardCount;
        advertisedAllocation = packet.header.bufferAllocation;
    };
    std::size_t packets = 0;
    while (credit() >= 1000) {
        const auto size = std::min<std::uint32_t>(chunk.size(), credit());
        guest.transmit(
            guestHeader(
                protocol::opReadWrite, port, connection.hostPort, size,
                0, protocol::bufferAllocation, 0),
            std::span(chunk).first(size));
        expected.insert(expected.end(), chunk.begin(), chunk.begin() + size);
        sent += size;
        ++packets;
        while (auto packet = guest.receive())
            absorb(*packet);
    }
    require(packets > 4);
    /* Nothing the guest may send now would fit. */
    require(credit() < 1000);
    /* Draining the application's end moves the device's buffer into it and
     * the guest earns credit back. */
    std::vector<std::uint8_t> arrived;
    require(uml::testing::waitUntil(
        [&] {
            guest.pump();
            while (auto packet = guest.receive())
                absorb(*packet);
            const auto more = drain(connection.app);
            arrived.insert(arrived.end(), more.begin(), more.end());
            return arrived.size() == expected.size();
        },
        std::chrono::seconds(5)));
    require(arrived == expected);
    guest.pump();
    while (auto packet = guest.receive())
        absorb(*packet);
    require(advertisedForward == sent);
    require(credit() == protocol::bufferAllocation);
}

void aCreditUpdateGoesOutEverySixtyFourKibibytes()
{
    Guest guest;
    auto connection = establish(guest, port);
    const auto chunk = pattern(32768);
    unsigned updates = 0;
    std::uint32_t lastForward = 0;
    for (unsigned index = 0; index < 8; ++index) {
        guest.transmit(
            guestHeader(
                protocol::opReadWrite, port, connection.hostPort, 32768),
            chunk);
        guest.pump();
        while (auto packet = guest.receive()) {
            require(packet->header.op == protocol::opCreditUpdate);
            require(packet->header.forwardCount - lastForward >= 65536);
            lastForward = packet->header.forwardCount;
            ++updates;
        }
        (void)drain(connection.app);
    }
    require(updates == 4);
    require(lastForward == 8U * 32768U);
}

void theHostStopsAtTheGuestsCredit()
{
    Guest guest;
    /* The guest grants 10000 bytes. */
    auto connection = establish(guest, port, 10000);
    writeAll(connection.app, pattern(200000));
    std::size_t received = 0;
    for (;;) {
        guest.pump();
        auto packet = guest.receive();
        if (!packet)
            break;
        require(packet->header.op == protocol::opReadWrite);
        received += packet->payload.size();
    }
    require(received == 10000);
    require(guest.quiet());
    /* More credit lets more through, and never more than was granted. */
    guest.transmit(guestHeader(
        protocol::opCreditUpdate, port, connection.hostPort, 0, 0, 10000, 6000));
    std::size_t more = 0;
    for (;;) {
        guest.pump();
        auto packet = guest.receive();
        if (!packet)
            break;
        more += packet->payload.size();
    }
    require(more == 6000);
}

void aGuestExceedingItsCreditIsReset()
{
    Guest guest;
    auto connection = establish(guest, port);
    /* A guest that sends without regard to the credit it was given fills
     * the host's socket, then the device's buffer, and is reset when the next
     * packet does not fit what it was granted. The application reads
     * nothing. */
    const auto bytes = pattern(65536);
    bool exceeded = false;
    for (unsigned index = 0; index < 64 && !exceeded; ++index) {
        guest.transmit(
            guestHeader(protocol::opReadWrite, port, connection.hostPort, 65536),
            bytes);
        guest.pump();
        while (auto packet = guest.receive()) {
            if (packet->header.op == protocol::opReset) {
                exceeded = true;
                break;
            }
        }
    }
    require(exceeded, "the device never reset a guest past its credit");
    require(guest.quiet());
    require(guest.device().connectionCount() == 0);
    /* Reading what arrived ends in end of stream. */
    for (;;) {
        std::uint8_t buffer[65536];
        const auto count = ::recv(connection.app, buffer, sizeof(buffer), MSG_DONTWAIT);
        if (count == 0)
            break;
        require(count > 0 || errno != EAGAIN, "the application's end stayed open");
    }
}

void aPacketLargerThanTheWholeBufferIsReset()
{
    Guest guest;
    auto connection = establish(guest, port);
    const auto bytes = pattern(protocol::bufferAllocation + 1);
    guest.transmit(
        guestHeader(
            protocol::opReadWrite, port, connection.hostPort,
            protocol::bufferAllocation + 1),
        bytes);
    const auto reset = guest.expect();
    require(reset.header.op == protocol::opReset);
    require(atEnd(connection.app));
}

void aGuestHalfCloseEndsTheHostsStreamAfterItsData()
{
    Guest guest;
    auto connection = establish(guest, port);
    const auto bytes = pattern(100000);
    for (std::size_t offset = 0; offset < bytes.size(); offset += 50000) {
        guest.transmit(
            guestHeader(protocol::opReadWrite, port, connection.hostPort, 50000),
            std::span(bytes).subspan(offset, 50000));
    }
    guest.transmit(guestHeader(
        protocol::opShutdown, port, connection.hostPort, 0,
        protocol::shutdownSend));
    std::vector<std::uint8_t> arrived;
    require(uml::testing::waitUntil(
        [&] {
            guest.pump();
            while (guest.receive()) {
            }
            const auto more = drain(connection.app);
            arrived.insert(arrived.end(), more.begin(), more.end());
            return arrived.size() == bytes.size() && atEnd(connection.app);
        },
        std::chrono::seconds(5)));
    require(arrived == bytes);
    /* The host can still answer. */
    writeAll(connection.app, pattern(500));
    const auto reply = guest.expectNotCredit();
    require(reply.header.op == protocol::opReadWrite);
    require(reply.payload.size() == 500);
    /* And ending its own side finishes both: an RST, and the entry goes. */
    ::shutdown(connection.app, SHUT_WR);
    const auto shutdown = guest.expectNotCredit();
    require(shutdown.header.op == protocol::opShutdown);
    require(shutdown.header.flags == protocol::shutdownSend);
    const auto reset = guest.expectNotCredit();
    require(reset.header.op == protocol::opReset);
    require(guest.device().connectionCount() == 0);
}

void aHostHalfCloseIsToldToTheGuestWhichKeepsSending()
{
    Guest guest;
    auto connection = establish(guest, port);
    writeAll(connection.app, pattern(5000));
    ::shutdown(connection.app, SHUT_WR);
    std::size_t received = 0;
    bool ended = false;
    while (!ended) {
        const auto packet = guest.expectNotCredit();
        if (packet.header.op == protocol::opReadWrite) {
            received += packet.payload.size();
        } else {
            require(packet.header.op == protocol::opShutdown);
            require(packet.header.flags == protocol::shutdownSend);
            ended = true;
        }
    }
    require(received == 5000);
    /* The guest still sends to the host. */
    guest.transmit(
        guestHeader(protocol::opReadWrite, port, connection.hostPort, 700),
        pattern(700));
    std::vector<std::uint8_t> arrived;
    require(uml::testing::waitUntil(
        [&] {
            const auto more = drain(connection.app);
            arrived.insert(arrived.end(), more.begin(), more.end());
            return arrived.size() == 700;
        },
        std::chrono::seconds(5)));
}

void aGuestCloseIsAnsweredWithAnReset()
{
    Guest guest;
    auto connection = establish(guest, port);
    guest.transmit(guestHeader(
        protocol::opShutdown, port, connection.hostPort, 0,
        protocol::shutdownSend | protocol::shutdownReceive));
    const auto reset = guest.expectNotCredit();
    require(reset.header.op == protocol::opReset);
    require(guest.device().connectionCount() == 0);
    require(atEnd(connection.app));
}

void theApplicationClosingEndsTheStreamAndAnotherWriteResets()
{
    Guest guest;
    auto connection = establish(guest, port);
    connection.closeApp();
    const auto shutdown = guest.expectNotCredit();
    require(shutdown.header.op == protocol::opShutdown);
    require(shutdown.header.flags == protocol::shutdownSend);
    /* Data for an application that is gone resets the connection. */
    guest.transmit(
        guestHeader(protocol::opReadWrite, port, connection.hostPort, 10),
        pattern(10));
    const auto reset = guest.expectNotCredit();
    require(reset.header.op == protocol::opReset);
    require(guest.device().connectionCount() == 0);
}

void aGuestResetEndsTheApplicationsStream()
{
    Guest guest;
    auto connection = establish(guest, port);
    guest.transmit(guestHeader(protocol::opReset, port, connection.hostPort));
    require(atEnd(connection.app));
    require(guest.device().connectionCount() == 0);
    require(guest.quiet());
}

void everyGuestRequestIsReset()
{
    Guest guest;
    guest.transmit(guestHeader(protocol::opRequest, 1234, 80));
    auto reset = guest.expect();
    require(reset.header.op == protocol::opReset);
    require(reset.header.sourceCid == protocol::hostCid);
    require(reset.header.destinationCid == protocol::guestCid);
    require(reset.header.sourcePort == 80);
    require(reset.header.destinationPort == 1234);
    require(reset.header.type == protocol::typeStream);

    /* Another CID: the reset comes from the CID that was asked for. */
    auto elsewhere = guestHeader(protocol::opRequest, 1234, 80);
    elsewhere.destinationCid = 7;
    guest.transmit(elsewhere);
    {
        std::optional<Packet> packet;
        require(uml::testing::waitUntil(
            [&] {
                guest.pump();
                packet = guest.receive();
                return packet.has_value();
            },
            std::chrono::seconds(5)));
        require(packet->header.op == protocol::opReset);
        require(packet->header.sourceCid == 7);
        require(packet->header.destinationCid == protocol::guestCid);
    }

    /* A socket type the device does not offer. */
    auto seqpacket = guestHeader(protocol::opRequest, 99, 80);
    seqpacket.type = 2;
    guest.transmit(seqpacket);
    {
        std::optional<Packet> packet;
        require(uml::testing::waitUntil(
            [&] {
                guest.pump();
                packet = guest.receive();
                return packet.has_value();
            },
            std::chrono::seconds(5)));
        require(packet->header.op == protocol::opReset);
        require(packet->header.type == 2);
    }

    /* Stray data and credit packets are reset; an RST is not answered. */
    guest.transmit(guestHeader(protocol::opReadWrite, 5, 6, 3), pattern(3));
    guest.transmit(guestHeader(protocol::opReset, 5, 6));
    guest.transmit(guestHeader(protocol::opCreditUpdate, 5, 6));
    unsigned resets = 0;
    guest.pump();
    while (auto packet = guest.receive()) {
        require(packet->header.op == protocol::opReset);
        ++resets;
    }
    require(resets == 2);
    require(guest.device().connectionCount() == 0);
}

void aMalformedPacketIsDropped()
{
    Guest guest;
    std::array<std::uint8_t, 10> shortPacket {};
    auto result = guest.device().processQueue(
        protocol::transmitQueue, 1, shortPacket, 0);
    require(result.action == VhostUserQueueAction::complete);
    auto header = guestHeader(protocol::opRequest, 1, 2, 5);
    result = guest.device().processQueue(
        protocol::transmitQueue, 2,
        uml::virtio::toBytes(header), 0);
    require(result.action == VhostUserQueueAction::complete);
    require(guest.quiet());
}

void aResetClosesEveryHostEnd()
{
    Guest guest;
    auto first = establish(guest, port);
    auto second = establish(guest, port + 1);
    std::shared_ptr<Opening> pending;
    require(guest.endpoint().begin(port + 2, pending) == 0);
    guest.pump();
    require(guest.device().connectionCount() == 3);
    guest.device().reset();
    require(guest.device().connectionCount() == 0);
    require(atEnd(first.app));
    require(atEnd(second.app));
    require(pending->wait(milliseconds(0)) == -ECONNRESET);
    require(guest.quiet());
    /* And the device serves the next open as it did the first. */
    auto third = establish(guest, port);
    require(third.app >= 0);
}

void anOpenQueuedBeforeAResetFailsWithIt()
{
    Guest guest;
    std::shared_ptr<Opening> opening;
    require(guest.endpoint().begin(port, opening) == 0);
    guest.device().reset();
    require(opening->wait(milliseconds(0)) == -ECONNRESET);
}

void theDeviceEndingRefusesLaterOpens()
{
    std::shared_ptr<Gateway> gateway = std::make_shared<Gateway>();
    Endpoint endpoint(gateway);
    std::shared_ptr<Opening> queued;
    {
        VirtioVsockDevice device(gateway);
        require(endpoint.begin(port, queued) == 0);
    }
    require(queued->wait(milliseconds(0)) == -ENODEV);
    std::shared_ptr<Opening> later;
    require(endpoint.begin(port, later) == -ENODEV);
    require(endpoint.connect(port, milliseconds(0)) == -ENODEV);
}

void thirtyTwoConnectionsAreTheLimit()
{
    Guest guest;
    std::vector<Established> connections;
    std::set<std::uint32_t> hostPorts;
    for (std::uint32_t index = 0; index < 32; ++index) {
        connections.push_back(establish(guest, port + index));
        require(hostPorts.insert(connections.back().hostPort).second);
    }
    std::shared_ptr<Opening> opening;
    require(guest.endpoint().begin(port + 40, opening) == -EMFILE);
    /* One ends, and the next open takes a port nobody uses. */
    const auto freed = connections[5].hostPort;
    guest.transmit(guestHeader(
        protocol::opReset, connections[5].guestPort, freed));
    require(guest.device().connectionCount() == 31);
    guest.pump();
    auto again = establish(guest, port + 40);
    require(again.hostPort != freed);
    require(hostPorts.insert(again.hostPort).second);
}

void aTableFullOfOpensRefusesTheRest()
{
    Guest guest;
    std::vector<std::shared_ptr<Opening>> openings;
    for (unsigned index = 0; index < 32; ++index) {
        openings.emplace_back();
        require(guest.endpoint().begin(port + index, openings.back()) == 0);
    }
    std::shared_ptr<Opening> extra;
    require(guest.endpoint().begin(port + 40, extra) == -EMFILE);
    guest.pump();
    require(guest.device().connectionCount() == 32);
}

void theEventQueueAlwaysRetries()
{
    Guest guest;
    for (unsigned index = 0; index < 4; ++index) {
        const auto result = guest.device().processQueue(
            protocol::eventQueue, index, {}, 8);
        require(result.action == VhostUserQueueAction::retry);
        require(result.response.empty());
    }
    /* Even with connections and traffic around it. */
    auto connection = establish(guest, port);
    writeAll(connection.app, pattern(100));
    guest.pump();
    require(guest.device().processQueue(protocol::eventQueue, 9, {}, 8).action ==
        VhostUserQueueAction::retry);
}

void aReceiveBufferTooSmallForAHeaderWaits()
{
    Guest guest;
    std::shared_ptr<Opening> opening;
    require(guest.endpoint().begin(port, opening) == 0);
    guest.pump();
    require(!guest.receive(sizeof(protocol::Header) - 1).has_value());
    /* A buffer with room for the header and no data still carries it. */
    const auto request = guest.receive(sizeof(protocol::Header));
    require(request.has_value());
    require(request->header.op == protocol::opRequest);
}

void dataFillsTheBufferItIsGiven()
{
    Guest guest;
    auto connection = establish(guest, port);
    writeAll(connection.app, pattern(10000));
    const auto first = guest.expect(1000);
    require(first.header.op == protocol::opReadWrite);
    require(first.payload.size() == 1000 - sizeof(protocol::Header));
    const auto second = guest.expect(64);
    require(second.payload.size() == 64 - sizeof(protocol::Header));
}

void connectionsShareTheReceiveBuffersFairly()
{
    Guest guest;
    auto a = establish(guest, port);
    auto b = establish(guest, port + 1);
    writeAll(a.app, pattern(40000, 1));
    writeAll(b.app, pattern(40000, 2));
    std::size_t fromA = 0;
    std::size_t fromB = 0;
    for (unsigned index = 0; index < 20; ++index) {
        const auto packet = guest.expectNotCredit();
        require(packet.header.op == protocol::opReadWrite);
        (packet.header.sourcePort == a.hostPort ? fromA : fromB) += 1;
    }
    require(fromA > 5 && fromB > 5);
}

void attachesAnAcceptedStream()
{
    Guest guest;
    std::array<int, 2> pair {};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair.data()) == 0);
    uml::gpu::UniqueDescriptor client {pair[0]};
    std::shared_ptr<Opening> opening;
    require(guest.endpoint().beginAttach(
                uml::gpu::UniqueDescriptor {pair[1]}, port, opening) == 0);
    const auto request = guest.expect();
    require(request.header.op == protocol::opRequest);
    guest.transmit(guestHeader(
        protocol::opResponse, port, request.header.sourcePort));
    /* An attached stream has no descriptor to hand back. */
    require(opening->wait(milliseconds(0)) == 0);
    writeAll(client.get(), pattern(300));
    const auto data = guest.expect();
    require(data.payload.size() == 300);
    guest.transmit(
        guestHeader(protocol::opReadWrite, port, request.header.sourcePort, 40),
        pattern(40));
    std::vector<std::uint8_t> arrived;
    require(uml::testing::waitUntil(
        [&] {
            const auto more = drain(client.get());
            arrived.insert(arrived.end(), more.begin(), more.end());
            return arrived.size() == 40;
        },
        std::chrono::seconds(5)));
}

void aRefusedAttachClosesTheStream()
{
    Guest guest;
    std::array<int, 2> pair {};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair.data()) == 0);
    uml::gpu::UniqueDescriptor client {pair[0]};
    std::shared_ptr<Opening> opening;
    require(guest.endpoint().beginAttach(
                uml::gpu::UniqueDescriptor {pair[1]}, port, opening) == 0);
    const auto request = guest.expect();
    guest.transmit(guestHeader(protocol::opReset, port, request.header.sourcePort));
    require(opening->wait(milliseconds(0)) == -ECONNREFUSED);
    require(atEnd(client.get()));
}

void aBlockedConnectCompletesFromAnotherThread()
{
    uml::testing::Watchdog watchdog("concurrent connect", std::chrono::seconds(30));
    Guest guest;
    std::atomic_int result {-1};
    std::thread opener([&] {
        result = guest.endpoint().connect(port, std::chrono::seconds(10));
    });
    const auto request = guest.expect();
    guest.transmit(guestHeader(
        protocol::opResponse, port, request.header.sourcePort));
    opener.join();
    require(result.load() >= 0);
    ::close(result.load());
}

void noRequestIsEverDeferredWhateverTheGuestDoes()
{
    Guest guest;
    auto connection = establish(guest, port);
    /* The same stress as above, with every op, every queue and every
     * ordering; Guest::transmit and receive require "never defer". */
    const protocol::Header base =
        guestHeader(protocol::opRequest, port, connection.hostPort);
    for (std::uint16_t op = 0; op < 10; ++op) {
        auto header = base;
        header.op = op;
        if (op == protocol::opReset)
            continue;
        guest.transmit(header);
        guest.pump();
        while (guest.receive()) {
        }
    }
    require(guest.device().processQueue(protocol::receiveQueue, 5, {}, 4096).action !=
        VhostUserQueueAction::defer);
    require(guest.device().processQueue(protocol::transmitQueue, 6, {}, 0).action ==
        VhostUserQueueAction::complete);
    require(guest.device().processQueue(protocol::eventQueue, 7, {}, 4096).action !=
        VhostUserQueueAction::defer);
}

/* Several threads open, use and close connections at once, while one thread
 * plays a guest that echoes whatever it is sent. */
void manyThreadsOpenEchoAndCloseAtOnce()
{
    uml::testing::Watchdog watchdog("concurrent opens", std::chrono::seconds(120));
    Guest guest;
    std::atomic_bool stop {false};
    std::thread driver([&] {
        while (!stop.load()) {
            guest.pump();
            bool busy = false;
            while (auto packet = guest.receive()) {
                busy = true;
                const auto& header = packet->header;
                switch (header.op) {
                case protocol::opRequest:
                    guest.transmit(guestHeader(
                        protocol::opResponse, header.destinationPort,
                        header.sourcePort));
                    break;
                case protocol::opReadWrite:
                    guest.transmit(
                        guestHeader(
                            protocol::opReadWrite, header.destinationPort,
                            header.sourcePort,
                            static_cast<std::uint32_t>(packet->payload.size())),
                        packet->payload);
                    break;
                case protocol::opShutdown:
                    if ((header.flags & protocol::shutdownSend) != 0) {
                        guest.transmit(guestHeader(
                            protocol::opShutdown, header.destinationPort,
                            header.sourcePort, 0,
                            protocol::shutdownSend | protocol::shutdownReceive));
                    }
                    break;
                default:
                    break;
                }
            }
            if (!busy)
                std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    constexpr unsigned threads = 6;
    constexpr unsigned rounds = 12;
    std::atomic_uint failures {0};
    std::vector<std::thread> openers;
    for (unsigned thread = 0; thread < threads; ++thread) {
        openers.emplace_back([&, thread] {
            for (unsigned round = 0; round < rounds; ++round) {
                const int descriptor = guest.endpoint().connect(
                    port + thread, std::chrono::seconds(20));
                if (descriptor < 0) {
                    ++failures;
                    continue;
                }
                const auto bytes = pattern(1500 + round, static_cast<std::uint8_t>(thread));
                std::vector<std::uint8_t> echoed;
                bool good = ::send(descriptor, bytes.data(), bytes.size(), 0) ==
                    static_cast<ssize_t>(bytes.size());
                ::shutdown(descriptor, SHUT_WR);
                std::array<std::uint8_t, 4096> buffer {};
                while (good) {
                    pollfd waiting {.fd = descriptor, .events = POLLIN, .revents = 0};
                    if (::poll(&waiting, 1, 20000) <= 0) {
                        good = false;
                        break;
                    }
                    const auto count = ::recv(descriptor, buffer.data(), buffer.size(), 0);
                    if (count <= 0)
                        break;
                    echoed.insert(echoed.end(), buffer.begin(), buffer.begin() + count);
                }
                if (!good || echoed != bytes)
                    ++failures;
                ::close(descriptor);
            }
        });
    }
    for (auto& opener : openers)
        opener.join();
    stop = true;
    driver.join();
    require(failures.load() == 0, "an open, echo or close failed");
    for (unsigned index = 0; index < 8; ++index) {
        guest.pump();
        while (guest.receive()) {
        }
    }
    require(guest.device().connectionCount() == 0);
}

} // namespace

int main()
{
    const uml::testing::Watchdog watchdog("vsock device tests", std::chrono::minutes(5));
    static constexpr uml::testing::NamedTest tests[] = {
        {"describes-the-device", describesTheDevice},
        {"opens-and-carries-data-both-ways", opensAndCarriesDataBothWays},
        {"every-packet-carries-the-buffer-and-forward-count",
         everyPacketCarriesTheBufferAndForwardCount},
        {"refusal-of-the-open-is-connection-refused",
         refusalOfTheOpenIsConnectionRefused},
        {"an-open-times-out-and-a-late-response-is-reset",
         anOpenTimesOutAndALateResponseIsReset},
        {"an-open-abandoned-before-it-is-sent-is-dropped-quietly",
         anOpenAbandonedBeforeItIsSentIsDroppedQuietly},
        {"cancelling-an-open-ends-the-wait-and-the-connection",
         cancellingAnOpenEndsTheWaitAndTheConnection},
        {"an-open-without-a-receive-buffer-times-out",
         anOpenWithoutAReceiveBufferTimesOut},
        {"the-guest-stalls-on-its-credit-and-resumes",
         theGuestStallsOnItsCreditAndResumes},
        {"a-credit-update-goes-out-every-64-kibibytes",
         aCreditUpdateGoesOutEverySixtyFourKibibytes},
        {"the-host-stops-at-the-guests-credit", theHostStopsAtTheGuestsCredit},
        {"a-guest-exceeding-its-credit-is-reset", aGuestExceedingItsCreditIsReset},
        {"a-packet-larger-than-the-whole-buffer-is-reset",
         aPacketLargerThanTheWholeBufferIsReset},
        {"a-guest-half-close-ends-the-hosts-stream-after-its-data",
         aGuestHalfCloseEndsTheHostsStreamAfterItsData},
        {"a-host-half-close-is-told-to-the-guest-which-keeps-sending",
         aHostHalfCloseIsToldToTheGuestWhichKeepsSending},
        {"a-guest-close-is-answered-with-a-reset", aGuestCloseIsAnsweredWithAnReset},
        {"the-application-closing-ends-the-stream-and-another-write-resets",
         theApplicationClosingEndsTheStreamAndAnotherWriteResets},
        {"a-guest-reset-ends-the-applications-stream",
         aGuestResetEndsTheApplicationsStream},
        {"every-guest-request-is-reset", everyGuestRequestIsReset},
        {"a-malformed-packet-is-dropped", aMalformedPacketIsDropped},
        {"a-reset-closes-every-host-end", aResetClosesEveryHostEnd},
        {"an-open-queued-before-a-reset-fails-with-it",
         anOpenQueuedBeforeAResetFailsWithIt},
        {"the-device-ending-refuses-later-opens", theDeviceEndingRefusesLaterOpens},
        {"thirty-two-connections-are-the-limit", thirtyTwoConnectionsAreTheLimit},
        {"a-table-full-of-opens-refuses-the-rest", aTableFullOfOpensRefusesTheRest},
        {"the-event-queue-always-retries", theEventQueueAlwaysRetries},
        {"a-receive-buffer-too-small-for-a-header-waits",
         aReceiveBufferTooSmallForAHeaderWaits},
        {"data-fills-the-buffer-it-is-given", dataFillsTheBufferItIsGiven},
        {"connections-share-the-receive-buffers-fairly",
         connectionsShareTheReceiveBuffersFairly},
        {"attaches-an-accepted-stream", attachesAnAcceptedStream},
        {"a-refused-attach-closes-the-stream", aRefusedAttachClosesTheStream},
        {"a-blocked-connect-completes-from-another-thread",
         aBlockedConnectCompletesFromAnotherThread},
        {"many-threads-open-echo-and-close-at-once", manyThreadsOpenEchoAndCloseAtOnce},
        {"no-request-is-ever-deferred-whatever-the-guest-does",
         noRequestIsEverDeferredWhateverTheGuestDoes},
    };
    return uml::testing::runNamedTests(tests);
}
