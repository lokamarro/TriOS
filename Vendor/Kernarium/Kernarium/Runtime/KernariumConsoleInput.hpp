// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string_view>
#include <system_error>

namespace kernarium::console {

/*
 * The host end of the guest console socket. The guest reads its console only
 * while it runs and its line is not throttled, so a write waits for room for
 * as long as that takes, as a write to a terminal does. It never waits past
 * stop(): the stop wakes a waiting write at once, and every later write fails
 * with ECANCELED. The runtime stops the input when the guest's run ends, so
 * nothing stays blocked on a guest that is gone.
 *
 * Darwin's send() blocks on a local stream socket even with MSG_DONTWAIT, so
 * the socket itself is made nonblocking and both directions wait in poll().
 * The end owns the socket once it is constructed; a constructor that throws
 * leaves the socket with the caller. The one reader, which relays guest
 * output, is the one that calls close().
 */
class HostEnd final {
public:
    explicit HostEnd(int socket)
    {
        const int flags = ::fcntl(socket, F_GETFL);
        if (flags < 0 || ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) != 0)
            throw std::system_error(errno, std::generic_category(),
                "console socket");
        int descriptors[2] {-1, -1};
        if (::pipe(descriptors) != 0)
            throw std::system_error(errno, std::generic_category(),
                "console input stop pipe");
        for (const int descriptor : descriptors) {
            const int pipeFlags = ::fcntl(descriptor, F_GETFL);
            if (pipeFlags < 0 ||
                ::fcntl(descriptor, F_SETFL, pipeFlags | O_NONBLOCK) != 0 ||
                ::fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0) {
                const int error = errno;
                ::close(descriptors[0]);
                ::close(descriptors[1]);
                throw std::system_error(error, std::generic_category(),
                    "console input stop pipe");
            }
        }
        socket_ = socket;
        stopRead_ = descriptors[0];
        stopWrite_ = descriptors[1];
    }

    ~HostEnd()
    {
        if (socket_ >= 0)
            ::close(socket_);
        ::close(stopRead_);
        ::close(stopWrite_);
    }

    HostEnd(const HostEnd&) = delete;
    HostEnd& operator=(const HostEnd&) = delete;

    /* Receives guest output, waiting until some arrives. Returns the count,
     * 0 once the guest end has closed, or -1 with errno set. Only the reader
     * calls it, and never after its own close(). */
    [[nodiscard]] ssize_t receive(void* buffer, std::size_t capacity) noexcept
    {
        for (;;) {
            const ssize_t received = ::recv(socket_, buffer, capacity, 0);
            if (received >= 0)
                return received;
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN)
                return -1;
            pollfd waiting {socket_, POLLIN, 0};
            if (::poll(&waiting, 1, -1) < 0 && errno != EINTR)
                return -1;
        }
    }

#ifndef UML_APP_STORE_RUNTIME
    /* Writes all of `buffer`. Returns 0, or ECANCELED once the input is
     * stopped, ENOTCONN once the end is closed, or the socket's error. An App
     * Store runtime has no console input. */
    [[nodiscard]] int send(const void* buffer, std::size_t length) noexcept
    {
        if (length == 0U)
            return 0;
        if (buffer == nullptr)
            return EINVAL;
        std::lock_guard lock(writeMutex_);
        if (socket_ < 0)
            return ENOTCONN;
        std::string_view remaining {
            static_cast<const char*>(buffer), length};
        while (!remaining.empty()) {
            if (stopped_.load(std::memory_order_acquire))
                return ECANCELED;
            const ssize_t sent = ::send(
                socket_, remaining.data(), remaining.size(), 0);
            if (sent > 0) {
                remaining.remove_prefix(static_cast<std::size_t>(sent));
                continue;
            }
            if (sent == 0)
                return EIO;
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN)
                return errno;
            pollfd waiting[2] {
                {socket_, POLLOUT, 0},
                {stopRead_, POLLIN, 0},
            };
            if (::poll(waiting, 2, -1) < 0 && errno != EINTR)
                return errno;
        }
        return 0;
    }
#endif

    /* Ends a write that waits for room, and refuses every later one. The stop
     * pipe is never drained, so it stays readable for every later wait. */
    void stop() noexcept
    {
        if (stopped_.exchange(true, std::memory_order_acq_rel))
            return;
        const char signal = 0;
        while (::write(stopWrite_, &signal, 1) < 0 && errno == EINTR) {
        }
    }

    void close() noexcept
    {
        stop();
        std::lock_guard lock(writeMutex_);
        if (socket_ >= 0) {
            ::close(socket_);
            socket_ = -1;
        }
    }

private:
    std::mutex writeMutex_;
    int socket_ {-1};
    int stopRead_ {-1};
    int stopWrite_ {-1};
    std::atomic<bool> stopped_ {};
};

#ifndef UML_APP_STORE_RUNTIME
/*
 * The console input the application reaches through the runtime API while a
 * guest runs. A caller that finds an end published writes to it without
 * holding the registry's mutex, so neither publishing nor retiring ever waits
 * behind a guest that has stopped reading. Retiring stops the input, which
 * ends such a write at once, and returns when the last one has left, so the
 * end may be destroyed afterwards.
 */
class InputRegistry final {
public:
    void publish(HostEnd& end) noexcept
    {
        std::lock_guard lock(mutex_);
        active_ = &end;
    }

    void retire() noexcept
    {
        std::unique_lock lock(mutex_);
        if (active_ != nullptr)
            active_->stop();
        active_ = nullptr;
        idle_.wait(lock, [this] { return writers_ == 0U; });
    }

    /* Returns 0, ENOTCONN when no end is published, or HostEnd::send's
     * error. */
    [[nodiscard]] int send(const void* buffer, std::size_t length) noexcept
    {
        HostEnd* end;
        {
            std::lock_guard lock(mutex_);
            if (active_ == nullptr)
                return ENOTCONN;
            end = active_;
            ++writers_;
        }
        const int error = end->send(buffer, length);
        std::lock_guard lock(mutex_);
        if (--writers_ == 0U)
            idle_.notify_all();
        return error;
    }

private:
    std::mutex mutex_;
    std::condition_variable idle_;
    HostEnd* active_ {};
    std::size_t writers_ {};
};
#endif

} // namespace kernarium::console
