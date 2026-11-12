// SPDX-License-Identifier: GPL-2.0-or-later

#include "forwarder.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>

namespace uml::vsock {
namespace {

constexpr std::uint32_t anyGuestPort = 0xFFFFFFFFU;

bool parseNumber(std::string_view text, std::uint64_t maximum, std::uint64_t& value)
{
    if (text.empty() || text.size() > 10)
        return false;
    std::uint64_t parsed = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc {} || end != text.data() + text.size() ||
        parsed > maximum)
        return false;
    value = parsed;
    return true;
}

} // namespace

std::optional<ForwardSpecification> parseForward(
    std::string_view text,
    std::string& error)
{
    const auto fail = [&](std::string reason) -> std::optional<ForwardSpecification> {
        error = "invalid vsock forward '" + std::string(text) + "': " + reason;
        return std::nullopt;
    };
    const auto dash = text.rfind('-');
    if (dash == std::string_view::npos)
        return fail("expected [127.0.0.1:]HOSTPORT-GUESTPORT");
    auto host = text.substr(0, dash);
    const auto guest = text.substr(dash + 1);
    ForwardSpecification specification;
    if (const auto colon = host.find(':'); colon != std::string_view::npos) {
        specification.hostAddress = std::string(host.substr(0, colon));
        host = host.substr(colon + 1);
        in_addr address {};
        if (inet_pton(AF_INET, specification.hostAddress.c_str(), &address) != 1 ||
            (ntohl(address.s_addr) >> 24) != 127U)
            return fail("only a loopback address is accepted");
    }
    std::uint64_t hostPort = 0;
    std::uint64_t guestPort = 0;
    if (!parseNumber(host, 65535, hostPort))
        return fail("the host port must be 0 to 65535");
    if (!parseNumber(guest, anyGuestPort - 1U, guestPort) || guestPort == 0)
        return fail("the guest port must be a positive port number");
    specification.hostPort = static_cast<std::uint16_t>(hostPort);
    specification.guestPort = static_cast<std::uint32_t>(guestPort);
    return specification;
}

struct Forwarder::Listener final {
    ForwardSpecification specification;
    uml::gpu::UniqueDescriptor descriptor;
    std::uint16_t port {};
};

struct Forwarder::Worker final {
    std::thread thread;
    std::atomic_bool done {};
    std::mutex mutex;
    std::shared_ptr<Opening> opening;
    bool cancelled {};
};

Forwarder::Forwarder(
    std::shared_ptr<Endpoint> endpoint,
    std::vector<ForwardSpecification> forwards)
    : endpoint_(std::move(endpoint))
    , wake_(std::make_unique<uml::platform::WakeSocket>("vsock forwarder wake"))
{
    for (auto& specification : forwards) {
        auto listener = std::make_unique<Listener>();
        listener->specification = std::move(specification);
        const auto describe = [&] {
            return listener->specification.hostAddress + ":" +
                std::to_string(listener->specification.hostPort);
        };
        listener->descriptor.reset(::socket(AF_INET, SOCK_STREAM, 0));
        if (listener->descriptor.get() < 0) {
            throw std::runtime_error(
                "cannot create the vsock forward socket: " +
                std::string(std::strerror(errno)));
        }
        const int enabled = 1;
        (void)::setsockopt(
            listener->descriptor.get(), SOL_SOCKET, SO_REUSEADDR, &enabled,
            sizeof(enabled));
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_len = sizeof(address);
        address.sin_port = htons(listener->specification.hostPort);
        (void)inet_pton(
            AF_INET, listener->specification.hostAddress.c_str(),
            &address.sin_addr);
        if (!uml::platform::configureDescriptor(
                listener->descriptor.get(),
                {.closeOnExec = true, .nonblocking = true}) ||
            ::bind(
                listener->descriptor.get(),
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0 ||
            ::listen(listener->descriptor.get(), 16) != 0) {
            throw std::runtime_error(
                "cannot listen on " + describe() + " for the vsock forward: " +
                std::strerror(errno));
        }
        socklen_t length = sizeof(address);
        if (::getsockname(
                listener->descriptor.get(),
                reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            throw std::runtime_error(
                "cannot read the port of the vsock forward on " + describe());
        }
        listener->port = ntohs(address.sin_port);
        listeners_.push_back(std::move(listener));
    }
    thread_ = std::thread([this] { run(); });
}

Forwarder::~Forwarder()
{
    stopping_.store(true, std::memory_order_release);
    (void)wake_->notify();
    if (thread_.joinable())
        thread_.join();
    /* Connections still waiting for the guest are given up. */
    std::list<std::unique_ptr<Worker>> workers;
    {
        std::lock_guard lock(workersMutex_);
        workers.swap(workers_);
    }
    for (auto& worker : workers) {
        {
            std::lock_guard lock(worker->mutex);
            worker->cancelled = true;
            if (worker->opening)
                worker->opening->cancel();
        }
        if (worker->thread.joinable())
            worker->thread.join();
    }
}

std::uint16_t Forwarder::port(std::size_t index) const
{
    return listeners_.at(index)->port;
}

void Forwarder::run()
{
    std::vector<pollfd> descriptors(listeners_.size() + 1U);
    while (!stopping_.load(std::memory_order_acquire)) {
        for (std::size_t index = 0; index < listeners_.size(); ++index) {
            descriptors[index] = {
                .fd = listeners_[index]->descriptor.get(),
                .events = POLLIN,
                .revents = 0,
            };
        }
        descriptors.back() = {
            .fd = wake_->descriptor(),
            .events = POLLIN,
            .revents = 0,
        };
        if (::poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), 1000) < 0 &&
            errno != EINTR)
            break;
        reap();
        if (stopping_.load(std::memory_order_acquire))
            break;
        for (std::size_t index = 0; index < listeners_.size(); ++index) {
            if ((descriptors[index].revents & POLLIN) == 0)
                continue;
            for (;;) {
                const int accepted =
                    ::accept(listeners_[index]->descriptor.get(), nullptr, nullptr);
                if (accepted < 0)
                    break;
                auto worker = std::make_unique<Worker>();
                auto* raw = worker.get();
                const auto guestPort = listeners_[index]->specification.guestPort;
                {
                    std::lock_guard lock(workersMutex_);
                    workers_.push_back(std::move(worker));
                }
                raw->thread = std::thread(
                    [this, raw, accepted, guestPort] { serve(*raw, accepted, guestPort); });
            }
        }
    }
}

void Forwarder::serve(Worker& worker, int accepted, std::uint32_t guestPort)
{
    uml::gpu::UniqueDescriptor stream {accepted};
    std::shared_ptr<Opening> opening;
    const int started = endpoint_->beginAttach(std::move(stream), guestPort, opening);
    if (started == 0) {
        {
            std::lock_guard lock(worker.mutex);
            if (worker.cancelled)
                opening->cancel();
            worker.opening = opening;
        }
        const int result = opening->wait(acceptTimeout);
        if (result != 0 && result != -ECANCELED) {
            std::fprintf(
                stderr, "vsock forward to guest port %u: %s\n", guestPort,
                std::strerror(-result));
        }
    } else {
        std::fprintf(
            stderr, "vsock forward to guest port %u: %s\n", guestPort,
            std::strerror(-started));
    }
    worker.done.store(true, std::memory_order_release);
}

void Forwarder::reap()
{
    std::list<std::unique_ptr<Worker>> finished;
    {
        std::lock_guard lock(workersMutex_);
        for (auto it = workers_.begin(); it != workers_.end();) {
            if ((*it)->done.load(std::memory_order_acquire)) {
                finished.push_back(std::move(*it));
                it = workers_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& worker : finished) {
        if (worker->thread.joinable())
            worker->thread.join();
    }
}

} // namespace uml::vsock
