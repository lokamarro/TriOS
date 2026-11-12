// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "shared_memory_regions.hpp"
#include "virtio_video.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace uml::video {

/* One contiguous piece of a guest buffer. `address` is a guest-physical
 * address, or an offset in shared-memory region `region` when the guest marked
 * the entry shared. */
struct GuestSegment {
    std::uint64_t address;
    std::uint32_t length;
    bool shared {};
    std::uint8_t region {};
};

/* The planes of one guest buffer, each a list of segments. */
using GuestPlanes = std::vector<std::vector<GuestSegment>>;

using GuestMapper = std::function<std::span<std::uint8_t>(std::uint64_t, std::size_t)>;

/* The planes a RESOURCE_ATTACH command describes after its fixed fields:
 * `planeCount` scatter lists, each a count and its entries. Every entry is
 * checked against guest memory, or against the published shared-memory
 * regions for an entry the guest marked shared. Throws std::invalid_argument
 * for anything malformed, out of range or left over. */
[[nodiscard]] GuestPlanes parseScatterLists(
    std::span<const std::uint8_t> payload, unsigned planeCount,
    const GuestMapper& mapper, const uml::virtio::SharedMemoryRegions* regions);

/* Copies `data.size()` bytes between `data` and the plane, starting `skip`
 * bytes into it: from the plane when `write` is false, into it when true.
 * A shared-memory range is resolved again for every transfer: the backend
 * that owns the region withdraws it from the index before the guest unmaps
 * it, so a buffer whose blob has gone completes with an error instead of
 * touching memory the guest no longer has. Returns false, having copied
 * part of the range at most, when the plane is shorter than the range or a
 * segment cannot be reached. */
[[nodiscard]] bool transferSegments(
    const GuestMapper& mapper, const uml::virtio::SharedMemoryRegions* regions,
    std::span<const GuestSegment> segments, std::span<std::uint8_t> data, bool write,
    std::uint64_t skip = 0);

/* Reads one plane of a guest buffer front to back in pieces, such as the rows
 * of a picture, without searching the whole segment list for each piece. A
 * read that starts before the previous one began finds its segment again from
 * the start. */
class GuestPlaneReader final {
public:
    GuestPlaneReader(const GuestMapper& mapper, const uml::virtio::SharedMemoryRegions* regions,
                     std::span<const GuestSegment> segments) noexcept
        : mapper_(mapper), regions_(regions), segments_(segments)
    {
    }
    /* Fills `out` from `offset` bytes into the plane; false when the plane is
     * shorter than that or a segment cannot be reached. */
    [[nodiscard]] bool read(std::uint64_t offset, std::span<std::uint8_t> out);

private:
    const GuestMapper& mapper_;
    const uml::virtio::SharedMemoryRegions* regions_;
    std::span<const GuestSegment> segments_;
    std::size_t index_ {};
    std::uint64_t indexStart_ {};
};

} // namespace uml::video
