// SPDX-License-Identifier: GPL-2.0-or-later

// A draw uploads the constants of its program, and the vertex attributes it
// has no array for, into buffers the host driver allocates for it. With a
// bounded device memory budget the driver refuses the next of those buffers
// once the budget is full, and the state tracker must not copy into a buffer
// it did not get: the host faulted on a NULL pointer, on the thread that
// runs the context's draws, and took the application down with it.
//
// The contract under test is what the host does when the upload memory of a
// draw is refused:
//
//  - the host survives the refusal, however many draws meet it;
//  - the draw that could not be set up is skipped and raises
//    GL_OUT_OF_MEMORY, rather than drawing with constants it never got; and
//  - once the memory comes back, draws are made again: the state the refused
//    draw left unfinished is set up again by the next one.
//
// The draws here upload 32 KiB of constants each, so a few dozen of them fill
// one of the uploader's 2 MiB buffers, and the buffers of the draws still in
// flight cannot be freed: the budget runs out within a few hundred draws.

#include "host_gl_test_support.hpp"
#include "host_gpu_memory.hpp"
#include "require.hpp"
#include "virgl_renderer.hpp"

#include <epoxy/gl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <thread>

namespace {

using uml::gpu::testing::skipWithoutRenderer;
using uml::testing::require;
using namespace std::chrono_literals;

constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;
constexpr std::uint64_t roomBytes = 48 * mebibyte;
constexpr int drawsPerRound = 256;
constexpr int maximumDraws = 40000;
constexpr int refusedRounds = 3;

constexpr const char* vertexShader = R"(#version 450 core
uniform vec4 values[2048];
uniform int index;
out vec4 tint;
void main()
{
    tint = values[index & 2047];
    gl_Position = vec4(-0.75, -0.75, 0.0, 1.0);
    gl_PointSize = 1.0;
}
)";

constexpr const char* fragmentShader = R"(#version 450 core
in vec4 tint;
out vec4 color;
void main()
{
    color = tint;
}
)";

// What the pool counts: the larger of Metal's allocation and the graphics
// footprint, since nothing here is imported.
[[nodiscard]] std::uint64_t used() noexcept
{
    return std::max(uml::platform::hostGpuAllocatedBytes(),
        uml::platform::hostGraphicsFootprintBytes());
}

// Draws a point at the first pixel of the bound target, in the colour of
// values[index & 2047].
void drawPoint(GLint indexLocation, int index)
{
    glUniform1i(indexLocation, index);
    glDrawArrays(GL_POINTS, 0, 1);
}

// Reads the target's first pixel.
[[nodiscard]] std::array<std::uint8_t, 4> firstPixel()
{
    std::array<std::uint8_t, 4> pixel {};
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    return pixel;
}

} // namespace

int main()
{
    // The pool takes its limit when the renderer starts, and counts everything
    // the process holds, so the limit is what is held now plus the room.
    uml::gpu::VirglRenderer::setDeviceMemoryLimit(used() + roomBytes);
    std::unique_ptr<uml::gpu::VirglRenderer> renderer;
    try {
        renderer = std::make_unique<uml::gpu::VirglRenderer>();
    } catch (const std::exception& error) {
        return skipWithoutRenderer(error);
    }

    uml::gpu::HostGlProvider provider;
    const auto current = uml::gpu::testing::currentContext(provider, 4, 6);
    if (current.context == nullptr)
        return uml::gpu::testing::skip(current.failure);

    const GLuint program = uml::gpu::testing::linkProgram(
        {uml::gpu::testing::compileShader(GL_VERTEX_SHADER, vertexShader),
            uml::gpu::testing::compileShader(
                GL_FRAGMENT_SHADER, fragmentShader)});
    glUseProgram(program);
    const GLint indexLocation = glGetUniformLocation(program, "index");
    const GLint valuesLocation = glGetUniformLocation(program, "values");
    require(indexLocation >= 0 && valuesLocation >= 0,
        "the program's uniforms were not found");

    GLuint vertexArray = 0;
    glGenVertexArrays(1, &vertexArray);
    glBindVertexArray(vertexArray);
    GLuint target = 0;
    glGenTextures(1, &target);
    glBindTexture(GL_TEXTURE_2D, target);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 4, 4);
    GLuint framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    glViewport(0, 0, 4, 4);
    glEnable(GL_PROGRAM_POINT_SIZE);

    // values[0] is green; the draws below read it.
    const std::array<GLfloat, 4> green {0.0F, 1.0F, 0.0F, 1.0F};
    glUniform4fv(valuesLocation, 1, green.data());
    glClearColor(1.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    drawPoint(indexLocation, 0);
    glFinish();
    require(glGetError() == GL_NO_ERROR, "the first draw raised an error");
    require(firstPixel() == std::array<std::uint8_t, 4> {0, 255, 0, 255},
        "the first draw did not show the program's colour");

    // Draws until the budget refuses the uploader's next buffer. The draws
    // that follow keep meeting the refusal.
    int draws = 0;
    int refused = 0;
    while (refused < refusedRounds && draws < maximumDraws) {
        for (int index = 0; index < drawsPerRound; ++index)
            drawPoint(indexLocation, ++draws);
        const GLenum error = glGetError();
        if (error == GL_OUT_OF_MEMORY) {
            ++refused;
        } else {
            require(error == GL_NO_ERROR,
                "a draw raised an error other than out of memory");
        }
    }
    std::fprintf(stderr,
        "%d draws; %d rounds of %d met a refused upload (limit %.1f MiB over "
        "%.1f MiB)\n",
        draws, refused, drawsPerRound, static_cast<double>(roomBytes) / mebibyte,
        static_cast<double>(used()) / mebibyte);
    require(refused > 0,
        "the budget never refused a draw's upload, so nothing was proven");

    // The GPU finishes what the draws before the refusal started, which gives
    // the memory back; the next draws set their state up again and show their
    // colour.
    glFinish();
    (void)glGetError();
    glClear(GL_COLOR_BUFFER_BIT);
    bool shown = false;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!shown && std::chrono::steady_clock::now() < deadline) {
        glClear(GL_COLOR_BUFFER_BIT);
        drawPoint(indexLocation, 0);
        glFinish();
        (void)glGetError();
        shown = firstPixel() == std::array<std::uint8_t, 4> {0, 255, 0, 255};
        if (!shown)
            std::this_thread::sleep_for(50ms);
    }
    require(shown, "draws did not resume once the memory came back");

    std::fputs("Upload refusal: ok\n", stderr);
    return EXIT_SUCCESS;
}
