// SPDX-License-Identifier: GPL-2.0-or-later

#include "allocation_failure.hpp"
#include "deferred_fences.hpp"
#include "fence_deadline.hpp"
#include "gpu_test_support.hpp"
#include "virtio_gpu_device.hpp"
#include "virtio_gpu_device_testing.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <new>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <string_view>
#include <sys/event.h>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace uml::gpu::testing {
namespace {

using ::uml::testing::cancel_pending_allocation_failure;
using ::uml::testing::fail;
using ::uml::testing::fail_after_allocations;
using protocol::GpuCommand;

class FakeGpuDependencies final : public VirtioGpuDeviceDependencies {
public:
    enum class Operation {
        none,
        create_context,
        destroy_context,
        submit,
        create_resource,
        /* the host driver has no room for the resource's storage */
        create_resource_refused,
        transfer,
        read_resource,
        fence,
        unref_resource,
        attach_backing,
        detach_backing,
        attach_resource,
        detach_resource,
        blob_scanout,
        display_update,
    };

    [[nodiscard]] std::uint32_t width() const noexcept override
    {
        return displayWidth;
    }
    [[nodiscard]] std::uint32_t height() const noexcept override
    {
        return displayHeight;
    }

    [[nodiscard]] bool guestScanoutChanged(
        std::uint32_t width,
        std::uint32_t height) noexcept override
    {
        lastGuestScanoutWidth = width;
        lastGuestScanoutHeight = height;
        ++guestScanoutChangeCount;
        return guestScanoutChangeAccepted;
    }

    void resetGuestSession() noexcept override
    {
        ++guestSessionResetCount;
    }

    void setGuestScanoutActive(bool active) noexcept override
    {
        guestScanoutActive = active;
    }

    void recordMemoryRefusal() noexcept override
    {
        ++memoryRefusalCount;
    }

    void clear() noexcept override
    {
        std::fill(framebuffer.begin(), framebuffer.end(), 0);
        ++clearCount;
    }

    void update(
        std::span<const std::uint8_t> source,
        std::uint32_t sourceWidth,
        std::uint32_t sourceHeight,
        std::size_t sourceRowBytes,
        protocol::GpuFormat,
        std::uint32_t sourceX,
        std::uint32_t sourceY,
        std::uint32_t destinationX,
        std::uint32_t destinationY,
        std::uint32_t rectangleWidth,
        std::uint32_t rectangleHeight) override
    {
        failIfRequested(Operation::display_update);
        lastDisplaySourceWidth = sourceWidth;
        lastDisplaySourceHeight = sourceHeight;
        lastDisplaySourceRowBytes = sourceRowBytes;
        lastDisplaySourceX = sourceX;
        lastDisplaySourceY = sourceY;
        lastDisplayDestinationX = destinationX;
        lastDisplayDestinationY = destinationY;
        lastDisplayWidth = rectangleWidth;
        lastDisplayHeight = rectangleHeight;
        for (std::uint32_t row = 0; row < rectangleHeight; ++row) {
            const auto sourceOffset =
                static_cast<std::size_t>(sourceY + row) * sourceRowBytes +
                static_cast<std::size_t>(sourceX) *
                    protocol::gpuBytesPerPixel;
            const auto destinationOffset =
                (static_cast<std::size_t>(destinationY + row) * width() +
                    destinationX) * protocol::gpuBytesPerPixel;
            std::memcpy(
                framebuffer.data() + destinationOffset,
                source.data() + sourceOffset,
                static_cast<std::size_t>(rectangleWidth) *
                    protocol::gpuBytesPerPixel);
        }
        ++displayUpdateCount;
    }

    [[nodiscard]] VirglRenderer::Capset capset(
        std::uint32_t capsetId) const override
    {
        if (capsetFailures > 0) {
            --capsetFailures;
            throw std::runtime_error("forced capset failure");
        }
        return {
            .maximumVersion = capsetId == protocol::venusCapsetId ? 0U : 1U,
            .data = {0, 0, 0, 0},
        };
    }

    [[nodiscard]] bool offersVenus() const noexcept override
    {
        return venusOffered;
    }

    [[nodiscard]] int createContext(
        std::uint32_t contextId,
        std::uint32_t flags,
        std::string_view) noexcept override
    {
        if (reject(Operation::create_context))
            return EIO;
        try {
            contexts.insert(contextId);
        } catch (...) {
            return ENOMEM;
        }
        lastContextFlags = flags;
        ++createContextCount;
        return 0;
    }

    void destroyContext(std::uint32_t contextId) noexcept override
    {
        contexts.erase(contextId);
        /* The renderer's fence slots go with the context, as in
         * VirglRenderer::destroyContext. */
        std::erase_if(retiredContextFences, [contextId](const auto& entry) {
            return static_cast<std::uint32_t>(entry.first >> 8U) == contextId;
        });
        ++destroyContextCount;
    }

    [[nodiscard]] int submit(
        std::uint32_t,
        std::span<const std::uint8_t>) noexcept override
    {
        if (reject(Operation::submit))
            return EIO;
        ++submitCount;
        return 0;
    }

    [[nodiscard]] int createResource3d(
        const VirglRenderer::Resource3d& resource) noexcept override
    {
        if (reject(Operation::create_resource))
            return EIO;
        if (reject(Operation::create_resource_refused))
            return ENOMEM;
        try {
            resources.insert(resource.resourceId);
            nativeTextures[resource.resourceId] = VirglRenderer::NativeTexture {
                .texture = reinterpret_cast<void*>(
                    static_cast<std::uintptr_t>(0x1234)),
                .format = resource.format,
                .width = resource.width,
                .height = resource.height,
            };
        } catch (...) {
            return ENOMEM;
        }
        ++createResourceCount;
        return 0;
    }

    [[nodiscard]] int createResourceBlob(
        const VirglRenderer::ResourceBlob& resource) noexcept override
    {
        if (reject(Operation::create_resource))
            return EIO;
        /* libvirglrenderer reports its failures as negative errors. */
        if (reject(Operation::create_resource_refused))
            return -ENOMEM;
        try {
            resources.insert(resource.resourceId);
        } catch (...) {
            return ENOMEM;
        }
        lastBlobId = resource.blobId;
        ++createResourceCount;
        return 0;
    }

    [[nodiscard]] int exportResourceBlob(
        std::uint32_t resourceId,
        std::uint32_t& descriptorType,
        int& descriptor) noexcept override
    {
        if (!resources.contains(resourceId))
            return ENOENT;
        descriptorType = VirglRenderer::sharedMemoryBlobDescriptor;
        descriptor = ::dup(STDERR_FILENO);
        return descriptor < 0 ? errno : 0;
    }

    [[nodiscard]] int resourceMapInfo(
        std::uint32_t resourceId,
        std::uint32_t& mapInfo) noexcept override
    {
        if (!resources.contains(resourceId))
            return ENOENT;
        mapInfo = protocol::mapCacheCached;
        return 0;
    }

    [[nodiscard]] int mapResource(
        std::uint32_t resourceId,
        void*& mapping,
        std::uint64_t& size) noexcept override
    {
        if (!resources.contains(resourceId) ||
            reject(Operation::blob_scanout)) {
            mapping = nullptr;
            size = 0;
            return ENOTSUP;
        }
        mapping = blobPixels.data();
        size = blobPixels.size();
        ++mapResourceCount;
        return 0;
    }

    void unmapResource(std::uint32_t resourceId) noexcept override
    {
        lastUnmappedResource = resourceId;
        ++unmapResourceCount;
    }

    [[nodiscard]] int createBlobScanout(
        std::uint32_t resourceId,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t format,
        std::uint32_t stride,
        std::uint32_t offset,
        void*& texture) noexcept override
    {
        ++createBlobScanoutCount;
        if (!resources.contains(resourceId) ||
            reject(Operation::blob_scanout)) {
            texture = nullptr;
            return ENOTSUP;
        }
        lastTextureResource = resourceId;
        lastBlobScanoutWidth = width;
        lastBlobScanoutHeight = height;
        lastBlobScanoutFormat = format;
        lastBlobScanoutStride = stride;
        lastBlobScanoutOffset = offset;
        texture = blobScanoutTexture;
        return 0;
    }

    void releaseBlobScanout(void* texture) noexcept override
    {
        lastReleasedBlobScanout = texture;
        ++releaseBlobScanoutCount;
    }

    [[nodiscard]] int transferResource(
        bool,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        std::uint32_t,
        const VirglRenderer::Box&,
        std::uint64_t) noexcept override
    {
        if (reject(Operation::transfer))
            return EIO;
        ++transferCount;
        return 0;
    }

    int readResourceRectangle(
        std::uint32_t resourceId,
        std::span<std::uint8_t> destination,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t width,
        std::uint32_t height) override
    {
        if (reject(Operation::read_resource))
            return EIO;
        failIfRequested(Operation::read_resource);
        const auto rowBytes =
            static_cast<std::size_t>(width) * protocol::gpuBytesPerPixel;
        if (width == 0 || height == 0 ||
            height > destination.size() / rowBytes ||
            destination.size() !=
                static_cast<std::size_t>(height) * rowBytes) {
            return EINVAL;
        }
        lastReadResource = resourceId;
        lastReadX = x;
        lastReadY = y;
        lastReadWidth = width;
        lastReadHeight = height;
        lastReadDestinationBytes = destination.size();
        lastReadDestination = destination.data();
        for (std::uint32_t row = 0; row < height; ++row) {
            for (std::uint32_t column = 0; column < width; ++column) {
                const auto offset =
                    (static_cast<std::size_t>(row) * width + column) *
                    protocol::gpuBytesPerPixel;
                destination[offset] =
                    static_cast<std::uint8_t>(x + column);
                destination[offset + 1U] =
                    static_cast<std::uint8_t>(y + row);
                destination[offset + 2U] = readResourceTag;
                destination[offset + 3U] = 0xffU;
            }
        }
        ++readResourceCount;
        return 0;
    }

    int resourceMetalTexture(
        std::uint32_t resourceId,
        VirglRenderer::NativeTexture& texture) noexcept override
    {
        texture = {};
        if (!nativeResourceAvailable)
            return ENOTSUP;
        /* A blob reports the GL texture its Virgl context created, which
         * the test describes; a classic resource reports its own. */
        const auto native = nativeTextures.find(resourceId);
        texture = native != nativeTextures.end() ? native->second
                                                 : blobNativeTexture;
        if (texture.texture == nullptr)
            return ENOTSUP;
        texture.bottomUp = nativeTextureBottomUp;
        lastTextureResource = resourceId;
        ++resourceMetalTextureCount;
        return 0;
    }

    PresentOutcome presentMetal(
        const MetalScanout& scanout) noexcept override
    {
        if (!directPresentationEnabled)
            return PresentOutcome::unsupported;
        lastMetalScanout = scanout;
        ++directPresentationCount;
        if (directPresentationDrops) {
            ++directPresentationDropCount;
            return PresentOutcome::dropped;
        }
        return PresentOutcome::presented;
    }

    bool needsFullCpuFallback() const noexcept override
    {
        return fullCpuFallback;
    }

    void recordCpuFallbackFrame() noexcept override
    {
        ++cpuFallbackFrameCount;
    }

    [[nodiscard]] int createFenceAndWait(
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t fenceId,
        const std::atomic_bool* stopRequested) noexcept override
    {
        lastFence = {contextId, ringIndex, fenceId};
        if (stopRequested != nullptr &&
            stopRequested->load(std::memory_order_acquire)) {
            return ECANCELED;
        }
        if (reject(Operation::fence))
            return EIO;
        ++fenceCount;
        return 0;
    }

    [[nodiscard]] int createContextFence(
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t fenceCookie) noexcept override
    {
        if (reject(Operation::fence))
            return EIO;
        try {
            retiredContextFences.try_emplace(
                contextRingKey(contextId, ringIndex), 0);
            contextFenceCookies.push_back(fenceCookie);
        } catch (...) {
            return ENOMEM;
        }
        return 0;
    }

    [[nodiscard]] int pollContextFences(std::uint32_t) noexcept override
    {
        return 0;
    }

    [[nodiscard]] int pollRenderer() noexcept override
    {
        ++rendererPollCount;
        return 0;
    }

    [[nodiscard]] bool hasPendingWork() noexcept override
    {
        return rendererPendingWork;
    }

    [[nodiscard]] std::optional<std::chrono::steady_clock::duration>
        nextIdleTrimDelay() const noexcept override
    {
        return idleTrimDelay;
    }

    void trimIdleContexts() noexcept override
    {
        ++idleTrimCount;
    }

    [[nodiscard]] int contextFenceStatus(
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t fenceCookie) noexcept override
    {
        const auto iterator = retiredContextFences.find(
            contextRingKey(contextId, ringIndex));
        if (iterator == retiredContextFences.end())
            return EIO;
        return iterator->second >= fenceCookie ? 0 : EBUSY;
    }

    void unrefResource(std::uint32_t resourceId) noexcept override
    {
        resources.erase(resourceId);
        ++unrefResourceCount;
    }

    [[nodiscard]] std::uint64_t resourceStorageBytes(
        std::uint32_t) noexcept override
    {
        return resourceStorage;
    }

    [[nodiscard]] int attachBacking(
        std::uint32_t resourceId,
        std::span<iovec> backing) noexcept override
    {
        if (reject(Operation::attach_backing))
            return EIO;
        attachedBackingResource = resourceId;
        attachedBackingAddress = backing.data();
        ++attachBackingCount;
        return 0;
    }

    void detachBacking(std::uint32_t resourceId) noexcept override
    {
        if (attachedBackingResource == resourceId) {
            attachedBackingResource = 0;
            attachedBackingAddress = nullptr;
        }
        ++detachBackingCount;
    }

    int attachResource(
        std::uint32_t,
        std::uint32_t) noexcept override
    {
        if (reject(Operation::attach_resource))
            return EIO;
        ++attachResourceCount;
        return 0;
    }

    int detachResource(
        std::uint32_t,
        std::uint32_t) noexcept override
    {
        if (reject(Operation::detach_resource))
            return EIO;
        ++detachResourceCount;
        return 0;
    }

    void resetFenceState() noexcept override
    {
        retiredContextFences.clear();
        contextFenceCookies.clear();
        ++resetFenceCount;
    }

    void retireContextFence(
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t fenceCookie)
    {
        retiredContextFences[contextRingKey(contextId, ringIndex)] =
            fenceCookie;
    }

    void rejectNext(Operation operation) noexcept
    {
        rejectedOperation = operation;
    }

    void throwNext(Operation operation) noexcept
    {
        throwingOperation = operation;
    }

    void failNextCapsets(unsigned count) noexcept
    {
        capsetFailures = count;
    }

    void failAllocationNext(Operation operation) noexcept
    {
        allocationFailureOperation = operation;
    }

    std::array<std::uint8_t, 64U * 64U * protocol::gpuBytesPerPixel>
        framebuffer {};
    std::array<std::uint8_t, 64U * 64U * protocol::gpuBytesPerPixel>
        blobPixels {};
    std::unordered_set<std::uint32_t> contexts;
    std::unordered_set<std::uint32_t> resources;
    void* attachedBackingAddress {};
    std::uint32_t attachedBackingResource {};
    unsigned clearCount {};
    unsigned displayUpdateCount {};
    // A renderer without a Vulkan driver offers no Venus capset.
    bool venusOffered {true};
    // What the renderer reports a Virgl resource to hold.
    std::uint64_t resourceStorage {4U << 20U};
    unsigned createContextCount {};
    std::uint32_t lastContextFlags {};
    unsigned destroyContextCount {};
    unsigned submitCount {};
    unsigned createResourceCount {};
    unsigned transferCount {};
    unsigned readResourceCount {};
    std::uint32_t lastReadResource {};
    std::uint32_t lastReadX {};
    std::uint32_t lastReadY {};
    std::uint32_t lastReadWidth {};
    std::uint32_t lastReadHeight {};
    std::size_t lastReadDestinationBytes {};
    const std::uint8_t* lastReadDestination {};
    std::uint8_t readResourceTag {0x5aU};
    unsigned resourceMetalTextureCount {};
    unsigned directPresentationCount {};
    unsigned directPresentationDropCount {};
    unsigned cpuFallbackFrameCount {};
    unsigned createBlobScanoutCount {};
    unsigned releaseBlobScanoutCount {};
    unsigned mapResourceCount {};
    unsigned unmapResourceCount {};
    bool directPresentationEnabled {};
    bool directPresentationDrops {};
    bool nativeResourceAvailable {};
    bool nativeTextureBottomUp {};
    std::unordered_map<std::uint32_t, VirglRenderer::NativeTexture>
        nativeTextures;
    VirglRenderer::NativeTexture blobNativeTexture {};
    bool fullCpuFallback {};
    std::uint32_t displayWidth {64};
    std::uint32_t displayHeight {64};
    std::uint32_t lastDisplaySourceWidth {};
    std::uint32_t lastDisplaySourceHeight {};
    std::size_t lastDisplaySourceRowBytes {};
    std::uint32_t lastDisplaySourceX {};
    std::uint32_t lastDisplaySourceY {};
    std::uint32_t lastDisplayDestinationX {};
    std::uint32_t lastDisplayDestinationY {};
    std::uint32_t lastDisplayWidth {};
    std::uint32_t lastDisplayHeight {};
    std::uint32_t lastGuestScanoutWidth {};
    std::uint32_t lastGuestScanoutHeight {};
    unsigned guestScanoutChangeCount {};
    bool guestScanoutChangeAccepted {true};
    unsigned guestSessionResetCount {};
    bool guestScanoutActive {};
    std::uint32_t memoryRefusalCount {};
    std::uint32_t lastTextureResource {};
    std::uint32_t lastUnmappedResource {};
    std::uint32_t lastBlobScanoutWidth {};
    std::uint32_t lastBlobScanoutHeight {};
    std::uint32_t lastBlobScanoutFormat {};
    std::uint32_t lastBlobScanoutStride {};
    std::uint32_t lastBlobScanoutOffset {};
    std::uint64_t lastBlobId {};
    void* blobScanoutTexture {
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x5678))};
    void* lastReleasedBlobScanout {};
    MetalScanout lastMetalScanout {};
    unsigned fenceCount {};
    unsigned rendererPollCount {};
    bool rendererPendingWork {};
    std::optional<std::chrono::steady_clock::duration> idleTrimDelay;
    unsigned idleTrimCount {};
    std::vector<std::uint64_t> contextFenceCookies;
    VirtioGpuDevice::DeferredFence lastFence {};
    unsigned unrefResourceCount {};
    unsigned attachBackingCount {};
    unsigned detachBackingCount {};
    unsigned attachResourceCount {};
    unsigned detachResourceCount {};
    unsigned resetFenceCount {};

private:
    [[nodiscard]] static std::uint64_t contextRingKey(
        std::uint32_t contextId,
        std::uint32_t ringIndex) noexcept
    {
        return (static_cast<std::uint64_t>(contextId) << 8U) |
            (ringIndex & 0xffU);
    }

    [[nodiscard]] bool reject(Operation operation) noexcept
    {
        if (rejectedOperation != operation)
            return false;
        rejectedOperation = Operation::none;
        return true;
    }

    void failIfRequested(Operation operation)
    {
        if (allocationFailureOperation == operation) {
            allocationFailureOperation = Operation::none;
            throw std::bad_alloc {};
        }
        if (throwingOperation != operation)
            return;
        throwingOperation = Operation::none;
        throw std::runtime_error("forced GPU dependency failure");
    }

    Operation rejectedOperation {Operation::none};
    Operation throwingOperation {Operation::none};
    // Capability queries are const on the renderer, so their failures are
    // counted apart from the operations above.
    mutable unsigned capsetFailures {};
    Operation allocationFailureOperation {Operation::none};
    std::unordered_map<std::uint64_t, std::uint64_t>
        retiredContextFences;
};

/* The writable bytes a guest leaves a command whose reply is a bare header. */
constexpr std::size_t headerReplyCapacity = sizeof(protocol::GpuControlHeader);

template<typename Request>
[[nodiscard]] std::span<const std::uint8_t> requestBytes(
    const Request& request)
{
    return {
        reinterpret_cast<const std::uint8_t*>(&request),
        sizeof(request),
    };
}

/* A zeroed `Request` for `command`. A request begins with its control header,
 * which is all of a bare header. */
template<typename Request>
[[nodiscard]] Request newRequest(GpuCommand command)
{
    Request request {};
    if constexpr (std::is_same_v<Request, protocol::GpuControlHeader>)
        request.type = static_cast<std::uint32_t>(command);
    else
        request.header.type = static_cast<std::uint32_t>(command);
    return request;
}

[[nodiscard]] protocol::GpuResponse responseType(
    const std::vector<std::uint8_t>& response)
{
    require(response.size() >= sizeof(protocol::GpuControlHeader));
    protocol::GpuControlHeader header {};
    std::memcpy(&header, response.data(), sizeof(header));
    return static_cast<protocol::GpuResponse>(header.type);
}

[[nodiscard]] protocol::GpuControlHeader responseHeader(
    const std::vector<std::uint8_t>& response)
{
    require(response.size() >= sizeof(protocol::GpuControlHeader));
    protocol::GpuControlHeader header {};
    std::memcpy(&header, response.data(), sizeof(header));
    return header;
}

/* Fails the test unless the device answers `request` with `expected`. The
 * failure names the caller's line. */
void requireResponse(
    VirtioGpuDevice& device,
    std::span<const std::uint8_t> request,
    protocol::GpuResponse expected,
    const std::source_location location = std::source_location::current())
{
    const auto actual = responseType(device.process(request, false));
    if (actual == expected)
        return;
    char message[64];
    std::snprintf(message, sizeof(message), "response 0x%x, expected 0x%x",
        static_cast<unsigned>(actual), static_cast<unsigned>(expected));
    fail(message, location);
}

void requireOk(
    VirtioGpuDevice& device,
    std::span<const std::uint8_t> request,
    const std::source_location location = std::source_location::current())
{
    requireResponse(device, request, protocol::GpuResponse::okNoData, location);
}

/* Guest memory callbacks for a device whose commands reach no guest memory. */
bool noGuestReads(std::uint64_t, std::span<std::uint8_t>)
{
    return false;
}

std::span<std::uint8_t> noGuestMappings(std::uint64_t, std::size_t)
{
    return {};
}

/* Maps the guest range at 0x1000 onto `memory`. */
template<std::size_t Size>
[[nodiscard]] auto guestMemoryAt0x1000(std::array<std::uint8_t, Size>& memory)
{
    return [&memory](std::uint64_t address, std::size_t size) {
        if (address != 0x1000 || size > memory.size())
            return std::span<std::uint8_t> {};
        return std::span<std::uint8_t> {memory}.first(size);
    };
}

template<typename Header>
[[nodiscard]] std::vector<std::uint8_t> requestWithEntry(
    const Header& header,
    const protocol::GpuMemoryEntry& entry)
{
    std::vector<std::uint8_t> request(sizeof(header) + sizeof(entry));
    std::memcpy(request.data(), &header, sizeof(header));
    std::memcpy(request.data() + sizeof(header), &entry, sizeof(entry));
    return request;
}

/* Gives `resourceId` the `length` bytes of guest memory at 0x1000. */
[[nodiscard]] std::vector<std::uint8_t> attachBackingRequest(
    std::uint32_t resourceId,
    std::size_t length)
{
    auto attach = newRequest<protocol::GpuAttachBacking>(
        GpuCommand::resourceAttachBacking);
    attach.resourceId = resourceId;
    attach.entryCount = 1;
    return requestWithEntry(
        attach,
        protocol::GpuMemoryEntry {
            .address = 0x1000,
            .length = static_cast<std::uint32_t>(length),
        });
}

/* Takes the shared-memory action the device queued and completes it, as having
 * succeeded or not. Returns the action. */
VirtioGpuDevice::SharedMemoryAction completeAction(
    VirtioGpuDevice& device,
    bool succeeded = true,
    const std::source_location location = std::source_location::current())
{
    auto action = device.takeSharedMemoryAction();
    require(action.has_value(), "the device queued no shared-memory action",
        location);
    device.completeSharedMemoryAction(action->actionId, succeeded);
    return std::move(*action);
}

/* What a command did on the attempts that ran out of memory. */
struct AllocationSweep {
    /* One per allocation the command makes, first to last: the response of
     * the attempt that could not make it, or nullopt when the bad_alloc left
     * the attempt. */
    std::vector<std::optional<protocol::GpuResponse>> failures;
    /* The response of the attempt that made every allocation it needed, or
     * nothing when that attempt held the command behind a fence. */
    std::vector<std::uint8_t> response;
};

/* Fails each allocation a command makes in turn, first to last, each on a
 * fresh attempt. Every such attempt must fail the command and leave
 * `unchanged()` true. A command that commits before it has made all its
 * allocations leaves state behind, which `unchanged`, the next attempt's own
 * validation or the caller's counts see. The attempt that makes them all is
 * the command's own, and the caller checks what it did. It ends with the
 * failure still armed, so it allocated nothing after its last allocation.
 * `attempt()` runs the command once and returns its response, or nullopt
 * when it held the command. */
template<typename Attempt, typename Unchanged>
AllocationSweep failEachAllocationOf(
    Attempt attempt,
    Unchanged unchanged,
    const std::source_location location)
{
    constexpr std::size_t maximumAllocations = 32;
    AllocationSweep sweep;
    for (std::size_t allocation = 0;
         allocation < maximumAllocations;
         ++allocation) {
        fail_after_allocations(allocation);
        bool outOfMemory = false;
        std::optional<std::vector<std::uint8_t>> response;
        try {
            response = attempt();
        } catch (const std::bad_alloc&) {
            outOfMemory = true;
        }
        if (cancel_pending_allocation_failure()) {
            require(!outOfMemory, "the command ran out of memory", location);
            require(!sweep.failures.empty(), "the command allocated nothing",
                location);
            if (response)
                sweep.response = std::move(*response);
            return sweep;
        }
        std::optional<protocol::GpuResponse> failure;
        if (response) {
            failure = responseType(*response);
            require(*failure != protocol::GpuResponse::okNoData,
                "the command succeeded without an allocation it made",
                location);
        }
        sweep.failures.push_back(failure);
        require(unchanged(), "a command that ran out of memory changed state",
            location);
    }
    fail("the command never stopped allocating", location);
}

template<typename Unchanged>
AllocationSweep failEachAllocationBeforeCommit(
    VirtioGpuDevice& device,
    std::span<const std::uint8_t> request,
    Unchanged unchanged,
    const std::source_location location = std::source_location::current())
{
    return failEachAllocationOf(
        [&] {
            return std::optional<std::vector<std::uint8_t>> {
                device.process(request, false)};
        },
        unchanged, location);
}

AllocationSweep failEachAllocationBeforeCommit(
    VirtioGpuDevice& device,
    std::span<const std::uint8_t> request,
    const std::source_location location = std::source_location::current())
{
    return failEachAllocationBeforeCommit(
        device, request, [] { return true; }, location);
}

/* The sweep over a fenced command in the vhost-user server's order, which
 * mirrors GpuVhostUserDevice::processQueue: the server makes room to hold the
 * command, the device runs it, and the server holds one the device deferred
 * in `fences` until its fence retires. Once the device has run it, the
 * renderer owns a fence that names the request, so a failure after that
 * point leaves the command committed with nothing held to answer the
 * guest. */
template<typename Unchanged>
AllocationSweep failEachAllocationBeforeDeferral(
    VirtioGpuDevice& device,
    DeferredFences& fences,
    std::span<const std::uint8_t> request,
    std::uint64_t requestId,
    Unchanged unchanged,
    const std::source_location location = std::source_location::current())
{
    protocol::GpuControlHeader header {};
    std::memcpy(&header, request.data(), sizeof(header));
    return failEachAllocationOf(
        [&]() -> std::optional<std::vector<std::uint8_t>> {
            fences.reserveDefer();
            auto result = device.processWithDeferredFence(
                request, false, requestId, headerReplyCapacity);
            if (!result.fence)
                return std::move(result.response);
            fences.defer(
                requestId,
                header,
                std::move(result.response),
                *result.fence,
                DeferredFences::Clock::now());
            return std::nullopt;
        },
        unchanged, location);
}

/* Processes a request that may allocate its reply and nothing else. */
[[nodiscard]] std::vector<std::uint8_t> processAllocatingOnlyTheReply(
    VirtioGpuDevice& device,
    std::span<const std::uint8_t> request)
{
    std::vector<std::uint8_t> result;
    bool allocatedMore = false;
    fail_after_allocations(1);
    try {
        result = device.process(request, false);
    } catch (const std::bad_alloc&) {
        allocatedMore = true;
    }
    (void)cancel_pending_allocation_failure();
    require(!allocatedMore);
    return result;
}

[[nodiscard]] protocol::GpuResourceCreate2d create2d(
    std::uint32_t resourceId,
    std::uint32_t width = 4,
    std::uint32_t height = 4)
{
    auto create = newRequest<protocol::GpuResourceCreate2d>(
        GpuCommand::resourceCreate2d);
    create.resourceId = resourceId;
    create.format =
        static_cast<std::uint32_t>(protocol::GpuFormat::b8g8r8a8Unorm);
    create.width = width;
    create.height = height;
    return create;
}

[[nodiscard]] protocol::GpuResourceCreate3d create3d(
    std::uint32_t resourceId,
    std::uint32_t width = 4,
    std::uint32_t height = 4)
{
    auto create = newRequest<protocol::GpuResourceCreate3d>(
        GpuCommand::resourceCreate3d);
    create.resourceId = resourceId;
    create.target = 2;
    create.format =
        static_cast<std::uint32_t>(protocol::GpuFormat::b8g8r8a8Unorm);
    create.bind = 1U << 3U;
    create.width = width;
    create.height = height;
    create.depth = 1;
    create.arraySize = 1;
    return create;
}

[[nodiscard]] protocol::GpuContextCreate createContext(
    std::uint32_t contextId,
    std::uint32_t contextInit = protocol::virglCapsetId)
{
    auto context = newRequest<protocol::GpuContextCreate>(
        GpuCommand::contextCreate);
    context.header.contextId = contextId;
    context.contextInit = contextInit;
    return context;
}

void contextInitUsesVirgl2ForTheDeviceSelectedDefault()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {dependencies, noGuestReads, noGuestMappings};

    const auto deviceSelected = createContext(401, 0);
    requireOk(device, requestBytes(deviceSelected));
    require(dependencies.lastContextFlags == protocol::virgl2CapsetId);

    auto resource = create3d(402);
    requireOk(device, requestBytes(resource));
    auto association = newRequest<protocol::GpuContextResource>(
        GpuCommand::contextAttachResource);
    association.header.contextId = deviceSelected.header.contextId;
    association.resourceId = resource.resourceId;
    requireOk(device, requestBytes(association));
    require(dependencies.attachResourceCount == 1);

    const auto virgl = createContext(403, protocol::virglCapsetId);
    requireOk(device, requestBytes(virgl));
    require(dependencies.lastContextFlags == protocol::virglCapsetId);

    const auto virgl2 = createContext(404, protocol::virgl2CapsetId);
    requireOk(device, requestBytes(virgl2));
    require(dependencies.lastContextFlags == protocol::virgl2CapsetId);

    const auto invalidCapset = createContext(405, 3);
    requireResponse(device, requestBytes(invalidCapset),
        protocol::GpuResponse::errorInvalidParameter);
    const auto reservedBits = createContext(406, 1U << 8U);
    requireResponse(device, requestBytes(reservedBits),
        protocol::GpuResponse::errorInvalidParameter);
    require(dependencies.createContextCount == 3);
}

void aRendererWithoutVulkanOffersAndRunsNoVenus()
{
    FakeGpuDependencies dependencies;
    dependencies.venusOffered = false;
    VirtioGpuDevice device {dependencies, noGuestReads, noGuestMappings};
    require(device.config().capsetCount == protocol::venusCapsetIndex);

    // The Virgl capsets are answered as ever; the Venus index names nothing.
    auto capsetInfo = newRequest<protocol::GpuGetCapsetInfo>(
        GpuCommand::getCapsetInfo);
    capsetInfo.capsetIndex = protocol::virgl2CapsetIndex;
    requireResponse(device, requestBytes(capsetInfo),
        protocol::GpuResponse::okCapsetInfo);
    capsetInfo.capsetIndex = protocol::venusCapsetIndex;
    const auto venusInfo = device.process(requestBytes(capsetInfo), false);
    protocol::GpuCapsetInfo information {};
    std::memcpy(&information, venusInfo.data(), sizeof(information));
    require(information.capsetId == 0);
    require(information.maximumSize == 0);

    auto capset = newRequest<protocol::GpuGetCapset>(GpuCommand::getCapset);
    capset.capsetId = protocol::venusCapsetId;
    capset.version = 0;
    requireResponse(device, requestBytes(capset),
        protocol::GpuResponse::errorInvalidParameter);
    capset.capsetId = protocol::virgl2CapsetId;
    capset.version = 1;
    requireResponse(device, requestBytes(capset),
        protocol::GpuResponse::okCapset);

    // A Venus context is refused before the renderer hears of it, and a
    // Virgl context beside it is unaffected.
    const auto venus = createContext(407, protocol::venusCapsetId);
    requireResponse(device, requestBytes(venus),
        protocol::GpuResponse::errorInvalidParameter);
    require(dependencies.createContextCount == 0);
    const auto virgl2 = createContext(408, protocol::virgl2CapsetId);
    requireOk(device, requestBytes(virgl2));
    require(dependencies.createContextCount == 1);
}

void lifecycleCommandsAllocateBeforeCommitAndRetry()
{
    std::array<std::uint8_t, 256> guestMemory {};
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {
        dependencies, noGuestReads, guestMemoryAt0x1000(guestMemory)};
    auto context = createContext(501);
    auto sweep = failEachAllocationBeforeCommit(
        device, requestBytes(context),
        [&] { return dependencies.createContextCount == 0; });
    require(responseType(sweep.response) == protocol::GpuResponse::okNoData);
    require(dependencies.createContextCount == 1);

    auto rendered = create3d(601);
    sweep = failEachAllocationBeforeCommit(
        device, requestBytes(rendered),
        [&] { return dependencies.createResourceCount == 0; });
    require(responseType(sweep.response) == protocol::GpuResponse::okNoData);
    require(dependencies.createResourceCount == 1);

    auto association = newRequest<protocol::GpuContextResource>(
        GpuCommand::contextAttachResource);
    association.header.contextId = context.header.contextId;
    association.resourceId = rendered.resourceId;
    dependencies.rejectNext(
        FakeGpuDependencies::Operation::attach_resource);
    requireResponse(device, requestBytes(association),
        protocol::GpuResponse::errorUnspecified);
    require(dependencies.attachResourceCount == 0);
    sweep = failEachAllocationBeforeCommit(
        device, requestBytes(association),
        [&] { return dependencies.attachResourceCount == 0; });
    require(responseType(sweep.response) == protocol::GpuResponse::okNoData);
    require(dependencies.attachResourceCount == 1);

    association.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::contextDetachResource);
    dependencies.rejectNext(
        FakeGpuDependencies::Operation::detach_resource);
    requireResponse(device, requestBytes(association),
        protocol::GpuResponse::errorUnspecified);
    require(dependencies.detachResourceCount == 0);
    sweep = failEachAllocationBeforeCommit(
        device, requestBytes(association),
        [&] { return dependencies.detachResourceCount == 0; });
    require(responseType(sweep.response) == protocol::GpuResponse::okNoData);
    require(dependencies.detachResourceCount == 1);

    const auto attachRequest = attachBackingRequest(
        rendered.resourceId, guestMemory.size());
    dependencies.rejectNext(
        FakeGpuDependencies::Operation::attach_backing);
    requireResponse(device, attachRequest,
        protocol::GpuResponse::errorInvalidParameter);
    require(dependencies.attachedBackingAddress == nullptr);
    sweep = failEachAllocationBeforeCommit(device, attachRequest, [&] {
        return dependencies.attachBackingCount == 0 &&
            dependencies.attachedBackingAddress == nullptr;
    });
    require(responseType(sweep.response) == protocol::GpuResponse::okNoData);
    require(dependencies.attachBackingCount == 1);
    require(dependencies.attachedBackingAddress != nullptr);

    auto reference = newRequest<protocol::GpuResourceReference>(
        GpuCommand::resourceDetachBacking);
    reference.resourceId = rendered.resourceId;
    sweep = failEachAllocationBeforeCommit(
        device, requestBytes(reference), [&] {
            return dependencies.detachBackingCount == 0 &&
                dependencies.attachedBackingAddress != nullptr;
        });
    require(responseType(sweep.response) == protocol::GpuResponse::okNoData);
    require(dependencies.detachBackingCount == 1);
    require(dependencies.attachedBackingAddress == nullptr);

    reference.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceUnref);
    sweep = failEachAllocationBeforeCommit(
        device, requestBytes(reference),
        [&] { return dependencies.unrefResourceCount == 0; });
    require(responseType(sweep.response) == protocol::GpuResponse::okNoData);
    require(dependencies.unrefResourceCount == 1);
    require(!dependencies.resources.contains(rendered.resourceId));

    protocol::GpuControlHeader destroy = context.header;
    destroy.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::contextDestroy);
    sweep = failEachAllocationBeforeCommit(
        device, requestBytes(destroy),
        [&] { return dependencies.destroyContextCount == 0; });
    require(responseType(sweep.response) == protocol::GpuResponse::okNoData);
    require(dependencies.destroyContextCount == 1);
    require(!dependencies.contexts.contains(context.header.contextId));

    requireResponse(device, requestBytes(destroy),
        protocol::GpuResponse::errorInvalidContext);

    /* A software resource's commands keep their state in the device, so a
     * failed attempt that committed shows as the next attempt's rejection. */
    auto software = create2d(602);
    sweep = failEachAllocationBeforeCommit(device, requestBytes(software));
    require(responseType(sweep.response) == protocol::GpuResponse::okNoData);

    sweep = failEachAllocationBeforeCommit(
        device, attachBackingRequest(software.resourceId, guestMemory.size()));
    require(responseType(sweep.response) == protocol::GpuResponse::okNoData);
    reference.resourceId = software.resourceId;
    reference.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceDetachBacking);
    sweep = failEachAllocationBeforeCommit(device, requestBytes(reference));
    require(responseType(sweep.response) == protocol::GpuResponse::okNoData);
    reference.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::resourceUnref);
    sweep = failEachAllocationBeforeCommit(device, requestBytes(reference));
    require(responseType(sweep.response) == protocol::GpuResponse::okNoData);
}

[[nodiscard]] protocol::GpuContextCreate namedContext(
    std::uint32_t contextId, std::string_view name, std::uint32_t contextInit)
{
    auto context = createContext(contextId, contextInit);
    require(name.size() <= context.debugName.size());
    std::memcpy(context.debugName.data(), name.data(), name.size());
    context.nameLength = static_cast<std::uint32_t>(name.size());
    return context;
}

/* The host GPU's memory is accounted by the guest context that holds it, as
 * the guest's commands create, attach, release and destroy it, and a command
 * that runs out of memory leaves the accounts as they were. */
void theLedgerFollowsTheGuestsCommands()
{
    constexpr std::uint64_t mebibyte = 1U << 20U;
    using Counts = ContextMemoryLedger::Counts;
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {dependencies, noGuestReads, noGuestMappings};
    auto& ledger = dependencies.graphicsLedger();
    const auto holder = [&](std::uint32_t pid) -> std::optional<std::uint64_t> {
        const auto snapshot = ledger.snapshot(
            ContextMemoryLedger::Clock::now(), 16,
            ContextMemoryLedger::Clock::duration::zero(), 0);
        for (const auto& entry : snapshot.holders) {
            if (entry.pid == pid)
                return entry.bytes;
        }
        return std::nullopt;
    };

    const auto allocator =
        namedContext(11, "t|pid=100|comm=allocator", protocol::virglCapsetId);
    const auto app = namedContext(12, "t|pid=200|comm=app", protocol::virglCapsetId);
    const auto vulkan = namedContext(13, "t|pid=300|comm=vk", protocol::venusCapsetId);
    for (const auto* context : {&allocator, &app, &vulkan}) {
        const auto before = ledger.counts();
        const auto sweep = failEachAllocationBeforeCommit(
            device, requestBytes(*context),
            [&] { return ledger.counts() == before; });
        require(responseType(sweep.response) ==
            protocol::GpuResponse::okNoData);
        require(ledger.counts().contexts == before.contexts + 1);
    }
    require(ledger.counts() == Counts {3, 0});

    /* A context the renderer refuses is not kept. */
    dependencies.rejectNext(FakeGpuDependencies::Operation::create_context);
    requireResponse(device,
        requestBytes(namedContext(14, "t|pid=400|comm=x", protocol::virglCapsetId)),
        protocol::GpuResponse::errorInvalidContext);
    require(ledger.counts() == Counts {3, 0});

    /* A resource no context holds yet is no one's. */
    const auto rendered = create3d(601);
    auto sweep = failEachAllocationBeforeCommit(
        device, requestBytes(rendered),
        [&] { return ledger.counts() == Counts {3, 0}; });
    require(responseType(sweep.response) == protocol::GpuResponse::okNoData);
    require(ledger.counts() == Counts {3, 1} && ledger.totalBytes() == 0);

    auto association = newRequest<protocol::GpuContextResource>(
        GpuCommand::contextAttachResource);
    association.header.contextId = allocator.header.contextId;
    association.resourceId = rendered.resourceId;
    dependencies.rejectNext(FakeGpuDependencies::Operation::attach_resource);
    requireResponse(device, requestBytes(association),
        protocol::GpuResponse::errorUnspecified);
    require(ledger.totalBytes() == 0 && !holder(100),
        "an attach the renderer refused was credited");
    sweep = failEachAllocationBeforeCommit(
        device, requestBytes(association),
        [&] { return ledger.totalBytes() == 0 && !holder(100); });
    require(responseType(sweep.response) == protocol::GpuResponse::okNoData);
    require(holder(100) == 4 * mebibyte && ledger.totalBytes() == 4 * mebibyte);

    /* A second holder shares the bytes and does not add to the total, and
     * its extra bookkeeping is made before the command commits. */
    association.header.contextId = app.header.contextId;
    dependencies.rejectNext(FakeGpuDependencies::Operation::attach_resource);
    requireResponse(device, requestBytes(association),
        protocol::GpuResponse::errorUnspecified);
    require(!holder(200), "a second attach the renderer refused was credited");
    sweep = failEachAllocationBeforeCommit(
        device, requestBytes(association),
        [&] { return !holder(200) && dependencies.attachResourceCount == 1; });
    require(responseType(sweep.response) == protocol::GpuResponse::okNoData);
    require(holder(200) == 4 * mebibyte && holder(100) == 4 * mebibyte &&
            ledger.totalBytes() == 4 * mebibyte,
        "a resource two contexts hold was not shared");

    association.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::contextDetachResource);
    requireOk(device, requestBytes(association));
    require(!holder(200) && holder(100) == 4 * mebibyte,
        "a detach did not take the credit back");
    association.header.type = static_cast<std::uint32_t>(
        protocol::GpuCommand::contextAttachResource);
    requireOk(device, requestBytes(association));

    /* A blob belongs to its creator, except that a Venus context's device
     * memory is the renderer's report. */
    auto blob = newRequest<protocol::GpuResourceCreateBlob>(
        GpuCommand::resourceCreateBlob);
    blob.memory = protocol::blobMemoryHost3d;
    blob.flags = protocol::blobFlagMappable;
    blob.size = protocol::hostVisibleSharedMemoryAlignment;
    blob.header.contextId = allocator.header.contextId;
    blob.resourceId = 701;
    sweep = failEachAllocationBeforeCommit(
        device, requestBytes(blob), [&] {
            return ledger.counts() == Counts {3, 1} &&
                dependencies.createResourceCount == 1;
        });
    require(responseType(sweep.response) == protocol::GpuResponse::okNoData);
    require(holder(100) == 4 * mebibyte + blob.size &&
            ledger.counts().resources == 2,
        "a blob was not credited to its creator");

    blob.header.contextId = vulkan.header.contextId;
    blob.resourceId = 702;
    sweep = failEachAllocationBeforeCommit(
        device, requestBytes(blob), [&] {
            return ledger.counts() == Counts {3, 2} &&
                dependencies.createResourceCount == 2;
        });
    require(responseType(sweep.response) == protocol::GpuResponse::okNoData);
    require(!holder(300), "a Venus blob was counted besides its memory");
    ledger.venusMemory(vulkan.header.contextId, 8 * mebibyte);
    require(holder(300) == 8 * mebibyte,
        "a Venus context's device memory was not credited");

    /* A blob the renderer refuses is not kept. */
    blob.resourceId = 703;
    dependencies.rejectNext(FakeGpuDependencies::Operation::create_resource);
    requireResponse(device, requestBytes(blob),
        protocol::GpuResponse::errorInvalidParameter);
    require(ledger.counts() == Counts {3, 3});

    /* A resource the guest releases leaves every holder. */
    auto reference = newRequest<protocol::GpuResourceReference>(
        GpuCommand::resourceUnref);
    reference.resourceId = rendered.resourceId;
    requireOk(device, requestBytes(reference));
    require(!holder(200) && holder(100) == blob.size &&
            ledger.counts() == Counts {3, 2},
        "an unreferenced resource kept its credit");

    /* A context the guest destroys takes its credit, and its Venus memory,
     * with it. */
    protocol::GpuControlHeader destroy = vulkan.header;
    destroy.type =
        static_cast<std::uint32_t>(protocol::GpuCommand::contextDestroy);
    requireOk(device, requestBytes(destroy));
    require(!holder(300) && ledger.counts().contexts == 2,
        "a destroyed Venus context kept its memory");
    ledger.venusMemory(vulkan.header.contextId, mebibyte);
    require(!holder(300), "a report for a destroyed context was credited");

    device.reset();
    require(ledger.counts() == Counts {} && ledger.totalBytes() == 0,
        "a reset guest left accounts behind");
}

/* A context create that runs out of memory leaves no trace, so the guest's
 * retry of the same ID succeeds and makes the kind of context it asked for.
 * A Venus context is named by two sets, and a create that entered only the
 * first refused every retry. */
void contextCreateIsRetryableAfterEachAllocationFailure()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {dependencies, noGuestReads, noGuestMappings};
    for (const auto capset :
         {protocol::venusCapsetId, protocol::virgl2CapsetId}) {
        const bool venus = capset == protocol::venusCapsetId;
        const auto context = createContext(901, capset);
        protocol::GpuControlHeader destroy = context.header;
        destroy.type =
            static_cast<std::uint32_t>(protocol::GpuCommand::contextDestroy);
        /* A ring without a fence: Venus rejects the header, Virgl takes it,
         * and an unknown context is reported as one. */
        auto probe = newRequest<protocol::GpuSubmit3d>(GpuCommand::submit3d);
        probe.header.contextId = context.header.contextId;
        probe.header.flags = protocol::gpuFlagInfoRingIndex;
        const auto createdKind = venus
            ? protocol::GpuResponse::errorInvalidParameter
            : protocol::GpuResponse::okNoData;

        std::size_t retries = 0;
        const auto sweep = failEachAllocationBeforeCommit(
            device, requestBytes(context), [&] {
                if (dependencies.createContextCount !=
                        dependencies.destroyContextCount ||
                    responseType(device.process(requestBytes(probe), false)) !=
                        protocol::GpuResponse::errorInvalidContext) {
                    return false;
                }
                requireOk(device, requestBytes(context));
                requireResponse(device, requestBytes(probe), createdKind);
                requireOk(device, requestBytes(destroy));
                ++retries;
                return true;
            });
        require(responseType(sweep.response) ==
            protocol::GpuResponse::okNoData);
        require(retries == sweep.failures.size());
        require(sweep.failures.size() >= (venus ? 3U : 2U));
        requireResponse(device, requestBytes(probe), createdKind);
        requireOk(device, requestBytes(destroy));
    }
}

void blobCommandsAllocateBeforeCommitAndRetry()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {dependencies, noGuestReads, noGuestMappings};
    const auto context = createContext(0x901, protocol::venusCapsetId);
    requireOk(device, requestBytes(context));

    auto create = newRequest<protocol::GpuResourceCreateBlob>(
        GpuCommand::resourceCreateBlob);
    create.header.contextId = context.header.contextId;
    create.resourceId = 0x902;
    create.memory = protocol::blobMemoryHost3d;
    create.flags = protocol::blobFlagMappable;
    create.size = protocol::hostVisibleSharedMemoryAlignment;
    const auto created = failEachAllocationBeforeCommit(
        device, requestBytes(create), [&] {
            return dependencies.createResourceCount == 0 &&
                !dependencies.resources.contains(create.resourceId);
        });
    require(responseType(created.response) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.createResourceCount == 1);

    /* A map or unmap that committed queues its shared-memory action. */
    const auto noAction = [&] {
        return !device.takeSharedMemoryAction().has_value();
    };
    auto map = newRequest<protocol::GpuResourceMapBlob>(
        GpuCommand::resourceMapBlob);
    map.resourceId = create.resourceId;
    const auto mapped =
        failEachAllocationBeforeCommit(device, requestBytes(map), noAction);
    require(responseType(mapped.response) ==
        protocol::GpuResponse::okMapInfo);
    completeAction(device);

    auto unmap = newRequest<protocol::GpuResourceUnmapBlob>(
        GpuCommand::resourceUnmapBlob);
    unmap.resourceId = create.resourceId;
    const auto unmapped =
        failEachAllocationBeforeCommit(device, requestBytes(unmap), noAction);
    require(responseType(unmapped.response) ==
        protocol::GpuResponse::okNoData);
    completeAction(device);
}

/* A blob the host had no memory to create is answered as out of memory, not
 * as a blob described wrongly, and counted for the application's notice;
 * nothing of it stays behind, and the same blob can be created once there is
 * room. */
void refusedBlobIsAnsweredAsOutOfMemory()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {dependencies, noGuestReads, noGuestMappings};
    const auto context = createContext(0x911, protocol::venusCapsetId);
    requireOk(device, requestBytes(context));

    auto create = newRequest<protocol::GpuResourceCreateBlob>(
        GpuCommand::resourceCreateBlob);
    create.header.contextId = context.header.contextId;
    create.resourceId = 0x912;
    create.memory = protocol::blobMemoryHost3d;
    create.flags = protocol::blobFlagMappable;
    create.size = protocol::hostVisibleSharedMemoryAlignment;

    dependencies.rejectNext(FakeGpuDependencies::Operation::create_resource);
    requireResponse(device, requestBytes(create),
        protocol::GpuResponse::errorInvalidParameter);
    require(dependencies.memoryRefusalCount == 0,
        "a blob described wrongly was counted as a refusal");

    dependencies.rejectNext(
        FakeGpuDependencies::Operation::create_resource_refused);
    requireResponse(device, requestBytes(create),
        protocol::GpuResponse::errorOutOfMemory);
    require(!dependencies.resources.contains(create.resourceId));
    require(dependencies.memoryRefusalCount == 1,
        "the refused blob the guest was answered was not counted");

    requireOk(device, requestBytes(create));
    require(dependencies.resources.contains(create.resourceId));
    require(dependencies.memoryRefusalCount == 1,
        "a blob that was created was counted as a refusal");
}

void edidReportsDisplayPixelsAndEffectiveDensity()
{
    FakeGpuDependencies dependencies;
    dependencies.displayWidth = 1290;
    dependencies.displayHeight = 2796;
    constexpr std::uint32_t dotsPerInch = 288;
    VirtioGpuDevice device {
        dependencies, noGuestReads, noGuestMappings, dotsPerInch};

    auto request = newRequest<protocol::GpuGetEdid>(GpuCommand::getEdid);
    request.header.flags = protocol::gpuFlagFence;
    request.header.fenceId = 0x1234;
    request.scanoutId = protocol::primaryScanoutId;
    const auto bytes = device.process(requestBytes(request), false);
    require(bytes.size() == sizeof(protocol::GpuEdid));
    protocol::GpuEdid response {};
    std::memcpy(&response, bytes.data(), sizeof(response));
    require(responseType(bytes) == protocol::GpuResponse::okEdid);
    require(response.header.flags == protocol::gpuFlagFence);
    require(response.header.fenceId == request.header.fenceId);
    require(response.size == protocol::baseEdidSize);

    unsigned checksum = 0;
    for (std::size_t index = 0; index < response.size; ++index)
        checksum += response.edid[index];
    require((checksum & 0xffU) == 0);
    require(std::equal(
        response.edid.begin(), response.edid.begin() + 8U,
        std::array<std::uint8_t, 8> {
            0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00}.begin()));

    const auto horizontalPixels = static_cast<std::uint32_t>(
        response.edid[56] | ((response.edid[58] & 0xf0U) << 4U));
    const auto verticalPixels = static_cast<std::uint32_t>(
        response.edid[59] | ((response.edid[61] & 0xf0U) << 4U));
    const auto horizontalMillimeters = static_cast<std::uint32_t>(
        response.edid[66] | ((response.edid[68] & 0xf0U) << 4U));
    const auto verticalMillimeters = static_cast<std::uint32_t>(
        response.edid[67] | ((response.edid[68] & 0x0fU) << 8U));
    require(horizontalPixels == dependencies.displayWidth);
    require(verticalPixels == dependencies.displayHeight);
    require(horizontalMillimeters == 114);
    require(verticalMillimeters == 247);

    request.scanoutId = 1;
    requireResponse(device, requestBytes(request),
        protocol::GpuResponse::errorInvalidScanout);
    auto truncated = newRequest<protocol::GpuControlHeader>(
        GpuCommand::getEdid);
    requireResponse(device, requestBytes(truncated),
        protocol::GpuResponse::errorInvalidParameter);
}

void displayChangeEventsPreserveAResizeThatRacesGuestClear()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {dependencies, noGuestReads, noGuestMappings};

    const auto recordFullConfigRead = [&device] {
        device.recordConfigRead(0, sizeof(protocol::GpuConfig));
    };

    require(device.config().eventsRead == 0);
    recordFullConfigRead();
    device.notifyDisplayChanged();
    require(device.config().eventsRead == protocol::gpuEventDisplay);
    recordFullConfigRead();
    require(device.displayEventPending());

    /* The host publishes another geometry after the guest read events_read
     * but before its matching events_clear write. Clearing the reported
     * generation must leave that newer change asserted. */
    device.notifyDisplayChanged();
    require(device.config().scanoutCount == 1);
    device.recordConfigRead(
        offsetof(protocol::GpuConfig, scanoutCount),
        sizeof(protocol::GpuConfig::scanoutCount));
    device.clearEvents(protocol::gpuEventDisplay);
    require(device.config().eventsRead == protocol::gpuEventDisplay);
    recordFullConfigRead();
    require(device.displayEventPending());
    device.clearEvents(protocol::gpuEventDisplay);
    require(device.config().eventsRead == 0);
    require(!device.displayEventPending());

    dependencies.displayWidth = 1024;
    dependencies.displayHeight = 768;
    auto request = newRequest<protocol::GpuControlHeader>(
        GpuCommand::getDisplayInfo);
    const auto bytes = device.process(requestBytes(request), false);
    require(bytes.size() == sizeof(protocol::GpuDisplayInfo));
    protocol::GpuDisplayInfo information {};
    std::memcpy(&information, bytes.data(), sizeof(information));
    require(information.modes[0].rectangle.width == 1024);
    require(information.modes[0].rectangle.height == 768);

    device.notifyDisplayChanged();
    device.reset();
    require(device.config().eventsRead == 0);
}

void rendererExecutionFailuresPreserveDeviceState()
{
    std::array<std::uint8_t, 256> guestMemory {};
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {
        dependencies, noGuestReads, guestMemoryAt0x1000(guestMemory)};
    std::atomic_bool stopRequested {};
    device.setStopRequest(&stopRequested);

    auto context = createContext(701);
    dependencies.rejectNext(
        FakeGpuDependencies::Operation::create_context);
    requireResponse(device, requestBytes(context),
        protocol::GpuResponse::errorInvalidContext);
    require(!dependencies.contexts.contains(context.header.contextId));
    requireOk(device, requestBytes(context));

    auto resource = create3d(702, 4, 4);
    dependencies.rejectNext(
        FakeGpuDependencies::Operation::create_resource);
    requireResponse(device, requestBytes(resource),
        protocol::GpuResponse::errorInvalidParameter);
    require(!dependencies.resources.contains(resource.resourceId));
    requireOk(device, requestBytes(resource));

    /* A host driver with no room for a resource's storage is told apart from
     * a resource described wrongly, so the guest can fail the creation with
     * -ENOMEM; nothing of the refused resource stays behind. */
    auto refused = create3d(703, 4, 4);
    /* Only a creation the guest was refused for want of room is counted for
     * the application's notice: not a resource described wrongly, and not a
     * creation that succeeded. */
    require(dependencies.memoryRefusalCount == 0,
        "a refusal was counted before there was one");
    dependencies.rejectNext(
        FakeGpuDependencies::Operation::create_resource_refused);
    requireResponse(device, requestBytes(refused),
        protocol::GpuResponse::errorOutOfMemory);
    require(!dependencies.resources.contains(refused.resourceId));
    require(dependencies.memoryRefusalCount == 1,
        "the refusal the guest was answered was not counted");
    requireOk(device, requestBytes(refused));
    require(dependencies.memoryRefusalCount == 1,
        "a creation that succeeded was counted as a refusal");

    const auto attachRequest = attachBackingRequest(
        resource.resourceId, guestMemory.size());
    requireOk(device, attachRequest);

    auto submit = newRequest<protocol::GpuSubmit3d>(GpuCommand::submit3d);
    submit.header.contextId = context.header.contextId;
    dependencies.rejectNext(FakeGpuDependencies::Operation::submit);
    requireResponse(device, requestBytes(submit),
        protocol::GpuResponse::errorInvalidContext);
    require(dependencies.submitCount == 0);
    const auto submitted = failEachAllocationBeforeCommit(
        device, requestBytes(submit),
        [&] { return dependencies.submitCount == 0; });
    require(responseType(submitted.response) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.submitCount == 1);

    protocol::GpuTransferHost3d transfer {};
    transfer.header.contextId = context.header.contextId;
    transfer.box = {.width = 1, .height = 1, .depth = 1};
    transfer.resourceId = resource.resourceId;
    for (const auto command : {
             protocol::GpuCommand::transferToHost3d,
             protocol::GpuCommand::transferFromHost3d}) {
        transfer.header.type = static_cast<std::uint32_t>(command);
        const auto before = dependencies.transferCount;
        dependencies.rejectNext(FakeGpuDependencies::Operation::transfer);
        requireResponse(device, requestBytes(transfer),
            protocol::GpuResponse::errorInvalidParameter);
        require(dependencies.transferCount == before);
        const auto transferred = failEachAllocationBeforeCommit(
            device, requestBytes(transfer),
            [&] { return dependencies.transferCount == before; });
        require(responseType(transferred.response) ==
            protocol::GpuResponse::okNoData);
        require(dependencies.transferCount == before + 1);
    }

    submit.header.flags = protocol::gpuFlagFence;
    submit.header.fenceId = 17;
    dependencies.rejectNext(FakeGpuDependencies::Operation::fence);
    const auto submittedBeforeFenceFailure = dependencies.submitCount;
    requireResponse(device, requestBytes(submit),
        protocol::GpuResponse::errorUnspecified);
    // Submission acceptance is irreversible. A later fence-status failure is
    // reported exactly once and never causes an internal retry.
    require(dependencies.submitCount == submittedBeforeFenceFailure + 1);
    require(dependencies.fenceCount == 0);
    require(dependencies.lastFence.contextId == context.header.contextId);
    require(dependencies.lastFence.ringIndex == 0);
    require(dependencies.lastFence.cookie == submit.header.fenceId);

    // A transfer's fence fails alike: the transfer is done, and the failure
    // is reported once.
    transfer.header.flags = protocol::gpuFlagFence;
    transfer.header.fenceId = 19;
    const auto transfersBeforeFenceFailure = dependencies.transferCount;
    dependencies.rejectNext(FakeGpuDependencies::Operation::fence);
    requireResponse(device, requestBytes(transfer),
        protocol::GpuResponse::errorUnspecified);
    require(dependencies.transferCount == transfersBeforeFenceFailure + 1);
    require(dependencies.fenceCount == 0);
    require(dependencies.lastFence.cookie == transfer.header.fenceId);
    transfer.header.flags = 0;

    auto scanout = newRequest<protocol::GpuSetScanout>(GpuCommand::setScanout);
    scanout.rectangle = {.x = 1, .y = 1, .width = 2, .height = 2};
    scanout.scanoutId = protocol::primaryScanoutId;
    scanout.resourceId = resource.resourceId;
    const auto scanoutChangesBefore = dependencies.guestScanoutChangeCount;
    const auto scanoutSet = failEachAllocationBeforeCommit(
        device, requestBytes(scanout), [&] {
            return dependencies.guestScanoutChangeCount ==
                scanoutChangesBefore;
        });
    require(responseType(scanoutSet.response) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.guestScanoutChangeCount == scanoutChangesBefore + 1);

    auto flush = newRequest<protocol::GpuResourceFlush>(
        GpuCommand::resourceFlush);
    flush.rectangle = scanout.rectangle;
    flush.resourceId = resource.resourceId;
    require(dependencies.readResourceCount == 0);
    require(dependencies.displayUpdateCount == 0);
    dependencies.rejectNext(FakeGpuDependencies::Operation::read_resource);
    requireResponse(device, requestBytes(flush),
        protocol::GpuResponse::errorUnspecified);
    require(dependencies.displayUpdateCount == 0);
    dependencies.throwNext(FakeGpuDependencies::Operation::read_resource);
    requireResponse(device, requestBytes(flush),
        protocol::GpuResponse::errorUnspecified);
    require(dependencies.displayUpdateCount == 0);
    dependencies.failAllocationNext(
        FakeGpuDependencies::Operation::display_update);
    requireResponse(device, requestBytes(flush),
        protocol::GpuResponse::errorOutOfMemory);
    require(dependencies.displayUpdateCount == 0);
    dependencies.throwNext(
        FakeGpuDependencies::Operation::display_update);
    requireResponse(device, requestBytes(flush),
        protocol::GpuResponse::errorUnspecified);
    require(dependencies.displayUpdateCount == 0);
    require(dependencies.readResourceCount == 2);
    const auto flushed = failEachAllocationBeforeCommit(
        device, requestBytes(flush), [&] {
            return dependencies.readResourceCount == 2 &&
                dependencies.displayUpdateCount == 0;
        });
    require(responseType(flushed.response) == protocol::GpuResponse::okNoData);
    require(dependencies.readResourceCount == 3);
    require(dependencies.displayUpdateCount == 1);

    // Once the first readback has sized the packed staging area, a fallback
    // flush allocates only its response. Any second allocation is a regression
    // to per-frame staging or cloning.
    const auto warmedFlush =
        processAllocatingOnlyTheReply(device, requestBytes(flush));
    require(responseType(warmedFlush) == protocol::GpuResponse::okNoData);
    require(dependencies.readResourceCount == 4);
    require(dependencies.displayUpdateCount == 2);

    // A nonzero resource-space damage box is packed at the start of the
    // staging span and translated into scanout-relative display coordinates.
    const auto beforePartialFlush = dependencies.framebuffer;
    dependencies.readResourceTag = 0xa5U;
    flush.rectangle = {.x = 2, .y = 2, .width = 1, .height = 1};
    requireOk(device, requestBytes(flush));
    require(dependencies.lastReadResource == resource.resourceId);
    require(dependencies.lastReadX == 2);
    require(dependencies.lastReadY == 2);
    require(dependencies.lastReadWidth == 1);
    require(dependencies.lastReadHeight == 1);
    require(dependencies.lastReadDestinationBytes ==
        protocol::gpuBytesPerPixel);
    require(dependencies.lastDisplaySourceWidth == 1);
    require(dependencies.lastDisplaySourceHeight == 1);
    require(dependencies.lastDisplaySourceRowBytes ==
        protocol::gpuBytesPerPixel);
    require(dependencies.lastDisplaySourceX == 0);
    require(dependencies.lastDisplaySourceY == 0);
    require(dependencies.lastDisplayDestinationX == 1);
    require(dependencies.lastDisplayDestinationY == 1);
    require(dependencies.lastDisplayWidth == 1);
    require(dependencies.lastDisplayHeight == 1);
    constexpr std::size_t changedPixel =
        (64U + 1U) * protocol::gpuBytesPerPixel;
    const std::array<std::uint8_t, protocol::gpuBytesPerPixel>
        expectedPixel {2U, 2U, 0xa5U, 0xffU};
    require(std::memcmp(
        dependencies.framebuffer.data(),
        beforePartialFlush.data(),
        changedPixel) == 0);
    require(std::memcmp(
        dependencies.framebuffer.data() + changedPixel,
        expectedPixel.data(),
        expectedPixel.size()) == 0);
    require(std::memcmp(
        dependencies.framebuffer.data() + changedPixel +
            protocol::gpuBytesPerPixel,
        beforePartialFlush.data() + changedPixel +
            protocol::gpuBytesPerPixel,
        dependencies.framebuffer.size() - changedPixel -
            protocol::gpuBytesPerPixel) == 0);
    require(dependencies.readResourceCount == 5);
    require(dependencies.displayUpdateCount == 3);

    // A display that cannot consume partial damage requests the complete
    // nonzero-origin scanout, still packed at the start of the staging buffer.
    dependencies.fullCpuFallback = true;
    dependencies.readResourceTag = 0xc3U;
    requireOk(device, requestBytes(flush));
    require(dependencies.lastReadX == scanout.rectangle.x);
    require(dependencies.lastReadY == scanout.rectangle.y);
    require(dependencies.lastReadWidth == scanout.rectangle.width);
    require(dependencies.lastReadHeight == scanout.rectangle.height);
    require(dependencies.lastReadDestinationBytes ==
        scanout.rectangle.width * scanout.rectangle.height *
            protocol::gpuBytesPerPixel);
    require(dependencies.lastDisplaySourceWidth == scanout.rectangle.width);
    require(dependencies.lastDisplaySourceHeight == scanout.rectangle.height);
    require(dependencies.lastDisplaySourceX == 0);
    require(dependencies.lastDisplaySourceY == 0);
    require(dependencies.lastDisplayDestinationX == 0);
    require(dependencies.lastDisplayDestinationY == 0);
    const std::array<std::uint8_t, protocol::gpuBytesPerPixel>
        expectedFullOrigin {1U, 1U, 0xc3U, 0xffU};
    require(std::memcmp(
        dependencies.framebuffer.data(),
        expectedFullOrigin.data(),
        expectedFullOrigin.size()) == 0);
    dependencies.fullCpuFallback = false;
    dependencies.readResourceTag = 0x5aU;
    flush.rectangle = scanout.rectangle;

    dependencies.nativeResourceAvailable = true;
    dependencies.directPresentationEnabled = true;
    const auto readsBeforeDirect = dependencies.readResourceCount;
    const auto updatesBeforeDirect = dependencies.displayUpdateCount;
    requireOk(device, requestBytes(flush));
    require(dependencies.resourceMetalTextureCount == 1);
    require(dependencies.directPresentationCount == 1);
    require(dependencies.lastTextureResource == resource.resourceId);
    require(dependencies.lastMetalScanout.texture ==
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1234)));
    require(dependencies.lastMetalScanout.width == scanout.rectangle.width);
    require(dependencies.lastMetalScanout.height == scanout.rectangle.height);
    require(dependencies.lastMetalScanout.damageX == flush.rectangle.x);
    require(dependencies.lastMetalScanout.damageY == flush.rectangle.y);
    require(dependencies.lastMetalScanout.damageWidth == flush.rectangle.width);
    require(
        dependencies.lastMetalScanout.damageHeight == flush.rectangle.height);
    require(dependencies.readResourceCount == readsBeforeDirect);
    require(dependencies.displayUpdateCount == updatesBeforeDirect);

    /* A frame the display drops because host presentation is behind must cost
     * the guest exactly what a presented frame costs: no GPU-to-CPU readback
     * and no CPU display update. That is what decouples the two timelines. */
    dependencies.directPresentationDrops = true;
    const auto readsBeforeDrop = dependencies.readResourceCount;
    const auto updatesBeforeDrop = dependencies.displayUpdateCount;
    const auto fallbacksBeforeDrop = dependencies.cpuFallbackFrameCount;
    requireOk(device, requestBytes(flush));
    require(dependencies.directPresentationDropCount == 1);
    require(dependencies.readResourceCount == readsBeforeDrop);
    require(dependencies.displayUpdateCount == updatesBeforeDrop);
    require(dependencies.cpuFallbackFrameCount == fallbacksBeforeDrop);
    dependencies.directPresentationDrops = false;

    /* A resource the renderer stores bottom row first reaches the display
     * with that orientation rather than through a readback. */
    dependencies.nativeTextureBottomUp = true;
    requireOk(device, requestBytes(flush));
    require(dependencies.lastMetalScanout.flipped);
    require(dependencies.readResourceCount == readsBeforeDrop);
    dependencies.nativeTextureBottomUp = false;

    /* A texture that does not hold the resource at its own size is read back
     * instead of shown. */
    const auto presentsBeforeMismatch = dependencies.directPresentationCount;
    dependencies.nativeTextures[resource.resourceId].height += 1U;
    requireOk(device, requestBytes(flush));
    require(dependencies.directPresentationCount == presentsBeforeMismatch);
    require(dependencies.readResourceCount == readsBeforeDrop + 1U);
    dependencies.nativeTextures[resource.resourceId].height -= 1U;

    /* An unsupported scanout still takes the readback path, and the display is
     * told so. */
    dependencies.directPresentationEnabled = false;
    const auto fallbacksBeforeRefusal = dependencies.cpuFallbackFrameCount;
    requireOk(device, requestBytes(flush));
    require(dependencies.cpuFallbackFrameCount == fallbacksBeforeRefusal + 1);

    scanout.resourceId = protocol::invalidResourceId;
    const auto clearsBeforeDisable = dependencies.clearCount;
    const auto disabled = failEachAllocationBeforeCommit(
        device, requestBytes(scanout),
        [&] { return dependencies.clearCount == clearsBeforeDisable; });
    require(responseType(disabled.response) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.clearCount == clearsBeforeDisable + 1);

    scanout.resourceId = resource.resourceId;
    requireOk(device, requestBytes(scanout));
    auto unref = newRequest<protocol::GpuResourceReference>(
        GpuCommand::resourceUnref);
    unref.resourceId = resource.resourceId;
    const auto clearsBeforeUnref = dependencies.clearCount;
    requireOk(device, requestBytes(unref));
    require(dependencies.clearCount == clearsBeforeUnref + 1);
}

void softwareScanoutProgressesWhileRendererFenceIsPending()
{
    const std::array<std::uint8_t, 4> pixel {11, 22, 33, 255};
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {
        dependencies,
        [&](std::uint64_t address, std::span<std::uint8_t> destination) {
            if (address != 0x1000 || destination.size() != pixel.size())
                return false;
            std::copy(pixel.begin(), pixel.end(), destination.begin());
            return true;
        },
        noGuestMappings};

    const auto context = createContext(811, protocol::venusCapsetId);
    const auto software = create2d(812, 1, 1);
    const auto rendered = create3d(813, 1, 1);
    requireOk(device, requestBytes(context));
    requireOk(device, requestBytes(software));
    requireOk(device, requestBytes(rendered));

    requireOk(device, attachBackingRequest(software.resourceId, pixel.size()));
    auto scanout = newRequest<protocol::GpuSetScanout>(GpuCommand::setScanout);
    scanout.rectangle = {.x = 0, .y = 0, .width = 1, .height = 1};
    scanout.resourceId = software.resourceId;
    requireOk(device, requestBytes(scanout));

    auto waitSubmit = newRequest<protocol::GpuSubmit3d>(GpuCommand::submit3d);
    waitSubmit.header.contextId = context.header.contextId;
    waitSubmit.header.flags =
        protocol::gpuFlagFence | protocol::gpuFlagInfoRingIndex;
    waitSubmit.header.ringIndex = 1;
    const auto pending = device.processWithDeferredFence(
        requestBytes(waitSubmit), false, 41, headerReplyCapacity);
    require(pending.fence.has_value());
    require(device.contextFenceStatus(*pending.fence) == EBUSY);

    /* A console update can precede the submit carrying the host semaphore
     * signal. Both software commands must pass the transport's production
     * predicate without retiring the renderer fence. */
    auto transfer = newRequest<protocol::GpuTransferToHost2d>(
        GpuCommand::transferToHost2d);
    transfer.rectangle = scanout.rectangle;
    transfer.resourceId = software.resourceId;
    require(device.canProcessWhileFencesPending(requestBytes(transfer), false));
    requireOk(device, requestBytes(transfer));
    auto flush = newRequest<protocol::GpuResourceFlush>(
        GpuCommand::resourceFlush);
    flush.rectangle = scanout.rectangle;
    flush.resourceId = software.resourceId;
    require(device.canProcessWhileFencesPending(requestBytes(flush), false));
    requireOk(device, requestBytes(flush));
    require(std::equal(
        pixel.begin(), pixel.end(), dependencies.framebuffer.begin()));
    require(device.contextFenceStatus(*pending.fence) == EBUSY);

    /* The same opcode must not admit renderer readback or unknown resources.
     * A software transfer cannot be used to access a renderer resource. */
    flush.resourceId = rendered.resourceId;
    require(!device.canProcessWhileFencesPending(requestBytes(flush), false));
    flush.resourceId = 9999;
    require(!device.canProcessWhileFencesPending(requestBytes(flush), false));
    require(!device.canProcessWhileFencesPending(
        requestBytes(flush).first(sizeof(protocol::GpuControlHeader)), false));
    transfer.resourceId = rendered.resourceId;
    requireResponse(device, requestBytes(transfer),
        protocol::GpuResponse::errorInvalidResource);
    require(dependencies.transferCount == 0);
    for (const auto command : {
             protocol::GpuCommand::resourceUnref,
             protocol::GpuCommand::resourceDetachBacking,
             protocol::GpuCommand::contextDetachResource,
             protocol::GpuCommand::contextDestroy,
         }) {
        auto mutation = newRequest<protocol::GpuResourceReference>(command);
        mutation.header.contextId = context.header.contextId;
        mutation.resourceId = rendered.resourceId;
        require(!device.canProcessWhileFencesPending(
            requestBytes(mutation), false));
    }
    auto signalSubmit = waitSubmit;
    signalSubmit.header.flags = 0;
    signalSubmit.header.ringIndex = 0;
    require(device.canProcessWhileFencesPending(
        requestBytes(signalSubmit), false));
    requireOk(device, requestBytes(signalSubmit));
    require(dependencies.submitCount == 2);
    dependencies.retireContextFence(context.header.contextId, 1, 41);
    require(device.pollContextFences(context.header.contextId) == 0);
    require(device.contextFenceStatus(*pending.fence) == 0);

    /* Classification follows current ownership when an ID is recycled. */
    auto unref = newRequest<protocol::GpuResourceReference>(
        GpuCommand::resourceUnref);
    unref.resourceId = software.resourceId;
    requireOk(device, requestBytes(unref));
    flush.resourceId = software.resourceId;
    require(!device.canProcessWhileFencesPending(requestBytes(flush), false));
    const auto replacement = create3d(software.resourceId, 1, 1);
    requireOk(device, requestBytes(replacement));
    require(!device.canProcessWhileFencesPending(requestBytes(flush), false));
}

void deferredFencesUseHostTimelineAndMergedRetirement()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {dependencies, noGuestReads, noGuestMappings};

    const auto firstContext = createContext(801);
    const auto secondContext = createContext(802);
    requireOk(device, requestBytes(firstContext));
    requireOk(device, requestBytes(secondContext));

    constexpr std::array<std::uint64_t, 3> guestFenceIds {900, 2, 900};
    constexpr std::array<std::uint64_t, 3> hostFenceCookies {11, 12, 13};
    for (std::size_t index = 0; index < guestFenceIds.size(); ++index) {
        auto submit = newRequest<protocol::GpuSubmit3d>(GpuCommand::submit3d);
        submit.header.flags = protocol::gpuFlagFence;
        submit.header.contextId = firstContext.header.contextId;
        submit.header.fenceId = guestFenceIds[index];
        const auto result = device.processWithDeferredFence(
            requestBytes(submit),
            false,
            hostFenceCookies[index],
            headerReplyCapacity);
        require(result.fence.has_value());
        require(result.fence->contextId == firstContext.header.contextId);
        require(result.fence->ringIndex == 0);
        require(result.fence->cookie == hostFenceCookies[index]);
        const auto reply = responseHeader(result.response);
        require(reply.type == static_cast<std::uint32_t>(
            protocol::GpuResponse::okNoData));
        require(reply.fenceId == guestFenceIds[index]);
        require((reply.flags & protocol::gpuFlagFence) != 0);
    }
    require(dependencies.contextFenceCookies.size() == hostFenceCookies.size());
    require(std::ranges::equal(
        dependencies.contextFenceCookies, hostFenceCookies));

    dependencies.retireContextFence(
        firstContext.header.contextId, 0, hostFenceCookies.back());
    require(device.pollContextFences(firstContext.header.contextId) == 0);
    for (const auto cookie : hostFenceCookies) {
        require(device.contextFenceStatus({
                    .contextId = firstContext.header.contextId,
                    .ringIndex = 0,
                    .cookie = cookie,
                }) == 0);
    }

    auto otherSubmit = newRequest<protocol::GpuSubmit3d>(GpuCommand::submit3d);
    otherSubmit.header.flags = protocol::gpuFlagFence;
    otherSubmit.header.contextId = secondContext.header.contextId;
    otherSubmit.header.fenceId = 1;
    const auto otherResult = device.processWithDeferredFence(
        requestBytes(otherSubmit), false, 20, headerReplyCapacity);
    require(otherResult.fence.has_value());
    require(device.contextFenceStatus(*otherResult.fence) == EBUSY);
    dependencies.retireContextFence(secondContext.header.contextId, 0, 20);
    require(device.contextFenceStatus(*otherResult.fence) == 0);

    auto nonzeroRing = newRequest<protocol::GpuSubmit3d>(GpuCommand::submit3d);
    nonzeroRing.header.flags =
        protocol::gpuFlagFence | protocol::gpuFlagInfoRingIndex;
    nonzeroRing.header.contextId = firstContext.header.contextId;
    nonzeroRing.header.ringIndex = 1;
    nonzeroRing.header.fenceId = 7;
    const auto synchronous = device.processWithDeferredFence(
        requestBytes(nonzeroRing), false, 21, headerReplyCapacity);
    require(!synchronous.fence.has_value());
    require(responseType(synchronous.response) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.fenceCount == 1);

    auto failedFence = newRequest<protocol::GpuSubmit3d>(GpuCommand::submit3d);
    failedFence.header.flags = protocol::gpuFlagFence;
    failedFence.header.contextId = firstContext.header.contextId;
    dependencies.rejectNext(FakeGpuDependencies::Operation::fence);
    const auto submitsBeforeFailure = dependencies.submitCount;
    const auto failure = device.processWithDeferredFence(
        requestBytes(failedFence), false, 22, headerReplyCapacity);
    require(!failure.fence.has_value());
    require(responseType(failure.response) ==
        protocol::GpuResponse::errorUnspecified);
    require(dependencies.submitCount == submitsBeforeFailure + 1);
}

void resetRetiresEachExternalOwnerExactlyOnce()
{
    std::array<std::uint8_t, 64> guestMemory {};
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {
        dependencies, noGuestReads, guestMemoryAt0x1000(guestMemory)};

    const auto context = createContext(801);
    const auto first = create3d(802);
    const auto second = create3d(803);
    requireOk(device, requestBytes(context));
    requireOk(device, requestBytes(first));
    requireOk(device, requestBytes(second));

    requireOk(device, attachBackingRequest(
        first.resourceId, guestMemory.size()));

    device.reset();
    require(dependencies.detachBackingCount == 1);
    require(dependencies.unrefResourceCount == 2);
    require(dependencies.destroyContextCount == 1);
    require(dependencies.resetFenceCount == 1);
    require(dependencies.clearCount == 1);
    require(dependencies.contexts.empty());
    require(dependencies.resources.empty());
    require(dependencies.attachedBackingAddress == nullptr);

    device.reset();
    require(dependencies.detachBackingCount == 1);
    require(dependencies.unrefResourceCount == 2);
    require(dependencies.destroyContextCount == 1);
    require(dependencies.resetFenceCount == 2);
    require(dependencies.clearCount == 2);

    requireOk(device, requestBytes(context));
    requireOk(device, requestBytes(first));
}

void resetIsPristineAndAllocationRetryIsAtomic()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {dependencies, noGuestReads, noGuestMappings};

    auto context = createContext(41);
    requireOk(device, requestBytes(context));

    auto resource = create2d(73);
    requireOk(device, requestBytes(resource));

    device.reset();
    device.reset();
    requireOk(device, requestBytes(context));
    requireOk(device, requestBytes(resource));

    device.reset();
    context.header.contextId = 42;
    const auto contextsBefore = dependencies.createContextCount;
    const auto createdContext = failEachAllocationBeforeCommit(
        device, requestBytes(context),
        [&] { return dependencies.createContextCount == contextsBefore; });
    /* The reply's bad_alloc leaves process(). */
    require(!createdContext.failures.front().has_value());
    require(responseType(createdContext.response) ==
        protocol::GpuResponse::okNoData);

    resource.resourceId = 74;
    const auto createdResource =
        failEachAllocationBeforeCommit(device, requestBytes(resource));
    for (const auto failure : createdResource.failures)
        require(failure == protocol::GpuResponse::errorOutOfMemory);
    require(responseType(createdResource.response) ==
        protocol::GpuResponse::okNoData);

    device.reset();
    const auto overflowingResource =
        create2d(76, UINT32_MAX, UINT32_MAX);
    requireResponse(device, requestBytes(overflowingResource),
        protocol::GpuResponse::errorOutOfMemory);
    const auto overflowRetry = create2d(76);
    requireOk(device, requestBytes(overflowRetry));
}

void rendererBackingAttachIsRetryableAfterAllocationFailure()
{
    std::array<std::uint8_t, 64> guestMemory {};
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {
        dependencies, noGuestReads, guestMemoryAt0x1000(guestMemory)};

    const auto create = create3d(75);
    requireOk(device, requestBytes(create));
    const auto request = attachBackingRequest(
        create.resourceId, guestMemory.size());

    // The response is allocated first; the backing's iovec array second.
    fail_after_allocations(1);
    requireThrows<std::bad_alloc>(
        [&] { (void)device.process(request, false); });
    (void)cancel_pending_allocation_failure();
    requireOk(device, request);
}

void backingRangesAndAttachStateAreValidated()
{
    std::array<std::uint8_t, 64> guestMemory {};
    unsigned mapperCalls = 0;
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {
        dependencies,
        noGuestReads,
        [&](std::uint64_t address, std::size_t size) {
            ++mapperCalls;
            if (address != 0x1000 || size > guestMemory.size())
                return std::span<std::uint8_t> {};
            return std::span<std::uint8_t> {guestMemory}.first(size);
        }};

    const auto software = create2d(81);
    const auto rendered = create3d(82);
    requireOk(device, requestBytes(software));
    requireOk(device, requestBytes(rendered));

    for (const auto resourceId : {software.resourceId, rendered.resourceId}) {
        auto attach = newRequest<protocol::GpuAttachBacking>(
            GpuCommand::resourceAttachBacking);
        attach.resourceId = resourceId;
        attach.entryCount = 0;
        requireResponse(device, requestBytes(attach),
            protocol::GpuResponse::errorInvalidParameter);

        attach.entryCount = 1;
        const protocol::GpuMemoryEntry overflow {
            .address = std::numeric_limits<std::uint64_t>::max() - 31U,
            .length = 64,
        };
        requireResponse(device, requestWithEntry(attach, overflow),
            protocol::GpuResponse::errorInvalidParameter);

        const protocol::GpuMemoryEntry valid {
            .address = 0x1000,
            .length = static_cast<std::uint32_t>(guestMemory.size()),
        };
        const auto request = requestWithEntry(attach, valid);
        requireOk(device, request);
        requireResponse(device, request,
            protocol::GpuResponse::errorInvalidParameter);

        auto detach = newRequest<protocol::GpuResourceReference>(
            GpuCommand::resourceDetachBacking);
        detach.resourceId = resourceId;
        requireOk(device, requestBytes(detach));
        requireOk(device, request);
    }
    // Only the rendered resource asks the mapper for valid backing. Invalid
    // wrapping ranges are rejected before the callback.
    require(mapperCalls == 2);
}

void failedTransferDoesNotPartiallyCommitPixels()
{
    std::array<std::uint8_t, 16> backing {
        1, 2, 3, 4, 5, 6, 7, 8,
        9, 10, 11, 12, 13, 14, 15, 16,
    };
    unsigned reads = 0;
    bool failSecondRead = true;
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {
        dependencies,
        [&](std::uint64_t address, std::span<std::uint8_t> destination) {
            ++reads;
            if (failSecondRead && reads == 2)
                return false;
            if (address < 0x1000 ||
                address - 0x1000 > backing.size() ||
                destination.size() > backing.size() - (address - 0x1000)) {
                return false;
            }
            std::memcpy(
                destination.data(),
                backing.data() + (address - 0x1000),
                destination.size());
            return true;
        },
        noGuestMappings};

    const auto create = create2d(83, 2, 2);
    requireOk(device, requestBytes(create));
    requireOk(device, attachBackingRequest(create.resourceId, backing.size()));

    auto scanout = newRequest<protocol::GpuSetScanout>(GpuCommand::setScanout);
    scanout.rectangle = {.x = 0, .y = 0, .width = 2, .height = 2};
    scanout.scanoutId = protocol::primaryScanoutId;
    scanout.resourceId = create.resourceId;
    requireOk(device, requestBytes(scanout));

    auto transfer = newRequest<protocol::GpuTransferToHost2d>(
        GpuCommand::transferToHost2d);
    transfer.rectangle = scanout.rectangle;
    transfer.offset = 0;
    transfer.resourceId = create.resourceId;
    /* Each attempt reads the rows only after allocating its reply, and its
     * second read fails. No failed attempt commits a pixel, which the flush
     * below shows. */
    std::vector<unsigned> readsBeforeFailure;
    const auto failedTransfer = failEachAllocationBeforeCommit(
        device, requestBytes(transfer), [&] {
            readsBeforeFailure.push_back(std::exchange(reads, 0U));
            return true;
        });
    require(readsBeforeFailure.front() == 0);
    require(responseType(failedTransfer.response) ==
        protocol::GpuResponse::errorInvalidParameter);

    auto flush = newRequest<protocol::GpuResourceFlush>(
        GpuCommand::resourceFlush);
    flush.rectangle = scanout.rectangle;
    flush.resourceId = create.resourceId;
    requireOk(device, requestBytes(flush));
    auto framebuffer = dependencies.framebuffer;
    for (const auto value : std::span {framebuffer}.first(8U))
        require(value == 0);
    for (const auto value : std::span {framebuffer}.subspan(64U * 4U, 8U))
        require(value == 0);

    reads = 0;
    failSecondRead = false;
    requireOk(device, requestBytes(transfer));

    // Pixel conversion is staged before the live framebuffer is locked and
    // committed. An allocation failure therefore reports OOM without leaving
    // a partially converted display update.
    dependencies.failAllocationNext(
        FakeGpuDependencies::Operation::display_update);
    const auto failedFlush = device.process(requestBytes(flush), false);
    require(responseType(failedFlush) ==
        protocol::GpuResponse::errorOutOfMemory);
    framebuffer = dependencies.framebuffer;
    for (const auto value : std::span {framebuffer}.first(8U))
        require(value == 0);
    for (const auto value : std::span {framebuffer}.subspan(64U * 4U, 8U))
        require(value == 0);

    requireOk(device, requestBytes(flush));
    framebuffer = dependencies.framebuffer;
    require(std::memcmp(framebuffer.data(), backing.data(), 8) == 0);
    require(std::memcmp(
        framebuffer.data() + 64U * 4U, backing.data() + 8, 8) == 0);

    device.reset();
    framebuffer = dependencies.framebuffer;
    require(std::ranges::all_of(framebuffer, [](std::uint8_t value) {
        return value == 0;
    }));
}

void setScanoutReplyFailureDoesNotCommitAllocation()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {dependencies, noGuestReads, noGuestMappings};
    const auto create = create3d(84);
    requireOk(device, requestBytes(create));

    auto scanout = newRequest<protocol::GpuSetScanout>(GpuCommand::setScanout);
    scanout.rectangle = {.x = 0, .y = 0, .width = 4, .height = 4};
    scanout.scanoutId = protocol::primaryScanoutId;
    scanout.resourceId = create.resourceId;

    /* The reply is all a Virgl scanout allocates: its readback staging waits
     * for the first flush that reads back. */
    const auto set = failEachAllocationBeforeCommit(
        device, requestBytes(scanout),
        [&] { return dependencies.guestScanoutChangeCount == 0; });
    require(set.failures.size() == 1);
    require(!set.failures.front().has_value());
    require(responseType(set.response) == protocol::GpuResponse::okNoData);
    require(dependencies.guestScanoutChangeCount == 1);
    require(dependencies.lastGuestScanoutWidth == scanout.rectangle.width);
    require(dependencies.lastGuestScanoutHeight == scanout.rectangle.height);

    /* Resource rebinds at the active geometry are page flips, not guest mode
     * changes, and must not disturb the host-to-guest resize policy. */
    requireOk(device, requestBytes(scanout));
    require(dependencies.guestScanoutChangeCount == 1);

    scanout.rectangle = {.x = 1, .y = 0, .width = 3, .height = 4};
    requireOk(device, requestBytes(scanout));
    require(dependencies.guestScanoutChangeCount == 2);
    require(dependencies.lastGuestScanoutWidth == 3);
    require(dependencies.lastGuestScanoutHeight == 4);

    scanout.resourceId = 999;
    requireResponse(device, requestBytes(scanout),
        protocol::GpuResponse::errorInvalidResource);
    require(dependencies.guestScanoutChangeCount == 2);

    scanout.resourceId = create.resourceId;
    scanout.rectangle = {.x = 0, .y = 0, .width = 2, .height = 4};
    dependencies.guestScanoutChangeAccepted = false;
    requireResponse(device, requestBytes(scanout),
        protocol::GpuResponse::errorOutOfMemory);
    require(dependencies.guestScanoutChangeCount == 3);
    dependencies.guestScanoutChangeAccepted = true;
    requireOk(device, requestBytes(scanout));
    require(dependencies.guestScanoutChangeCount == 4);
    require(dependencies.lastGuestScanoutWidth == 2);
    require(dependencies.lastGuestScanoutHeight == 4);

    device.reset();
    require(dependencies.guestSessionResetCount == 1);
}

/* The guest's display is on while it has a scanout bound: the application
 * waits for an Android screen to go off by asking, and the guest's screen turns
 * off by unbinding the scanout from its disabled CRTC. A bind the device
 * refused is no scanout, and a reset takes it away with the rest. */
void theGuestScanoutFlagFollowsTheScanout()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {dependencies, noGuestReads, noGuestMappings};
    const auto create = create3d(0x77);
    requireOk(device, requestBytes(create));
    require(!dependencies.guestScanoutActive);

    auto scanout = newRequest<protocol::GpuSetScanout>(GpuCommand::setScanout);
    scanout.rectangle = {.x = 0, .y = 0, .width = 4, .height = 4};
    scanout.scanoutId = protocol::primaryScanoutId;
    scanout.resourceId = 999;
    requireResponse(device, requestBytes(scanout),
        protocol::GpuResponse::errorInvalidResource);
    require(!dependencies.guestScanoutActive);

    scanout.resourceId = create.resourceId;
    requireOk(device, requestBytes(scanout));
    require(dependencies.guestScanoutActive);

    /* The screen goes off. */
    scanout.resourceId = protocol::invalidResourceId;
    requireOk(device, requestBytes(scanout));
    require(!dependencies.guestScanoutActive);

    /* A refused rebind leaves the display off. */
    scanout.resourceId = 999;
    requireResponse(device, requestBytes(scanout),
        protocol::GpuResponse::errorInvalidResource);
    require(!dependencies.guestScanoutActive);

    /* It comes on again, and a reset ends it with the connection. */
    scanout.resourceId = create.resourceId;
    requireOk(device, requestBytes(scanout));
    require(dependencies.guestScanoutActive);
    device.reset();
    require(!dependencies.guestScanoutActive);
}

/* A Virgl scanout allocates its readback staging at the first flush that
 * reads back, which is therefore where running out of memory is reported. The
 * staging serves every later readback of that scanout and of page flips at
 * its size, and is released when the scanout changes size or kind. */
void readbackStagingIsAllocatedByTheFirstReadback()
{
    FakeGpuDependencies dependencies;
    dependencies.fullCpuFallback = true;
    VirtioGpuDevice device {dependencies, noGuestReads, noGuestMappings};
    const auto first = create3d(0x901);
    const auto second = create3d(0x902);
    const auto software = create2d(0x903);
    requireOk(device, requestBytes(first));
    requireOk(device, requestBytes(second));
    requireOk(device, requestBytes(software));

    auto scanout = newRequest<protocol::GpuSetScanout>(GpuCommand::setScanout);
    scanout.rectangle = {.x = 0, .y = 0, .width = 4, .height = 4};
    scanout.scanoutId = protocol::primaryScanoutId;
    scanout.resourceId = first.resourceId;
    auto flush = newRequest<protocol::GpuResourceFlush>(
        GpuCommand::resourceFlush);
    flush.rectangle = scanout.rectangle;
    flush.resourceId = first.resourceId;

    const auto setScanout = [&](std::uint32_t resourceId) {
        scanout.resourceId = resourceId;
        flush.resourceId = resourceId;
        flush.rectangle = scanout.rectangle;
        require(responseType(processAllocatingOnlyTheReply(
                    device, requestBytes(scanout))) ==
            protocol::GpuResponse::okNoData);
    };
    // The first readback fails if it cannot allocate the staging, and
    // succeeds once it can. Either way nothing reaches the display early.
    const auto requireReadbackAllocates = [&] {
        const auto reads = dependencies.readResourceCount;
        const auto updates = dependencies.displayUpdateCount;
        fail_after_allocations(1);
        const auto failed = device.process(requestBytes(flush), false);
        (void)cancel_pending_allocation_failure();
        require(responseType(failed) ==
            protocol::GpuResponse::errorOutOfMemory);
        require(dependencies.readResourceCount == reads);
        require(dependencies.displayUpdateCount == updates);
        requireOk(device, requestBytes(flush));
        require(dependencies.readResourceCount == reads + 1U);
        require(dependencies.displayUpdateCount == updates + 1U);
        require(dependencies.lastReadDestinationBytes ==
            static_cast<std::size_t>(scanout.rectangle.width) *
                scanout.rectangle.height * protocol::gpuBytesPerPixel);
        return dependencies.lastReadDestination;
    };
    const auto requireReadbackReuses = [&](const std::uint8_t* staging) {
        const auto reads = dependencies.readResourceCount;
        require(responseType(processAllocatingOnlyTheReply(
                    device, requestBytes(flush))) ==
            protocol::GpuResponse::okNoData);
        require(dependencies.readResourceCount == reads + 1U);
        require(dependencies.lastReadDestination == staging);
    };

    setScanout(first.resourceId);
    const auto* staging = requireReadbackAllocates();
    requireReadbackReuses(staging);
    /* A page flip at the same size keeps it. */
    setScanout(second.resourceId);
    requireReadbackReuses(staging);

    /* A new size releases it, and the next readback allocates it again. */
    scanout.rectangle = {.x = 0, .y = 0, .width = 2, .height = 2};
    setScanout(first.resourceId);
    staging = requireReadbackAllocates();
    requireReadbackReuses(staging);

    /* So does a 2D scanout, which never reads back, at the same size. */
    setScanout(software.resourceId);
    setScanout(first.resourceId);
    staging = requireReadbackAllocates();
    requireReadbackReuses(staging);
}

// A capability query the renderer cannot answer, as when it has lost its GL
// context, fails that query and nothing else. Its exception used to escape
// the device and end the whole vhost-user connection.
void unanswerableCapabilityQueriesFailOnlyThemselves()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {dependencies, noGuestReads, noGuestMappings};
    auto information = newRequest<protocol::GpuGetCapsetInfo>(
        GpuCommand::getCapsetInfo);
    information.capsetIndex = protocol::virgl2CapsetIndex;
    auto capset = newRequest<protocol::GpuGetCapset>(GpuCommand::getCapset);
    capset.capsetId = protocol::virgl2CapsetId;
    capset.version = 1;

    dependencies.failNextCapsets(2);
    requireResponse(device, requestBytes(information),
        protocol::GpuResponse::errorUnspecified);
    requireResponse(device, requestBytes(capset),
        protocol::GpuResponse::errorUnspecified);
    requireResponse(device, requestBytes(information),
        protocol::GpuResponse::okCapsetInfo);
    requireResponse(device, requestBytes(capset),
        protocol::GpuResponse::okCapset);
}

void venusBlobsAndFencesUseStandardWireState()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {dependencies, noGuestReads, noGuestMappings};
    require(device.config().capsetCount == protocol::venusCapsetIndex + 1U);
    auto capsetInfo = newRequest<protocol::GpuGetCapsetInfo>(
        GpuCommand::getCapsetInfo);
    capsetInfo.capsetIndex = protocol::venusCapsetIndex;
    const auto enabledInfo = device.process(requestBytes(capsetInfo), false);
    protocol::GpuCapsetInfo venusInfo {};
    std::memcpy(&venusInfo, enabledInfo.data(), sizeof(venusInfo));
    require(venusInfo.capsetId == protocol::venusCapsetId);
    require(venusInfo.maximumVersion == 0);

    auto context = createContext(0x401, protocol::venusCapsetId);
    requireOk(device, requestBytes(context));

    auto invalidContextFlags = createContext(0x402, protocol::venusCapsetId);
    invalidContextFlags.contextInit |= 1U << 8U;
    requireResponse(device, requestBytes(invalidContextFlags),
        protocol::GpuResponse::errorInvalidParameter);

    auto create = newRequest<protocol::GpuResourceCreateBlob>(
        GpuCommand::resourceCreateBlob);
    create.header.contextId = context.header.contextId;
    create.resourceId = 0x501;
    create.memory = protocol::blobMemoryHost3d;
    create.flags =
        protocol::blobFlagMappable | protocol::blobFlagShareable;
    create.blobId = 0;
    create.size = 4096;

    auto apertureCreate = create;
    apertureCreate.resourceId = 0x500;
    apertureCreate.size = protocol::hostVisibleSharedMemoryRegionSize;
    requireOk(device, requestBytes(apertureCreate));
    /* Unmapped renderer storage does not consume aperture address space. */
    requireOk(device, requestBytes(create));
    auto apertureUnref = newRequest<protocol::GpuResourceReference>(
        GpuCommand::resourceUnref);
    apertureUnref.resourceId = apertureCreate.resourceId;
    requireOk(device, requestBytes(apertureUnref));

    auto crossDeviceCreate = create;
    crossDeviceCreate.resourceId = 0x502;
    crossDeviceCreate.flags |= protocol::blobFlagCrossDevice;
    requireOk(device, requestBytes(crossDeviceCreate));
    auto crossDeviceUnref = newRequest<protocol::GpuResourceReference>(
        GpuCommand::resourceUnref);
    crossDeviceUnref.resourceId = crossDeviceCreate.resourceId;
    requireOk(device, requestBytes(crossDeviceUnref));

    auto invalidCreate = create;
    invalidCreate.resourceId = 0x502;
    invalidCreate.memory = protocol::blobMemoryHost3dGuest;
    requireResponse(device, requestBytes(invalidCreate),
        protocol::GpuResponse::errorInvalidParameter);
    auto rendererBlobCreate = create;
    rendererBlobCreate.resourceId = 0x5ff;
    rendererBlobCreate.blobId = 0x12345678;
    requireOk(device, requestBytes(rendererBlobCreate));
    require(dependencies.lastBlobId == rendererBlobCreate.blobId);
    auto rendererBlobUnref = newRequest<protocol::GpuResourceReference>(
        GpuCommand::resourceUnref);
    rendererBlobUnref.resourceId = rendererBlobCreate.resourceId;
    requireOk(device, requestBytes(rendererBlobUnref));

    /*
     * Virgl contexts own host-visible storage too: coherent GL buffers reach
     * the guest through the same aperture as Venus device memory.
     */
    auto virglContext = createContext(0x403, protocol::virgl2CapsetId);
    requireOk(device, requestBytes(virglContext));
    auto virglBlobCreate = create;
    virglBlobCreate.header.contextId = virglContext.header.contextId;
    virglBlobCreate.resourceId = 0x5fe;
    requireOk(device, requestBytes(virglBlobCreate));
    auto virglBlobMap = newRequest<protocol::GpuResourceMapBlob>(
        GpuCommand::resourceMapBlob);
    virglBlobMap.resourceId = virglBlobCreate.resourceId;
    virglBlobMap.offset = 0;
    requireResponse(device, requestBytes(virglBlobMap),
        protocol::GpuResponse::okMapInfo);
    completeAction(device);
    auto virglBlobUnmap = newRequest<protocol::GpuResourceUnmapBlob>(
        GpuCommand::resourceUnmapBlob);
    virglBlobUnmap.resourceId = virglBlobCreate.resourceId;
    requireOk(device, requestBytes(virglBlobUnmap));
    completeAction(device);
    auto virglBlobUnref = newRequest<protocol::GpuResourceReference>(
        GpuCommand::resourceUnref);
    virglBlobUnref.resourceId = virglBlobCreate.resourceId;
    requireOk(device, requestBytes(virglBlobUnref));

    auto map = newRequest<protocol::GpuResourceMapBlob>(
        GpuCommand::resourceMapBlob);
    map.resourceId = create.resourceId;
    map.offset = protocol::hostVisibleSharedMemoryAlignment;
    const auto mapResponse = device.process(requestBytes(map), false);
    require(responseType(mapResponse) == protocol::GpuResponse::okMapInfo);
    protocol::GpuMapInfo mapInfoResponse {};
    std::memcpy(
        &mapInfoResponse, mapResponse.data(), sizeof(mapInfoResponse));
    require(mapInfoResponse.mapInfo == protocol::mapCacheCached);
    auto mapAction = device.takeSharedMemoryAction();
    require(mapAction.has_value());
    require(mapAction->type ==
        VirtioGpuDevice::SharedMemoryAction::Type::map);
    require(mapAction->descriptor.get() >= 0);
    require(mapAction->regionId ==
        protocol::hostVisibleSharedMemoryRegionId);
    require(mapAction->regionOffset == map.offset);
    require(mapAction->length ==
        protocol::hostVisibleSharedMemoryAlignment);

    auto unmap = newRequest<protocol::GpuResourceUnmapBlob>(
        GpuCommand::resourceUnmapBlob);
    unmap.resourceId = create.resourceId;
    requireResponse(device, requestBytes(unmap),
        protocol::GpuResponse::errorInvalidParameter);
    device.completeSharedMemoryAction(mapAction->actionId, true);

    auto secondCreate = create;
    secondCreate.resourceId = 0x502;
    requireOk(device, requestBytes(secondCreate));
    auto secondMap = map;
    secondMap.resourceId = secondCreate.resourceId;
    requireResponse(device, requestBytes(secondMap),
        protocol::GpuResponse::errorInvalidParameter);
    secondMap.offset += protocol::hostVisibleSharedMemoryAlignment;
    requireResponse(device, requestBytes(secondMap),
        protocol::GpuResponse::okMapInfo);
    completeAction(device, false);
    requireResponse(device, requestBytes(secondMap),
        protocol::GpuResponse::okMapInfo);
    completeAction(device);

    auto secondUnmap = unmap;
    secondUnmap.resourceId = secondCreate.resourceId;
    requireOk(device, requestBytes(secondUnmap));
    completeAction(device);

    requireOk(device, requestBytes(unmap));
    auto unmapAction = device.takeSharedMemoryAction();
    require(unmapAction.has_value());
    require(unmapAction->type ==
        VirtioGpuDevice::SharedMemoryAction::Type::unmap);
    require(unmapAction->descriptor.get() < 0);
    device.completeSharedMemoryAction(unmapAction->actionId, false);
    requireOk(device, requestBytes(unmap));
    completeAction(device);

    auto submit = newRequest<protocol::GpuSubmit3d>(GpuCommand::submit3d);
    submit.header.contextId = context.header.contextId;
    submit.header.flags =
        protocol::gpuFlagFence | protocol::gpuFlagInfoRingIndex;
    submit.header.ringIndex = 7;
    submit.header.fenceId = 0x1234;
    constexpr std::uint64_t venusFenceCookie = 0x5aa5;
    const auto deferred = device.processWithDeferredFence(
        requestBytes(submit), false, venusFenceCookie, headerReplyCapacity);
    require(responseType(deferred.response) ==
        protocol::GpuResponse::okNoData);
    require(deferred.fence.has_value());
    require(deferred.fence->contextId == context.header.contextId);
    require(deferred.fence->ringIndex == submit.header.ringIndex);
    require(deferred.fence->cookie == venusFenceCookie);
    require(device.contextFenceStatus(*deferred.fence) == EBUSY);
    dependencies.retireContextFence(
        context.header.contextId, submit.header.ringIndex, venusFenceCookie);
    require(device.pollContextFences(deferred.fence->contextId) == 0);
    require(device.contextFenceStatus(*deferred.fence) == 0);
    /* Renderer work with no fence of its own behind it, such as a query a
     * guest is blocked on, is reported and completed by a renderer-wide
     * poll rather than by polling a context. */
    require(!device.rendererHasPendingWork());
    dependencies.rendererPendingWork = true;
    require(device.rendererHasPendingWork());
    require(device.pollRenderer() == 0);
    require(dependencies.rendererPollCount == 1);
    dependencies.rendererPendingWork = false;
    /* The service asks the renderer when a quiet context could be trimmed and
     * calls it when that time has come; the device only forwards both. */
    require(!device.nextIdleTrimDelay().has_value());
    dependencies.idleTrimDelay = std::chrono::milliseconds(250);
    require(device.nextIdleTrimDelay() == std::chrono::milliseconds(250));
    device.trimIdleContexts();
    require(dependencies.idleTrimCount == 1);

    auto invalidRingSubmit = submit;
    invalidRingSubmit.header.ringIndex =
        protocol::venusContextRingCount;
    requireResponse(device, requestBytes(invalidRingSubmit),
        protocol::GpuResponse::errorInvalidParameter);

    auto unref = newRequest<protocol::GpuResourceReference>(
        GpuCommand::resourceUnref);
    unref.resourceId = secondCreate.resourceId;
    requireOk(device, requestBytes(unref));

    requireResponse(device, requestBytes(map),
        protocol::GpuResponse::okMapInfo);
    completeAction(device);
    unref.resourceId = create.resourceId;
    requireOk(device, requestBytes(unref));
    unmapAction = device.takeSharedMemoryAction();
    require(unmapAction.has_value());
    require(unmapAction->type ==
        VirtioGpuDevice::SharedMemoryAction::Type::unmap);
    device.completeSharedMemoryAction(unmapAction->actionId, true);

    auto resetCreate = create;
    resetCreate.resourceId = 0x503;
    requireOk(device, requestBytes(resetCreate));
    auto resetMap = map;
    resetMap.resourceId = resetCreate.resourceId;
    requireResponse(device, requestBytes(resetMap),
        protocol::GpuResponse::okMapInfo);
    completeAction(device);
    device.prepareReset();
    auto resetUnmapAction = device.takeSharedMemoryAction();
    require(resetUnmapAction.has_value());
    require(resetUnmapAction->type ==
        VirtioGpuDevice::SharedMemoryAction::Type::unmap);
    device.completeSharedMemoryAction(resetUnmapAction->actionId, true);
    device.reset();
}

void blobMappingCapacityFollowsTheAperture()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {dependencies, noGuestReads, noGuestMappings};
    const auto context = createContext(0x601, protocol::venusCapsetId);
    requireOk(device, requestBytes(context));

    auto create = newRequest<protocol::GpuResourceCreateBlob>(
        GpuCommand::resourceCreateBlob);
    create.header.contextId = context.header.contextId;
    create.memory = protocol::blobMemoryHost3d;
    create.flags = protocol::blobFlagMappable;
    create.size = protocol::hostVisibleSharedMemoryAlignment;
    auto map = newRequest<protocol::GpuResourceMapBlob>(
        GpuCommand::resourceMapBlob);

    // Small mappings must not exhaust a record-count limit before the aperture.
    constexpr std::uint32_t smallMappings = 1025;
    for (std::uint32_t index = 0; index < smallMappings; ++index) {
        create.resourceId = index + 1U;
        requireOk(device, requestBytes(create));
        map.resourceId = create.resourceId;
        map.offset = index * protocol::hostVisibleSharedMemoryAlignment;
        requireResponse(device, requestBytes(map),
            protocol::GpuResponse::okMapInfo);
        auto action = device.takeSharedMemoryAction();
        require(action.has_value());
        require(action->regionOffset == map.offset);
        require(action->length == create.size);
        device.completeSharedMemoryAction(action->actionId, true);
    }

    // Fill the rest with one range, then preserve overlap and bounds checks.
    create.resourceId = smallMappings + 1U;
    map.resourceId = create.resourceId;
    map.offset = smallMappings * protocol::hostVisibleSharedMemoryAlignment;
    create.size = protocol::hostVisibleSharedMemoryRegionSize - map.offset;
    requireOk(device, requestBytes(create));
    requireResponse(device, requestBytes(map),
        protocol::GpuResponse::okMapInfo);
    completeAction(device);

    ++create.resourceId;
    create.size = protocol::hostVisibleSharedMemoryAlignment;
    requireOk(device, requestBytes(create));
    map.resourceId = create.resourceId;
    map.offset = 0;
    requireResponse(device, requestBytes(map),
        protocol::GpuResponse::errorInvalidParameter);
    map.offset = protocol::hostVisibleSharedMemoryRegionSize;
    requireResponse(device, requestBytes(map),
        protocol::GpuResponse::errorInvalidParameter);

    auto unmap = newRequest<protocol::GpuResourceUnmapBlob>(
        GpuCommand::resourceUnmapBlob);
    unmap.resourceId = 1;
    requireOk(device, requestBytes(unmap));
    completeAction(device);
    map.offset = 0;
    requireResponse(device, requestBytes(map),
        protocol::GpuResponse::okMapInfo);
    completeAction(device);
    device.prepareReset();
    std::uint32_t unmapped = 0;
    while (auto pending = device.takeSharedMemoryAction()) {
        require(pending->type == VirtioGpuDevice::SharedMemoryAction::Type::unmap);
        device.completeSharedMemoryAction(pending->actionId, true);
        ++unmapped;
    }
    require(unmapped == smallMappings + 1U);
    device.reset();
    require(dependencies.resources.empty());
}

void blobScanoutUsesNativeTextureAndPersistentVncMapping()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice empty {dependencies, noGuestReads, noGuestMappings};
    auto scanout = newRequest<protocol::GpuSetScanoutBlob>(
        GpuCommand::setScanoutBlob);
    scanout.rectangle = {.width = 8, .height = 8};
    scanout.scanoutId = protocol::primaryScanoutId;
    scanout.resourceId = 0x702;
    scanout.width = 8;
    scanout.height = 8;
    scanout.format = static_cast<std::uint32_t>(
        protocol::GpuFormat::b8g8r8a8Unorm);
    scanout.strides[0] = 8U * protocol::gpuBytesPerPixel;
    /* A blob scanout must name a blob the device already holds. */
    require(responseType(empty.process(requestBytes(scanout), false)) !=
        protocol::GpuResponse::okNoData);

    VirtioGpuDevice device {dependencies, noGuestReads, noGuestMappings};
    const auto context = createContext(0x701, protocol::venusCapsetId);
    requireOk(device, requestBytes(context));
    auto create = newRequest<protocol::GpuResourceCreateBlob>(
        GpuCommand::resourceCreateBlob);
    create.header.contextId = context.header.contextId;
    create.resourceId = scanout.resourceId;
    create.memory = protocol::blobMemoryHost3d;
    create.flags =
        protocol::blobFlagShareable | protocol::blobFlagCrossDevice;
    create.blobId = 0x1234;
    create.size = dependencies.blobPixels.size();
    requireOk(device, requestBytes(create));

    auto invalid = scanout;
    invalid.strides[0]--;
    requireResponse(device, requestBytes(invalid),
        protocol::GpuResponse::errorInvalidParameter);
    invalid = scanout;
    invalid.strides[1] = 1;
    requireResponse(device, requestBytes(invalid),
        protocol::GpuResponse::errorInvalidParameter);
    invalid = scanout;
    invalid.offsets[0] = static_cast<std::uint32_t>(create.size - 1U);
    requireResponse(device, requestBytes(invalid),
        protocol::GpuResponse::errorInvalidParameter);
    require(dependencies.createBlobScanoutCount == 0);

    const auto blobScanout = failEachAllocationBeforeCommit(
        device, requestBytes(scanout), [&] {
            return dependencies.createBlobScanoutCount == 0 &&
                dependencies.guestScanoutChangeCount == 0;
        });
    require(responseType(blobScanout.response) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.createBlobScanoutCount == 1);
    /* The scanout rectangle is the guest's mode on the native path too. */
    require(dependencies.guestScanoutChangeCount == 1);
    require(dependencies.lastGuestScanoutWidth == scanout.rectangle.width);
    require(dependencies.lastGuestScanoutHeight == scanout.rectangle.height);
    require(dependencies.lastTextureResource == scanout.resourceId);
    /* A blob scanout turns the guest's display on as well. */
    require(dependencies.guestScanoutActive);
    require(dependencies.lastBlobScanoutWidth == scanout.width);
    require(dependencies.lastBlobScanoutHeight == scanout.height);
    require(dependencies.lastBlobScanoutFormat == scanout.format);
    require(dependencies.lastBlobScanoutStride == scanout.strides[0]);
    require(dependencies.lastBlobScanoutOffset == scanout.offsets[0]);

    auto flush = newRequest<protocol::GpuResourceFlush>(
        GpuCommand::resourceFlush);
    flush.rectangle = scanout.rectangle;
    flush.resourceId = scanout.resourceId;
    dependencies.directPresentationEnabled = true;
    requireOk(device, requestBytes(flush));
    require(dependencies.directPresentationCount == 1);
    require(dependencies.lastMetalScanout.texture ==
        dependencies.blobScanoutTexture);
    require(dependencies.lastMetalScanout.damageX == flush.rectangle.x);
    require(dependencies.lastMetalScanout.damageY == flush.rectangle.y);
    require(dependencies.lastMetalScanout.damageWidth == flush.rectangle.width);
    require(
        dependencies.lastMetalScanout.damageHeight == flush.rectangle.height);
    require(dependencies.mapResourceCount == 0);
    require(dependencies.displayUpdateCount == 0);

    dependencies.directPresentationEnabled = false;
    std::fill(
        dependencies.blobPixels.begin(), dependencies.blobPixels.end(), 0x6b);
    requireOk(device, requestBytes(flush));
    require(dependencies.mapResourceCount == 1);
    require(dependencies.displayUpdateCount == 1);
    require(dependencies.framebuffer[0] == 0x6b);
    requireOk(device, requestBytes(flush));
    require(dependencies.mapResourceCount == 1);
    require(dependencies.displayUpdateCount == 2);

    dependencies.rejectNext(FakeGpuDependencies::Operation::blob_scanout);
    requireResponse(device, requestBytes(scanout),
        protocol::GpuResponse::errorInvalidParameter);
    require(dependencies.releaseBlobScanoutCount == 0);
    require(dependencies.unmapResourceCount == 0);

    auto disable = scanout;
    disable.resourceId = protocol::invalidResourceId;
    requireOk(device, requestBytes(disable));
    require(dependencies.releaseBlobScanoutCount == 1);
    require(dependencies.lastReleasedBlobScanout ==
        dependencies.blobScanoutTexture);
    require(dependencies.unmapResourceCount == 1);
    require(dependencies.lastUnmappedResource == scanout.resourceId);
    require(dependencies.guestScanoutChangeCount == 1);

    /* A display that cannot take the mode refuses the scanout, and the
     * device keeps no texture for it. */
    dependencies.guestScanoutChangeAccepted = false;
    requireResponse(device, requestBytes(scanout),
        protocol::GpuResponse::errorOutOfMemory);
    require(dependencies.guestScanoutChangeCount == 2);
    require(dependencies.releaseBlobScanoutCount == 2);
    require(dependencies.lastReleasedBlobScanout ==
        dependencies.blobScanoutTexture);
    dependencies.guestScanoutChangeAccepted = true;
    requireOk(device, requestBytes(scanout));
    require(dependencies.guestScanoutChangeCount == 3);
    require(dependencies.releaseBlobScanoutCount == 2);
}

void virglBlobScanoutPresentsItsGlTexture()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {dependencies, noGuestReads, noGuestMappings};
    const auto context = createContext(0x801, protocol::virgl2CapsetId);
    requireOk(device, requestBytes(context));
    auto create = newRequest<protocol::GpuResourceCreateBlob>(
        GpuCommand::resourceCreateBlob);
    create.header.contextId = context.header.contextId;
    create.resourceId = 0x802;
    create.memory = protocol::blobMemoryHost3d;
    create.flags = protocol::blobFlagShareable;
    create.blobId = 0x2345;
    create.size = dependencies.blobPixels.size();
    requireOk(device, requestBytes(create));

    auto scanout = newRequest<protocol::GpuSetScanoutBlob>(
        GpuCommand::setScanoutBlob);
    scanout.rectangle = {.width = 8, .height = 8};
    scanout.scanoutId = protocol::primaryScanoutId;
    scanout.resourceId = create.resourceId;
    scanout.width = 8;
    scanout.height = 8;
    scanout.format = static_cast<std::uint32_t>(
        protocol::GpuFormat::r8g8b8x8Unorm);
    scanout.strides[0] = 8U * protocol::gpuBytesPerPixel;

    /* A Virgl context's blob is a GL texture with no scanout texture of its
     * own, the framebuffer Android's gralloc allocates once blobs exist. */
    dependencies.rejectNext(FakeGpuDependencies::Operation::blob_scanout);
    const auto blobScanout = failEachAllocationBeforeCommit(
        device, requestBytes(scanout), [&] {
            return dependencies.createBlobScanoutCount == 0 &&
                dependencies.guestScanoutChangeCount == 0;
        });
    require(responseType(blobScanout.response) ==
        protocol::GpuResponse::okNoData);
    require(dependencies.createBlobScanoutCount == 1);
    require(dependencies.guestScanoutChangeCount == 1);
    require(dependencies.lastGuestScanoutWidth == scanout.rectangle.width);
    require(dependencies.lastGuestScanoutHeight == scanout.rectangle.height);

    auto flush = newRequest<protocol::GpuResourceFlush>(
        GpuCommand::resourceFlush);
    flush.rectangle = {.x = 2, .y = 3, .width = 4, .height = 2};
    flush.resourceId = scanout.resourceId;
    dependencies.directPresentationEnabled = true;
    requireOk(device, requestBytes(flush));
    require(dependencies.directPresentationCount == 0);
    require(dependencies.mapResourceCount == 0);
    require(dependencies.cpuFallbackFrameCount == 1);
    require(dependencies.readResourceCount == 1);
    require(dependencies.lastReadResource == scanout.resourceId);
    require(dependencies.lastReadX == flush.rectangle.x);
    require(dependencies.lastReadY == flush.rectangle.y);
    require(dependencies.lastReadWidth == flush.rectangle.width);
    require(dependencies.lastReadHeight == flush.rectangle.height);
    require(dependencies.displayUpdateCount == 1);
    require(dependencies.lastDisplayDestinationX == flush.rectangle.x);
    require(dependencies.lastDisplayDestinationY == flush.rectangle.y);
    const auto updated =
        (static_cast<std::size_t>(flush.rectangle.y) *
                dependencies.displayWidth +
            flush.rectangle.x) *
        protocol::gpuBytesPerPixel;
    require(dependencies.framebuffer[updated + 2U] ==
        dependencies.readResourceTag);

    /* A client that needs a whole frame gets the whole scanout. */
    dependencies.fullCpuFallback = true;
    requireOk(device, requestBytes(flush));
    require(dependencies.readResourceCount == 2);
    require(dependencies.lastReadX == 0);
    require(dependencies.lastReadY == 0);
    require(dependencies.lastReadWidth == scanout.rectangle.width);
    require(dependencies.lastReadHeight == scanout.rectangle.height);
    dependencies.fullCpuFallback = false;

    /* The Metal texture that stores the GL texture is presented directly
     * when it holds exactly this scanout. Alpha and padding variants of one
     * channel order store the same bytes. */
    const auto blobTexture =
        reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x5678));
    dependencies.nativeResourceAvailable = true;
    dependencies.blobNativeTexture = VirglRenderer::NativeTexture {
        .texture = blobTexture,
        .format = static_cast<std::uint32_t>(
            protocol::GpuFormat::r8g8b8a8Unorm),
        .width = scanout.width,
        .height = scanout.height,
    };
    const auto readsBeforeTexture = dependencies.readResourceCount;
    const auto fallbacksBeforeTexture = dependencies.cpuFallbackFrameCount;
    requireOk(device, requestBytes(flush));
    require(dependencies.directPresentationCount == 1);
    require(dependencies.lastTextureResource == scanout.resourceId);
    require(dependencies.lastMetalScanout.texture == blobTexture);
    require(dependencies.lastMetalScanout.sourceWidth == scanout.width);
    require(dependencies.lastMetalScanout.sourceHeight == scanout.height);
    require(dependencies.lastMetalScanout.width == scanout.rectangle.width);
    require(dependencies.lastMetalScanout.height == scanout.rectangle.height);
    require(dependencies.lastMetalScanout.damageX == flush.rectangle.x);
    require(dependencies.lastMetalScanout.damageY == flush.rectangle.y);
    require(dependencies.lastMetalScanout.damageWidth == flush.rectangle.width);
    require(
        dependencies.lastMetalScanout.damageHeight == flush.rectangle.height);
    require(!dependencies.lastMetalScanout.flipped);
    require(dependencies.readResourceCount == readsBeforeTexture);
    require(dependencies.cpuFallbackFrameCount == fallbacksBeforeTexture);

    /* A texture stored bottom row first is presented turned the right way
     * up, not read back. */
    dependencies.nativeTextureBottomUp = true;
    requireOk(device, requestBytes(flush));
    require(dependencies.directPresentationCount == 2);
    require(dependencies.lastMetalScanout.flipped);
    require(dependencies.readResourceCount == readsBeforeTexture);
    dependencies.nativeTextureBottomUp = false;

    /* A dropped frame costs the guest what a presented one does. */
    dependencies.directPresentationDrops = true;
    requireOk(device, requestBytes(flush));
    require(dependencies.directPresentationDropCount == 1);
    require(dependencies.readResourceCount == readsBeforeTexture);
    require(dependencies.cpuFallbackFrameCount == fallbacksBeforeTexture);
    dependencies.directPresentationDrops = false;

    /* A texture that is not exactly this scanout is read back instead. */
    const auto presentsBeforeMismatch = dependencies.directPresentationCount;
    dependencies.blobNativeTexture.width = scanout.width * 2U;
    requireOk(device, requestBytes(flush));
    require(dependencies.readResourceCount == readsBeforeTexture + 1U);
    dependencies.blobNativeTexture.width = scanout.width;
    dependencies.blobNativeTexture.format =
        static_cast<std::uint32_t>(protocol::GpuFormat::b8g8r8a8Unorm);
    requireOk(device, requestBytes(flush));
    require(dependencies.readResourceCount == readsBeforeTexture + 2U);
    require(dependencies.directPresentationCount == presentsBeforeMismatch);
    require(dependencies.cpuFallbackFrameCount == fallbacksBeforeTexture + 2U);
    dependencies.nativeResourceAvailable = false;

    /* A scanout the display takes as a Metal texture releases the readback
     * staging, so the Virgl blob's next readback allocates it again. */
    requireOk(device, requestBytes(scanout));
    dependencies.rejectNext(FakeGpuDependencies::Operation::blob_scanout);
    requireOk(device, requestBytes(scanout));
    const auto readsBeforeRelease = dependencies.readResourceCount;
    fail_after_allocations(1);
    const auto releasedFlush = device.process(requestBytes(flush), false);
    (void)cancel_pending_allocation_failure();
    require(responseType(releasedFlush) ==
        protocol::GpuResponse::errorOutOfMemory);
    require(dependencies.readResourceCount == readsBeforeRelease);
    requireOk(device, requestBytes(flush));
    require(dependencies.readResourceCount == readsBeforeRelease + 1U);

    const auto releasesBeforeDisable = dependencies.releaseBlobScanoutCount;
    auto disable = scanout;
    disable.resourceId = protocol::invalidResourceId;
    requireOk(device, requestBytes(disable));
    require(dependencies.releaseBlobScanoutCount == releasesBeforeDisable);
    require(dependencies.unmapResourceCount == 0);
    const auto readsAfterDisable = dependencies.readResourceCount;
    requireOk(device, requestBytes(flush));
    require(dependencies.readResourceCount == readsAfterDisable);
}

void malformed3dCommandsUseSpecificErrors()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {dependencies, noGuestReads, noGuestMappings};
    auto transfer = newRequest<protocol::GpuControlHeader>(
        GpuCommand::transferToHost3d);
    requireResponse(device, requestBytes(transfer),
        protocol::GpuResponse::errorInvalidParameter);

    auto submit = newRequest<protocol::GpuSubmit3d>(GpuCommand::submit3d);
    submit.header.contextId = 99;
    requireResponse(device, requestBytes(submit),
        protocol::GpuResponse::errorInvalidContext);

    const auto dependencyCallCount = [&] {
        return dependencies.createContextCount +
            dependencies.destroyContextCount + dependencies.submitCount +
            dependencies.createResourceCount + dependencies.transferCount +
            dependencies.readResourceCount + dependencies.fenceCount +
            dependencies.unrefResourceCount +
            dependencies.attachBackingCount +
            dependencies.detachBackingCount +
            dependencies.attachResourceCount +
            dependencies.detachResourceCount +
            dependencies.displayUpdateCount;
    };
    const auto before = dependencyCallCount();
    for (const auto command : {
             protocol::GpuCommand::resourceCreate2d,
            protocol::GpuCommand::resourceUnref,
            protocol::GpuCommand::setScanout,
            protocol::GpuCommand::setScanoutBlob,
             protocol::GpuCommand::resourceFlush,
             protocol::GpuCommand::transferToHost2d,
             protocol::GpuCommand::resourceAttachBacking,
             protocol::GpuCommand::resourceDetachBacking,
             protocol::GpuCommand::contextCreate,
             protocol::GpuCommand::contextAttachResource,
             protocol::GpuCommand::contextDetachResource,
             protocol::GpuCommand::resourceCreate3d,
             protocol::GpuCommand::transferToHost3d,
             protocol::GpuCommand::transferFromHost3d,
             protocol::GpuCommand::submit3d,
             protocol::GpuCommand::updateCursor,
             protocol::GpuCommand::moveCursor}) {
        const auto truncated = newRequest<protocol::GpuControlHeader>(command);
        const auto result = device.process(requestBytes(truncated), false);
        require(responseType(result) != protocol::GpuResponse::okNoData);
    }
    require(dependencyCallCount() == before);
}

/* RESOURCE_UNREF of a mapped blob queues an unmap action for the transport
 * and keeps the blob, and its ID, until the action completes. A failed unmap
 * leaves the blob mapped. A completed one retires it in the renderer. */
void releasingAMappedBlobRetiresItWhenItsUnmapCompletes()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {dependencies, noGuestReads, noGuestMappings};
    const auto context = createContext(0xb01, protocol::venusCapsetId);
    requireOk(device, requestBytes(context));

    auto create = newRequest<protocol::GpuResourceCreateBlob>(
        GpuCommand::resourceCreateBlob);
    create.header.contextId = context.header.contextId;
    create.resourceId = 0xb02;
    create.memory = protocol::blobMemoryHost3d;
    create.flags = protocol::blobFlagMappable;
    create.size = protocol::hostVisibleSharedMemoryAlignment;
    requireOk(device, requestBytes(create));
    auto map = newRequest<protocol::GpuResourceMapBlob>(
        GpuCommand::resourceMapBlob);
    map.resourceId = create.resourceId;
    requireResponse(device, requestBytes(map),
        protocol::GpuResponse::okMapInfo);
    completeAction(device);

    auto unref = newRequest<protocol::GpuResourceReference>(
        GpuCommand::resourceUnref);
    unref.resourceId = create.resourceId;
    const auto noAction = [&] {
        return !device.takeSharedMemoryAction().has_value();
    };
    const auto released =
        failEachAllocationBeforeCommit(device, requestBytes(unref), noAction);
    require(responseType(released.response) ==
        protocol::GpuResponse::okNoData);
    const auto action = device.takeSharedMemoryAction();
    require(action.has_value());
    require(action->type == VirtioGpuDevice::SharedMemoryAction::Type::unmap);
    require(action->resourceId == create.resourceId);
    require(!device.takeSharedMemoryAction().has_value());
    requireResponse(device, requestBytes(unref),
        protocol::GpuResponse::errorInvalidParameter);

    /* The unmap fails: the blob is mapped again, and nothing is retired. */
    device.completeSharedMemoryAction(action->actionId, false);
    require(dependencies.unrefResourceCount == 0);
    requireResponse(device, requestBytes(create),
        protocol::GpuResponse::errorInvalidParameter);
    requireOk(device, requestBytes(unref));
    const auto retry = device.takeSharedMemoryAction();
    require(retry.has_value() && retry->actionId != action->actionId);

    /* Until it completes the blob still holds its ID. */
    requireResponse(device, requestBytes(create),
        protocol::GpuResponse::errorInvalidParameter);
    device.completeSharedMemoryAction(retry->actionId, true);
    require(dependencies.unrefResourceCount == 1);
    requireOk(device, requestBytes(create));
}

/* Whichever kind of resource holds an ID, 2D, Virgl or blob, no create
 * command takes the ID again: RESOURCE_CREATE_2D answers an invalid
 * resource, the others an invalid parameter. */
void aHeldResourceIdIsNotCreatedAgain()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {dependencies, noGuestReads, noGuestMappings};
    const auto context = createContext(0xa01, protocol::venusCapsetId);
    requireOk(device, requestBytes(context));

    const auto blob = [&](std::uint32_t resourceId) {
        auto create = newRequest<protocol::GpuResourceCreateBlob>(
            GpuCommand::resourceCreateBlob);
        create.header.contextId = context.header.contextId;
        create.resourceId = resourceId;
        create.memory = protocol::blobMemoryHost3d;
        create.flags = protocol::blobFlagMappable;
        create.size = protocol::hostVisibleSharedMemoryAlignment;
        return create;
    };
    requireOk(device, requestBytes(create2d(0xa02)));
    requireOk(device, requestBytes(create3d(0xa03)));
    requireOk(device, requestBytes(blob(0xa04)));
    for (const std::uint32_t id : {0xa02U, 0xa03U, 0xa04U}) {
        requireResponse(device, requestBytes(create2d(id)),
            protocol::GpuResponse::errorInvalidResource);
        requireResponse(device, requestBytes(create3d(id)),
            protocol::GpuResponse::errorInvalidParameter);
        requireResponse(device, requestBytes(blob(id)),
            protocol::GpuResponse::errorInvalidParameter);
    }
    require(dependencies.createResourceCount == 2);
}

/* The cursor virtqueue has no writable reply buffer and the device no cursor
 * plane, so the queue's commands are consumed whatever they carry: nothing is
 * answered and nothing runs, not even a command that belongs on the control
 * queue. Only a request too short for a header is answered. */
void theCursorQueueIsConsumedWithoutAReply()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {dependencies, noGuestReads, noGuestMappings};

    auto cursor = newRequest<protocol::GpuUpdateCursor>(
        GpuCommand::updateCursor);
    cursor.position.scanoutId = protocol::primaryScanoutId;
    require(device.process(requestBytes(cursor), true).empty());
    cursor.header.type = static_cast<std::uint32_t>(GpuCommand::moveCursor);
    require(device.process(requestBytes(cursor), true).empty());
    cursor.position.scanoutId = protocol::primaryScanoutId + 1U;
    require(device.process(requestBytes(cursor), true).empty());
    const auto truncated = newRequest<protocol::GpuControlHeader>(
        GpuCommand::updateCursor);
    require(device.process(requestBytes(truncated), true).empty());

    /* A control command on the cursor queue does not run: the resource it
     * would create is still free for the control queue to create. */
    const auto create = create2d(11);
    require(device.process(requestBytes(create), true).empty());
    require(dependencies.displayUpdateCount == 0);
    requireOk(device, requestBytes(create));

    const std::array<std::uint8_t, 1> tooShort {};
    require(responseType(device.process(tooShort, true)) ==
        protocol::GpuResponse::errorInvalidParameter);
}

/* A context the guest destroys takes its renderer fence slots with it, so a
 * command still held behind one of its fences can never retire through the
 * renderer. The transport completes such commands when the device reports the
 * destroy. Reading the fence instead answers EIO, and the transport would end
 * the whole GPU connection for a process that merely exited. */
void aDestroyedContextsHeldFencesComplete()
{
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {dependencies, noGuestReads, noGuestMappings};
    const auto exiting = createContext(831);
    const auto surviving = createContext(832);
    requireOk(device, requestBytes(exiting));
    requireOk(device, requestBytes(surviving));

    DeferredFences fences;
    auto now = DeferredFences::Clock::now();
    const auto deferSubmit = [&](std::uint32_t contextId,
                                 std::uint64_t requestId) {
        auto submit = newRequest<protocol::GpuSubmit3d>(GpuCommand::submit3d);
        submit.header.flags = protocol::gpuFlagFence;
        submit.header.contextId = contextId;
        submit.header.fenceId = requestId;
        auto result = device.processWithDeferredFence(
            requestBytes(submit), false, requestId, headerReplyCapacity);
        require(result.fence.has_value());
        require(!result.destroyedContextId.has_value());
        fences.defer(
            requestId,
            submit.header,
            std::move(result.response),
            *result.fence,
            now);
    };
    deferSubmit(exiting.header.contextId, 41);
    deferSubmit(surviving.header.contextId, 42);
    deferSubmit(exiting.header.contextId, 43);
    const auto status = [&device](const DeferredFence& fence) {
        return device.contextFenceStatus(fence);
    };
    fences.retire(now, status);
    require(!fences.empty());

    /* The first window expires with every fence busy: ordering is released,
     * which is what lets the guest's destroy through while fences are held. */
    now += DeferredFences::maximumFenceWait;
    fences.retire(now, status);
    require(fences.orderingRelaxed());

    auto destroy = newRequest<protocol::GpuControlHeader>(
        GpuCommand::contextDestroy);
    destroy.contextId = exiting.header.contextId;
    const auto destroyed = device.processWithDeferredFence(
        requestBytes(destroy), false, 0, headerReplyCapacity);
    require(responseType(destroyed.response) ==
        protocol::GpuResponse::okNoData);
    require(!destroyed.fence.has_value());
    require(destroyed.destroyedContextId == exiting.header.contextId);
    fences.forgetContext(*destroyed.destroyedContextId);

    /* The next tick reads every fence still held. None of them may be the
     * destroyed context's: its slots are gone and the read answers EIO. */
    bool connectionKept = true;
    try {
        fences.retire(now, status);
    } catch (const std::runtime_error&) {
        connectionKept = false;
    }
    require(connectionKept);

    /* Both of the destroyed context's commands complete, in order, with the
     * fenced responses the guest is waiting for. */
    DeferredFences::Completion completion;
    for (const std::uint64_t requestId : {41U, 43U}) {
        require(fences.takeCompletion(completion));
        require(completion.requestId == requestId);
        const auto reply = responseHeader(completion.response);
        require(reply.contextId == exiting.header.contextId);
        require(reply.fenceId == requestId);
        require((reply.flags & protocol::gpuFlagFence) != 0);
    }
    require(!fences.takeCompletion(completion));

    /* The surviving context's fence is still the renderer's to retire. */
    require(!fences.empty());
    require(fences.orderingRelaxed());
    dependencies.retireContextFence(surviving.header.contextId, 0, 42);
    fences.retire(now, status);
    require(fences.empty());
    require(!fences.orderingRelaxed());
    require(fences.takeCompletion(completion));
    require(completion.requestId == 42);
    require(!fences.takeCompletion(completion));

    /* A destroy the device refuses reports no context. */
    const auto refused = device.processWithDeferredFence(
        requestBytes(destroy), false, 0, headerReplyCapacity);
    require(responseType(refused.response) ==
        protocol::GpuResponse::errorInvalidContext);
    require(!refused.destroyedContextId.has_value());
}

/* A ring that stopped retiring belongs to its context. The guest reuses the
 * lowest free context ID, so a count that outlived the context would start
 * the next process's ring as dead and complete its fences before its GPU work
 * ran. */
void aReusedContextIdStartsWithALiveRing()
{
    DeferredFences fences;
    auto now = DeferredFences::Clock::now();
    const DeferredFence fence {.contextId = 5, .ringIndex = 0};
    const auto busy = [](const DeferredFence&) { return EBUSY; };
    DeferredFences::Completion completion;
    protocol::GpuControlHeader header {};
    header.contextId = fence.contextId;

    std::uint64_t requestId = 0;
    for (unsigned escape = 0; escape < maximumRingEscapes; ++escape) {
        fences.defer(++requestId, header, {}, fence, now);
        now += DeferredFences::maximumFenceWait;
        fences.retire(now, busy);
        require(!fences.empty());
        now += DeferredFences::maximumFenceWait;
        fences.retire(now, busy);
        require(fences.empty());
        require(fences.takeCompletion(completion));
        require(completion.requestId == requestId);
    }

    /* The ring is now dead: its next fence completes on the first tick. */
    fences.defer(++requestId, header, {}, fence, now);
    fences.retire(now, busy);
    require(fences.empty());
    require(fences.takeCompletion(completion));

    /* A new context with the same ID gets a whole window again. */
    fences.forgetContext(fence.contextId);
    fences.defer(++requestId, header, {}, fence, now);
    fences.retire(now, busy);
    require(!fences.empty());
    require(!fences.takeCompletion(completion));
    fences.retire(now + DeferredFences::maximumFenceWait, busy);
    require(!fences.empty());
    require(fences.orderingRelaxed());
}

/* Completions wait for the transport to take them, so they share the
 * virtqueue's capacity with the commands still held. */
void heldAndCompletedFencesShareTheQueueCapacity()
{
    DeferredFences fences;
    const auto now = DeferredFences::Clock::now();
    protocol::GpuControlHeader header {};
    header.contextId = 7;
    for (std::uint32_t request = 0;
         request < protocol::maximumVirtqueueSize;
         ++request) {
        require(!fences.full());
        fences.defer(request, header, {}, {.contextId = 7}, now);
    }
    require(fences.full());
    fences.forgetContext(7);
    require(fences.empty());
    require(fences.full());
    DeferredFences::Completion completion;
    require(fences.takeCompletion(completion));
    require(completion.requestId == 0);
    require(!fences.full());
}

/* The server arms its fence timer after the device has run a command, so
 * arming must not be able to fail for want of kernel memory: the timer's knote
 * is added once with the queue, and arming, firing and cancelling only modify
 * it. A knote exists exactly when it can be disabled without being added. */
void theFenceTimerKeepsItsKnote()
{
    const UniqueDescriptor queue {::kqueue()};
    require(queue.get() >= 0);
    const auto knoteExists = [&] {
        struct kevent change {};
        EV_SET(&change, fenceTimerIdentifier, EVFILT_TIMER, EV_DISABLE, 0, 0,
            nullptr);
        return ::kevent(queue.get(), &change, 1, nullptr, 0, nullptr) == 0;
    };
    const auto fires = [&](long milliseconds) {
        struct kevent event {};
        const timespec timeout {
            .tv_sec = milliseconds / 1000,
            .tv_nsec = milliseconds % 1000 * 1'000'000,
        };
        const int count =
            ::kevent(queue.get(), nullptr, 0, &event, 1, &timeout);
        require(count >= 0);
        return count == 1 && event.filter == EVFILT_TIMER &&
            event.ident == fenceTimerIdentifier;
    };

    addFenceTimer(queue.get());
    require(!fires(5));
    for (unsigned round = 0; round < 2; ++round) {
        armFenceTimer(queue.get());
        require(fires(2000));
        require(!fires(5));
        require(knoteExists(), "the fence timer's knote went when it fired");
    }
    armFenceTimer(queue.get());
    cancelFenceTimer(queue.get());
    require(!fires(5));
    require(knoteExists(), "cancelling the fence timer removed its knote");
    armFenceTimer(queue.get());
    require(fires(2000));
}

/* Each deferrable command, on a ring the server has not held a command for:
 * holding the first command on a ring is what adds the ring to the server's
 * escape counts. */
void deferredCommandsAllocateBeforeTheDeviceCommits()
{
    std::array<std::uint8_t, 256> guestMemory {};
    FakeGpuDependencies dependencies;
    VirtioGpuDevice device {
        dependencies, noGuestReads, guestMemoryAt0x1000(guestMemory)};
    const auto resource = create3d(850);
    requireOk(device, requestBytes(resource));
    requireOk(device, attachBackingRequest(
        resource.resourceId, guestMemory.size()));

    /* The fake renderer records each fence in containers of its own, where
     * the real one allocates in C, out of the sweep's reach. Make room in
     * them first, so that the sweep counts only the device and the server. */
    dependencies.contextFenceCookies.reserve(3);
    const auto commits = [&dependencies] {
        return dependencies.submitCount + dependencies.transferCount;
    };
    const auto status = [&device](const DeferredFence& fence) {
        return device.contextFenceStatus(fence);
    };

    DeferredFences fences;
    protocol::GpuSubmit3d submit {};
    protocol::GpuTransferHost3d transfer {};
    transfer.box = {.width = 1, .height = 1, .depth = 1};
    transfer.resourceId = resource.resourceId;
    std::uint32_t contextId = 851;
    for (const auto command : {
             protocol::GpuCommand::submit3d,
             protocol::GpuCommand::transferToHost3d,
             protocol::GpuCommand::transferFromHost3d}) {
        requireOk(device, requestBytes(createContext(contextId)));
        dependencies.retireContextFence(contextId, 0, 0);
        const bool submits = command == protocol::GpuCommand::submit3d;
        auto& header = submits ? submit.header : transfer.header;
        header.type = static_cast<std::uint32_t>(command);
        header.flags = protocol::gpuFlagFence;
        header.contextId = contextId;
        header.fenceId = contextId;
        const auto request =
            submits ? requestBytes(submit) : requestBytes(transfer);
        const std::uint64_t requestId = contextId;
        const auto committed = commits();
        const auto fenced = dependencies.contextFenceCookies.size();
        const auto held = failEachAllocationBeforeDeferral(
            device, fences, request, requestId, [&] {
                return commits() == committed &&
                    dependencies.contextFenceCookies.size() == fenced &&
                    fences.empty();
            });
        require(held.response.empty());
        require(commits() == committed + 1);
        require(!fences.empty());

        dependencies.retireContextFence(contextId, 0, requestId);
        fences.retire(DeferredFences::Clock::now(), status);
        require(fences.empty());
        DeferredFences::Completion completion;
        require(fences.takeCompletion(completion));
        require(completion.requestId == requestId);
        const auto reply = responseHeader(completion.response);
        require(reply.type == static_cast<std::uint32_t>(
            protocol::GpuResponse::okNoData));
        require(reply.fenceId == header.fenceId);
        require((reply.flags & protocol::gpuFlagFence) != 0);
        ++contextId;
    }
}

/* The vhost-user server passes the device the bytes the guest left for the
 * reply. A command whose reply would not fit there is refused before it runs,
 * since the server could not deliver the reply: a map that ran anyway would
 * already have queued its shared-memory action and claimed its pages. Only
 * the reply's whole size is enough. */
void repliesThatDoNotFitAreRefusedBeforeTheCommandRuns()
{
    FakeGpuDependencies dependencies;
    constexpr std::uint32_t dotsPerInch = 96;
    VirtioGpuDevice device {
        dependencies, noGuestReads, noGuestMappings, dotsPerInch};
    const auto context = createContext(0x921, protocol::venusCapsetId);
    requireOk(device, requestBytes(context));
    auto create = newRequest<protocol::GpuResourceCreateBlob>(
        GpuCommand::resourceCreateBlob);
    create.header.contextId = context.header.contextId;
    create.resourceId = 0x922;
    create.memory = protocol::blobMemoryHost3d;
    create.flags = protocol::blobFlagMappable;
    create.size = protocol::hostVisibleSharedMemoryAlignment;
    requireOk(device, requestBytes(create));

    auto displayInfo = newRequest<protocol::GpuControlHeader>(
        GpuCommand::getDisplayInfo);
    auto edid = newRequest<protocol::GpuGetEdid>(GpuCommand::getEdid);
    edid.scanoutId = protocol::primaryScanoutId;
    auto capsetInfo = newRequest<protocol::GpuGetCapsetInfo>(
        GpuCommand::getCapsetInfo);
    capsetInfo.capsetIndex = protocol::virgl2CapsetIndex;
    auto capset = newRequest<protocol::GpuGetCapset>(GpuCommand::getCapset);
    capset.capsetId = protocol::virgl2CapsetId;
    capset.version = 1;
    auto map = newRequest<protocol::GpuResourceMapBlob>(
        GpuCommand::resourceMapBlob);
    map.resourceId = create.resourceId;

    struct Command {
        std::span<const std::uint8_t> request;
        std::size_t replySize;
        protocol::GpuResponse success;
    };
    const std::array commands {
        Command {requestBytes(displayInfo), sizeof(protocol::GpuDisplayInfo),
            protocol::GpuResponse::okDisplayInfo},
        Command {requestBytes(edid), sizeof(protocol::GpuEdid),
            protocol::GpuResponse::okEdid},
        Command {requestBytes(capsetInfo), sizeof(protocol::GpuCapsetInfo),
            protocol::GpuResponse::okCapsetInfo},
        Command {requestBytes(capset),
            sizeof(protocol::GpuControlHeader) +
                dependencies.capset(capset.capsetId).data.size(),
            protocol::GpuResponse::okCapset},
        Command {requestBytes(map), sizeof(protocol::GpuMapInfo),
            protocol::GpuResponse::okMapInfo},
    };
    for (const auto& command : commands) {
        for (const std::size_t capacity :
             {headerReplyCapacity, command.replySize - 1}) {
            const auto refused = device.processWithDeferredFence(
                command.request, false, 0, capacity);
            require(refused.response.size() <= capacity);
            require(responseType(refused.response) ==
                protocol::GpuResponse::errorInvalidParameter);
            require(!device.takeSharedMemoryAction().has_value());
        }
        const auto answered = device.processWithDeferredFence(
            command.request, false, 0, command.replySize);
        require(answered.response.size() == command.replySize);
        require(responseType(answered.response) == command.success);
    }

    /* The refused maps left the blob unmapped: only the map that fit queued
     * an action. */
    const auto action = device.takeSharedMemoryAction();
    require(action.has_value());
    require(!device.takeSharedMemoryAction().has_value());
    device.completeSharedMemoryAction(action->actionId, true);
}

} // namespace

} // namespace uml::gpu::testing

int main()
{
    using namespace uml::gpu::testing;

    edidReportsDisplayPixelsAndEffectiveDensity();
    displayChangeEventsPreserveAResizeThatRacesGuestClear();
    contextInitUsesVirgl2ForTheDeviceSelectedDefault();
    aRendererWithoutVulkanOffersAndRunsNoVenus();
    lifecycleCommandsAllocateBeforeCommitAndRetry();
    theLedgerFollowsTheGuestsCommands();
    contextCreateIsRetryableAfterEachAllocationFailure();
    blobCommandsAllocateBeforeCommitAndRetry();
    refusedBlobIsAnsweredAsOutOfMemory();
    rendererExecutionFailuresPreserveDeviceState();
    softwareScanoutProgressesWhileRendererFenceIsPending();
    deferredFencesUseHostTimelineAndMergedRetirement();
    aDestroyedContextsHeldFencesComplete();
    aReusedContextIdStartsWithALiveRing();
    heldAndCompletedFencesShareTheQueueCapacity();
    deferredCommandsAllocateBeforeTheDeviceCommits();
    theFenceTimerKeepsItsKnote();
    resetRetiresEachExternalOwnerExactlyOnce();
    resetIsPristineAndAllocationRetryIsAtomic();
    rendererBackingAttachIsRetryableAfterAllocationFailure();
    backingRangesAndAttachStateAreValidated();
    failedTransferDoesNotPartiallyCommitPixels();
    setScanoutReplyFailureDoesNotCommitAllocation();
    theGuestScanoutFlagFollowsTheScanout();
    readbackStagingIsAllocatedByTheFirstReadback();
    unanswerableCapabilityQueriesFailOnlyThemselves();
    venusBlobsAndFencesUseStandardWireState();
    blobMappingCapacityFollowsTheAperture();
    repliesThatDoNotFitAreRefusedBeforeTheCommandRuns();
    blobScanoutUsesNativeTextureAndPersistentVncMapping();
    virglBlobScanoutPresentsItsGlTexture();
    malformed3dCommandsUseSpecificErrors();
    releasingAMappedBlobRetiresItWhenItsUnmapCompletes();
    aHeldResourceIdIsNotCreatedAgain();
    theCursorQueueIsConsumedWithoutAReply();
    std::puts("Virtio-GPU command atomicity tests passed");
    return 0;
}
