// SPDX-License-Identifier: GPL-2.0-or-later

#include "device_memory_pool.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <limits>
#include <thread>
#include <utility>

namespace uml::gpu {

DeviceMemoryPool::DeviceMemoryPool(std::uint64_t limit,
    std::function<std::uint64_t()> allocated,
    std::uint64_t granule,
    std::function<std::uint64_t()> prompt,
    std::uint64_t reserve)
    : limit_(limit)
    , ceiling_(reserve < limit ? limit - reserve : limit)
    , allocated_(std::move(allocated))
    , prompt_(std::move(prompt))
    , granule_(std::max<std::uint64_t>(granule, 1))
{
}

std::uint64_t DeviceMemoryPool::charge(std::uint64_t bytes) const noexcept
{
    const std::uint64_t remainder = bytes % granule_;
    if (remainder == 0)
        return bytes;
    const std::uint64_t padding = granule_ - remainder;
    return bytes > std::numeric_limits<std::uint64_t>::max() - padding
        ? std::numeric_limits<std::uint64_t>::max()
        : bytes + padding;
}

std::uint64_t DeviceMemoryPool::usageLocked() const
{
    /* Read under the lock, so that a reservation released after `allocated`
     * came to count its allocation is counted once or twice, never not at
     * all. */
    const std::uint64_t allocated = allocated_();
    return allocated > std::numeric_limits<std::uint64_t>::max() - reserved_
        ? std::numeric_limits<std::uint64_t>::max()
        : allocated + reserved_;
}

std::uint64_t DeviceMemoryPool::usage() const
{
    std::lock_guard lock(mutex_);
    return usageLocked();
}

std::uint64_t DeviceMemoryPool::sampleLocked(
    std::uint64_t& allocated, std::uint64_t& prompt)
{
    allocated = allocated_();
    prompt = prompt_ ? prompt_() : 0;
    if (prompt_) {
        if (sampled_) {
            if (prompt < lastPrompt_)
                owed_ += lastPrompt_ - prompt;
            else
                owed_ -= std::min(owed_, prompt - lastPrompt_);
            if (allocated < lastAllocated_)
                owed_ -= std::min(owed_, lastAllocated_ - allocated);
        }
        sampled_ = true;
        lastPrompt_ = prompt;
        lastAllocated_ = allocated;
    }
    return allocated > std::numeric_limits<std::uint64_t>::max() - reserved_
        ? std::numeric_limits<std::uint64_t>::max()
        : allocated + reserved_;
}

bool DeviceMemoryPool::tryReserve(std::uint64_t charged, std::uint64_t& used)
{
    std::lock_guard lock(mutex_);
    std::uint64_t allocated = 0;
    std::uint64_t prompt = 0;
    used = sampleLocked(allocated, prompt);
    if (charged > ceiling_ || used > ceiling_ - charged)
        return false;
    reserved_ += charged;
    return true;
}

bool DeviceMemoryPool::settleAndReserve(
    std::uint64_t charged, std::uint64_t& used)
{
    using Clock = std::chrono::steady_clock;
    const Clock::time_point begun = Clock::now();
    const Clock::time_point deadline = begun + settleLimit;
    bool waited = false;
    bool admitted = false;
    for (;;) {
        {
            /* Read under the lock, as the usage is: a reservation released
             * meanwhile is counted once or twice, never not at all. The lock
             * is not held while the loop sleeps. */
            std::lock_guard lock(mutex_);
            std::uint64_t allocated = 0;
            std::uint64_t prompt = 0;
            used = sampleLocked(allocated, prompt);
            if (charged > ceiling_ || reserved_ > ceiling_ - charged)
                break;
            if (used <= ceiling_ - charged) {
                reserved_ += charged;
                admitted = true;
                break;
            }
            /* What `allocated` will be once the ledger has paid what it owes,
             * and never less than the prompt reading. */
            const std::uint64_t owed = std::min(
                owed_, allocated > prompt ? allocated - prompt : 0);
            if (allocated - owed > ceiling_ - charged - reserved_)
                break;
            if (Clock::now() >= deadline) {
                owed_ = 0;
                break;
            }
        }
        if (!waited) {
            waited = true;
            settleWaits_.fetch_add(1, std::memory_order_relaxed);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (waited) {
        if (admitted)
            settleAdmissions_.fetch_add(1, std::memory_order_relaxed);
        settleMicroseconds_.fetch_add(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    Clock::now() - begun)
                    .count()),
            std::memory_order_relaxed);
    }
    return admitted;
}

void DeviceMemoryPool::setReclaim(Reclaim reclaim)
{
    std::lock_guard lock(reclaimMutex_);
    reclaim_ = std::move(reclaim);
}

std::uint64_t DeviceMemoryPool::reclaim()
{
    std::lock_guard lock(reclaimMutex_);
    return reclaim_ ? reclaim_() : 0;
}

bool DeviceMemoryPool::reserve(std::uint64_t bytes)
{
    const std::uint64_t charged = charge(bytes);
    std::uint64_t used = 0;
    if (tryReserve(charged, used))
        return true;

    /* A request larger than the pool is no cache's doing. Otherwise free what
     * the host drivers cache, with no lock of the pool held: freeing returns
     * reservations to it, and the memory the drivers free lowers the usage
     * that the decision below reads again. */
    std::uint64_t released = 0;
    if (charged <= ceiling_) {
        std::lock_guard lock(reclaimMutex_);
        if (reclaim_) {
            released = reclaim_();
            if (tryReserve(charged, used)) {
                if (released != 0)
                    reclaimed_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
    }

    /* What was freed has left Metal's counter at once and the ledger a
     * moment later: wait for it, with no lock of the pool or the hook held. */
    if (prompt_ && charged <= ceiling_ && settleAndReserve(charged, used)) {
        if (released != 0)
            reclaimed_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    const bool first = refusals_.fetch_add(1, std::memory_order_relaxed) == 0;
    if (released != 0)
        refusalsAfterRelease_.fetch_add(1, std::memory_order_relaxed);
    if (first)
        std::fprintf(stderr,
            "uml: the GPU budget is full: it refused %llu bytes with "
            "%llu of %llu bytes in use, %llu of the %llu-byte budget kept in "
            "reserve%s\n",
            static_cast<unsigned long long>(charged),
            static_cast<unsigned long long>(used),
            static_cast<unsigned long long>(ceiling_),
            static_cast<unsigned long long>(limit_ - ceiling_),
            static_cast<unsigned long long>(limit_),
            released != 0 ? " after releasing cached memory" : "");
    return false;
}

void DeviceMemoryPool::release(std::uint64_t bytes) noexcept
{
    const std::uint64_t charged = charge(bytes);
    std::lock_guard lock(mutex_);
    reserved_ -= std::min(charged, reserved_);
}

} // namespace uml::gpu
