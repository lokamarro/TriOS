// SPDX-License-Identifier: GPL-2.0-or-later

// A GL application may poll GL_QUERY_RESULT_AVAILABLE without ever waiting and
// read the result the first time the query reports available. Zink answers
// such a poll from vkGetQueryPoolResults without VK_QUERY_RESULT_WAIT_BIT and
// keeps the first result it reads, so a host Vulkan driver that lets a
// pipeline-statistics query's availability become visible before its counts
// hands the application a zero that never corrects itself.
// KHR-GL46.pipeline_statistics_query_tests_ARB.functional_geometry_shader_
// queries polls exactly this way.
//
// The contract under test, on the host GL stack alone with no guest and no
// virtio transport: a statistics query that a poll finds available already
// holds its counts, for every single-statistic target a draw feeds.

#include "host_gl_test_support.hpp"

#include <epoxy/gl.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace {

using uml::gpu::testing::colourTarget;
using uml::gpu::testing::compileShader;
using uml::gpu::testing::currentContext;
using uml::gpu::testing::linkProgram;
using uml::gpu::testing::skip;

constexpr int iterations = 200;

constexpr const char* vertexSource =
    "#version 330 core\n"
    "layout(location = 0) in vec2 position;\n"
    "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n";

constexpr const char* fragmentSource =
    "#version 330 core\n"
    "out vec4 colour;\n"
    "void main() { colour = vec4(1.0); }\n";

struct Statistic {
    GLenum target;
    const char* name;
    GLuint expected;
};

// One triangle of three vertices, drawn without indices.
constexpr std::array<Statistic, 4> statistics {{
    {GL_VERTICES_SUBMITTED_ARB, "GL_VERTICES_SUBMITTED_ARB", 3},
    {GL_PRIMITIVES_SUBMITTED_ARB, "GL_PRIMITIVES_SUBMITTED_ARB", 1},
    {GL_VERTEX_SHADER_INVOCATIONS_ARB, "GL_VERTEX_SHADER_INVOCATIONS_ARB", 3},
    {GL_CLIPPING_INPUT_PRIMITIVES_ARB, "GL_CLIPPING_INPUT_PRIMITIVES_ARB", 1},
}};

} // namespace

int main()
{
    uml::gpu::HostGlProvider provider;
    const auto current = currentContext(provider, 4, 6);
    if (current.context == nullptr)
        return skip(current.failure);
    if (epoxy_gl_version() < 46 && !epoxy_has_gl_extension("GL_ARB_pipeline_statistics_query"))
        return skip("no ARB_pipeline_statistics_query");

    const auto target = colourTarget(64, 64);
    if (!target.complete)
        return skip("host framebuffer incomplete");
    glViewport(0, 0, 64, 64);

    const GLuint vertex = compileShader(GL_VERTEX_SHADER, vertexSource);
    const GLuint fragment = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
    const GLuint program = linkProgram({vertex, fragment});
    glUseProgram(program);

    constexpr std::array<float, 6> positions {-0.5F, -0.5F, 0.5F, -0.5F, 0.0F, 0.5F};
    GLuint array = 0;
    glGenVertexArrays(1, &array);
    glBindVertexArray(array);
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(positions), positions.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

    std::fprintf(
        stderr,
        "Host statistics availability: renderer=\"%s\"\n",
        reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    // A poll that never sees the query available is a failure of its own, not
    // a reason to wait forever.
    constexpr auto pollLimit = std::chrono::seconds(60);
    int failures = 0;
    for (const Statistic& statistic : statistics) {
        // The first query of a target compiles what it needs, which under a
        // sanitizer takes longer than any poll should; wait it out untimed.
        GLuint warmQuery = 0;
        glGenQueries(1, &warmQuery);
        glBeginQuery(statistic.target, warmQuery);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glEndQuery(statistic.target);
        GLuint warm = 0;
        glGetQueryObjectuiv(warmQuery, GL_QUERY_RESULT, &warm);
        glDeleteQueries(1, &warmQuery);

        int early = 0;
        int unavailable = 0;
        GLuint firstBad = 0;
        for (int iteration = 0; iteration < iterations; ++iteration) {
            GLuint query = 0;
            glGenQueries(1, &query);
            glBeginQuery(statistic.target, query);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glEndQuery(statistic.target);

            const auto deadline = std::chrono::steady_clock::now() + pollLimit;
            GLuint available = GL_FALSE;
            do {
                glGetQueryObjectuiv(query, GL_QUERY_RESULT_AVAILABLE, &available);
            } while (available == GL_FALSE && std::chrono::steady_clock::now() < deadline);
            if (available == GL_FALSE) {
                ++unavailable;
            } else {
                GLuint counted = 0;
                glGetQueryObjectuiv(query, GL_QUERY_RESULT, &counted);
                if (counted != statistic.expected) {
                    if (early == 0)
                        firstBad = counted;
                    ++early;
                }
            }
            glDeleteQueries(1, &query);
        }
        std::fprintf(
            stderr,
            "Host statistics availability: %s polled %d times -> %d read "
            "before the count, %d never available (expected %u",
            statistic.name,
            iterations,
            early,
            unavailable,
            statistic.expected);
        if (early != 0)
            std::fprintf(stderr, ", first read %u", firstBad);
        std::fputs(")\n", stderr);
        failures += early + unavailable;
    }

    glDeleteBuffers(1, &buffer);
    glDeleteVertexArrays(1, &array);
    glDeleteProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    glDeleteFramebuffers(1, &target.framebuffer);
    glDeleteTextures(1, &target.texture);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
