// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>

namespace uml::vsock::protocol {

/* The wire records are little-endian, and so is every Apple host. */
static_assert(std::endian::native == std::endian::little);

inline constexpr std::uint32_t deviceId = 19;

/* The guest's driver addresses the host as CID 2 and itself as the CID the
 * device configuration states. */
inline constexpr std::uint64_t hostCid = 2;
inline constexpr std::uint64_t guestCid = 3;

inline constexpr std::size_t queueCount = 3;
inline constexpr std::uint32_t receiveQueue = 0;
inline constexpr std::uint32_t transmitQueue = 1;
inline constexpr std::uint32_t eventQueue = 2;

/* No feature bit is offered: STREAM sockets are implied, SEQPACKET is not
 * offered, and the driver sees a plain stream transport. */
inline constexpr std::uint64_t features = 0;

inline constexpr std::uint16_t typeStream = 1;

inline constexpr std::uint16_t opInvalid = 0;
inline constexpr std::uint16_t opRequest = 1;
inline constexpr std::uint16_t opResponse = 2;
inline constexpr std::uint16_t opReset = 3;
inline constexpr std::uint16_t opShutdown = 4;
inline constexpr std::uint16_t opReadWrite = 5;
inline constexpr std::uint16_t opCreditUpdate = 6;
inline constexpr std::uint16_t opCreditRequest = 7;

/* SHUTDOWN flags: the sender will not receive, or will not send, any more. */
inline constexpr std::uint32_t shutdownReceive = 1;
inline constexpr std::uint32_t shutdownSend = 2;

/* The header that precedes every packet, in both directions. */
struct [[gnu::packed]] Header final {
    std::uint64_t sourceCid;
    std::uint64_t destinationCid;
    std::uint32_t sourcePort;
    std::uint32_t destinationPort;
    std::uint32_t length;
    std::uint16_t type;
    std::uint16_t op;
    std::uint32_t flags;
    std::uint32_t bufferAllocation;
    std::uint32_t forwardCount;
};

static_assert(sizeof(Header) == 44);
static_assert(offsetof(Header, sourceCid) == 0);
static_assert(offsetof(Header, destinationCid) == 8);
static_assert(offsetof(Header, sourcePort) == 16);
static_assert(offsetof(Header, destinationPort) == 20);
static_assert(offsetof(Header, length) == 24);
static_assert(offsetof(Header, type) == 28);
static_assert(offsetof(Header, op) == 30);
static_assert(offsetof(Header, flags) == 32);
static_assert(offsetof(Header, bufferAllocation) == 36);
static_assert(offsetof(Header, forwardCount) == 40);

/* The device configuration space. */
struct [[gnu::packed]] Configuration final {
    std::uint64_t guestCid;
};

static_assert(sizeof(Configuration) == 8);

/* The flow control a connection starts with, in bytes. The device tells the
 * guest it may have this much of the host-bound stream in flight, and sends
 * a CREDIT_UPDATE each time this much more has reached the host's socket. */
inline constexpr std::uint32_t bufferAllocation = 256U * 1024U;
inline constexpr std::uint32_t creditUpdateThreshold = 64U * 1024U;

} // namespace uml::vsock::protocol
