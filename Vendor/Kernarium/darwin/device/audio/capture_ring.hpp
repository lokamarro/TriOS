// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>

namespace uml::audio {

/* A bounded single-producer, single-consumer ring of bytes, for a capture
 * source whose audio arrives on a thread of its own. Neither side allocates
 * or locks. The producer calls write() only; the consumer calls read(),
 * discard() and clear() only. A full ring drops the bytes the producer
 * offers beyond its room, and the consumer discards the oldest bytes it does
 * not want, so a source that runs ahead of the guest bounds its latency
 * instead of growing it. */
class CaptureRing final {
public:
    explicit CaptureRing(std::size_t capacityBytes)
        : capacity_(std::bit_ceil(std::max<std::size_t>(capacityBytes, 2)))
        , bytes_(std::make_unique<std::uint8_t[]>(capacity_))
    {
    }

    /* Returns how many of the bytes it took; the rest did not fit. */
    std::size_t write(std::span<const std::uint8_t> data) noexcept
    {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto tail = tail_.load(std::memory_order_acquire);
        const auto count = std::min(data.size(), capacity_ - (head - tail));
        copyIn(head, data.first(count));
        head_.store(head + count, std::memory_order_release);
        return count;
    }

    /* Moves up to out.size() of the oldest bytes into `out`. */
    std::size_t read(std::span<std::uint8_t> out) noexcept
    {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto head = head_.load(std::memory_order_acquire);
        const auto count = std::min(out.size(), head - tail);
        copyOut(tail, out.first(count));
        tail_.store(tail + count, std::memory_order_release);
        return count;
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return head_.load(std::memory_order_acquire) -
            tail_.load(std::memory_order_acquire);
    }

    /* Drops the `count` oldest bytes, or all that are held. */
    void discard(std::size_t count) noexcept
    {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto head = head_.load(std::memory_order_acquire);
        tail_.store(tail + std::min(count, head - tail),
                    std::memory_order_release);
    }

    void clear() noexcept { discard(capacity_); }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    void copyIn(std::size_t position, std::span<const std::uint8_t> data) noexcept
    {
        const auto start = position & (capacity_ - 1U);
        const auto first = std::min(data.size(), capacity_ - start);
        std::memcpy(bytes_.get() + start, data.data(), first);
        std::memcpy(bytes_.get(), data.data() + first, data.size() - first);
    }

    void copyOut(std::size_t position, std::span<std::uint8_t> out) const noexcept
    {
        const auto start = position & (capacity_ - 1U);
        const auto first = std::min(out.size(), capacity_ - start);
        std::memcpy(out.data(), bytes_.get() + start, first);
        std::memcpy(out.data() + first, bytes_.get(), out.size() - first);
    }

    const std::size_t capacity_;
    std::unique_ptr<std::uint8_t[]> bytes_;
    // Free-running positions; their difference is the fill.
    std::atomic<std::size_t> head_ {};
    std::atomic<std::size_t> tail_ {};
};

} // namespace uml::audio
