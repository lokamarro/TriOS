// SPDX-License-Identifier: GPL-2.0-or-later
#include "device_memory_pool.hpp"
#include "require.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace {

using uml::testing::require;
using uml::gpu::DeviceMemoryPool;
using namespace std::chrono_literals;

constexpr std::uint64_t page = 16384;

/* What Metal's allocation counter would report: an allocation appears in it
 * only once its owner has made it. */
struct Counter {
    std::atomic<std::uint64_t> bytes {};
};

void admitsWhatFits()
{
    Counter counter;
    DeviceMemoryPool pool(100 * page, [&] { return counter.bytes.load(); },
        page);
    require(pool.reserve(60 * page), "a reservation that fits is admitted");
    require(pool.usage() == 60 * page, "a reservation counts in the usage");
    require(pool.refusals() == 0, "nothing has been refused yet");
    require(!pool.reserve(41 * page), "one that does not is refused");
    require(pool.usage() == 60 * page, "a refusal charges nothing");
    require(pool.refusals() == 1, "a refusal is counted");
    require(pool.reserve(40 * page), "one that fills the pool is admitted");
    require(!pool.reserve(1), "a full pool admits nothing");

    // The first allocation is made, which the counter now counts, and its
    // reservation is released: the pool is exactly as full.
    counter.bytes += 60 * page;
    pool.release(60 * page);
    require(pool.usage() == 100 * page, "a made allocation replaces its "
                                        "reservation");
    pool.release(40 * page);
    require(pool.usage() == 60 * page, "a failed allocation leaves nothing");

    // Metal charges whole pages, and so does the pool.
    counter.bytes = 0;
    require(pool.reserve(1), "a byte is admitted");
    require(pool.usage() == page, "a byte is charged a page");
    require(pool.reserve(99 * page), "the other pages are admitted");
    require(!pool.reserve(1), "no page is left for another byte");
    pool.release(1);
    pool.release(99 * page);
    require(pool.usage() == 0, "every reservation is returned");
    require(!pool.reserve(std::numeric_limits<std::uint64_t>::max()),
        "a reservation past the limit is refused without overflowing");
}

/* The pool keeps its reserve for what it never refuses: a reservation is
 * admitted up to the ceiling, the limit less the reserve, and one that would
 * cross the ceiling is refused although the limit could hold it. The memory
 * nobody reserves may still fill the reserve. */
void refusesAtTheCeiling()
{
    constexpr std::uint64_t mebibyte = 1ULL << 20;
    constexpr std::uint64_t limit = 534 * mebibyte;
    constexpr std::uint64_t reserve = 16 * mebibyte;
    constexpr std::uint64_t ceiling = limit - reserve;
    Counter counter;
    unsigned calls = 0;
    DeviceMemoryPool pool(limit, [&] { return counter.bytes.load(); }, page,
        {}, reserve);
    pool.setReclaim([&]() -> std::uint64_t {
        ++calls;
        return 0;
    });
    require(pool.limit() == limit, "the limit is still B");
    require(pool.ceiling() == ceiling, "the ceiling is B less the reserve");

    counter.bytes = ceiling - 4 * mebibyte;
    require(pool.reserve(4 * mebibyte),
        "a reservation that ends at the ceiling is admitted");
    require(pool.usage() == ceiling, "the pool is at its ceiling");
    require(calls == 0, "a reservation that fits needs no reclaim");
    pool.release(4 * mebibyte);

    require(pool.reserve(4 * mebibyte - page),
        "a reservation just below the ceiling is admitted");
    pool.release(4 * mebibyte - page);
    require(!pool.reserve(4 * mebibyte + page),
        "a reservation that crosses the ceiling is refused");
    require(calls == 1, "the refusal ran the reclaim first");
    require(pool.refusals() == 1, "the refusal is counted");
    require(pool.usage() == ceiling - 4 * mebibyte,
        "the refusal charged nothing");

    /* What nobody reserves grows into the reserve, and the pool admits
     * nothing more until it is gone. */
    counter.bytes = limit - mebibyte;
    require(!pool.reserve(page), "a pool past its ceiling admits nothing");
    counter.bytes = ceiling - page;
    require(pool.reserve(page), "the room below the ceiling is admitted again");
    pool.release(page);

    /* A reserve that would leave nothing is not kept. */
    DeviceMemoryPool small(8 * page, [] { return std::uint64_t {0}; }, page,
        {}, 8 * page);
    require(small.ceiling() == 8 * page,
        "a reserve as large as the limit is not kept");
    require(small.reserve(8 * page), "such a pool admits up to its limit");
}

void growthLeavesTheRestToTheGuest()
{
    // A heap growth reserves and is made; a guest allocation of what the
    // pool has left is then admitted, and one page more is not.
    Counter counter;
    DeviceMemoryPool pool(256 * page, [&] { return counter.bytes.load(); },
        page);
    counter.bytes = 16 * page;
    require(pool.reserve(64 * page), "the heap's growth is admitted");
    counter.bytes += 64 * page;
    pool.release(64 * page);
    require(!pool.reserve(177 * page), "the guest cannot pass the limit");
    require(pool.reserve(176 * page), "the guest gets what is left");
}

void concurrentAllocationsNeverCrossTheLimit()
{
    /* Two allocations of 60 of 100 pages, which each fit alone and not
     * together, check the pool on two threads before either is made. The
     * counter shows neither, so only a reservation can tell each about the
     * other. The counter's first two reads each wait, for a fifth of a
     * second at most, for the other: a check that only reads the counter
     * lets both in, and the pool, which reads it under its lock, keeps the
     * second reader out until the first has reserved. */
    Counter counter;
    std::mutex readers_mutex;
    std::condition_variable readers_arrived;
    unsigned readers = 0;
    auto allocated = [&] {
        const std::uint64_t bytes = counter.bytes.load();
        std::unique_lock lock(readers_mutex);
        if (++readers <= 2) {
            readers_arrived.notify_all();
            readers_arrived.wait_for(lock, 200ms, [&] { return readers >= 2; });
        }
        return bytes;
    };
    DeviceMemoryPool pool(100 * page, allocated, page);
    std::atomic<unsigned> admitted {};
    auto allocate = [&] {
        if (!pool.reserve(60 * page))
            return;
        ++admitted;
        counter.bytes += 60 * page;
        pool.release(60 * page);
    };
    std::thread first(allocate);
    std::thread second(allocate);
    first.join();
    second.join();
    require(admitted == 1, "exactly one of two allocations that fit only "
                           "alone is admitted");
    require(counter.bytes <= pool.limit(), "together they stay within the "
                                           "limit");
}

void manyThreadsNeverCrossTheLimit()
{
    /* Eight threads allocate and free random sizes against a pool that holds
     * only a few of them at once. Each allocation appears in the counter
     * after its reservation, as Metal's does, and none may take the counter
     * past the limit. */
    Counter counter;
    DeviceMemoryPool pool(256 * page, [&] { return counter.bytes.load(); },
        page);
    std::atomic<std::uint64_t> highest {};
    std::atomic<unsigned> admitted {};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            std::minstd_rand random(t + 1);
            std::uniform_int_distribution<std::uint64_t> pages(1, 96);
            for (unsigned i = 0; i < 4000; ++i) {
                const std::uint64_t size = pages(random) * page;
                if (!pool.reserve(size))
                    continue;
                ++admitted;
                std::this_thread::yield();
                const std::uint64_t now = counter.bytes += size;
                std::uint64_t seen = highest.load();
                while (now > seen && !highest.compare_exchange_weak(seen, now))
                    ;
                pool.release(size);
                std::this_thread::yield();
                counter.bytes -= size;
            }
        });
    }
    for (std::thread& thread : threads)
        thread.join();
    require(admitted > 1000, "most allocations are admitted");
    require(highest <= pool.limit(), "no allocation passes the limit");
    require(pool.usage() == 0, "every reservation is returned");
}

void reclaimsBeforeRefusing()
{
    /* Half the pool is a cache the driver would free for the asking. A
     * reservation that does not fit asks it to, and is decided once more:
     * the hook's memory serves it. */
    Counter counter;
    DeviceMemoryPool pool(100 * page, [&] { return counter.bytes.load(); },
        page);
    counter.bytes = 70 * page;
    unsigned calls = 0;
    pool.setReclaim([&]() -> std::uint64_t {
        ++calls;
        counter.bytes = 20 * page;
        return 3;
    });
    require(pool.reserve(20 * page), "a reservation that fits needs no hook");
    require(calls == 0, "the hook is not called while reservations fit");
    pool.release(20 * page);
    require(pool.reserve(40 * page), "freed cache admits the reservation");
    require(calls == 1, "the hook ran once");
    require(pool.reclaimed() == 1, "the rescue is counted");
    require(pool.refusals() == 0, "nothing was refused");
    require(pool.usage() == 60 * page, "the reservation and the memory left");
    pool.release(40 * page);

    /* A cache that is gone leaves the next refusal to stand. */
    counter.bytes = 90 * page;
    pool.setReclaim([&]() -> std::uint64_t {
        ++calls;
        return 5;
    });
    require(!pool.reserve(20 * page), "released memory that is not enough "
                                      "still refuses");
    require(pool.refusals() == 1, "the refusal is counted");
    require(pool.refusalsAfterRelease() == 1,
        "a refusal that followed a release is counted apart");
    pool.setReclaim([&]() -> std::uint64_t {
        ++calls;
        return 0;
    });
    require(!pool.reserve(20 * page), "an empty cache refuses");
    require(pool.refusals() == 2 && pool.refusalsAfterRelease() == 1,
        "an empty release is not counted apart");

    /* A request no cache could serve does not ask. */
    const unsigned before = calls;
    require(!pool.reserve(101 * page), "a request past the limit is refused");
    require(calls == before, "the hook is not asked for the impossible");

    pool.setReclaim({});
    require(!pool.reserve(20 * page), "no hook, no rescue");
}

void hookRunsWithoutThePoolsLocks()
{
    /* The hook frees memory, which returns reservations to the pool and reads
     * its usage. Called under the pool's lock it would wait for itself; the
     * pool must also let a second thread's reservation proceed meanwhile. */
    Counter counter;
    DeviceMemoryPool pool(100 * page, [&] { return counter.bytes.load(); },
        page);
    counter.bytes = 80 * page;
    require(pool.reserve(10 * page), "a reservation to give back");
    std::atomic<bool> inside {};
    std::atomic<bool> proceed {};
    pool.setReclaim([&]() -> std::uint64_t {
        pool.release(10 * page);
        (void)pool.usage();
        inside = true;
        while (!proceed)
            std::this_thread::yield();
        counter.bytes = 0;
        return 1;
    });
    std::atomic<bool> rescued {};
    std::thread refused([&] { rescued = pool.reserve(50 * page); });
    while (!inside)
        std::this_thread::yield();
    /* The hook is running: the pool still answers a thread whose reservation
     * fits, without waiting for it. */
    counter.bytes = 0;
    require(pool.reserve(5 * page), "the pool answers while a hook runs");
    pool.release(5 * page);
    counter.bytes = 80 * page;
    proceed = true;
    refused.join();
    require(rescued, "the refused thread was admitted after the hook");
    pool.release(50 * page);
    pool.setReclaim({});
}

void setReclaimWaitsForARunningHook()
{
    Counter counter;
    DeviceMemoryPool pool(10 * page, [&] { return counter.bytes.load(); },
        page);
    counter.bytes = 10 * page;
    std::atomic<bool> inside {};
    std::atomic<bool> finished {};
    pool.setReclaim([&]() -> std::uint64_t {
        inside = true;
        std::this_thread::sleep_for(100ms);
        finished = true;
        return 0;
    });
    std::thread refused([&] { (void)pool.reserve(page); });
    while (!inside)
        std::this_thread::yield();
    pool.setReclaim({});
    require(finished, "clearing the hook waits for the call running it");
    refused.join();
}

void accountedBytesCountOnlyWhatTheSystemCounts()
{
    using uml::gpu::accountedGpuBytes;

    /* Metal counts a no-copy import at its full size; the memory the GPU
     * holds for the process is what remains, unless the kernel's graphics
     * ledger says more. Here the ledger holds only heaps. */
    require(accountedGpuBytes(100 * page, 0, 0, 0) == 100 * page,
        "with nothing imported the counter stands");
    require(accountedGpuBytes(100 * page, 30 * page, 0, 0) == 70 * page,
        "an import is left out of the counter");
    require(accountedGpuBytes(100 * page, 30 * page, 60 * page, 60 * page) ==
            70 * page,
        "the larger of the counter less imports and the ledger");
    require(accountedGpuBytes(100 * page, 30 * page, 80 * page, 80 * page) ==
            80 * page,
        "the ledger is a floor the imports cannot lower");
    require(accountedGpuBytes(100 * page, 100 * page, 5 * page, 5 * page) ==
            5 * page,
        "a counter that is all imports leaves the ledger");

    /* What the ledger holds beyond the resident heaps, the driver's command
     * storage, is memory the counter does not hold, and the counter's
     * IOSurfaces are memory the ledger does not: both count. */
    require(accountedGpuBytes(100 * page, 0, 80 * page, 60 * page) ==
            120 * page,
        "the IOSurfaces and the command storage both count");
    require(accountedGpuBytes(100 * page, 30 * page, 60 * page, 50 * page) ==
            80 * page,
        "the command storage adds to the counter less imports");
    require(accountedGpuBytes(100 * page, 0, 40 * page, 60 * page) ==
            100 * page,
        "heaps the ledger does not hold yet subtract nothing");
    require(accountedGpuBytes(10 * page, 0, 50 * page, 40 * page) ==
            50 * page,
        "never less than the ledger");

    /* The readings are taken at different instants: an import larger than
     * the counter reads as nothing made, not as a wrapped number. */
    require(accountedGpuBytes(10 * page, 30 * page, 0, 0) == 0,
        "an import past the counter does not wrap");
    require(accountedGpuBytes(10 * page, 30 * page, 4 * page, 4 * page) ==
            4 * page,
        "and leaves the ledger");
    constexpr std::uint64_t most = std::numeric_limits<std::uint64_t>::max();
    require(accountedGpuBytes(most, 0, 0, 0) == most, "the largest counter");
    require(accountedGpuBytes(0, most, 0, 0) == 0, "the largest import");
    require(accountedGpuBytes(most, 0, most, 0) == most,
        "a sum past the largest number saturates");
}

/* Metal's counter falls the moment memory is freed, and the graphics ledger
 * beside it follows later; the pool counts the larger of the two, and reads
 * Metal's counter alone as its prompt reading. */
struct Accounts {
    Counter metal;
    Counter ledger;
    [[nodiscard]] std::uint64_t allocated() const
    {
        // Every byte of the ledger here is a heap the counter holds too.
        const std::uint64_t footprint = ledger.bytes.load();
        return uml::gpu::accountedGpuBytes(
            metal.bytes.load(), 0, footprint, footprint);
    }
    [[nodiscard]] std::uint64_t prompt() const
    {
        return uml::gpu::madeGpuBytes(metal.bytes.load(), 0);
    }
};

/* A pool over the accounts, which have read the same bytes once so that the
 * pool has a reading to compare the next with. */
DeviceMemoryPool makePool(Accounts& accounts)
{
    return DeviceMemoryPool(
        100 * page, [&accounts] { return accounts.allocated(); }, page,
        [&accounts] { return accounts.prompt(); });
}

void takeAReading(DeviceMemoryPool& pool)
{
    require(pool.reserve(page), "a page is admitted");
    pool.release(page);
}

void waitsForALedgerThatLagsARelease()
{
    /* The hook frees 80 of 90 pages: Metal's counter shows it at once, and
     * the ledger stands still for 60 ms, as it does until its sweep, and then
     * follows. A reservation of 40 pages fits once the ledger follows, and
     * the pool waits for it instead of refusing. */
    Accounts accounts;
    accounts.metal.bytes = 90 * page;
    accounts.ledger.bytes = 90 * page;
    DeviceMemoryPool pool = makePool(accounts);
    std::thread ledger;
    pool.setReclaim([&]() -> std::uint64_t {
        accounts.metal.bytes = 10 * page;
        ledger = std::thread([&] {
            std::this_thread::sleep_for(60ms);
            accounts.ledger.bytes = 10 * page;
        });
        return 4;
    });
    require(pool.reserve(40 * page),
        "a reservation that fits once the ledger follows is admitted");
    ledger.join();
    require(pool.refusals() == 0, "nothing was refused");
    require(pool.reclaimed() == 1, "the rescue is counted");
    require(pool.settleWaits() == 1 && pool.settleAdmissions() == 1,
        "the wait is counted");
    require(pool.settleWaitedMicroseconds() >= 50000,
        "the pool waited for the still ledger");
    require(pool.usage() == 50 * page, "the reservation and the memory left");
    pool.release(40 * page);

    /* A ledger that already followed needs no wait. */
    pool.setReclaim({});
    require(pool.reserve(40 * page), "a settled ledger admits at once");
    require(pool.settleWaits() == 1, "no wait was needed");
    pool.release(40 * page);
}

void aReleaseThatHappenedBeforeTheRefusalIsWaitedFor()
{
    /* No hook: the guest freed the memory, and the pool sees Metal's counter
     * lower than at its last reading when it decides. */
    Accounts accounts;
    accounts.metal.bytes = 90 * page;
    accounts.ledger.bytes = 90 * page;
    DeviceMemoryPool pool = makePool(accounts);
    takeAReading(pool);
    accounts.metal.bytes = 10 * page;
    std::thread ledger([&] {
        std::this_thread::sleep_for(20ms);
        accounts.ledger.bytes = 10 * page;
    });
    require(pool.reserve(40 * page), "a release the pool did not cause");
    ledger.join();
    require(pool.settleWaits() == 1 && pool.settleAdmissions() == 1,
        "the pool waited for it");
    pool.release(40 * page);
}

void aFullBudgetIsRefusedAtOnce()
{
    /* Nothing was released, so there is nothing to wait for: neither Metal's
     * counter nor the ledger has room. */
    Accounts accounts;
    accounts.metal.bytes = 90 * page;
    accounts.ledger.bytes = 90 * page;
    DeviceMemoryPool pool = makePool(accounts);
    pool.setReclaim([]() -> std::uint64_t { return 2; });
    require(!pool.reserve(20 * page), "a full budget refuses");
    require(pool.refusals() == 1 && pool.refusalsAfterRelease() == 1,
        "the refusal is counted");
    require(pool.settleWaits() == 0, "the pool did not wait");

    /* A request past the limit is no ledger's lag. */
    require(!pool.reserve(101 * page), "a request past the limit is refused");
    require(pool.settleWaits() == 0, "the impossible is not waited for");

    /* A release that does not make room for the request is not waited for
     * either: 20 pages freed leave 70 held, and 40 more would pass the limit
     * however long the ledger takes. */
    accounts.metal.bytes = 70 * page;
    require(!pool.reserve(40 * page), "a release too small refuses");
    require(pool.settleWaits() == 0, "it is refused at once");
}

void aMarginalRequestIsNotWaitedForTwice()
{
    /* Metal's counter shows 10 pages and the ledger 90: the extras the counter
     * omits keep it above for good. With no release the request that Metal's
     * figure would admit is refused at once. */
    Accounts accounts;
    accounts.metal.bytes = 10 * page;
    accounts.ledger.bytes = 90 * page;
    DeviceMemoryPool pool = makePool(accounts);
    require(!pool.reserve(40 * page), "the ledger holds");
    require(pool.settleWaits() == 0, "no release, no wait");

    /* A release the ledger never repays is waited for once: the wait runs out
     * its bound, and the next refusals are not waited for. */
    accounts.metal.bytes = 60 * page;
    takeAReading(pool);
    accounts.metal.bytes = 10 * page;
    const auto begun = std::chrono::steady_clock::now();
    require(!pool.reserve(40 * page), "a ledger that holds refuses");
    require(std::chrono::steady_clock::now() - begun >=
            DeviceMemoryPool::settleLimit,
        "the pool waited out its bound");
    require(pool.settleWaits() == 1 && pool.settleAdmissions() == 0,
        "the wait was counted and did not admit");
    require(pool.usage() == 90 * page, "a refusal charges nothing");

    require(!pool.reserve(40 * page), "the same request is refused again");
    require(!pool.reserve(30 * page), "so is another that fits Metal's figure");
    require(pool.settleWaits() == 1, "a wait that gave up is not repeated");
    require(pool.reserve(5 * page), "what the ledger admits is admitted");
    pool.release(5 * page);

    /* The next release is waited for again, and the ledger repays it. */
    accounts.metal.bytes = 60 * page;
    takeAReading(pool);
    accounts.metal.bytes = 10 * page;
    std::thread ledger([&] {
        std::this_thread::sleep_for(10ms);
        accounts.ledger.bytes = 10 * page;
    });
    require(pool.reserve(40 * page), "a fresh release is waited for");
    ledger.join();
    require(pool.settleWaits() == 2 && pool.settleAdmissions() == 1,
        "the second wait admitted");
    pool.release(40 * page);
}

void aLedgerThatFallsTooSlowlyEndsAtTheBound()
{
    /* The ledger keeps falling, a page every 5 ms, and would take longer than
     * the bound to show the room for 80 pages. */
    Accounts accounts;
    accounts.metal.bytes = 90 * page;
    accounts.ledger.bytes = 90 * page;
    DeviceMemoryPool pool = makePool(accounts);
    takeAReading(pool);
    accounts.metal.bytes = 10 * page;
    std::atomic<bool> stop {};
    std::thread ledger([&] {
        while (!stop && accounts.ledger.bytes > 10 * page) {
            std::this_thread::sleep_for(5ms);
            accounts.ledger.bytes -= page;
        }
    });
    const auto begun = std::chrono::steady_clock::now();
    const bool admitted = pool.reserve(80 * page);
    const auto waited = std::chrono::steady_clock::now() - begun;
    stop = true;
    ledger.join();
    require(!admitted, "a ledger that is too slow refuses");
    require(waited >= DeviceMemoryPool::settleLimit,
        "the pool waited out its bound");
    require(pool.settleWaits() == 1 && pool.settleAdmissions() == 0,
        "the wait was counted and did not admit");
}

void aWaitEndsWhenAnotherTakesTheRoom()
{
    /* The pool waits for the ledger to repay 80 pages and for room for 50.
     * Another consumer then takes 50 pages of Metal's counter, so the room is
     * gone however the ledger falls, and the wait ends without running out
     * its bound. */
    Accounts accounts;
    accounts.metal.bytes = 90 * page;
    accounts.ledger.bytes = 90 * page;
    DeviceMemoryPool pool = makePool(accounts);
    takeAReading(pool);
    accounts.metal.bytes = 10 * page;
    std::thread other([&] {
        std::this_thread::sleep_for(20ms);
        accounts.metal.bytes = 60 * page;
    });
    const auto begun = std::chrono::steady_clock::now();
    const bool admitted = pool.reserve(50 * page);
    const auto waited = std::chrono::steady_clock::now() - begun;
    other.join();
    require(!admitted, "the room was taken");
    require(waited < DeviceMemoryPool::settleLimit,
        "the wait ended when the room was gone");
    require(pool.settleWaits() == 1 && pool.settleAdmissions() == 0,
        "the pool waited while the room could still come");
}

void theWaitHoldsNoLock()
{
    /* While one thread waits for the ledger, another reads the usage and
     * returns a reservation: the wait holds no lock of the pool. The ledger
     * keeps falling, so the wait lasts about 30 ms. */
    Accounts accounts;
    accounts.metal.bytes = 80 * page;
    accounts.ledger.bytes = 80 * page;
    DeviceMemoryPool pool = makePool(accounts);
    require(pool.reserve(5 * page), "a reservation to give back");
    accounts.metal.bytes = 10 * page;
    std::atomic<bool> admitted {};
    std::atomic<bool> stop {};
    std::thread waiter([&] { admitted = pool.reserve(50 * page); });
    while (pool.settleWaits() == 0)
        std::this_thread::yield();
    std::thread ledger([&] {
        while (!stop && accounts.ledger.bytes > 10 * page) {
            std::this_thread::sleep_for(1ms);
            accounts.ledger.bytes -= page;
        }
    });
    std::thread other([&] {
        (void)pool.usage();
        pool.release(5 * page);
    });
    other.join();
    require(!admitted, "the waiter is still waiting when the other is done");
    waiter.join();
    stop = true;
    ledger.join();
    require(admitted, "the waiter is admitted once the ledger follows");
}

void importsDoNotFillThePool()
{
    /* A guest's host-visible allocations are shared-memory segments the
     * driver imports, and Metal counts every one of them. The pool charges
     * what is made for the GPU: 50 of the counter's 90 pages are an import,
     * so a reservation of the 60 pages left fits where one counting the
     * import would refuse it. */
    Counter metal;
    Counter imported;
    Counter ledger;
    auto accounted = [&] {
        const std::uint64_t lent = imported.bytes.load();
        const std::uint64_t footprint = ledger.bytes.load();
        return uml::gpu::accountedGpuBytes(
            metal.bytes.load(), lent, footprint, footprint);
    };
    DeviceMemoryPool pool(100 * page, accounted, page);
    metal.bytes = 90 * page;
    imported.bytes = 50 * page;
    require(pool.usage() == 40 * page, "the import is not charged");
    require(pool.reserve(60 * page), "the pool holds what is left of it");
    require(pool.refusals() == 0, "nothing was refused");
    metal.bytes += 60 * page;
    pool.release(60 * page);
    require(pool.usage() == 100 * page, "the made allocation replaces its "
                                        "reservation");
    require(!pool.reserve(page), "a pool full of made memory refuses");

    /* An import that is freed leaves the pool as it was. */
    metal.bytes -= 50 * page;
    imported.bytes = 0;
    require(pool.usage() == 100 * page, "freeing an import changes nothing "
                                        "that was charged");

    /* The ledger holds what the counter omits. */
    ledger.bytes = 95 * page;
    require(pool.usage() == 100 * page, "the larger estimate stands");
    metal.bytes = 10 * page;
    require(pool.usage() == 95 * page, "the ledger is a floor");
}

} // namespace

int main()
{
    accountedBytesCountOnlyWhatTheSystemCounts();
    importsDoNotFillThePool();
    admitsWhatFits();
    refusesAtTheCeiling();
    growthLeavesTheRestToTheGuest();
    concurrentAllocationsNeverCrossTheLimit();
    manyThreadsNeverCrossTheLimit();
    reclaimsBeforeRefusing();
    hookRunsWithoutThePoolsLocks();
    setReclaimWaitsForARunningHook();
    waitsForALedgerThatLagsARelease();
    aReleaseThatHappenedBeforeTheRefusalIsWaitedFor();
    aFullBudgetIsRefusedAtOnce();
    aMarginalRequestIsNotWaitedForTwice();
    aLedgerThatFallsTooSlowlyEndsAtTheBound();
    aWaitEndsWhenAnotherTakesTheRoom();
    theWaitHoldsNoLock();
    std::puts("device memory pool: allocations that reserve first never "
              "pass the limit together");
    return 0;
}
