// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <utility>
#include <unistd.h>

namespace uml::platform {

/* Owns one open file descriptor, closing it on reset and destruction. */
class UniqueDescriptor final {
public:
    UniqueDescriptor() = default;
    explicit UniqueDescriptor(int descriptor) noexcept
        : descriptor_(descriptor)
    {
    }
    ~UniqueDescriptor() { reset(); }

    UniqueDescriptor(const UniqueDescriptor&) = delete;
    UniqueDescriptor& operator=(const UniqueDescriptor&) = delete;

    UniqueDescriptor(UniqueDescriptor&& other) noexcept
        : descriptor_(other.release())
    {
    }

    UniqueDescriptor& operator=(UniqueDescriptor&& other) noexcept
    {
        if (this != &other) {
            reset();
            descriptor_ = other.release();
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return descriptor_; }

    [[nodiscard]] int release() noexcept
    {
        return std::exchange(descriptor_, -1);
    }

    void reset(int descriptor = -1) noexcept
    {
        if (descriptor_ >= 0)
            (void)::close(descriptor_);
        descriptor_ = descriptor;
    }

private:
    int descriptor_ {-1};
};

/* The flags configureDescriptor() adds to a descriptor. */
struct DescriptorFlags final {
    bool closeOnExec {};
    bool nonblocking {};
    bool noSigpipe {};
};

/* Adds `flags` to `descriptor`. Both flag words it changes are read before
 * either is written, and F_SETNOSIGPIPE comes last. Returns false with errno
 * set if a call fails, leaving any flag already written in place. */
[[nodiscard]] inline bool configureDescriptor(
    int descriptor,
    DescriptorFlags flags)
{
    const int descriptorFlags =
        flags.closeOnExec ? ::fcntl(descriptor, F_GETFD) : 0;
    const int statusFlags =
        flags.nonblocking ? ::fcntl(descriptor, F_GETFL) : 0;
    return descriptorFlags >= 0 && statusFlags >= 0 &&
        (!flags.closeOnExec ||
         ::fcntl(descriptor, F_SETFD, descriptorFlags | FD_CLOEXEC) == 0) &&
        (!flags.nonblocking ||
         ::fcntl(descriptor, F_SETFL, statusFlags | O_NONBLOCK) == 0) &&
        (!flags.noSigpipe || ::fcntl(descriptor, F_SETNOSIGPIPE, 1) == 0);
}

/* configureDescriptor(), throwing "configuring <what> failed: <error>". */
inline void configureDescriptorOrThrow(
    int descriptor,
    DescriptorFlags flags,
    const char* what)
{
    if (configureDescriptor(descriptor, flags))
        return;
    const int error = errno;
    throw std::runtime_error(
        std::string("configuring ") + what + " failed: " +
        std::strerror(error));
}

/* A datagram socketpair that wakes a thread polling descriptor(). notify()
 * queues one byte without blocking and drain() reads every queued byte. Both
 * ends are close-on-exec and non-blocking, and the write end raises no
 * SIGPIPE. */
class WakeSocket final {
public:
    enum class Drained {
        empty,
        closed,
        failed,
    };

    /* Throws "creating <what> socketpair failed: <error>" or
     * "configuring <what> descriptor failed: <error>". */
    explicit WakeSocket(const char* what)
    {
        std::array<int, 2> descriptors {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_DGRAM, 0, descriptors.data()) != 0) {
            const int error = errno;
            throw std::runtime_error(
                std::string("creating ") + what + " socketpair failed: " +
                std::strerror(error));
        }
        read_.reset(descriptors[0]);
        write_.reset(descriptors[1]);
        const auto name = std::string(what) + " descriptor";
        configureDescriptorOrThrow(
            read_.get(),
            {.closeOnExec = true, .nonblocking = true},
            name.c_str());
        configureDescriptorOrThrow(
            write_.get(),
            {.closeOnExec = true, .nonblocking = true, .noSigpipe = true},
            name.c_str());
    }

    WakeSocket(const WakeSocket&) = delete;
    WakeSocket& operator=(const WakeSocket&) = delete;

    [[nodiscard]] int descriptor() const noexcept { return read_.get(); }

    /* Returns false with errno set unless the byte was queued or the queue
     * was already full, which leaves the reader woken anyway. */
    [[nodiscard]] bool notify() const noexcept
    {
        constexpr std::uint8_t ready = 1;
        for (;;) {
            if (::send(write_.get(), &ready, sizeof(ready), MSG_DONTWAIT) >= 0)
                return true;
            if (errno != EINTR)
                return errno == EAGAIN || errno == EWOULDBLOCK;
        }
    }

    /* Reports closed at end of file and failed, with errno set, on an error
     * other than an empty queue. */
    [[nodiscard]] Drained drain() const noexcept
    {
        std::array<std::uint8_t, 64> bytes {};
        for (;;) {
            const auto result = ::recv(
                read_.get(), bytes.data(), bytes.size(), MSG_DONTWAIT);
            if (result > 0)
                continue;
            if (result == 0)
                return Drained::closed;
            if (errno == EINTR)
                continue;
            return errno == EAGAIN || errno == EWOULDBLOCK ?
                Drained::empty : Drained::failed;
        }
    }

private:
    UniqueDescriptor read_;
    UniqueDescriptor write_;
};

} // namespace uml::platform
