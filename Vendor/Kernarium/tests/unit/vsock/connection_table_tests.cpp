// SPDX-License-Identifier: GPL-2.0-or-later

#include "connection_table.hpp"
#include "named_tests.hpp"
#include "protocol.hpp"
#include "require.hpp"

#include <cstring>
#include <set>

namespace {

using namespace uml::vsock;
using uml::testing::require;

void theProtocolRecordsHaveTheirWireLayout()
{
    /* The static_asserts in protocol.hpp hold the offsets; this holds the
     * bytes the guest's driver reads. */
    const protocol::Header header {
        .sourceCid = 2,
        .destinationCid = 3,
        .sourcePort = 0x11223344,
        .destinationPort = 0x55667788,
        .length = 9,
        .type = 1,
        .op = 5,
        .flags = 2,
        .bufferAllocation = 0x40000,
        .forwardCount = 7,
    };
    std::uint8_t bytes[sizeof(header)];
    std::memcpy(bytes, &header, sizeof(header));
    require(bytes[0] == 2 && bytes[1] == 0);
    require(bytes[8] == 3 && bytes[9] == 0);
    require(bytes[16] == 0x44 && bytes[19] == 0x11);
    require(bytes[20] == 0x88 && bytes[23] == 0x55);
    require(bytes[24] == 9);
    require(bytes[28] == 1 && bytes[29] == 0);
    require(bytes[30] == 5 && bytes[31] == 0);
    require(bytes[32] == 2);
    require(bytes[36] == 0 && bytes[37] == 0 && bytes[38] == 4);
    require(bytes[40] == 7);
    static_assert(protocol::deviceId == 19);
    static_assert(protocol::hostCid == 2 && protocol::guestCid == 3);
}

void creditLeavesTheGuestAsMuchAsItGranted()
{
    Credit credit;
    credit.peerBufferAllocation = 1000;
    require(credit.peerFree() == 1000);
    credit.sent = 400;
    require(credit.peerFree() == 600);
    credit.peerForwardCount = 150;
    require(credit.peerFree() == 750);
    credit.sent = 1150;
    require(credit.peerFree() == 0);
    /* More in flight than granted, as when the guest shrinks its buffer. */
    credit.sent = 5000;
    require(credit.peerFree() == 0);
}

void creditCountersWrapAtThirtyTwoBits()
{
    Credit credit;
    credit.peerBufferAllocation = 65536;
    credit.sent = 0xFFFFFF00U;
    credit.peerForwardCount = 0xFFFFFE00U;
    require(credit.peerFree() == 65536 - 0x100);
    credit.sent += 0x200U;
    require(credit.sent == 0x100U);
    require(credit.peerFree() == 65536 - 0x300);
    credit.received = 0x10U;
    credit.forwarded = 0xFFFFFFF0U;
    require(credit.buffered() == 0x20U);
    credit.advertised = 0xFFFF0000U;
    require(credit.forwarded - credit.advertised == 0xFFF0U);
    require(!credit.updateDue());
    credit.forwarded += 0x20U;
    require(credit.updateDue());
}

void creditAcceptsWhatFitsTheBuffer()
{
    Credit credit;
    require(credit.accepts(protocol::bufferAllocation));
    require(!credit.accepts(protocol::bufferAllocation + 1));
    credit.received = 200000;
    credit.forwarded = 100000;
    require(credit.buffered() == 100000);
    require(credit.accepts(protocol::bufferAllocation - 100000));
    require(!credit.accepts(protocol::bufferAllocation - 100000 + 1));
    /* More held than the buffer: nothing fits, whatever it is. */
    credit.received = 600000;
    credit.forwarded = 0;
    require(!credit.accepts(0 + 1));
}

void aCreditUpdateIsDueAtTheThreshold()
{
    Credit credit;
    require(!credit.updateDue());
    credit.forwarded = protocol::creditUpdateThreshold - 1;
    require(!credit.updateDue());
    credit.forwarded = protocol::creditUpdateThreshold;
    require(credit.updateDue());
    credit.advertised = credit.forwarded;
    require(!credit.updateDue());
}

void hostPortsCycleAndStayUnique()
{
    ConnectionTable table;
    std::set<std::uint32_t> seen;
    for (unsigned index = 0; index < 32; ++index) {
        auto* connection = table.create(5000 + index, {}, nullptr);
        require(connection != nullptr);
        require(connection->hostPort >= ConnectionTable::firstHostPort);
        require(connection->hostPort <= ConnectionTable::lastHostPort);
        require(seen.insert(connection->hostPort).second);
    }
    require(table.full());
    require(table.create(1, {}, nullptr) == nullptr);
    /* A freed port is not the next one handed out. */
    auto* victim = table.find(ConnectionTable::firstHostPort + 3, 5003);
    require(victim != nullptr);
    const auto freed = victim->hostPort;
    table.erase(victim->id);
    auto* next = table.create(77, {}, nullptr);
    require(next != nullptr);
    require(next->hostPort != freed);
    require(next->hostPort == ConnectionTable::firstHostPort + 32);
}

void thePortAllocatorWrapsTheRange()
{
    ConnectionTable table(4);
    std::uint32_t last = 0;
    for (unsigned round = 0; round < 20000; ++round) {
        auto* connection = table.create(1, {}, nullptr);
        require(connection != nullptr);
        last = connection->hostPort;
        require(last >= ConnectionTable::firstHostPort && last <= ConnectionTable::lastHostPort);
        table.erase(connection->id);
    }
    /* 20000 allocations wrapped a range of 16384 ports. */
    require(last == ConnectionTable::firstHostPort + (20000 - 16384) - 1);
}

void connectionsAreFoundByIdPortsAndOpening()
{
    ConnectionTable table;
    auto opening = std::make_shared<Opening>(
        std::weak_ptr<Gateway> {}, uml::gpu::UniqueDescriptor {});
    auto* connection = table.create(4242, {}, opening);
    require(connection != nullptr);
    const auto id = connection->id;
    require(table.find(id) == connection);
    require(table.find(connection->hostPort, 4242) == connection);
    require(table.find(connection->hostPort, 4243) == nullptr);
    require(table.find(connection->hostPort + 1, 4242) == nullptr);
    require(table.findOpening(opening.get()) == connection);
    table.erase(id);
    require(table.find(id) == nullptr);
    require(table.size() == 0);
}

void iterationStartsAfterTheCursorAndWraps()
{
    ConnectionTable table;
    std::vector<ConnectionId> ids;
    for (unsigned index = 0; index < 5; ++index)
        ids.push_back(table.create(index, {}, nullptr)->id);
    std::vector<ConnectionId> visited;
    table.forEachFrom(ids[2], [&](Connection& connection) {
        visited.push_back(connection.id);
        return false;
    });
    require((visited == std::vector<ConnectionId> {ids[3], ids[4], ids[0], ids[1], ids[2]}));
    visited.clear();
    table.forEachFrom(0, [&](Connection& connection) {
        visited.push_back(connection.id);
        return connection.id == ids[1];
    });
    require((visited == std::vector<ConnectionId> {ids[0], ids[1]}));
}

void aConnectionWantsControlOnlyForWhatItOwes()
{
    Connection connection;
    require(!connection.wantsControl());
    connection.wantShutdown = protocol::shutdownSend;
    require(connection.wantsControl());
    connection.wantShutdown = 0;
    connection.wantCreditUpdate = true;
    require(connection.wantsControl());
}

} // namespace

int main()
{
    static constexpr uml::testing::NamedTest tests[] = {
        {"the-protocol-records-have-their-wire-layout",
         theProtocolRecordsHaveTheirWireLayout},
        {"credit-leaves-the-guest-as-much-as-it-granted",
         creditLeavesTheGuestAsMuchAsItGranted},
        {"credit-counters-wrap-at-thirty-two-bits", creditCountersWrapAtThirtyTwoBits},
        {"credit-accepts-what-fits-the-buffer", creditAcceptsWhatFitsTheBuffer},
        {"a-credit-update-is-due-at-the-threshold", aCreditUpdateIsDueAtTheThreshold},
        {"host-ports-cycle-and-stay-unique", hostPortsCycleAndStayUnique},
        {"the-port-allocator-wraps-the-range", thePortAllocatorWrapsTheRange},
        {"connections-are-found-by-id-ports-and-opening",
         connectionsAreFoundByIdPortsAndOpening},
        {"iteration-starts-after-the-cursor-and-wraps",
         iterationStartsAfterTheCursorAndWraps},
        {"a-connection-wants-control-only-for-what-it-owes",
         aConnectionWantsControlOnlyForWhatItOwes},
    };
    return uml::testing::runNamedTests(tests);
}
