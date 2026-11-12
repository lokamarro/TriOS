// SPDX-License-Identifier: GPL-2.0-or-later
#include "fake_gbm.hpp"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace uml::gbm::test {
namespace {

using abi::Bo;
using abi::Device;

struct FakeBo {
    Bo base;
    int file;
    unsigned id;
};

struct FakeDevice {
    Device base;
};

unsigned bytesPerPixel(std::uint32_t format) noexcept
{
    switch (format) {
    case DRM_FORMAT_R8: return 1;
    case DRM_FORMAT_GR88:
    case DRM_FORMAT_R16: return 2;
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_GR1616: return 4;
    default: return 0;
    }
}

bool supported(std::uint32_t format) noexcept
{
    FakeState& state = fakeState();
    if (format == state.refuseFormat) return false;
    if (format == DRM_FORMAT_NV12) return state.nativeNv12;
    return bytesPerPixel(format) != 0;
}

void destroyDevice(Device* device)
{
    --fakeState().liveDevices;
    std::free(device);
}

int isFormatSupported(Device*, std::uint32_t format, std::uint32_t usage)
{
    // Mesa refuses a cursor that is also a render target.
    if ((usage & abi::kUseCursor) && (usage & abi::kUseRendering)) return 0;
    return supported(format) ? 1 : 0;
}

int modifierPlaneCount(Device*, std::uint32_t format, std::uint64_t)
{
    return supported(format) ? 1 : -1;
}

Bo* boCreate(Device* device, std::uint32_t width, std::uint32_t height, std::uint32_t format, std::uint32_t usage,
             const std::uint64_t*, unsigned count)
{
    FakeState& state = fakeState();
    state.creates.push_back({width, height, format, usage, count});
    if (!supported(format) || format == DRM_FORMAT_NV12) {
        errno = EINVAL;
        return nullptr;
    }
    if (format == state.failCreate) {
        errno = ENOSPC;
        return nullptr;
    }
    static unsigned ids = 1;
    auto* bo = static_cast<FakeBo*>(std::calloc(1, sizeof(FakeBo)));
    const unsigned stride = (width * bytesPerPixel(format) + 63u) & ~63u;
    bo->file = memfd_create("fake-gbm-bo", MFD_CLOEXEC);
    if (bo->file < 0 || ftruncate(bo->file, off_t(std::size_t(stride) * height)) < 0) {
        std::free(bo);
        errno = ENOMEM;
        return nullptr;
    }
    bo->id = ids++;
    bo->base.gbm = device;
    bo->base.v0.width = width;
    bo->base.v0.height = height;
    bo->base.v0.stride = stride;
    bo->base.v0.format = format;
    bo->base.v0.handle.u32 = bo->id;
    ++state.liveBos;
    return &bo->base;
}

FakeBo* fakeOf(Bo* bo) noexcept { return reinterpret_cast<FakeBo*>(bo); }

void* boMap(Bo* bo, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t* stride,
            void** mapData)
{
    *stride = bo->v0.stride;
    *mapData = nullptr;
    return mmap(nullptr, std::size_t(bo->v0.stride) * bo->v0.height, PROT_READ | PROT_WRITE, MAP_SHARED,
                fakeOf(bo)->file, 0);
}

void boUnmap(Bo* bo, void*) { (void)bo; }
int boWrite(Bo*, const void*, std::size_t) { return 0; }
int boGetFd(Bo* bo) { return fcntl(fakeOf(bo)->file, F_DUPFD_CLOEXEC, 0); }
int boGetPlanes(Bo*) { return 1; }
abi::BoHandle boGetHandle(Bo* bo, int) { return bo->v0.handle; }
int boGetPlaneFd(Bo* bo, int plane) { return plane == 0 ? boGetFd(bo) : -1; }
std::uint32_t boGetStride(Bo* bo, int) { return bo->v0.stride; }
std::uint32_t boGetOffset(Bo*, int) { return 0; }
std::uint64_t boGetModifier(Bo*) { return DRM_FORMAT_MOD_LINEAR; }

void boDestroy(Bo* bo)
{
    --fakeState().liveBos;
    close(fakeOf(bo)->file);
    std::free(bo);
}

Device* createDevice(int fd, std::uint32_t version)
{
    auto* device = static_cast<FakeDevice*>(std::calloc(1, sizeof(FakeDevice)));
    ++fakeState().liveDevices;
    abi::DeviceV0& v0 = device->base.v0;
    v0.backendVersion = version;
    v0.fd = fd;
    v0.name = "drm";
    v0.destroy = destroyDevice;
    v0.isFormatSupported = isFormatSupported;
    v0.getFormatModifierPlaneCount = modifierPlaneCount;
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
    return &device->base;
}

abi::Backend backend {{abi::kBackendVersion, "dri", createDevice}};

}

FakeState& fakeState()
{
    static FakeState state;
    return state;
}

const abi::Backend* fakeBackend()
{
    backend.v0.backendVersion = fakeState().backendVersion;
    return &backend;
}

int fakeBoFile(abi::Bo* bo) { return fakeOf(bo)->file; }

}

extern "C" {

#ifdef UML_FAKE_GBM_MODULE
const uml::gbm::abi::Backend* gbmint_get_backend(const uml::gbm::abi::Core*)
{
    return uml::gbm::test::fakeBackend();
}
#endif

uml::gbm::test::FakeState* uml_fake_gbm_state() { return &uml::gbm::test::fakeState(); }

}
