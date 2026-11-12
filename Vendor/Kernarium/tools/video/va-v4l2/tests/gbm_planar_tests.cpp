// SPDX-License-Identifier: GPL-2.0-or-later
// The planar adapter and the gbm module that carries it, against a stand-in for
// Mesa's DRI backend: what a planar request becomes, that every other request
// reaches Mesa's own function untouched, and that the module loads Mesa's
// backend from beside itself the way libgbm would have it.
#include <dlfcn.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "../gbm/planar.hpp"
#include "fake_gbm.hpp"
#include "test_support.hpp"

namespace uml::va::test {
namespace {

using namespace uml::gbm;
using namespace uml::gbm::test;
using abi::Bo;
using abi::Device;

struct Fixture {
    explicit Fixture(FakeState& s) : state(s) { state = FakeState {}; }
    FakeState& state;
    Device* device {};
    bool adapted {};
    Device* make()
    {
        device = fakeBackend()->v0.createDevice(7, abi::kBackendVersion);
        adapted = adapt(device);
        return device;
    }
    ~Fixture()
    {
        if (device) device->v0.destroy(device);
    }
};

off_t sizeOf(int fd)
{
    struct stat status {};
    return fstat(fd, &status) < 0 ? -1 : status.st_size;
}

void testNv12(Fixture& f)
{
    Device* device = f.make();
    check(f.adapted, "gbm: a device without planar formats is adapted");
    check(device->v0.isFormatSupported(device, DRM_FORMAT_NV12, abi::kUseRendering) == 1,
          "gbm: NV12 is supported once both plane formats are");
    check(device->v0.isFormatSupported(device, DRM_FORMAT_P010, abi::kUseRendering) == 1,
          "gbm: P010 is supported");
    check(device->v0.isFormatSupported(device, DRM_FORMAT_XRGB8888, abi::kUseRendering) == 1,
          "gbm: a colour format is still answered by the backend");
    check(device->v0.isFormatSupported(device, DRM_FORMAT_ABGR16161616F, 0) == 0,
          "gbm: an unknown format is still refused by the backend");

    f.state.creates.clear();
    Bo* bo = device->v0.boCreate(device, 640, 360, DRM_FORMAT_NV12, abi::kUseRendering | abi::kUseLinear, nullptr, 0);
    check(bo != nullptr, "gbm: an NV12 buffer is created");
    if (!bo) return;
    check(bo->gbm == device && bo->v0.width == 640 && bo->v0.height == 360 && bo->v0.format == DRM_FORMAT_NV12,
          "gbm: the buffer reports the image's own extent and format");
    check(f.state.creates.size() == 2, "gbm: NV12 is two plane buffers");
    if (f.state.creates.size() == 2) {
        check(f.state.creates[0].format == DRM_FORMAT_R8 && f.state.creates[0].width == 640 &&
                  f.state.creates[0].height == 360,
              "gbm: the first plane is R8 at the image's size");
        check(f.state.creates[1].format == DRM_FORMAT_GR88 && f.state.creates[1].width == 320 &&
                  f.state.creates[1].height == 180,
              "gbm: the second plane is GR88 at half the size");
        check(f.state.creates[0].usage == (abi::kUseRendering | abi::kUseLinear) &&
                  f.state.creates[1].usage == f.state.creates[0].usage,
              "gbm: the caller's usage reaches each plane");
    }
    check(device->v0.boGetPlanes(bo) == 2, "gbm: NV12 reports two planes");
    const int luma = device->v0.boGetPlaneFd(bo, 0);
    const int chroma = device->v0.boGetPlaneFd(bo, 1);
    check(luma >= 0 && chroma >= 0 && luma != chroma, "gbm: each plane has its own descriptor");
    check(device->v0.boGetPlaneFd(bo, 2) < 0, "gbm: a third plane does not exist");
    const int whole = device->v0.boGetFd(bo);
    check(whole >= 0, "gbm: the image has a descriptor, its first plane's");
    if (whole >= 0) close(whole);
    check(device->v0.boGetStride(bo, 0) >= 640 && bo->v0.stride == device->v0.boGetStride(bo, 0),
          "gbm: the image's stride is the first plane's");
    check(device->v0.boGetStride(bo, 1) >= 640 && device->v0.boGetOffset(bo, 0) == 0 &&
              device->v0.boGetOffset(bo, 1) == 0,
          "gbm: the planes sit at offset zero of their own buffers");
    check(luma >= 0 && sizeOf(luma) >= off_t(device->v0.boGetStride(bo, 0)) * 360,
          "gbm: the first plane's buffer holds its rows");
    check(chroma >= 0 && sizeOf(chroma) >= off_t(device->v0.boGetStride(bo, 1)) * 180,
          "gbm: the second plane's buffer holds its rows");
    check(device->v0.boGetModifier(bo) == DRM_FORMAT_MOD_LINEAR, "gbm: the layout is linear");
    check(device->v0.boGetHandle(bo, 0).u32 != 0 && device->v0.boGetHandle(bo, 0).u32 != device->v0.boGetHandle(bo, 1).u32,
          "gbm: each plane has its own handle");
    // Writing through a plane's descriptor lands in that plane's buffer only.
    if (luma >= 0) {
        void* map = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, luma, 0);
        check(map != MAP_FAILED, "gbm: a plane's descriptor maps");
        if (map != MAP_FAILED) munmap(map, 4096);
    }
    std::uint32_t stride = 0;
    void* data = nullptr;
    errno = 0;
    check(device->v0.boMap(bo, 0, 0, 640, 360, 3, &stride, &data) == nullptr && errno == ENOSYS,
          "gbm: a planar image cannot be mapped as one block");
    errno = 0;
    check(device->v0.boWrite(bo, "x", 1) == -1 && errno == ENOSYS, "gbm: a planar image cannot be written whole");
    if (luma >= 0) close(luma);
    if (chroma >= 0) close(chroma);
    check(f.state.liveBos == 2, "gbm: the image holds exactly its two plane buffers");
    device->v0.boDestroy(bo);
    check(f.state.liveBos == 0, "gbm: destroying the image destroys its plane buffers");
}

void testOddAndHighDepth(Fixture& f)
{
    Device* device = f.make();
    f.state.creates.clear();
    Bo* bo = device->v0.boCreate(device, 641, 361, DRM_FORMAT_P010, 0, nullptr, 0);
    check(bo != nullptr && f.state.creates.size() == 2, "gbm: a P010 buffer of odd size is created");
    if (f.state.creates.size() == 2) {
        check(f.state.creates[0].format == DRM_FORMAT_R16 && f.state.creates[0].width == 641,
              "gbm: P010's first plane is R16");
        check(f.state.creates[1].format == DRM_FORMAT_GR1616 && f.state.creates[1].width == 321 &&
                  f.state.creates[1].height == 181,
              "gbm: P010's second plane is GR1616 rounded up");
    }
    if (bo) device->v0.boDestroy(bo);

    f.state.creates.clear();
    bo = device->v0.boCreate(device, 64, 32, DRM_FORMAT_YVU420, 0, nullptr, 0);
    check(bo != nullptr && f.state.creates.size() == 3 && device->v0.boGetPlanes(bo) == 3,
          "gbm: a three-plane format has three plane buffers");
    if (f.state.creates.size() == 3)
        check(f.state.creates[1].width == 32 && f.state.creates[2].height == 16 &&
                  f.state.creates[2].format == DRM_FORMAT_R8,
              "gbm: its chroma planes are R8 at half the size");
    if (bo) device->v0.boDestroy(bo);
    check(f.state.liveBos == 0, "gbm: nothing leaks");
}

void testDelegation(Fixture& f)
{
    Device* device = f.make();
    f.state.creates.clear();
    Bo* bo = device->v0.boCreate(device, 100, 50, DRM_FORMAT_XRGB8888, abi::kUseRendering, nullptr, 0);
    check(bo != nullptr && f.state.creates.size() == 1 && f.state.creates[0].format == DRM_FORMAT_XRGB8888,
          "gbm: a colour buffer is the backend's own");
    if (bo) {
        check(device->v0.boGetPlanes(bo) == 1, "gbm: its plane count is the backend's");
        const int fd = device->v0.boGetFd(bo);
        check(fd >= 0 && fd != fakeBoFile(bo), "gbm: its descriptor is the backend's");
        if (fd >= 0) close(fd);
        std::uint32_t stride = 0;
        void* data = nullptr;
        void* map = device->v0.boMap(bo, 0, 0, 100, 50, 3, &stride, &data);
        check(map != nullptr && map != MAP_FAILED && stride == bo->v0.stride, "gbm: it maps through the backend");
        if (map && map != MAP_FAILED) munmap(map, std::size_t(stride) * 50);
        device->v0.boDestroy(bo);
    }
    check(f.state.liveBos == 0, "gbm: a colour buffer is destroyed by the backend");
    // A planar buffer and a colour one live side by side.
    Bo* colour = device->v0.boCreate(device, 8, 8, DRM_FORMAT_ARGB8888, 0, nullptr, 0);
    Bo* planar = device->v0.boCreate(device, 8, 8, DRM_FORMAT_NV12, 0, nullptr, 0);
    check(colour && planar && device->v0.boGetPlanes(colour) == 1 && device->v0.boGetPlanes(planar) == 2,
          "gbm: planar and colour buffers coexist");
    if (planar) device->v0.boDestroy(planar);
    if (colour) device->v0.boDestroy(colour);
    check(f.state.liveBos == 0, "gbm: both are destroyed");
    // A format the backend cannot allocate keeps the backend's refusal.
    errno = 0;
    check(device->v0.boCreate(device, 8, 8, DRM_FORMAT_ABGR16161616F, 0, nullptr, 0) == nullptr && errno == EINVAL,
          "gbm: an unknown format fails the way the backend fails it");
}

void testFailures(Fixture& f)
{
    Device* device = f.make();
    f.state.failCreate = DRM_FORMAT_GR88;
    errno = 0;
    check(device->v0.boCreate(device, 64, 64, DRM_FORMAT_NV12, 0, nullptr, 0) == nullptr && errno == ENOSPC,
          "gbm: a plane that cannot be allocated fails the image with its reason");
    check(f.state.liveBos == 0, "gbm: the planes made before the failure are released");
    f.state.failCreate = 0;
    errno = 0;
    check(device->v0.boCreate(device, 0, 64, DRM_FORMAT_NV12, 0, nullptr, 0) == nullptr && errno == EINVAL,
          "gbm: an empty planar image is invalid");
    const std::uint64_t vendor[] = {0x0123456789abcdefULL};
    errno = 0;
    check(device->v0.boCreate(device, 64, 64, DRM_FORMAT_NV12, 0, vendor, 1) == nullptr && errno == EINVAL,
          "gbm: modifiers that exclude linear cannot be met");
    const std::uint64_t linear[] = {0x0123456789abcdefULL, DRM_FORMAT_MOD_LINEAR};
    Bo* bo = device->v0.boCreate(device, 64, 64, DRM_FORMAT_NV12, 0, linear, 2);
    check(bo != nullptr, "gbm: modifiers that include linear are met");
    if (bo) device->v0.boDestroy(bo);
    check(device->v0.getFormatModifierPlaneCount(device, DRM_FORMAT_NV12, DRM_FORMAT_MOD_LINEAR) == 2 &&
              device->v0.getFormatModifierPlaneCount(device, DRM_FORMAT_NV12, vendor[0]) == -1 &&
              device->v0.getFormatModifierPlaneCount(device, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR) == 1,
          "gbm: the plane count of a planar format follows its modifier");
}

void testPlaneFormatsRefused(Fixture& f)
{
    f.state.refuseFormat = DRM_FORMAT_GR88;
    Device* device = f.make();
    check(f.adapted, "gbm: a backend that lacks a plane format is still adapted");
    check(device->v0.isFormatSupported(device, DRM_FORMAT_NV12, 0) == 0,
          "gbm: NV12 is not supported when a plane format is not");
    check(device->v0.isFormatSupported(device, DRM_FORMAT_YUV420, 0) == 1,
          "gbm: a format whose planes are all supported still is");
}

void testNativeBackend(Fixture& f)
{
    f.state.nativeNv12 = true;
    Device* device = f.make();
    check(!f.adapted, "gbm: a backend that allocates NV12 itself is left alone");
    check(device->v0.boCreate != nullptr && device->v0.isFormatSupported(device, DRM_FORMAT_NV12, 0) == 1,
          "gbm: its own answer stands");
}

void testTwoDevices(Fixture& f)
{
    Device* first = f.make();
    Device* second = fakeBackend()->v0.createDevice(8, abi::kBackendVersion);
    check(adapt(second), "gbm: a second device of the same backend is adapted");
    Bo* bo = second->v0.boCreate(second, 16, 16, DRM_FORMAT_NV12, 0, nullptr, 0);
    check(bo != nullptr, "gbm: the second device allocates planar buffers");
    if (bo) second->v0.boDestroy(bo);
    second->v0.destroy(second);
    // The first device still has what it needs after the second is gone.
    bo = first->v0.boCreate(first, 16, 16, DRM_FORMAT_NV12, 0, nullptr, 0);
    check(bo != nullptr, "gbm: the first device keeps working after the second is destroyed");
    if (bo) first->v0.boDestroy(bo);
    check(!adapt(nullptr), "gbm: no device is not adapted");
}

// The module the way libgbm loads it: dlopen, ask for the backend, make a device.
void testModule()
{
    namespace fs = std::filesystem;
    char self[4096] {};
    const ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (length <= 0) return;
    const fs::path build = fs::path(std::string(self, std::size_t(length))).parent_path();
    const fs::path module = build / "virtio_gpu_gbm.so";
    const fs::path fake = build / "fake_dri_gbm.so";
    std::error_code ignored;
    if (!fs::exists(module, ignored) || !fs::exists(fake, ignored)) {
        std::fprintf(stderr, "SKIP gbm module: %s or %s not built\n", module.c_str(), fake.c_str());
        return;
    }
    char pattern[] = "/tmp/uml-gbm-module.XXXXXX";
    if (!mkdtemp(pattern)) return;
    const fs::path directory = pattern;
    fs::copy_file(module, directory / "virtio_gpu_gbm.so", ignored);
    fs::copy_file(fake, directory / "dri_gbm.so", ignored);

    void* library = dlopen((directory / "virtio_gpu_gbm.so").c_str(), RTLD_NOW | RTLD_LOCAL);
    check(library != nullptr, "gbm module: it loads");
    if (library) {
        auto* entry = reinterpret_cast<abi::GetBackend>(dlsym(library, abi::kGetBackendSymbol));
        check(entry != nullptr, "gbm module: it exports the loader's entry point");
        check(dlsym(library, "gbm_bo_create") == nullptr && dlsym(library, "uml_fake_gbm_state") == nullptr,
              "gbm module: nothing else is exported");
        abi::Core core {{1, nullptr}};
        const abi::Backend* backend = entry ? entry(&core) : nullptr;
        check(backend != nullptr && backend->v0.createDevice != nullptr, "gbm module: it offers a backend");
        if (backend && backend->v0.createDevice) {
            check(std::strcmp(backend->v0.backendName, "virtio_gpu") == 0 &&
                      backend->v0.backendVersion == abi::kBackendVersion,
                  "gbm module: it names itself and speaks the loader's version");
            Device* device = backend->v0.createDevice(9, abi::kBackendVersion);
            check(device != nullptr, "gbm module: it makes a device from Mesa's backend beside it");
            if (device) {
                check(std::strcmp(device->v0.name, "drm") == 0 && device->v0.fd == 9,
                      "gbm module: the device is Mesa's own, as the EGL platform expects");
                Bo* bo = device->v0.boCreate(device, 128, 72, DRM_FORMAT_NV12, abi::kUseRendering, nullptr, 0);
                check(bo != nullptr && device->v0.boGetPlanes(bo) == 2, "gbm module: the device allocates NV12");
                if (bo) device->v0.boDestroy(bo);
                device->v0.destroy(device);
            }
        }
        dlclose(library);
    }
    fs::remove_all(directory, ignored);

    // Without Mesa's backend beside it the module declines, and libgbm moves on
    // to Mesa's own.
    char bare[] = "/tmp/uml-gbm-module.XXXXXX";
    if (!mkdtemp(bare)) return;
    const fs::path alone = bare;
    fs::copy_file(module, alone / "virtio_gpu_gbm.so", ignored);
    library = dlopen((alone / "virtio_gpu_gbm.so").c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library) {
        auto* entry = reinterpret_cast<abi::GetBackend>(dlsym(library, abi::kGetBackendSymbol));
        abi::Core core {{1, nullptr}};
        const abi::Backend* backend = entry ? entry(&core) : nullptr;
        check(backend != nullptr, "gbm module: it still answers the loader without Mesa's backend");
        check(backend && backend->v0.createDevice && backend->v0.createDevice(9, abi::kBackendVersion) == nullptr,
              "gbm module: it makes no device without Mesa's backend");
        dlclose(library);
    }
    fs::remove_all(alone, ignored);
}

}

void runGbmPlanarTests()
{
    uml::gbm::test::FakeState& state = uml::gbm::test::fakeState();
    {
        Fixture f(state);
        testNv12(f);
    }
    {
        Fixture f(state);
        testOddAndHighDepth(f);
    }
    {
        Fixture f(state);
        testDelegation(f);
    }
    {
        Fixture f(state);
        testFailures(f);
    }
    {
        Fixture f(state);
        testPlaneFormatsRefused(f);
    }
    {
        Fixture f(state);
        testNativeBackend(f);
    }
    {
        Fixture f(state);
        testTwoDevices(f);
    }
    testModule();
    check(state.liveDevices == 0 && state.liveBos == 0, "gbm: the fake backend ends with nothing live");
}

}
