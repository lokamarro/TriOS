// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "endpoint.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace uml::vsock {

/* A loopback TCP port that reaches a guest vsock port. */
struct ForwardSpecification final {
    std::string hostAddress {"127.0.0.1"};
    /* 0 lets the system choose. */
    std::uint16_t hostPort {};
    std::uint32_t guestPort {};

    [[nodiscard]] bool operator==(const ForwardSpecification&) const = default;
};

/* "[127.0.0.1:]HOSTPORT-GUESTPORT". Only a loopback host address is
 * accepted, because anything that reaches the port reaches the guest's
 * service; the reason is `error` when the result is empty. */
[[nodiscard]] std::optional<ForwardSpecification> parseForward(
    std::string_view text,
    std::string& error);

/* Test tooling: listens on loopback for each forward and attaches every
 * accepted connection to the guest port through the endpoint, so a stock
 * client of a guest service (adb, a script) reaches it. Each connection is
 * served by the device alone; the forwarder moves no bytes. */
class Forwarder final {
public:
    /* How long a connection waits for the guest to accept before it is
     * closed. */
    static constexpr std::chrono::milliseconds acceptTimeout {10000};

    /* Throws std::runtime_error naming the forward that could not listen. */
    Forwarder(
        std::shared_ptr<Endpoint> endpoint,
        std::vector<ForwardSpecification> forwards);
    ~Forwarder();

    Forwarder(const Forwarder&) = delete;
    Forwarder& operator=(const Forwarder&) = delete;

    /* The port forward `index` listens on. */
    [[nodiscard]] std::uint16_t port(std::size_t index) const;

private:
    struct Listener;
    struct Worker;

    void run();
    void serve(Worker& worker, int accepted, std::uint32_t guestPort);
    void reap();

    std::shared_ptr<Endpoint> endpoint_;
    std::vector<std::unique_ptr<Listener>> listeners_;
    std::unique_ptr<uml::platform::WakeSocket> wake_;
    std::atomic_bool stopping_ {};
    std::mutex workersMutex_;
    std::list<std::unique_ptr<Worker>> workers_;
    std::thread thread_;
};

} // namespace uml::vsock
