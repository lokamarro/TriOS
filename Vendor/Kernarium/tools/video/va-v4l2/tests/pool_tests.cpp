// SPDX-License-Identifier: GPL-2.0-or-later
// The descriptor pool against an I/O layer that opens nothing: what it opens,
// what it lends, what it keeps, and what it does when the nodes can no longer
// be opened, which is how a sandboxed process sees them.
#include <cerrno>
#include <map>
#include <set>
#include <string>

#include "../v4l2_pool.hpp"
#include "test_support.hpp"

namespace uml::va::test {
namespace {

class CountingIo final : public V4l2Io {
public:
    int open(const char* path) override
    {
        if (denied) { errno = EACCES; return -1; }
        const int fd = next++;
        opened[fd] = path;
        ++opens;
        return fd;
    }
    int close(int fd) override
    {
        const bool known = opened.erase(fd) != 0;
        if (known) ++closes;
        return known ? 0 : -1;
    }
    int ioctl(int, unsigned long, void*) override { return -1; }
    void* mmap(int, std::size_t, std::int64_t) override { return nullptr; }
    int munmap(void*, std::size_t) override { return -1; }
    int poll(pollfd*, unsigned long, int) override { return -1; }

    bool denied {};
    int next {100};
    int opens {};
    int closes {};
    std::map<int, std::string> opened;
};

void testSizes()
{
    PoolSizes sizes = poolSizes(nullptr);
    check(sizes.encoders == 4 && sizes.decoders == 4, "the default pool is four of each");
    sizes = poolSizes("");
    check(sizes.encoders == 4 && sizes.decoders == 4, "an empty request is the default");
    sizes = poolSizes("2,6");
    check(sizes.encoders == 2 && sizes.decoders == 6, "a request names encoders then decoders");
    sizes = poolSizes("3");
    check(sizes.encoders == 3 && sizes.decoders == 3, "one number is both");
    sizes = poolSizes("0,0");
    check(sizes.encoders == 0 && sizes.decoders == 0, "zero turns the pool off");
    sizes = poolSizes("99,40");
    check(sizes.encoders == 16 && sizes.decoders == 16, "a count is capped at what the host keeps");
    sizes = poolSizes("many");
    check(sizes.encoders == 4 && sizes.decoders == 4, "an unusable request is the default");
    sizes = poolSizes("5,x");
    check(sizes.encoders == 5 && sizes.decoders == 5, "an unusable second count is the first");
}

void testFillAndReuse()
{
    CountingIo io;
    DescriptorPool pool(io);
    pool.setTarget(Role::encoder, 3);
    pool.setTarget(Role::decoder, 2);
    check(pool.fill(Role::encoder, "/dev/video1") == 3, "fill reaches the target");
    check(pool.fill(Role::encoder, "/dev/video1") == 3 && io.opens == 3, "a second fill opens nothing");
    check(pool.idle(Role::encoder) == 3 && pool.held(Role::encoder) == 3, "every filled descriptor is idle");
    check(pool.fill(Role::decoder, "/dev/video0") == 2 && io.opens == 5, "the roles are filled separately");
    check(pool.knownDevice(Role::encoder) == "/dev/video1", "the node a fill opened is remembered");
    check(pool.fill(Role::decoder, "") == 2, "an empty node name opens nothing");

    std::string error;
    Lease first = pool.acquire(Role::encoder, "/dev/video1", error);
    check(first && first.pooled() && error.empty(), "a lease takes an idle descriptor");
    check(pool.idle(Role::encoder) == 2 && pool.held(Role::encoder) == 3, "a lent descriptor is held, not idle");
    const int fd = first.fd();
    first.giveBack(true);
    check(!first && pool.idle(Role::encoder) == 3, "a healthy descriptor goes back to the stock");
    first.giveBack(true);
    check(pool.idle(Role::encoder) == 3 && io.closes == 0, "giving back twice does nothing");
    Lease second = pool.acquire(Role::encoder, "/dev/video1", error);
    check(second.fd() == fd || second.fd() > 0, "a lease is served from the stock");
    check(io.opens == 5, "reuse opens nothing");

    // A lease that is dropped says nothing about the device's state.
    { Lease dropped = std::move(second); }
    check(io.closes == 1 && pool.held(Role::encoder) == 2 && pool.idle(Role::encoder) == 2,
          "a dropped lease closes its descriptor");
    check(pool.fill(Role::encoder, "/dev/video1") == 3 && io.opens == 6, "a later fill tops the stock up");

    Lease other = pool.acquire(Role::decoder, "/dev/video0", error);
    check(other && pool.idle(Role::decoder) == 1 && pool.idle(Role::encoder) == 3,
          "a role takes from its own stock");
    other.giveBack(false);
    check(pool.held(Role::decoder) == 1 && io.closes == 2, "a spoiled descriptor is closed and no longer held");
}

void testSandbox()
{
    CountingIo io;
    DescriptorPool pool(io);
    pool.setTarget(Role::encoder, 2);
    pool.fill(Role::encoder, "/dev/video1");
    io.denied = true;

    std::string error;
    Lease a = pool.acquire(Role::encoder, "/dev/video1", error);
    Lease b = pool.acquire(Role::encoder, "/dev/video1", error);
    check(a && b && a.pooled() && b.pooled(), "a sandboxed process is served from the stock");
    Lease c = pool.acquire(Role::encoder, "/dev/video1", error);
    check(!c && !error.empty(), "an empty stock that cannot open reports why");
    check(error.find("/dev/video1") != std::string::npos, "the error names the node");
    a.giveBack(true);
    Lease d = pool.acquire(Role::encoder, "/dev/video1", error);
    check(d && d.pooled(), "a returned descriptor serves the next session");
    check(pool.fill(Role::encoder, "/dev/video1") == 2, "a fill that cannot open leaves the pool as it was");
    Lease none = pool.acquire(Role::decoder, "/dev/video0", error);
    check(!none, "a role with no stock gets nothing");
    Lease nameless = pool.acquire(Role::encoder, "", error);
    check(!nameless && error.find("encoder") != std::string::npos, "no node is its own error");
}

void testGrowth()
{
    CountingIo io;
    DescriptorPool pool(io);
    pool.setTarget(Role::decoder, 1);
    pool.fill(Role::decoder, "/dev/video0");
    std::string error;
    Lease a = pool.acquire(Role::decoder, "/dev/video0", error);
    Lease b = pool.acquire(Role::decoder, "/dev/video0", error);
    check(a && b && a.pooled() && !b.pooled(), "an unsandboxed process may take more than the stock");
    check(pool.held(Role::decoder) == 2, "the extra descriptor is held while it is lent");
    b.giveBack(true);
    check(pool.held(Role::decoder) == 1 && pool.idle(Role::decoder) == 0 && io.closes == 1,
          "an extra descriptor is closed when its session ends");
    a.giveBack(true);
    check(pool.idle(Role::decoder) == 1, "the stock keeps its own");

    CountingIo off;
    DescriptorPool disabled(off);
    check(disabled.fill(Role::encoder, "/dev/video1") == 0 && off.opens == 0, "a target of zero opens nothing ahead");
    Lease lazy = disabled.acquire(Role::encoder, "/dev/video1", error);
    check(lazy && !lazy.pooled(), "a disabled pool still serves a session by opening");
    lazy.giveBack(true);
    check(off.closes == 1 && disabled.held(Role::encoder) == 0, "and keeps nothing afterwards");
}

void testReuseFlag()
{
    CountingIo io;
    DescriptorPool pool(io);
    pool.setTarget(Role::encoder, 1);
    pool.fill(Role::encoder, "/dev/video1");
    std::string error;
    Lease probe = pool.acquire(Role::encoder, "/dev/video1", error);
    check(probe && !probe.reused(), "a descriptor nobody has used is not reused");
    probe.giveBack(true, false);
    Lease session = pool.acquire(Role::encoder, "/dev/video1", error);
    check(session && !session.reused(), "a descriptor that was only read from is not reused");
    session.giveBack(true);
    Lease next = pool.acquire(Role::encoder, "/dev/video1", error);
    check(next && next.reused(), "a descriptor a session used is reused");
    next.giveBack(true, false);
    Lease again = pool.acquire(Role::encoder, "/dev/video1", error);
    check(again && again.reused(), "reading from a used descriptor does not make it new");
}

void testDestruction()
{
    CountingIo io;
    {
        DescriptorPool pool(io);
        pool.setTarget(Role::encoder, 2);
        pool.fill(Role::encoder, "/dev/video1");
    }
    check(io.opened.empty(), "a pool closes the descriptors it still holds");
}

}

void runPoolTests()
{
    testSizes();
    testFillAndReuse();
    testSandbox();
    testGrowth();
    testReuseFlag();
    testDestruction();
}

}
