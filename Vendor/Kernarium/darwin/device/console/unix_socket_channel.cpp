// SPDX-License-Identifier: GPL-2.0-or-later

#include "unix_socket_channel.hpp"

#include "unique_descriptor.hpp"

#include "platform/descriptor.hpp"
#include "socket_paths.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <string>
#include <utility>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace uml::console {
namespace {

constexpr std::size_t maximumInputBytes = 64U * 1024U;
constexpr std::size_t maximumOutputTailBytes = 64U * 1024U;

/*
 * A client that connects and closes again before this process accepts it is
 * ordinary, and Darwin rejects F_SETNOSIGPIPE on such a socket with EINVAL
 * because it is already fully shut down.  Report that per-client condition
 * instead of throwing: an exception here escapes drainEventSource() into the
 * vhost-user connection loop, which tears the console device down for the rest
 * of the run and leaves the guest spinning on an end-of-file call descriptor.
 */
bool configureClient(int descriptor) noexcept
{
    return uml::platform::configureDescriptor(
        descriptor,
        {.closeOnExec = true, .nonblocking = true, .noSigpipe = true});
}

} // namespace

class UnixSocketChannel::Implementation final {
public:
    explicit Implementation(std::filesystem::path socketPath)
        : socketPath_(std::move(socketPath))
        , cleanup_(socketPath_)
    {
        if (!socketPath_.is_absolute() ||
            socketPath_.lexically_normal() != socketPath_ ||
            socketPath_.filename().empty()) {
            throw std::invalid_argument(
                "Virtio console socket path must be absolute and normalized");
        }
        if (socketPath_.string().size() >= sizeof(sockaddr_un::sun_path)) {
            throw std::invalid_argument(
                "Virtio console socket path is too long");
        }
        struct stat existing {};
        if (::lstat(socketPath_.c_str(), &existing) == 0) {
            throw std::runtime_error(
                "Virtio console socket path already exists; refusing to replace it");
        }
        if (errno != ENOENT) {
            throw std::runtime_error(
                "Virtio console socket path inspection failed: " +
                std::string(std::strerror(errno)));
        }

        listener_.reset(::socket(AF_UNIX, SOCK_STREAM, 0));
        if (listener_.get() < 0) {
            throw std::runtime_error(
                "Virtio console socket creation failed: " +
                std::string(std::strerror(errno)));
        }
        uml::platform::configureDescriptorOrThrow(
            listener_.get(), {.closeOnExec = true}, "console descriptor");
        uml::platform::configureDescriptorOrThrow(
            listener_.get(),
            {.nonblocking = true},
            "nonblocking console descriptor");

        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        std::memcpy(
            address.sun_path,
            socketPath_.c_str(),
            socketPath_.string().size() + 1U);
        /*
         * The harness accepts the socket only when it first observes mode
         * 0600, so the file must never exist with a wider mode.  Create it
         * under a private umask instead of narrowing it after the fact, and
         * narrow it again before listen() makes it connectable.
         */
        const mode_t previousMask = ::umask(0177);
        const int bound = ::bind(
            listener_.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address));
        const int bindError = errno;
        ::umask(previousMask);
        if (bound != 0) {
            throw std::runtime_error(
                "Virtio console bind failed: " +
                std::string(std::strerror(bindError)));
        }
        cleanup_.adopt();
        if (::chmod(socketPath_.c_str(), 0600) != 0) {
            throw std::runtime_error(
                "Virtio console socket permission update failed: " +
                std::string(std::strerror(errno)));
        }
        if (::listen(listener_.get(), 1) != 0) {
            throw std::runtime_error(
                "Virtio console listen failed: " +
                std::string(std::strerror(errno)));
        }
        std::fprintf(
            stdout,
            "Virtio console socket: %s\n",
            socketPath_.c_str());
        if (std::fflush(stdout) != 0) {
            throw std::runtime_error(
                "Virtio console readiness flush failed: " +
                std::string(std::strerror(errno)));
        }
    }

    std::vector<std::uint8_t> read(std::size_t maximumBytes)
    {
        const auto count = std::min(maximumBytes, input_.size());
        std::vector<std::uint8_t> result;
        result.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            result.push_back(input_.front());
            input_.pop_front();
        }
        return result;
    }

    void write(std::span<const std::uint8_t> bytes) noexcept
    {
        if (bytes.empty())
            return;
        if (client_.get() < 0) {
            rememberOutputTail(bytes);
            return;
        }
        const auto sent = sendToClient(bytes);
        if (sent == bytes.size())
            return;
        reportOutputDrop();
    }

    int eventDescriptor() const noexcept
    {
        return client_.get() >= 0 ? client_.get() : listener_.get();
    }

    void drainEventSource()
    {
        if (client_.get() < 0)
            acceptClient();
        else
            drainClient();
    }

    void reset() noexcept
    {
        input_.clear();
        outputTail_.clear();
        reportedInputOverflow_ = false;
        reportedOutputDrop_ = false;
    }

private:
    void acceptClient()
    {
        uml::gpu::UniqueDescriptor accepted {
            ::accept(listener_.get(), nullptr, nullptr)};
        if (accepted.get() < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            throw std::runtime_error(
                "Virtio console accept failed: " +
                std::string(std::strerror(errno)));
        }
        if (!configureClient(accepted.get())) {
            std::fprintf(
                stderr,
                "Virtio console client rejected: %s\n",
                std::strerror(errno));
            return;
        }
        client_ = std::move(accepted);
        reportedInputOverflow_ = false;
        reportedOutputDrop_ = false;
        std::fputs("Virtio console client attached\n", stderr);
        flushOutputTail();
    }

    void drainClient()
    {
        std::array<std::uint8_t, 4096> bytes {};
        for (;;) {
            const auto count = ::recv(
                client_.get(), bytes.data(), bytes.size(), MSG_DONTWAIT);
            if (count > 0) {
                const auto available = maximumInputBytes - input_.size();
                const auto accepted = std::min<std::size_t>(
                    static_cast<std::size_t>(count), available);
                input_.insert(
                    input_.end(), bytes.begin(), bytes.begin() + accepted);
                if (accepted != static_cast<std::size_t>(count) &&
                    !reportedInputOverflow_) {
                    std::fputs(
                        "Virtio console input queue full; dropping bytes\n",
                        stderr);
                    reportedInputOverflow_ = true;
                }
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return;
            if (count == 0 || (count < 0 &&
                (errno == ECONNRESET || errno == ENOTCONN))) {
                detachClient();
                return;
            }
            throw std::runtime_error(
                "Virtio console receive failed: " +
                std::string(std::strerror(errno)));
        }
    }

    std::size_t sendToClient(std::span<const std::uint8_t> bytes) noexcept
    {
        for (;;) {
            const auto count = ::send(
                client_.get(), bytes.data(), bytes.size(), MSG_DONTWAIT);
            if (count >= 0)
                return static_cast<std::size_t>(count);
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;
            if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN) {
                detachClient();
                return 0;
            }
            std::fprintf(
                stderr,
                "Virtio console output failed: %s\n",
                std::strerror(errno));
            detachClient();
            return 0;
        }
    }

    void rememberOutputTail(std::span<const std::uint8_t> bytes) noexcept
    {
        if (bytes.size() >= maximumOutputTailBytes) {
            outputTail_.clear();
            const auto tail = bytes.last(maximumOutputTailBytes);
            outputTail_.insert(outputTail_.end(), tail.begin(), tail.end());
            return;
        }
        while (outputTail_.size() + bytes.size() > maximumOutputTailBytes)
            outputTail_.pop_front();
        outputTail_.insert(outputTail_.end(), bytes.begin(), bytes.end());
    }

    void flushOutputTail() noexcept
    {
        if (outputTail_.empty())
            return;
        std::vector<std::uint8_t> tail(
            outputTail_.begin(), outputTail_.end());
        outputTail_.clear();
        if (sendToClient(tail) != tail.size())
            reportOutputDrop();
    }

    void reportOutputDrop() noexcept
    {
        if (reportedOutputDrop_)
            return;
        std::fputs(
            "Virtio console client is not draining output; dropping bytes\n",
            stderr);
        reportedOutputDrop_ = true;
    }

    void detachClient() noexcept
    {
        if (client_.get() < 0)
            return;
        client_.reset();
        reportedInputOverflow_ = false;
        reportedOutputDrop_ = false;
        std::fputs("Virtio console client detached\n", stderr);
    }

    std::filesystem::path socketPath_;
    ::uml::device::SocketPath cleanup_;
    uml::gpu::UniqueDescriptor listener_;
    uml::gpu::UniqueDescriptor client_;
    std::deque<std::uint8_t> input_;
    std::deque<std::uint8_t> outputTail_;
    bool reportedInputOverflow_ {};
    bool reportedOutputDrop_ {};
};

UnixSocketChannel::UnixSocketChannel(std::filesystem::path socketPath)
    : implementation_(
          std::make_unique<Implementation>(std::move(socketPath)))
{
}

UnixSocketChannel::~UnixSocketChannel() = default;

std::vector<std::uint8_t> UnixSocketChannel::read(std::size_t maximumBytes)
{
    return implementation_->read(maximumBytes);
}

void UnixSocketChannel::write(std::span<const std::uint8_t> bytes) noexcept
{
    implementation_->write(bytes);
}

int UnixSocketChannel::eventDescriptor() const noexcept
{
    return implementation_->eventDescriptor();
}

void UnixSocketChannel::drainEventSource()
{
    implementation_->drainEventSource();
}

void UnixSocketChannel::reset() noexcept
{
    implementation_->reset();
}

} // namespace uml::console
