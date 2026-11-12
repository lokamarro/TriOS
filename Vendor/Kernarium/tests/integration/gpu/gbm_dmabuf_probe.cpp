// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest probe for the virtio-gpu dma-buf contract of a classic 3D resource,
// the buffers guest Mesa allocates through gbm. It checks, on the guest's own
// libgbm and libEGL:
//
//   T1  a PRIME export is read-write whatever the caller asked for, so a
//       buffer Mesa exported first can still be mapped writable;
//   T2  after the GPU writes a buffer, DMA_BUF_IOCTL_SYNC makes the whole
//       write visible through a read mapping of its dma-buf, again after a
//       second write; and
//   T3  a CPU write through a write mapping, bracketed by DMA_BUF_IOCTL_SYNC,
//       is what the GPU reads, again after a second write.
//
// Both libraries are opened at run time, so the probe needs only libc and
// libdl to build and runs against whatever Mesa the image carries:
//
//   c++ -std=c++20 -O2 -Wall -Wextra -Werror gbm_dmabuf_probe.cpp -ldl
//
// Run it as root with the render node as its optional argument. It prints
// PASS or FAIL for each check and ends with UML_GBM_DMABUF_PROBE: PASS or
// UML_GBM_DMABUF_PROBE: FAIL, exiting 0 only after PASS.
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr std::uint32_t fourcc(char a, char b, char c, char d)
{
    return static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b) << 8 |
           static_cast<std::uint32_t>(c) << 16 |
           static_cast<std::uint32_t>(d) << 24;
}

// DRM_FORMAT_ABGR8888: bytes R, G, B, A, so a little-endian word reads
// 0xAABBGGRR and matches what glReadPixels(GL_RGBA, GL_UNSIGNED_BYTE) returns.
constexpr std::uint32_t kAbgr8888 = fourcc('A', 'B', '2', '4');
constexpr std::uint32_t kGbmUseRendering = 4;
constexpr std::uint32_t kGbmUseLinear = 16;

constexpr int kWidth = 64;
constexpr int kHeight = 64;
constexpr std::size_t kPixels = static_cast<std::size_t>(kWidth) * kHeight;

struct DrmPrimeHandle {
    std::uint32_t handle;
    std::uint32_t flags;
    std::int32_t fd;
};
constexpr unsigned long kPrimeHandleToFd = 0xC00C642DUL;  // DRM_IOCTL_PRIME_HANDLE_TO_FD

struct DmaBufSync {
    std::uint64_t flags;
};
constexpr unsigned long kDmaBufIoctlSync = 0x40086200UL;  // DMA_BUF_IOCTL_SYNC
constexpr std::uint64_t kSyncRead = 1;
constexpr std::uint64_t kSyncWrite = 2;
constexpr std::uint64_t kSyncEnd = 4;

using EglDisplay = void*;
using EglContext = void*;
using EglConfig = void*;
using EglImage = void*;
using EglInt = int;
using Proc = void (*)();

constexpr EglInt kEglPlatformGbm = 0x31D7;
constexpr EglInt kEglNone = 0x3038;
constexpr EglInt kEglOpenGlEsApi = 0x30A0;
constexpr EglInt kEglContextClientVersion = 0x3098;
constexpr EglInt kEglLinuxDmaBuf = 0x3270;
constexpr EglInt kEglLinuxDrmFourcc = 0x3271;
constexpr EglInt kEglDmaBufPlane0Fd = 0x3272;
constexpr EglInt kEglDmaBufPlane0Offset = 0x3273;
constexpr EglInt kEglDmaBufPlane0Pitch = 0x3274;
constexpr EglInt kEglWidth = 0x3057;
constexpr EglInt kEglHeight = 0x3056;
constexpr EglInt kEglRenderableType = 0x3040;
constexpr EglInt kEglOpenGlEs2Bit = 4;
constexpr EglInt kEglSurfaceType = 0x3033;
constexpr unsigned kGlTexture2d = 0x0DE1;
constexpr unsigned kGlFramebuffer = 0x8D40;
constexpr unsigned kGlColorAttachment0 = 0x8CE0;
constexpr unsigned kGlColorBufferBit = 0x4000;
constexpr unsigned kGlRgba = 0x1908;
constexpr unsigned kGlUnsignedByte = 0x1401;
constexpr unsigned kGlFramebufferComplete = 0x8CD5;
constexpr unsigned kGlRenderer = 0x1F01;

struct Library {
    void* gbm = nullptr;
    void* egl = nullptr;

    void* (*gbm_create_device)(int) = nullptr;
    void* (*gbm_bo_create)(void*, std::uint32_t, std::uint32_t, std::uint32_t,
                           std::uint32_t) = nullptr;
    void (*gbm_bo_destroy)(void*) = nullptr;
    int (*gbm_bo_get_fd)(void*) = nullptr;
    std::uint32_t (*gbm_bo_get_stride)(void*) = nullptr;
    std::uint64_t (*gbm_bo_get_handle)(void*) = nullptr;

    Proc (*eglGetProcAddress)(const char*) = nullptr;
    EglDisplay (*eglGetPlatformDisplay)(EglInt, void*, const std::intptr_t*) = nullptr;
    unsigned (*eglInitialize)(EglDisplay, EglInt*, EglInt*) = nullptr;
    unsigned (*eglBindAPI)(EglInt) = nullptr;
    unsigned (*eglChooseConfig)(EglDisplay, const EglInt*, EglConfig*, EglInt,
                                EglInt*) = nullptr;
    EglContext (*eglCreateContext)(EglDisplay, EglConfig, EglContext,
                                   const EglInt*) = nullptr;
    unsigned (*eglMakeCurrent)(EglDisplay, void*, void*, EglContext) = nullptr;
    EglImage (*eglCreateImageKHR)(EglDisplay, EglContext, EglInt, void*,
                                  const EglInt*) = nullptr;
    EglInt (*eglGetError)() = nullptr;

    void (*glEGLImageTargetTexture2DOES)(unsigned, void*) = nullptr;
    void (*glGenTextures)(int, unsigned*) = nullptr;
    void (*glBindTexture)(unsigned, unsigned) = nullptr;
    void (*glGenFramebuffers)(int, unsigned*) = nullptr;
    void (*glBindFramebuffer)(unsigned, unsigned) = nullptr;
    void (*glFramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int) = nullptr;
    unsigned (*glCheckFramebufferStatus)(unsigned) = nullptr;
    void (*glClearColor)(float, float, float, float) = nullptr;
    void (*glClear)(unsigned) = nullptr;
    void (*glFinish)() = nullptr;
    void (*glReadPixels)(int, int, int, int, unsigned, unsigned, void*) = nullptr;
    unsigned (*glGetError)() = nullptr;
    const unsigned char* (*glGetString)(unsigned) = nullptr;
};

Library gl;
int failures = 0;
int checks = 0;

__attribute__((format(printf, 2, 3))) void check(bool condition, const char* format, ...)
{
    char text[256];
    std::va_list arguments;

    va_start(arguments, format);
    std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    checks++;
    if (!condition)
        failures++;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", text);
    std::fflush(stdout);
}

template <typename Function>
bool bind(void* library, const char* name, Function& function)
{
    function = reinterpret_cast<Function>(dlsym(library, name));
    if (!function)
        check(false, "the library provides %s", name);
    return function != nullptr;
}

template <typename Function>
bool bindGl(const char* name, Function& function)
{
    function = reinterpret_cast<Function>(gl.eglGetProcAddress(name));
    if (!function)
        check(false, "EGL provides %s", name);
    return function != nullptr;
}

bool loadLibraries()
{
    gl.gbm = dlopen("libgbm.so.1", RTLD_NOW | RTLD_GLOBAL);
    gl.egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!gl.gbm || !gl.egl) {
        check(false, "libgbm.so.1 and libEGL.so.1 open: %s", dlerror());
        return false;
    }
    bool ok = bind(gl.gbm, "gbm_create_device", gl.gbm_create_device);
    ok = bind(gl.gbm, "gbm_bo_create", gl.gbm_bo_create) && ok;
    ok = bind(gl.gbm, "gbm_bo_destroy", gl.gbm_bo_destroy) && ok;
    ok = bind(gl.gbm, "gbm_bo_get_fd", gl.gbm_bo_get_fd) && ok;
    ok = bind(gl.gbm, "gbm_bo_get_stride", gl.gbm_bo_get_stride) && ok;
    ok = bind(gl.gbm, "gbm_bo_get_handle", gl.gbm_bo_get_handle) && ok;
    ok = bind(gl.egl, "eglGetProcAddress", gl.eglGetProcAddress) && ok;
    ok = bind(gl.egl, "eglInitialize", gl.eglInitialize) && ok;
    ok = bind(gl.egl, "eglBindAPI", gl.eglBindAPI) && ok;
    ok = bind(gl.egl, "eglChooseConfig", gl.eglChooseConfig) && ok;
    ok = bind(gl.egl, "eglCreateContext", gl.eglCreateContext) && ok;
    ok = bind(gl.egl, "eglMakeCurrent", gl.eglMakeCurrent) && ok;
    ok = bind(gl.egl, "eglGetError", gl.eglGetError) && ok;
    if (!ok)
        return false;
    ok = bindGl("eglGetPlatformDisplay", gl.eglGetPlatformDisplay);
    ok = bindGl("eglCreateImageKHR", gl.eglCreateImageKHR) && ok;
    ok = bindGl("glEGLImageTargetTexture2DOES", gl.glEGLImageTargetTexture2DOES) && ok;
    ok = bindGl("glGenTextures", gl.glGenTextures) && ok;
    ok = bindGl("glBindTexture", gl.glBindTexture) && ok;
    ok = bindGl("glGenFramebuffers", gl.glGenFramebuffers) && ok;
    ok = bindGl("glBindFramebuffer", gl.glBindFramebuffer) && ok;
    ok = bindGl("glFramebufferTexture2D", gl.glFramebufferTexture2D) && ok;
    ok = bindGl("glCheckFramebufferStatus", gl.glCheckFramebufferStatus) && ok;
    ok = bindGl("glClearColor", gl.glClearColor) && ok;
    ok = bindGl("glClear", gl.glClear) && ok;
    ok = bindGl("glFinish", gl.glFinish) && ok;
    ok = bindGl("glReadPixels", gl.glReadPixels) && ok;
    ok = bindGl("glGetError", gl.glGetError) && ok;
    ok = bindGl("glGetString", gl.glGetString) && ok;
    return ok;
}

int drmFd = -1;
void* gbmDevice = nullptr;
EglDisplay display = nullptr;

// A buffer the probe maps, with the dma-buf it exports.
struct Buffer {
    void* bo = nullptr;
    int fd = -1;
    std::uint32_t stride = 0;

    ~Buffer()
    {
        if (fd >= 0)
            close(fd);
        if (bo)
            gl.gbm_bo_destroy(bo);
    }
};

bool createBuffer(Buffer& buffer)
{
    buffer.bo = gl.gbm_bo_create(gbmDevice, kWidth, kHeight, kAbgr8888,
                                 kGbmUseLinear | kGbmUseRendering);
    if (!buffer.bo) {
        check(false, "gbm allocates a %dx%d ABGR8888 buffer: errno %d", kWidth, kHeight, errno);
        return false;
    }
    buffer.stride = gl.gbm_bo_get_stride(buffer.bo);
    return true;
}

int accessMode(int fd)
{
    return fcntl(fd, F_GETFL) & O_ACCMODE;
}

bool sync(int fd, std::uint64_t flags)
{
    DmaBufSync request = {flags};

    if (ioctl(fd, kDmaBufIoctlSync, &request) == 0)
        return true;
    check(false, "DMA_BUF_IOCTL_SYNC %#llx: errno %d",
          static_cast<unsigned long long>(flags), errno);
    return false;
}

// How many of the buffer's pixels differ from `expected`, and the first that
// does.
struct Mismatch {
    std::size_t count = 0;
    std::uint32_t first = 0;
};

Mismatch compareRows(const void* base, std::uint32_t stride, std::uint32_t expected)
{
    Mismatch result;

    for (int y = 0; y < kHeight; y++) {
        const auto* row = reinterpret_cast<const std::uint32_t*>(
            static_cast<const unsigned char*>(base) + static_cast<std::size_t>(y) * stride);
        for (int x = 0; x < kWidth; x++) {
            if (row[x] == expected)
                continue;
            if (result.count++ == 0)
                result.first = row[x];
        }
    }
    return result;
}

Mismatch comparePixels(const std::array<std::uint32_t, kPixels>& pixels, std::uint32_t expected)
{
    Mismatch result;

    for (std::uint32_t pixel : pixels) {
        if (pixel == expected)
            continue;
        if (result.count++ == 0)
            result.first = pixel;
    }
    return result;
}

void checkPixels(const char* what, const Mismatch& mismatch, std::uint32_t expected)
{
    check(mismatch.count == 0, "%s: %zu of %zu pixels differ, first %08x, want %08x", what,
          mismatch.count, kPixels, mismatch.first, expected);
}

unsigned framebufferFor(int fd, std::uint32_t stride)
{
    const std::array<EglInt, 13> attributes = {
        kEglWidth, kWidth, kEglHeight, kHeight, kEglLinuxDrmFourcc,
        static_cast<EglInt>(kAbgr8888), kEglDmaBufPlane0Fd, fd, kEglDmaBufPlane0Offset, 0,
        kEglDmaBufPlane0Pitch, static_cast<EglInt>(stride), kEglNone};
    EglImage image = gl.eglCreateImageKHR(display, nullptr, kEglLinuxDmaBuf, nullptr,
                                          attributes.data());
    unsigned texture = 0;
    unsigned framebuffer = 0;

    if (!image) {
        check(false, "EGL imports the dma-buf: error %#x", gl.eglGetError());
        return 0;
    }
    gl.glGenTextures(1, &texture);
    gl.glBindTexture(kGlTexture2d, texture);
    gl.glEGLImageTargetTexture2DOES(kGlTexture2d, image);
    gl.glGenFramebuffers(1, &framebuffer);
    gl.glBindFramebuffer(kGlFramebuffer, framebuffer);
    gl.glFramebufferTexture2D(kGlFramebuffer, kGlColorAttachment0, kGlTexture2d, texture, 0);
    unsigned status = gl.glCheckFramebufferStatus(kGlFramebuffer);
    check(status == kGlFramebufferComplete, "the imported dma-buf is a complete framebuffer: %#x",
          status);
    return status == kGlFramebufferComplete ? framebuffer : 0;
}

bool readGpu(std::array<std::uint32_t, kPixels>& pixels)
{
    gl.glReadPixels(0, 0, kWidth, kHeight, kGlRgba, kGlUnsignedByte, pixels.data());
    return gl.glGetError() == 0;
}

bool setupGl(const char* node)
{
    drmFd = open(node, O_RDWR | O_CLOEXEC);
    if (drmFd < 0) {
        check(false, "%s opens: errno %d", node, errno);
        return false;
    }
    gbmDevice = gl.gbm_create_device(drmFd);
    check(gbmDevice != nullptr, "gbm opens %s", node);
    if (!gbmDevice)
        return false;
    display = gl.eglGetPlatformDisplay(kEglPlatformGbm, gbmDevice, nullptr);
    EglInt major = 0;
    EglInt minor = 0;
    if (!display || !gl.eglInitialize(display, &major, &minor)) {
        check(false, "EGL initializes on the gbm device: error %#x", gl.eglGetError());
        return false;
    }
    gl.eglBindAPI(kEglOpenGlEsApi);
    const std::array<EglInt, 5> configAttributes = {kEglRenderableType, kEglOpenGlEs2Bit,
                                                    kEglSurfaceType, 0, kEglNone};
    EglConfig config = nullptr;
    EglInt configs = 0;
    gl.eglChooseConfig(display, configAttributes.data(), &config, 1, &configs);
    const std::array<EglInt, 3> contextAttributes = {kEglContextClientVersion, 2, kEglNone};
    EglContext context = gl.eglCreateContext(display, configs ? config : nullptr, nullptr,
                                             contextAttributes.data());
    if (!context || !gl.eglMakeCurrent(display, nullptr, nullptr, context)) {
        check(false, "a surfaceless GLES2 context is current: error %#x", gl.eglGetError());
        return false;
    }
    std::printf("EGL %d.%d GL_RENDERER=%s\n", major, minor,
                reinterpret_cast<const char*>(gl.glGetString(kGlRenderer)));
    return true;
}

// Every buffer Mesa exports is writable, however its first export asked.
void testExportMode()
{
    {
        Buffer buffer;
        if (!createBuffer(buffer))
            return;
        buffer.fd = gl.gbm_bo_get_fd(buffer.bo);
        check(buffer.fd >= 0, "T1 gbm exports the buffer");
        if (buffer.fd < 0)
            return;
        check(accessMode(buffer.fd) == O_RDWR, "T1 gbm_bo_get_fd is read-write: access mode %d",
              accessMode(buffer.fd));
        void* mapping = mmap(nullptr, static_cast<std::size_t>(buffer.stride) * kHeight,
                             PROT_READ | PROT_WRITE, MAP_SHARED, buffer.fd, 0);
        check(mapping != MAP_FAILED, "T1 the gbm export maps writable: errno %d",
              mapping == MAP_FAILED ? errno : 0);
        if (mapping != MAP_FAILED)
            munmap(mapping, static_cast<std::size_t>(buffer.stride) * kHeight);
    }
    {
        // The first export of this buffer asks for neither read nor write.
        Buffer buffer;
        if (!createBuffer(buffer))
            return;
        DrmPrimeHandle request = {static_cast<std::uint32_t>(gl.gbm_bo_get_handle(buffer.bo)),
                                  O_CLOEXEC, -1};
        int result = ioctl(drmFd, kPrimeHandleToFd, &request);
        check(result == 0, "T1 PRIME exports the buffer without DRM_RDWR: errno %d",
              result == 0 ? 0 : errno);
        if (result != 0)
            return;
        buffer.fd = request.fd;
        check(accessMode(buffer.fd) == O_RDWR,
              "T1 an export that asked for no write access is read-write: access mode %d",
              accessMode(buffer.fd));
        void* mapping = mmap(nullptr, static_cast<std::size_t>(buffer.stride) * kHeight,
                             PROT_READ | PROT_WRITE, MAP_SHARED, buffer.fd, 0);
        check(mapping != MAP_FAILED, "T1 that export maps writable: errno %d",
              mapping == MAP_FAILED ? errno : 0);
        if (mapping != MAP_FAILED)
            munmap(mapping, static_cast<std::size_t>(buffer.stride) * kHeight);
    }
}

struct Clear {
    float red, green, blue;
    std::uint32_t word;
};

// The GPU writes the buffer; a reader of its dma-buf sees the whole write
// once it brackets the read, before anything in Mesa has read the resource
// back.
void testGpuWriteVisible()
{
    Buffer buffer;
    if (!createBuffer(buffer))
        return;
    buffer.fd = gl.gbm_bo_get_fd(buffer.bo);
    if (buffer.fd < 0) {
        check(false, "T2 gbm exports the buffer");
        return;
    }
    unsigned framebuffer = framebufferFor(buffer.fd, buffer.stride);
    if (!framebuffer)
        return;
    const std::size_t length = static_cast<std::size_t>(buffer.stride) * kHeight;
    void* mapping = mmap(nullptr, length, PROT_READ, MAP_SHARED, buffer.fd, 0);
    check(mapping != MAP_FAILED, "T2 the dma-buf maps for reading: errno %d",
          mapping == MAP_FAILED ? errno : 0);
    if (mapping == MAP_FAILED)
        return;

    // 0.25, 0.5 and 0.75 round to 0x40, 0x80 and 0xbf.
    const std::array<Clear, 2> clears = {{{0.25f, 0.5f, 0.75f, 0xffbf8040u},
                                          {1.0f, 0.0f, 1.0f, 0xffff00ffu}}};
    for (std::size_t round = 0; round < clears.size(); round++) {
        const Clear& clear = clears[round];
        gl.glBindFramebuffer(kGlFramebuffer, framebuffer);
        gl.glClearColor(clear.red, clear.green, clear.blue, 1.0f);
        gl.glClear(kGlColorBufferBit);
        gl.glFinish();
        char what[96];
        std::snprintf(what, sizeof(what), "T2 clear %zu is visible through the dma-buf after SYNC",
                      round + 1);
        if (sync(buffer.fd, kSyncRead)) {
            checkPixels(what, compareRows(mapping, buffer.stride, clear.word), clear.word);
            sync(buffer.fd, kSyncRead | kSyncEnd);
        }
        // The readback below is the control: the clear itself reached the GPU.
        std::array<std::uint32_t, kPixels> pixels;
        bool read = readGpu(pixels);
        Mismatch mismatch = comparePixels(pixels, clear.word);
        check(read && mismatch.count == 0, "T2 clear %zu reads back through GL: %zu of %zu pixels differ, want %08x",
              round + 1, mismatch.count, kPixels, clear.word);
    }
    munmap(mapping, length);
}

// The CPU writes the buffer through its dma-buf; the GPU reads the whole
// write once the writer brackets it.
void testCpuWriteVisible()
{
    Buffer buffer;
    if (!createBuffer(buffer))
        return;
    buffer.fd = gl.gbm_bo_get_fd(buffer.bo);
    if (buffer.fd < 0) {
        check(false, "T3 gbm exports the buffer");
        return;
    }
    const std::size_t length = static_cast<std::size_t>(buffer.stride) * kHeight;
    void* mapping = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, buffer.fd, 0);
    check(mapping != MAP_FAILED, "T3 the dma-buf maps for writing: errno %d",
          mapping == MAP_FAILED ? errno : 0);
    if (mapping == MAP_FAILED)
        return;
    unsigned framebuffer = framebufferFor(buffer.fd, buffer.stride);
    if (!framebuffer) {
        munmap(mapping, length);
        return;
    }

    const std::array<std::uint32_t, 2> words = {0xff112233u, 0xff445566u};
    for (std::size_t round = 0; round < words.size(); round++) {
        if (!sync(buffer.fd, kSyncWrite))
            break;
        for (int y = 0; y < kHeight; y++) {
            auto* row = reinterpret_cast<std::uint32_t*>(static_cast<unsigned char*>(mapping) +
                                                         static_cast<std::size_t>(y) * buffer.stride);
            for (int x = 0; x < kWidth; x++)
                row[x] = words[round];
        }
        if (!sync(buffer.fd, kSyncWrite | kSyncEnd))
            break;
        gl.glBindFramebuffer(kGlFramebuffer, framebuffer);
        std::array<std::uint32_t, kPixels> pixels;
        bool read = readGpu(pixels);
        char what[96];
        std::snprintf(what, sizeof(what), "T3 CPU write %zu is what the GPU reads", round + 1);
        checkPixels(what, comparePixels(pixels, words[round]), words[round]);
        check(read, "T3 the GL readback %zu raises no error", round + 1);
    }
    munmap(mapping, length);
}

}  // namespace

int main(int argc, char** argv)
{
    const char* node = argc > 1 ? argv[1] : "/dev/dri/renderD128";

    if (loadLibraries() && setupGl(node)) {
        testExportMode();
        testGpuWriteVisible();
        testCpuWriteVisible();
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    bool passed = failures == 0 && checks > 0;
    std::printf("UML_GBM_DMABUF_PROBE: %s\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
