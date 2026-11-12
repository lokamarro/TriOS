// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Locks tall RGBA hardware buffers whose rows are padded, and times the locks.
 *
 * gralloc pads a row to a multiple of 16 bytes, so an RGBA buffer whose width
 * is not a multiple of four has a stride wider than its width. The lock of such
 * a buffer used to send its rows to the renderer, and fetch them from it, one
 * row at a time, and a tall buffer took half a second to lock. Each case here
 * checks two things on a buffer 604 rows tall:
 *
 *  - Rows at the stride. A buffer the GPU rendered a pattern into through
 *    OpenGL ES is locked for reading and every pixel compared at the stride
 *    gralloc reports. A buffer the CPU wrote through a lock, and unlocked, is
 *    read back by the GPU and compared too. A row that lands at the wrong
 *    place shows as a wrong pixel. Locks of a rectangle are checked the same
 *    way.
 *  - Time. Locking for reading, which fetches the GPU's pixels, and unlocking
 *    after writing, which sends them, are timed several times, and the median
 *    of a padded width must be within four times the median of the widths
 *    whose rows are not padded (taking a millisecond as the least of those).
 *
 * Usage: kernarium_ahb_rgba_lock [width...]
 * With no argument it runs the widths 340 to 350, three of which (340, 344 and
 * 348) are not padded and so serve as the control. The exit status is 0 when
 * every case passed, 1 for a wrong pixel or a lock that took too long, and 2
 * when there is no OpenGL ES context or no unpadded width to compare with.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>

namespace {

constexpr uint32_t kHeight = 604;
constexpr uint32_t kBytesPerPixel = 4;
constexpr int kRepeats = 5;
constexpr double kMaxRatio = 4.0;
constexpr double kLeastBaselineMs = 1.0;
constexpr size_t kMaxWidths = 32;

// What the pixel at (x, y) holds for a pattern seed.
uint8_t pattern(uint32_t seed, uint32_t x, uint32_t y, uint32_t byte) {
    return static_cast<uint8_t>(seed * 29 + x * 7 + y * 13 + byte * 31 + 1);
}

double nowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1e3 + static_cast<double>(ts.tv_nsec) / 1e6;
}

double median(const double* values, size_t count) {
    double sorted[kMaxWidths];
    for (size_t i = 0; i < count; i++) {
        size_t j = i;
        for (; j > 0 && sorted[j - 1] > values[i]; j--) sorted[j] = sorted[j - 1];
        sorted[j] = values[i];
    }
    return sorted[count / 2];
}

struct Gl {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    GLuint program = 0;
    GLint seedLocation = -1;
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC getClientBuffer = nullptr;
    PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTarget = nullptr;
};

bool glOk(const char* what) {
    const GLenum error = glGetError();
    if (error == GL_NO_ERROR) return true;
    printf("    %s -> GL error 0x%x\n", what, error);
    return false;
}

// The vertex shader covers the viewport with one triangle. The fragment shader writes the pattern()
// of the pixel it shades, so the GPU renders a different value into every byte of a row.
const char* kVertexShader =
        "#version 300 es\n"
        "void main() {\n"
        "    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
        "    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
const char* kFragmentShader =
        "#version 300 es\n"
        "precision highp float;\n"
        "precision highp int;\n"
        "uniform int seed;\n"
        "out vec4 color;\n"
        "float channel(ivec2 p, int b) {\n"
        "    return float((seed * 29 + p.x * 7 + p.y * 13 + b * 31 + 1) & 255) / 255.0;\n"
        "}\n"
        "void main() {\n"
        "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
        "    color = vec4(channel(p, 0), channel(p, 1), channel(p, 2), channel(p, 3));\n"
        "}\n";

GLuint compile(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512] = {};
        glGetShaderInfoLog(shader, sizeof log - 1, nullptr, log);
        printf("    shader: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool createGl(Gl* g) {
    g->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0, minor = 0;
    if (g->display == EGL_NO_DISPLAY || !eglInitialize(g->display, &major, &minor)) {
        printf("    eglInitialize failed (0x%x)\n", eglGetError());
        return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint configAttributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                       EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                       EGL_NONE};
    EGLConfig config = nullptr;
    EGLint configs = 0;
    if (!eglChooseConfig(g->display, configAttributes, &config, 1, &configs) || configs == 0) {
        printf("    no OpenGL ES 3 pbuffer config (0x%x)\n", eglGetError());
        return false;
    }
    const EGLint surfaceAttributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    g->surface = eglCreatePbufferSurface(g->display, config, surfaceAttributes);
    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    g->context = eglCreateContext(g->display, config, EGL_NO_CONTEXT, contextAttributes);
    if (g->surface == EGL_NO_SURFACE || g->context == EGL_NO_CONTEXT ||
        !eglMakeCurrent(g->display, g->surface, g->surface, g->context)) {
        printf("    no current OpenGL ES 3 context (0x%x)\n", eglGetError());
        return false;
    }
    g->getClientBuffer = reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(
            eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    g->createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    g->destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    g->imageTarget = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!g->getClientBuffer || !g->createImage || !g->destroyImage || !g->imageTarget) {
        printf("    the driver lacks EGL images over hardware buffers\n");
        return false;
    }

    GLuint vertex = compile(GL_VERTEX_SHADER, kVertexShader);
    GLuint fragment = compile(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vertex || !fragment) return false;
    g->program = glCreateProgram();
    glAttachShader(g->program, vertex);
    glAttachShader(g->program, fragment);
    glLinkProgram(g->program);
    GLint linked = 0;
    glGetProgramiv(g->program, GL_LINK_STATUS, &linked);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    if (!linked) {
        printf("    the shader program did not link\n");
        return false;
    }
    g->seedLocation = glGetUniformLocation(g->program, "seed");
    return glOk("shader program");
}

// A hardware buffer as an OpenGL ES texture and a framebuffer that renders into it.
struct Import {
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint texture = 0;
    GLuint framebuffer = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

void destroyImport(const Gl& g, Import* im) {
    if (im->framebuffer) glDeleteFramebuffers(1, &im->framebuffer);
    if (im->texture) glDeleteTextures(1, &im->texture);
    if (im->image != EGL_NO_IMAGE_KHR) g.destroyImage(g.display, im->image);
    *im = Import();
}

bool importBuffer(const Gl& g, AHardwareBuffer* buffer, const AHardwareBuffer_Desc& desc, Import* im) {
    im->width = desc.width;
    im->height = desc.height;
    const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    im->image = g.createImage(g.display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, g.getClientBuffer(buffer),
                              attributes);
    if (im->image == EGL_NO_IMAGE_KHR) {
        printf("    eglCreateImageKHR failed (0x%x)\n", eglGetError());
        return false;
    }
    glGenTextures(1, &im->texture);
    glBindTexture(GL_TEXTURE_2D, im->texture);
    g.imageTarget(GL_TEXTURE_2D, static_cast<GLeglImageOES>(im->image));
    glGenFramebuffers(1, &im->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, im->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, im->texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        printf("    the framebuffer over the buffer is incomplete\n");
        return false;
    }
    return glOk("import");
}

// Renders the pattern of `seed` into the buffer and waits for the GPU to finish.
bool render(const Gl& g, const Import& im, uint32_t seed) {
    glBindFramebuffer(GL_FRAMEBUFFER, im.framebuffer);
    glViewport(0, 0, static_cast<GLsizei>(im.width), static_cast<GLsizei>(im.height));
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glUseProgram(g.program);
    glUniform1i(g.seedLocation, static_cast<GLint>(seed));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    return glOk("render");
}

// Reads the buffer back through the GPU into tightly packed rows.
bool readBack(const Import& im, uint8_t* tight) {
    glBindFramebuffer(GL_FRAMEBUFFER, im.framebuffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, static_cast<GLsizei>(im.width), static_cast<GLsizei>(im.height), GL_RGBA, GL_UNSIGNED_BYTE,
                 tight);
    return glOk("glReadPixels");
}

struct Rect {
    uint32_t x, y, width, height;
    bool contains(uint32_t px, uint32_t py) const {
        return px >= x && px < x + width && py >= y && py < y + height;
    }
};

// Compares the pixels of a picture whose rows are `stride` pixels apart with the pattern of `seed`, and those inside
// `rect` with the pattern of `rectSeed`. With `onlyInside` the pixels outside `rect` are not looked at. Padding never is.
bool picturesMatch(const uint8_t* bytes, uint32_t stride, uint32_t width, uint32_t height, uint32_t seed,
                   const Rect& rect, uint32_t rectSeed, bool onlyInside, const char* what) {
    uint64_t wrong = 0;
    uint32_t firstX = 0, firstY = 0, firstGot = 0, firstWant = 0;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const bool inside = rect.contains(x, y);
            if (onlyInside && !inside) continue;
            for (uint32_t b = 0; b < kBytesPerPixel; b++) {
                const uint8_t got = bytes[(static_cast<size_t>(y) * stride + x) * kBytesPerPixel + b];
                const uint8_t want = pattern(inside ? rectSeed : seed, x, y, b);
                if (got != want) {
                    if (!wrong) {
                        firstX = x;
                        firstY = y;
                        firstGot = got;
                        firstWant = want;
                    }
                    wrong++;
                }
            }
        }
    }
    if (wrong) {
        printf("    %s: %" PRIu64 " bytes differ, first at (%u,%u): read %u, wanted %u\n", what, wrong, firstX, firstY,
               firstGot, firstWant);
    }
    return wrong == 0;
}

bool rowsMatch(const uint8_t* bytes, uint32_t stride, uint32_t width, uint32_t height, uint32_t seed,
               const char* what) {
    return picturesMatch(bytes, stride, width, height, seed, Rect{0, 0, 0, 0}, 0, false, what);
}

void fillPattern(uint8_t* bytes, uint32_t stride, uint32_t seed, const Rect& area) {
    for (uint32_t y = area.y; y < area.y + area.height; y++) {
        for (uint32_t x = area.x; x < area.x + area.width; x++) {
            for (uint32_t b = 0; b < kBytesPerPixel; b++) {
                bytes[(static_cast<size_t>(y) * stride + x) * kBytesPerPixel + b] = pattern(seed, x, y, b);
            }
        }
    }
}

// The same two directions through locks of a rectangle only, which have to find where the rectangle's first row
// starts and where each row after it does: the GPU renders and the CPU reads the rectangle, and the CPU rewrites the
// rectangle of a picture it wrote whole and the GPU reads all of it back.
bool rectangleChecks(const Gl& g, AHardwareBuffer* buffer, const AHardwareBuffer_Desc& desc, const Import& im,
                     uint8_t* tight) {
    Rect rect{desc.width / 5 + 1, 3, 0, desc.height - 6};
    rect.width = desc.width - rect.x - 1;
    const ARect area = {static_cast<int32_t>(rect.x), static_cast<int32_t>(rect.y),
                        static_cast<int32_t>(rect.x + rect.width), static_cast<int32_t>(rect.y + rect.height)};
    void* mapped = nullptr;

    if (!render(g, im, 50)) return false;
    if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, &area, &mapped) != 0 || !mapped) {
        printf("    the lock of a rectangle for reading failed\n");
        return false;
    }
    const bool read = picturesMatch(static_cast<const uint8_t*>(mapped), desc.stride, desc.width, desc.height, 50,
                                    rect, 50, true, "GPU wrote, CPU read a rectangle");
    AHardwareBuffer_unlock(buffer, nullptr);
    if (!read) return false;

    if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &mapped) != 0 || !mapped) {
        printf("    the lock for writing failed\n");
        return false;
    }
    fillPattern(static_cast<uint8_t*>(mapped), desc.stride, 60, Rect{0, 0, desc.width, desc.height});
    AHardwareBuffer_unlock(buffer, nullptr);
    if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, &area, &mapped) != 0 || !mapped) {
        printf("    the lock of a rectangle for writing failed\n");
        return false;
    }
    fillPattern(static_cast<uint8_t*>(mapped), desc.stride, 70, rect);
    AHardwareBuffer_unlock(buffer, nullptr);
    memset(tight, 0, static_cast<size_t>(desc.width) * desc.height * kBytesPerPixel);
    return readBack(im, tight) && picturesMatch(tight, desc.width, desc.width, desc.height, 60, rect, 70, false,
                                                "CPU wrote a rectangle, GPU read");
}

struct Result {
    uint32_t width = 0;
    uint32_t stride = 0;
    bool padded = false;
    bool pixelsOk = false;
    double lockMs = 0;    // the median lock for reading after the GPU wrote
    double unlockMs = 0;  // the median unlock after the CPU wrote
};

// Runs both directions on one buffer.
bool runWidth(const Gl& g, uint32_t width, Result* result) {
    AHardwareBuffer_Desc want = {};
    want.width = width;
    want.height = kHeight;
    want.layers = 1;
    want.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    want.usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                 AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    AHardwareBuffer* buffer = nullptr;
    if (AHardwareBuffer_allocate(&want, &buffer) != 0 || !buffer) {
        printf("%4ux%u: the buffer cannot be allocated\n", width, kHeight);
        return false;
    }
    AHardwareBuffer_Desc got;
    AHardwareBuffer_describe(buffer, &got);
    result->width = width;
    result->stride = got.stride;
    result->padded = got.stride != got.width;

    Import im;
    uint8_t* tight = static_cast<uint8_t*>(malloc(static_cast<size_t>(got.width) * got.height * kBytesPerPixel));
    bool ok = tight && importBuffer(g, buffer, got, &im);
    double lock[kRepeats], unlock[kRepeats];
    size_t locks = 0, unlocks = 0;
    for (int repeat = 0; ok && repeat < kRepeats; repeat++) {
        // The GPU renders the picture; the CPU locks it for reading and finds it at the stride.
        const uint32_t gpuSeed = 1 + static_cast<uint32_t>(repeat);
        ok = render(g, im, gpuSeed);
        if (!ok) break;
        void* mapped = nullptr;
        double start = nowMs();
        int locked = AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &mapped);
        lock[locks++] = nowMs() - start;
        if (locked != 0 || !mapped) {
            printf("    the lock for reading failed (%d)\n", locked);
            ok = false;
            break;
        }
        ok = rowsMatch(static_cast<const uint8_t*>(mapped), got.stride, got.width, got.height, gpuSeed,
                       "GPU wrote, CPU read");
        AHardwareBuffer_unlock(buffer, nullptr);
        if (!ok) break;

        // The CPU writes the picture at the stride and unlocks; the GPU copies it out and finds it tight.
        const uint32_t cpuSeed = 100 + static_cast<uint32_t>(repeat);
        locked = AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &mapped);
        if (locked != 0 || !mapped) {
            printf("    the lock for writing failed (%d)\n", locked);
            ok = false;
            break;
        }
        uint8_t* bytes = static_cast<uint8_t*>(mapped);
        memset(bytes, 0xee, static_cast<size_t>(got.stride) * got.height * kBytesPerPixel);
        fillPattern(bytes, got.stride, cpuSeed, Rect{0, 0, got.width, got.height});
        start = nowMs();
        AHardwareBuffer_unlock(buffer, nullptr);
        unlock[unlocks++] = nowMs() - start;
        memset(tight, 0, static_cast<size_t>(got.width) * got.height * kBytesPerPixel);
        ok = readBack(im, tight) && rowsMatch(tight, got.width, got.width, got.height, cpuSeed, "CPU wrote, GPU read");
    }
    ok = ok && rectangleChecks(g, buffer, got, im, tight);
    destroyImport(g, &im);
    free(tight);
    AHardwareBuffer_release(buffer);
    result->pixelsOk = ok;
    if (ok) {
        result->lockMs = median(lock, locks);
        result->unlockMs = median(unlock, unlocks);
    }
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    uint32_t widths[kMaxWidths];
    size_t widthCount = 0;
    for (int i = 1; i < argc && widthCount < kMaxWidths; i++) {
        widths[widthCount++] = static_cast<uint32_t>(strtoul(argv[i], nullptr, 0));
    }
    if (widthCount == 0) {
        for (uint32_t width = 340; width <= 350; width++) widths[widthCount++] = width;
    }

    Gl gl;
    if (!createGl(&gl)) {
        printf("FAIL: no OpenGL ES context to render with\n");
        return 2;
    }

    Result results[kMaxWidths];
    size_t resultCount = 0;
    int failed = 0;
    for (size_t w = 0; w < widthCount; w++) {
        const uint32_t width = widths[w];
        Result result;
        const bool ok = runWidth(gl, width, &result);
        if (!result.stride) {
            failed++;
            continue;
        }
        printf("%4ux%u stride %4u %-8s lock %8.2f ms, unlock %8.2f ms  %s\n", width, kHeight, result.stride,
               result.padded ? "padded" : "unpadded", result.lockMs, result.unlockMs, ok ? "pixels ok" : "WRONG");
        if (!ok) failed++;
        results[resultCount++] = result;
    }

    double controlLock[kMaxWidths], controlUnlock[kMaxWidths];
    size_t controls = 0;
    for (size_t i = 0; i < resultCount; i++) {
        if (!results[i].padded && results[i].pixelsOk) {
            controlLock[controls] = results[i].lockMs;
            controlUnlock[controls] = results[i].unlockMs;
            controls++;
        }
    }
    if (controls == 0) {
        printf("FAIL: no unpadded width to compare the locks with\n");
        return failed ? 1 : 2;
    }
    const double lockMedian = median(controlLock, controls);
    const double unlockMedian = median(controlUnlock, controls);
    const double baselineLock = lockMedian > kLeastBaselineMs ? lockMedian : kLeastBaselineMs;
    const double baselineUnlock = unlockMedian > kLeastBaselineMs ? unlockMedian : kLeastBaselineMs;
    printf("unpadded: lock %.2f ms, unlock %.2f ms (limit %.0fx of at least %.1f ms)\n", lockMedian, unlockMedian,
           kMaxRatio, kLeastBaselineMs);
    for (size_t i = 0; i < resultCount; i++) {
        const Result& r = results[i];
        if (!r.padded || !r.pixelsOk) continue;
        if (r.lockMs > kMaxRatio * baselineLock || r.unlockMs > kMaxRatio * baselineUnlock) {
            printf("FAIL: width %u locks in %.2f ms and unlocks in %.2f ms, over %.0f times the unpadded %.2f and %.2f\n",
                   r.width, r.lockMs, r.unlockMs, kMaxRatio, baselineLock, baselineUnlock);
            failed++;
        }
    }

    printf("%s: %zu widths, %d failed\n", failed ? "FAIL" : "PASS", resultCount, failed);
    return failed ? 1 : 0;
}
