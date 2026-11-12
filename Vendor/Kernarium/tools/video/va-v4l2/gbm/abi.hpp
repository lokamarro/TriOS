// SPDX-License-Identifier: GPL-2.0-or-later
// The interface between Mesa's libgbm and its backend modules, as the loader
// of Debian's libgbm 25 defines it (gbm_backend_abi.h, interface version 1).
//
// Debian's libgbm-dev does not install that header, and the interface is frozen
// by design: a later version only appends fields, so a module written for this
// layout runs under every loader that supports version 1. The layout is
// declared here from that contract, and the asserts below pin it for the two
// 64-bit guests this project builds.
#pragma once

#include <cstddef>
#include <cstdint>

namespace uml::gbm::abi {

// The interface version this module implements.
constexpr std::uint32_t kBackendVersion = 1;

union BoHandle {
    void* ptr;
    std::int32_t s32;
    std::uint32_t u32;
    std::int64_t s64;
    std::uint64_t u64;
};

struct BackendDesc;
struct Device;
struct Bo;
struct Surface;

struct DeviceV0 {
    const BackendDesc* backendDesc;
    // MIN(loader, backend) version the device was created at.
    std::uint32_t backendVersion;
    int fd;
    const char* name;

    void (*destroy)(Device* device);
    int (*isFormatSupported)(Device* device, std::uint32_t format, std::uint32_t usage);
    int (*getFormatModifierPlaneCount)(Device* device, std::uint32_t format, std::uint64_t modifier);
    Bo* (*boCreate)(Device* device, std::uint32_t width, std::uint32_t height, std::uint32_t format,
                    std::uint32_t usage, const std::uint64_t* modifiers, unsigned count);
    Bo* (*boImport)(Device* device, std::uint32_t type, void* buffer, std::uint32_t usage);
    void* (*boMap)(Bo* bo, std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height,
                   std::uint32_t flags, std::uint32_t* stride, void** mapData);
    void (*boUnmap)(Bo* bo, void* mapData);
    int (*boWrite)(Bo* bo, const void* buffer, std::size_t size);
    int (*boGetFd)(Bo* bo);
    int (*boGetPlanes)(Bo* bo);
    BoHandle (*boGetHandle)(Bo* bo, int plane);
    int (*boGetPlaneFd)(Bo* bo, int plane);
    std::uint32_t (*boGetStride)(Bo* bo, int plane);
    std::uint32_t (*boGetOffset)(Bo* bo, int plane);
    std::uint64_t (*boGetModifier)(Bo* bo);
    void (*boDestroy)(Bo* bo);
    Surface* (*surfaceCreate)(Device* device, std::uint32_t width, std::uint32_t height, std::uint32_t format,
                              std::uint32_t flags, const std::uint64_t* modifiers, unsigned count);
    Bo* (*surfaceLockFrontBuffer)(Surface* surface);
    void (*surfaceReleaseBuffer)(Surface* surface, Bo* bo);
    int (*surfaceHasFreeBuffers)(Surface* surface);
    void (*surfaceDestroy)(Surface* surface);
};

struct Device {
    // The loader recognises a device by its first element.
    Device* (*dummy)(int);
    DeviceV0 v0;
};

struct BoV0 {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t stride;
    std::uint32_t format;
    BoHandle handle;
    void* userData;
    void (*destroyUserData)(Bo* bo, void* userData);
};

struct Bo {
    Device* gbm;
    BoV0 v0;
};

struct BackendV0 {
    std::uint32_t backendVersion;
    const char* backendName;
    Device* (*createDevice)(int fd, std::uint32_t gbmBackendVersion);
};

struct Backend {
    BackendV0 v0;
};

struct CoreV0 {
    std::uint32_t coreVersion;
    std::uint32_t (*formatCanonicalize)(std::uint32_t gbmFormat);
};

struct Core {
    CoreV0 v0;
};

#if defined(__LP64__)
static_assert(sizeof(DeviceV0) == 192 && sizeof(Device) == 200);
static_assert(offsetof(Device, v0) == 8);
static_assert(offsetof(DeviceV0, backendVersion) == 8 && offsetof(DeviceV0, fd) == 12 &&
              offsetof(DeviceV0, name) == 16 && offsetof(DeviceV0, destroy) == 24 &&
              offsetof(DeviceV0, boCreate) == 48 && offsetof(DeviceV0, boGetModifier) == 136 &&
              offsetof(DeviceV0, boDestroy) == 144 && offsetof(DeviceV0, surfaceDestroy) == 184);
static_assert(sizeof(BoV0) == 40 && sizeof(Bo) == 48);
static_assert(offsetof(Bo, v0) == 8 && offsetof(BoV0, handle) == 16 && offsetof(BoV0, userData) == 24 &&
              offsetof(BoV0, destroyUserData) == 32);
static_assert(sizeof(BackendV0) == 24 && offsetof(BackendV0, backendName) == 8 &&
              offsetof(BackendV0, createDevice) == 16);
static_assert(sizeof(CoreV0) == 16 && offsetof(CoreV0, formatCanonicalize) == 8);
#endif

// The one symbol a backend module exports; the loader calls it before it makes
// any device.
using GetBackend = const Backend* (*)(const Core* core);
constexpr const char* kGetBackendSymbol = "gbmint_get_backend";

// The GBM_BO_USE_* and GBM_BO_IMPORT_* values of the public header.
constexpr std::uint32_t kUseScanout = 1u << 0;
constexpr std::uint32_t kUseCursor = 1u << 1;
constexpr std::uint32_t kUseRendering = 1u << 2;
constexpr std::uint32_t kUseWrite = 1u << 3;
constexpr std::uint32_t kUseLinear = 1u << 4;

}
