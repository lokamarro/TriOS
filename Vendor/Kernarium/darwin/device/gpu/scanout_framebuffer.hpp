// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "protocol.hpp"

#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

#include <os/lock.h>

namespace uml::gpu {

struct ScanoutRectangle {
    std::uint32_t x {};
    std::uint32_t y {};
    std::uint32_t width {};
    std::uint32_t height {};
};

class FramebufferMutex final {
public:
    void lock() noexcept { os_unfair_lock_lock(&lock_); }
    void unlock() noexcept { os_unfair_lock_unlock(&lock_); }

private:
    os_unfair_lock lock_ = OS_UNFAIR_LOCK_INIT;
};

/* When a framebuffer holds pixel storage. The dimensions are always valid;
 * only the pixels come and go. */
enum class FramebufferStorage : std::uint8_t {
    /* None until the first write, which allocates it zeroed. resize() and
     * clear() release it again, so a framebuffer without storage is black. */
    lazy,
    /* Zeroed from construction on and cleared in place, for a reader that
     * keeps data() across calls, as LibVNCServer does. */
    retained,
};

class ScanoutFramebuffer final {
public:
    ScanoutFramebuffer(
        std::uint32_t width,
        std::uint32_t height,
        FramebufferStorage storage = FramebufferStorage::lazy);

    ScanoutFramebuffer(const ScanoutFramebuffer&) = delete;
    ScanoutFramebuffer& operator=(const ScanoutFramebuffer&) = delete;

    [[nodiscard]] std::uint32_t width() const noexcept;
    [[nodiscard]] std::uint32_t height() const noexcept;
    [[nodiscard]] std::size_t rowBytes() const noexcept;
    /* These three read the storage, so they need the framebuffer locked
     * unless nothing else can reach it. Without storage the pixels are an
     * empty span and data() is null. */
    [[nodiscard]] bool hasStorage() const noexcept;
    [[nodiscard]] std::span<const std::uint8_t> pixels() const noexcept;
    [[nodiscard]] std::uint8_t* data() noexcept;
    void lock() noexcept;
    void unlock() noexcept;

    void resize(std::uint32_t width, std::uint32_t height);
    void clear() noexcept;
    void clearLocked() noexcept;
    [[nodiscard]] ScanoutRectangle update(
        std::span<const std::uint8_t> source,
        std::uint32_t sourceWidth,
        std::uint32_t sourceHeight,
        std::size_t sourceRowBytes,
        protocol::GpuFormat sourceFormat,
        std::uint32_t sourceX,
        std::uint32_t sourceY,
        std::uint32_t destinationX,
        std::uint32_t destinationY,
        std::uint32_t width,
        std::uint32_t height);
    void fillSelfTestPattern();
    void fillSelfTestPatternLocked();

private:
    void ensureStorageLocked(std::uint64_t dimensions);

    std::atomic_uint64_t dimensions_;
    FramebufferStorage storage_;
    std::vector<std::uint8_t> pixels_;
    FramebufferMutex mutex_;
};

} // namespace uml::gpu
