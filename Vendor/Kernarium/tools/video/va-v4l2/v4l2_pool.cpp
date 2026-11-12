// SPDX-License-Identifier: GPL-2.0-or-later
#include "v4l2_pool.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace uml::va {

Lease& Lease::operator=(Lease&& other) noexcept
{
    if (this == &other) return *this;
    giveBack(false);
    pool_ = other.pool_;
    io_ = other.io_;
    role_ = other.role_;
    device_ = std::move(other.device_);
    fd_ = other.fd_;
    pooled_ = other.pooled_;
    reused_ = other.reused_;
    other.pool_ = nullptr;
    other.fd_ = -1;
    return *this;
}

void Lease::giveBack(bool healthy, bool used) noexcept
{
    if (fd_ < 0) return;
    const int fd = fd_;
    fd_ = -1;
    pool_->giveBack(role_, device_, fd, healthy, used || reused_);
}

DescriptorPool::~DescriptorPool()
{
    for (auto& stock : stocks_)
        for (auto& entry : stock.idle) io_.close(entry.fd);
}

DescriptorPool& DescriptorPool::process()
{
    static DescriptorPool* pool = new DescriptorPool(systemIo());
    return *pool;
}

void DescriptorPool::setTarget(Role role, unsigned count)
{
    std::lock_guard guard(lock_);
    stock(role).target = count;
}

unsigned DescriptorPool::target(Role role) const
{
    std::lock_guard guard(lock_);
    return stock(role).target;
}

unsigned DescriptorPool::idle(Role role) const
{
    std::lock_guard guard(lock_);
    return unsigned(stock(role).idle.size());
}

unsigned DescriptorPool::held(Role role) const
{
    std::lock_guard guard(lock_);
    return stock(role).held;
}

std::string DescriptorPool::knownDevice(Role role) const
{
    std::lock_guard guard(lock_);
    return stock(role).device;
}

unsigned DescriptorPool::fill(Role role, const std::string& device)
{
    if (device.empty()) return held(role);
    std::lock_guard guard(lock_);
    Stock& own = stock(role);
    own.device = device;
    while (own.held < own.target) {
        const int fd = io_.open(device.c_str());
        if (fd < 0) break;
        own.idle.push_back({device, fd});
        ++own.held;
    }
    return own.held;
}

Lease DescriptorPool::acquire(Role role, const std::string& device, std::string& error)
{
    Lease lease;
    if (device.empty()) {
        error = role == Role::encoder ? "no Virtio video encoder node" : "no Virtio video decoder node";
        return lease;
    }
    std::lock_guard guard(lock_);
    Stock& own = stock(role);
    const auto found = std::find_if(own.idle.begin(), own.idle.end(),
                                    [&device](const Entry& entry) { return entry.device == device; });
    if (found != own.idle.end()) {
        lease.fd_ = found->fd;
        lease.pooled_ = true;
        lease.reused_ = found->used;
        own.idle.erase(found);
    } else {
        const int fd = io_.open(device.c_str());
        if (fd < 0) {
            error = std::string("open ") + device + ": " + std::strerror(errno);
            return lease;
        }
        lease.fd_ = fd;
        ++own.held;
    }
    lease.pool_ = this;
    lease.io_ = &io_;
    lease.role_ = role;
    lease.device_ = device;
    return lease;
}

void DescriptorPool::giveBack(Role role, const std::string& device, int fd, bool healthy, bool used) noexcept
{
    std::lock_guard guard(lock_);
    Stock& own = stock(role);
    // A descriptor opened because the stock ran out is one more than the pool
    // is meant to hold, and goes when its session does.
    const bool keep = healthy && own.held <= own.target;
    if (!keep) {
        io_.close(fd);
        if (own.held) --own.held;
        return;
    }
    own.idle.push_back({device, fd, used});
}

PoolSizes poolSizes(const char* request) noexcept
{
    constexpr unsigned kDefault = 4;
    constexpr unsigned kMost = 16;
    PoolSizes sizes {kDefault, kDefault};
    if (!request || !*request) return sizes;
    char* end = nullptr;
    const unsigned long first = std::strtoul(request, &end, 10);
    if (end == request) return sizes;
    sizes.encoders = sizes.decoders = unsigned(std::min<unsigned long>(first, kMost));
    if (*end == ',') {
        const char* rest = end + 1;
        const unsigned long second = std::strtoul(rest, &end, 10);
        if (end != rest) sizes.decoders = unsigned(std::min<unsigned long>(second, kMost));
    }
    return sizes;
}

}
