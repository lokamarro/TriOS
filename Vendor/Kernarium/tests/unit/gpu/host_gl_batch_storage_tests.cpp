// SPDX-License-Identifier: GPL-2.0-or-later

// The driver keeps on its screen every batch state a context has needed, for
// the next context to take, and a free state keeps the command storage of the
// last batch it recorded. Without a release, the storage that a heavy client's
// batches grew to stays allocated for as long as the renderer lives, after the
// client is gone: an Android guest that has closed every application still
// holds the command upload buffers its heaviest one needed.
//
// Metal's allocation counter is the observable. A second context draws frames
// of thousands of draws, each of which the command buffer uploads state for,
// and flushes every frame without waiting, so the driver needs several batch
// states and each grows its upload storage. Once the context is finished and
// destroyed, the counter must come back to what it was before the context
// existed, within the small buffers its uniforms used, which the screen's
// slab cache keeps until a host asks for its cached memory; once the host
// has, it must come back all the way. The first context, which stays, then
// draws once more, so the states it takes back are usable.

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

namespace {

using uml::gpu::testing::colourTarget;
using uml::gpu::testing::compileShader;
using uml::gpu::testing::linkProgram;
using uml::gpu::testing::TextureStorage;
using uml::testing::require;
using namespace std::chrono_literals;

typedef EGLint (*ReleaseCachedMemory)();

constexpr std::uint64_t mebibyte = 1U << 20U;
constexpr GLsizei targetSize = 256;
constexpr int heavyFrames = 48;
constexpr int heavyDraws = 4096;
// What may stay once the context is gone: the slabs of its uniform buffers
// until the host releases the screen's caches, then the driver's own small
// allocations. Kept command storage is many times either.
constexpr std::uint64_t slabSlackBytes = 32 * mebibyte;
constexpr std::uint64_t slackBytes = 4 * mebibyte;
// What the heavy context must have grown for the test to prove anything.
constexpr std::uint64_t grownAtLeast = 16 * mebibyte;

constexpr const char* vertexSource = R"(#version 450 core
uniform vec4 offset;
void main()
{
    const vec2 corners[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(corners[gl_VertexID] * 0.01 + offset.xy, 0.0, 1.0);
}
)";

constexpr const char* fragmentSource = R"(#version 450 core
uniform vec4 colour;
out vec4 result;
void main()
{
    result = colour;
}
)";

[[nodiscard]] std::uint64_t allocated()
{
    return uml::platform::hostGpuAllocatedBytes();
}

[[nodiscard]] std::uint64_t grown(std::uint64_t baseline)
{
    const std::uint64_t now = allocated();
    return now > baseline ? now - baseline : 0;
}

/* Draws `frames` frames of `drawsPerFrame` draws into a small target, with
 * new uniforms for every draw, and flushes each frame without waiting. */
void drawHeavily(int frames, int drawsPerFrame)
{
    const auto target = colourTarget(targetSize, targetSize, TextureStorage::immutable);
    require(target.complete, "the heavy context's target was incomplete");
    const GLuint program = linkProgram({compileShader(GL_VERTEX_SHADER, vertexSource),
        compileShader(GL_FRAGMENT_SHADER, fragmentSource)});
    glUseProgram(program);
    const GLint offset = glGetUniformLocation(program, "offset");
    const GLint colour = glGetUniformLocation(program, "colour");
    require(offset >= 0 && colour >= 0, "the program has no uniforms");
    GLuint vertexArray = 0;
    glGenVertexArrays(1, &vertexArray);
    glBindVertexArray(vertexArray);
    glViewport(0, 0, targetSize, targetSize);
    for (int frame = 0; frame < frames; ++frame) {
        glClear(GL_COLOR_BUFFER_BIT);
        for (int draw = 0; draw < drawsPerFrame; ++draw) {
            const float x = static_cast<float>(draw % 64) / 32.0F - 1.0F;
            const float y = static_cast<float>(draw / 64 % 64) / 32.0F - 1.0F;
            glUniform4f(offset, x, y, 0.0F, 0.0F);
            glUniform4f(colour, static_cast<float>(frame) / frames, x, y, 1.0F);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glFlush();
    }
    std::array<std::uint8_t, 4> pixel {};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    require(glGetError() == GL_NO_ERROR, "the heavy context reported an error");
    glFinish();
}

} // namespace

int main()
{
    /* Nothing may be cached, so a released buffer leaves the counter at once.
     * Before the driver starts. */
    ::setenv("ZINK_BO_CACHE_MAX_MIB", "0", 1);

    uml::gpu::HostGlProvider provider;
    const auto first = uml::gpu::testing::currentContext(provider, 4, 5);
    if (first.context == nullptr)
        return uml::gpu::testing::skip(first.failure);
    const EGLDisplay display = eglGetCurrentDisplay();
    require(display != EGL_NO_DISPLAY, "no current EGL display");
    require(epoxy_has_egl_extension(display, "EGL_MESA_context_trim"),
        "the driver does not advertise EGL_MESA_context_trim");
    const auto release = reinterpret_cast<ReleaseCachedMemory>(
        eglGetProcAddress("eglReleaseCachedMemoryMESA"));
    require(release != nullptr, "eglReleaseCachedMemoryMESA was not found");

    /* The same draws once, so that what the device makes once for every
     * context, such as the heap its first draw creates, is in the baseline. */
    drawHeavily(1, 1);
    release();
    std::this_thread::sleep_for(200ms);
    const std::uint64_t baseline = allocated();

    void* const heavy = provider.createContext(
        uml::gpu::testing::glContextParameters(4, 5));
    require(heavy != nullptr, "no second host GL context");
    require(provider.makeCurrent(heavy) == 0, "the second context would not bind");
    drawHeavily(heavyFrames, heavyDraws);
    const std::uint64_t peak = grown(baseline);
    std::printf("the heavy context grew the counter by %llu MiB\n",
        static_cast<unsigned long long>(peak / mebibyte));
    require(peak >= grownAtLeast,
        "the heavy context grew too little for the test to prove anything");

    require(provider.makeCurrent(first.context) == 0, "the first context would not bind");
    provider.destroyContext(heavy);
    std::this_thread::sleep_for(500ms);
    const std::uint64_t kept = grown(baseline);
    std::printf("%llu KiB stayed once the heavy context was destroyed\n",
        static_cast<unsigned long long>(kept >> 10U));
    require(kept <= slabSlackBytes,
        "the driver kept the command storage of a destroyed context");
    release();
    std::this_thread::sleep_for(200ms);
    const std::uint64_t cached = grown(baseline);
    std::printf("%llu KiB stayed once the host released the cached memory\n",
        static_cast<unsigned long long>(cached >> 10U));
    require(cached <= slackBytes,
        "the driver kept memory the host asked for once the context was gone");

    /* The first context takes the released states back and draws with them. */
    const auto target = colourTarget(16, 16, TextureStorage::immutable);
    require(target.complete, "the render target after the release was incomplete");
    glClearColor(0.0F, 1.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    std::array<std::uint8_t, 4> pixel {};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    require(pixel[0] == 0 && pixel[1] == 255 && pixel[2] == 0 && pixel[3] == 255,
        "the first context drew the wrong pixel after the release");
    require(glGetError() == GL_NO_ERROR, "GL reported an error after the release");

    std::printf("PASS batch storage: a destroyed context's command storage is released\n");
    return EXIT_SUCCESS;
}
