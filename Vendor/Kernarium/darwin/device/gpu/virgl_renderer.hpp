// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "context_idle_tracker.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

struct iovec;
struct virgl_renderer_gl_ctx_param;

namespace uml::gpu {

class HostGlProvider;

// Owns the process-global virglrenderer instance. Every Apple product uses the
// same opaque EGL provider backed by Metal; provider state never changes this
// public class layout.
class VirglRenderer {
public:
    static constexpr std::uint32_t sharedMemoryBlobDescriptor = 3;

    struct Capset {
        std::uint32_t maximumVersion {};
        std::vector<std::uint8_t> data;
    };

    struct Resource3d {
        std::uint32_t resourceId {};
        std::uint32_t target {};
        std::uint32_t format {};
        std::uint32_t bind {};
        std::uint32_t width {};
        std::uint32_t height {};
        std::uint32_t depth {};
        std::uint32_t arraySize {};
        std::uint32_t lastLevel {};
        std::uint32_t sampleCount {};
        std::uint32_t flags {};
    };

    struct Box {
        std::uint32_t x {};
        std::uint32_t y {};
        std::uint32_t z {};
        std::uint32_t width {};
        std::uint32_t height {};
        std::uint32_t depth {};
    };

    /* The Metal texture a resource is stored in, as the renderer describes
     * it. The texture is borrowed: it stays valid while the resource keeps
     * its storage, so a caller asks again for every frame. */
    struct NativeTexture {
        void* texture {};
        std::uint32_t format {};
        std::uint32_t width {};
        std::uint32_t height {};
        /* The resource's first row is the bottom of the image it holds. */
        bool bottomUp {};
    };

    struct ResourceBlob {
        std::uint32_t resourceId {};
        std::uint32_t contextId {};
        std::uint32_t memory {};
        std::uint32_t flags {};
        std::uint64_t blobId {};
        std::uint64_t size {};
    };

    VirglRenderer();
    ~VirglRenderer();

    /* Bounds the device memory a Venus guest may allocate to `bytes`,
     * counted against everything the host GPU holds for this process, or
     * zero for no bound. Takes effect for the next renderer constructed. The
     * pool admits reservations only up to `bytes` less
     * deviceMemoryReserveBytes, the room the memory it never refuses grows
     * into. */
    static void setDeviceMemoryLimit(std::uint64_t bytes) noexcept;
    static constexpr std::uint64_t deviceMemoryReserveBytes = 16ULL << 20;

    /* Called, from the render server's threads, for each allocation of a
     * Venus guest that failed for want of device memory, after the pool had
     * freed what it could. Cleared when the renderer goes. */
    using MemoryRefusalObserver = void (*)(void* data) noexcept;
    static void setMemoryRefusalObserver(
        MemoryRefusalObserver observer, void* data) noexcept;

    /* Called, from the render server's threads, whenever the device memory a
     * Venus context's guest holds changes: its size when an allocation
     * succeeds and minus that size when the memory is freed or the context
     * goes. Memory the host driver only imports is not reported. Cleared
     * when the renderer goes. */
    using MemoryAccountObserver = void (*)(
        void* data, std::uint32_t contextId, std::int64_t delta) noexcept;
    static void setMemoryAccountObserver(
        MemoryAccountObserver observer, void* data) noexcept;

    /* The device memory budget and what the process holds against it, while
     * a renderer with a budget exists; any thread. Nothing without one. The
     * usage is the pool's own, which is what the budget refuses by
     * (accountedGpuBytes(), plus the reservations in flight). It includes
     * memory the host's drivers hold for reuse. The ceiling is the usage the
     * pool refuses above: the budget less deviceMemoryReserveBytes. */
    struct DeviceMemory {
        std::uint64_t budgetBytes {};
        std::uint64_t usageBytes {};
        std::uint64_t ceilingBytes {};
    };
    [[nodiscard]] static std::optional<DeviceMemory> deviceMemory() noexcept;

    /* Frees what the host's drivers cache for reuse, the way the pool does
     * before it refuses, and returns how many cached allocations that freed;
     * any thread. Nothing while no renderer with a budget exists. */
    static std::uint64_t reclaimCachedMemory() noexcept;

    VirglRenderer(const VirglRenderer&) = delete;
    VirglRenderer& operator=(const VirglRenderer&) = delete;

    [[nodiscard]] Capset capset(std::uint32_t capsetId) const;
    [[nodiscard]] int createContext(
        std::uint32_t contextId,
        std::uint32_t flags,
        std::string_view debugName) noexcept;
    void destroyContext(std::uint32_t contextId) noexcept;

    [[nodiscard]] int submit(
        std::uint32_t contextId,
        std::span<const std::uint8_t> command) noexcept;
    [[nodiscard]] int createResource3d(
        const Resource3d& resource) noexcept;
    [[nodiscard]] int createResourceBlob(
        const ResourceBlob& resource) noexcept;
    [[nodiscard]] int exportResourceBlob(
        std::uint32_t resourceId,
        std::uint32_t& descriptorType,
        int& descriptor) noexcept;
    [[nodiscard]] int resourceMapInfo(
        std::uint32_t resourceId,
        std::uint32_t& mapInfo) noexcept;
    [[nodiscard]] int mapResource(
        std::uint32_t resourceId,
        void*& mapping,
        std::uint64_t& size) noexcept;
    void unmapResource(std::uint32_t resourceId) noexcept;
    [[nodiscard]] int createBlobScanout(
        std::uint32_t resourceId,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t format,
        std::uint32_t stride,
        std::uint32_t offset,
        void*& texture) noexcept;
    void releaseBlobScanout(void* texture) noexcept;
    [[nodiscard]] int transferResource(
        bool toHost,
        std::uint32_t resourceId,
        std::uint32_t contextId,
        std::uint32_t level,
        std::uint32_t stride,
        std::uint32_t layerStride,
        const Box& box,
        std::uint64_t offset) noexcept;
    [[nodiscard]] int readResourceRectangle(
        std::uint32_t resourceId,
        std::span<std::uint8_t> destination,
        std::uint32_t x,
        std::uint32_t y,
        std::uint32_t width,
        std::uint32_t height) noexcept;
    [[nodiscard]] int resourceMetalTexture(
        std::uint32_t resourceId,
        NativeTexture& texture) noexcept;
    [[nodiscard]] int createFenceAndWait(
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t fenceId,
        const std::atomic_bool* stopRequested) noexcept;
    [[nodiscard]] int createContextFence(
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t fenceCookie) noexcept;
    [[nodiscard]] int pollContextFences(
        std::uint32_t contextId) noexcept;
    [[nodiscard]] int pollRenderer() noexcept;
    [[nodiscard]] bool hasPendingWork() noexcept;
    [[nodiscard]] int contextFenceStatus(
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t fenceCookie) noexcept;
    void unrefResource(std::uint32_t resourceId) noexcept;
    /* The bytes a Virgl resource's storage takes on the host; 0 for a
     * resource with none of its own. */
    [[nodiscard]] std::uint64_t resourceStorageBytes(
        std::uint32_t resourceId) noexcept;
    [[nodiscard]] int attachBacking(
        std::uint32_t resourceId,
        std::span<iovec> backing) noexcept;
    void detachBacking(std::uint32_t resourceId) noexcept;
    [[nodiscard]] int attachResource(
        std::uint32_t contextId,
        std::uint32_t resourceId) noexcept;
    [[nodiscard]] int detachResource(
        std::uint32_t contextId,
        std::uint32_t resourceId) noexcept;
    void resetFenceState() noexcept;

    /* Guest GL contexts that have gone quiet release what the host GL driver
     * keeps for them, and so does the renderer's own context (context 0),
     * where a resource is made and transferred when a command names no guest
     * context. A context is idle once a second has passed since its last
     * command, transfer, resource creation or fence and it has no fence
     * outstanding; the renderer is told once, and again after the context is
     * next active. The service thread asks how long until the next context
     * could be idle, arms a timer for it, and calls trimIdleContexts when it
     * fires. */
    [[nodiscard]] std::optional<std::chrono::steady_clock::duration>
        nextIdleTrimDelay() const noexcept;
    void trimIdleContexts() noexcept;

private:
    static void* createGlContext(
        void* cookie,
        int scanoutIndex,
        virgl_renderer_gl_ctx_param* parameters) noexcept;
    static void destroyGlContext(void* cookie, void* context) noexcept;
    static int makeGlContextCurrent(
        void* cookie,
        int scanoutIndex,
        void* context) noexcept;
    static void* getEglDisplay(void* cookie) noexcept;
    static void writeFence(void* cookie, std::uint32_t fenceId) noexcept;
    static void writeContextFence(
        void* cookie,
        std::uint32_t contextId,
        std::uint32_t ringIndex,
        std::uint64_t fenceId) noexcept;
    void noteContextActivity(std::uint32_t contextId) noexcept;
    void noteResourceReleased() noexcept;
    [[nodiscard]] static std::uint64_t contextRingKey(
        std::uint32_t contextId,
        std::uint32_t ringIndex) noexcept;

    mutable std::mutex mutex_;
    std::condition_variable fenceCondition_;
    std::unordered_map<std::uint64_t, std::uint64_t> retiredContextFences_;
    std::atomic_bool fenceCallbackFailed_ {};
    std::unique_ptr<HostGlProvider> provider_;
    std::uint32_t retiredFence_ {};
    ContextIdleTracker idleContexts_;
    bool idleTrimEnabled_ {true};
};

} // namespace uml::gpu
