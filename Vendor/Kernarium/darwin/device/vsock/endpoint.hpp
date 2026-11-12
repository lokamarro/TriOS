// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "host_events.hpp"
#include "unique_descriptor.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace uml::vsock {

class Gateway;

/* One open in flight, shared by the thread that opens and the device. */
class Opening final : public std::enable_shared_from_this<Opening> {
public:
    /* `appEnd` is the descriptor the opener receives when the guest accepts,
     * or invalid for a stream the opener already has (attach). */
    Opening(
        std::weak_ptr<Gateway> gateway,
        uml::gpu::UniqueDescriptor appEnd) noexcept;

    /* The opener's side. Waits up to `timeout` for the guest's answer and
     * returns the descriptor (0 for an attached stream), or a negated errno:
     * ECONNREFUSED when nothing listens on the port, ECONNRESET when the
     * guest went away, ENODEV when the service ended, EMFILE when the table
     * is full, ETIMEDOUT when there was no answer, which also abandons the
     * open, so that a late answer is refused. It may be called once. */
    [[nodiscard]] int wait(std::chrono::milliseconds timeout);

    /* The opener's side, from another thread: gives the open up now. A
     * wait() in progress returns ECANCELED. */
    void cancel();

    /* The device's side. succeed() is false when the opener already gave up,
     * and the device then refuses the connection itself. */
    [[nodiscard]] bool succeed();
    void fail(int error);

private:
    enum class State : std::uint8_t {
        pending,
        connected,
        failed,
        abandoned,
        taken,
    };

    std::weak_ptr<Gateway> gateway_;
    std::mutex mutex_;
    std::condition_variable changed_;
    State state_ {State::pending};
    int error_ {};
    uml::gpu::UniqueDescriptor appEnd_;
};

/* What crosses from any thread to the device: opens and abandoned opens. The
 * gateway also owns the event queue the device is polled through, so a
 * request wakes it. */
class Gateway final {
public:
    struct Request final {
        std::shared_ptr<Opening> opening;
        uml::gpu::UniqueDescriptor hostEnd;
        std::uint32_t guestPort {};
    };

    /* How many connections, open and opening, the device keeps. */
    static constexpr std::size_t maximumConnections = 32;

    Gateway() = default;
    Gateway(const Gateway&) = delete;
    Gateway& operator=(const Gateway&) = delete;

    [[nodiscard]] HostEvents& events() noexcept { return events_; }

    /* Any thread. 0, or a negated errno: ENODEV once the device has ended,
     * EMFILE above the limit. */
    [[nodiscard]] int submit(Request request);
    void abandon(std::shared_ptr<Opening> opening);

    /* The device's thread. */
    void take(
        std::vector<Request>& requests,
        std::vector<std::shared_ptr<Opening>>& abandoned);
    void publishLive(std::size_t count) noexcept;
    /* The device has ended: queued opens fail with `error`, and later ones
     * with ENODEV. */
    void close(int error) noexcept;

private:
    HostEvents events_;
    std::mutex mutex_;
    std::vector<Request> requests_;
    std::vector<std::shared_ptr<Opening>> abandoned_;
    std::size_t live_ {};
    bool closed_ {};
};

/* The open API of the device: a connected stream to a guest port. Thread-safe;
 * it holds nothing but the gateway, so it outlives its device and then
 * refuses with ENODEV. */
class Endpoint final {
public:
    /* The send and receive buffer of the host's end of a connection. */
    static constexpr int socketBufferBytes = 256 * 1024;

    explicit Endpoint(std::shared_ptr<Gateway> gateway) noexcept;

    /* A connected SOCK_STREAM descriptor the caller owns, or a negated errno
     * (see Opening::wait). It blocks for up to `timeout`. */
    [[nodiscard]] int connect(
        std::uint32_t guestPort,
        std::chrono::milliseconds timeout);

    /* The same for a stream the caller already has, such as an accepted TCP
     * socket: the device owns `stream` from here on, closes it when the open
     * fails, and the result is 0 when the guest accepts. */
    [[nodiscard]] int attach(
        uml::gpu::UniqueDescriptor stream,
        std::uint32_t guestPort,
        std::chrono::milliseconds timeout);

    /* The two halves of connect() and attach(), for a caller that must not
     * block while the device runs on the same thread. On success `opening` is
     * set and its wait() gives the result. */
    [[nodiscard]] int begin(
        std::uint32_t guestPort,
        std::shared_ptr<Opening>& opening);
    [[nodiscard]] int beginAttach(
        uml::gpu::UniqueDescriptor stream,
        std::uint32_t guestPort,
        std::shared_ptr<Opening>& opening);

private:
    std::shared_ptr<Gateway> gateway_;
};

} // namespace uml::vsock
