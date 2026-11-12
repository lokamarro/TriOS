// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>

namespace uml::gpu {

/* The device memory the host GPU holds for the process, as the pool counts
 * it, from four readings.
 *
 * `metalAllocated` is Metal's allocation counter, which counts every heap
 * from its creation, a no-copy buffer at its full size whatever backs it, and
 * the IOSurfaces behind presentation and decoding. `imported` is the part of
 * that the process only lends the GPU, host memory a device maps by its
 * address, such as the shared-memory segments behind a guest's host-visible
 * allocations: on iOS they are container files, outside the footprint the
 * system limits the process by, so they must not fill a budget that exists to
 * keep the footprint under its limit. `graphicsFootprint` is the kernel's
 * graphics ledger, part of what jetsam counts. It holds the driver's own
 * command storage, which Metal's counter omits, and not the IOSurfaces, and it
 * counts a heap's pages only once they are resident, so a consumer makes a
 * heap resident before it releases the heap's reservation (KosmicKrisp does).
 * `residentHeaps` is what both hold: the heaps KosmicKrisp made resident.
 *
 * The usage is the counter less imports, plus what the ledger holds beyond
 * the resident heaps, so the IOSurfaces and the command storage both count;
 * it is never less than either estimate alone. A heap enters `residentHeaps`
 * only once the ledger holds it and leaves before Metal drops it, so reading
 * `residentHeaps` last can count a heap twice for a moment but never leave it
 * out; a heap made between the readings is still reserved. Other memory that
 * both hold, the host's own Metal textures and buffers, counts twice. An
 * import larger than the counter (the two are read at different instants)
 * reads as nothing made.
 *
 * Read `imported` before `metalAllocated`: an import that lands between the
 * two readings is then counted a moment too long and never too short.
 * `madeGpuBytes` is the first estimate alone, the one that follows a release
 * at once; the pool uses it as its prompt reading. */
[[nodiscard]] constexpr std::uint64_t madeGpuBytes(
    std::uint64_t metalAllocated, std::uint64_t imported) noexcept
{
    return metalAllocated > imported ? metalAllocated - imported : 0;
}

[[nodiscard]] constexpr std::uint64_t accountedGpuBytes(
    std::uint64_t metalAllocated,
    std::uint64_t imported,
    std::uint64_t graphicsFootprint,
    std::uint64_t residentHeaps) noexcept
{
    const std::uint64_t made = madeGpuBytes(metalAllocated, imported);
    const std::uint64_t larger =
        made > graphicsFootprint ? made : graphicsFootprint;
    const std::uint64_t ledgerOnly = graphicsFootprint > residentHeaps
        ? graphicsFootprint - residentHeaps
        : 0;
    const std::uint64_t both = made > ~std::uint64_t {0} - ledgerOnly
        ? ~std::uint64_t {0}
        : made + ledgerOnly;
    return both > larger ? both : larger;
}

/* The device memory the host grants a Venus guest, as one pool that every
 * consumer charges before it allocates: the renderer for each allocation the
 * guest asks for, and the host GPU driver for memory it allocates for itself,
 * such as KosmicKrisp's geometry heap.
 *
 * Its usage is what the host GPU holds for the process, which `allocated`
 * reports, plus the reservations of allocations under way that `allocated`
 * does not count yet. A reservation is admitted only when the usage and it
 * fit the limit, decided under the pool's lock, and its consumer releases it
 * once `allocated` counts the allocation or the allocation has failed. So
 * allocations that each fit alone never together take the process past the
 * limit, whichever threads make them. Reservations are charged in whole
 * granules, the unit in which `allocated` counts an allocation.
 *
 * A pool given a `reserve` admits reservations only up to its ceiling, the
 * limit less the reserve, and keeps the rest for what it counts but never
 * refuses: memory nobody reserves, such as the driver's command storage, grows
 * past the last reservation into that room rather than past the limit. A
 * reserve that would leave no ceiling is not kept.
 *
 * Memory the host driver caches for reuse counts against the pool, and the
 * driver releases it only when a client of that driver is refused, never for
 * another consumer's refusal. So the pool takes one reclaim hook, which frees
 * what the host drivers cache and reports how many cached allocations it
 * freed, and calls it before it refuses: a reservation that does not fit
 * runs the hook and is decided once more.
 *
 * What `allocated` reports does not all follow a release at once. Metal's
 * counter drops the moment a heap is freed, and the graphics ledger beside it
 * follows later: within milliseconds, or only at the ledger's next sweep,
 * a quarter of a second at most. A request that fits once everything is freed
 * is therefore refused by the ledger's share for that while. A pool given a
 * `prompt` reading, the part of `allocated` that does follow at once, sees
 * the release: at every decision it compares the prompt reading with the one
 * before, and what it fell by is what the ledger still owes, less what
 * `allocated` has fallen by since and what the prompt reading has risen by.
 * When a request is refused and would fit once the ledger had paid what it
 * owes, the pool waits for `allocated` to fall rather than refuse, until it
 * admits the request, the ledger has paid and it still does not fit, or the
 * bound below passes, and a wait that reached its bound forgets what the
 * ledger owed. A request that would not fit even then, or that follows no
 * release, is refused at once: command storage that the ledger holds for
 * good costs no wait. */
class DeviceMemoryPool final {
public:
    /* Frees what the host drivers cache for reuse, on whichever thread was
     * refused or called reclaim(), and returns the number of cached
     * allocations freed. It runs with none of the pool's locks held, so it may
     * release reservations and read the usage, and at most one runs at a
     * time. The refused thread may hold locks of the host driver that the hook
     * must not take. */
    using Reclaim = std::function<std::uint64_t()>;

    /* The longest the pool waits for `allocated` to follow a release: the
     * ledger's sweep and a margin. */
    static constexpr std::chrono::milliseconds settleLimit {300};

    /* `prompt` is optional; without it a refusal is decided as soon as the
     * hook has run. Both readings are taken under the pool's lock. */
    DeviceMemoryPool(std::uint64_t limit,
        std::function<std::uint64_t()> allocated,
        std::uint64_t granule,
        std::function<std::uint64_t()> prompt = {},
        std::uint64_t reserve = 0);

    DeviceMemoryPool(const DeviceMemoryPool&) = delete;
    DeviceMemoryPool& operator=(const DeviceMemoryPool&) = delete;

    [[nodiscard]] std::uint64_t limit() const noexcept { return limit_; }

    /* The usage reservations are admitted up to: the limit less the reserve. */
    [[nodiscard]] std::uint64_t ceiling() const noexcept { return ceiling_; }

    /* The bytes allocated and reserved now. */
    [[nodiscard]] std::uint64_t usage() const;

    /* How many reservations the pool has refused. The first is reported on
     * stderr, so a run says where its budget ended. */
    [[nodiscard]] std::uint64_t refusals() const noexcept
    {
        return refusals_.load(std::memory_order_relaxed);
    }

    /* How many reservations were refused although the reclaim hook had just
     * freed something: the refusals that cached memory did not explain. */
    [[nodiscard]] std::uint64_t refusalsAfterRelease() const noexcept
    {
        return refusalsAfterRelease_.load(std::memory_order_relaxed);
    }

    /* How many reservations did not fit until the reclaim hook freed
     * something, and then did. */
    [[nodiscard]] std::uint64_t reclaimed() const noexcept
    {
        return reclaimed_.load(std::memory_order_relaxed);
    }

    /* How many times the pool waited for `allocated` to follow a release,
     * how many of those waits admitted the reservation, and the microseconds
     * spent waiting in all. */
    [[nodiscard]] std::uint64_t settleWaits() const noexcept
    {
        return settleWaits_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t settleAdmissions() const noexcept
    {
        return settleAdmissions_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t settleWaitedMicroseconds() const noexcept
    {
        return settleMicroseconds_.load(std::memory_order_relaxed);
    }

    /* Sets the reclaim hook, or clears it with an empty function. Returns
     * once no call of the previous one is running. */
    void setReclaim(Reclaim reclaim);

    /* Charges `bytes` to the pool, or returns false and charges nothing when
     * the pool cannot hold them. A reservation that does not fit runs the
     * reclaim hook, if there is one, and is decided once more. */
    [[nodiscard]] bool reserve(std::uint64_t bytes);

    /* Runs the reclaim hook as a refused reservation does, for a caller that
     * wants the memory the drivers cache freed before any refusal, and returns
     * what the hook freed; nothing without a hook. */
    [[nodiscard]] std::uint64_t reclaim();

    /* Returns a reservation of `bytes`. */
    void release(std::uint64_t bytes) noexcept;

private:
    [[nodiscard]] std::uint64_t charge(std::uint64_t bytes) const noexcept;
    [[nodiscard]] std::uint64_t usageLocked() const;
    /* Admits `charged` when it fits; `used` is the usage it was decided at. */
    [[nodiscard]] bool tryReserve(std::uint64_t charged, std::uint64_t& used);
    /* Reads `allocated` and the prompt reading, credits what the prompt
     * reading fell by since the last reading to what the ledger owes, and
     * returns the usage. */
    [[nodiscard]] std::uint64_t sampleLocked(
        std::uint64_t& allocated, std::uint64_t& prompt);
    /* Waits for `allocated` to follow a release the prompt reading showed, and
     * admits `charged` once it does. False when the request would not fit
     * even then, when none is owed, or at the bound. */
    [[nodiscard]] bool settleAndReserve(
        std::uint64_t charged, std::uint64_t& used);

    const std::uint64_t limit_;
    const std::uint64_t ceiling_;
    const std::function<std::uint64_t()> allocated_;
    const std::function<std::uint64_t()> prompt_;
    const std::uint64_t granule_;
    mutable std::mutex mutex_;
    std::uint64_t reserved_ {};
    std::atomic<std::uint64_t> refusals_ {};
    std::atomic<std::uint64_t> refusalsAfterRelease_ {};
    std::atomic<std::uint64_t> reclaimed_ {};
    std::atomic<std::uint64_t> settleWaits_ {};
    std::atomic<std::uint64_t> settleAdmissions_ {};
    std::atomic<std::uint64_t> settleMicroseconds_ {};
    /* Under mutex_: the bytes the ledger still owes a release the prompt
     * reading showed, and the readings that were last taken. */
    std::uint64_t owed_ {};
    std::uint64_t lastPrompt_ {};
    std::uint64_t lastAllocated_ {};
    bool sampled_ {};
    /* Held while the hook runs, which keeps it to one at a time and lets
     * setReclaim wait for a running one. Never held with mutex_. */
    std::mutex reclaimMutex_;
    Reclaim reclaim_;
};

} // namespace uml::gpu
