// SPDX-License-Identifier: GPL-2.0-or-later

#include "virgl_renderer.hpp"

#include "device_memory_pool.hpp"
#include "host_gl_provider.hpp"
#include "gpu_memory_figures.hpp"
#include "host_gpu_memory.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <epoxy/gl.h>
#include <stdexcept>
#include <string>
#include <sys/uio.h>
#include <unistd.h>

extern "C" {
#include <virgl_hw.h>
#include <virglrenderer.h>

typedef void (*epoxy_resolver_stub_t)(void);
typedef epoxy_resolver_stub_t
    (*epoxy_resolver_failure_handler_t)(const char* name);
epoxy_resolver_failure_handler_t epoxy_set_resolver_failure_handler(
    epoxy_resolver_failure_handler_t handler);
}

namespace uml::gpu {
namespace {

// A renderer fence normally retires in milliseconds. A finite wait prevents a
// malformed guest command or a lost emulator callback from hanging the whole
// in-process emulator indefinitely.
constexpr auto maximumFenceWait = std::chrono::seconds(60);
// Stable Virtio-GPU ABI identifiers consumed by virgl_renderer_get_cap_set().
constexpr std::uint32_t virglCapsetId = 1;
constexpr std::uint32_t virgl2CapsetId = 2;
constexpr std::uint32_t venusCapsetId = 4;
// The renderer's own context, which no guest names: resources are made and
// transferred on it when a command carries no context.
constexpr std::uint32_t hostContextId = 0;

/* UML_GPU_IDLE_TRIM_MS is how long a guest GL context stays quiet before the
 * renderer asks the host driver to release what it keeps for it: a whole
 * number of milliseconds, zero to never. Unset or unusable, one second. */
constexpr auto defaultIdleTrim = std::chrono::milliseconds(1000);

[[nodiscard]] std::optional<std::chrono::milliseconds> idleTrimInterval()
{
#ifdef UML_APP_STORE_RUNTIME
    const char* const text = nullptr;
#else
    const char* const text = std::getenv("UML_GPU_IDLE_TRIM_MS");
#endif
    if (text == nullptr || *text == '\0')
        return defaultIdleTrim;
    char* end = nullptr;
    errno = 0;
    const long long value = std::strtoll(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < 0 ||
        value > std::numeric_limits<int>::max()) {
        return defaultIdleTrim;
    }
    if (value == 0)
        return std::nullopt;
    return std::chrono::milliseconds(value);
}

std::atomic_bool missingGlEntryPoint;
std::atomic_bool rendererInstanceActive;
std::atomic<std::uint64_t> deviceMemoryLimit;
// The pool the renderer and the host GPU driver charge the guest's device
// memory to. virgl_renderer_init() keeps its address until cleanup, and a
// renderer replaces it only once the one before has been cleaned up.
std::unique_ptr<DeviceMemoryPool> deviceMemoryPool;

// Who is told of each allocation the pool refused a Venus guest. Set while a
// renderer exists; the render server's threads read it.
std::atomic<VirglRenderer::MemoryRefusalObserver> memoryRefusalObserver;
std::atomic<void*> memoryRefusalObserverData;
// Who is told how much device memory each Venus context holds; the same rule.
std::atomic<VirglRenderer::MemoryAccountObserver> memoryAccountObserver;
std::atomic<void*> memoryAccountObserverData;
// Guards the pool's address for the readers of its figures: a statistics read
// from any thread must not meet a renderer that is going.
std::mutex deviceMemoryReadMutex;
bool deviceMemoryReadable;

#if !UML_GPU_SOFTWARE_RENDERER
std::uint64_t deviceMemoryPoolUsage(void* data) noexcept
{
    return static_cast<DeviceMemoryPool*>(data)->usage();
}

bool reserveDeviceMemory(void* data, std::uint64_t size) noexcept
{
    return static_cast<DeviceMemoryPool*>(data)->reserve(size);
}

void releaseDeviceMemory(void* data, std::uint64_t size) noexcept
{
    static_cast<DeviceMemoryPool*>(data)->release(size);
}

void deviceMemoryRefused(void*) noexcept
{
    if (const auto observer =
            memoryRefusalObserver.load(std::memory_order_acquire)) {
        observer(memoryRefusalObserverData.load(std::memory_order_relaxed));
    }
}

void deviceMemoryAccounted(
    void*, std::uint32_t contextId, std::int64_t delta) noexcept
{
    if (const auto observer =
            memoryAccountObserver.load(std::memory_order_acquire)) {
        observer(
            memoryAccountObserverData.load(std::memory_order_relaxed),
            contextId,
            delta);
    }
}
#endif

/* The capability report is a diagnostic an App Store runtime does not carry. */
#ifndef UML_APP_STORE_RUNTIME
[[nodiscard]] bool capabilityReportEnabled() noexcept
{
    const char* value = std::getenv("UML_GPU_CAPABILITY_REPORT");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

[[nodiscard]] std::string hexEncode(std::span<const std::uint8_t> bytes)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string encoded;
    encoded.resize(bytes.size() * 2U);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        encoded[index * 2U] = digits[bytes[index] >> 4U];
        encoded[index * 2U + 1U] = digits[bytes[index] & 0x0fU];
    }
    return encoded;
}

[[nodiscard]] std::string hexEncode(std::string_view text)
{
    return hexEncode(std::span {
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
}

void writeCapabilityRecord(std::string_view record) noexcept
{
    /*
     * iOS device console capture transports stdout and stderr independently
     * before merging them on the host. A short stderr write can therefore be
     * split around later stdout readiness output even though the producer
     * writes one logical record at a time. Keep the complete diagnostic on
     * the same ordered stream as GPU readiness and the relayed guest console.
     */
    while (!record.empty()) {
        const auto written = ::write(STDOUT_FILENO, record.data(), record.size());
        if (written > 0) {
            record.remove_prefix(static_cast<std::size_t>(written));
            continue;
        }
        if (written < 0 && errno == EINTR)
            continue;
        return;
    }
}

void reportContextProbe(const HostGlProvider::ContextProbe& probe)
{
    std::string record =
        "UML_GPU_HOST_CONTEXT|requested=" +
        std::to_string(probe.requestedMajor) + "." +
        std::to_string(probe.requestedMinor) + "|created=" +
        std::to_string(probe.created ? 1U : 0U) + "|egl_error=" +
        std::to_string(probe.eglError) + "|actual=" +
        std::to_string(probe.actualMajor) + "." +
        std::to_string(probe.actualMinor) + "|vendor_hex=" +
        hexEncode(probe.vendor) + "|renderer_hex=" +
        hexEncode(probe.renderer) + "|version_hex=" +
        hexEncode(probe.version) + "|glsl_hex=" +
        hexEncode(probe.shadingLanguageVersion) + "|extensions=" +
        std::to_string(probe.extensions.size()) + "\n";
    writeCapabilityRecord(record);
    for (const auto& extension : probe.extensions) {
        record = "UML_GPU_HOST_EXTENSION|requested=" +
            std::to_string(probe.requestedMajor) + "." +
            std::to_string(probe.requestedMinor) + "|name_hex=" +
            hexEncode(extension) + "\n";
        writeCapabilityRecord(record);
    }
}

void reportCapset(
    std::uint32_t identifier,
    const VirglRenderer::Capset& capset)
{
    constexpr std::size_t chunkBytes = 256;
    const std::size_t chunks =
        (capset.data.size() + chunkBytes - 1U) / chunkBytes;
    writeCapabilityRecord(
        "UML_GPU_CAPSET_BEGIN|id=" + std::to_string(identifier) +
        "|version=" + std::to_string(capset.maximumVersion) +
        "|size=" + std::to_string(capset.data.size()) +
        "|chunks=" + std::to_string(chunks) + "\n");
    for (std::size_t index = 0; index < chunks; ++index) {
        const std::size_t offset = index * chunkBytes;
        const std::size_t size = std::min(
            chunkBytes, capset.data.size() - offset);
        writeCapabilityRecord(
            "UML_GPU_CAPSET_CHUNK|id=" + std::to_string(identifier) +
            "|index=" + std::to_string(index) + "|hex=" +
            hexEncode(std::span {capset.data}.subspan(offset, size)) + "\n");
    }
    writeCapabilityRecord(
        "UML_GPU_CAPSET_END|id=" + std::to_string(identifier) +
        "|chunks=" + std::to_string(chunks) + "\n");
}
#endif

void unavailableGlEntryPoint()
{
}

epoxy_resolver_stub_t handleMissingGlEntryPoint(const char* name) noexcept
{
    missingGlEntryPoint.store(true, std::memory_order_release);
    std::fprintf(
        stderr,
        "Virgl rejected unavailable host GL entry point: %s\n",
        name == nullptr ? "(unknown)" : name);
    /*
     * Libepoxy normally aborts here. Its supported failure-handler contract
     * allows a fallback stub; Virgl's public operation wrapper then turns the
     * recorded resolution failure into EIO for the guest context.
     */
    return &unavailableGlEntryPoint;
}

int checkGlDispatchResult(int result) noexcept
{
    if (!missingGlEntryPoint.exchange(false, std::memory_order_acq_rel))
        return result;
    return result == 0 ? EIO : result;
}

} // namespace

void VirglRenderer::setDeviceMemoryLimit(std::uint64_t bytes) noexcept
{
    deviceMemoryLimit.store(bytes, std::memory_order_release);
}

VirglRenderer::VirglRenderer()
    : provider_(std::make_unique<HostGlProvider>())
    , idleContexts_(idleTrimInterval().value_or(defaultIdleTrim))
    , idleTrimEnabled_(idleTrimInterval().has_value())
{
    if (rendererInstanceActive.exchange(true, std::memory_order_acq_rel)) {
        throw std::runtime_error(
            "an in-process Virgl renderer is already active");
    }
    bool initialized = false;
    try {
        epoxy_set_resolver_failure_handler(&handleMissingGlEntryPoint);
        // virgl_renderer_init() retains this pointer until cleanup rather than
        // copying the callback table. Keep it alive for the process lifetime
        // so deferred fence callbacks cannot refer to constructor storage.
        static virgl_renderer_callbacks callbacks = [] {
            virgl_renderer_callbacks result {};
            result.version = VIRGL_RENDERER_CALLBACKS_VERSION;
            result.write_fence = &VirglRenderer::writeFence;
            result.create_gl_context = &VirglRenderer::createGlContext;
            result.destroy_gl_context = &VirglRenderer::destroyGlContext;
            result.make_current = &VirglRenderer::makeGlContextCurrent;
            result.write_context_fence = &VirglRenderer::writeContextFence;
            result.get_egl_display = &VirglRenderer::getEglDisplay;
            return result;
        }();

        /*
         * The Mesa provider supplies an external EGLDisplay whose contexts are
         * desktop OpenGL 4.6 core. A vrend resource names the Metal texture
         * Zink stores it in through virgl_renderer_resource_get_info_ext, so
         * its scanout is presented directly and read back only when that
         * lookup fails.
         *
         * The render server publishes each context's per-ring timeline
         * sequence numbers in shared memory, so virgl_renderer_context_poll
         * retires Venus fences and invokes the fence callback on the polling
         * thread. VIRGL_RENDERER_ASYNC_FENCE_CB would instead move retirement
         * onto renderer-owned sync threads, forbid that poll, and make Virgl
         * fence delivery asynchronous too.
         */
#if UML_GPU_SOFTWARE_RENDERER
        // The Simulator renderer serves Virgl only: it has no Vulkan driver,
        // so there is no Venus, no render server and no device-memory pool.
        const int rendererFlags = 0;
        deviceMemoryPool.reset();
        virgl_renderer_set_device_memory_pool(nullptr);
#else
        const int rendererFlags =
            VIRGL_RENDERER_VENUS |
            VIRGL_RENDERER_RENDER_SERVER;
        /*
         * Host-visible Venus memory reaches the guest only through the blob
         * aperture, so the renderer advertises a host-visible heap the
         * aperture can deliver rather than the host's system memory. A guest
         * sizes its largest host-visible mapping from that heap.
         */
        virgl_renderer_set_host_visible_aperture_size(
            protocol::hostVisibleSharedMemoryRegionSize);
        /*
         * GPU memory is charged to this process like the guest's own RAM.
         * The guest is shown, and may allocate, only the share the host
         * granted it, counted against everything the GPU holds for the
         * process so that the presentation path and the host GL driver draw
         * on the same share. Guest allocations and the memory the Venus
         * devices allocate for themselves reserve from the share before they
         * allocate, in the host pages Metal counts.
         */
        const std::uint64_t limit =
            deviceMemoryLimit.load(std::memory_order_acquire);
        deviceMemoryPool.reset();
        if (limit != 0) {
            /*
             * What the host GPU holds is Metal's allocation counter less the
             * host memory the drivers only import, which holds the
             * IOSurfaces, plus what the graphics ledger jetsam counts holds
             * beyond the heaps both count, the driver's command storage. iOS
             * keeps an import's file-backed pages outside the footprint. The
             * readings go in the order accountedGpuBytes() needs: imports
             * before the counter, the resident heaps last.
             */
            deviceMemoryPool = std::make_unique<DeviceMemoryPool>(limit,
                [] {
                    const std::uint64_t imported =
                        virgl_renderer_get_imported_host_memory();
                    const std::uint64_t metal =
                        uml::platform::hostGpuAllocatedBytes();
                    const std::uint64_t ledger =
                        uml::platform::hostGraphicsFootprintBytes();
                    const std::uint64_t heaps =
                        virgl_renderer_get_resident_heap_memory();
                    const std::uint64_t held =
                        accountedGpuBytes(metal, imported, ledger, heaps);
                    /* For the session record's memory checkpoint, which
                     * reads it without waiting on the pool. */
                    uml::platform::publishGpuMemoryHeld(held);
                    return held;
                },
                static_cast<std::uint64_t>(getpagesize()),
                /* The counter alone follows a release at once, the ledger
                 * later: the pool tells a release the ledger has yet to
                 * follow from a full budget by comparing this reading with
                 * its last, and waits for the ledger. */
                [] {
                    const std::uint64_t imported =
                        virgl_renderer_get_imported_host_memory();
                    return madeGpuBytes(
                        uml::platform::hostGpuAllocatedBytes(), imported);
                },
                deviceMemoryReserveBytes);
            uml::platform::publishGpuMemoryBudget(limit);
            /* A Venus guest is shown, and budgets, what the pool admits. */
            const virgl_renderer_device_memory_pool pool {
                .limit = deviceMemoryPool->ceiling(),
                .data = deviceMemoryPool.get(),
                .usage = &deviceMemoryPoolUsage,
                .reserve = &reserveDeviceMemory,
                .release = &releaseDeviceMemory,
                .refused = &deviceMemoryRefused,
                .account = &deviceMemoryAccounted,
            };
            virgl_renderer_set_device_memory_pool(&pool);
        } else {
            virgl_renderer_set_device_memory_pool(nullptr);
        }
#endif
        const int result =
            virgl_renderer_init(this, rendererFlags, &callbacks);
        const int checkedResult = checkGlDispatchResult(result);
        if (checkedResult != 0) {
            if (result == 0)
                virgl_renderer_cleanup(this);
            throw std::runtime_error(
                "failed to initialize in-process Virgl renderer: " +
                std::to_string(checkedResult));
        }
        initialized = true;
        if (deviceMemoryPool) {
            std::lock_guard lock(deviceMemoryReadMutex);
            deviceMemoryReadable = true;
        }
        try {
            /* The renderer's own context is where a resource is made and
             * transferred when the guest names no context of its own. */
            idleContexts_.add(hostContextId, ContextIdleTracker::Clock::now());
        } catch (...) {
            // A context that is not tracked is never trimmed; that is all.
        }
        if (deviceMemoryPool) {
            /* A refused reservation first frees what the host GL driver
             * caches. The hook reaches this renderer's provider, so it is
             * cleared before the provider goes. */
            deviceMemoryPool->setReclaim(
                [provider = provider_.get()]() -> std::uint64_t {
                    return provider->releaseCachedMemory();
                });
        }
#ifndef UML_APP_STORE_RUNTIME
        if (capabilityReportEnabled()) {
            // vrend asks for 4.6 first, the core context this provider serves.
            reportContextProbe(provider_->probeContext(4, 6));
            reportCapset(virglCapsetId, capset(virglCapsetId));
            reportCapset(virgl2CapsetId, capset(virgl2CapsetId));
#if !UML_GPU_SOFTWARE_RENDERER
            reportCapset(venusCapsetId, capset(venusCapsetId));
#endif
        }
#endif
    } catch (...) {
        {
            std::lock_guard lock(deviceMemoryReadMutex);
            deviceMemoryReadable = false;
        }
        if (deviceMemoryPool)
            deviceMemoryPool->setReclaim({});
        if (initialized)
            virgl_renderer_cleanup(this);
        rendererInstanceActive.store(false, std::memory_order_release);
        throw;
    }
}

void VirglRenderer::setMemoryRefusalObserver(
    MemoryRefusalObserver observer, void* data) noexcept
{
    if (observer == nullptr)
        memoryRefusalObserver.store(nullptr, std::memory_order_release);
    memoryRefusalObserverData.store(data, std::memory_order_relaxed);
    if (observer != nullptr)
        memoryRefusalObserver.store(observer, std::memory_order_release);
}

void VirglRenderer::setMemoryAccountObserver(
    MemoryAccountObserver observer, void* data) noexcept
{
    if (observer == nullptr)
        memoryAccountObserver.store(nullptr, std::memory_order_release);
    memoryAccountObserverData.store(data, std::memory_order_relaxed);
    if (observer != nullptr)
        memoryAccountObserver.store(observer, std::memory_order_release);
}

std::optional<VirglRenderer::DeviceMemory> VirglRenderer::deviceMemory() noexcept
{
    std::lock_guard lock(deviceMemoryReadMutex);
    if (!deviceMemoryReadable || !deviceMemoryPool)
        return std::nullopt;
    return DeviceMemory {
        .budgetBytes = deviceMemoryPool->limit(),
        .usageBytes = deviceMemoryPool->usage(),
        .ceilingBytes = deviceMemoryPool->ceiling(),
    };
}

std::uint64_t VirglRenderer::reclaimCachedMemory() noexcept
{
    std::lock_guard lock(deviceMemoryReadMutex);
    if (!deviceMemoryReadable || !deviceMemoryPool)
        return 0;
    return deviceMemoryPool->reclaim();
}

VirglRenderer::~VirglRenderer()
{
    // The render server's threads stop with the cleanup below, and nothing
    // they report is wanted after that.
    setMemoryRefusalObserver(nullptr, nullptr);
    setMemoryAccountObserver(nullptr, nullptr);
    {
        // A reader of the pool's figures that is inside the pool finishes
        // first, and none starts after this.
        std::lock_guard lock(deviceMemoryReadMutex);
        deviceMemoryReadable = false;
    }
    if (deviceMemoryPool)
        deviceMemoryPool->setReclaim({});
    virgl_renderer_cleanup(this);
    /* The pool is the renderer's: nothing the cleanup left can reach it, and
     * the guest that comes after this one starts with nothing reserved. */
    virgl_renderer_set_device_memory_pool(nullptr);
    if (deviceMemoryPool)
        uml::platform::publishGpuMemoryBudget(0);
    deviceMemoryPool.reset();
    rendererInstanceActive.store(false, std::memory_order_release);
}

VirglRenderer::Capset VirglRenderer::capset(std::uint32_t capsetId) const
{
    Capset result;
    std::uint32_t maximumSize = 0;
    virgl_renderer_get_cap_set(
        capsetId, &result.maximumVersion, &maximumSize);
    if (maximumSize == 0)
        throw std::runtime_error("Virgl renderer returned an empty capset");
    if (capsetId == virglCapsetId || capsetId == virgl2CapsetId) {
        /*
         * vrend fills the Virgl capsets from whatever GL context is current
         * on this thread, and reads its vendor and renderer strings without
         * checking them. A guest context whose creation failed can leave the
         * thread with none, and the next GET_CAPSET then dereferenced a null
         * string. Fill them from the renderer's root context, and refuse the
         * query when even that cannot be made current, as when the display
         * under it has been terminated.
         */
        virgl_renderer_force_ctx_0();
        if (glGetString(GL_VENDOR) == nullptr ||
            glGetString(GL_RENDERER) == nullptr) {
            throw std::runtime_error(
                "Virgl capset has no current host GL context");
        }
    }
    result.data.resize(maximumSize);
    virgl_renderer_fill_caps(
        capsetId, result.maximumVersion, result.data.data());
    if (checkGlDispatchResult(0) != 0) {
        throw std::runtime_error(
            "Virgl capset requested an unavailable host GL entry point");
    }
    return result;
}

int VirglRenderer::createContext(
    std::uint32_t contextId,
    std::uint32_t flags,
    std::string_view debugName) noexcept
{
    if (debugName.size() > std::numeric_limits<std::uint32_t>::max())
        return EINVAL;
    const int createResult = virgl_renderer_context_create_with_flags(
        contextId,
        flags,
        static_cast<std::uint32_t>(debugName.size()),
        debugName.data());
    const int checkedResult = checkGlDispatchResult(createResult);
    if (checkedResult != 0) {
        if (createResult == 0)
            virgl_renderer_context_destroy(contextId);
        return checkedResult;
    }

    if ((flags & VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK) ==
        venusCapsetId) {
        return 0;
    }

    /*
     * Context creation leaves the new GL context current without updating
     * virglrenderer's current-context bookkeeping. If another guest context
     * was current, its next submission can consequently execute in the new
     * context and cache GL state there. Return to context zero so the next
     * guest submission performs an explicit, correctly tracked switch.
     */
    virgl_renderer_force_ctx_0();
    if (checkGlDispatchResult(0) != 0) {
        virgl_renderer_context_destroy(contextId);
        return EIO;
    }
    try {
        idleContexts_.add(contextId, ContextIdleTracker::Clock::now());
    } catch (...) {
        // A context that is not tracked is never trimmed; that is all.
    }
    return 0;
}

void VirglRenderer::destroyContext(std::uint32_t contextId) noexcept
{
    virgl_renderer_context_destroy(contextId);
    try {
        idleContexts_.remove(contextId);
        // Destroying a context deletes the textures only it held.
        idleContexts_.noteRelease(ContextIdleTracker::Clock::now());
        std::lock_guard lock(mutex_);
        std::erase_if(
            retiredContextFences_,
            [contextId](const auto& entry) {
                return static_cast<std::uint32_t>(entry.first >> 8U) ==
                    contextId;
            });
    } catch (...) {
        // Never carry a C++ exception through teardown or the C renderer API.
    }
}

int VirglRenderer::submit(
    std::uint32_t contextId,
    std::span<const std::uint8_t> command) noexcept
{
    constexpr std::size_t commandWordSize = sizeof(std::uint32_t);
    if (command.size() % commandWordSize != 0 ||
        command.size() / commandWordSize >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return EINVAL;
    }

    noteContextActivity(contextId);
    // virglrenderer accepts an immutable command but retains a legacy void*
    // API. The implementation does not modify this storage.
    return checkGlDispatchResult(
        virgl_renderer_submit_cmd(
            const_cast<std::uint8_t*>(command.data()),
            static_cast<int>(contextId),
            static_cast<int>(command.size() / commandWordSize)));
}

int VirglRenderer::createResource3d(
    const Resource3d& resource) noexcept
{
    noteContextActivity(hostContextId);
    virgl_renderer_resource_create_args arguments {
        .handle = resource.resourceId,
        .target = resource.target,
        .format = resource.format,
        .bind = resource.bind,
        .width = resource.width,
        .height = resource.height,
        .depth = resource.depth,
        .array_size = resource.arraySize,
        .last_level = resource.lastLevel,
        .nr_samples = resource.sampleCount,
        .flags = resource.flags,
    };
    const int createResult =
        virgl_renderer_resource_create(&arguments, nullptr, 0);
    const int checkedResult = checkGlDispatchResult(createResult);
    if (createResult == 0 && checkedResult != 0)
        virgl_renderer_resource_unref(resource.resourceId);
    return checkedResult;
}

int VirglRenderer::createResourceBlob(
    const ResourceBlob& resource) noexcept
{
    /* A blob is renderer storage; the device refuses guest backing for one,
     * so no iovec reaches the renderer. */
    virgl_renderer_resource_create_blob_args arguments {
        .res_handle = resource.resourceId,
        .ctx_id = resource.contextId,
        .blob_mem = resource.memory,
        .blob_flags = resource.flags,
        .blob_id = resource.blobId,
        .size = resource.size,
        .iovecs = nullptr,
        .num_iovs = 0,
    };
    return virgl_renderer_resource_create_blob(&arguments);
}

int VirglRenderer::exportResourceBlob(
    std::uint32_t resourceId,
    std::uint32_t& descriptorType,
    int& descriptor) noexcept
{
    descriptorType = 0;
    descriptor = -1;
    return virgl_renderer_resource_export_blob(
        resourceId, &descriptorType, &descriptor);
}

int VirglRenderer::resourceMapInfo(
    std::uint32_t resourceId,
    std::uint32_t& mapInfo) noexcept
{
    mapInfo = 0;
    return virgl_renderer_resource_get_map_info(resourceId, &mapInfo);
}

int VirglRenderer::mapResource(
    std::uint32_t resourceId,
    void*& mapping,
    std::uint64_t& size) noexcept
{
    mapping = nullptr;
    size = 0;
    return virgl_renderer_resource_map(resourceId, &mapping, &size);
}

void VirglRenderer::unmapResource(std::uint32_t resourceId) noexcept
{
    (void)virgl_renderer_resource_unmap(resourceId);
}

int VirglRenderer::createBlobScanout(
    std::uint32_t resourceId,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t format,
    std::uint32_t stride,
    std::uint32_t offset,
    void*& texture) noexcept
{
    texture = nullptr;
    const auto type = virgl_renderer_create_handle_for_scanout(
        resourceId, width, height, format, 0, stride, offset, &texture);
    if (type != VIRGL_NATIVE_HANDLE_METAL_TEXTURE || texture == nullptr) {
        if (type != VIRGL_NATIVE_HANDLE_NONE && texture != nullptr)
            virgl_renderer_release_handle_for_scanout(type, texture);
        texture = nullptr;
        return ENOTSUP;
    }
    return 0;
}

void VirglRenderer::releaseBlobScanout(void* texture) noexcept
{
    if (texture != nullptr) {
        virgl_renderer_release_handle_for_scanout(
            VIRGL_NATIVE_HANDLE_METAL_TEXTURE, texture);
    }
}

int VirglRenderer::transferResource(
    bool toHost,
    std::uint32_t resourceId,
    std::uint32_t contextId,
    std::uint32_t level,
    std::uint32_t stride,
    std::uint32_t layerStride,
    const Box& box,
    std::uint64_t offset) noexcept
{
    noteContextActivity(contextId);
    virgl_box transferBox {
        .x = box.x,
        .y = box.y,
        .z = box.z,
        .w = box.width,
        .h = box.height,
        .d = box.depth,
    };
    if (toHost) {
        return checkGlDispatchResult(
            virgl_renderer_transfer_write_iov(
                resourceId,
                contextId,
                static_cast<int>(level),
                stride,
                layerStride,
                &transferBox,
                offset,
                nullptr,
                0));
    }
    return checkGlDispatchResult(
        virgl_renderer_transfer_read_iov(
            resourceId,
            contextId,
            level,
            stride,
            layerStride,
            &transferBox,
            offset,
            nullptr,
            0));
}

int VirglRenderer::readResourceRectangle(
    std::uint32_t resourceId,
    std::span<std::uint8_t> destination,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    if (width == 0 || height == 0)
        return EINVAL;
    if (width >
        std::numeric_limits<std::uint32_t>::max() /
            protocol::gpuBytesPerPixel) {
        return EOVERFLOW;
    }
    const auto stride = width * protocol::gpuBytesPerPixel;
    if (height > destination.size() / stride)
        return ENOSPC;
    noteContextActivity(hostContextId);
    iovec vector {
        .iov_base = destination.data(),
        .iov_len = destination.size(),
    };
    virgl_box box {
        .x = x,
        .y = y,
        .z = 0,
        .w = width,
        .h = height,
        .d = 1,
    };
    return checkGlDispatchResult(
        virgl_renderer_transfer_read_iov(
            resourceId,
            0,
            0,
            stride,
            0,
            &box,
            0,
            &vector,
            1));
}

int VirglRenderer::resourceMetalTexture(
    std::uint32_t resourceId,
    NativeTexture& texture) noexcept
{
    texture = {};
    virgl_renderer_resource_info_ext information {};
    information.version = VIRGL_RENDERER_RESOURCE_INFO_EXT_VERSION;
    const int result = virgl_renderer_resource_get_info_ext(
        static_cast<int>(resourceId), &information);
    const int checkedResult = checkGlDispatchResult(result);
    if (checkedResult != 0)
        return checkedResult;
    if (information.native_type != VIRGL_NATIVE_HANDLE_METAL_TEXTURE ||
        information.native_handle == nullptr) {
        return EINVAL;
    }
    texture = NativeTexture {
        .texture = information.native_handle,
        .format = information.base.virgl_format,
        .width = information.base.width,
        .height = information.base.height,
        .bottomUp =
            (information.base.flags & VIRGL_RESOURCE_Y_0_TOP) != 0,
    };
    return 0;
}

int VirglRenderer::createFenceAndWait(
    std::uint32_t contextId,
    std::uint32_t ringIndex,
    std::uint64_t fenceId,
    const std::atomic_bool* stopRequested) noexcept
{
    try {
        if (fenceCallbackFailed_.load(std::memory_order_acquire))
            return EIO;
        if (stopRequested != nullptr &&
            stopRequested->load(std::memory_order_acquire)) {
            return ECANCELED;
        }
        const bool contextZero = contextId == 0;
        std::uint64_t key = 0;
        int result = 0;
        if (contextZero) {
            if (fenceId > std::numeric_limits<std::uint32_t>::max())
                return EOVERFLOW;
            result = virgl_renderer_create_fence(
                static_cast<int>(fenceId), contextId);
        } else {
            // Virgl fences retire in order within each context and ring.
            constexpr std::uint32_t mergeableFence =
                VIRGL_RENDERER_FENCE_FLAG_MERGEABLE;
            key = contextRingKey(contextId, ringIndex);
            {
                // Publish the callback slot before the renderer can retire the
                // fence. The C callback then performs only allocation-free
                // lookup and update and cannot throw across its ABI boundary.
                std::lock_guard lock(mutex_);
                retiredContextFences_.try_emplace(key, 0);
            }
            result = virgl_renderer_context_create_fence(
                contextId, mergeableFence, ringIndex, fenceId);
        }
        result = checkGlDispatchResult(result);
        if (result != 0)
            return result;

        const auto deadline =
            std::chrono::steady_clock::now() + maximumFenceWait;
        // A sub-millisecond interval avoids adding a full scheduler tick to
        // the many short Metal fences emitted by compositors and video
        // playback, while the timed wait still keeps this thread off-CPU.
        constexpr auto pollingInterval = std::chrono::microseconds(100);
        std::unique_lock lock(mutex_);
        while (true) {
            if (fenceCallbackFailed_.load(std::memory_order_acquire))
                return EIO;
            const bool retired = contextZero
                ? retiredFence_ >= fenceId
                : [&] {
                    const auto iterator = retiredContextFences_.find(key);
                    return iterator != retiredContextFences_.end() &&
                        iterator->second >= fenceId;
                }();
            if (retired)
                return 0;
            if (stopRequested != nullptr &&
                stopRequested->load(std::memory_order_acquire)) {
                return ECANCELED;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return ETIMEDOUT;

            lock.unlock();
            if (contextZero)
                virgl_renderer_poll();
            else
                virgl_renderer_context_poll(contextId);
            if (checkGlDispatchResult(0) != 0)
                return EIO;
            lock.lock();
            /*
             * Polling invokes the fence callback synchronously on the common
             * non-threaded renderer path. Recheck the callback-owned state in
             * the wait predicate: an unconditional timed wait here loses the
             * notification which was issued by that just-completed poll and
             * adds a full polling interval to every retired fence.
             */
            fenceCondition_.wait_until(
                lock,
                std::min(deadline, now + pollingInterval),
                [&] {
                    if (fenceCallbackFailed_.load(
                            std::memory_order_acquire)) {
                        return true;
                    }
                    if (stopRequested != nullptr &&
                        stopRequested->load(std::memory_order_acquire)) {
                        return true;
                    }
                    if (contextZero)
                        return retiredFence_ >= fenceId;
                    const auto iterator =
                        retiredContextFences_.find(key);
                    return iterator != retiredContextFences_.end() &&
                        iterator->second >= fenceId;
                });
        }
    } catch (...) {
        return EIO;
    }
}

int VirglRenderer::createContextFence(
    std::uint32_t contextId,
    std::uint32_t ringIndex,
    std::uint64_t fenceCookie) noexcept
{
    try {
        /* Venus places each guest ring on its own fence timeline, so the ring
         * index is bounded by the ring count the Venus protocol allows rather
         * than pinned to the single virgl ring. */
        if (contextId == 0 ||
            ringIndex >= protocol::venusContextRingCount ||
            fenceCookie == 0) {
            return EINVAL;
        }
        if (fenceCallbackFailed_.load(std::memory_order_acquire))
            return EIO;

        const auto key = contextRingKey(contextId, ringIndex);
        {
            /* Publish the callback slot before the renderer can retire the
             * fence. A synchronously retired fence is then observable by the
             * caller immediately after this function returns. */
            std::lock_guard lock(mutex_);
            retiredContextFences_.try_emplace(key, 0);
        }
        constexpr std::uint32_t mergeableFence =
            VIRGL_RENDERER_FENCE_FLAG_MERGEABLE;
        const int result =
            checkGlDispatchResult(virgl_renderer_context_create_fence(
                contextId, mergeableFence, ringIndex, fenceCookie));
        if (result == 0) {
            idleContexts_.fenceCreated(
                contextId,
                ringIndex,
                fenceCookie,
                ContextIdleTracker::Clock::now());
        }
        return result;
    } catch (const std::bad_alloc&) {
        return ENOMEM;
    } catch (...) {
        return EIO;
    }
}

int VirglRenderer::pollContextFences(std::uint32_t contextId) noexcept
{
    if (contextId == 0)
        return EINVAL;
    if (fenceCallbackFailed_.load(std::memory_order_acquire))
        return EIO;
    virgl_renderer_context_poll(contextId);
    if (checkGlDispatchResult(0) != 0 ||
        fenceCallbackFailed_.load(std::memory_order_acquire)) {
        return EIO;
    }
    return 0;
}

int VirglRenderer::pollRenderer() noexcept
{
    if (fenceCallbackFailed_.load(std::memory_order_acquire))
        return EIO;
    virgl_renderer_poll();
    if (checkGlDispatchResult(0) != 0 ||
        fenceCallbackFailed_.load(std::memory_order_acquire)) {
        return EIO;
    }
    return 0;
}

bool VirglRenderer::hasPendingWork() noexcept
{
    return virgl_renderer_has_pending_work();
}

int VirglRenderer::contextFenceStatus(
    std::uint32_t contextId,
    std::uint32_t ringIndex,
    std::uint64_t fenceCookie) noexcept
{
    try {
        if (contextId == 0 ||
            ringIndex >= protocol::venusContextRingCount ||
            fenceCookie == 0) {
            return EINVAL;
        }
        if (fenceCallbackFailed_.load(std::memory_order_acquire))
            return EIO;
        const auto key = contextRingKey(contextId, ringIndex);
        std::lock_guard lock(mutex_);
        const auto iterator = retiredContextFences_.find(key);
        if (iterator == retiredContextFences_.end())
            return EIO;
        return iterator->second >= fenceCookie ? 0 : EBUSY;
    } catch (...) {
        return EIO;
    }
}

std::uint64_t VirglRenderer::resourceStorageBytes(
    std::uint32_t resourceId) noexcept
{
    return virgl_renderer_resource_get_storage_size(
        static_cast<int>(resourceId));
}

void VirglRenderer::unrefResource(std::uint32_t resourceId) noexcept
{
    virgl_renderer_resource_unref(resourceId);
    noteResourceReleased();
}

int VirglRenderer::attachBacking(
    std::uint32_t resourceId,
    std::span<iovec> backing) noexcept
{
    if (backing.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return EINVAL;
    }
    const int result = virgl_renderer_resource_attach_iov(
        static_cast<int>(resourceId),
        backing.data(),
        static_cast<int>(backing.size()));
    if (result != 0) {
        virgl_renderer_resource_detach_iov(
            static_cast<int>(resourceId), nullptr, nullptr);
    }
    return result;
}

void VirglRenderer::detachBacking(std::uint32_t resourceId) noexcept
{
    virgl_renderer_resource_detach_iov(
        static_cast<int>(resourceId), nullptr, nullptr);
}

int VirglRenderer::attachResource(
    std::uint32_t contextId,
    std::uint32_t resourceId) noexcept
{
    virgl_renderer_ctx_attach_resource(
        static_cast<int>(contextId), static_cast<int>(resourceId));
    const int result = checkGlDispatchResult(0);
    if (result != 0) {
        virgl_renderer_ctx_detach_resource(
            static_cast<int>(contextId), static_cast<int>(resourceId));
    }
    return result;
}

int VirglRenderer::detachResource(
    std::uint32_t contextId,
    std::uint32_t resourceId) noexcept
{
    virgl_renderer_ctx_detach_resource(
        static_cast<int>(contextId), static_cast<int>(resourceId));
    const int result = checkGlDispatchResult(0);
    if (result != 0) {
        virgl_renderer_ctx_attach_resource(
            static_cast<int>(contextId), static_cast<int>(resourceId));
        return result;
    }
    noteResourceReleased();
    return result;
}

void VirglRenderer::resetFenceState() noexcept
{
    try {
        idleContexts_.reset();
        // The renderer's own context outlives the guest's contexts.
        idleContexts_.add(hostContextId, ContextIdleTracker::Clock::now());
        std::lock_guard lock(mutex_);
        retiredFence_ = 0;
        retiredContextFences_.clear();
        fenceCallbackFailed_.store(false, std::memory_order_release);
    } catch (...) {
    }
}

void* VirglRenderer::createGlContext(
    void* cookie,
    int,
    virgl_renderer_gl_ctx_param* parameters) noexcept
{
    if (cookie == nullptr || parameters == nullptr)
        return nullptr;
    auto& renderer = *static_cast<VirglRenderer*>(cookie);
    return renderer.provider_->createContext(*parameters);
}

void VirglRenderer::destroyGlContext(void* cookie, void* context) noexcept
{
    if (cookie == nullptr)
        return;
    auto& renderer = *static_cast<VirglRenderer*>(cookie);
    renderer.provider_->destroyContext(context);
}

int VirglRenderer::makeGlContextCurrent(
    void* cookie, int, void* context) noexcept
{
    if (cookie == nullptr)
        return -EIO;
    auto& renderer = *static_cast<VirglRenderer*>(cookie);
    return renderer.provider_->makeCurrent(context);
}

void* VirglRenderer::getEglDisplay(void* cookie) noexcept
{
    if (cookie == nullptr)
        return nullptr;
    auto& renderer = *static_cast<VirglRenderer*>(cookie);
    return renderer.provider_->display();
}

void VirglRenderer::writeFence(
    void* cookie, std::uint32_t fenceId) noexcept
{
    try {
        auto& renderer = *static_cast<VirglRenderer*>(cookie);
        {
            std::lock_guard lock(renderer.mutex_);
            renderer.retiredFence_ = std::max(renderer.retiredFence_, fenceId);
        }
        renderer.fenceCondition_.notify_all();
    } catch (...) {
        auto& renderer = *static_cast<VirglRenderer*>(cookie);
        renderer.fenceCallbackFailed_.store(true, std::memory_order_release);
        renderer.fenceCondition_.notify_all();
    }
}

void VirglRenderer::writeContextFence(
    void* cookie,
    std::uint32_t contextId,
    std::uint32_t ringIndex,
    std::uint64_t fenceId) noexcept
{
    try {
        auto& renderer = *static_cast<VirglRenderer*>(cookie);
        {
            std::lock_guard lock(renderer.mutex_);
            const auto iterator = renderer.retiredContextFences_.find(
                contextRingKey(contextId, ringIndex));
            if (iterator != renderer.retiredContextFences_.end())
                iterator->second = std::max(iterator->second, fenceId);
        }
        renderer.idleContexts_.fenceRetired(
            contextId,
            ringIndex,
            fenceId,
            ContextIdleTracker::Clock::now());
        renderer.fenceCondition_.notify_all();
    } catch (...) {
        auto& renderer = *static_cast<VirglRenderer*>(cookie);
        renderer.fenceCallbackFailed_.store(true, std::memory_order_release);
        renderer.fenceCondition_.notify_all();
    }
}

void VirglRenderer::noteContextActivity(std::uint32_t contextId) noexcept
{
    try {
        idleContexts_.activity(contextId, ContextIdleTracker::Clock::now());
    } catch (...) {
    }
}

void VirglRenderer::noteResourceReleased() noexcept
{
    try {
        idleContexts_.noteRelease(ContextIdleTracker::Clock::now());
    } catch (...) {
    }
}

std::optional<std::chrono::steady_clock::duration>
VirglRenderer::nextIdleTrimDelay() const noexcept
{
    if (!idleTrimEnabled_)
        return std::nullopt;
    try {
        return idleContexts_.nextDelay(ContextIdleTracker::Clock::now());
    } catch (...) {
        return std::nullopt;
    }
}

void VirglRenderer::trimIdleContexts() noexcept
{
    if (!idleTrimEnabled_)
        return;
    try {
        for (const auto contextId : idleContexts_.takeIdle(
                 ContextIdleTracker::Clock::now())) {
            virgl_renderer_context_trim(contextId);
            // A missing GL entry point is reported through the next call that
            // checks; trimming has no result to carry it.
            (void)checkGlDispatchResult(0);
        }
    } catch (...) {
        // Never carry a C++ exception through the C renderer API.
    }
}

std::uint64_t VirglRenderer::contextRingKey(
    std::uint32_t contextId,
    std::uint32_t ringIndex) noexcept
{
    // The Virtio-GPU header carries an 8-bit ring index.
    return (static_cast<std::uint64_t>(contextId) << 8U) |
        (ringIndex & 0xffU);
}

} // namespace uml::gpu
