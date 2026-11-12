// SPDX-License-Identifier: GPL-2.0-or-later
// virtio_gpu_gbm.so: the gbm backend module that libgbm loads for the
// virtio-gpu DRM driver. It makes the device Mesa's own DRI backend makes and
// adds planar buffers to it (planar.hpp); the file name is the driver name
// libgbm asks for, so no setting is involved. GBM_BACKEND=dri turns it off.
#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "abi.hpp"
#include "lock.hpp"
#include "planar.hpp"

namespace {

using namespace uml::gbm;

struct Inner {
    Mutex lock;
    bool tried {};
    void* library {};
    const abi::Backend* backend {};
};

Inner inner;
const abi::Core* loaderCore {};

abi::Backend backend {{abi::kBackendVersion, "virtio_gpu", nullptr}};

// Mesa's DRI backend sits beside this module, under the name libgbm gives it.
bool innerPath(char* path, std::size_t capacity)
{
    constexpr char kName[] = "dri_gbm.so";
    Dl_info info {};
    std::size_t directory = 0;
    if (dladdr(reinterpret_cast<const void*>(&loaderCore), &info) && info.dli_fname) {
        const char* slash = std::strrchr(info.dli_fname, '/');
        if (slash) directory = std::size_t(slash - info.dli_fname + 1);
    }
    if (directory + sizeof(kName) > capacity) return false;
    std::memcpy(path, info.dli_fname, directory);
    std::memcpy(path + directory, kName, sizeof(kName));
    return true;
}

const abi::Backend* loadInner()
{
    Guard guard(inner.lock);
    if (inner.tried) return inner.backend;
    inner.tried = true;
    char path[4096];
    if (!innerPath(path, sizeof(path))) return nullptr;
    void* library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) return nullptr;
    auto* entry = reinterpret_cast<abi::GetBackend>(dlsym(library, abi::kGetBackendSymbol));
    const abi::Backend* found = entry ? entry(loaderCore) : nullptr;
    if (!found || !found->v0.createDevice) {
        dlclose(library);
        return nullptr;
    }
    inner.library = library;
    inner.backend = found;
    return found;
}

abi::Device* createDevice(int fd, std::uint32_t version)
{
    const abi::Backend* mesa = loadInner();
    if (!mesa) return nullptr;
    abi::Device* device = mesa->v0.createDevice(fd, version);
    if (device) adapt(device);
    return device;
}

}

extern "C" __attribute__((visibility("default"))) const abi::Backend* gbmint_get_backend(const abi::Core* core)
{
    loaderCore = core;
    // The version a device may be made at is the lower of what this module
    // and Mesa's backend speak; the loader checks the device against it.
    if (const abi::Backend* mesa = loadInner())
        backend.v0.backendVersion = mesa->v0.backendVersion < abi::kBackendVersion ? mesa->v0.backendVersion
                                                                                   : abi::kBackendVersion;
    backend.v0.createDevice = createDevice;
    return &backend;
}
