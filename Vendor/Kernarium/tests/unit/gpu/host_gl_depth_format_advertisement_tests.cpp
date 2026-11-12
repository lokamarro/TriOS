// SPDX-License-Identifier: GPL-2.0-or-later

// A sized depth internal format is a request, not a promise: a host that has
// no such storage substitutes another one and reports success. Virglrenderer
// probes those formats with glTexImage2D and advertises whatever did not
// error, so a substitution reaches the guest as a capability.
//
// The guest cannot see the substitution, and it does not merely use the
// advertised list for textures: Mesa builds its EGL/GLX config list out of the
// depth formats the driver claims, so an advertised depth format becomes the
// storage of every default framebuffer with that many depth bits. The guest
// then names that storage with the format it was promised.
//
// For a GL_DEPTH_COMPONENT32 host substitution that is guest-visible damage.
// OpenGL ES has no unsigned-normalized 32-bit depth format, so an ES context
// whose default framebuffer is PIPE_FORMAT_Z32_UNORM cannot name its own depth
// buffer: every blit out of it is rejected, because Mesa's blit validation
// compares the datatypes of the two depth attachments and the only 32-bit
// depth format ES can name on the other side is GL_DEPTH_COMPONENT32F. That is
// exactly what
// KHR-GLES3.framebuffer_blit.multisampled_to_singlesampled_blit_depth_config_test
// hit on every 32-bit-depth EGL config.
//
// The contract under test is therefore the advertisement itself: a depth
// format virglrenderer offers the guest must be stored by the host with the
// datatype and depth width that format names. Dropping an inexact one is not a
// loss -- the host storage does not change, only the guest's belief about it,
// and the guest falls back to the format the host really has.

#include "host_gl_test_support.hpp"
#include "protocol.hpp"
#include "virgl_renderer.hpp"

#include <epoxy/gl.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <span>
#include <string_view>
#include <vector>

namespace {

using uml::gpu::testing::currentContext;
using uml::gpu::testing::skip;
using uml::gpu::testing::skipWithoutRenderer;

int failureCount = 0;

void check(bool condition, std::string_view description)
{
    if (condition)
        return;
    std::fprintf(
        stderr,
        "Host depth format advertisement: %.*s\n",
        static_cast<int>(description.size()),
        description.data());
    ++failureCount;
}

// virgl_hw.h format identifiers. The guest indexes the capset bitmasks with
// these, so they are ABI and safe to name here.
enum : std::uint32_t {
    virglFormatZ16Unorm = 16,
    virglFormatZ32Unorm = 17,
    virglFormatZ32Float = 18,
    virglFormatZ24X8Unorm = 21,
};

// caps_v1 is { uint32_t max_version; format_mask sampler; ... }, and each mask
// is 16 uint32_t words indexed by format identifier.
constexpr std::size_t samplerMaskOffset = 4;
constexpr std::size_t maskWords = 16;

[[nodiscard]] bool advertisesFormat(
    std::span<const std::uint8_t> capset,
    std::uint32_t format)
{
    const std::size_t word = format / 32U;
    if (word >= maskWords)
        return false;
    const std::size_t offset = samplerMaskOffset + word * sizeof(std::uint32_t);
    if (offset + sizeof(std::uint32_t) > capset.size())
        return false;
    std::uint32_t value = 0;
    for (std::size_t byte = 0; byte < sizeof(value); ++byte)
        value |= static_cast<std::uint32_t>(capset[offset + byte]) << (8U * byte);
    return ((value >> (format % 32U)) & 1U) != 0U;
}

struct DepthFormat {
    std::uint32_t virglFormat;
    const char* name;
    // The host internal format virglrenderer probes this virgl format with,
    // from vrend_formats.c.
    GLenum hostInternalFormat;
    GLenum hostFormat;
    GLenum hostType;
    GLint depthBits;
    // GL_UNSIGNED_NORMALIZED or GL_FLOAT, as the guest's Mesa reports it for
    // the mesa_format this virgl format maps to.
    GLint depthType;
};

constexpr std::array depthFormats {
    DepthFormat {virglFormatZ16Unorm, "Z16_UNORM", GL_DEPTH_COMPONENT16,
        GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 16, GL_UNSIGNED_NORMALIZED},
    DepthFormat {virglFormatZ24X8Unorm, "Z24X8_UNORM", GL_DEPTH_COMPONENT24,
        GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, 24, GL_UNSIGNED_NORMALIZED},
    DepthFormat {virglFormatZ32Unorm, "Z32_UNORM", GL_DEPTH_COMPONENT32,
        GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, 32, GL_UNSIGNED_NORMALIZED},
    DepthFormat {virglFormatZ32Float, "Z32_FLOAT", GL_DEPTH_COMPONENT32F,
        GL_DEPTH_COMPONENT, GL_FLOAT, 32, GL_FLOAT},
};

struct HostStorage {
    bool accepted {};
    GLint depthBits {};
    GLint depthType {};
};

[[nodiscard]] HostStorage probeHostStorage(const DepthFormat& format)
{
    HostStorage storage;
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    while (glGetError() != GL_NO_ERROR) { }
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        static_cast<GLint>(format.hostInternalFormat),
        32,
        32,
        0,
        format.hostFormat,
        format.hostType,
        nullptr);
    if (glGetError() == GL_NO_ERROR) {
        storage.accepted = true;
        glGetTexLevelParameteriv(
            GL_TEXTURE_2D, 0, GL_TEXTURE_DEPTH_SIZE, &storage.depthBits);
        glGetTexLevelParameteriv(
            GL_TEXTURE_2D, 0, GL_TEXTURE_DEPTH_TYPE, &storage.depthType);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glDeleteTextures(1, &texture);
    while (glGetError() != GL_NO_ERROR) { }
    return storage;
}

const char* depthTypeName(GLint type)
{
    switch (type) {
    case GL_FLOAT:
        return "GL_FLOAT";
    case GL_UNSIGNED_NORMALIZED:
        return "GL_UNSIGNED_NORMALIZED";
    case GL_NONE:
        return "GL_NONE";
    default:
        return "(other)";
    }
}

} // namespace

int main()
{
    // Probe the host storage on a context of this test's own, before the
    // renderer takes over the display: virglrenderer destroys the context it
    // builds its format list on, so nothing is guaranteed current afterwards.
    // This provider is declared first so it outlives the renderer.
    uml::gpu::HostGlProvider provider;
    const auto current = currentContext(provider, 4, 6);
    if (current.context == nullptr)
        return skip(current.failure);

    std::array<HostStorage, depthFormats.size()> storages {};
    for (std::size_t index = 0; index < depthFormats.size(); ++index)
        storages[index] = probeHostStorage(depthFormats[index]);

    std::vector<std::uint8_t> caps;
    try {
        uml::gpu::VirglRenderer renderer;
        caps = renderer.capset(uml::gpu::protocol::virgl2CapsetId).data;
    } catch (const std::exception& error) {
        return skipWithoutRenderer(error);
    }

    bool advertisedAnyDepthFormat = false;
    for (std::size_t index = 0; index < depthFormats.size(); ++index) {
        const DepthFormat& format = depthFormats[index];
        const HostStorage& storage = storages[index];
        const bool advertised = advertisesFormat(caps, format.virglFormat);
        std::fprintf(
            stderr,
            "virgl %s: advertised=%d host_accepts=%d host_depth_bits=%d "
            "host_depth_type=%s\n",
            format.name,
            advertised ? 1 : 0,
            storage.accepted ? 1 : 0,
            storage.depthBits,
            depthTypeName(storage.depthType));
        if (!advertised)
            continue;
        advertisedAnyDepthFormat = true;
        check(storage.accepted,
            "an advertised depth format is not storable on the host");
        if (!storage.accepted)
            continue;
        check(storage.depthBits == format.depthBits,
            "an advertised depth format has a different host depth width, so "
            "a guest default framebuffer built from it reports a precision "
            "the host does not have");
        check(storage.depthType == format.depthType,
            "an advertised depth format has a different host datatype, so an "
            "OpenGL ES guest cannot name the depth buffer of its own default "
            "framebuffer and every blit out of it fails");
    }
    check(advertisedAnyDepthFormat,
        "the renderer advertised no depth format at all");

    if (failureCount != 0)
        return 1;
    std::fputs("Host depth format advertisement: ok\n", stderr);
    return 0;
}
