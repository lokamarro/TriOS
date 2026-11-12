// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <mutex>
#include <string>
#include <vector>

#include "v4l2_io.hpp"

namespace uml::va {

enum class Role { encoder, decoder };

class DescriptorPool;

// A descriptor on loan from the pool. A session that is done with it hands it
// back with giveBack(); a lease that is simply dropped closes its descriptor,
// because nothing says what state the device was left in.
class Lease {
public:
    Lease() = default;
    Lease(Lease&& other) noexcept { *this = std::move(other); }
    Lease& operator=(Lease&& other) noexcept;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    ~Lease() { giveBack(false); }

    int fd() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }
    // Whether the descriptor was opened ahead of time rather than for this
    // lease. A pooled descriptor is the one a sandboxed process can still get.
    bool pooled() const noexcept { return pooled_; }
    // Whether an earlier session has already used the descriptor. Whatever
    // that session left set on it - a control, above all - is still set.
    bool reused() const noexcept { return reused_; }
    V4l2Io& io() const noexcept { return *io_; }

    // A healthy descriptor has had both of its queues stopped and released, so
    // the next session can start from the initialisation sequence; the pool
    // keeps it. Anything else is closed. `used` says whether anything beyond
    // reading the node's capabilities was done with it.
    void giveBack(bool healthy, bool used = true) noexcept;

private:
    friend class DescriptorPool;
    DescriptorPool* pool_ {};
    V4l2Io* io_ {};
    Role role_ {Role::decoder};
    std::string device_;
    int fd_ {-1};
    bool pooled_ {};
    bool reused_ {};
};

// A process-wide stock of open V4L2 descriptors.
//
// A browser's GPU process initialises VA-API before it engages its sandbox,
// and once the sandbox is engaged a device node can no longer be opened, so a
// context created later has nothing to talk to. The pool therefore opens its
// descriptors at the first initialisation of a process and lends them out, and
// takes them back when a session ends. It lives beside the driver's own state
// rather than inside it: libva loads a driver once per process and keeps it,
// and a terminate followed by another initialise must find the same stock.
//
// A process that is not sandboxed loses nothing by this and may ask for more:
// acquire() opens another descriptor whenever the stock is empty and the node
// can still be opened.
class DescriptorPool {
public:
    explicit DescriptorPool(V4l2Io& io) noexcept : io_(io) {}
    DescriptorPool(const DescriptorPool&) = delete;
    DescriptorPool& operator=(const DescriptorPool&) = delete;
    ~DescriptorPool();

    // The pool of this process. It is never destroyed, so that a session torn
    // down by a static destructor can still hand its descriptor back.
    static DescriptorPool& process();

    V4l2Io& io() const noexcept { return io_; }

    // How many descriptors of a role the pool holds, idle or on loan.
    void setTarget(Role role, unsigned count);
    unsigned target(Role role) const;
    // Opens descriptors on `device` until the pool holds its target. Returns
    // how many it holds afterwards; a node that cannot be opened ends the fill.
    unsigned fill(Role role, const std::string& device);
    // A descriptor for a new session: an idle one for `device` when there is
    // one, otherwise a newly opened one. An empty lease and an error say that
    // neither was possible.
    Lease acquire(Role role, const std::string& device, std::string& error);

    unsigned idle(Role role) const;
    unsigned held(Role role) const;
    // The node the last fill() of a role opened, so that a driver initialised
    // again inside a sandbox, where no node can be searched for, finds it.
    std::string knownDevice(Role role) const;

private:
    friend class Lease;
    struct Entry {
        std::string device;
        int fd {-1};
        bool used {};
    };
    struct Stock {
        unsigned target {};
        unsigned held {};
        std::string device;
        std::vector<Entry> idle;
    };

    Stock& stock(Role role) noexcept { return stocks_[role == Role::encoder ? 0 : 1]; }
    const Stock& stock(Role role) const noexcept { return stocks_[role == Role::encoder ? 0 : 1]; }
    void giveBack(Role role, const std::string& device, int fd, bool healthy, bool used) noexcept;

    V4l2Io& io_;
    mutable std::mutex lock_;
    Stock stocks_[2];
};

struct PoolSizes {
    unsigned encoders {};
    unsigned decoders {};
};

// Parses UML_VA_V4L2_POOL: "encoders,decoders", or one number for both. An
// unset or unusable request is four of each; each count is capped at sixteen,
// which is the most streams the host keeps for a device.
PoolSizes poolSizes(const char* request) noexcept;

}
