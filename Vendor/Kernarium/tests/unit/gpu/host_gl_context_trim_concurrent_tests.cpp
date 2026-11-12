// SPDX-License-Identifier: GPL-2.0-or-later

// Several host GL contexts of one driver screen draw, delete what they drew
// and flush while some of them trim, each on its own thread. The trim hands a
// context's completed batch states to the screen and resets the screen's
// completed states, which the flush queue of every context also does after
// each submission; the two must never touch a state at once, release one
// twice, or lose one. Every context keeps drawing correctly throughout, so a
// state released early or reused wrongly shows up as wrong pixels or a GL
// error, and a race on the screen's lists shows up under the thread sanitizer
// that this test is also built with.

#include "host_gl_test_support.hpp"
#include "require.hpp"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>
#include <vector>

namespace {

using uml::gpu::testing::colourTarget;
using uml::gpu::testing::TextureStorage;

typedef EGLBoolean (*TrimContext)();

constexpr GLsizei size = 384;
constexpr int rounds = 300;

std::atomic<int> failures {};
std::atomic<int> trims {};

void fail(const char* what, int worker, int round)
{
    std::fprintf(stderr, "worker %d round %d: %s\n", worker, round, what);
    ++failures;
}

/* One context's life. `trimEvery` is how often it trims, zero never: a
 * context that never trims still has its completed states reset by another's
 * trim. Returns when it has done `rounds` rounds or another worker failed. */
void worker(
    uml::gpu::HostGlProvider& provider,
    int index,
    int trimEvery,
    TrimContext trim)
{
    const auto current = uml::gpu::testing::currentContext(provider, 4, 5);
    if (current.context == nullptr) {
        fail(current.failure, index, 0);
        return;
    }
    for (int round = 0; round < rounds && failures == 0; ++round) {
        const auto target = colourTarget(size, size, TextureStorage::immutable);
        if (!target.complete) {
            fail("incomplete render target", index, round);
            return;
        }
        const float red = static_cast<float>((round * 37 + index * 11) % 256) / 255.0F;
        glClearColor(red, 0.5F, 0.25F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (round % 7 == 0) {
            /* Read back through a fresh binding so the pixels are checked. */
            glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
            std::array<std::uint8_t, 4> pixel {};
            glReadPixels(size / 2, size / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            const int expected = static_cast<int>(red * 255.0F + 0.5F);
            if (std::abs(static_cast<int>(pixel[0]) - expected) > 1 ||
                pixel[3] != 255) {
                fail("wrong pixel after a draw", index, round);
                return;
            }
        }
        glDeleteFramebuffers(1, &target.framebuffer);
        glDeleteTextures(1, &target.texture);
        glFlush();
        if (round % 11 == 0)
            glFinish();
        if (trimEvery != 0 && round % trimEvery == 0) {
            if (trim() != EGL_TRUE) {
                fail("trim failed", index, round);
                return;
            }
            ++trims;
        }
        if (glGetError() != GL_NO_ERROR) {
            fail("GL error", index, round);
            return;
        }
    }
    glFinish();
}

} // namespace

int main()
{
    uml::gpu::HostGlProvider provider;
    const auto probe = uml::gpu::testing::currentContext(provider, 4, 5);
    if (probe.context == nullptr)
        return uml::gpu::testing::skip(probe.failure);

    const EGLDisplay display = eglGetCurrentDisplay();
    uml::testing::require(
        epoxy_has_egl_extension(display, "EGL_MESA_context_trim"),
        "the driver does not advertise EGL_MESA_context_trim");
    const auto trim = reinterpret_cast<TrimContext>(
        eglGetProcAddress("eglTrimContextMESA"));
    uml::testing::require(trim != nullptr, "eglTrimContextMESA was not found");
    /* The probe context is current on this thread; the workers have their own,
     * and this thread does no GL again. */
    uml::testing::require(provider.makeCurrent(nullptr) == 0,
        "the probe context would not release");

    std::vector<std::thread> workers;
    workers.emplace_back(worker, std::ref(provider), 0, 1, trim);
    workers.emplace_back(worker, std::ref(provider), 1, 5, trim);
    workers.emplace_back(worker, std::ref(provider), 2, 0, trim);
    workers.emplace_back(worker, std::ref(provider), 3, 3, trim);
    for (auto& thread : workers)
        thread.join();

    if (failures != 0)
        return EXIT_FAILURE;
    std::printf(
        "PASS concurrent context trim: 4 contexts, %d rounds each, %d trims\n",
        rounds,
        trims.load());
    return EXIT_SUCCESS;
}
