// SPDX-License-Identifier: GPL-2.0-or-later
#include "storage.hpp"

#include <drm_fourcc.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <dlfcn.h>
#include <fcntl.h>

#include <cerrno>
#include <cstdio>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <va/va.h>

namespace uml::va {
namespace {

constexpr unsigned kGbmBoUseLinear = 1u << 4;    // GBM_BO_USE_LINEAR
constexpr unsigned kGbmBoUseRendering = 1u << 2; // GBM_BO_USE_RENDERING

// libgbm is loaded on demand so that a guest without a usable graphics stack
// still gets a working driver with plain memory surfaces.
struct Gbm {
    void* library {};
    void* device {};
    void* (*createDevice)(int) {};
    void* (*boCreate)(void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {};
    void (*boDestroy)(void*) {};
    int (*boGetFd)(void*) {};
    std::uint32_t (*boGetStrideForPlane)(void*, int) {};
    std::uint32_t (*boGetOffset)(void*, int) {};
    int (*boGetPlaneCount)(void*) {};
    std::uint64_t (*boGetModifier)(void*) {};
    void* (*boMap)(void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                   std::uint32_t*, void**) {};
    void (*boUnmap)(void*, void*) {};
    int drm {-1};
};

int& clientDrm() noexcept
{
    static int descriptor = -1;
    return descriptor;
}

const char*& reason() noexcept
{
    static const char* text = "not probed";
    return text;
}

Gbm& gbm()
{
    static Gbm instance;
    static std::once_flag once;
    std::call_once(once, [] {
        instance.library = dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!instance.library) return;
        const auto symbol = [](void* library, const char* name) { return dlsym(library, name); };
        instance.createDevice = reinterpret_cast<void* (*)(int)>(symbol(instance.library, "gbm_create_device"));
        instance.boCreate = reinterpret_cast<void* (*)(void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t)>(
            symbol(instance.library, "gbm_bo_create"));
        instance.boDestroy = reinterpret_cast<void (*)(void*)>(symbol(instance.library, "gbm_bo_destroy"));
        instance.boGetFd = reinterpret_cast<int (*)(void*)>(symbol(instance.library, "gbm_bo_get_fd"));
        instance.boGetStrideForPlane = reinterpret_cast<std::uint32_t (*)(void*, int)>(
            symbol(instance.library, "gbm_bo_get_stride_for_plane"));
        instance.boGetOffset = reinterpret_cast<std::uint32_t (*)(void*, int)>(symbol(instance.library, "gbm_bo_get_offset"));
        instance.boGetPlaneCount = reinterpret_cast<int (*)(void*)>(symbol(instance.library, "gbm_bo_get_plane_count"));
        instance.boGetModifier = reinterpret_cast<std::uint64_t (*)(void*)>(symbol(instance.library, "gbm_bo_get_modifier"));
        instance.boMap = reinterpret_cast<void* (*)(void*, std::uint32_t, std::uint32_t, std::uint32_t,
                                                    std::uint32_t, std::uint32_t, std::uint32_t*, void**)>(
            symbol(instance.library, "gbm_bo_map"));
        instance.boUnmap = reinterpret_cast<void (*)(void*, void*)>(symbol(instance.library, "gbm_bo_unmap"));
        if (!instance.createDevice || !instance.boCreate || !instance.boGetFd || !instance.boGetStrideForPlane ||
            !instance.boGetOffset || !instance.boGetPlaneCount) return;
        if (const int client = clientDrm(); client >= 0) {
            instance.drm = ::fcntl(client, F_DUPFD_CLOEXEC, 0);
            if (instance.drm >= 0) {
                instance.device = instance.createDevice(instance.drm);
                if (!instance.device) { ::close(instance.drm); instance.drm = -1; }
            }
        }
        const char* node = std::getenv("UML_VA_DRM_DEVICE");
        for (const char* candidate : {node, "/dev/dri/renderD128", "/dev/dri/card0"}) {
            if (instance.device) break;
            if (!candidate || !*candidate) continue;
            instance.drm = ::open(candidate, O_RDWR | O_CLOEXEC);
            if (instance.drm < 0) continue;
            instance.device = instance.createDevice(instance.drm);
            if (instance.device) break;
            ::close(instance.drm);
            instance.drm = -1;
        }
    });
    return instance;
}

std::uint32_t drmFormat(std::uint32_t fourcc) noexcept
{
    switch (fourcc) {
    case VA_FOURCC_NV12: return DRM_FORMAT_NV12;
    case VA_FOURCC_BGRA: return DRM_FORMAT_ABGR8888;
    case VA_FOURCC_RGBA: return DRM_FORMAT_ABGR8888;
    case VA_FOURCC_ARGB: return DRM_FORMAT_ARGB8888;
    case VA_FOURCC_ABGR: return DRM_FORMAT_ABGR8888;
    case VA_FOURCC_BGRX: return DRM_FORMAT_XRGB8888;
    default: return 0;
    }
}

StorageKind& selection() noexcept
{
    static StorageKind kind = StorageKind::memory;
    return kind;
}

bool& selectionDone() noexcept
{
    static bool done = false;
    return done;
}

}

std::uint64_t descriptorIdentity(int fd) noexcept
{
    struct stat status {};
    if (fd < 0 || fstat(fd, &status) < 0) return 0;
    return std::uint64_t(status.st_ino);
}

bool planeLayout(std::uint32_t fourcc, unsigned width, unsigned height, unsigned& planes,
                 std::size_t* pitch, std::size_t* offset, std::size_t& size) noexcept
{
    const std::size_t even = (std::size_t(width) + 1) & ~std::size_t(1);
    const std::size_t rows = (std::size_t(height) + 1) & ~std::size_t(1);
    switch (fourcc) {
    case VA_FOURCC_NV12:
        planes = 2;
        pitch[0] = even;
        offset[0] = 0;
        pitch[1] = even;
        offset[1] = pitch[0] * rows;
        size = offset[1] + pitch[1] * (rows / 2);
        return true;
    case VA_FOURCC_BGRA:
    case VA_FOURCC_RGBA:
    case VA_FOURCC_ARGB:
    case VA_FOURCC_ABGR:
    case VA_FOURCC_BGRX:
    case VA_FOURCC_RGBX:
        planes = 1;
        pitch[0] = std::size_t(width) * 4;
        offset[0] = 0;
        size = pitch[0] * height;
        return true;
    default:
        return false;
    }
}

void selectStorage(const char* request)
{
    if (selectionDone()) return;
    selectionDone() = true;
    const std::string wanted = request && *request ? request : "auto";
    if (wanted == "memory" || wanted == "memfd") { selection() = StorageKind::memory; reason() = "requested"; return; }
    if (wanted == "gbm") { selection() = StorageKind::gbm; reason() = "requested"; return; }
    // Auto: keep GBM only when a linear NV12 buffer really can be allocated,
    // exported and written to, because the decoder copies each picture into it
    // and the guest graphics stack has not always allowed all three.
    std::string error;
    selection() = StorageKind::gbm;
    auto& loader = gbm();
    if (!loader.library) reason() = "no libgbm";
    else if (!loader.device) reason() = "no DRM device";
    else {
        auto probe = PixelBuffer::allocate(VA_FOURCC_NV12, 64, 64, error);
        const int exported = probe && probe->kind() == StorageKind::gbm ? probe->exportDescriptor() : -1;
        if (exported >= 0) ::close(exported);
        if (!probe || probe->kind() != StorageKind::gbm) reason() = "no GBM allocation";
        else if (exported < 0) reason() = "no dma-buf export";
        else if (errno = 0, !probe->map()) {
            // The exact refusal matters to whoever owns the graphics stack:
            // a dma-buf that cannot be mapped cannot carry a decoded picture.
            static char text[64] {};
            std::snprintf(text, sizeof(text), "no mapping: %s", std::strerror(errno));
            reason() = text;
        }
        else { reason() = "probed"; return; }
    }
    selection() = StorageKind::memory;
}

const char* storageReason() noexcept { return reason(); }

std::shared_ptr<PixelBuffer> takeReusable(std::vector<std::shared_ptr<PixelBuffer>>& pool,
                                          std::uint32_t fourcc, unsigned width, unsigned height)
{
    for (auto& spare : pool) {
        if (!spare || spare.use_count() != 1) continue;
        if (spare->fourcc() != fourcc || spare->width() != width || spare->height() != height) continue;
        auto taken = spare;
        spare.reset();
        return taken;
    }
    return nullptr;
}

void returnToPool(std::vector<std::shared_ptr<PixelBuffer>>& pool, std::shared_ptr<PixelBuffer> buffer,
                  std::size_t limit)
{
    if (!buffer) return;
    for (auto& spare : pool)
        if (!spare) { spare = std::move(buffer); return; }
    if (pool.size() < limit) pool.push_back(std::move(buffer));
}

void useClientDrmDescriptor(int fd)
{
    if (fd >= 0) clientDrm() = fd;
}

StorageKind selectedStorage() noexcept { return selection(); }

const char* storageName(StorageKind kind) noexcept
{
    switch (kind) {
    case StorageKind::memory: return "memory";
    case StorageKind::gbm: return "gbm";
    case StorageKind::imported: return "imported";
    }
    return "unknown";
}

bool planarCarrier(std::uint32_t fourcc, unsigned width, unsigned height, unsigned& carrierWidth,
                   unsigned& carrierHeight, std::size_t& pitch) noexcept
{
    if (fourcc != VA_FOURCC_NV12 || !width || !height) return false;
    // Rows are aligned the way the guest's GBM aligns them, and the carrier is
    // a 32-bit format whose own alignment then leaves the pitch unchanged.
    const std::size_t rowBytes = (std::size_t(width) + 15u) & ~std::size_t(15u);
    const unsigned rows = height + (height + 1u) / 2u;
    carrierWidth = unsigned(rowBytes / 4u);
    carrierHeight = rows;
    pitch = rowBytes;
    return carrierWidth && carrierHeight;
}

std::unique_ptr<PixelBuffer> PixelBuffer::allocate(std::uint32_t fourcc, unsigned width, unsigned height,
                                                   std::string& error)
{
    std::unique_ptr<PixelBuffer> buffer(new PixelBuffer);
    buffer->fourcc_ = fourcc;
    buffer->width_ = width;
    buffer->height_ = height;
    buffer->modifier_ = DRM_FORMAT_MOD_LINEAR;
    if (!width || !height) { error = "surface has no extent"; return nullptr; }
    if (selection() == StorageKind::gbm) {
        auto& loader = gbm();
        // A planar surface is carried by one packed buffer: the guest's GBM
        // gives each plane of a planar buffer its own object, and a client that
        // imports a surface may only be handed one.
        unsigned requestWidth = width;
        unsigned requestHeight = height;
        std::size_t carrierPitch = 0;
        const bool carried = planarCarrier(fourcc, width, height, requestWidth, requestHeight, carrierPitch);
        const auto format = carried ? drmFormat(VA_FOURCC_BGRX) : drmFormat(fourcc);
        if (loader.device && format) {
            void* bo = loader.boCreate(loader.device, requestWidth, requestHeight, format,
                                       kGbmBoUseLinear | kGbmBoUseRendering);
            if (bo) {
                const int count = loader.boGetPlaneCount(bo);
                if (count == 1 || (!carried && count > 0 && count <= 4)) {
                    buffer->kind_ = StorageKind::gbm;
                    buffer->bo_ = bo;
                    const std::size_t stride = loader.boGetStrideForPlane(bo, 0);
                    if (carried && stride >= carrierPitch) {
                        buffer->planes_ = 2;
                        buffer->pitch_[0] = stride;
                        buffer->pitch_[1] = stride;
                        buffer->offset_[0] = 0;
                        buffer->offset_[1] = stride * height;
                        buffer->size_ = stride * requestHeight;
                    } else if (!carried) {
                        std::size_t end = 0;
                        buffer->planes_ = unsigned(count);
                        for (int plane = 0; plane < count; ++plane) {
                            buffer->pitch_[plane] = loader.boGetStrideForPlane(bo, plane);
                            buffer->offset_[plane] = loader.boGetOffset(bo, plane);
                            end = std::max(end, buffer->offset_[plane] + buffer->pitch_[plane] * height);
                        }
                        buffer->size_ = end;
                    } else {
                        buffer->planes_ = 0;
                    }
                    if (buffer->planes_) {
                        buffer->boWidth_ = requestWidth;
                        buffer->boHeight_ = requestHeight;
                        if (loader.boGetModifier) buffer->modifier_ = loader.boGetModifier(bo);
                        buffer->fd_ = loader.boGetFd(bo);
                        buffer->identity_ = descriptorIdentity(buffer->fd_);
                        if (buffer->fd_ >= 0) return buffer;
                    }
                }
                loader.boDestroy(bo);
                buffer->bo_ = nullptr;
            }
        }
        // Fall through to plain memory rather than failing the allocation.
    }
    unsigned planes = 0;
    std::size_t pitch[4] {};
    std::size_t offset[4] {};
    std::size_t size = 0;
    if (!planeLayout(fourcc, width, height, planes, pitch, offset, size)) {
        error = "unsupported surface format";
        return nullptr;
    }
    buffer->kind_ = StorageKind::memory;
    buffer->planes_ = planes;
    for (unsigned plane = 0; plane < planes; ++plane) {
        buffer->pitch_[plane] = pitch[plane];
        buffer->offset_[plane] = offset[plane];
    }
    buffer->size_ = size;
    buffer->fd_ = memfd_create("uml-va-surface", MFD_CLOEXEC);
    if (buffer->fd_ < 0) { error = std::string("create surface memory: ") + std::strerror(errno); return nullptr; }
    if (ftruncate(buffer->fd_, off_t(size)) < 0) {
        error = std::string("size surface memory: ") + std::strerror(errno);
        return nullptr;
    }
    buffer->identity_ = descriptorIdentity(buffer->fd_);
    if (!buffer->map()) { error = "cannot map surface memory"; return nullptr; }
    std::memset(buffer->map_, 0, size);
    return buffer;
}

std::unique_ptr<PixelBuffer> PixelBuffer::adopt(int fd, std::uint32_t fourcc, unsigned width, unsigned height,
                                                unsigned planes, const std::uint32_t* pitch,
                                                const std::uint32_t* offset, std::uint64_t modifier,
                                                std::size_t size, std::string& error)
{
    const Object object {fd, size};
    const unsigned owner[4] {};
    return adoptObjects(&object, 1, fourcc, width, height, planes, pitch, offset, owner, modifier, error);
}

std::unique_ptr<PixelBuffer> PixelBuffer::adoptObjects(const Object* objects, unsigned objectCount,
                                                       std::uint32_t fourcc, unsigned width, unsigned height,
                                                       unsigned planes, const std::uint32_t* pitch,
                                                       const std::uint32_t* offset, const unsigned* object,
                                                       std::uint64_t modifier, std::string& error)
{
    // The driver copies whole rows of this driver's own layout into and out
    // of an imported surface, so each plane it uses needs rows at least that
    // long, and the object a plane lives in has to hold it.
    unsigned needed = 0;
    std::size_t rowBytes[4] {};
    std::size_t ownOffset[4] {};
    std::size_t ownSize = 0;
    if (!objects || !objectCount || objectCount > 4 || planes > 4 ||
        !planeLayout(fourcc, width, height, needed, rowBytes, ownOffset, ownSize) || planes < needed) {
        error = "imported surface has no usable planes";
        return nullptr;
    }
    for (unsigned index = 0; index < objectCount; ++index)
        if (objects[index].fd < 0) {
            error = "imported surface has an object without a descriptor";
            return nullptr;
        }
    std::unique_ptr<PixelBuffer> buffer(new PixelBuffer);
    buffer->kind_ = StorageKind::imported;
    buffer->fourcc_ = fourcc;
    buffer->width_ = width;
    buffer->height_ = height;
    buffer->planes_ = planes;
    buffer->modifier_ = modifier;
    std::size_t end[4] {};
    for (unsigned plane = 0; plane < planes; ++plane) {
        if (plane < needed && pitch[plane] < rowBytes[plane]) {
            error = "imported surface rows are shorter than its width";
            return nullptr;
        }
        if (object[plane] >= objectCount) {
            error = "imported surface names an object it does not have";
            return nullptr;
        }
        buffer->pitch_[plane] = pitch[plane];
        buffer->offset_[plane] = offset[plane];
        buffer->planeObject_[plane] = object[plane];
        const unsigned rows = plane && fourcc == VA_FOURCC_NV12 ? (height + 1u) / 2u : height;
        end[object[plane]] = std::max(end[object[plane]], std::size_t(offset[plane]) + std::size_t(pitch[plane]) * rows);
    }
    for (unsigned index = 0; index < objectCount; ++index)
        if (objects[index].size && objects[index].size < end[index]) {
            error = "imported surface is smaller than its planes";
            return nullptr;
        }
    buffer->objects_ = objectCount;
    buffer->size_ = objects[0].size ? objects[0].size : end[0];
    buffer->fd_ = objects[0].fd;
    buffer->identity_ = descriptorIdentity(objects[0].fd);
    buffer->objectIdentity_[0] = buffer->identity_;
    for (unsigned index = 1; index < objectCount; ++index) {
        buffer->extraFd_[index - 1] = objects[index].fd;
        buffer->extraSize_[index - 1] = objects[index].size ? objects[index].size : end[index];
        buffer->objectIdentity_[index] = descriptorIdentity(objects[index].fd);
    }
    return buffer;
}

PixelBuffer::~PixelBuffer()
{
    if (map_ && boMapping_) gbm().boUnmap(bo_, boMapping_);
    else if (map_) munmap(map_, size_);
    if (readMap_) munmap(const_cast<std::uint8_t*>(readMap_), size_);
    if (bo_) gbm().boDestroy(bo_);
    if (fd_ >= 0) ::close(fd_);
    for (unsigned index = 0; index < 3; ++index) {
        if (extraMap_[index]) munmap(const_cast<std::uint8_t*>(extraMap_[index]), extraSize_[index]);
        if (extraFd_[index] >= 0) ::close(extraFd_[index]);
    }
}

const std::uint8_t* PixelBuffer::planeForRead(unsigned plane)
{
    if (plane >= planes_) return nullptr;
    const unsigned object = planeObject_[plane];
    const std::uint8_t* base = nullptr;
    if (object == 0) {
        base = mapForRead();
    } else {
        const unsigned slot = object - 1;
        if (!extraMap_[slot] && extraFd_[slot] >= 0 && extraSize_[slot]) {
            void* address = mmap(nullptr, extraSize_[slot], PROT_READ, MAP_SHARED, extraFd_[slot], 0);
            if (address != MAP_FAILED) extraMap_[slot] = static_cast<const std::uint8_t*>(address);
        }
        base = extraMap_[slot];
    }
    return base ? base + offset_[plane] : nullptr;
}

PixelBuffer::CpuAccess::CpuAccess(const PixelBuffer& buffer, bool write) noexcept : write_(write)
{
    // A buffer the driver allocated itself is bracketed for writing only: the
    // CPU mapping it keeps is never unmapped, so nothing else takes what is
    // written to the GPU, while a read has nothing to bring up to date that
    // the driver's own writes have not already made current.
    if (buffer.kind_ != StorageKind::imported && !(buffer.kind_ == StorageKind::gbm && write)) return;
    const std::uint64_t flags = DMA_BUF_SYNC_START | (write ? DMA_BUF_SYNC_RW : DMA_BUF_SYNC_READ);
    for (unsigned index = 0; index < buffer.objects_; ++index) {
        const int fd = index == 0 ? buffer.fd_ : buffer.extraFd_[index - 1];
        dma_buf_sync request {flags};
        if (fd >= 0 && ioctl(fd, DMA_BUF_IOCTL_SYNC, &request) == 0) fd_[count_++] = fd;
    }
}

PixelBuffer::CpuAccess::~CpuAccess()
{
    const std::uint64_t flags = DMA_BUF_SYNC_END | (write_ ? DMA_BUF_SYNC_RW : DMA_BUF_SYNC_READ);
    for (unsigned index = 0; index < count_; ++index) {
        dma_buf_sync request {flags};
        ioctl(fd_[index], DMA_BUF_IOCTL_SYNC, &request);
    }
}

int PixelBuffer::exportDescriptor() const
{
    if (fd_ < 0) return -1;
    return fcntl(fd_, F_DUPFD_CLOEXEC, 0);
}

const std::uint8_t* PixelBuffer::mapForRead()
{
    if (const std::uint8_t* writable = map()) return writable;
    if (readMap_) return readMap_;
    if (fd_ < 0 || !size_) return nullptr;
    void* address = mmap(nullptr, size_, PROT_READ, MAP_SHARED, fd_, 0);
    if (address == MAP_FAILED) return nullptr;
    readMap_ = static_cast<const std::uint8_t*>(address);
    return readMap_;
}

std::uint8_t* PixelBuffer::map()
{
    if (map_) return map_;
    if (objects_ > 1) return nullptr;
    if (bo_ && boWidth_ && boHeight_) {
        auto& loader = gbm();
        if (loader.boMap && loader.boUnmap) {
            std::uint32_t stride = 0;
            void* handle = nullptr;
            // GBM_BO_TRANSFER_READ_WRITE
            void* address = loader.boMap(bo_, 0, 0, boWidth_, boHeight_, 3u, &stride, &handle);
            if (address && address != MAP_FAILED && stride == pitch_[0]) {
                boMapping_ = handle;
                map_ = static_cast<std::uint8_t*>(address);
                return map_;
            }
            if (address && address != MAP_FAILED) loader.boUnmap(bo_, handle);
        }
    }
    if (fd_ < 0 || !size_) return nullptr;
    void* address = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (address == MAP_FAILED) return nullptr;
    map_ = static_cast<std::uint8_t*>(address);
    return map_;
}

}
