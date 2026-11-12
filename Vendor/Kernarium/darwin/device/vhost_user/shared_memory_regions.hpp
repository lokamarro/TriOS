// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../../memory/shared_backing.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <vector>

namespace uml::virtio {

/* The vhost-user shared-memory ranges a backend has published to the guest,
 * and the host memory behind each of them.
 *
 * A guest buffer that lives in such a region has no guest-physical address,
 * so a device asked to fill one resolves the region and offset here instead
 * of through the guest memory table. A range leaves this index before its
 * owner revokes it from the guest, so memory handed out while the index holds
 * it is never a revoked mapping.
 */
class SharedMemoryRegions final {
public:
    SharedMemoryRegions() = default;
    SharedMemoryRegions(const SharedMemoryRegions&) = delete;
    SharedMemoryRegions& operator=(const SharedMemoryRegions&) = delete;

    /* Records the host memory of a range the owning backend is publishing.
     * Returns false when the descriptor cannot be mapped or the range
     * overlaps one already published. */
    [[nodiscard]] bool publish(
        std::uint8_t regionId,
        std::uint64_t regionOffset,
        std::uint64_t length,
        int descriptor,
        std::uint64_t descriptorOffset);
    void revoke(
        std::uint8_t regionId,
        std::uint64_t regionOffset,
        std::uint64_t length) noexcept;
    void revokeAll() noexcept;

    /* Calls `visit` with the host memory for the range while it stays
     * published, and answers whether it did. The span is borrowed for the
     * call only. */
    bool resolve(
        std::uint8_t regionId,
        std::uint64_t regionOffset,
        std::size_t length,
        const std::function<void(std::span<std::uint8_t>)>& visit) const;
    [[nodiscard]] bool contains(
        std::uint8_t regionId,
        std::uint64_t regionOffset,
        std::size_t length) const;

private:
    struct Range final {
        std::uint8_t regionId {};
        std::uint64_t offset {};
        std::uint64_t length {};
        uml::memory::SharedBackingView backing;
    };

    [[nodiscard]] const Range* find(
        std::uint8_t regionId,
        std::uint64_t regionOffset,
        std::size_t length) const noexcept;

    mutable std::mutex mutex_;
    std::vector<Range> ranges_;
};

} // namespace uml::virtio
