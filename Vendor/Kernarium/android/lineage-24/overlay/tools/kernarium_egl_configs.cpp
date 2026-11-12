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
 * Lists the EGL configs and draws with stencil in the one the hardware renderer
 * would pick for a window.
 *
 * HWUI asks eglChooseConfig for an RGBA8888 window config with an OpenGL ES 2
 * context, no depth and eight bits of stencil, for every window it draws. The
 * guest offers a config whose only depth-stencil plane is eight bits of
 * stencil, which eglChooseConfig sorts before every config that has a depth
 * plane for that request, so a window stores one byte per pixel where it used
 * to store four or eight. This tool checks that the config is there, that it is
 * the one chosen, and that stencil works in it:
 *
 *  - Choice. The attribute list is HWUI's. The chosen config's depth and
 *    stencil sizes are printed.
 *  - Stencil. A pbuffer of the chosen config is cleared to red and its stencil
 *    to zero, the left half is stencilled with a reference of five by a draw
 *    that writes no colour, and a green draw that passes only where the stencil
 *    equals five must colour the left half and leave the right half red.
 *    GL_DEPTH_BITS and GL_STENCIL_BITS of the context must agree with the
 *    config.
 *
 * Usage: kernarium_egl_configs [--list] [--expect-stencil-only]
 *   --list                print every config
 *   --expect-stencil-only require that the chosen config has no depth and eight
 *                         bits of stencil
 * It always prints the chosen config and runs the stencil draw in it, so a guest
 * without the config shows what it chooses instead. The exit status is 0 when
 * every requested check passed, 1 for a wrong choice or a wrong pixel, and 2
 * when there is no display, config or OpenGL ES context.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

namespace {

constexpr int kSize = 64;
constexpr int kStencilReference = 5;
// Pixels this close to the vertical edge between the halves are not compared.
constexpr int kEdgeMargin = 2;

struct Options {
    bool list = false;
    bool expectStencilOnly = false;
};

int attribute(EGLDisplay display, EGLConfig config, EGLint name) {
    EGLint value = -1;
    eglGetConfigAttrib(display, config, name, &value);
    return value;
}

const char* caveat(int value) {
    switch (value) {
        case EGL_NONE:
            return "none";
        case EGL_SLOW_CONFIG:
            return "slow";
        case EGL_NON_CONFORMANT_CONFIG:
            return "non-conformant";
        default:
            return "unknown";
    }
}

void printConfig(EGLDisplay display, EGLConfig config) {
    printf("config %d: rgba %d%d%d%d depth %d stencil %d samples %d caveat %s surfaces %#x "
           "renderable %#x visual %d\n",
           attribute(display, config, EGL_CONFIG_ID), attribute(display, config, EGL_RED_SIZE),
           attribute(display, config, EGL_GREEN_SIZE), attribute(display, config, EGL_BLUE_SIZE),
           attribute(display, config, EGL_ALPHA_SIZE), attribute(display, config, EGL_DEPTH_SIZE),
           attribute(display, config, EGL_STENCIL_SIZE), attribute(display, config, EGL_SAMPLES),
           caveat(attribute(display, config, EGL_CONFIG_CAVEAT)),
           attribute(display, config, EGL_SURFACE_TYPE),
           attribute(display, config, EGL_RENDERABLE_TYPE),
           attribute(display, config, EGL_NATIVE_VISUAL_ID));
}

// The request android::uirenderer::renderthread::EglManager makes for a window.
bool chooseLikeHwui(EGLDisplay display, EGLConfig* config) {
    const EGLint attributes[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_CONFIG_CAVEAT, EGL_NONE,
        EGL_STENCIL_SIZE, 8,
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_NONE,
    };
    EGLint count = 0;
    return eglChooseConfig(display, attributes, config, 1, &count) == EGL_TRUE && count == 1;
}

GLuint compile(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == GL_FALSE) {
        char log[512] = {};
        glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        fprintf(stderr, "shader did not compile: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint makeProgram() {
    GLuint vertex = compile(GL_VERTEX_SHADER,
                            "attribute vec2 position;\n"
                            "void main() { gl_Position = vec4(position, 0.0, 1.0); }\n");
    GLuint fragment = compile(GL_FRAGMENT_SHADER,
                              "precision mediump float;\n"
                              "uniform vec4 color;\n"
                              "void main() { gl_FragColor = color; }\n");
    if (vertex == 0 || fragment == 0) return 0;
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "position");
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint status = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status == GL_FALSE) {
        fprintf(stderr, "program did not link\n");
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

// Fills the rectangle [x0, x1] x [y0, y1] of clip space with the colour.
void fill(GLuint program, float x0, float y0, float x1, float y1, float r, float g, float b) {
    const float quad[] = {x0, y0, x1, y0, x0, y1, x1, y1};
    glUseProgram(program);
    glUniform4f(glGetUniformLocation(program, "color"), r, g, b, 1.0f);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// Returns 0 when the stencil draw gave the expected picture, 1 when it did not
// and 2 when the context could not be made.
int renderStencil(EGLDisplay display, EGLConfig config) {
    const int depthBits = attribute(display, config, EGL_DEPTH_SIZE);
    const int stencilBits = attribute(display, config, EGL_STENCIL_SIZE);
    if ((attribute(display, config, EGL_SURFACE_TYPE) & EGL_PBUFFER_BIT) == 0) {
        fprintf(stderr, "the chosen config has no pbuffer surface; the stencil draw is skipped\n");
        return 2;
    }
    const EGLint surfaceAttributes[] = {EGL_WIDTH, kSize, EGL_HEIGHT, kSize, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, surfaceAttributes);
    if (surface == EGL_NO_SURFACE) {
        fprintf(stderr, "eglCreatePbufferSurface failed: %#x\n", eglGetError());
        return 2;
    }
    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
    if (context == EGL_NO_CONTEXT || eglMakeCurrent(display, surface, surface, context) != EGL_TRUE) {
        fprintf(stderr, "no OpenGL ES 2 context on the chosen config: %#x\n", eglGetError());
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        return 2;
    }

    int result = 1;
    GLint glDepthBits = -1;
    GLint glStencilBits = -1;
    glGetIntegerv(GL_DEPTH_BITS, &glDepthBits);
    glGetIntegerv(GL_STENCIL_BITS, &glStencilBits);
    printf("context: GL_DEPTH_BITS %d GL_STENCIL_BITS %d\n", glDepthBits, glStencilBits);
    GLuint program = makeProgram();
    if (program == 0) {
        result = 2;
    } else if (glDepthBits != depthBits || glStencilBits != stencilBits) {
        fprintf(stderr, "the context has depth %d and stencil %d bits, the config %d and %d\n",
                glDepthBits, glStencilBits, depthBits, stencilBits);
    } else {
        glViewport(0, 0, kSize, kSize);
        glDisable(GL_DEPTH_TEST);
        glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        // The left half takes a stencil of five from a draw that writes no colour.
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_ALWAYS, kStencilReference, 0xff);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        fill(program, -1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        // Green passes only where the stencil equals five.
        glStencilFunc(GL_EQUAL, kStencilReference, 0xff);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        fill(program, -1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f);
        glDisable(GL_STENCIL_TEST);

        static uint8_t pixels[kSize * kSize * 4];
        glReadPixels(0, 0, kSize, kSize, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        int wrong = 0;
        for (int y = 0; y < kSize; y++) {
            for (int x = 0; x < kSize; x++) {
                if (x >= kSize / 2 - kEdgeMargin && x < kSize / 2 + kEdgeMargin) continue;
                const uint8_t* pixel = &pixels[(y * kSize + x) * 4];
                const bool left = x < kSize / 2;
                const uint8_t red = left ? 0 : 255;
                const uint8_t green = left ? 255 : 0;
                if (pixel[0] != red || pixel[1] != green || pixel[2] != 0 || pixel[3] != 255) {
                    if (wrong++ < 4) {
                        fprintf(stderr, "pixel (%d, %d) is %u %u %u %u, expected %u %u 0 255\n", x,
                                y, pixel[0], pixel[1], pixel[2], pixel[3], red, green);
                    }
                }
            }
        }
        const GLenum error = glGetError();
        if (wrong != 0 || error != GL_NO_ERROR) {
            fprintf(stderr, "the stencil draw gave %d wrong pixels and GL error %#x\n", wrong,
                    error);
        } else {
            printf("stencil draw: ok\n");
            result = 0;
        }
    }
    if (program != 0) glDeleteProgram(program);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    return result;
}

bool parse(int argc, char** argv, Options* options) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            options->list = true;
        } else if (strcmp(argv[i], "--expect-stencil-only") == 0) {
            options->expectStencilOnly = true;
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, &options)) {
        fprintf(stderr, "usage: %s [--list] [--expect-stencil-only]\n", argv[0]);
        return 2;
    }
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0;
    EGLint minor = 0;
    if (display == EGL_NO_DISPLAY || eglInitialize(display, &major, &minor) != EGL_TRUE) {
        fprintf(stderr, "no EGL display: %#x\n", eglGetError());
        return 2;
    }
    printf("EGL %d.%d, vendor %s\n", major, minor, eglQueryString(display, EGL_VENDOR));

    EGLint count = 0;
    eglGetConfigs(display, nullptr, 0, &count);
    printf("%d configs\n", count);
    int stencilOnly = 0;
    if (count > 0) {
        EGLConfig* configs = static_cast<EGLConfig*>(calloc(static_cast<size_t>(count), sizeof(EGLConfig)));
        if (configs == nullptr || eglGetConfigs(display, configs, count, &count) != EGL_TRUE) {
            fprintf(stderr, "eglGetConfigs failed: %#x\n", eglGetError());
            free(configs);
            return 2;
        }
        for (EGLint i = 0; i < count; i++) {
            if (attribute(display, configs[i], EGL_DEPTH_SIZE) == 0 &&
                attribute(display, configs[i], EGL_STENCIL_SIZE) == 8) {
                stencilOnly++;
            }
            if (options.list) printConfig(display, configs[i]);
        }
        free(configs);
    }
    printf("%d configs with no depth and eight bits of stencil\n", stencilOnly);

    EGLConfig chosen = nullptr;
    if (!chooseLikeHwui(display, &chosen)) {
        fprintf(stderr, "eglChooseConfig found no config for HWUI's request\n");
        return 2;
    }
    printf("HWUI's choice: ");
    printConfig(display, chosen);

    int status = 0;
    if (options.expectStencilOnly &&
        (attribute(display, chosen, EGL_DEPTH_SIZE) != 0 || attribute(display, chosen, EGL_STENCIL_SIZE) != 8)) {
        fprintf(stderr, "HWUI's choice is not a config with no depth and eight bits of stencil\n");
        status = 1;
    }
    const int rendered = renderStencil(display, chosen);
    if (rendered == 2) return 2;
    if (rendered != 0) status = 1;
    printf(status == 0 ? "result: ok\n" : "result: failed\n");
    return status;
}
