// SPDX-License-Identifier: GPL-2.0-or-later

#include "connected_socket_channel.hpp"

#include "platform/descriptor.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include <sys/socket.h>
#include <unistd.h>

namespace uml::console {
namespace {

/* The device asks for as much as the guest's input buffers hold, which the
 * guest chooses: one descriptor may cover all of its RAM, and a chain may
 * repeat it. A read never allocates more than this, and recv() hands back no
 * more than is queued anyway. */
constexpr std::size_t maximumReadBytes = 64U * 1024U;

} // namespace

ConnectedSocketChannel::ConnectedSocketChannel(int descriptor)
    : descriptor_(descriptor)
{
    if (descriptor_ < 0)
        throw std::invalid_argument("connected console descriptor is invalid");
    if (!uml::platform::configureDescriptor(
            descriptor_,
            {.closeOnExec = true, .nonblocking = true, .noSigpipe = true})) {
        const int error = errno;
        ::close(descriptor_);
        descriptor_ = -1;
        throw std::runtime_error(
            "configuring connected console descriptor failed: " +
            std::string(std::strerror(error)));
    }
}

ConnectedSocketChannel::~ConnectedSocketChannel()
{
    if (descriptor_ >= 0)
        ::close(descriptor_);
}

std::vector<std::uint8_t> ConnectedSocketChannel::read(
    std::size_t maximumBytes)
{
    std::vector<std::uint8_t> result(std::min(maximumBytes, maximumReadBytes));
    for (;;) {
        const ssize_t received = ::recv(
            descriptor_, result.data(), result.size(), MSG_DONTWAIT);
        if (received > 0) {
            result.resize(static_cast<std::size_t>(received));
            return result;
        }
        if (received < 0 && errno == EINTR)
            continue;
        result.clear();
        return result;
    }
}

void ConnectedSocketChannel::write(
    std::span<const std::uint8_t> bytes) noexcept
{
    sendWaitingOutput();
    if (waitingSent_ < waiting_.size()) {
        // The device offers no output while some waits; anything written
        // regardless queues behind it rather than overtaking it.
        keepUnsent(bytes);
        return;
    }
    while (!bytes.empty()) {
        const ssize_t sent = ::send(
            descriptor_, bytes.data(), bytes.size(), 0);
        if (sent > 0) {
            bytes = bytes.subspan(static_cast<std::size_t>(sent));
            continue;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            keepUnsent(bytes);
            return;
        }
        reportOutputDrop(sent < 0 ? std::strerror(errno) : "no progress");
        return;
    }
}

bool ConnectedSocketChannel::outputWaiting() noexcept
{
    sendWaitingOutput();
    return waitingSent_ < waiting_.size();
}

void ConnectedSocketChannel::sendWaitingOutput() noexcept
{
    while (waitingSent_ < waiting_.size()) {
        const ssize_t sent = ::send(
            descriptor_,
            waiting_.data() + waitingSent_,
            waiting_.size() - waitingSent_,
            0);
        if (sent > 0) {
            waitingSent_ += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        reportOutputDrop(sent < 0 ? std::strerror(errno) : "no progress");
        break;
    }
    waiting_.clear();
    waitingSent_ = 0;
}

void ConnectedSocketChannel::keepUnsent(
    std::span<const std::uint8_t> bytes) noexcept
{
    try {
        waiting_.insert(waiting_.end(), bytes.begin(), bytes.end());
    } catch (...) {
        reportOutputDrop("no memory to keep it");
    }
}

void ConnectedSocketChannel::reportOutputDrop(const char* reason) noexcept
{
    if (reportedOutputDrop_)
        return;
    reportedOutputDrop_ = true;
    std::fprintf(
        stderr, "Virtio console relay output dropped: %s\n", reason);
}

int ConnectedSocketChannel::eventDescriptor() const noexcept
{
    return descriptor_;
}

void ConnectedSocketChannel::drainEventSource()
{
    sendWaitingOutput();
}

void ConnectedSocketChannel::reset() noexcept
{
}

} // namespace uml::console
