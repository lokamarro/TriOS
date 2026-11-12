// SPDX-License-Identifier: GPL-2.0-or-later
#include "planar.hpp"

#include <drm_fourcc.h>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <new>

#include "lock.hpp"

namespace uml::gbm {
namespace {

using abi::Bo;
using abi::Device;
using abi::DeviceV0;

constexpr PlanarFormat kFormats[] = {
    {DRM_FORMAT_NV12, 2, {{DRM_FORMAT_R8, 0}, {DRM_FORMAT_GR88, 1}, {0, 0}}},
    {DRM_FORMAT_P010, 2, {{DRM_FORMAT_R16, 0}, {DRM_FORMAT_GR1616, 1}, {0, 0}}},
    {DRM_FORMAT_YUV420, 3, {{DRM_FORMAT_R8, 0}, {DRM_FORMAT_R8, 1}, {DRM_FORMAT_R8, 1}}},
    {DRM_FORMAT_YVU420, 3, {{DRM_FORMAT_R8, 0}, {DRM_FORMAT_R8, 1}, {DRM_FORMAT_R8, 1}}},
};

constexpr std::uint64_t kModifierLinear = DRM_FORMAT_MOD_LINEAR;
constexpr std::uint64_t kModifierInvalid = DRM_FORMAT_MOD_INVALID;

// A planar buffer: the buffer objects of its planes, with the image's own
// extent and format in the header the loader reads. The words after the header
// are zero so that a reader written for Mesa's own buffer object, which keeps
// its DRI image there, finds none instead of the adapter's bookkeeping.
struct PlanarBo {
    Bo base;
    unsigned char foreign[64];
    PlanarBo* next;
    const PlanarFormat* format;
    Bo* plane[3];
};

// The entries of the inner device's function table that this adapter replaces,
// as the inner backend filled them in. Every device of one backend carries the
// same functions, so one copy serves them all.
struct State {
    Mutex lock;
    unsigned devices {};
    DeviceV0 original {};
    PlanarBo* live {};
};

State state;

const DeviceV0& inner() noexcept { return state.original; }

PlanarBo* planarOf(Bo* bo) noexcept
{
    if (!bo || !planarFormat(bo->v0.format)) return nullptr;
    Guard guard(state.lock);
    for (PlanarBo* entry = state.live; entry; entry = entry->next)
        if (&entry->base == bo) return entry;
    return nullptr;
}

unsigned scaled(unsigned extent, unsigned shift) noexcept
{
    return (extent + (1u << shift) - 1u) >> shift;
}

bool usableModifiers(const std::uint64_t* modifiers, unsigned count) noexcept
{
    if (!modifiers || !count) return true;
    for (unsigned index = 0; index < count; ++index)
        if (modifiers[index] == kModifierLinear || modifiers[index] == kModifierInvalid) return true;
    return false;
}

void destroyPlanes(PlanarBo& bo) noexcept
{
    for (unsigned index = 0; index < bo.format->planes; ++index)
        if (bo.plane[index]) inner().boDestroy(bo.plane[index]);
}

void destroyDevice(Device* device)
{
    const auto destroy = inner().destroy;
    destroy(device);
    Guard guard(state.lock);
    if (state.devices && --state.devices == 0) state.original = {};
}

int isFormatSupported(Device* device, std::uint32_t format, std::uint32_t usage)
{
    const PlanarFormat* planar = planarFormat(format);
    if (!planar) return inner().isFormatSupported(device, format, usage);
    for (unsigned index = 0; index < planar->planes; ++index)
        if (!inner().isFormatSupported(device, planar->plane[index].format, usage)) return 0;
    return 1;
}

int formatModifierPlaneCount(Device* device, std::uint32_t format, std::uint64_t modifier)
{
    const PlanarFormat* planar = planarFormat(format);
    if (!planar) return inner().getFormatModifierPlaneCount(device, format, modifier);
    if (modifier != kModifierLinear && modifier != kModifierInvalid) {
        errno = EINVAL;
        return -1;
    }
    return int(planar->planes);
}

Bo* boCreate(Device* device, std::uint32_t width, std::uint32_t height, std::uint32_t format, std::uint32_t usage,
             const std::uint64_t* modifiers, unsigned count)
{
    const PlanarFormat* planar = planarFormat(format);
    if (!planar) return inner().boCreate(device, width, height, format, usage, modifiers, count);
    if (!width || !height || width > 0x7fffffffu || height > 0x7fffffffu || !usableModifiers(modifiers, count)) {
        errno = EINVAL;
        return nullptr;
    }
    void* memory = std::calloc(1, sizeof(PlanarBo));
    if (!memory) {
        errno = ENOMEM;
        return nullptr;
    }
    auto* bo = new (memory) PlanarBo {};
    bo->format = planar;
    for (unsigned index = 0; index < planar->planes; ++index) {
        const PlaneSpec& spec = planar->plane[index];
        // The plane's own buffer object is a linear one: the modifiers named
        // for the image are only the layouts it may take, and a linear one is
        // the layout of every plane here.
        bo->plane[index] = inner().boCreate(device, scaled(width, spec.shift), scaled(height, spec.shift),
                                            spec.format, usage, nullptr, 0);
        if (!bo->plane[index]) {
            const int reason = errno;
            destroyPlanes(*bo);
            std::free(bo);
            errno = reason;
            return nullptr;
        }
    }
    bo->base.gbm = device;
    bo->base.v0.width = width;
    bo->base.v0.height = height;
    bo->base.v0.stride = bo->plane[0]->v0.stride;
    bo->base.v0.format = format;
    bo->base.v0.handle = bo->plane[0]->v0.handle;
    Guard guard(state.lock);
    bo->next = state.live;
    state.live = bo;
    return &bo->base;
}

void* boMap(Bo* bo, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height, std::uint32_t flags,
            std::uint32_t* stride, void** mapData)
{
    if (planarOf(bo)) {
        // The call has no way to name a plane, so a planar image cannot be
        // mapped as one block; a client maps the dma-buf of each plane.
        errno = ENOSYS;
        return nullptr;
    }
    return inner().boMap(bo, x, y, width, height, flags, stride, mapData);
}

void boUnmap(Bo* bo, void* mapData)
{
    if (planarOf(bo)) return;
    inner().boUnmap(bo, mapData);
}

int boWrite(Bo* bo, const void* buffer, std::size_t size)
{
    if (planarOf(bo)) {
        errno = ENOSYS;
        return -1;
    }
    return inner().boWrite(bo, buffer, size);
}

int boGetFd(Bo* bo)
{
    if (PlanarBo* planar = planarOf(bo)) return inner().boGetFd(planar->plane[0]);
    return inner().boGetFd(bo);
}

int boGetPlanes(Bo* bo)
{
    if (PlanarBo* planar = planarOf(bo)) return int(planar->format->planes);
    return inner().boGetPlanes(bo);
}

// The plane's own buffer object, or null with errno set for a plane that the
// image does not have.
Bo* planeOf(PlanarBo& planar, int plane) noexcept
{
    if (plane < 0 || unsigned(plane) >= planar.format->planes) {
        errno = EINVAL;
        return nullptr;
    }
    return planar.plane[plane];
}

abi::BoHandle boGetHandle(Bo* bo, int plane)
{
    if (PlanarBo* planar = planarOf(bo)) {
        Bo* part = planeOf(*planar, plane);
        if (!part) return {};
        return inner().boGetHandle(part, 0);
    }
    return inner().boGetHandle(bo, plane);
}

int boGetPlaneFd(Bo* bo, int plane)
{
    if (PlanarBo* planar = planarOf(bo)) {
        Bo* part = planeOf(*planar, plane);
        return part ? inner().boGetFd(part) : -1;
    }
    return inner().boGetPlaneFd(bo, plane);
}

std::uint32_t boGetStride(Bo* bo, int plane)
{
    if (PlanarBo* planar = planarOf(bo)) {
        Bo* part = planeOf(*planar, plane);
        return part ? part->v0.stride : 0;
    }
    return inner().boGetStride(bo, plane);
}

std::uint32_t boGetOffset(Bo* bo, int plane)
{
    if (PlanarBo* planar = planarOf(bo)) {
        Bo* part = planeOf(*planar, plane);
        return part ? inner().boGetOffset(part, 0) : 0;
    }
    return inner().boGetOffset(bo, plane);
}

std::uint64_t boGetModifier(Bo* bo)
{
    if (planarOf(bo)) return kModifierLinear;
    return inner().boGetModifier(bo);
}

void boDestroy(Bo* bo)
{
    if (!bo) return;
    if (planarFormat(bo->v0.format)) {
        PlanarBo* found = nullptr;
        {
            Guard guard(state.lock);
            for (PlanarBo** link = &state.live; *link; link = &(*link)->next)
                if (&(*link)->base == bo) {
                    found = *link;
                    *link = found->next;
                    break;
                }
        }
        if (found) {
            destroyPlanes(*found);
            std::free(found);
            return;
        }
    }
    inner().boDestroy(bo);
}

bool sameFunctions(const DeviceV0& a, const DeviceV0& b) noexcept
{
    return a.destroy == b.destroy && a.isFormatSupported == b.isFormatSupported &&
           a.getFormatModifierPlaneCount == b.getFormatModifierPlaneCount && a.boCreate == b.boCreate &&
           a.boMap == b.boMap && a.boUnmap == b.boUnmap && a.boWrite == b.boWrite && a.boGetFd == b.boGetFd &&
           a.boGetPlanes == b.boGetPlanes && a.boGetHandle == b.boGetHandle && a.boGetPlaneFd == b.boGetPlaneFd &&
           a.boGetStride == b.boGetStride && a.boGetOffset == b.boGetOffset &&
           a.boGetModifier == b.boGetModifier && a.boDestroy == b.boDestroy;
}

bool complete(const DeviceV0& v0) noexcept
{
    return v0.destroy && v0.isFormatSupported && v0.getFormatModifierPlaneCount && v0.boCreate && v0.boMap &&
           v0.boUnmap && v0.boWrite && v0.boGetFd && v0.boGetPlanes && v0.boGetHandle && v0.boGetPlaneFd &&
           v0.boGetStride && v0.boGetOffset && v0.boGetModifier && v0.boDestroy;
}

}

const PlanarFormat* planarFormat(std::uint32_t fourcc) noexcept
{
    for (const PlanarFormat& format : kFormats)
        if (format.fourcc == fourcc) return &format;
    return nullptr;
}

bool adapt(Device* device) noexcept
{
    if (!device) return false;
    DeviceV0& v0 = device->v0;
    if (!complete(v0)) return false;
    Guard guard(state.lock);
    if (state.devices == 0) {
        // A backend that allocates these formats itself is left to do it.
        if (v0.isFormatSupported(device, DRM_FORMAT_NV12, abi::kUseRendering)) return false;
        state.original = v0;
    } else if (!sameFunctions(v0, state.original)) {
        return false;
    }
    v0.destroy = destroyDevice;
    v0.isFormatSupported = isFormatSupported;
    v0.getFormatModifierPlaneCount = formatModifierPlaneCount;
    v0.boCreate = boCreate;
    v0.boMap = boMap;
    v0.boUnmap = boUnmap;
    v0.boWrite = boWrite;
    v0.boGetFd = boGetFd;
    v0.boGetPlanes = boGetPlanes;
    v0.boGetHandle = boGetHandle;
    v0.boGetPlaneFd = boGetPlaneFd;
    v0.boGetStride = boGetStride;
    v0.boGetOffset = boGetOffset;
    v0.boGetModifier = boGetModifier;
    v0.boDestroy = boDestroy;
    ++state.devices;
    return true;
}

}
