// SPDX-License-Identifier: GPL-2.0-or-later

// The host GL driver keeps the buffers a context freed in a cache, and what it
// caches counts against the device memory pool that every consumer of the
// process charges. The driver frees the cache only when one of its own
// allocations fails, so a Venus guest whose allocation the pool refused could
// not make the room. eglReleaseCachedMemoryMESA frees what every screen of the
// process caches, from any thread and with no context current, and the pool's
// reclaim hook calls it before it refuses.
//
// Metal's allocation counter is the observable: buffers a context wrote and
// deleted stay allocated, held by the driver's cache, until the release.

#include "host_gl_test_support.hpp"
#include "host_gpu_memory.hpp"
#include "require.hpp"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

using uml::testing::require;
using namespace std::chrono_literals;

constexpr std::uint64_t mebibyte = 1U << 20U;
constexpr GLsizeiptr bufferBytes = static_cast<GLsizeiptr>(4U * mebibyte);
constexpr int bufferCount = 5;

typedef EGLint (*ReleaseCachedMemory)();

[[nodiscard]] std::uint64_t allocated()
{
    return uml::platform::hostGpuAllocatedBytes();
}

/* Writes and deletes buffers so that the driver has them to cache: each is
 * written so that its memory is real, and the context is finished with them
 * before they are deleted. */
void fillAndDeleteBuffers(const std::vector<std::uint8_t>& bytes)
{
    std::vector<GLuint> buffers(bufferCount);
    glCreateBuffers(bufferCount, buffers.data());
    for (const GLuint buffer : buffers) {
        glNamedBufferData(buffer, bufferBytes, nullptr, GL_STATIC_DRAW);
        glNamedBufferSubData(buffer, 0, bufferBytes, bytes.data());
    }
    glFinish();
    glDeleteBuffers(bufferCount, buffers.data());
    glFinish();
    require(glGetError() == GL_NO_ERROR, "GL reported an error");
}

/* Waits for the allocation counter to settle at or below `limit`. */
[[nodiscard]] bool fallsBelow(
    std::uint64_t limit, std::chrono::seconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (allocated() <= limit)
            return true;
        std::this_thread::sleep_for(50ms);
    }
    return allocated() <= limit;
}

} // namespace

int main()
{
    uml::gpu::HostGlProvider provider;
    const auto current = uml::gpu::testing::currentContext(provider, 4, 5);
    if (current.context == nullptr)
        return uml::gpu::testing::skip(current.failure);

    const EGLDisplay display = eglGetCurrentDisplay();
    require(display != EGL_NO_DISPLAY, "no current EGL display");
    require(epoxy_has_egl_extension(display, "EGL_MESA_context_trim"),
        "the driver does not advertise EGL_MESA_context_trim");
    const auto release = reinterpret_cast<ReleaseCachedMemory>(
        eglGetProcAddress("eglReleaseCachedMemoryMESA"));
    require(release != nullptr, "eglReleaseCachedMemoryMESA was not found");

    const std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(bufferBytes), 0x5a);

    /* A first round warms the driver up and fills its cache, which the first
     * release then empties, so that the baseline holds nothing cached. */
    fillAndDeleteBuffers(bytes);
    (void)release();
    std::this_thread::sleep_for(300ms);
    const std::uint64_t baseline = allocated();

    fillAndDeleteBuffers(bytes);
    std::this_thread::sleep_for(300ms);
    const std::uint64_t cached = allocated();
    std::printf("%llu MiB cached after the buffers were deleted\n",
        static_cast<unsigned long long>(
            (cached > baseline ? cached - baseline : 0) / mebibyte));
    require(cached >= baseline + 2 * bufferBytes,
        "the driver cached nothing, so the release has nothing to prove");

    /* From a thread that has no context current, as the pool's refused thread
     * may be. */
    EGLint released = 0;
    std::thread other([&] { released = release(); });
    other.join();
    std::printf("released %d cached allocations\n", released);
    require(released > 0, "the release freed nothing");
    require(fallsBelow(baseline + 2U * bufferBytes, 5s),
        "the cached memory was still allocated after the release");

    /* Nothing is left to release, and the context goes on working. */
    require(release() >= 0, "a second release failed");
    fillAndDeleteBuffers(bytes);
    require(release() > 0, "the driver did not cache again after a release");
    require(glGetError() == GL_NO_ERROR, "GL reported an error after release");

    std::printf("PASS cached memory release: freed from another thread, "
                "cache refilled afterwards\n");
    return EXIT_SUCCESS;
}
