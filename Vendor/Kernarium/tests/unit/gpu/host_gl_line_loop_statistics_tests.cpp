// SPDX-License-Identifier: GPL-2.0-or-later

// GL_VERTICES_SUBMITTED_ARB counts the vertices an application submitted, not
// the vertices the driver went on to assemble. A GL_LINE_LOOP over N vertices
// is N segments, so a driver that closes the loop by decomposing it into N
// independent lines hands the rasterizer 2N vertices. If the statistic is read
// after that decomposition it reports 2N, which is what
// KHR-GL46.pipeline_statistics_query_tests_ARB.functional_primitives_vertices_
// submitted_and_clipping_input_output_primitives checks and rejects.
//
// This matters here because the host stack decomposes: Zink does not advertise
// MESA_PRIM_LINE_LOOP (zink_screen.c clears the bit unconditionally), so Mesa
// converts every line loop through u_primconvert before the driver sees it,
// while virglrenderer does advertise PIPE_PRIM_LINE_LOOP to the guest and so
// passes guest line loops through untouched. Mesa flags the conversion with
// pipe_draw_info::was_line_loop precisely so a driver can subtract it back out
// of the statistic.
//
// The contract under test is that contract, on the host GL stack alone, with
// no guest and no virtio transport: a line loop of N vertices must report N
// submitted vertices. The spec permits N+1 -- an implementation may count the
// vertex that closes the loop -- so both are accepted, and only the doubled
// count is a failure.

#include "host_gl_test_support.hpp"

#include <epoxy/gl.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace {

using uml::gpu::testing::colourTarget;
using uml::gpu::testing::compileShader;
using uml::gpu::testing::currentContext;
using uml::gpu::testing::linkProgram;
using uml::gpu::testing::skip;

int failureCount = 0;

void check(bool condition, std::string_view description)
{
    if (condition)
        return;
    std::fprintf(
        stderr,
        "Host line-loop statistics: %.*s\n",
        static_cast<int>(description.size()),
        description.data());
    ++failureCount;
}

constexpr GLsizei loopVertexCount = 7;

constexpr const char* vertexSource =
    "#version 330 core\n"
    "layout(location = 0) in vec2 position;\n"
    "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n";

constexpr const char* fragmentSource =
    "#version 330 core\n"
    "out vec4 colour;\n"
    "void main() { colour = vec4(1.0); }\n";

} // namespace

int main()
{
    uml::gpu::HostGlProvider provider;
    // virglrenderer builds its contexts either way depending on
    // use_core_profile, and the statistic path may differ between them, so let
    // the probe cover both.
    const char* compat = std::getenv("UML_LINE_LOOP_COMPAT_CTX");
    const auto current =
        currentContext(provider, 4, 6, compat != nullptr && compat[0] == '1');
    if (current.context == nullptr)
        return skip(current.failure);

    // The statistic is an ARB_pipeline_statistics_query counter; without it
    // there is nothing to assert against.
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

    // A regular heptagon, so every vertex is distinct and inside the viewport.
    std::array<float, loopVertexCount * 2> positions {};
    for (int index = 0; index < loopVertexCount; ++index) {
        const double angle = 6.283185307179586 * index / loopVertexCount;
        positions[static_cast<std::size_t>(index) * 2] =
            static_cast<float>(0.5 * std::cos(angle));
        positions[static_cast<std::size_t>(index) * 2 + 1] =
            static_cast<float>(0.5 * std::sin(angle));
    }
    GLuint array = 0;
    glGenVertexArrays(1, &array);
    glBindVertexArray(array);
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(
        GL_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(positions.size() * sizeof(float)),
        positions.data(),
        GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

    while (glGetError() != GL_NO_ERROR) { }

    GLuint query = 0;
    glGenQueries(1, &query);
    glBeginQuery(GL_VERTICES_SUBMITTED_ARB, query);
    glDrawArrays(GL_LINE_LOOP, 0, loopVertexCount);
    glEndQuery(GL_VERTICES_SUBMITTED_ARB);
    glFinish();

    check(glGetError() == GL_NO_ERROR, "the line-loop draw raised a GL error");

    GLuint submitted = 0;
    glGetQueryObjectuiv(query, GL_QUERY_RESULT, &submitted);

    // Say what was measured and on what, so a pass is evidence the path ran
    // rather than evidence the driver never converted anything.
    std::fprintf(
        stderr,
        "Host line-loop statistics: renderer=\"%s\" version=\"%s\"\n",
        reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
        reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    std::fprintf(
        stderr,
        "Host line-loop statistics: GL_LINE_LOOP n=%d -> "
        "GL_VERTICES_SUBMITTED_ARB=%u\n",
        loopVertexCount,
        submitted);

    // Independent lines have no closing vertex, so the line-loop compensation
    // must leave them alone: the count is exactly the vertices submitted.
    GLuint linesQuery = 0;
    glGenQueries(1, &linesQuery);
    glBeginQuery(GL_VERTICES_SUBMITTED_ARB, linesQuery);
    glDrawArrays(GL_LINES, 0, loopVertexCount - 1);
    glEndQuery(GL_VERTICES_SUBMITTED_ARB);
    glFinish();
    GLuint lines = 0;
    glGetQueryObjectuiv(linesQuery, GL_QUERY_RESULT, &lines);
    std::fprintf(
        stderr,
        "Host line-loop statistics: GL_LINES n=%d -> "
        "GL_VERTICES_SUBMITTED_ARB=%u (control)\n",
        loopVertexCount - 1,
        lines);
    check(lines == static_cast<GLuint>(loopVertexCount - 1),
        "GL_LINES did not count exactly the vertices submitted");
    glDeleteQueries(1, &linesQuery);

    // A line loop of N vertices is N segments, so the clipper sees N
    // primitives however the driver closes the loop. Count them alone first,
    // so the paired measurement below has a control.
    GLuint clippingQuery = 0;
    glGenQueries(1, &clippingQuery);
    glBeginQuery(GL_CLIPPING_INPUT_PRIMITIVES_ARB, clippingQuery);
    glDrawArrays(GL_LINE_LOOP, 0, loopVertexCount);
    glEndQuery(GL_CLIPPING_INPUT_PRIMITIVES_ARB);
    glFinish();
    GLuint clippedAlone = 0;
    glGetQueryObjectuiv(clippingQuery, GL_QUERY_RESULT, &clippedAlone);
    std::fprintf(
        stderr,
        "Host line-loop statistics: GL_LINE_LOOP n=%d -> "
        "GL_CLIPPING_INPUT_PRIMITIVES_ARB=%u (control)\n",
        loopVertexCount,
        clippedAlone);
    check(clippedAlone == static_cast<GLuint>(loopVertexCount),
        "GL_LINE_LOOP did not reach the clipper as one primitive per vertex");
    glDeleteQueries(1, &clippingQuery);

    // The CTS case queries clipping primitives alongside the submitted
    // vertices, and Zink splits a statistics query's starts when the line-loop
    // flag changes. Run the same line loop with a second statistics query
    // active, which is the shape the failing case actually uses. Zink begins
    // a Vulkan query per GL query, so the driver sees two statistics queries
    // active at once; each must still count what it counts alone.
    GLuint pairVertices = 0;
    GLuint pairClipping = 0;
    glGenQueries(1, &pairVertices);
    glGenQueries(1, &pairClipping);
    glBeginQuery(GL_CLIPPING_INPUT_PRIMITIVES_ARB, pairClipping);
    glBeginQuery(GL_VERTICES_SUBMITTED_ARB, pairVertices);
    glDrawArrays(GL_LINE_LOOP, 0, loopVertexCount);
    glEndQuery(GL_VERTICES_SUBMITTED_ARB);
    glEndQuery(GL_CLIPPING_INPUT_PRIMITIVES_ARB);
    glFinish();
    GLuint paired = 0;
    GLuint clipped = 0;
    glGetQueryObjectuiv(pairVertices, GL_QUERY_RESULT, &paired);
    glGetQueryObjectuiv(pairClipping, GL_QUERY_RESULT, &clipped);
    std::fprintf(
        stderr,
        "Host line-loop statistics: with a clipping query active, "
        "GL_LINE_LOOP n=%d -> GL_VERTICES_SUBMITTED_ARB=%u "
        "(clipping input primitives=%u)\n",
        loopVertexCount,
        paired,
        clipped);
    check(paired == static_cast<GLuint>(loopVertexCount)
            || paired == static_cast<GLuint>(loopVertexCount) + 1U,
        "with a clipping query active, GL_LINE_LOOP did not count the vertices submitted");
    check(clipped == static_cast<GLuint>(loopVertexCount),
        "with a vertices query active, GL_LINE_LOOP did not count its clipping input primitives");
    glDeleteQueries(1, &pairVertices);
    glDeleteQueries(1, &pairClipping);

    // The CTS case reports the entry point with every mismatch, and it fails on
    // six of them while passing on the plain ones. So the entry point, not the
    // primitive mode alone, decides whether the line-loop compensation applies.
    // Measure each entry point the case names, with the plain draws as controls.
    std::array<std::uint32_t, loopVertexCount> indices {};
    for (std::uint32_t index = 0;
         index < static_cast<std::uint32_t>(loopVertexCount);
         ++index)
        indices[index] = index;
    GLuint indexBuffer = 0;
    glGenBuffers(1, &indexBuffer);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indexBuffer);
    glBufferData(
        GL_ELEMENT_ARRAY_BUFFER,
        static_cast<GLsizeiptr>(indices.size() * sizeof(std::uint32_t)),
        indices.data(),
        GL_STATIC_DRAW);

    constexpr GLsizei instanceCount = 3;

    const std::array<std::uint32_t, 5> indirectCommand {
        static_cast<std::uint32_t>(loopVertexCount),
        static_cast<std::uint32_t>(instanceCount),
        0U,
        0U,
        0U,
    };
    GLuint indirectBuffer = 0;
    glGenBuffers(1, &indirectBuffer);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, indirectBuffer);
    glBufferData(
        GL_DRAW_INDIRECT_BUFFER,
        static_cast<GLsizeiptr>(indirectCommand.size() * sizeof(std::uint32_t)),
        indirectCommand.data(),
        GL_STATIC_DRAW);

    while (glGetError() != GL_NO_ERROR) { }

    const auto measure = [](const char* entryPoint, GLsizei instances, auto&& draw) {
        GLuint entryQuery = 0;
        glGenQueries(1, &entryQuery);
        glBeginQuery(GL_VERTICES_SUBMITTED_ARB, entryQuery);
        draw();
        glEndQuery(GL_VERTICES_SUBMITTED_ARB);
        glFinish();
        const GLenum error = glGetError();
        GLuint counted = 0;
        glGetQueryObjectuiv(entryQuery, GL_QUERY_RESULT, &counted);
        glDeleteQueries(1, &entryQuery);

        const GLuint expected = static_cast<GLuint>(loopVertexCount)
            * static_cast<GLuint>(instances);
        const GLuint tolerated = expected + static_cast<GLuint>(instances);
        const bool ok = counted == expected || counted == tolerated;
        std::fprintf(
            stderr,
            "Host line-loop statistics: %-36s instances=%d -> %3u "
            "(expected %u or %u) %s%s\n",
            entryPoint,
            instances,
            counted,
            expected,
            tolerated,
            ok ? "ok" : "MISMATCH",
            counted == expected * 2U ? " EXACTLY DOUBLE" : "");
        if (error != GL_NO_ERROR)
            std::fprintf(
                stderr,
                "Host line-loop statistics: %s raised GL error 0x%04x\n",
                entryPoint,
                error);
        if (!ok)
            ++failureCount;
    };

    const int glVersion = epoxy_gl_version();

    measure("glDrawArrays (control)", 1, [] {
        glDrawArrays(GL_LINE_LOOP, 0, loopVertexCount);
    });
    measure("glDrawArraysInstanced (control)", instanceCount, [] {
        glDrawArraysInstanced(GL_LINE_LOOP, 0, loopVertexCount, instanceCount);
    });
    measure("glDrawElements (control)", 1, [] {
        glDrawElements(GL_LINE_LOOP, loopVertexCount, GL_UNSIGNED_INT, nullptr);
    });
    measure("glDrawRangeElements", 1, [] {
        glDrawRangeElements(
            GL_LINE_LOOP,
            0,
            static_cast<GLuint>(loopVertexCount) - 1U,
            loopVertexCount,
            GL_UNSIGNED_INT,
            nullptr);
    });
    measure("glDrawRangeElementsBaseVertex", 1, [] {
        glDrawRangeElementsBaseVertex(
            GL_LINE_LOOP,
            0,
            static_cast<GLuint>(loopVertexCount) - 1U,
            loopVertexCount,
            GL_UNSIGNED_INT,
            nullptr,
            0);
    });
    if (glVersion >= 42) {
        measure("glDrawArraysInstancedBaseInstance", instanceCount, [] {
            glDrawArraysInstancedBaseInstance(
                GL_LINE_LOOP, 0, loopVertexCount, instanceCount, 0);
        });
        measure("glDrawElementsInstancedBaseInstance", instanceCount, [] {
            glDrawElementsInstancedBaseInstance(
                GL_LINE_LOOP,
                loopVertexCount,
                GL_UNSIGNED_INT,
                nullptr,
                instanceCount,
                0);
        });
    } else {
        std::fprintf(
            stderr,
            "Host line-loop statistics: no base-instance draws, GL %d.%d\n",
            glVersion / 10,
            glVersion % 10);
    }
    if (glVersion >= 40) {
        measure("glDrawElementsIndirect", instanceCount, [] {
            glDrawElementsIndirect(GL_LINE_LOOP, GL_UNSIGNED_INT, nullptr);
        });
    }

    // virglrenderer picks its GL entry point from the virgl draw info, and a
    // Gallium draw always carries instance_count >= 1, so a plain guest draw
    // still reaches an *Instanced* entry point here. Cover every call in
    // vrend_renderer.c's dispatch, so "the host is correct" means correct for
    // everything the host can actually be asked to do.
    measure("glDrawElementsInstancedARB", instanceCount, [] {
        glDrawElementsInstancedARB(
            GL_LINE_LOOP, loopVertexCount, GL_UNSIGNED_INT, nullptr, instanceCount);
    });
    measure("glDrawElementsBaseVertex", 1, [] {
        glDrawElementsBaseVertex(
            GL_LINE_LOOP, loopVertexCount, GL_UNSIGNED_INT, nullptr, 0);
    });
    measure("glDrawElementsInstancedBaseVertex", instanceCount, [] {
        glDrawElementsInstancedBaseVertex(
            GL_LINE_LOOP,
            loopVertexCount,
            GL_UNSIGNED_INT,
            nullptr,
            instanceCount,
            0);
    });
    if (glVersion >= 42) {
        measure("glDrawElementsInstancedBaseVertexBaseInstance", instanceCount, [] {
            glDrawElementsInstancedBaseVertexBaseInstance(
                GL_LINE_LOOP,
                loopVertexCount,
                GL_UNSIGNED_INT,
                nullptr,
                instanceCount,
                0,
                0);
        });
    }

    // glDrawArraysIndirect takes a four-word command, not the five-word
    // elements one, so it needs its own buffer.
    const std::array<std::uint32_t, 4> arraysIndirectCommand {
        static_cast<std::uint32_t>(loopVertexCount),
        static_cast<std::uint32_t>(instanceCount),
        0U,
        0U,
    };
    GLuint arraysIndirectBuffer = 0;
    glGenBuffers(1, &arraysIndirectBuffer);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, arraysIndirectBuffer);
    glBufferData(
        GL_DRAW_INDIRECT_BUFFER,
        static_cast<GLsizeiptr>(arraysIndirectCommand.size() * sizeof(std::uint32_t)),
        arraysIndirectCommand.data(),
        GL_STATIC_DRAW);
    measure("glDrawArraysIndirect", instanceCount, [] {
        glDrawArraysIndirect(GL_LINE_LOOP, nullptr);
    });

    // Two back-to-back commands, so the multi-draw entry points take their
    // draw_count > 1 branch the way vrend does.
    const std::array<std::uint32_t, 8> multiArraysCommands {
        static_cast<std::uint32_t>(loopVertexCount),
        static_cast<std::uint32_t>(instanceCount), 0U, 0U,
        static_cast<std::uint32_t>(loopVertexCount),
        static_cast<std::uint32_t>(instanceCount), 0U, 0U,
    };
    glBufferData(
        GL_DRAW_INDIRECT_BUFFER,
        static_cast<GLsizeiptr>(multiArraysCommands.size() * sizeof(std::uint32_t)),
        multiArraysCommands.data(),
        GL_STATIC_DRAW);
    measure("glMultiDrawArraysIndirect", instanceCount * 2, [] {
        glMultiDrawArraysIndirect(GL_LINE_LOOP, nullptr, 2, 0);
    });

    const std::array<std::uint32_t, 10> multiElementsCommands {
        static_cast<std::uint32_t>(loopVertexCount),
        static_cast<std::uint32_t>(instanceCount), 0U, 0U, 0U,
        static_cast<std::uint32_t>(loopVertexCount),
        static_cast<std::uint32_t>(instanceCount), 0U, 0U, 0U,
    };
    glBufferData(
        GL_DRAW_INDIRECT_BUFFER,
        static_cast<GLsizeiptr>(multiElementsCommands.size() * sizeof(std::uint32_t)),
        multiElementsCommands.data(),
        GL_STATIC_DRAW);
    measure("glMultiDrawElementsIndirect", instanceCount * 2, [] {
        glMultiDrawElementsIndirect(GL_LINE_LOOP, GL_UNSIGNED_INT, nullptr, 2, 0);
    });

    glDeleteBuffers(1, &arraysIndirectBuffer);

    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    glDeleteBuffers(1, &indirectBuffer);

    // Everything above measured one draw per query with a glFinish after it and
    // never changed primitive type in between. The CTS does none of that: it
    // runs 576 queries back to back in one context, interleaves primitive
    // types, and synchronises only through the blocking query read. Zink
    // records the line-loop flag per query start and only resynchronises the
    // start when the flag CHANGES, so alternating primitive types is the state
    // the flag logic actually has to survive. Repeat the sweep that way.
    const auto measureInterleaved = [](const char* entryPoint, GLsizei instances, auto&& draw) {
        // A triangle draw in its own query first, so the line-loop flag has to
        // flip on every measurement rather than staying constant.
        GLuint warmQuery = 0;
        glGenQueries(1, &warmQuery);
        glBeginQuery(GL_VERTICES_SUBMITTED_ARB, warmQuery);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glEndQuery(GL_VERTICES_SUBMITTED_ARB);
        GLuint warm = 0;
        glGetQueryObjectuiv(warmQuery, GL_QUERY_RESULT, &warm);
        glDeleteQueries(1, &warmQuery);

        GLuint entryQuery = 0;
        glGenQueries(1, &entryQuery);
        glBeginQuery(GL_VERTICES_SUBMITTED_ARB, entryQuery);
        draw();
        glEndQuery(GL_VERTICES_SUBMITTED_ARB);
        // No glFinish: the blocking result read is the only sync, as in the CTS.
        GLuint counted = 0;
        glGetQueryObjectuiv(entryQuery, GL_QUERY_RESULT, &counted);
        glDeleteQueries(1, &entryQuery);

        const GLuint expected = static_cast<GLuint>(loopVertexCount)
            * static_cast<GLuint>(instances);
        const GLuint tolerated = expected + static_cast<GLuint>(instances);
        const bool ok = counted == expected || counted == tolerated;
        std::fprintf(
            stderr,
            "Host line-loop statistics: interleaved %-36s -> %3u "
            "(expected %u or %u, triangles control %u) %s%s\n",
            entryPoint,
            counted,
            expected,
            tolerated,
            warm,
            ok ? "ok" : "MISMATCH",
            counted == expected * 2U ? " EXACTLY DOUBLE" : "");
        if (!ok)
            ++failureCount;
    };

    measureInterleaved("glDrawArrays", 1, [] {
        glDrawArrays(GL_LINE_LOOP, 0, loopVertexCount);
    });
    measureInterleaved("glDrawElements", 1, [] {
        glDrawElements(GL_LINE_LOOP, loopVertexCount, GL_UNSIGNED_INT, nullptr);
    });
    measureInterleaved("glDrawRangeElements", 1, [] {
        glDrawRangeElements(
            GL_LINE_LOOP, 0, static_cast<GLuint>(loopVertexCount) - 1U,
            loopVertexCount, GL_UNSIGNED_INT, nullptr);
    });
    measureInterleaved("glDrawRangeElementsBaseVertex", 1, [] {
        glDrawRangeElementsBaseVertex(
            GL_LINE_LOOP, 0, static_cast<GLuint>(loopVertexCount) - 1U,
            loopVertexCount, GL_UNSIGNED_INT, nullptr, 0);
    });
    if (glVersion >= 42) {
        measureInterleaved("glDrawArraysInstancedBaseInstance", instanceCount, [] {
            glDrawArraysInstancedBaseInstance(
                GL_LINE_LOOP, 0, loopVertexCount, instanceCount, 0);
        });
        measureInterleaved("glDrawElementsInstancedBaseInstance", instanceCount, [] {
            glDrawElementsInstancedBaseInstance(
                GL_LINE_LOOP, loopVertexCount, GL_UNSIGNED_INT, nullptr,
                instanceCount, 0);
        });
    }


    // Every sweep above reads each query result exactly once, through
    // glGetQueryObject*v with no query buffer bound.  The CTS case does not:
    // for every one of its 576 queries it also reads the same result four more
    // times into a query buffer object, and it never calls glFinish.  Zink
    // keeps a query's Vulkan results in a list of query buffer objects and
    // splits a statistics query into several "starts" whenever the line-loop
    // flag changes, so that read pattern is the state the per-start
    // bookkeeping has to survive.  Drive it here, many times, because the case
    // fails on only about one per cent of its queries and a single-shot probe
    // will not see it.
    {
        const char* iterationsText = std::getenv("UML_LINE_LOOP_STRESS");
        long iterations = iterationsText != nullptr ? std::strtol(iterationsText, nullptr, 10) : 600;
        if (iterations < 0)
            iterations = 0;
        GLuint queryBuffer = 0;
        glGenBuffers(1, &queryBuffer);
        glBindBuffer(GL_QUERY_BUFFER, queryBuffer);
        glBufferData(GL_QUERY_BUFFER, 64, nullptr, GL_DYNAMIC_READ);
        glBindBuffer(GL_QUERY_BUFFER, 0);

        // A second render target, so the loop can move a renderpass boundary
        // around the query the way a guest that keeps changing framebuffers
        // does.  Zink only records the line-loop flag on the query start that
        // is current when the draw is seen, and a boundary makes a new start.
        GLuint scratchColour = 0;
        glGenTextures(1, &scratchColour);
        glBindTexture(GL_TEXTURE_2D, scratchColour);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        GLuint scratchFramebuffer = 0;
        glGenFramebuffers(1, &scratchFramebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, scratchFramebuffer);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scratchColour, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);

        const auto breakRenderpass = [&] {
            glBindFramebuffer(GL_FRAMEBUFFER, scratchFramebuffer);
            glClear(GL_COLOR_BUFFER_BIT);
            glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
        };

        long stressFailures = 0;
        GLuint firstBad = 0;
        long firstBadIteration = -1;
        long firstBadShape = -1;
        long shapeFailures[4] = { 0, 0, 0, 0 };
        for (long iteration = 0; iteration < iterations; ++iteration) {
            const long shape = iteration % 4;
            GLuint queries[3] = { 0, 0, 0 };
            glGenQueries(3, queries);
            // shape 1: the measured query begins while no renderpass is open,
            // which is when Zink defers the start and ctx->vertices_query is
            // still null on the draw that follows.
            if (shape == 1)
                breakRenderpass();

            // A triangle draw in its own query, so the line-loop flag has to
            // flip on entry to the measured query, as it does in the CTS.
            glBeginQuery(GL_VERTICES_SUBMITTED_ARB, queries[0]);
            glDrawElementsInstancedARB(
                GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr, 1);
            glEndQuery(GL_VERTICES_SUBMITTED_ARB);

            // The clipping counter the case queries alongside the vertices.
            glBeginQuery(GL_CLIPPING_INPUT_PRIMITIVES_ARB, queries[1]);
            glDrawElementsInstancedARB(
                GL_LINE_LOOP, loopVertexCount, GL_UNSIGNED_INT, nullptr, 1);
            glEndQuery(GL_CLIPPING_INPUT_PRIMITIVES_ARB);

            glBeginQuery(GL_VERTICES_SUBMITTED_ARB, queries[2]);
            // shape 2: a boundary between the begin and the draw; shape 3: one
            // between the draw and the end.
            if (shape == 2)
                breakRenderpass();
            glDrawElementsInstancedARB(
                GL_LINE_LOOP, loopVertexCount, GL_UNSIGNED_INT, nullptr, 1);
            if (shape == 3)
                breakRenderpass();
            glEndQuery(GL_VERTICES_SUBMITTED_ARB);

            // The four query-buffer reads the case performs before it reads
            // the value it checks.
            glBindBuffer(GL_QUERY_BUFFER, queryBuffer);
            glGetQueryObjectiv(queries[2], GL_QUERY_RESULT, nullptr);
            glGetQueryObjectuiv(
                queries[2], GL_QUERY_RESULT,
                reinterpret_cast<GLuint*>(static_cast<std::uintptr_t>(8)));
            glGetQueryObjecti64v(
                queries[2], GL_QUERY_RESULT,
                reinterpret_cast<GLint64*>(static_cast<std::uintptr_t>(16)));
            glGetQueryObjectui64v(
                queries[2], GL_QUERY_RESULT,
                reinterpret_cast<GLuint64*>(static_cast<std::uintptr_t>(24)));
            glBindBuffer(GL_QUERY_BUFFER, 0);

            GLuint counted = 0;
            glGetQueryObjectuiv(queries[2], GL_QUERY_RESULT, &counted);
            if (counted != static_cast<GLuint>(loopVertexCount)
                && counted != static_cast<GLuint>(loopVertexCount) + 1U) {
                if (stressFailures == 0) {
                    firstBad = counted;
                    firstBadIteration = iteration;
                    firstBadShape = shape;
                }
                ++stressFailures;
                ++shapeFailures[shape];
            }
            glDeleteQueries(3, queries);
        }
        glDeleteBuffers(1, &queryBuffer);
        glDeleteFramebuffers(1, &scratchFramebuffer);
        glDeleteTextures(1, &scratchColour);
        std::fprintf(
            stderr,
            "Host line-loop statistics: CTS-shaped stress %ld iterations -> "
            "%ld mismatching results (per shape %ld/%ld/%ld/%ld)",
            iterations,
            stressFailures,
            shapeFailures[0],
            shapeFailures[1],
            shapeFailures[2],
            shapeFailures[3]);
        if (stressFailures != 0)
            std::fprintf(
                stderr, " (first at iteration %ld shape %ld: %u)",
                firstBadIteration, firstBadShape, firstBad);
        std::fputc('\n', stderr);
        if (stressFailures != 0)
            ++failureCount;
    }

    glDeleteBuffers(1, &indexBuffer);

    const bool acceptable =
        submitted == static_cast<GLuint>(loopVertexCount)
        || submitted == static_cast<GLuint>(loopVertexCount) + 1U;
    if (!acceptable) {
        std::fprintf(
            stderr,
            "Host line-loop statistics: GL_VERTICES_SUBMITTED_ARB reported %u "
            "for a %d-vertex GL_LINE_LOOP; expected %d or %d%s\n",
            submitted,
            loopVertexCount,
            loopVertexCount,
            loopVertexCount + 1,
            submitted == static_cast<GLuint>(loopVertexCount) * 2U
                ? " (exactly double: the decomposed lines were counted)"
                : "");
        ++failureCount;
    }

    glDeleteQueries(1, &query);
    glDeleteProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    if (failureCount != 0)
        return EXIT_FAILURE;

    std::puts("Host line-loop statistics tests passed");
    return 0;
}
