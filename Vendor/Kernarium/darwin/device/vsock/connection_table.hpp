// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "endpoint.hpp"
#include "protocol.hpp"
#include "unique_descriptor.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <vector>

namespace uml::vsock {

using ConnectionId = std::uint64_t;

/* The flow-control counters of one connection (Virtio 1.2, 5.10.6.3). They
 * wrap at 32 bits, as they do on the wire, and every difference below is
 * taken in 32-bit arithmetic. */
struct Credit final {
    /* The buffer the guest granted and the forward count it last reported. */
    std::uint32_t peerBufferAllocation {};
    std::uint32_t peerForwardCount {};
    /* Bytes of stream the host has sent the guest. */
    std::uint32_t sent {};
    /* Bytes of stream the guest has sent the host, and how many of them the
     * host's socket has accepted (the fwd_cnt the device reports). */
    std::uint32_t received {};
    std::uint32_t forwarded {};
    /* The forward count in the last packet sent to the guest. */
    std::uint32_t advertised {};

    /* How many bytes the host may send now. */
    [[nodiscard]] std::uint32_t peerFree() const noexcept
    {
        const std::uint32_t inFlight = sent - peerForwardCount;
        return inFlight >= peerBufferAllocation ?
            0U : peerBufferAllocation - inFlight;
    }

    /* The bytes the device holds for the host's socket. */
    [[nodiscard]] std::uint32_t buffered() const noexcept
    {
        return received - forwarded;
    }

    /* Whether the guest may send `length` more bytes. */
    [[nodiscard]] bool accepts(std::uint32_t length) const noexcept
    {
        const std::uint32_t held = buffered();
        return held <= protocol::bufferAllocation &&
            length <= protocol::bufferAllocation - held;
    }

    /* Whether enough has reached the host's socket since the guest was last
     * told, for it to want a CREDIT_UPDATE. */
    [[nodiscard]] bool updateDue() const noexcept
    {
        return forwarded - advertised >= protocol::creditUpdateThreshold;
    }
};

struct Connection final {
    enum class Phase : std::uint8_t {
        /* The REQUEST is queued or sent and the guest has not answered. */
        requesting,
        established,
    };

    ConnectionId id {};
    std::uint32_t hostPort {};
    std::uint32_t guestPort {};
    Phase phase {Phase::requesting};
    /* The host's end of the stream. Invalid once the connection only waits
     * for its RST to be sent. */
    uml::gpu::UniqueDescriptor stream;
    /* Who waits for the guest's answer, until it comes. */
    std::shared_ptr<Opening> opening;
    Credit credit;

    /* Packets the guest is owed, in the order they are sent. */
    bool wantRequest {};
    std::uint32_t wantShutdown {};
    bool wantCreditUpdate {};
    bool wantReset {};
    bool requestSent {};
    bool queuedForControl {};

    /* Guest to host: what the host's socket has not accepted yet, and what
     * the guest has said it will no longer do. */
    std::deque<std::vector<std::uint8_t>> pending;
    std::size_t pendingOffset {};
    bool guestShutSend {};
    bool guestShutReceive {};
    bool hostWriteShut {};

    /* Host to guest: the host's end reported readable and was not read dry,
     * or reached end of stream. */
    bool readable {};
    bool hostEof {};

    /* Which of the stream's filters the event queue has enabled. */
    bool readEnabled {};
    bool writeEnabled {};

    [[nodiscard]] bool wantsControl() const noexcept
    {
        return wantRequest || wantShutdown != 0 || wantCreditUpdate ||
            wantReset;
    }

    /* Whether a data packet can be read from the host's end now. */
    [[nodiscard]] bool canSend() const noexcept
    {
        return phase == Phase::established && stream.get() >= 0 &&
            !wantReset && !hostEof && !guestShutReceive && readable &&
            credit.peerFree() != 0;
    }

    [[nodiscard]] std::size_t pendingBytes() const noexcept
    {
        std::size_t total = 0;
        for (const auto& chunk : pending)
            total += chunk.size();
        return total - pendingOffset;
    }
};

/* The live connections, keyed by id and by the pair of ports, with the host
 * port allocator. It does no I/O. */
class ConnectionTable final {
public:
    static constexpr std::uint32_t firstHostPort = 49152;
    static constexpr std::uint32_t lastHostPort = 65535;

    explicit ConnectionTable(
        std::size_t limit = Gateway::maximumConnections) noexcept
        : limit_(limit)
    {
    }

    [[nodiscard]] std::size_t size() const noexcept { return connections_.size(); }
    [[nodiscard]] bool full() const noexcept { return size() >= limit_; }

    /* The next host port that no live connection uses, cycling through the
     * ephemeral range so that a port is not reused soon after it is freed;
     * 0 when none is free, which a limit below the range cannot reach. */
    [[nodiscard]] std::uint32_t allocatePort();

    /* A new connection to `guestPort`, or null when the table is full. */
    [[nodiscard]] Connection* create(
        std::uint32_t guestPort,
        uml::gpu::UniqueDescriptor stream,
        std::shared_ptr<Opening> opening);

    [[nodiscard]] Connection* find(ConnectionId id) noexcept;
    [[nodiscard]] Connection* find(
        std::uint32_t hostPort,
        std::uint32_t guestPort) noexcept;
    [[nodiscard]] Connection* findOpening(const Opening* opening) noexcept;

    void erase(ConnectionId id) noexcept;
    void clear() noexcept { connections_.clear(); }

    /* Calls `visit` on each connection after `after` and then on the ones up
     * to it, in id order, until it returns true. `visit` must not erase. */
    template<typename Visitor>
    void forEachFrom(ConnectionId after, Visitor&& visit)
    {
        for (auto it = connections_.upper_bound(after);
             it != connections_.end();
             ++it) {
            if (visit(it->second))
                return;
        }
        for (auto it = connections_.begin();
             it != connections_.end() && it->first <= after;
             ++it) {
            if (visit(it->second))
                return;
        }
    }

    template<typename Visitor>
    void forEach(Visitor&& visit)
    {
        for (auto& entry : connections_)
            visit(entry.second);
    }

    template<typename Visitor>
    void forEach(Visitor&& visit) const
    {
        for (const auto& entry : connections_)
            visit(entry.second);
    }

private:
    std::size_t limit_;
    ConnectionId nextId_ {1};
    std::uint32_t nextPort_ {firstHostPort};
    std::map<ConnectionId, Connection> connections_;
};

} // namespace uml::vsock
