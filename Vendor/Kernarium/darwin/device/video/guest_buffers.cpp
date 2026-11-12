// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_buffers.hpp"
#include "vhost_user_device.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace uml::video {
namespace {
constexpr std::size_t maximumResource = 128 * 1024 * 1024;

template<class T> T load(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < sizeof(T)) throw std::invalid_argument("short video command");
    return uml::virtio::fromBytes<T>(bytes);
}
}

GuestPlanes parseScatterLists(std::span<const std::uint8_t> payload, unsigned planeCount,
    const GuestMapper& mapper, const uml::virtio::SharedMemoryRegions* regions)
{
    GuestPlanes planes;
    std::size_t total = 0;
    for (unsigned plane = 0; plane < planeCount; ++plane) {
        if (payload.size() < 8) throw std::invalid_argument("short video scatter list");
        const auto count = load<std::uint32_t>(payload);
        payload = payload.subspan(8);
        if (!count || count > 32768 || count > payload.size() / sizeof(virtio_video_resource_sg_entry))
            throw std::invalid_argument("invalid video scatter list length");
        planes.emplace_back();
        for (unsigned i = 0; i < count; ++i) {
            const auto entry = load<virtio_video_resource_sg_entry>(payload);
            payload = payload.subspan(sizeof(entry));
            if ((entry.flags & ~VIRTIO_VIDEO_RESOURCE_SG_FLAG_MASK) != 0)
                throw std::invalid_argument("unknown video scatter entry flags");
            const bool shared = (entry.flags & VIRTIO_VIDEO_RESOURCE_SG_FLAG_SHM_REGION) != 0;
            const auto region = static_cast<std::uint8_t>(
                (entry.flags & VIRTIO_VIDEO_RESOURCE_SG_SHM_ID_MASK) >>
                VIRTIO_VIDEO_RESOURCE_SG_SHM_ID_SHIFT);
            if (!entry.length || entry.length > maximumResource - total ||
                (!shared && region != 0))
                throw std::invalid_argument("invalid video scatter entry");
            if (shared) {
                if (!regions || !regions->contains(region, entry.addr, entry.length))
                    throw std::invalid_argument(
                        "video resource is not in a published shared-memory region");
            } else if (!mapper || mapper(entry.addr, entry.length).size() != entry.length) {
                throw std::invalid_argument("video resource lies outside guest memory");
            }
            total += entry.length;
            planes.back().push_back({entry.addr, entry.length, shared, region});
        }
    }
    if (!payload.empty()) throw std::invalid_argument("trailing video resource data");
    return planes;
}

namespace {
/* Moves `data` between itself and the part of `segment` that starts `start`
 * bytes into it. */
bool transferOne(const GuestMapper& mapper, const uml::virtio::SharedMemoryRegions* regions,
                 const GuestSegment& segment, std::uint64_t start, std::span<std::uint8_t> data,
                 bool write)
{
    const auto size = data.size();
    if (segment.shared) {
        bool copied = false;
        const auto copy = [&](std::span<std::uint8_t> memory) {
            if (memory.size() != size) return;
            if (write) std::memcpy(memory.data(), data.data(), size);
            else std::memcpy(data.data(), memory.data(), size);
            copied = true;
        };
        return regions && regions->resolve(segment.region, segment.address + start, size, copy) && copied;
    }
    auto memory = mapper ? mapper(segment.address + start, size) : std::span<std::uint8_t>{};
    if (memory.size() != size) return false;
    if (write) std::memcpy(memory.data(), data.data(), size);
    else std::memcpy(data.data(), memory.data(), size);
    return true;
}
}

bool transferSegments(const GuestMapper& mapper, const uml::virtio::SharedMemoryRegions* regions,
    std::span<const GuestSegment> segments, std::span<std::uint8_t> data, bool write,
    std::uint64_t skip)
{
    std::size_t offset = 0;
    for (const auto segment : segments) {
        std::uint64_t start = 0;
        if (skip != 0) {
            if (skip >= segment.length) { skip -= segment.length; continue; }
            start = skip;
            skip = 0;
        }
        const auto size = std::min<std::size_t>(data.size() - offset, segment.length - start);
        if (!size) break;
        if (!transferOne(mapper, regions, segment, start, data.subspan(offset, size), write))
            return false;
        offset += size;
    }
    return offset == data.size();
}

bool GuestPlaneReader::read(std::uint64_t offset, std::span<std::uint8_t> out)
{
    if (offset < indexStart_) index_ = 0, indexStart_ = 0;
    while (index_ < segments_.size() && offset >= indexStart_ + segments_[index_].length) {
        indexStart_ += segments_[index_].length;
        ++index_;
    }
    std::size_t done = 0;
    std::uint64_t start = offset - indexStart_;
    for (std::size_t index = index_; done < out.size() && index < segments_.size(); ++index) {
        const auto& segment = segments_[index];
        const auto size = std::min<std::size_t>(out.size() - done, segment.length - start);
        if (!transferOne(mapper_, regions_, segment, start, out.subspan(done, size), false))
            return false;
        done += size;
        start = 0;
    }
    return done == out.size();
}

} // namespace uml::video
