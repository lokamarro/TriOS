// SPDX-License-Identifier: GPL-2.0-or-later

#include "connection_table.hpp"

#include <utility>

namespace uml::vsock {

std::uint32_t ConnectionTable::allocatePort()
{
    constexpr std::uint32_t range = lastHostPort - firstHostPort + 1U;
    for (std::uint32_t tried = 0; tried < range; ++tried) {
        const std::uint32_t candidate = nextPort_;
        nextPort_ = candidate == lastHostPort ? firstHostPort : candidate + 1U;
        bool used = false;
        for (const auto& entry : connections_) {
            if (entry.second.hostPort == candidate) {
                used = true;
                break;
            }
        }
        if (!used)
            return candidate;
    }
    return 0;
}

Connection* ConnectionTable::create(
    std::uint32_t guestPort,
    uml::gpu::UniqueDescriptor stream,
    std::shared_ptr<Opening> opening)
{
    if (full())
        return nullptr;
    const std::uint32_t hostPort = allocatePort();
    if (hostPort == 0)
        return nullptr;
    const ConnectionId id = nextId_++;
    auto [position, inserted] = connections_.try_emplace(id);
    Connection& connection = position->second;
    connection.id = id;
    connection.hostPort = hostPort;
    connection.guestPort = guestPort;
    connection.stream = std::move(stream);
    connection.opening = std::move(opening);
    return &connection;
}

Connection* ConnectionTable::find(ConnectionId id) noexcept
{
    const auto found = connections_.find(id);
    return found == connections_.end() ? nullptr : &found->second;
}

Connection* ConnectionTable::find(
    std::uint32_t hostPort,
    std::uint32_t guestPort) noexcept
{
    for (auto& entry : connections_) {
        if (entry.second.hostPort == hostPort &&
            entry.second.guestPort == guestPort)
            return &entry.second;
    }
    return nullptr;
}

Connection* ConnectionTable::findOpening(const Opening* opening) noexcept
{
    for (auto& entry : connections_) {
        if (entry.second.opening.get() == opening)
            return &entry.second;
    }
    return nullptr;
}

void ConnectionTable::erase(ConnectionId id) noexcept
{
    connections_.erase(id);
}

} // namespace uml::vsock
