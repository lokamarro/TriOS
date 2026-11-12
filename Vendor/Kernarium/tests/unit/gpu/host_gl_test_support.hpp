// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "host_gl_provider.hpp"
#include "require.hpp"

#include <epoxy/gl.h>

extern "C" {
#include <virglrenderer.h>
}

#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <source_location>

/* Setup shared by the tests that drive the host GL stack, and the Virgl wire
 * numbers the tests that talk to the renderer send. A host without a usable
 * GL stack skips a test with the status its registration lists as
 * SKIP_RETURN_CODE; every other failure aborts at the caller's line. */
namespace uml::gpu::testing {

inline constexpr int skippedExitCode = 77;

/* Says why the host cannot run the test and returns the skipped status. */
[[nodiscard]] inline int skip(const char* reason)
{
    std::fprintf(stderr, "%s; skipping\n", reason);
    return skippedExitCode;
}

/* As skip, for a renderer that would not start on this host. */
[[nodiscard]] inline int skipWithoutRenderer(const std::exception& error)
{
    std::fprintf(
        stderr, "renderer unavailable: %s; skipping\n", error.what());
    return skippedExitCode;
}

// virgl_hw.h and p_defines.h values the guest sends on the wire.
inline constexpr std::uint32_t pipeTexture2d = 2;
inline constexpr std::uint32_t pipeTexture3d = 3;
inline constexpr std::uint32_t virglFormatB8g8r8a8Unorm = 1;
inline constexpr std::uint32_t virglFormatR8g8b8a8Unorm = 67;
inline constexpr std::uint32_t virglBindRenderTarget = 1U << 1U;
inline constexpr std::uint32_t virglBindSamplerView = 1U << 3U;

/* The parameters of a version 2 GL context request. */
[[nodiscard]] inline virgl_renderer_gl_ctx_param glContextParameters(
    int major,
    int minor,
    bool shared = false,
    int compat = 0) noexcept
{
    return {
        .version = 2,
        .shared = shared,
        .major_ver = major,
        .minor_ver = minor,
        .compat_ctx = compat,
    };
}

/* A desktop GL context made current on this thread. `context` is null, and
 * `failure` says which step the host failed, when there is none. */
struct CurrentContext {
    void* context {};
    const char* failure {};
};

[[nodiscard]] inline CurrentContext currentContext(
    HostGlProvider& provider,
    int major,
    int minor,
    bool compat = false)
{
    if (provider.display() == nullptr)
        return {nullptr, "no host GL display"};
    void* const context = provider.createContext(
        glContextParameters(major, minor, false, compat ? 1 : 0));
    if (context == nullptr)
        return {nullptr, "no host GL context"};
    if (provider.makeCurrent(context) != 0)
        return {nullptr, "host GL context would not bind"};
    return {context, nullptr};
}

/* As currentContext, for a test whose host must have the context. */
[[nodiscard]] inline void* requireCurrentContext(
    HostGlProvider& provider,
    int major,
    int minor,
    const std::source_location location = std::source_location::current())
{
    const auto current = currentContext(provider, major, minor);
    if (current.context == nullptr)
        ::uml::testing::fail(current.failure, location);
    return current.context;
}

/* Compiles one shader stage; an error prints the driver's log. */
[[nodiscard]] inline GLuint compileShader(
    GLenum stage,
    const char* source,
    const std::source_location location = std::source_location::current())
{
    const GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_FALSE) {
        std::array<char, 4096> log {};
        glGetShaderInfoLog(
            shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
        std::fprintf(stderr, "shader: %s\n", log.data());
        ::uml::testing::fail("a shader did not compile", location);
    }
    return shader;
}

/* Links the shaders into a program; an error prints the driver's log. */
[[nodiscard]] inline GLuint linkProgram(
    std::initializer_list<GLuint> shaders,
    const std::source_location location = std::source_location::current())
{
    const GLuint program = glCreateProgram();
    for (const GLuint shader : shaders)
        glAttachShader(program, shader);
    glLinkProgram(program);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        std::array<char, 4096> log {};
        glGetProgramInfoLog(
            program, static_cast<GLsizei>(log.size()), nullptr, log.data());
        std::fprintf(stderr, "program: %s\n", log.data());
        ::uml::testing::fail("a program did not link", location);
    }
    return program;
}

enum class TextureStorage {
    mutableLevels,
    immutable,
};

/* An RGBA8 colour texture bound to a new framebuffer, which is left bound.
 * `complete` is the framebuffer's completeness; a host that cannot render to
 * it skips or fails the test, as that test decides. */
struct ColourTarget {
    GLuint texture {};
    GLuint framebuffer {};
    bool complete {};
};

[[nodiscard]] inline ColourTarget colourTarget(
    GLsizei width,
    GLsizei height,
    TextureStorage storage = TextureStorage::mutableLevels)
{
    ColourTarget target;
    glGenTextures(1, &target.texture);
    glBindTexture(GL_TEXTURE_2D, target.texture);
    if (storage == TextureStorage::immutable)
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
    else
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA8,
            width,
            height,
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            nullptr);
    glGenFramebuffers(1, &target.framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0);
    target.complete =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    return target;
}

} // namespace uml::gpu::testing
