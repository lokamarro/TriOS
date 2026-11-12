// SPDX-License-Identifier: GPL-2.0-or-later

// The host GL provider must be able to alias memory this process already owns
// as a sampled 2D texture, so a guest's shared-memory scanout can be read
// without copying it every frame. EGL_MESA_image_host_pointer is that entry
// point; the import has to be exact or refused, never silently copied. The
// caller states how many bytes it owns there, and an image that would reach
// past them is refused: the layout comes from a guest.

#include "host_gl_test_support.hpp"
#include "require.hpp"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <sys/mman.h>

#ifndef EGL_HOST_POINTER_MESA
#define EGL_HOST_POINTER_MESA 0x3293
#define EGL_HOST_POINTER_PITCH_MESA 0x3294
#define EGL_HOST_POINTER_OFFSET_MESA 0x3295
#endif
#ifndef EGL_HOST_POINTER_SIZE_MESA
#define EGL_HOST_POINTER_SIZE_MESA 0x3296
#endif

namespace {

using uml::gpu::testing::colourTarget;
using uml::gpu::testing::compileShader;
using uml::gpu::testing::linkProgram;
using uml::gpu::testing::requireCurrentContext;
using uml::gpu::testing::TextureStorage;
using uml::testing::require;

// 'AB24': one plane, red in the first byte, matching PIPE_FORMAT_R8G8B8A8_UNORM.
constexpr std::int32_t kFourccAbgr8888 = 0x34324241;
constexpr std::uint32_t kWidth = 256;
constexpr std::uint32_t kHeight = 128;
constexpr std::size_t kHostPageSize = 16384;

class HostPages final {
public:
    HostPages(std::size_t pitch, std::uint32_t height)
        : pitch_ {pitch}
        , bytes_ {(pitch * height + kHostPageSize - 1) & ~(kHostPageSize - 1)}
    {
        base_ = static_cast<std::uint8_t*>(mmap(
            nullptr,
            bytes_,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANON,
            -1,
            0));
        require(base_ != MAP_FAILED);
    }
    ~HostPages() { munmap(base_, bytes_); }
    HostPages(const HostPages&) = delete;
    HostPages& operator=(const HostPages&) = delete;

    [[nodiscard]] std::uint8_t* base() const noexcept { return base_; }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_; }
    [[nodiscard]] std::uint32_t& texel(std::uint32_t y, std::uint32_t x) const
    {
        return *reinterpret_cast<std::uint32_t*>(base_ + y * pitch_ + x * 4);
    }

private:
    std::size_t pitch_;
    std::size_t bytes_;
    std::uint8_t* base_ {};
};

void hostPointerImageAliasesTheCallersPages()
{
    uml::gpu::HostGlProvider provider;
    require(provider.display() != nullptr);
    auto* const display = static_cast<EGLDisplay>(provider.display());

    const char* const extensions = eglQueryString(display, EGL_EXTENSIONS);
    require(extensions != nullptr);
    require(std::strstr(extensions, "EGL_MESA_image_host_pointer") != nullptr);

    void* const context = requireCurrentContext(provider, 4, 6);

    const std::size_t pitch = static_cast<std::size_t>(kWidth) * 4;
    const HostPages pages {pitch, kHeight};
    for (std::uint32_t y = 0; y < kHeight; ++y)
        for (std::uint32_t x = 0; x < kWidth; ++x)
            pages.texel(y, x) = 0xff000000u | ((x & 0xffu) << 8) | (y & 0xffu);

    const EGLint attributes[] = {
        EGL_WIDTH,
        static_cast<EGLint>(kWidth),
        EGL_HEIGHT,
        static_cast<EGLint>(kHeight),
        EGL_LINUX_DRM_FOURCC_EXT,
        kFourccAbgr8888,
        EGL_HOST_POINTER_PITCH_MESA,
        static_cast<EGLint>(pitch),
        EGL_HOST_POINTER_SIZE_MESA,
        static_cast<EGLint>(pages.size()),
        EGL_NONE};
    const EGLImageKHR image = eglCreateImageKHR(
        display,
        EGL_NO_CONTEXT,
        EGL_HOST_POINTER_MESA,
        static_cast<EGLClientBuffer>(pages.base()),
        attributes);
    require(image != EGL_NO_IMAGE_KHR);

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    require(glGetError() == GL_NO_ERROR);

    const auto target =
        colourTarget(kWidth, kHeight, TextureStorage::immutable);
    require(target.complete);

    const GLuint program = linkProgram({
        compileShader(
            GL_VERTEX_SHADER,
            "#version 460 core\n"
            "void main(void) {\n"
            "   vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
            "   gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
            "}\n"),
        compileShader(
            GL_FRAGMENT_SHADER,
            "#version 460 core\n"
            "uniform sampler2D src;\n"
            "out vec4 colour;\n"
            "void main(void) {\n"
            "   colour = texelFetch(src, ivec2(gl_FragCoord.xy), 0);\n"
            "}\n")});
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "src"), 0);

    GLuint array = 0;
    glGenVertexArrays(1, &array);
    glBindVertexArray(array);
    glViewport(0, 0, kWidth, kHeight);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);

    std::vector<std::uint32_t> read(
        static_cast<std::size_t>(kWidth) * kHeight, 0u);
    for (int pass = 0; pass < 2; ++pass) {
        // The second pass writes the pages through the CPU and makes no GL
        // call that could upload them: an aliased texture must show the new
        // bytes, a copied one would not.
        if (pass == 1)
            for (std::uint32_t y = 0; y < kHeight; ++y)
                for (std::uint32_t x = 0; x < kWidth; ++x)
                    pages.texel(y, x) ^= 0x00ffffffu;

        glClearColor(1.0F, 0.0F, 1.0F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFinish();
        glReadPixels(
            0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, read.data());
        require(glGetError() == GL_NO_ERROR);

        for (std::uint32_t y = 0; y < kHeight; ++y)
            for (std::uint32_t x = 0; x < kWidth; ++x)
                require(read[y * kWidth + x] == pages.texel(y, x));
    }

    // An import the driver cannot honour exactly has to be refused, so the
    // caller can choose its own fallback instead of paying a hidden copy.
    EGLint wrongPitch[] = {
        EGL_WIDTH,
        static_cast<EGLint>(kWidth),
        EGL_HEIGHT,
        static_cast<EGLint>(kHeight),
        EGL_LINUX_DRM_FOURCC_EXT,
        kFourccAbgr8888,
        EGL_HOST_POINTER_PITCH_MESA,
        static_cast<EGLint>(pitch + 4),
        EGL_HOST_POINTER_SIZE_MESA,
        static_cast<EGLint>(pages.size()),
        EGL_NONE};
    require(
        eglCreateImageKHR(
            display,
            EGL_NO_CONTEXT,
            EGL_HOST_POINTER_MESA,
            static_cast<EGLClientBuffer>(pages.base()),
            wrongPitch)
        == EGL_NO_IMAGE_KHR);
    require(
        eglCreateImageKHR(
            display,
            EGL_NO_CONTEXT,
            EGL_HOST_POINTER_MESA,
            static_cast<EGLClientBuffer>(pages.base() + 4),
            attributes)
        == EGL_NO_IMAGE_KHR);

    // The caller's bytes bound the image: one that needs more than the caller
    // owns, or whose size the caller never stated, is refused rather than
    // laid over whatever memory follows.
    const auto createWithin = [&](std::size_t size, std::size_t offset) {
        const EGLint bounded[] = {
            EGL_WIDTH,
            static_cast<EGLint>(kWidth),
            EGL_HEIGHT,
            static_cast<EGLint>(kHeight),
            EGL_LINUX_DRM_FOURCC_EXT,
            kFourccAbgr8888,
            EGL_HOST_POINTER_PITCH_MESA,
            static_cast<EGLint>(pitch),
            EGL_HOST_POINTER_OFFSET_MESA,
            static_cast<EGLint>(offset),
            EGL_HOST_POINTER_SIZE_MESA,
            static_cast<EGLint>(size),
            EGL_NONE};
        return eglCreateImageKHR(
            display,
            EGL_NO_CONTEXT,
            EGL_HOST_POINTER_MESA,
            static_cast<EGLClientBuffer>(pages.base()),
            bounded);
    };
    require(createWithin(pitch * kHeight - pitch, 0) == EGL_NO_IMAGE_KHR);
    require(eglGetError() == EGL_BAD_PARAMETER);
    require(createWithin(pages.size(), kHostPageSize) == EGL_NO_IMAGE_KHR);
    require(eglGetError() == EGL_BAD_PARAMETER);
    const EGLint unbounded[] = {
        EGL_WIDTH,
        static_cast<EGLint>(kWidth),
        EGL_HEIGHT,
        static_cast<EGLint>(kHeight),
        EGL_LINUX_DRM_FOURCC_EXT,
        kFourccAbgr8888,
        EGL_HOST_POINTER_PITCH_MESA,
        static_cast<EGLint>(pitch),
        EGL_NONE};
    require(
        eglCreateImageKHR(
            display,
            EGL_NO_CONTEXT,
            EGL_HOST_POINTER_MESA,
            static_cast<EGLClientBuffer>(pages.base()),
            unbounded)
        == EGL_NO_IMAGE_KHR);
    require(eglGetError() == EGL_BAD_PARAMETER);

    glDeleteProgram(program);
    glDeleteVertexArrays(1, &array);
    glDeleteFramebuffers(1, &target.framebuffer);
    glDeleteTextures(1, &target.texture);
    glDeleteTextures(1, &texture);
    require(eglDestroyImageKHR(display, image) == EGL_TRUE);
    require(provider.makeCurrent(nullptr) == 0);
    provider.destroyContext(context);
}

} // namespace

int main()
{
    hostPointerImageAliasesTheCallersPages();
    std::printf(
        "PASS host-pointer image: %ux%u aliased, sampled and re-read after a "
        "CPU write, with wrong pitch, misalignment, and an image past or "
        "without its stated size refused\n",
        kWidth,
        kHeight);
    return 0;
}
