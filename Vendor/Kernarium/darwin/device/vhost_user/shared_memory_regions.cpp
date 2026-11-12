// SPDX-License-Identifier: GPL-2.0-or-later
#include "shared_memory_regions.hpp"

#include <algorithm>
#include <limits>

#include <sys/mman.h>

namespace uml::virtio {

bool SharedMemoryRegions::publish(
    std::uint8_t regionId,
    std::uint64_t regionOffset,
    std::uint64_t length,
    int descriptor,
    std::uint64_t descriptorOffset)
{
    if (length == 0 || descriptor < 0 ||
        length > std::numeric_limits<std::size_t>::max() ||
        regionOffset > std::numeric_limits<std::uint64_t>::max() - length) {
        return false;
    }
    auto backing = uml::memory::acquire_shared_backing_view(
        descriptor,
        descriptorOffset,
        static_cast<std::size_t>(length),
        PROT_READ | PROT_WRITE);
    if (!backing)
        return false;

    const std::lock_guard guard {mutex_};
    const bool overlaps = std::any_of(
        ranges_.begin(), ranges_.end(), [&](const Range& range) {
            return range.regionId == regionId &&
                regionOffset < range.offset + range.length &&
                range.offset < regionOffset + length;
        });
    if (overlaps)
        return false;
    try {
        ranges_.push_back(Range {
            .regionId = regionId,
            .offset = regionOffset,
            .length = length,
            .backing = std::move(backing),
        });
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

void SharedMemoryRegions::revoke(
    std::uint8_t regionId,
    std::uint64_t regionOffset,
    std::uint64_t length) noexcept
{
    const std::lock_guard guard {mutex_};
    const auto removed = std::remove_if(
        ranges_.begin(), ranges_.end(), [&](const Range& range) {
            return range.regionId == regionId &&
                range.offset == regionOffset && range.length == length;
        });
    ranges_.erase(removed, ranges_.end());
}

void SharedMemoryRegions::revokeAll() noexcept
{
    const std::lock_guard guard {mutex_};
    ranges_.clear();
}

const SharedMemoryRegions::Range* SharedMemoryRegions::find(
    std::uint8_t regionId,
    std::uint64_t regionOffset,
    std::size_t length) const noexcept
{
    if (length == 0 ||
        regionOffset > std::numeric_limits<std::uint64_t>::max() - length) {
        return nullptr;
    }
    for (const auto& range : ranges_) {
        if (range.regionId != regionId || regionOffset < range.offset)
            continue;
        const std::uint64_t offset = regionOffset - range.offset;
        if (offset < range.length && length <= range.length - offset &&
            range.backing.size() >= offset + length) {
            return &range;
        }
    }
    return nullptr;
}

bool SharedMemoryRegions::resolve(
    std::uint8_t regionId,
    std::uint64_t regionOffset,
    std::size_t length,
    const std::function<void(std::span<std::uint8_t>)>& visit) const
{
    const std::lock_guard guard {mutex_};
    const auto* range = find(regionId, regionOffset, length);
    if (range == nullptr || !visit)
        return false;
    auto* data = static_cast<std::uint8_t*>(range->backing.data());
    visit(std::span {data + (regionOffset - range->offset), length});
    return true;
}

bool SharedMemoryRegions::contains(
    std::uint8_t regionId,
    std::uint64_t regionOffset,
    std::size_t length) const
{
    const std::lock_guard guard {mutex_};
    return find(regionId, regionOffset, length) != nullptr;
}

} // namespace uml::virtio
