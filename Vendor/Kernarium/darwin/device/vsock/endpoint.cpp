// SPDX-License-Identifier: GPL-2.0-or-later

#include "endpoint.hpp"

#include <array>
#include <cerrno>
#include <limits>
#include <utility>

#include <sys/socket.h>

namespace uml::vsock {
namespace {

/* VMADDR_PORT_ANY, which no guest listens on. */
constexpr std::uint32_t anyPort = std::numeric_limits<std::uint32_t>::max();

void sizeBuffers(int descriptor) noexcept
{
    const int bytes = Endpoint::socketBufferBytes;
    /* A smaller buffer than asked for only costs speed. */
    (void)::setsockopt(descriptor, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes));
    (void)::setsockopt(descriptor, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
}

} // namespace

Opening::Opening(
    std::weak_ptr<Gateway> gateway,
    uml::gpu::UniqueDescriptor appEnd) noexcept
    : gateway_(std::move(gateway))
    , appEnd_(std::move(appEnd))
{
}

int Opening::wait(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    if (!changed_.wait_for(
            lock, timeout, [this] { return state_ != State::pending; })) {
        state_ = State::abandoned;
        lock.unlock();
        if (const auto gateway = gateway_.lock())
            gateway->abandon(shared_from_this());
        return -ETIMEDOUT;
    }
    switch (state_) {
    case State::connected:
        state_ = State::taken;
        return appEnd_.get() >= 0 ? appEnd_.release() : 0;
    case State::failed:
        state_ = State::taken;
        return -error_;
    case State::abandoned:
        return -ECANCELED;
    case State::pending:
    case State::taken:
        break;
    }
    return -EALREADY;
}

void Opening::cancel()
{
    {
        std::lock_guard lock(mutex_);
        if (state_ != State::pending)
            return;
        state_ = State::abandoned;
    }
    changed_.notify_all();
    if (const auto gateway = gateway_.lock())
        gateway->abandon(shared_from_this());
}

bool Opening::succeed()
{
    {
        std::lock_guard lock(mutex_);
        if (state_ != State::pending)
            return false;
        state_ = State::connected;
    }
    changed_.notify_all();
    return true;
}

void Opening::fail(int error)
{
    {
        std::lock_guard lock(mutex_);
        if (state_ != State::pending)
            return;
        state_ = State::failed;
        error_ = error;
        /* The opener never receives its end of a refused connection. */
        appEnd_.reset();
    }
    changed_.notify_all();
}

int Gateway::submit(Request request)
{
    {
        std::lock_guard lock(mutex_);
        if (closed_)
            return -ENODEV;
        if (live_ + requests_.size() >= maximumConnections)
            return -EMFILE;
        requests_.push_back(std::move(request));
    }
    events_.trigger();
    return 0;
}

void Gateway::abandon(std::shared_ptr<Opening> opening)
{
    {
        std::lock_guard lock(mutex_);
        if (closed_)
            return;
        abandoned_.push_back(std::move(opening));
    }
    events_.trigger();
}

void Gateway::take(
    std::vector<Request>& requests,
    std::vector<std::shared_ptr<Opening>>& abandoned)
{
    std::lock_guard lock(mutex_);
    for (auto& request : requests_)
        requests.push_back(std::move(request));
    requests_.clear();
    for (auto& opening : abandoned_)
        abandoned.push_back(std::move(opening));
    abandoned_.clear();
}

void Gateway::publishLive(std::size_t count) noexcept
{
    std::lock_guard lock(mutex_);
    live_ = count;
}

void Gateway::close(int error) noexcept
{
    std::vector<Request> queued;
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        queued.swap(requests_);
        abandoned_.clear();
        live_ = 0;
    }
    for (auto& request : queued)
        request.opening->fail(error);
}

Endpoint::Endpoint(std::shared_ptr<Gateway> gateway) noexcept
    : gateway_(std::move(gateway))
{
}

int Endpoint::begin(
    std::uint32_t guestPort,
    std::shared_ptr<Opening>& opening)
{
    if (guestPort == anyPort)
        return -EINVAL;
    std::array<int, 2> pair {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair.data()) != 0)
        return -errno;
    uml::gpu::UniqueDescriptor hostEnd {pair[0]};
    uml::gpu::UniqueDescriptor appEnd {pair[1]};
    if (!uml::platform::configureDescriptor(
            hostEnd.get(),
            {.closeOnExec = true, .nonblocking = true, .noSigpipe = true}) ||
        !uml::platform::configureDescriptor(
            appEnd.get(), {.closeOnExec = true, .noSigpipe = true}))
        return -errno;
    sizeBuffers(hostEnd.get());
    sizeBuffers(appEnd.get());
    auto started = std::make_shared<Opening>(gateway_, std::move(appEnd));
    const int submitted = gateway_->submit(
        {.opening = started,
         .hostEnd = std::move(hostEnd),
         .guestPort = guestPort});
    if (submitted != 0)
        return submitted;
    opening = std::move(started);
    return 0;
}

int Endpoint::beginAttach(
    uml::gpu::UniqueDescriptor stream,
    std::uint32_t guestPort,
    std::shared_ptr<Opening>& opening)
{
    if (guestPort == anyPort || stream.get() < 0)
        return -EINVAL;
    if (!uml::platform::configureDescriptor(
            stream.get(),
            {.closeOnExec = true, .nonblocking = true, .noSigpipe = true}))
        return -errno;
    sizeBuffers(stream.get());
    auto started = std::make_shared<Opening>(
        gateway_, uml::gpu::UniqueDescriptor {});
    const int submitted = gateway_->submit(
        {.opening = started,
         .hostEnd = std::move(stream),
         .guestPort = guestPort});
    if (submitted != 0)
        return submitted;
    opening = std::move(started);
    return 0;
}

int Endpoint::connect(
    std::uint32_t guestPort,
    std::chrono::milliseconds timeout)
{
    std::shared_ptr<Opening> opening;
    if (const int started = begin(guestPort, opening); started != 0)
        return started;
    return opening->wait(timeout);
}

int Endpoint::attach(
    uml::gpu::UniqueDescriptor stream,
    std::uint32_t guestPort,
    std::chrono::milliseconds timeout)
{
    std::shared_ptr<Opening> opening;
    if (const int started = beginAttach(std::move(stream), guestPort, opening);
        started != 0)
        return started;
    return opening->wait(timeout);
}

} // namespace uml::vsock
