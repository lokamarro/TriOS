// SPDX-License-Identifier: GPL-2.0-or-later

// Metal counts a buffer the host driver makes over memory it was lent at the
// buffer's full size, so the device memory pool, which charges the memory made
// for the GPU, would count the shared-memory segments and pinned pages a guest
// maps as if they were GPU allocations. The renderer therefore reports the
// bytes the host's Vulkan devices hold by import, and the pool subtracts them
// from Metal's counter.
//
// The contract under test is the wiring from the driver to the host: an
// image the host GL provider lays over 32 MiB of the caller's pages raises
// the renderer's import count by those pages, Metal's counter rises with it,
// the difference between the two (the memory made for the GPU) does not, and
// everything returns to where it was once the image is gone.

// The renderer reports imports through its unstable interface.
#define VIRGL_RENDERER_UNSTABLE_APIS

#include "host_gl_test_support.hpp"
#include "host_gpu_memory.hpp"
#include "require.hpp"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include <sys/mman.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

#ifndef EGL_HOST_POINTER_MESA
#define EGL_HOST_POINTER_MESA 0x3293
#define EGL_HOST_POINTER_PITCH_MESA 0x3294
#define EGL_HOST_POINTER_OFFSET_MESA 0x3295
#endif
#ifndef EGL_HOST_POINTER_SIZE_MESA
#define EGL_HOST_POINTER_SIZE_MESA 0x3296
#endif

namespace {

using namespace std::chrono_literals;
using uml::testing::require;

constexpr std::int32_t fourccAbgr8888 = 0x34324241;
constexpr std::uint32_t width = 2048;
constexpr std::uint32_t height = 4096;
constexpr std::size_t pitch = static_cast<std::size_t>(width) * 4;
constexpr std::size_t imageBytes = pitch * height;
constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;
// What the import's render target, command storage and pipelines add, none of
// which is lent memory.
constexpr std::uint64_t slackBytes = 12 * mebibyte;

typedef EGLBoolean (*TrimContext)();

[[nodiscard]] double mib(std::uint64_t bytes) noexcept
{
    return static_cast<double>(bytes) / mebibyte;
}

// What the drivers made for the GPU: Metal's counter less the lent memory.
// The import count is read first, so a buffer that appears between the two
// reads is never subtracted before Metal counted it.
[[nodiscard]] std::uint64_t made(std::uint64_t& imported) noexcept
{
    imported = virgl_renderer_get_imported_host_memory();
    const std::uint64_t metal = uml::platform::hostGpuAllocatedBytes();
    return metal > imported ? metal - imported : 0;
}

} // namespace

int main()
{
    uml::gpu::HostGlProvider provider;
    const auto current =
        uml::gpu::testing::currentContext(provider, 4, 6);
    if (current.context == nullptr)
        return uml::gpu::testing::skip(current.failure);

    auto* const display = static_cast<EGLDisplay>(provider.display());
    require(display != EGL_NO_DISPLAY, "no host EGL display");
    const char* const extensions = eglQueryString(display, EGL_EXTENSIONS);
    require(extensions != nullptr &&
            std::strstr(extensions, "EGL_MESA_image_host_pointer") != nullptr,
        "the host driver cannot lay an image over the caller's pages");
    const auto trim = reinterpret_cast<TrimContext>(
        eglGetProcAddress("eglTrimContextMESA"));
    require(trim != nullptr, "eglTrimContextMESA was not found");

    // Warm the driver up, so the baseline already holds its own storage.
    glFinish();
    std::uint64_t importedBefore = 0;
    const std::uint64_t madeBefore = made(importedBefore);
    const std::uint64_t metalBefore = uml::platform::hostGpuAllocatedBytes();

    void* const pages = mmap(nullptr, imageBytes, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANON, -1, 0);
    require(pages != MAP_FAILED, "the pages could not be mapped");
    static_cast<std::uint8_t*>(pages)[0] = 1;

    const EGLint attributes[] = {
        EGL_WIDTH, static_cast<EGLint>(width),
        EGL_HEIGHT, static_cast<EGLint>(height),
        EGL_LINUX_DRM_FOURCC_EXT, fourccAbgr8888,
        EGL_HOST_POINTER_PITCH_MESA, static_cast<EGLint>(pitch),
        EGL_HOST_POINTER_SIZE_MESA, static_cast<EGLint>(imageBytes),
        EGL_NONE};
    const EGLImageKHR image = eglCreateImageKHR(display, EGL_NO_CONTEXT,
        EGL_HOST_POINTER_MESA, static_cast<EGLClientBuffer>(pages),
        attributes);
    require(image != EGL_NO_IMAGE_KHR, "the image could not be laid over the "
                                      "pages");
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    require(glGetError() == GL_NO_ERROR, "the image could not be bound");

    // Sample the first texel into a small target, so the driver has used the
    // image and imported the pages.
    GLuint framebuffer = 0;
    GLuint target = 0;
    glGenTextures(1, &target);
    glBindTexture(GL_TEXTURE_2D, target);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 4, 4);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    GLuint source = 0;
    glGenFramebuffers(1, &source);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
    glFramebufferTexture2D(
        GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    glBlitFramebuffer(0, 0, 4, 4, 0, 0, 4, 4, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glFinish();
    require(glGetError() == GL_NO_ERROR, "the image could not be read");

    std::uint64_t importedDuring = 0;
    const std::uint64_t madeDuring = made(importedDuring);
    const std::uint64_t metalDuring = uml::platform::hostGpuAllocatedBytes();
    std::fprintf(stderr,
        "image of %.1f MiB: imports %.1f -> %.1f MiB, Metal %.1f -> %.1f "
        "MiB, made for the GPU %.1f -> %.1f MiB\n",
        mib(imageBytes), mib(importedBefore), mib(importedDuring),
        mib(metalBefore), mib(metalDuring), mib(madeBefore), mib(madeDuring));

    require(importedDuring >= importedBefore + imageBytes,
        "the renderer did not count the pages the driver imported");
    require(metalDuring >= metalBefore + imageBytes,
        "Metal's counter did not rise by the import: the subtraction would "
        "take memory the drivers made");
    require(importedDuring - importedBefore <= metalDuring - metalBefore,
        "the import count passed what Metal counts");
    require(madeDuring <= madeBefore + slackBytes,
        "the memory made for the GPU rose with a lent import");

    // Everything the image held returns once it is gone.
    glDeleteFramebuffers(1, &source);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &target);
    glDeleteTextures(1, &texture);
    eglDestroyImageKHR(display, image);
    glFinish();
    std::uint64_t importedAfter = importedDuring;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        (void)trim();
        glFinish();
        std::uint64_t imported = 0;
        (void)made(imported);
        importedAfter = imported;
        if (importedAfter <= importedBefore)
            break;
        std::this_thread::sleep_for(100ms);
    }
    munmap(pages, imageBytes);
    std::fprintf(stderr, "after the image: imports %.1f MiB\n",
        mib(importedAfter));
    require(importedAfter <= importedBefore,
        "the renderer still counts pages the driver let go");
    std::fputs("Imported memory count: ok\n", stderr);
    return 0;
}
