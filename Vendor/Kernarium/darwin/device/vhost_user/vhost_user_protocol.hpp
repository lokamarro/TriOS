// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

/* The vhost-user wire protocol and split-virtqueue layout that every
 * vhost-user device shares. */
namespace uml::gpu::protocol {

constexpr std::uint32_t vhostUserVersion = 1;
constexpr std::uint32_t vhostUserVersionMask = 0x3U;
constexpr std::uint32_t vhostUserReply = 1U << 2U;
constexpr std::uint32_t vhostUserNeedReply = 1U << 3U;

enum class VhostRequest : std::uint32_t {
    getFeatures = 1,
    setFeatures = 2,
    setOwner = 3,
    resetOwner = 4,
    setMemTable = 5,
    setVringNum = 8,
    setVringAddr = 9,
    setVringBase = 10,
    getVringBase = 11,
    setVringKick = 12,
    setVringCall = 13,
    getProtocolFeatures = 15,
    setProtocolFeatures = 16,
    getQueueNum = 17,
    setVringEnable = 18,
    setSlaveRequestFd = 21,
    getConfig = 24,
    setConfig = 25,
    // In-band queue notification enabled by PROTOCOL_F_INBAND_NOTIFICATIONS.
    vringKick = 35,
    getSharedMemoryConfig = 44,
};

enum class VhostSlaveRequest : std::uint32_t {
    configChange = 2,
    vringCall = 4,
    sharedMemoryMap = 9,
    sharedMemoryUnmap = 10,
};

constexpr std::uint64_t vhostFeatureProtocol = 1ULL << 30U;
constexpr std::uint64_t virtioFeatureVersion1 = 1ULL << 32U;
constexpr std::uint64_t protocolFeatureMultipleQueues = 1ULL << 0U;
constexpr std::uint64_t protocolFeatureReplyAck = 1ULL << 3U;
constexpr std::uint64_t protocolFeatureSlaveRequest = 1ULL << 5U;
constexpr std::uint64_t protocolFeatureConfig = 1ULL << 9U;
constexpr std::uint64_t protocolFeatureInbandNotifications = 1ULL << 14U;
constexpr std::uint64_t protocolFeatureSharedMemoryMap = 1ULL << 22U;

struct [[gnu::packed]] VhostHeader {
    std::uint32_t request;
    std::uint32_t flags;
    std::uint32_t size;
};

struct [[gnu::packed]] VringState {
    std::uint32_t index;
    std::uint32_t value;
};

struct [[gnu::packed]] VringAddress {
    std::uint32_t index;
    std::uint32_t flags;
    std::uint64_t descriptor;
    std::uint64_t used;
    std::uint64_t available;
    std::uint64_t log;
};

struct [[gnu::packed]] MemoryRegion {
    std::uint64_t guestAddress;
    std::uint64_t size;
    std::uint64_t userAddress;
    std::uint64_t mmapOffset;
};

struct [[gnu::packed]] MemoryTableHeader {
    std::uint32_t regionCount;
    std::uint32_t padding;
};

struct [[gnu::packed]] ConfigHeader {
    std::uint32_t offset;
    std::uint32_t size;
    std::uint32_t flags;
};

constexpr std::size_t maximumSharedMemoryRegions = 256;

struct [[gnu::packed]] SharedMemoryConfig {
    std::uint32_t regionCount;
    std::uint32_t padding;
    std::array<std::uint64_t, maximumSharedMemoryRegions> regionSizes;
};

struct [[gnu::packed]] SharedMemoryMap {
    std::uint8_t regionId;
    std::array<std::uint8_t, 7> padding;
    std::uint64_t descriptorOffset;
    std::uint64_t regionOffset;
    std::uint64_t length;
    std::uint64_t flags;
};

// A shared-memory map's offsets and length are whole host pages, the
// granularity at which the front-end maps them.
constexpr std::uint64_t sharedMemoryMapAlignment = 16ULL * 1024ULL;
constexpr std::uint64_t sharedMemoryMapReadOnly = 0;
constexpr std::uint64_t sharedMemoryMapReadWrite = 1;

struct [[gnu::packed]] VirtqueueDescriptor {
    std::uint64_t address;
    std::uint32_t length;
    std::uint16_t flags;
    std::uint16_t next;
};

struct [[gnu::packed]] VirtqueueUsedElement {
    std::uint32_t id;
    std::uint32_t length;
};

constexpr std::uint16_t descriptorNext = 1U;
constexpr std::uint16_t descriptorWrite = 2U;
constexpr std::uint16_t descriptorIndirect = 4U;
constexpr std::uint16_t availableSuppressInterrupt = 1U;
constexpr std::uint16_t usedSuppressNotification = 1U;
// Vhost-user encodes the queue index and "no descriptor" flag in one u64.
constexpr std::uint64_t vhostVringIndexMask = 0xffU;
constexpr std::uint64_t vhostVringNoDescriptor = 1U << 8U;
constexpr std::uint64_t vhostVringDescriptorMask =
    vhostVringIndexMask | vhostVringNoDescriptor;
constexpr std::uint32_t configFlagWritable = 0;
constexpr std::uint32_t configFlagMigration = 1;
constexpr std::size_t splitRingHeaderSize =
    2U * sizeof(std::uint16_t); // flags followed by the producer index.
constexpr std::uint32_t maximumVirtqueueSize = 256;

} // namespace uml::gpu::protocol
