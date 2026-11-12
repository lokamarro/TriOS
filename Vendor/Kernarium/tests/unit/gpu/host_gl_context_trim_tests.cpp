// SPDX-License-Identifier: GPL-2.0-or-later

// A host GL context that has been flushed and left alone keeps the resources
// of its last batches until it submits again, which for a guest compositor
// with nothing to draw is minutes. EGL_MESA_context_trim is how the renderer
// tells the driver that a context has gone idle. The driver must then release
// what completed work held, without waiting for the device, and the context
// must go on working: the same context draws, reads back and trims again.
//
// Metal's allocation counter is the observable: textures the context has
// written, read back and deleted are still held by the batch that used them
// until the context is trimmed, and leave the counter when it is. The process
// footprint is not, because Metal returns a released allocation's pages to it
// on its own schedule, sometimes seconds later.

#include "host_gl_test_support.hpp"
#include "host_gpu_memory.hpp"
#include "require.hpp"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

using uml::gpu::testing::colourTarget;
using uml::gpu::testing::TextureStorage;
using uml::testing::require;
using namespace std::chrono_literals;

constexpr GLsizei width = 2304;
constexpr GLsizei height = 1408;
constexpr std::uint64_t textureBytes =
    static_cast<std::uint64_t>(width) * height * 4U;
constexpr int textureCount = 4;
constexpr std::uint64_t mebibyte = 1U << 20U;

typedef EGLBoolean (*TrimContext)();

[[nodiscard]] std::uint64_t allocated()
{
    return uml::platform::hostGpuAllocatedBytes();
}

/* How much the allocation counter has grown since `baseline`. */
[[nodiscard]] std::uint64_t grown(std::uint64_t baseline)
{
    const std::uint64_t now = allocated();
    return now > baseline ? now - baseline : 0;
}

/* A texture's worth of bytes that no texture compression can shrink. */
[[nodiscard]] std::vector<std::uint8_t> makeNoise()
{
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(textureBytes));
    std::uint32_t state = 0x9e3779b9U;
    for (auto& byte : bytes) {
        state = state * 1664525U + 1013904223U;
        byte = static_cast<std::uint8_t>(state >> 24U);
    }
    return bytes;
}

/* Fills several full-size textures with data that does not compress, reads a
 * texel of each back so that the work is known to have completed, and
 * deletes them all. The textures are only written and read as images: none
 * is ever bound to a framebuffer or a sampler, so the batch that used a
 * texture is the last thing that holds it. The context is flushed and idle,
 * and the textures are gone as far as GL can tell. */
void drawAndDeleteTextures(const std::vector<std::uint8_t>& noise)
{
    std::vector<GLuint> textures(textureCount);
    glCreateTextures(GL_TEXTURE_2D, textureCount, textures.data());
    for (const GLuint texture : textures) {
        glTextureStorage2D(texture, 1, GL_RGBA8, width, height);
        glTextureSubImage2D(
            texture, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, noise.data());
        std::array<std::uint8_t, 4> texel {};
        glGetTextureSubImage(
            texture, 0, 0, 0, 0, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
            static_cast<GLsizei>(texel.size()), texel.data());
        require(texel[0] == noise[0] && texel[1] == noise[1] &&
                texel[2] == noise[2] && texel[3] == noise[3],
            "a texture did not hold what was written to it");
    }
    glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
    /* A flush, not a finish: waiting for the device is what lets the driver
     * reset every batch it has, and this test is about the context that did
     * not wait. The readbacks above already made the work complete. */
    glFlush();
    require(glGetError() == GL_NO_ERROR, "GL reported an error");
}

/* Waits for the counter to settle at or below `limit` above the baseline. */
[[nodiscard]] bool fallsBelow(
    std::uint64_t baseline, std::uint64_t limit, std::chrono::seconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (grown(baseline) <= limit)
            return true;
        std::this_thread::sleep_for(50ms);
    }
    return grown(baseline) <= limit;
}

} // namespace

int main()
{
    /* Nothing may be cached, so a released object is gone at once and the
     * counter shows it. Before the driver starts. */
    ::setenv("ZINK_BO_CACHE_MAX_MIB", "0", 1);

    const std::vector<std::uint8_t> noise = makeNoise();
    uml::gpu::HostGlProvider provider;
    const auto current = uml::gpu::testing::currentContext(provider, 4, 5);
    if (current.context == nullptr)
        return uml::gpu::testing::skip(current.failure);

    const EGLDisplay display = eglGetCurrentDisplay();
    require(display != EGL_NO_DISPLAY, "no current EGL display");
    require(epoxy_has_egl_extension(display, "EGL_MESA_context_trim"),
        "the driver does not advertise EGL_MESA_context_trim");
    const auto trim = reinterpret_cast<TrimContext>(
        eglGetProcAddress("eglTrimContextMESA"));
    require(trim != nullptr, "eglTrimContextMESA was not found");

    /* Start the driver's batches so that the baseline includes everything but
     * the textures: a first draw, a first readback. */
    {
        const auto warm = colourTarget(64, 64, TextureStorage::immutable);
        require(warm.complete, "the warm-up target was incomplete");
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &warm.framebuffer);
        glDeleteTextures(1, &warm.texture);
        glFinish();
        require(trim() == EGL_TRUE, "the first trim failed");
    }
    std::this_thread::sleep_for(200ms);

    constexpr std::uint64_t total = textureBytes * textureCount;
    for (int round = 0; round < 2; ++round) {
        const std::uint64_t baseline = allocated();
        drawAndDeleteTextures(noise);

        /* Held: the context has nothing to submit, so nothing lets go of the
         * last batches' objects. The batches before them were handed on as
         * the context started the next ones and are reset by the next
         * submission, so what stays is the last texture or two, not all. */
        std::this_thread::sleep_for(1500ms);
        const std::uint64_t held = grown(baseline);
        std::printf(
            "round %d: %llu MiB of %llu MiB held before the trim\n",
            round,
            static_cast<unsigned long long>(held / mebibyte),
            static_cast<unsigned long long>(total / mebibyte));
        require(held >= textureBytes - (textureBytes >> 2U),
            "the idle context held nothing, so the trim has nothing to prove");

        glFlush();
        require(trim() == EGL_TRUE, "the trim failed");
        require(fallsBelow(baseline, textureBytes >> 2U, 5s),
            "the idle context kept the textures after the trim");
        std::printf("round %d: %llu MiB held after the trim\n",
            round,
            static_cast<unsigned long long>(grown(baseline) / mebibyte));
    }

    /* The context is still fine: it draws and reads back, then trims again
     * with nothing left to release. */
    drawAndDeleteTextures(noise);
    require(trim() == EGL_TRUE, "a trim after more work failed");
    require(trim() == EGL_TRUE, "a second trim in a row failed");
    glFlush();
    const auto target = colourTarget(16, 16, TextureStorage::immutable);
    require(target.complete, "the render target after the trim was incomplete");
    glClearColor(1.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    std::array<std::uint8_t, 4> pixel {};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    require(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255,
        "the context drew the wrong pixel after the trim");
    require(glGetError() == GL_NO_ERROR, "GL reported an error after the trim");

    std::printf("PASS context trim: idle textures released, context still works\n");
    return EXIT_SUCCESS;
}
