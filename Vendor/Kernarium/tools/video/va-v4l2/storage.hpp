// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace uml::va {

// Which kind of buffer backs a surface. A surface always has a CPU mapping;
// the difference is whether the file descriptor it exports is something a
// graphics stack can import.
enum class StorageKind {
    memory,   // memfd: a CPU buffer with a descriptor, for clients that read pixels
    gbm,      // a linear GBM buffer object, importable by the guest graphics stack
    imported, // a descriptor handed to the driver by a client
};

// One buffer object holding every plane of one surface: a single allocation, so
// that it can be exported as one DRM PRIME object with per-plane offsets, which
// is what Chromium's VA-API wrapper requires.
class PixelBuffer {
public:
    static std::unique_ptr<PixelBuffer> allocate(std::uint32_t fourcc, unsigned width, unsigned height,
                                                 std::string& error);
    static std::unique_ptr<PixelBuffer> adopt(int fd, std::uint32_t fourcc, unsigned width, unsigned height,
                                              unsigned planes, const std::uint32_t* pitch,
                                              const std::uint32_t* offset, std::uint64_t modifier,
                                              std::size_t size, std::string& error);

    // A descriptor a client hands over and the extent it says the object has
    // (zero when it names none).
    struct Object {
        int fd;
        std::size_t size;
    };
    // The same for an image whose planes live in several objects, as a
    // graphics stack's planar buffer does when it gives every plane a
    // resource of its own. `object[plane]` names the object each plane is in.
    // The descriptors are taken over only when the call succeeds. A buffer of
    // several objects is for a reader of planes (planeForRead); it has no
    // single mapping.
    static std::unique_ptr<PixelBuffer> adoptObjects(const Object* objects, unsigned objectCount,
                                                     std::uint32_t fourcc, unsigned width, unsigned height,
                                                     unsigned planes, const std::uint32_t* pitch,
                                                     const std::uint32_t* offset, const unsigned* object,
                                                     std::uint64_t modifier, std::string& error);
    ~PixelBuffer();

    PixelBuffer(const PixelBuffer&) = delete;
    PixelBuffer& operator=(const PixelBuffer&) = delete;

    // A descriptor the caller owns and must close, or -1 when this buffer
    // cannot be shared.
    int exportDescriptor() const;
    // The buffer's own descriptor, which it keeps: for lending the buffer to
    // something that only queues it, such as a V4L2 capture queue.
    int descriptor() const noexcept { return fd_; }
    // The CPU mapping, created on demand. Null when the buffer cannot be mapped.
    std::uint8_t* map();
    const std::uint8_t* map() const { return const_cast<PixelBuffer*>(this)->map(); }
    // The CPU mapping, or when the descriptor was opened read-only (the usual
    // state of a descriptor a graphics stack exports) a read-only mapping:
    // all an encoder needs of its input. Null when neither can be made.
    const std::uint8_t* mapForRead();

    // Where one plane's first byte is, read-only, whichever object the plane
    // lives in. Null when that object cannot be mapped.
    const std::uint8_t* planeForRead(unsigned plane);

    // CPU access to a buffer a client imported, bracketed the way the kernel
    // wants dma-buf accesses bracketed (DMA_BUF_IOCTL_SYNC): from construction
    // to destruction the buffer's contents are current for the CPU, and when
    // the access wrote, the GPU sees what was written. A GBM buffer the driver
    // allocated for itself is bracketed when the access writes, because a
    // client that imported the surface's descriptor samples it on the GPU; a
    // read of one does nothing. So does any buffer whose descriptors are no
    // dma-bufs. A failed sync is not an error, because a descriptor that is no
    // dma-buf has nothing to sync.
    class CpuAccess {
    public:
        CpuAccess(const PixelBuffer& buffer, bool write) noexcept;
        ~CpuAccess();
        CpuAccess(const CpuAccess&) = delete;
        CpuAccess& operator=(const CpuAccess&) = delete;

    private:
        int fd_[4] {-1, -1, -1, -1};
        unsigned count_ {};
        bool write_ {};
    };

    StorageKind kind() const noexcept { return kind_; }
    unsigned objects() const noexcept { return objects_; }
    // The identity of the n-th object, as descriptorIdentity() says it.
    std::uint64_t objectIdentity(unsigned index) const noexcept { return objectIdentity_[index]; }
    std::uint32_t fourcc() const noexcept { return fourcc_; }
    unsigned width() const noexcept { return width_; }
    unsigned height() const noexcept { return height_; }
    unsigned planes() const noexcept { return planes_; }
    std::size_t pitch(unsigned plane) const noexcept { return pitch_[plane]; }
    std::size_t offset(unsigned plane) const noexcept { return offset_[plane]; }
    std::size_t size() const noexcept { return size_; }
    std::uint64_t modifier() const noexcept { return modifier_; }
    // The identity of the underlying object, so that a descriptor handed back
    // to the driver can be recognised as one of its own allocations. It is
    // descriptorIdentity() of the buffer's descriptor.
    std::uint64_t identity() const noexcept { return identity_; }
    // Whether a surface of this format and size can use this buffer: a
    // descriptor imported again reuses its buffer only for the same image.
    bool holds(std::uint32_t fourcc, unsigned width, unsigned height) const noexcept
    {
        return fourcc_ == fourcc && width_ == width && height_ == height;
    }

private:
    PixelBuffer() = default;

    StorageKind kind_ {StorageKind::memory};
    std::uint32_t fourcc_ {};
    unsigned width_ {};
    unsigned height_ {};
    unsigned planes_ {};
    std::size_t pitch_[4] {};
    std::size_t offset_[4] {};
    std::size_t size_ {};
    std::uint64_t modifier_ {};
    std::uint64_t identity_ {};
    int fd_ {-1};
    std::uint8_t* map_ {};
    // Set instead of map_ when only a read-only mapping could be made.
    const std::uint8_t* readMap_ {};
    // The objects after the first of an image that lives in several; the
    // first is fd_, size_ and readMap_.
    unsigned objects_ {1};
    unsigned planeObject_[4] {};
    int extraFd_[3] {-1, -1, -1};
    std::size_t extraSize_[3] {};
    const std::uint8_t* extraMap_[3] {};
    std::uint64_t objectIdentity_[4] {};
    void* bo_ {};
    // A GBM buffer is mapped through GBM itself: the descriptor it exports
    // need not be writable, and mapping that descriptor then fails.
    void* boMapping_ {};
    unsigned boWidth_ {};
    unsigned boHeight_ {};
};

// Chooses the backing store once per process: UML_VA_V4L2_SURFACES is
// memory|gbm|auto, and auto prefers a GBM buffer the graphics stack can import
// and falls back to plain memory.
void selectStorage(const char* request);
StorageKind selectedStorage() noexcept;
const char* storageName(StorageKind kind) noexcept;
// Why the choice came out that way, for the log: a client that cannot import
// an exported surface needs to know which step of the probe declined.
const char* storageReason() noexcept;

// The identity of the object a descriptor refers to, as PixelBuffer::identity()
// records it; zero for a descriptor that is not open.
std::uint64_t descriptorIdentity(int fd) noexcept;

// The DRM device a client passed to vaGetDisplayDRM. It is already open and
// already permitted, which a node this driver opens itself need not be, so the
// GBM device is made from it when there is one. Must be called before the
// first allocation.
void useClientDrmDescriptor(int fd);

// Takes a buffer the pool can hand out again: one of the right shape that
// nothing but the pool still refers to. A surface still showing a picture, or
// an image derived from it, keeps a reference of its own, so a buffer in use
// is never handed out twice. Returns null when the pool has nothing suitable.
std::shared_ptr<PixelBuffer> takeReusable(std::vector<std::shared_ptr<PixelBuffer>>& pool,
                                          std::uint32_t fourcc, unsigned width, unsigned height);

// Puts a buffer back, reusing an empty slot when there is one. The pool is
// bounded: beyond `limit` buffers the oldest surplus is simply dropped.
void returnToPool(std::vector<std::shared_ptr<PixelBuffer>>& pool, std::shared_ptr<PixelBuffer> buffer,
                  std::size_t limit = 32);

// A planar surface is allocated as one single-plane buffer that carries every
// plane, because a client that imports it may only be handed one object.
// Returns the packed carrier's extent and the planar layout inside it, or
// false for a format that needs no carrier.
bool planarCarrier(std::uint32_t fourcc, unsigned width, unsigned height, unsigned& carrierWidth,
                   unsigned& carrierHeight, std::size_t& pitch) noexcept;

// The plane layout this driver uses for a format, matching what the Virtio
// video device produces: tightly packed rows.
bool planeLayout(std::uint32_t fourcc, unsigned width, unsigned height, unsigned& planes,
                 std::size_t* pitch, std::size_t* offset, std::size_t& size) noexcept;

}
