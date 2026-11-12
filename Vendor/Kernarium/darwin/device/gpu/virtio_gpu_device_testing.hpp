// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "context_memory_ledger.hpp"
#include "display.hpp"
#include "protocol.hpp"
#include "virgl_renderer.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace uml::gpu::testing {

/* Test-only dependency boundary for VirtioGpuDevice. Ordinary builds use
 * UML_GPU_DEVICE_TESTING=0 and call VirglRenderer and VncDisplay directly, so
 * this virtual surface adds no production field, branch, or indirect call. */
class VirtioGpuDeviceDependencies {
public:
    virtual ~VirtioGpuDeviceDependencies() = default;

    [[nodiscard]] virtual std::uint32_t width() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t height() const noexcept = 0;
    [[nodiscard]] virtual bool guestScanoutChanged(
        std::uint32_t width,
        std::uint32_t height) noexcept = 0;
    virtual void resetGuestSession() noexcept = 0;
    virtual void clear() noexcept = 0;
    /* A display keeps the flag itself; a fake that records it overrides this. */
    virtual void setGuestScanoutActive(bool) noexcept {}
    /* A display counts the refusals itself; a fake that records them
     * overrides this. */
    virtual void recordMemoryRefusal() noexcept {}
    /* A display counts the refusals itself; a fake that records them
     * overrides this. */
    [[nodiscard]] virtual std::uint64_t memoryRefusals() const noexcept
    {
        return 0;
    }
    /* A display keeps whether a guest reclaimer acts on the memory reports
     * (Display::guestReclaims); the device's test double keeps it here. */
    void setGuestReclaims(bool reclaims) noexcept { guestReclaims_ = reclaims; }
    [[nodiscard]] bool guestReclaims() const noexcept { return guestReclaims_; }
    /* The device memory budget and the pool's usage, while a renderer with a
     * budget runs: VirglRenderer::deviceMemory() for the production device. */
    [[nodiscard]] virtual std::optional<VirglRenderer::DeviceMemory>
        deviceMemory() const noexcept
    {
        return std::nullopt;
    }
    /* Frees what the host's drivers cache, as the pool does before it refuses
     * (VirglRenderer::reclaimCachedMemory() for the production device), and
     * returns how many cached allocations it freed. */
    [[nodiscard]] virtual std::uint64_t reclaimCachedMemory() noexcept
    {
        return 0;
    }
    /* A display keeps what the guest's contexts hold of the host GPU's
     * memory; the device's test double keeps it here. */
    [[nodiscard]] ContextMemoryLedger& graphicsLedger() noexcept
    {
        return graphicsLedger_;
    }
    virtual void update(
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
        std::uint32_t height) = 0;

    [[nodiscard]] virtual VirglRenderer::Capset capset(
        std::uint32_t capsetId) const = 0;
    /* A renderer without a Vulkan driver, as the iOS Simulator's, offers no
     * Venus capset. */
    [[nodiscard]] virtual bool offersVenus() const noexcept { return true; }
    [[nodiscard]] virtual int createContext(
        std::uint32_t contextId,
        std::uint32_t flags,
        std::string_view debugName) noexcept = 0;
    virtual void destroyContext(std::uint32_t contextId) noexcept = 0;
    [[nodiscard]] virtual int submit(
        std::uint32_t contextId,
        std::span<const std::uint8_t> command) noexcept = 0;
    [[nodiscard]] virtual int createResource3d(
        const VirglRenderer::Resource3d& resource) noexcept = 0;
    [[nodiscard]] virtual int createResourceBlob(
        const VirglRenderer::ResourceBlob& resource) noexcept = 0;
    [[nodiscard]] virtual int exportResourceBlob(
        std::uint32_t resourceId,
        std::uint32_t& descriptorType,
        int& descriptor) noexcept = 0;
    [[nodiscard]] virtual int resourceMapInfo(
        std::uint32_t resourceId,
        std::uint32_t& mapInfo) noexcept = 0;
    [[nodiscard]] virtual int mapResource(
        std::uint32_t resourceId,
        void*& mapping,
        std::uint64_t& size) noexcept = 0;
    virtual void unmapResource(std::uint32_t resourceId) noexcept = 0;
    [[nodiscard]] virtual int createBlobScanout(
        std::uint32_t resourceId,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t format,
        std::uint32_t stride,
        std::uint32_t offset,
        void*& texture) noexcept = 0;
    virtual void releaseBlobScanout(void* texture) noexcept = 0;
    [[nodiscard]] virtual int transferResource(
        bool toHost,
        std::uint32_t resourceId,
        std::uint32_t contextId,
        std::uint32_t level,
        std::uint32_t stride,
        std::uint32_t layerStride,
        const VirglRenderer::Box& box,
        std::uint64_t offset) noexcept = 0;
    [[nodiscard]] virtual int readResourceRectangle(
        std::uint32_t resourceId,
        std::span<std::uint8_t> destination,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t width,
        std::uint32_t height) = 0;
    [[nodiscard]] virtual int resourceMetalTexture(
        std::uint32_t resourceId,
        VirglRenderer::NativeTexture& texture) noexcept = 0;
    [[nodiscard]] virtual PresentOutcome presentMetal(
        const MetalScanout& scanout) noexcept = 0;
    [[nodiscard]] virtual bool needsFullCpuFallback() const noexcept = 0;
    virtual void recordCpuFallbackFrame() noexcept = 0;
    [[nodiscard]] virtual int createFenceAndWait(
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t fenceId,
        const std::atomic_bool* stopRequested) noexcept = 0;
    [[nodiscard]] virtual int createContextFence(
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t fenceCookie) noexcept = 0;
    [[nodiscard]] virtual int pollContextFences(
        std::uint32_t contextId) noexcept = 0;
    [[nodiscard]] virtual int pollRenderer() noexcept = 0;
    [[nodiscard]] virtual bool hasPendingWork() noexcept = 0;
    [[nodiscard]] virtual std::optional<std::chrono::steady_clock::duration>
        nextIdleTrimDelay() const noexcept = 0;
    virtual void trimIdleContexts() noexcept = 0;
    [[nodiscard]] virtual int contextFenceStatus(
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t fenceCookie) noexcept = 0;
    virtual void unrefResource(std::uint32_t resourceId) noexcept = 0;
    /* The host storage of a Virgl resource, which the ledger credits. */
    [[nodiscard]] virtual std::uint64_t resourceStorageBytes(
        std::uint32_t) noexcept
    {
        return 0;
    }
    [[nodiscard]] virtual int attachBacking(
        std::uint32_t resourceId,
        std::span<iovec> backing) noexcept = 0;
    virtual void detachBacking(std::uint32_t resourceId) noexcept = 0;
    [[nodiscard]] virtual int attachResource(
        std::uint32_t contextId,
        std::uint32_t resourceId) noexcept = 0;
    [[nodiscard]] virtual int detachResource(
        std::uint32_t contextId,
        std::uint32_t resourceId) noexcept = 0;
    virtual void resetFenceState() noexcept = 0;

private:
    ContextMemoryLedger graphicsLedger_;
    bool guestReclaims_ {};
};

} // namespace uml::gpu::testing
