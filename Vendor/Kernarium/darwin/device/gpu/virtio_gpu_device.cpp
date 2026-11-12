// SPDX-License-Identifier: GPL-2.0-or-later

#include "virtio_gpu_device.hpp"

#include "display_activity_log.hpp"
#include "virtio_gpu_uml.h"

#if UML_GPU_DEVICE_TESTING
#include "virtio_gpu_device_testing.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

namespace uml::gpu {
namespace {

[[nodiscard]] constexpr std::uint64_t alignHostVisibleSize(
    std::uint64_t size) noexcept
{
    constexpr auto alignment = protocol::hostVisibleSharedMemoryAlignment;
    return (size + alignment - 1U) & ~(alignment - 1U);
}

[[nodiscard]] bool headerPaddingIsZero(
    const protocol::GpuControlHeader& header) noexcept
{
    return std::all_of(
        header.padding.begin(), header.padding.end(),
        [](std::uint8_t value) { return value == 0; });
}

[[nodiscard]] bool validVenusFenceHeader(
    const protocol::GpuControlHeader& header) noexcept
{
    constexpr auto validFlags =
        protocol::gpuFlagFence | protocol::gpuFlagInfoRingIndex;
    const bool fenced = (header.flags & protocol::gpuFlagFence) != 0;
    const bool namesRing =
        (header.flags & protocol::gpuFlagInfoRingIndex) != 0;
    return (header.flags & ~validFlags) == 0 &&
        (!namesRing || fenced) &&
        (namesRing
            ? header.ringIndex < protocol::venusContextRingCount
            : header.ringIndex == 0) &&
        headerPaddingIsZero(header);
}

/* The software renderer of the iOS Simulator has no Vulkan driver, so the
 * device offers no Venus capset there and refuses a guest that asks for one. */
#if UML_GPU_DEVICE_TESTING
#define UML_GPU_RENDERER dependencies_
#define UML_GPU_DISPLAY dependencies_
#define UML_GPU_VENUS_OFFERED dependencies_.offersVenus()
#else
#define UML_GPU_RENDERER renderer_
#define UML_GPU_DISPLAY display_
#define UML_GPU_VENUS_OFFERED (UML_GPU_SOFTWARE_RENDERER == 0)
#endif
/* What the guest's contexts hold of the host GPU's memory (the display keeps
 * it, since the display outlives this connection). */
#define UML_GPU_LEDGER UML_GPU_DISPLAY.graphicsLedger()
/* The device memory budget and the pool's usage, while a renderer with a
 * budget runs. */
#if UML_GPU_DEVICE_TESTING
#define UML_GPU_DEVICE_MEMORY dependencies_.deviceMemory()
#define UML_GPU_RECLAIM_MEMORY dependencies_.reclaimCachedMemory()
#else
#define UML_GPU_DEVICE_MEMORY VirglRenderer::deviceMemory()
#define UML_GPU_RECLAIM_MEMORY VirglRenderer::reclaimCachedMemory()
#endif

/* The memory report's wire is the kernel's header; the host's own names for
 * its codes must agree. */
static_assert(protocol::virtioGpuFeatureUmlMemoryReport ==
    1ULL << VIRTIO_GPU_F_UML_MEMORY_REPORT);
static_assert(protocol::gpuEventMemory == VIRTIO_GPU_EVENT_UML_MEMORY);
static_assert(static_cast<std::uint32_t>(protocol::GpuCommand::umlGetMemory) ==
    VIRTIO_GPU_CMD_UML_GET_MEMORY);
static_assert(static_cast<std::uint32_t>(protocol::GpuResponse::okUmlMemory) ==
    VIRTIO_GPU_RESP_OK_UML_MEMORY);
static_assert(sizeof(virtio_gpu_ctrl_hdr) == sizeof(protocol::GpuControlHeader));
static_assert(sizeof(virtio_gpu_uml_get_memory) == 48);
static_assert(sizeof(virtio_gpu_uml_memory) == 72);
static_assert(sizeof(virtio_gpu_uml_memory_holder) == 40);
static_assert(VIRTIO_GPU_UML_COMM_LEN == 16);

/* The size of the memory reply with no holder, and with every holder. */
constexpr std::size_t memoryReplyFixedSize = sizeof(virtio_gpu_uml_memory);
constexpr std::size_t memoryReplyMaximumSize = memoryReplyFixedSize +
    VIRTIO_GPU_UML_MAX_HOLDERS * sizeof(virtio_gpu_uml_memory_holder);

/* A resource's room in the ledger, given back unless the command commits. */
class LedgerReservation {
public:
    LedgerReservation(ContextMemoryLedger& ledger, std::uint32_t resourceId)
        : ledger_(ledger), resourceId_(resourceId)
    {
        ledger_.reserveResource(resourceId_);
    }
    LedgerReservation(const LedgerReservation&) = delete;
    LedgerReservation& operator=(const LedgerReservation&) = delete;
    ~LedgerReservation()
    {
        if (!committed_)
            ledger_.resourceReleased(resourceId_);
    }
    void commit() noexcept { committed_ = true; }

private:
    ContextMemoryLedger& ledger_;
    std::uint32_t resourceId_;
    bool committed_ {};
};

/* A blob scanout the renderer answered with a native Metal texture is backed
 * by a host allocation; the descriptor the guest exported for it names that
 * allocation and never carries its pixels. Reading it back therefore
 * publishes zeros, which is indistinguishable from a black frame the guest
 * drew, so say once that this happened. */
void reportNativeScanoutFallback() noexcept
{
    static std::atomic_bool reported;
    if (!reported.exchange(true, std::memory_order_relaxed)) {
        std::fputs(
            "Virtio-GPU: a native scanout fell back to CPU readback; its "
            "blob names a host allocation and holds no pixels\n",
            stderr);
    }
}

[[nodiscard]] const char* presentOutcomeName(PresentOutcome outcome) noexcept
{
    switch (outcome) {
    case PresentOutcome::unsupported:
        return "unsupported";
    case PresentOutcome::presented:
        return "presented";
    case PresentOutcome::dropped:
        return "dropped";
    }
    return "unknown";
}

template<typename Structure>
const Structure* readStructure(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < sizeof(Structure))
        return nullptr;
    return reinterpret_cast<const Structure*>(bytes.data());
}

/* The size of a control command's successful reply. GET_CAPSET's reply also
 * carries the capability set after its header. */
[[nodiscard]] constexpr std::size_t replySize(
    protocol::GpuCommand command) noexcept
{
    switch (command) {
    case protocol::GpuCommand::getDisplayInfo:
        return sizeof(protocol::GpuDisplayInfo);
    case protocol::GpuCommand::getEdid:
        return sizeof(protocol::GpuEdid);
    case protocol::GpuCommand::getCapsetInfo:
        return sizeof(protocol::GpuCapsetInfo);
    case protocol::GpuCommand::resourceMapBlob:
        return sizeof(protocol::GpuMapInfo);
    case protocol::GpuCommand::umlGetMemory:
        return memoryReplyFixedSize;
    default:
        return sizeof(protocol::GpuControlHeader);
    }
}

#if !UML_GPU_SOFTWARE_RENDERER
/* Two formats that differ only in whether the fourth byte is alpha or
 * padding store their colour channels identically. The native texture lookup
 * that compares them has no software-renderer counterpart. */
[[nodiscard]] std::uint32_t formatLayout(std::uint32_t format) noexcept
{
    switch (static_cast<protocol::GpuFormat>(format)) {
    case protocol::GpuFormat::b8g8r8x8Unorm:
        return static_cast<std::uint32_t>(protocol::GpuFormat::b8g8r8a8Unorm);
    case protocol::GpuFormat::x8r8g8b8Unorm:
        return static_cast<std::uint32_t>(protocol::GpuFormat::a8r8g8b8Unorm);
    case protocol::GpuFormat::r8g8b8x8Unorm:
        return static_cast<std::uint32_t>(protocol::GpuFormat::r8g8b8a8Unorm);
    case protocol::GpuFormat::x8b8g8r8Unorm:
        return static_cast<std::uint32_t>(protocol::GpuFormat::a8b8g8r8Unorm);
    default:
        return format;
    }
}
#endif

bool supportedFormat(std::uint32_t format)
{
    switch (static_cast<protocol::GpuFormat>(format)) {
    case protocol::GpuFormat::b8g8r8a8Unorm:
    case protocol::GpuFormat::b8g8r8x8Unorm:
    case protocol::GpuFormat::a8r8g8b8Unorm:
    case protocol::GpuFormat::x8r8g8b8Unorm:
    case protocol::GpuFormat::r8g8b8a8Unorm:
    case protocol::GpuFormat::x8b8g8r8Unorm:
    case protocol::GpuFormat::a8b8g8r8Unorm:
    case protocol::GpuFormat::r8g8b8x8Unorm:
        return true;
    default:
        return false;
    }
}

bool backingRangeFits(std::uint64_t address, std::uint32_t length)
{
    return length != 0 &&
        address <= std::numeric_limits<std::uint64_t>::max() - length;
}

bool rectangleFits(
    const protocol::GpuRectangle& rectangle,
    std::uint32_t width,
    std::uint32_t height)
{
    return rectangle.width != 0 && rectangle.height != 0 &&
        rectangle.x <= width && rectangle.y <= height &&
        rectangle.width <= width - rectangle.x &&
        rectangle.height <= height - rectangle.y;
}

bool linearImageRangeFits(
    std::uint64_t size,
    std::uint32_t offset,
    std::uint32_t stride,
    std::uint32_t width,
    std::uint32_t height)
{
    if (width == 0 || height == 0)
        return false;
    const auto rowBytes =
        static_cast<std::uint64_t>(width) * protocol::gpuBytesPerPixel;
    if (stride < rowBytes || offset > size)
        return false;
    const auto remaining = size - offset;
    const auto precedingRows = static_cast<std::uint64_t>(height - 1U);
    return precedingRows <=
            (std::numeric_limits<std::uint64_t>::max() - rowBytes) / stride &&
        precedingRows * stride + rowBytes <= remaining;
}

std::optional<std::size_t> pixelBufferSize(
    std::uint32_t width,
    std::uint32_t height)
{
    if (width == 0 || height == 0)
        return std::nullopt;
    const auto convertedWidth = static_cast<std::size_t>(width);
    if (height >
        std::numeric_limits<std::size_t>::max() / convertedWidth) {
        return std::nullopt;
    }
    const auto pixels = convertedWidth * static_cast<std::size_t>(height);
    if (pixels >
        std::numeric_limits<std::size_t>::max() /
            protocol::gpuBytesPerPixel) {
        return std::nullopt;
    }
    return pixels * protocol::gpuBytesPerPixel;
}

std::optional<protocol::GpuRectangle> intersectRectangles(
    const protocol::GpuRectangle& first,
    const protocol::GpuRectangle& second)
{
    const auto left = std::max(first.x, second.x);
    const auto top = std::max(first.y, second.y);
    const auto right = std::min(
        static_cast<std::uint64_t>(first.x) + first.width,
        static_cast<std::uint64_t>(second.x) + second.width);
    const auto bottom = std::min(
        static_cast<std::uint64_t>(first.y) + first.height,
        static_cast<std::uint64_t>(second.y) + second.height);
    if (right <= left || bottom <= top)
        return std::nullopt;
    return protocol::GpuRectangle {
        .x = left,
        .y = top,
        .width = static_cast<std::uint32_t>(right - left),
        .height = static_cast<std::uint32_t>(bottom - top),
    };
}

std::uint16_t physicalMillimeters(
    std::uint32_t pixels,
    std::uint32_t dotsPerInch)
{
    const auto numerator =
        static_cast<std::uint64_t>(pixels) * 254U +
        static_cast<std::uint64_t>(dotsPerInch) * 5U;
    const auto millimeters = numerator /
        (static_cast<std::uint64_t>(dotsPerInch) * 10U);
    return static_cast<std::uint16_t>(std::clamp<std::uint64_t>(
        millimeters, 1U, protocol::maximumEdidDimension));
}

void writeMonitorDescriptor(
    std::array<std::uint8_t, protocol::baseEdidSize>& edid,
    std::size_t offset,
    std::uint8_t tag,
    std::string_view text = {})
{
    std::fill_n(edid.begin() + offset, 18, 0);
    edid[offset + 3U] = tag;
    if (text.empty())
        return;
    std::fill_n(edid.begin() + offset + 5U, 13, ' ');
    const auto length = std::min<std::size_t>(text.size(), 12U);
    std::copy_n(text.begin(), length, edid.begin() + offset + 5U);
    edid[offset + 5U + length] = '\n';
}

std::array<std::uint8_t, protocol::baseEdidSize> makeEdid(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t dotsPerInch)
{
    std::array<std::uint8_t, protocol::baseEdidSize> edid {};
    constexpr std::array<std::uint8_t, 8> header {
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    std::copy(header.begin(), header.end(), edid.begin());
    // Big-endian EISA manufacturer ID "UML" and a little-endian product ID.
    edid[8] = 0x55;
    edid[9] = 0xac;
    edid[10] = 0x01;
    edid[16] = 1;
    edid[17] = 36; // 2026, relative to EDID's 1990 epoch.
    edid[18] = 1;
    edid[19] = 4;
    edid[20] = 0x80; // Digital display input.

    const auto widthMillimeters =
        physicalMillimeters(width, dotsPerInch);
    const auto heightMillimeters =
        physicalMillimeters(height, dotsPerInch);
    edid[21] = static_cast<std::uint8_t>(std::clamp<unsigned>(
        (widthMillimeters + 5U) / 10U, 1U, 255U));
    edid[22] = static_cast<std::uint8_t>(std::clamp<unsigned>(
        (heightMillimeters + 5U) / 10U, 1U, 255U));
    edid[23] = 120; // Gamma 2.2.
    edid[24] = 0x06; // sRGB and the first detailed timing is preferred.
    constexpr std::array<std::uint8_t, 10> srgbChromaticity {
        0xee, 0x91, 0xa3, 0x54, 0x4c,
        0x99, 0x26, 0x0f, 0x50, 0x54};
    std::copy(
        srgbChromaticity.begin(), srgbChromaticity.end(),
        edid.begin() + 25U);
    std::fill(edid.begin() + 38U, edid.begin() + 54U, 0x01);

    const auto horizontalBlank = std::min<std::uint32_t>(
        protocol::maximumEdidDimension,
        std::max<std::uint32_t>(160U, width / 5U));
    const auto verticalBlank = std::min<std::uint32_t>(
        protocol::maximumEdidDimension,
        std::max<std::uint32_t>(30U, height / 20U));
    const auto horizontalSyncOffset = std::min<std::uint32_t>(
        1023U, std::max<std::uint32_t>(8U, horizontalBlank / 3U));
    const auto horizontalSyncPulse = std::min<std::uint32_t>(
        1023U, std::max<std::uint32_t>(8U, horizontalBlank / 3U));
    constexpr std::uint32_t verticalSyncOffset = 3;
    constexpr std::uint32_t verticalSyncPulse = 5;
    const auto clock = std::clamp<std::uint64_t>(
        ((static_cast<std::uint64_t>(width) + horizontalBlank) *
             (static_cast<std::uint64_t>(height) + verticalBlank) * 60U +
            5000U) /
            10000U,
        1U,
        65535U);

    constexpr std::size_t timing = 54;
    edid[timing] = static_cast<std::uint8_t>(clock);
    edid[timing + 1U] = static_cast<std::uint8_t>(clock >> 8U);
    edid[timing + 2U] = static_cast<std::uint8_t>(width);
    edid[timing + 3U] = static_cast<std::uint8_t>(horizontalBlank);
    edid[timing + 4U] = static_cast<std::uint8_t>(
        ((width >> 8U) << 4U) | (horizontalBlank >> 8U));
    edid[timing + 5U] = static_cast<std::uint8_t>(height);
    edid[timing + 6U] = static_cast<std::uint8_t>(verticalBlank);
    edid[timing + 7U] = static_cast<std::uint8_t>(
        ((height >> 8U) << 4U) | (verticalBlank >> 8U));
    edid[timing + 8U] = static_cast<std::uint8_t>(horizontalSyncOffset);
    edid[timing + 9U] = static_cast<std::uint8_t>(horizontalSyncPulse);
    edid[timing + 10U] = static_cast<std::uint8_t>(
        (verticalSyncOffset << 4U) | verticalSyncPulse);
    edid[timing + 11U] = static_cast<std::uint8_t>(
        ((horizontalSyncOffset >> 8U) << 6U) |
        ((horizontalSyncPulse >> 8U) << 4U));
    edid[timing + 12U] = static_cast<std::uint8_t>(widthMillimeters);
    edid[timing + 13U] = static_cast<std::uint8_t>(heightMillimeters);
    edid[timing + 14U] = static_cast<std::uint8_t>(
        ((widthMillimeters >> 8U) << 4U) |
        (heightMillimeters >> 8U));
    edid[timing + 17U] = 0x1e; // Non-interlaced separate positive sync.

    writeMonitorDescriptor(edid, 72, 0xfc, "Kernarium");
    writeMonitorDescriptor(edid, 90, 0x10);
    writeMonitorDescriptor(edid, 108, 0x10);
    edid[126] = 0;
    unsigned checksum = 0;
    for (std::size_t index = 0; index < 127U; ++index)
        checksum += edid[index];
    edid[127] = static_cast<std::uint8_t>(0U - checksum);
    return edid;
}

} // namespace

#if UML_GPU_DEVICE_TESTING
VirtioGpuDevice::VirtioGpuDevice(
    testing::VirtioGpuDeviceDependencies& dependencies,
    GuestReader guestReader,
    GuestMapper guestMapper,
    std::uint32_t dotsPerInch)
    : dependencies_(dependencies)
    , guestReader_(std::move(guestReader))
    , guestMapper_(std::move(guestMapper))
    , dotsPerInch_(dotsPerInch)
{
    sharedMemoryActions_.reserve(protocol::maximumHostVisibleBlobResources);
    blobMappingOwners_.resize(protocol::maximumHostVisibleBlobResources);
}
#else
VirtioGpuDevice::VirtioGpuDevice(
    Display& display,
    GuestReader guestReader,
    GuestMapper guestMapper,
    std::uint32_t dotsPerInch)
    : display_(display)
    , guestReader_(std::move(guestReader))
    , guestMapper_(std::move(guestMapper))
    , dotsPerInch_(dotsPerInch)
{
    sharedMemoryActions_.reserve(protocol::maximumHostVisibleBlobResources);
    blobMappingOwners_.resize(protocol::maximumHostVisibleBlobResources);
    /* A Venus guest's allocation is decided on the render server's thread,
     * long after the guest's command left this device, so the renderer
     * reports a refusal to the display itself. It stops when the renderer
     * goes, with this device. */
    VirglRenderer::setMemoryRefusalObserver(
        [](void* display) noexcept {
            static_cast<Display*>(display)->recordMemoryRefusal();
        },
        &display_);
    /* The same thread reports the device memory a Venus context holds, and
     * the display's ledger credits it to the context. */
    VirglRenderer::setMemoryAccountObserver(
        [](void* ledger, std::uint32_t contextId,
            std::int64_t delta) noexcept {
            static_cast<ContextMemoryLedger*>(ledger)->venusMemory(
                contextId, delta);
        },
        &display_.graphicsLedger());
}
#endif

VirtioGpuDevice::~VirtioGpuDevice()
{
    reset();
}

std::span<std::uint32_t> VirtioGpuDevice::blobMappingPages(
    std::uint64_t offset, std::uint64_t size) noexcept
{
    // Callers validate alignment and extent before reserving an aperture range.
    return std::span {blobMappingOwners_}.subspan(
        offset / protocol::hostVisibleSharedMemoryAlignment,
        size / protocol::hostVisibleSharedMemoryAlignment);
}

void VirtioGpuDevice::releaseBlobScanout() noexcept
{
    if (scanoutMapping_ != nullptr) {
        UML_GPU_RENDERER.unmapResource(scanoutResource_);
        scanoutMapping_ = nullptr;
        scanoutMappingSize_ = 0;
    }
    if (scanoutTexture_ != nullptr) {
        UML_GPU_RENDERER.releaseBlobScanout(scanoutTexture_);
        scanoutTexture_ = nullptr;
    }
    scanoutVirglBlob_ = false;
    scanoutFormat_ = {};
    scanoutWidth_ = 0;
    scanoutHeight_ = 0;
    scanoutStride_ = 0;
    scanoutOffset_ = 0;
}

void VirtioGpuDevice::clearScanout() noexcept
{
    displayActivityLog().record("scanout clear resource=%u", scanoutResource_);
    releaseBlobScanout();
    std::vector<std::uint8_t>().swap(readbackStaging_);
    scanoutResource_ = protocol::invalidResourceId;
    scanoutRectangle_ = {};
    UML_GPU_DISPLAY.setGuestScanoutActive(false);
    UML_GPU_DISPLAY.clear();
}

protocol::GpuResponse VirtioGpuDevice::presentReadback(
    std::uint32_t resourceId,
    protocol::GpuFormat format,
    const protocol::GpuRectangle& damage)
{
    UML_GPU_DISPLAY.recordCpuFallbackFrame();
    const auto fallbackDamage =
        UML_GPU_DISPLAY.needsFullCpuFallback() ? scanoutRectangle_ : damage;
    const auto stagingBytes = pixelBufferSize(
        scanoutRectangle_.width, scanoutRectangle_.height);
    const auto readbackBytes = pixelBufferSize(
        fallbackDamage.width, fallbackDamage.height);
    if (!stagingBytes || !readbackBytes || *readbackBytes > *stagingBytes)
        return protocol::GpuResponse::errorUnspecified;
    const auto readbackRowBytes =
        static_cast<std::size_t>(fallbackDamage.width) *
        protocol::gpuBytesPerPixel;
    try {
        /* The first readback of a scanout sizes the staging to all of it,
         * and later ones reuse it until the scanout changes. A scanout that
         * the display always takes as a Metal texture never allocates it. */
        if (readbackStaging_.size() != *stagingBytes)
            readbackStaging_.resize(*stagingBytes);
        const auto readback = std::span<std::uint8_t>(readbackStaging_)
            .first(*readbackBytes);
        if (UML_GPU_RENDERER.readResourceRectangle(
                resourceId,
                readback,
                fallbackDamage.x,
                fallbackDamage.y,
                fallbackDamage.width,
                fallbackDamage.height) != 0) {
            return protocol::GpuResponse::errorUnspecified;
        }
        UML_GPU_DISPLAY.update(
            std::span<const std::uint8_t>(readback),
            fallbackDamage.width,
            fallbackDamage.height,
            readbackRowBytes,
            format,
            0,
            0,
            fallbackDamage.x - scanoutRectangle_.x,
            fallbackDamage.y - scanoutRectangle_.y,
            fallbackDamage.width,
            fallbackDamage.height);
    } catch (const std::bad_alloc&) {
        return protocol::GpuResponse::errorOutOfMemory;
    } catch (...) {
        return protocol::GpuResponse::errorUnspecified;
    }
    return protocol::GpuResponse::okNoData;
}

protocol::GpuConfig VirtioGpuDevice::config() const noexcept
{
    std::uint32_t events = 0;
    if (displayGeneration_ != clearedDisplayGeneration_)
        events |= protocol::gpuEventDisplay;
    if (memoryGeneration_ != clearedMemoryGeneration_)
        events |= protocol::gpuEventMemory;
    return {
        .eventsRead = events,
        .eventsClear = 0,
        .scanoutCount = protocol::primaryScanoutId + 1U,
        .capsetCount = protocol::venusCapsetIndex +
            (UML_GPU_VENUS_OFFERED ? 1U : 0U),
    };
}

void VirtioGpuDevice::recordConfigRead(
    std::uint32_t offset,
    std::uint32_t size) noexcept
{
    constexpr std::uint32_t eventsReadOffset =
        offsetof(protocol::GpuConfig, eventsRead);
    constexpr std::uint32_t eventsReadEnd =
        eventsReadOffset + sizeof(protocol::GpuConfig::eventsRead);
    const auto readEnd =
        static_cast<std::uint64_t>(offset) + size;
    if (offset <= eventsReadOffset && readEnd >= eventsReadEnd) {
        /* events_clear acknowledges the generation whose events_read field
         * the guest actually received. Unrelated partial config reads must
         * not advance this snapshot. */
        if (displayGeneration_ != clearedDisplayGeneration_)
            reportedDisplayGeneration_ = displayGeneration_;
        if (memoryGeneration_ != clearedMemoryGeneration_)
            reportedMemoryGeneration_ = memoryGeneration_;
    }
}

void VirtioGpuDevice::notifyDisplayChanged() noexcept
{
    ++displayGeneration_;
}

void VirtioGpuDevice::clearEvents(std::uint32_t events) noexcept
{
    if ((events & protocol::gpuEventDisplay) != 0)
        clearedDisplayGeneration_ = reportedDisplayGeneration_;
    if ((events & protocol::gpuEventMemory) != 0)
        clearedMemoryGeneration_ = reportedMemoryGeneration_;
}

bool VirtioGpuDevice::memoryEventPending() const noexcept
{
    return memoryGeneration_ != clearedMemoryGeneration_;
}

bool VirtioGpuDevice::offersMemoryReport() const noexcept
{
    return UML_GPU_DEVICE_MEMORY.has_value();
}

void VirtioGpuDevice::setMemoryWake(std::function<void(MemoryWake)> wake)
{
    memoryWake_ = std::move(wake);
    /* A subscription that outlives the call keeps the wake it was made with. */
    if (memoryPressure_.subscribed())
        UML_GPU_LEDGER.setObserver(memoryObserver());
}

std::function<void()> VirtioGpuDevice::memoryObserver()
{
    if (!memoryWake_)
        return {};
    /* A refusal reaches the ledger's observer as a change does. The ledger
     * calls its observer one call at a time. */
    wokenRefusals_.store(
        UML_GPU_DISPLAY.memoryRefusals(), std::memory_order_relaxed);
    return [this, wake = memoryWake_] {
        const std::uint64_t refusals = UML_GPU_DISPLAY.memoryRefusals();
        const bool refused =
            wokenRefusals_.exchange(refusals, std::memory_order_relaxed) !=
            refusals;
        wake(refused ? MemoryWake::immediate : MemoryWake::coalesced);
    };
}

MemoryPressure::Reading VirtioGpuDevice::memoryReading() const noexcept
{
    MemoryPressure::Reading reading {
        .refusals = UML_GPU_DISPLAY.memoryRefusals(),
    };
    if (const auto figures = UML_GPU_DEVICE_MEMORY) {
        reading.budgetBytes = figures->budgetBytes;
        reading.usageBytes = figures->usageBytes;
        reading.ceilingBytes = figures->ceilingBytes;
    }
    return reading;
}

bool VirtioGpuDevice::evaluateMemory(
    std::chrono::steady_clock::time_point now) noexcept
{
    if (!memoryPressure_.subscribed())
        return false;
    const bool raise = memoryPressure_.evaluate(now, memoryReading());
    /* The usage holds what the host's drivers cache, which the pool frees
     * before it refuses: free it first, and let the evaluator read the figure
     * that is left once it has settled (its deadline). */
    if (memoryPressure_.reclaimDue(now))
        memoryPressure_.reclaimed(now, UML_GPU_RECLAIM_MEMORY != 0);
    if (!raise)
        return false;
    ++memoryGeneration_;
    ++memoryEventsRaised_;
    return true;
}

std::optional<std::chrono::steady_clock::time_point>
VirtioGpuDevice::memoryDeadline() const noexcept
{
    return memoryPressure_.deadline();
}

std::vector<std::uint8_t> VirtioGpuDevice::memoryReport(
    const protocol::GpuControlHeader& header,
    std::span<const std::uint8_t> bytes,
    std::size_t capacity)
{
    if (!offersMemoryReport())
        return response(header, protocol::GpuResponse::errorUnspecified);
    virtio_gpu_uml_get_memory query;
    constexpr std::uint32_t knownFlags = VIRTIO_GPU_UML_MEMORY_SUBSCRIBE |
        VIRTIO_GPU_UML_MEMORY_GUEST_RECLAIMS;
    if (bytes.size() < sizeof(query))
        return response(header, protocol::GpuResponse::errorInvalidParameter);
    std::memcpy(&query, bytes.data(), sizeof(query));
    if ((query.flags & ~knownFlags) != 0 || query.padding != 0) {
        return response(
            header, protocol::GpuResponse::errorInvalidParameter);
    }

    const auto now = std::chrono::steady_clock::now();
    const auto reading = memoryReading();
    const bool subscribe =
        (query.flags & VIRTIO_GPU_UML_MEMORY_SUBSCRIBE) != 0;
    if (subscribe && !memoryPressure_.subscribed()) {
        memoryPressure_.subscribe(reading);
        UML_GPU_LEDGER.setObserver(memoryObserver());
    } else if (!subscribe && memoryPressure_.subscribed()) {
        memoryPressure_.unsubscribe();
        UML_GPU_LEDGER.setObserver({});
    }
    UML_GPU_DISPLAY.setGuestReclaims(
        (query.flags & VIRTIO_GPU_UML_MEMORY_GUEST_RECLAIMS) != 0);
    const auto state = memoryPressure_.query(now, reading);

    /* Only a process the host can name is a holder to the guest. */
    const std::size_t room = capacity > memoryReplyFixedSize
        ? (capacity - memoryReplyFixedSize) /
            sizeof(virtio_gpu_uml_memory_holder)
        : 0;
    const std::size_t limit = std::min<std::size_t>(
        {query.max_holders, VIRTIO_GPU_UML_MAX_HOLDERS, room});
    const auto snapshot = UML_GPU_LEDGER.snapshot(
        now,
        std::numeric_limits<std::size_t>::max(),
        std::chrono::milliseconds {query.min_idle_ms},
        query.min_bytes);

    std::vector<std::uint8_t> reply(memoryReplyFixedSize);
    std::uint32_t count = 0;
    for (const auto& holder : snapshot.holders) {
        if (count >= limit)
            break;
        if (holder.pid == 0)
            continue;
        virtio_gpu_uml_memory_holder wire {};
        wire.bytes = holder.bytes;
        wire.pid = holder.pid;
        wire.contexts = holder.contexts;
        wire.idle_ms = static_cast<std::uint32_t>(std::min<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(holder.idle)
                .count(),
            std::numeric_limits<std::uint32_t>::max()));
        /* A command name is whatever the process chose: keep it to one
         * printable line, so a reader of the guest's file cannot be handed a
         * forged line. */
        const auto length = std::min<std::size_t>(
            holder.name.size(), VIRTIO_GPU_UML_COMM_LEN - 1);
        for (std::size_t index = 0; index < length; ++index) {
            const auto character = static_cast<unsigned char>(holder.name[index]);
            wire.comm[index] = character < 0x20 || character == 0x7f
                ? '?'
                : static_cast<char>(character);
        }
        const auto* wireBytes = reinterpret_cast<const std::uint8_t*>(&wire);
        reply.insert(reply.end(), wireBytes, wireBytes + sizeof(wire));
        ++count;
    }

    virtio_gpu_uml_memory fixed {};
    fixed.hdr.type = VIRTIO_GPU_RESP_OK_UML_MEMORY;
    fixed.hdr.flags = header.flags &
        (protocol::gpuFlagFence | protocol::gpuFlagInfoRingIndex);
    fixed.hdr.fence_id = header.fenceId;
    fixed.hdr.ctx_id = header.contextId;
    fixed.hdr.ring_idx = header.ringIndex;
    fixed.budget_bytes = reading.budgetBytes;
    fixed.used_bytes = reading.usageBytes;
    fixed.context_bytes = snapshot.totalBytes;
    fixed.refusals = reading.refusals;
    fixed.state = (state.pressure ? VIRTIO_GPU_UML_STATE_PRESSURE : 0U) |
        (state.refused ? VIRTIO_GPU_UML_STATE_REFUSED : 0U);
    fixed.generation = memoryEventsRaised_;
    fixed.holder_count = count;
    std::memcpy(reply.data(), &fixed, sizeof(fixed));
    return reply;
}

bool VirtioGpuDevice::displayEventPending() const noexcept
{
    return displayGeneration_ != clearedDisplayGeneration_;
}

void VirtioGpuDevice::setStopRequest(
    const std::atomic_bool* stopRequested) noexcept
{
    stopRequested_ = stopRequested;
}

void VirtioGpuDevice::reset() noexcept
{
    /* Renderer objects may retain iovecs into the current connection's guest
     * mappings. Release them before the transport unmaps those regions. Each
     * tracked renderer object is retired once, then all host bookkeeping is
     * returned to the pristine connection state. */
    /* No wake may reach the transport once the guest is gone, and the
     * ledger's clear below is no change anyone is told of. */
    UML_GPU_LEDGER.setObserver({});
    memoryPressure_.unsubscribe();
    UML_GPU_DISPLAY.setGuestReclaims(false);
    memoryGeneration_ = 0;
    reportedMemoryGeneration_ = 0;
    clearedMemoryGeneration_ = 0;
    memoryEventsRaised_ = 0;
    releaseBlobScanout();
    for (auto& [resourceId, resource] : virglResources_) {
        if (!resource.vectors.empty())
            UML_GPU_RENDERER.detachBacking(resourceId);
        UML_GPU_RENDERER.unrefResource(resourceId);
    }
    for (auto& [resourceId, resource] : blobResources_)
        UML_GPU_RENDERER.unrefResource(resourceId);
    for (const auto contextId : contexts_)
        UML_GPU_RENDERER.destroyContext(contextId);
    UML_GPU_RENDERER.resetFenceState();
    UML_GPU_LEDGER.clear();
    contexts_.clear();
    venusContexts_.clear();
    virglResources_.clear();
    blobResources_.clear();
    std::fill(blobMappingOwners_.begin(), blobMappingOwners_.end(),
        protocol::invalidResourceId);
    resources_.clear();
    transferStaging_.clear();
    std::vector<std::uint8_t>().swap(readbackStaging_);
    displayGeneration_ = 0;
    reportedDisplayGeneration_ = 0;
    clearedDisplayGeneration_ = 0;
    sharedMemoryActions_.clear();
    scanoutResource_ = protocol::invalidResourceId;
    scanoutRectangle_ = {};
    UML_GPU_DISPLAY.setGuestScanoutActive(false);
    UML_GPU_DISPLAY.resetGuestSession();
    UML_GPU_DISPLAY.clear();
}

std::vector<std::uint8_t> VirtioGpuDevice::response(
    const protocol::GpuControlHeader& request,
    protocol::GpuResponse type) const
{
    protocol::GpuControlHeader header = request;
    header.type = static_cast<std::uint32_t>(type);
    header.flags &=
        protocol::gpuFlagFence | protocol::gpuFlagInfoRingIndex;
    std::vector<std::uint8_t> bytes(sizeof(header));
    std::memcpy(bytes.data(), &header, sizeof(header));
    return bytes;
}

std::vector<std::uint8_t> VirtioGpuDevice::discardedResponse(
    std::span<const std::uint8_t> request,
    bool cursorQueue) const
{
    const auto* header = readStructure<protocol::GpuControlHeader>(request);
    if (cursorQueue || header == nullptr)
        return {};
    const protocol::GpuControlHeader answer {
        .type = static_cast<std::uint32_t>(protocol::GpuResponse::okNoData),
        .flags = header->flags &
            (protocol::gpuFlagFence | protocol::gpuFlagInfoRingIndex),
        .fenceId = header->fenceId,
        .contextId = header->contextId,
        .ringIndex = header->ringIndex,
        .padding = {},
    };
    std::vector<std::uint8_t> bytes(sizeof(answer));
    std::memcpy(bytes.data(), &answer, sizeof(answer));
    return bytes;
}

std::vector<std::uint8_t> VirtioGpuDevice::displayInfo(
    const protocol::GpuControlHeader& request) const
{
    protocol::GpuDisplayInfo information {};
    information.header = request;
    information.header.type =
        static_cast<std::uint32_t>(protocol::GpuResponse::okDisplayInfo);
    information.header.flags &=
        protocol::gpuFlagFence | protocol::gpuFlagInfoRingIndex;
    information.modes[0].rectangle = {
        .x = 0,
        .y = 0,
        .width = UML_GPU_DISPLAY.width(),
        .height = UML_GPU_DISPLAY.height(),
    };
    information.modes[protocol::primaryScanoutId].enabled =
        protocol::enabledDisplayMode;
    std::vector<std::uint8_t> bytes(sizeof(information));
    std::memcpy(bytes.data(), &information, sizeof(information));
    return bytes;
}

std::vector<std::uint8_t> VirtioGpuDevice::edid(
    const protocol::GpuGetEdid& request) const
{
    const auto width = UML_GPU_DISPLAY.width();
    const auto height = UML_GPU_DISPLAY.height();
    if (request.scanoutId != protocol::primaryScanoutId) {
        return response(
            request.header, protocol::GpuResponse::errorInvalidScanout);
    }
    if (dotsPerInch_ == 0 || width == 0 || height == 0 ||
        width > protocol::maximumEdidDimension ||
        height > protocol::maximumEdidDimension) {
        return response(
            request.header, protocol::GpuResponse::errorInvalidParameter);
    }
    protocol::GpuEdid information {};
    information.header = request.header;
    information.header.type =
        static_cast<std::uint32_t>(protocol::GpuResponse::okEdid);
    information.header.flags &=
        protocol::gpuFlagFence | protocol::gpuFlagInfoRingIndex;
    information.size = protocol::baseEdidSize;
    const auto base = makeEdid(width, height, dotsPerInch_);
    std::copy(base.begin(), base.end(), information.edid.begin());
    std::vector<std::uint8_t> bytes(sizeof(information));
    std::memcpy(bytes.data(), &information, sizeof(information));
    return bytes;
}

bool VirtioGpuDevice::readBacking(
    const Resource& resource,
    std::uint64_t offset,
    std::span<std::uint8_t> destination) const
{
    std::uint64_t logicalStart = 0;
    std::size_t copied = 0;
    for (const auto& entry : resource.backing) {
        const std::uint64_t logicalEnd = logicalStart + entry.length;
        if (logicalEnd < logicalStart)
            return false;
        if (offset < logicalEnd && copied < destination.size()) {
            const std::uint64_t entryOffset =
                offset > logicalStart ? offset - logicalStart : 0;
            if (entry.address >
                std::numeric_limits<std::uint64_t>::max() - entryOffset) {
                return false;
            }
            const auto available = static_cast<std::size_t>(
                entry.length - entryOffset);
            const auto count =
                std::min(available, destination.size() - copied);
            if (!guestReader_(
                    entry.address + entryOffset,
                    destination.subspan(copied, count))) {
                return false;
            }
            copied += count;
            offset += count;
        }
        logicalStart = logicalEnd;
    }
    return copied == destination.size();
}

bool VirtioGpuDevice::parseBacking(
    std::span<const std::uint8_t> request,
    std::size_t entriesOffset,
    std::uint32_t entryCount,
    std::vector<iovec>& vectors) const
{
    if (entryCount > protocol::maximumResourceBackingEntries)
        return false;
    const auto entriesSize =
        static_cast<std::size_t>(entryCount) *
        sizeof(protocol::GpuMemoryEntry);
    if (entriesOffset > request.size() ||
        entriesSize > request.size() - entriesOffset) {
        return false;
    }

    vectors.clear();
    vectors.reserve(entryCount);
    for (std::uint32_t index = 0; index < entryCount; ++index) {
        const auto* entry = readStructure<protocol::GpuMemoryEntry>(
            request.subspan(
                entriesOffset +
                static_cast<std::size_t>(index) *
                    sizeof(protocol::GpuMemoryEntry)));
        if (entry == nullptr ||
            !backingRangeFits(entry->address, entry->length))
            return false;
        auto mapping = guestMapper_(entry->address, entry->length);
        if (mapping.size() != entry->length)
            return false;
        vectors.push_back({
            .iov_base = mapping.data(),
            .iov_len = mapping.size(),
        });
    }
    return true;
}

std::variant<VirglRenderer::Capset, protocol::GpuResponse>
VirtioGpuDevice::rendererCapset(std::uint32_t capsetId) const
{
    try {
        return UML_GPU_RENDERER.capset(capsetId);
    } catch (const std::bad_alloc&) {
        return protocol::GpuResponse::errorOutOfMemory;
    } catch (const std::exception&) {
        /* A capability set the renderer cannot fill, as when it has lost its
         * GL context, fails the query that asked for it and nothing else. */
        return protocol::GpuResponse::errorUnspecified;
    }
}

std::vector<std::uint8_t> VirtioGpuDevice::capsetInfo(
    const protocol::GpuGetCapsetInfo& request) const
{
    protocol::GpuCapsetInfo information {};
    information.header = request.header;
    information.header.type =
        static_cast<std::uint32_t>(protocol::GpuResponse::okCapsetInfo);
    information.header.flags &=
        protocol::gpuFlagFence | protocol::gpuFlagInfoRingIndex;
    if (request.capsetIndex == protocol::virglCapsetIndex ||
        request.capsetIndex == protocol::virgl2CapsetIndex ||
        (UML_GPU_VENUS_OFFERED &&
         request.capsetIndex == protocol::venusCapsetIndex)) {
        information.capsetId =
            request.capsetIndex == protocol::virglCapsetIndex
            ? protocol::virglCapsetId
            : request.capsetIndex == protocol::virgl2CapsetIndex
                ? protocol::virgl2CapsetId
                : protocol::venusCapsetId;
        const auto result = rendererCapset(information.capsetId);
        if (const auto* error = std::get_if<protocol::GpuResponse>(&result))
            return response(request.header, *error);
        const auto& virglCapset = std::get<VirglRenderer::Capset>(result);
        information.maximumVersion = virglCapset.maximumVersion;
        information.maximumSize =
            static_cast<std::uint32_t>(virglCapset.data.size());
    }
    std::vector<std::uint8_t> bytes(sizeof(information));
    std::memcpy(bytes.data(), &information, sizeof(information));
    return bytes;
}

std::vector<std::uint8_t> VirtioGpuDevice::capset(
    const protocol::GpuGetCapset& request) const
{
    if (request.capsetId != protocol::virglCapsetId &&
        request.capsetId != protocol::virgl2CapsetId &&
        !(UML_GPU_VENUS_OFFERED &&
          request.capsetId == protocol::venusCapsetId)) {
        return response(
            request.header, protocol::GpuResponse::errorInvalidParameter);
    }
    const auto result = rendererCapset(request.capsetId);
    if (const auto* error = std::get_if<protocol::GpuResponse>(&result))
        return response(request.header, *error);
    const auto& virglCapset = std::get<VirglRenderer::Capset>(result);
    if (request.version > virglCapset.maximumVersion)
        return response(
            request.header, protocol::GpuResponse::errorInvalidParameter);

    auto header = request.header;
    header.type = static_cast<std::uint32_t>(protocol::GpuResponse::okCapset);
    header.flags &=
        protocol::gpuFlagFence | protocol::gpuFlagInfoRingIndex;
    std::vector<std::uint8_t> bytes(
        sizeof(header) + virglCapset.data.size());
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::memcpy(
        bytes.data() + sizeof(header),
        virglCapset.data.data(),
        virglCapset.data.size());
    return bytes;
}

std::vector<std::uint8_t> VirtioGpuDevice::process(
    std::span<const std::uint8_t> request,
    bool cursorQueue)
{
    return processInternal(request, cursorQueue, 0, nullptr, nullptr);
}

VirtioGpuDevice::ProcessResult VirtioGpuDevice::processWithDeferredFence(
    std::span<const std::uint8_t> request,
    bool cursorQueue,
    std::uint64_t fenceCookie,
    std::size_t responseCapacity)
{
    /* A reply the guest left no room for is refused before the command runs,
     * so the command changes nothing. The refusal is a bare header, which the
     * caller has checked fits. */
    const auto* header = readStructure<protocol::GpuControlHeader>(request);
    const bool control = !cursorQueue && header != nullptr;
    const auto command = control
        ? static_cast<protocol::GpuCommand>(header->type)
        : protocol::GpuCommand {};
    if (control && responseCapacity < replySize(command)) {
        return {
            .response = response(
                *header, protocol::GpuResponse::errorInvalidParameter),
        };
    }

    /* The reply to a memory query is cut to the room the guest left, and
     * the query is no part of the renderer's order. */
    if (control && command == protocol::GpuCommand::umlGetMemory)
        return {.response = memoryReport(*header, request, responseCapacity)};

    std::optional<DeferredFence> deferredFence;
    std::optional<std::uint32_t> destroyedContextId;
    auto reply = processInternal(
        request,
        cursorQueue,
        fenceCookie,
        &deferredFence,
        &destroyedContextId);
    /* GET_CAPSET's reply also carries the capability set, whose size only
     * the renderer knows. The query changes nothing, so it is refused once
     * its reply is built. */
    if (command == protocol::GpuCommand::getCapset &&
        reply.size() > responseCapacity) {
        reply = response(*header, protocol::GpuResponse::errorInvalidParameter);
    }
    return {
        .response = std::move(reply),
        .fence = deferredFence,
        .destroyedContextId = destroyedContextId,
    };
}

bool VirtioGpuDevice::canProcessWhileFencesPending(
    std::span<const std::uint8_t> request,
    bool cursorQueue) const noexcept
{
    const auto* header = readStructure<protocol::GpuControlHeader>(request);
    if (cursorQueue || header == nullptr)
        return true;
    switch (static_cast<protocol::GpuCommand>(header->type)) {
    case protocol::GpuCommand::getDisplayInfo:
    case protocol::GpuCommand::getEdid:
    case protocol::GpuCommand::getCapsetInfo:
    case protocol::GpuCommand::getCapset:
    case protocol::GpuCommand::umlGetMemory:
    case protocol::GpuCommand::submit3d:
    case protocol::GpuCommand::transferToHost3d:
    case protocol::GpuCommand::transferFromHost3d:
    case protocol::GpuCommand::transferToHost2d:
        return true;
    case protocol::GpuCommand::resourceFlush: {
        /* Console pixels are CPU-owned. Blocking their flush behind a
         * renderer fence can also block a later submit carrying the host
         * signal that fence needs. Renderer flushes retain ordering. */
        const auto* flush = readStructure<protocol::GpuResourceFlush>(request);
        return flush != nullptr && resources_.contains(flush->resourceId);
    }
    default:
        return false;
    }
}

int VirtioGpuDevice::pollContextFences(
    std::uint32_t contextId) noexcept
{
    return UML_GPU_RENDERER.pollContextFences(contextId);
}

int VirtioGpuDevice::pollRenderer() noexcept
{
    return UML_GPU_RENDERER.pollRenderer();
}

bool VirtioGpuDevice::rendererHasPendingWork() noexcept
{
    return UML_GPU_RENDERER.hasPendingWork();
}

std::optional<std::chrono::steady_clock::duration>
VirtioGpuDevice::nextIdleTrimDelay() const noexcept
{
    return UML_GPU_RENDERER.nextIdleTrimDelay();
}

void VirtioGpuDevice::trimIdleContexts() noexcept
{
    UML_GPU_RENDERER.trimIdleContexts();
}

int VirtioGpuDevice::contextFenceStatus(
    const DeferredFence& fence) noexcept
{
    return UML_GPU_RENDERER.contextFenceStatus(
        fence.contextId, fence.ringIndex, fence.cookie);
}

/* Inlined into both callers: as a call it costs each fenced command 17
 * instructions. */
[[gnu::always_inline]] bool VirtioGpuDevice::completeFence(
    const protocol::GpuControlHeader& header,
    std::uint64_t fenceCookie,
    std::optional<DeferredFence>* deferredFence)
{
    const std::uint32_t ringIndex =
        (header.flags & protocol::gpuFlagInfoRingIndex) != 0
        ? header.ringIndex
        : 0;
    /* Venus places each guest ring on its own fence timeline, so any ring of
     * a Venus context is deferrable; Virgl retires one timeline per context,
     * so only its ring zero is. Venus ring indexes are already bounded by
     * validVenusFenceHeader(). */
    if (deferredFence != nullptr && fenceCookie != 0 &&
        header.contextId != 0 &&
        (ringIndex == 0 || venusContexts_.contains(header.contextId))) {
        if (UML_GPU_RENDERER.createContextFence(
                header.contextId, ringIndex, fenceCookie) != 0) {
            return false;
        }
        *deferredFence = DeferredFence {
            .contextId = header.contextId,
            .ringIndex = ringIndex,
            .cookie = fenceCookie,
        };
        return true;
    }
    return UML_GPU_RENDERER.createFenceAndWait(
               header.contextId,
               ringIndex,
               header.fenceId,
               stopRequested_) == 0;
}

std::vector<std::uint8_t> VirtioGpuDevice::processInternal(
    std::span<const std::uint8_t> request,
    bool cursorQueue,
    std::uint64_t fenceCookie,
    std::optional<DeferredFence>* deferredFence,
    std::optional<std::uint32_t>* destroyedContextId)
{
    const auto* header = readStructure<protocol::GpuControlHeader>(request);
    if (header == nullptr) {
        protocol::GpuControlHeader empty {};
        return response(empty, protocol::GpuResponse::errorInvalidParameter);
    }
    const auto command = static_cast<protocol::GpuCommand>(header->type);

    if (cursorQueue) {
        /*
         * The cursor virtqueue has only a device-readable command
         * descriptor. Unlike the control queue, cursor commands do not carry
         * a writable response buffer; completion is reported solely through
         * the used ring. The device has no cursor plane, so it consumes them.
         */
        return {};
    }

    switch (command) {
    case protocol::GpuCommand::getDisplayInfo:
        return displayInfo(*header);

    case protocol::GpuCommand::getEdid: {
        const auto* edidRequest =
            readStructure<protocol::GpuGetEdid>(request);
        if (edidRequest == nullptr)
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        return edid(*edidRequest);
    }

    case protocol::GpuCommand::getCapsetInfo: {
        const auto* capsetInfoRequest =
            readStructure<protocol::GpuGetCapsetInfo>(request);
        if (capsetInfoRequest == nullptr)
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        return capsetInfo(*capsetInfoRequest);
    }

    case protocol::GpuCommand::umlGetMemory:
        return memoryReport(*header, request, memoryReplyMaximumSize);

    case protocol::GpuCommand::getCapset: {
        const auto* capsetRequest =
            readStructure<protocol::GpuGetCapset>(request);
        if (capsetRequest == nullptr)
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        return capset(*capsetRequest);
    }

    case protocol::GpuCommand::contextCreate: {
        const auto* create =
            readStructure<protocol::GpuContextCreate>(request);
        if (create == nullptr) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        const auto requestedCapset =
            create->contextInit & protocol::contextInitCapsetMask;
        if (create->header.contextId == protocol::invalidContextId ||
            create->nameLength >
                protocol::maximumContextDebugNameLength ||
            (requestedCapset != 0 &&
             requestedCapset != protocol::virglCapsetId &&
             requestedCapset != protocol::virgl2CapsetId &&
             !(UML_GPU_VENUS_OFFERED &&
               requestedCapset == protocol::venusCapsetId)) ||
            (create->contextInit & ~protocol::contextInitCapsetMask) != 0 ||
            contexts_.contains(create->header.contextId)) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        const auto rendererContextInit = requestedCapset == 0 ?
            protocol::virgl2CapsetId : create->contextInit;
        const std::string_view debugName(
            create->debugName.data(), create->nameLength);
        auto success = response(*header, protocol::GpuResponse::okNoData);
        /* A Venus context is named by both sets and the ledger, so it enters
         * all of them or none: a create that runs out of memory leaves no
         * trace, and the guest's retry of the same ID succeeds. The ledger
         * knows the context before the renderer does, since the renderer
         * reports the context's device memory from its first allocation. */
        try {
            contexts_.insert(create->header.contextId);
            if (requestedCapset == protocol::venusCapsetId)
                venusContexts_.insert(create->header.contextId);
            UML_GPU_LEDGER.contextCreated(
                create->header.contextId,
                debugName,
                requestedCapset == protocol::venusCapsetId,
                ContextMemoryLedger::Clock::now());
        } catch (const std::bad_alloc&) {
            contexts_.erase(create->header.contextId);
            venusContexts_.erase(create->header.contextId);
            return response(
                *header, protocol::GpuResponse::errorOutOfMemory);
        }
        if (UML_GPU_RENDERER.createContext(
                create->header.contextId,
                rendererContextInit,
                debugName) != 0) {
            UML_GPU_LEDGER.contextDestroyed(create->header.contextId);
            contexts_.erase(create->header.contextId);
            venusContexts_.erase(create->header.contextId);
            return response(
                *header, protocol::GpuResponse::errorInvalidContext);
        }
        return success;
    }

    case protocol::GpuCommand::contextDestroy: {
        if (header->contextId == protocol::invalidContextId ||
            !contexts_.contains(header->contextId)) {
            return response(
                *header, protocol::GpuResponse::errorInvalidContext);
        }
        auto success = response(*header, protocol::GpuResponse::okNoData);
        UML_GPU_RENDERER.destroyContext(header->contextId);
        UML_GPU_LEDGER.contextDestroyed(header->contextId);
        contexts_.erase(header->contextId);
        venusContexts_.erase(header->contextId);
        if (destroyedContextId != nullptr)
            *destroyedContextId = header->contextId;
        return success;
    }

    case protocol::GpuCommand::contextAttachResource:
    case protocol::GpuCommand::contextDetachResource: {
        const auto* contextResource =
            readStructure<protocol::GpuContextResource>(request);
        if (contextResource == nullptr) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        if (command == protocol::GpuCommand::contextAttachResource) {
            if (!contexts_.contains(header->contextId)) {
                return response(
                    *header, protocol::GpuResponse::errorInvalidContext);
            }
            if (!virglResources_.contains(contextResource->resourceId) &&
                !blobResources_.contains(contextResource->resourceId) &&
                !resources_.contains(contextResource->resourceId)) {
                return response(
                    *header, protocol::GpuResponse::errorInvalidResource);
            }
            auto success = response(
                *header, protocol::GpuResponse::okNoData);
            if (virglResources_.contains(contextResource->resourceId) ||
                blobResources_.contains(contextResource->resourceId)) {
                bool credited = false;
                try {
                    credited = UML_GPU_LEDGER.resourceAttached(
                        header->contextId, contextResource->resourceId);
                } catch (const std::bad_alloc&) {
                    return response(
                        *header, protocol::GpuResponse::errorOutOfMemory);
                }
                if (UML_GPU_RENDERER.attachResource(
                        header->contextId,
                        contextResource->resourceId) != 0) {
                    if (credited) {
                        UML_GPU_LEDGER.resourceDetached(
                            header->contextId, contextResource->resourceId);
                    }
                    return response(
                        *header, protocol::GpuResponse::errorUnspecified);
                }
            }
            /*
             * Linux also associates legacy RESOURCE_CREATE_2D buffers with a
             * Virgl context. Those buffers remain on the device's software
             * 2D path, so there is no virglrenderer object to attach; context
             * association is valid bookkeeping and intentionally a no-op.
             */
            return success;
        }
        auto success = response(*header, protocol::GpuResponse::okNoData);
        if (contexts_.contains(header->contextId) &&
            (virglResources_.contains(contextResource->resourceId) ||
             blobResources_.contains(contextResource->resourceId))) {
            /*
             * RESOURCE_UNREF asks virglrenderer to detach the resource from
             * every context before removing it, and context destruction may
             * race the guest's resource bookkeeping. Preserve
             * virglrenderer's idempotent no-op semantics when either object
             * has already gone away.
             */
            if (UML_GPU_RENDERER.detachResource(
                    header->contextId,
                    contextResource->resourceId) != 0) {
                return response(
                    *header, protocol::GpuResponse::errorUnspecified);
            }
            UML_GPU_LEDGER.resourceDetached(
                header->contextId, contextResource->resourceId);
        }
        return success;
    }

    case protocol::GpuCommand::submit3d: {
        const auto* submit =
            readStructure<protocol::GpuSubmit3d>(request);
        if (submit == nullptr ||
            submit->size > protocol::maximumGpuSubmitSize ||
            submit->size % protocol::gpuCommandWordSize != 0 ||
            submit->size != request.size() - sizeof(*submit) ||
            (venusContexts_.contains(header->contextId) &&
             !validVenusFenceHeader(*header))) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        if (!contexts_.contains(header->contextId)) {
            std::fprintf(
                stderr,
                "Virtio-GPU submit rejected unknown context: id=%u bytes=%u\n",
                header->contextId,
                submit->size);
            return response(
                *header, protocol::GpuResponse::errorInvalidContext);
        }
        auto success = response(*header, protocol::GpuResponse::okNoData);
        UML_GPU_LEDGER.contextActive(
            header->contextId, ContextMemoryLedger::Clock::now());
        const int submitResult = UML_GPU_RENDERER.submit(
            header->contextId,
            request.subspan(sizeof(*submit), submit->size));
        if (submitResult != 0) {
            std::fprintf(
                stderr,
                "Virtio-GPU renderer submit failed: context=%u bytes=%u "
                "result=%d venus=%u\n",
                header->contextId,
                submit->size,
                submitResult,
                venusContexts_.contains(header->contextId) ? 1U : 0U);
            return response(
                *header, protocol::GpuResponse::errorInvalidContext);
        }
        if ((header->flags & protocol::gpuFlagFence) != 0 &&
            !completeFence(*header, fenceCookie, deferredFence)) {
            return response(
                *header, protocol::GpuResponse::errorUnspecified);
        }
        return success;
    }

    case protocol::GpuCommand::resourceCreate3d: {
        const auto* create =
            readStructure<protocol::GpuResourceCreate3d>(request);
        if (create == nullptr ||
            create->resourceId == protocol::invalidResourceId ||
            create->width == 0 || create->height == 0 ||
            create->depth == 0 || create->arraySize == 0 ||
            resourceIdInUse(create->resourceId)) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        const VirglRenderer::Resource3d resource {
            .resourceId = create->resourceId,
            .target = create->target,
            .format = create->format,
            .bind = create->bind,
            .width = create->width,
            .height = create->height,
            .depth = create->depth,
            .arraySize = create->arraySize,
            .lastLevel = create->lastLevel,
            .sampleCount = create->sampleCount,
            .flags = create->flags,
        };
        auto success = response(*header, protocol::GpuResponse::okNoData);
        try {
            UML_GPU_LEDGER.reserveResource(create->resourceId);
            virglResources_.emplace(
                create->resourceId,
                VirglResource {
                    .format =
                        static_cast<protocol::GpuFormat>(create->format),
                    .target = create->target,
                    .width = create->width,
                    .height = create->height,
                });
        } catch (const std::bad_alloc&) {
            UML_GPU_LEDGER.resourceReleased(create->resourceId);
            return response(
                *header, protocol::GpuResponse::errorOutOfMemory);
        }
        if (const int created = UML_GPU_RENDERER.createResource3d(resource);
            created != 0) {
            UML_GPU_LEDGER.resourceReleased(create->resourceId);
            virglResources_.erase(create->resourceId);
            /* The renderer reports a host driver with no room for the
             * resource's storage as ENOMEM, which is the one failure the
             * guest can tell from a resource it described wrongly: the
             * driver gives its application GL_OUT_OF_MEMORY, and the guest
             * kernel fails the creation ioctl with -ENOMEM. The renderer has
             * already reclaimed and retried by now, so this is a refusal that
             * reaches the guest, and the display counts it for the
             * application to report. */
            const bool noRoom = created == ENOMEM || created == -ENOMEM;
            if (noRoom)
                UML_GPU_DISPLAY.recordMemoryRefusal();
            return response(
                *header,
                noRoom ? protocol::GpuResponse::errorOutOfMemory
                       : protocol::GpuResponse::errorInvalidParameter);
        }
        UML_GPU_LEDGER.setResourceBytes(
            create->resourceId,
            UML_GPU_RENDERER.resourceStorageBytes(create->resourceId));
        return success;
    }

    case protocol::GpuCommand::resourceCreateBlob: {
        const auto* create =
            readStructure<protocol::GpuResourceCreateBlob>(request);
        const auto chargedSize = create == nullptr || create->size == 0 ||
                create->size > protocol::hostVisibleSharedMemoryRegionSize
            ? 0U
            : alignHostVisibleSize(create->size);
        if (create == nullptr ||
            create->resourceId == protocol::invalidResourceId ||
            create->header.flags != 0 ||
            create->header.fenceId != 0 ||
            create->header.ringIndex != 0 ||
            !headerPaddingIsZero(create->header) ||
            create->size == 0 ||
            create->size > protocol::hostVisibleSharedMemoryRegionSize ||
            chargedSize == 0 ||
            (create->flags & ~protocol::blobFlagMask) != 0 ||
            create->memory != protocol::blobMemoryHost3d ||
            create->entryCount != 0 ||
            resourceIdInUse(create->resourceId)) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        /* Renderer-owned blob storage is not resident in the guest-visible
         * aperture until RESOURCE_MAP_BLOB. Bound the control-plane objects
         * here; map-time range and overlap checks bound aperture residency. */
        if (blobResources_.size() >=
            protocol::maximumHostVisibleBlobResources) {
            return response(
                *header, protocol::GpuResponse::errorOutOfMemory);
        }
        /*
         * Both renderer capsets own host storage the guest can map: Venus
         * publishes Vulkan device memory, and Virgl publishes the coherent
         * buffer storage behind GL_ARB_buffer_storage. Any 3D context may
         * therefore create a host-visible blob; the aperture bounds above
         * apply equally to both.
         */
        if (!contexts_.contains(header->contextId)) {
            return response(
                *header, protocol::GpuResponse::errorInvalidContext);
        }
        if (request.size() != sizeof(*create)) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }

        try {
            auto success = response(*header, protocol::GpuResponse::okNoData);
            LedgerReservation reservation(UML_GPU_LEDGER, create->resourceId);
            auto [iterator, inserted] = blobResources_.emplace(
                create->resourceId,
                BlobResource {
                    .contextId = header->contextId,
                    .venus = venusContexts_.contains(header->contextId),
                    .memory = create->memory,
                    .flags = create->flags,
                    .size = create->size,
                    .chargedSize = chargedSize,
                });
            if (!inserted) {
                return response(
                    *header, protocol::GpuResponse::errorInvalidResource);
            }
            const VirglRenderer::ResourceBlob resource {
                .resourceId = create->resourceId,
                .contextId = header->contextId,
                .memory = create->memory,
                .flags = create->flags,
                .blobId = create->blobId,
                .size = create->size,
            };
            if (const int created =
                    UML_GPU_RENDERER.createResourceBlob(resource);
                created != 0) {
                blobResources_.erase(iterator);
                /* A blob the host had no memory to create is answered as
                 * out of memory, so the guest can tell it from a blob it
                 * described wrongly, and the display counts it as it counts
                 * a refused 3D resource. */
                const bool noRoom = created == ENOMEM || created == -ENOMEM;
                if (noRoom)
                    UML_GPU_DISPLAY.recordMemoryRefusal();
                return response(
                    *header,
                    noRoom ? protocol::GpuResponse::errorOutOfMemory
                           : protocol::GpuResponse::errorInvalidParameter);
            }
            UML_GPU_LEDGER.blobMade(
                create->resourceId, header->contextId, create->size);
            reservation.commit();
            return success;
        } catch (const std::bad_alloc&) {
            return response(
                *header, protocol::GpuResponse::errorOutOfMemory);
        }
    }

    case protocol::GpuCommand::resourceMapBlob: {
        const auto* map =
            readStructure<protocol::GpuResourceMapBlob>(request);
        if (map == nullptr ||
            request.size() != sizeof(*map) ||
            map->header.flags != 0 ||
            map->header.fenceId != 0 ||
            map->header.contextId != 0 ||
            map->header.ringIndex != 0 ||
            !headerPaddingIsZero(map->header) ||
            map->padding != 0 ||
            map->offset % protocol::hostVisibleSharedMemoryAlignment != 0) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        const auto iterator = blobResources_.find(map->resourceId);
        if (iterator == blobResources_.end()) {
            return response(
                *header, protocol::GpuResponse::errorInvalidResource);
        }
        auto& resource = iterator->second;
        if ((resource.flags & protocol::blobFlagMappable) == 0 ||
            resource.mappingState != BlobResource::MappingState::unmapped ||
            map->offset > protocol::hostVisibleSharedMemoryRegionSize ||
            resource.chargedSize >
                protocol::hostVisibleSharedMemoryRegionSize - map->offset) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        const auto pages = blobMappingPages(map->offset, resource.chargedSize);
        if (std::any_of(pages.begin(), pages.end(), [](std::uint32_t owner) {
                return owner != protocol::invalidResourceId;
            })) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }

        std::uint32_t descriptorType = 0;
        int descriptor = -1;
        const int exportResult = UML_GPU_RENDERER.exportResourceBlob(
            map->resourceId, descriptorType, descriptor);
        UniqueDescriptor ownedDescriptor {descriptor};
        if (exportResult != 0 || descriptor < 0 ||
            descriptorType != VirglRenderer::sharedMemoryBlobDescriptor) {
            return response(
                *header, protocol::GpuResponse::errorUnspecified);
        }
        std::uint32_t mapInfo = 0;
        if (UML_GPU_RENDERER.resourceMapInfo(
                map->resourceId, mapInfo) != 0 ||
            (mapInfo & ~protocol::mapCacheMask) != 0 ||
            (mapInfo & protocol::mapCacheMask) >
                protocol::mapCacheWriteCombined) {
            return response(
                *header, protocol::GpuResponse::errorUnspecified);
        }

        if (nextSharedMemoryActionId_ == 0) {
            return response(
                *header, protocol::GpuResponse::errorUnspecified);
        }
        protocol::GpuMapInfo information {};
        information.header = *header;
        information.header.type =
            static_cast<std::uint32_t>(protocol::GpuResponse::okMapInfo);
        information.header.flags &=
            protocol::gpuFlagFence | protocol::gpuFlagInfoRingIndex;
        information.mapInfo = mapInfo;
        std::vector<std::uint8_t> bytes(sizeof(information));
        std::memcpy(bytes.data(), &information, sizeof(information));
        const auto actionId = nextSharedMemoryActionId_++;
        try {
            sharedMemoryActions_.push_back(SharedMemoryAction {
                .type = SharedMemoryAction::Type::map,
                .actionId = actionId,
                .resourceId = map->resourceId,
                .regionId = protocol::hostVisibleSharedMemoryRegionId,
                .descriptorOffset = 0,
                .regionOffset = map->offset,
                .length = resource.chargedSize,
                .writable = true,
                .descriptor = std::move(ownedDescriptor),
            });
        } catch (const std::bad_alloc&) {
            return response(
                *header, protocol::GpuResponse::errorOutOfMemory);
        }
        resource.mappingState = BlobResource::MappingState::mapping;
        resource.regionOffset = map->offset;
        resource.pendingActionId = actionId;
        std::fill(pages.begin(), pages.end(), map->resourceId);
        return bytes;
    }

    case protocol::GpuCommand::resourceUnmapBlob: {
        const auto* unmap =
            readStructure<protocol::GpuResourceUnmapBlob>(request);
        if (unmap == nullptr ||
            request.size() != sizeof(*unmap) ||
            unmap->header.flags != 0 ||
            unmap->header.fenceId != 0 ||
            unmap->header.contextId != 0 ||
            unmap->header.ringIndex != 0 ||
            !headerPaddingIsZero(unmap->header) ||
            unmap->padding != 0) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        const auto iterator = blobResources_.find(unmap->resourceId);
        if (iterator == blobResources_.end()) {
            return response(
                *header, protocol::GpuResponse::errorInvalidResource);
        }
        auto& resource = iterator->second;
        if (resource.mappingState != BlobResource::MappingState::mapped) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        auto success = response(*header, protocol::GpuResponse::okNoData);
        if (const auto error = queueUnmap(unmap->resourceId, resource))
            return response(*header, *error);
        return success;
    }

    case protocol::GpuCommand::transferToHost3d:
    case protocol::GpuCommand::transferFromHost3d: {
        const auto* transfer =
            readStructure<protocol::GpuTransferHost3d>(request);
        if (transfer == nullptr) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        if (header->contextId != protocol::invalidContextId &&
            !contexts_.contains(header->contextId)) {
            return response(
                *header, protocol::GpuResponse::errorInvalidContext);
        }
        if (header->contextId != protocol::invalidContextId) {
            UML_GPU_LEDGER.contextActive(
                header->contextId, ContextMemoryLedger::Clock::now());
        }
        const auto resource =
            virglResources_.find(transfer->resourceId);
        if (resource == virglResources_.end()) {
            return response(
                *header, protocol::GpuResponse::errorInvalidResource);
        }
        if (resource->second.vectors.empty() ||
            transfer->box.width == 0 ||
            transfer->box.height == 0 ||
            transfer->box.depth == 0) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        const VirglRenderer::Box box {
            .x = transfer->box.x,
            .y = transfer->box.y,
            .z = transfer->box.z,
            .width = transfer->box.width,
            .height = transfer->box.height,
            .depth = transfer->box.depth,
        };
        auto success = response(*header, protocol::GpuResponse::okNoData);
        if (UML_GPU_RENDERER.transferResource(
                command == protocol::GpuCommand::transferToHost3d,
                transfer->resourceId,
                header->contextId,
                transfer->level,
                transfer->stride,
                transfer->layerStride,
                box,
                transfer->offset) != 0) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        if ((header->flags & protocol::gpuFlagFence) != 0 &&
            !completeFence(*header, fenceCookie, deferredFence)) {
            return response(
                *header, protocol::GpuResponse::errorUnspecified);
        }
        return success;
    }

    case protocol::GpuCommand::resourceCreate2d: {
        const auto* create =
            readStructure<protocol::GpuResourceCreate2d>(request);
        if (create == nullptr ||
            create->resourceId == protocol::invalidResourceId ||
            create->width == 0 || create->height == 0 ||
            !supportedFormat(create->format)) {
            return response(*header, protocol::GpuResponse::errorInvalidParameter);
        }
        if (resourceIdInUse(create->resourceId)) {
            return response(*header, protocol::GpuResponse::errorInvalidResource);
        }
        const auto size = pixelBufferSize(create->width, create->height);
        if (!size)
            return response(*header, protocol::GpuResponse::errorOutOfMemory);
        try {
            auto success = response(
                *header, protocol::GpuResponse::okNoData);
            Resource resource {
                .format = static_cast<protocol::GpuFormat>(create->format),
                .width = create->width,
                .height = create->height,
                .pixels = std::vector<std::uint8_t>(*size),
            };
            resources_.emplace(create->resourceId, std::move(resource));
            return success;
        } catch (const std::bad_alloc&) {
            return response(*header, protocol::GpuResponse::errorOutOfMemory);
        }
    }

    case protocol::GpuCommand::resourceUnref: {
        const auto* reference =
            readStructure<protocol::GpuResourceReference>(request);
        if (reference == nullptr)
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        auto success = response(*header, protocol::GpuResponse::okNoData);
        if (resources_.erase(reference->resourceId) != 0) {
            if (scanoutResource_ == reference->resourceId) {
                clearScanout();
            }
            return success;
        }
        if (const auto virgl = virglResources_.find(reference->resourceId);
            virgl != virglResources_.end()) {
            /* The renderer retains the resource's iovec array, so release its
             * reference before erasing the vector that owns that array. */
            UML_GPU_RENDERER.unrefResource(reference->resourceId);
            UML_GPU_LEDGER.resourceReleased(reference->resourceId);
            virglResources_.erase(virgl);
            if (scanoutResource_ == reference->resourceId) {
                clearScanout();
            }
            return success;
        }
        if (const auto blob = blobResources_.find(reference->resourceId);
            blob != blobResources_.end()) {
            auto& resource = blob->second;
            if (scanoutResource_ == reference->resourceId)
                clearScanout();
            if (resource.mappingState == BlobResource::MappingState::mapping ||
                resource.mappingState == BlobResource::MappingState::unmapping) {
                return response(
                    *header, protocol::GpuResponse::errorInvalidParameter);
            }
            if (resource.mappingState == BlobResource::MappingState::mapped) {
                if (const auto error =
                        queueUnmap(reference->resourceId, resource)) {
                    return response(*header, *error);
                }
                resource.retireAfterAction = true;
                return success;
            }
            UML_GPU_RENDERER.unrefResource(reference->resourceId);
            UML_GPU_LEDGER.resourceReleased(reference->resourceId);
            blobResources_.erase(blob);
            return success;
        }
        return response(*header, protocol::GpuResponse::errorInvalidResource);
    }

    case protocol::GpuCommand::resourceAttachBacking: {
        const auto* attach =
            readStructure<protocol::GpuAttachBacking>(request);
        if (attach == nullptr ||
            attach->entryCount == 0 ||
            attach->entryCount > protocol::maximumResourceBackingEntries)
            return response(*header, protocol::GpuResponse::errorInvalidParameter);
        if (const auto iterator = resources_.find(attach->resourceId);
            iterator != resources_.end()) {
            if (!iterator->second.backing.empty())
                return response(
                    *header, protocol::GpuResponse::errorInvalidParameter);
            const std::size_t expected =
                sizeof(*attach) +
                static_cast<std::size_t>(attach->entryCount) *
                    sizeof(protocol::GpuMemoryEntry);
            if (request.size() < expected)
                return response(
                    *header, protocol::GpuResponse::errorInvalidParameter);
            std::vector<BackingEntry> backing;
            backing.reserve(attach->entryCount);
            const auto* entries =
                reinterpret_cast<const protocol::GpuMemoryEntry*>(
                    request.data() + sizeof(*attach));
            for (std::uint32_t index = 0;
                 index < attach->entryCount;
                 ++index) {
                if (!backingRangeFits(
                        entries[index].address, entries[index].length))
                    return response(
                        *header,
                        protocol::GpuResponse::errorInvalidParameter);
                backing.push_back({
                    .address = entries[index].address,
                    .length = entries[index].length,
                });
            }
            auto success = response(
                *header, protocol::GpuResponse::okNoData);
            iterator->second.backing = std::move(backing);
            return success;
        }

        const auto virgl = virglResources_.find(attach->resourceId);
        if (virgl == virglResources_.end())
            return response(
                *header, protocol::GpuResponse::errorInvalidResource);
        if (!virgl->second.vectors.empty())
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        auto success = response(*header, protocol::GpuResponse::okNoData);
        std::vector<iovec> vectors;
        if (!parseBacking(
                request,
                sizeof(*attach),
                attach->entryCount,
                vectors)) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        const int attachResult = UML_GPU_RENDERER.attachBacking(
            attach->resourceId, vectors);
        if (attachResult != 0) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        /* Virgl retains the iovec array pointer. Swapping the successfully
         * attached allocation into the resource preserves that address and
         * makes the final bookkeeping commit non-throwing. */
        virgl->second.vectors.swap(vectors);
        return success;
    }

    case protocol::GpuCommand::resourceDetachBacking: {
        const auto* reference =
            readStructure<protocol::GpuResourceReference>(request);
        if (reference == nullptr)
            return response(*header, protocol::GpuResponse::errorInvalidParameter);
        if (const auto iterator = resources_.find(reference->resourceId);
            iterator != resources_.end()) {
            auto success = response(
                *header, protocol::GpuResponse::okNoData);
            iterator->second.backing.clear();
            return success;
        }
        const auto virgl =
            virglResources_.find(reference->resourceId);
        if (virgl == virglResources_.end())
            return response(
                *header, protocol::GpuResponse::errorInvalidResource);
        auto success = response(*header, protocol::GpuResponse::okNoData);
        UML_GPU_RENDERER.detachBacking(reference->resourceId);
        virgl->second.vectors.clear();
        return success;
    }

    case protocol::GpuCommand::transferToHost2d: {
        const auto* transfer =
            readStructure<protocol::GpuTransferToHost2d>(request);
        if (transfer == nullptr)
            return response(*header, protocol::GpuResponse::errorInvalidParameter);
        const auto iterator = resources_.find(transfer->resourceId);
        if (iterator == resources_.end())
            return response(*header, protocol::GpuResponse::errorInvalidResource);
        auto& resource = iterator->second;
        if (!rectangleFits(
                transfer->rectangle, resource.width, resource.height)) {
            return response(*header, protocol::GpuResponse::errorInvalidParameter);
        }
        const auto rowBytes =
            static_cast<std::size_t>(transfer->rectangle.width) *
            protocol::gpuBytesPerPixel;
        auto success = response(*header, protocol::GpuResponse::okNoData);
        try {
            transferStaging_.resize(
                rowBytes * static_cast<std::size_t>(
                    transfer->rectangle.height));
        } catch (const std::bad_alloc&) {
            return response(*header, protocol::GpuResponse::errorOutOfMemory);
        }
        const std::uint64_t sourceStride =
            static_cast<std::uint64_t>(resource.width) *
            protocol::gpuBytesPerPixel;
        for (std::uint32_t row = 0; row < transfer->rectangle.height; ++row) {
            if (static_cast<std::uint64_t>(row) >
                (std::numeric_limits<std::uint64_t>::max() -
                    transfer->offset) /
                    sourceStride) {
                return response(
                    *header, protocol::GpuResponse::errorInvalidParameter);
            }
            const auto sourceOffset =
                transfer->offset +
                static_cast<std::uint64_t>(row) * sourceStride;
            if (!readBacking(
                    resource,
                    sourceOffset,
                    std::span(transferStaging_).subspan(
                        static_cast<std::size_t>(row) * rowBytes,
                        rowBytes))) {
                return response(
                    *header, protocol::GpuResponse::errorInvalidParameter);
            }
        }
        for (std::uint32_t row = 0; row < transfer->rectangle.height; ++row) {
            const auto destinationOffset =
                (static_cast<std::size_t>(transfer->rectangle.y + row) *
                    resource.width +
                    transfer->rectangle.x) *
                protocol::gpuBytesPerPixel;
            std::memcpy(
                resource.pixels.data() + destinationOffset,
                transferStaging_.data() +
                    static_cast<std::size_t>(row) * rowBytes,
                rowBytes);
        }
        return success;
    }

    case protocol::GpuCommand::setScanout: {
        const auto* scanout =
            readStructure<protocol::GpuSetScanout>(request);
        if (scanout == nullptr ||
            scanout->scanoutId != protocol::primaryScanoutId)
            return response(*header, protocol::GpuResponse::errorInvalidScanout);
        if (scanout->resourceId == protocol::invalidResourceId) {
            auto success = response(
                *header, protocol::GpuResponse::okNoData);
            clearScanout();
            return success;
        }
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        VirglResource* virglResource = nullptr;
        if (const auto iterator = resources_.find(scanout->resourceId);
            iterator != resources_.end()) {
            width = iterator->second.width;
            height = iterator->second.height;
        } else if (const auto virgl =
                       virglResources_.find(scanout->resourceId);
                   virgl != virglResources_.end()) {
            if (!supportedFormat(
                    static_cast<std::uint32_t>(virgl->second.format))) {
                return response(
                    *header, protocol::GpuResponse::errorInvalidParameter);
            }
            width = virgl->second.width;
            height = virgl->second.height;
            virglResource = &virgl->second;
        } else {
            return response(
                *header, protocol::GpuResponse::errorInvalidResource);
        }
        if (!rectangleFits(scanout->rectangle, width, height))
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        /* A Virgl scanout's readback staging is allocated by its first flush
         * that reads back, but the scanout must be one it could hold. */
        if (virglResource != nullptr &&
            !pixelBufferSize(
                scanout->rectangle.width, scanout->rectangle.height)) {
            return response(*header, protocol::GpuResponse::errorOutOfMemory);
        }
        auto success = response(*header, protocol::GpuResponse::okNoData);
        const bool dimensionsChanged =
            scanoutRectangle_.width != scanout->rectangle.width ||
            scanoutRectangle_.height != scanout->rectangle.height;
        if (dimensionsChanged &&
            !UML_GPU_DISPLAY.guestScanoutChanged(
                scanout->rectangle.width,
                scanout->rectangle.height)) {
            protocol::GpuControlHeader failure = *header;
            failure.type = static_cast<std::uint32_t>(
                protocol::GpuResponse::errorOutOfMemory);
            failure.flags &=
                protocol::gpuFlagFence | protocol::gpuFlagInfoRingIndex;
            std::memcpy(success.data(), &failure, sizeof(failure));
            return success;
        }
        /* Staging for another size, or for a scanout that never reads back,
         * is only held memory. */
        if (virglResource == nullptr || dimensionsChanged)
            std::vector<std::uint8_t>().swap(readbackStaging_);
        releaseBlobScanout();
        scanoutResource_ = scanout->resourceId;
        scanoutRectangle_ = scanout->rectangle;
        UML_GPU_DISPLAY.setGuestScanoutActive(true);
        displayActivityLog().record(
            "scanout set kind=%s resource=%u source=%ux%u rect=%ux%u+%u+%u",
            virglResource != nullptr ? "virgl" : "2d",
            scanout->resourceId,
            width,
            height,
            scanout->rectangle.width,
            scanout->rectangle.height,
            scanout->rectangle.x,
            scanout->rectangle.y);
        return success;
    }

    case protocol::GpuCommand::setScanoutBlob: {
        const auto* scanout =
            readStructure<protocol::GpuSetScanoutBlob>(request);
        if (scanout == nullptr ||
            request.size() != sizeof(*scanout) ||
            scanout->scanoutId != protocol::primaryScanoutId) {
            return response(
                *header, protocol::GpuResponse::errorInvalidScanout);
        }
        if (scanout->resourceId == protocol::invalidResourceId) {
            auto success = response(
                *header, protocol::GpuResponse::okNoData);
            clearScanout();
            return success;
        }
        const auto blob = blobResources_.find(scanout->resourceId);
        if (blob == blobResources_.end()) {
            return response(
                *header, protocol::GpuResponse::errorInvalidResource);
        }
        const bool unusedPlanesAreZero = std::all_of(
            scanout->strides.begin() + 1, scanout->strides.end(),
            [](std::uint32_t value) { return value == 0; }) &&
            std::all_of(
                scanout->offsets.begin() + 1, scanout->offsets.end(),
                [](std::uint32_t value) { return value == 0; });
        if (!supportedFormat(scanout->format) || scanout->padding != 0 ||
            !unusedPlanesAreZero ||
            !rectangleFits(
                scanout->rectangle, scanout->width, scanout->height) ||
            !linearImageRangeFits(
                blob->second.size, scanout->offsets[0],
                scanout->strides[0], scanout->width, scanout->height)) {
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }

        auto success = response(*header, protocol::GpuResponse::okNoData);
        void* texture = nullptr;
        const int scanoutResult = UML_GPU_RENDERER.createBlobScanout(
            scanout->resourceId, scanout->width, scanout->height,
            scanout->format, scanout->strides[0], scanout->offsets[0],
            texture);
        const bool readback = scanoutResult != 0 || texture == nullptr;
        if (readback && blob->second.venus) {
            displayActivityLog().record(
                "scanout set kind=blob resource=%u source=%ux%u format=%u "
                "stride=%u offset=%u texture=none error=%d",
                scanout->resourceId,
                scanout->width,
                scanout->height,
                scanout->format,
                scanout->strides[0],
                scanout->offsets[0],
                scanoutResult);
            return response(
                *header, protocol::GpuResponse::errorInvalidParameter);
        }
        /*
         * A Virgl context's blob is a GL texture with no scanout texture of
         * its own; Android's gralloc allocates the compositor's framebuffers
         * this way once blobs exist. Each flush presents the Metal texture
         * that stores the GL texture, and reads the blob back into the CPU
         * framebuffer only when there is none. That readback allocates its
         * staging at the first flush that needs it, so a scanout the display
         * always takes as a Metal texture holds none.
         */
        if (readback &&
            !pixelBufferSize(
                scanout->rectangle.width, scanout->rectangle.height)) {
            return response(*header, protocol::GpuResponse::errorOutOfMemory);
        }
        /* The scanout rectangle is the guest mode whichever path shows it,
         * so the display sizes presentation, input and its window to it as
         * it does for a classic scanout. */
        const bool dimensionsChanged =
            scanoutRectangle_.width != scanout->rectangle.width ||
            scanoutRectangle_.height != scanout->rectangle.height;
        if (dimensionsChanged &&
            !UML_GPU_DISPLAY.guestScanoutChanged(
                scanout->rectangle.width,
                scanout->rectangle.height)) {
            if (texture != nullptr)
                UML_GPU_RENDERER.releaseBlobScanout(texture);
            protocol::GpuControlHeader failure = *header;
            failure.type = static_cast<std::uint32_t>(
                protocol::GpuResponse::errorOutOfMemory);
            failure.flags &=
                protocol::gpuFlagFence | protocol::gpuFlagInfoRingIndex;
            std::memcpy(success.data(), &failure, sizeof(failure));
            return success;
        }
        if (!readback || dimensionsChanged)
            std::vector<std::uint8_t>().swap(readbackStaging_);
        displayActivityLog().record(
            "scanout set kind=blob resource=%u source=%ux%u rect=%ux%u+%u+%u "
            "format=%u stride=%u offset=%u texture=%p path=%s",
            scanout->resourceId,
            scanout->width,
            scanout->height,
            scanout->rectangle.width,
            scanout->rectangle.height,
            scanout->rectangle.x,
            scanout->rectangle.y,
            scanout->format,
            scanout->strides[0],
            scanout->offsets[0],
            texture,
            readback ? "virgl" : "metal");

        releaseBlobScanout();
        scanoutResource_ = scanout->resourceId;
        scanoutRectangle_ = scanout->rectangle;
        scanoutFormat_ = static_cast<protocol::GpuFormat>(scanout->format);
        scanoutWidth_ = scanout->width;
        scanoutHeight_ = scanout->height;
        scanoutStride_ = scanout->strides[0];
        scanoutOffset_ = scanout->offsets[0];
        scanoutTexture_ = texture;
        scanoutVirglBlob_ = readback;
        UML_GPU_DISPLAY.setGuestScanoutActive(true);
        return success;
    }

    case protocol::GpuCommand::resourceFlush: {
        const auto* flush =
            readStructure<protocol::GpuResourceFlush>(request);
        if (flush == nullptr)
            return response(*header, protocol::GpuResponse::errorInvalidParameter);
        if (const auto iterator = resources_.find(flush->resourceId);
            iterator != resources_.end()) {
            const auto& resource = iterator->second;
            if (!rectangleFits(
                    flush->rectangle, resource.width, resource.height)) {
                return response(
                    *header, protocol::GpuResponse::errorInvalidParameter);
            }
            if (flush->resourceId != scanoutResource_)
                return response(*header, protocol::GpuResponse::okNoData);
            const auto damage = intersectRectangles(
                flush->rectangle, scanoutRectangle_);
            if (!damage)
                return response(*header, protocol::GpuResponse::okNoData);
            auto success = response(
                *header, protocol::GpuResponse::okNoData);
            displayActivityLog().record(
                "flush kind=2d resource=%u damage=%ux%u+%u+%u path=cpu",
                flush->resourceId,
                damage->width,
                damage->height,
                damage->x,
                damage->y);
            try {
                UML_GPU_DISPLAY.update(
                    std::span<const std::uint8_t>(resource.pixels),
                    resource.width,
                    resource.height,
                    static_cast<std::size_t>(resource.width) *
                        protocol::gpuBytesPerPixel,
                    resource.format,
                    damage->x,
                    damage->y,
                    damage->x - scanoutRectangle_.x,
                    damage->y - scanoutRectangle_.y,
                    damage->width,
                    damage->height);
            } catch (const std::bad_alloc&) {
                return response(
                    *header, protocol::GpuResponse::errorOutOfMemory);
            } catch (...) {
                return response(
                    *header, protocol::GpuResponse::errorUnspecified);
            }
            return success;
        }

        if (const auto virgl = virglResources_.find(flush->resourceId);
            virgl != virglResources_.end()) {
            auto& resource = virgl->second;
            if (!rectangleFits(
                    flush->rectangle, resource.width, resource.height)) {
                return response(
                    *header, protocol::GpuResponse::errorInvalidParameter);
            }
            if (flush->resourceId != scanoutResource_)
                return response(
                    *header, protocol::GpuResponse::okNoData);
            const auto damage = intersectRectangles(
                flush->rectangle, scanoutRectangle_);
            if (!damage)
                return response(
                    *header, protocol::GpuResponse::okNoData);
            auto success = response(
                *header, protocol::GpuResponse::okNoData);
            /* The guest flushes a scanout only once the fence of the
             * rendering it shows has retired, and a Virgl context fence
             * retires only when the host GPU has finished that work, so the
             * texture is complete whenever the display samples it. */
#if !UML_GPU_SOFTWARE_RENDERER
            VirglRenderer::NativeTexture nativeTexture;
            int textureResult = UML_GPU_RENDERER.resourceMetalTexture(
                flush->resourceId, nativeTexture);
            if (textureResult == 0 &&
                (nativeTexture.width != resource.width ||
                 nativeTexture.height != resource.height)) {
                textureResult = EINVAL;
            }
            const auto outcome = textureResult == 0
                ? UML_GPU_DISPLAY.presentMetal(MetalScanout {
                      .texture = nativeTexture.texture,
                      .sourceWidth = resource.width,
                      .sourceHeight = resource.height,
                      .sourceX = scanoutRectangle_.x,
                      .sourceY = scanoutRectangle_.y,
                      .width = scanoutRectangle_.width,
                      .height = scanoutRectangle_.height,
                      .damageX = damage->x,
                      .damageY = damage->y,
                      .damageWidth = damage->width,
                      .damageHeight = damage->height,
                      .flipped = nativeTexture.bottomUp,
                  })
                : PresentOutcome::unsupported;
            displayActivityLog().record(
                "flush kind=virgl resource=%u damage=%ux%u+%u+%u "
                "texture=%p lookup=%d flipped=%d path=%s outcome=%s",
                flush->resourceId,
                damage->width,
                damage->height,
                damage->x,
                damage->y,
                nativeTexture.texture,
                textureResult,
                nativeTexture.bottomUp ? 1 : 0,
                presentAccepted(outcome) ? "metal" : "cpu",
                presentOutcomeName(outcome));
            if (presentAccepted(outcome)) {
                /* A dropped frame is success for the guest: it renders on its
                 * own timeline and the host presents the newest frame it can. */
                return success;
            }
#else
            /* The software renderer draws on the CPU and owns no Metal
             * texture, so no lookup is made: every flush reads the resource
             * back, which is its one presentation path. */
            displayActivityLog().record(
                "flush kind=virgl resource=%u damage=%ux%u+%u+%u "
                "path=readback renderer=software",
                flush->resourceId,
                damage->width,
                damage->height,
                damage->x,
                damage->y);
#endif
            const auto result = presentReadback(
                flush->resourceId, resource.format, *damage);
            if (result != protocol::GpuResponse::okNoData)
                return response(*header, result);
            return success;
        }

        if (blobResources_.contains(flush->resourceId)) {
            if (flush->resourceId != scanoutResource_)
                return response(
                    *header, protocol::GpuResponse::okNoData);
            if (!rectangleFits(
                    flush->rectangle, scanoutWidth_, scanoutHeight_)) {
                return response(
                    *header, protocol::GpuResponse::errorInvalidParameter);
            }
            const auto damage = intersectRectangles(
                flush->rectangle, scanoutRectangle_);
            if (!damage)
                return response(
                    *header, protocol::GpuResponse::okNoData);
            auto success = response(
                *header, protocol::GpuResponse::okNoData);
            if (scanoutVirglBlob_) {
                /* A Virgl context's blob is a GL texture. Show the Metal
                 * texture that stores it when it holds exactly this scanout,
                 * under the same fence guarantee as a classic resource, and
                 * read it back otherwise. */
#if !UML_GPU_SOFTWARE_RENDERER
                VirglRenderer::NativeTexture nativeTexture;
                int textureResult = UML_GPU_RENDERER.resourceMetalTexture(
                    flush->resourceId, nativeTexture);
                if (textureResult == 0 &&
                    (scanoutOffset_ != 0 ||
                     nativeTexture.width != scanoutWidth_ ||
                     nativeTexture.height != scanoutHeight_ ||
                     formatLayout(nativeTexture.format) !=
                         formatLayout(static_cast<std::uint32_t>(
                             scanoutFormat_)))) {
                    textureResult = EINVAL;
                }
                const auto outcome = textureResult == 0
                    ? UML_GPU_DISPLAY.presentMetal(MetalScanout {
                          .texture = nativeTexture.texture,
                          .sourceWidth = scanoutWidth_,
                          .sourceHeight = scanoutHeight_,
                          .sourceX = scanoutRectangle_.x,
                          .sourceY = scanoutRectangle_.y,
                          .width = scanoutRectangle_.width,
                          .height = scanoutRectangle_.height,
                          .damageX = damage->x,
                          .damageY = damage->y,
                          .damageWidth = damage->width,
                          .damageHeight = damage->height,
                          .flipped = nativeTexture.bottomUp,
                      })
                    : PresentOutcome::unsupported;
                displayActivityLog().record(
                    "flush kind=blob resource=%u damage=%ux%u+%u+%u "
                    "texture=%p lookup=%d flipped=%d path=%s outcome=%s",
                    flush->resourceId,
                    damage->width,
                    damage->height,
                    damage->x,
                    damage->y,
                    nativeTexture.texture,
                    textureResult,
                    nativeTexture.bottomUp ? 1 : 0,
                    presentAccepted(outcome) ? "metal" : "readback",
                    presentOutcomeName(outcome));
                if (presentAccepted(outcome))
                    return success;
#else
                /* As for a classic Virgl resource: the software renderer owns
                 * no Metal texture, so each flush reads the blob back. */
                displayActivityLog().record(
                    "flush kind=blob resource=%u damage=%ux%u+%u+%u "
                    "path=readback renderer=software",
                    flush->resourceId,
                    damage->width,
                    damage->height,
                    damage->x,
                    damage->y);
#endif
                const auto result = presentReadback(
                    flush->resourceId, scanoutFormat_, *damage);
                if (result != protocol::GpuResponse::okNoData)
                    return response(*header, result);
                return success;
            }
            const auto outcome = scanoutTexture_ != nullptr
                ? UML_GPU_DISPLAY.presentMetal(MetalScanout {
                      .texture = scanoutTexture_,
                      .sourceWidth = scanoutWidth_,
                      .sourceHeight = scanoutHeight_,
                      .sourceX = scanoutRectangle_.x,
                      .sourceY = scanoutRectangle_.y,
                      .width = scanoutRectangle_.width,
                      .height = scanoutRectangle_.height,
                      .damageX = damage->x,
                      .damageY = damage->y,
                      .damageWidth = damage->width,
                      .damageHeight = damage->height,
                  })
                : PresentOutcome::unsupported;
            displayActivityLog().record(
                "flush kind=blob resource=%u damage=%ux%u+%u+%u texture=%p "
                "path=%s outcome=%s",
                flush->resourceId,
                damage->width,
                damage->height,
                damage->x,
                damage->y,
                scanoutTexture_,
                presentAccepted(outcome) ? "metal" : "cpu",
                presentOutcomeName(outcome));
            if (presentAccepted(outcome)) {
                return success;
            }

            UML_GPU_DISPLAY.recordCpuFallbackFrame();
            if (scanoutTexture_ != nullptr)
                reportNativeScanoutFallback();
            if (scanoutMapping_ == nullptr) {
                void* mapping = nullptr;
                std::uint64_t mappingSize = 0;
                if (UML_GPU_RENDERER.mapResource(
                        flush->resourceId, mapping, mappingSize) != 0 ||
                    mapping == nullptr ||
                    !linearImageRangeFits(
                        mappingSize, scanoutOffset_, scanoutStride_,
                        scanoutWidth_, scanoutHeight_)) {
                    if (mapping != nullptr)
                        UML_GPU_RENDERER.unmapResource(flush->resourceId);
                    return response(
                        *header, protocol::GpuResponse::errorUnspecified);
                }
                scanoutMapping_ = mapping;
                scanoutMappingSize_ = mappingSize;
            }

            try {
                const auto* pixels = static_cast<const std::uint8_t*>(
                    scanoutMapping_);
                UML_GPU_DISPLAY.update(
                    std::span<const std::uint8_t>(
                        pixels + scanoutOffset_,
                        static_cast<std::size_t>(
                            scanoutMappingSize_ - scanoutOffset_)),
                    scanoutWidth_,
                    scanoutHeight_,
                    scanoutStride_,
                    scanoutFormat_,
                    damage->x,
                    damage->y,
                    damage->x - scanoutRectangle_.x,
                    damage->y - scanoutRectangle_.y,
                    damage->width,
                    damage->height);
            } catch (const std::bad_alloc&) {
                return response(
                    *header, protocol::GpuResponse::errorOutOfMemory);
            } catch (...) {
                return response(
                    *header, protocol::GpuResponse::errorUnspecified);
            }
            return success;
        }

        return response(*header, protocol::GpuResponse::errorInvalidResource);
    }

    default:
        return response(*header, protocol::GpuResponse::errorUnspecified);
    }
}

std::optional<VirtioGpuDevice::SharedMemoryAction>
VirtioGpuDevice::takeSharedMemoryAction() noexcept
{
    if (sharedMemoryActions_.empty())
        return std::nullopt;
    auto action = std::move(sharedMemoryActions_.front());
    sharedMemoryActions_.erase(sharedMemoryActions_.begin());
    return action;
}

void VirtioGpuDevice::completeSharedMemoryAction(
    std::uint64_t actionId,
    bool success) noexcept
{
    for (auto iterator = blobResources_.begin();
         iterator != blobResources_.end();
         ++iterator) {
        auto& resource = iterator->second;
        if (resource.pendingActionId != actionId)
            continue;
        resource.pendingActionId = 0;
        if (resource.mappingState == BlobResource::MappingState::mapping) {
            resource.mappingState = success
                ? BlobResource::MappingState::mapped
                : BlobResource::MappingState::unmapped;
            if (!success) {
                const auto pages = blobMappingPages(
                    resource.regionOffset, resource.chargedSize);
                std::fill(pages.begin(), pages.end(), protocol::invalidResourceId);
                resource.regionOffset = 0;
            }
            return;
        }
        if (resource.mappingState ==
            BlobResource::MappingState::unmapping) {
            if (!success) {
                resource.mappingState = BlobResource::MappingState::mapped;
                return;
            }
            resource.mappingState = BlobResource::MappingState::unmapped;
            const auto pages = blobMappingPages(
                resource.regionOffset, resource.chargedSize);
            std::fill(pages.begin(), pages.end(), protocol::invalidResourceId);
            resource.regionOffset = 0;
            if (resource.retireAfterAction) {
                const auto resourceId = iterator->first;
                UML_GPU_RENDERER.unrefResource(resourceId);
                UML_GPU_LEDGER.resourceReleased(resourceId);
                blobResources_.erase(iterator);
            }
            return;
        }
        return;
    }
}

std::optional<protocol::GpuResponse> VirtioGpuDevice::queueUnmap(
    std::uint32_t resourceId, BlobResource& resource)
{
    if (nextSharedMemoryActionId_ == 0)
        return protocol::GpuResponse::errorUnspecified;
    const auto actionId = nextSharedMemoryActionId_++;
    try {
        sharedMemoryActions_.push_back(SharedMemoryAction {
            .type = SharedMemoryAction::Type::unmap,
            .actionId = actionId,
            .resourceId = resourceId,
            .regionId = protocol::hostVisibleSharedMemoryRegionId,
            .regionOffset = resource.regionOffset,
            .length = resource.chargedSize,
        });
    } catch (const std::bad_alloc&) {
        return protocol::GpuResponse::errorOutOfMemory;
    }
    resource.mappingState = BlobResource::MappingState::unmapping;
    resource.pendingActionId = actionId;
    return std::nullopt;
}

void VirtioGpuDevice::prepareReset() noexcept
{
    try {
        std::size_t mappedCount = 0;
        for (const auto& entry : blobResources_) {
            if (entry.second.mappingState ==
                BlobResource::MappingState::mapped) {
                ++mappedCount;
            }
        }
        sharedMemoryActions_.reserve(
            sharedMemoryActions_.size() + mappedCount);
        for (auto& [resourceId, resource] : blobResources_) {
            if (resource.mappingState != BlobResource::MappingState::mapped)
                continue;
            if (queueUnmap(resourceId, resource))
                return;
        }
    } catch (...) {
    }
}

} // namespace uml::gpu

#undef UML_GPU_RENDERER
#undef UML_GPU_DISPLAY
