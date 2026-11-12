// GPU-resident Mesa bridge contract: a multi-pass scene whose results must be
// identical with and without LR_MESA_METAL_RESIDENT=1 (replace mode). Run once
// without the bridge (or LR_MESA_METAL_MODE=off) and once resident; compare the
// RESIDENT_CONTRACT lines. Pixel-aligned rectangles and NEAREST sampling avoid
// implementation-dependent edge pixels, so results must match exactly.
// Covers: colour + packed depth/stencil texture targets, GPU clears, depth-tested
// overlap, a pass sampling the previous pass (RGBA16F), a Mesa glTexSubImage2D
// between GPU passes, glReadPixels (texture FBOs and window), glGetTexImage of
// depth, glClearBufferfv, a scissored clear (Mesa path) and glBlitFramebuffer.
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 128
#define H 128
static uint64_t hash(const void *p, size_t n) {
    const unsigned char *b = p; uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
static GLuint shader(GLenum kind, const char *src) {
    GLuint s = glCreateShader(kind); glShaderSource(s, 1, &src, NULL); glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "shader: %s\n", log); exit(2); }
    return s;
}
static GLuint program(const char *vs, const char *fs) {
    GLuint p = glCreateProgram(); glAttachShader(p, shader(GL_VERTEX_SHADER, vs)); glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "pos"); glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok); if (!ok) { fprintf(stderr, "link failed\n"); exit(2); }
    return p;
}
static GLuint texture(GLenum internal, GLenum format, GLenum type) {
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, W, H, 0, format, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}
// Axis-aligned rectangle in pixels of a w x h target, as two triangles.
static GLuint rect_vbo;
static void rect(int x0, int y0, int x1, int y1, int w, int h, float z) {
    float a = 2.0f * x0 / w - 1, b = 2.0f * y0 / h - 1, c = 2.0f * x1 / w - 1, d = 2.0f * y1 / h - 1;
    float v[] = {a, b, z, c, b, z, c, d, z, a, b, z, c, d, z, a, d, z};
    glBindBuffer(GL_ARRAY_BUFFER, rect_vbo); glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, NULL); glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

int main(void) {
    Display *d = XOpenDisplay(NULL); if (!d) { fprintf(stderr, "no display\n"); return 2; }
    int attrs[] = {GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_DOUBLEBUFFER, True, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8, None};
    int n; GLXFBConfig *configs = glXChooseFBConfig(d, DefaultScreen(d), attrs, &n); if (!configs || !n) return 2;
    XVisualInfo *vi = glXGetVisualFromFBConfig(d, configs[0]);
    XSetWindowAttributes swa = {.colormap = XCreateColormap(d, RootWindow(d, vi->screen), vi->visual, AllocNone)};
    Window win = XCreateWindow(d, RootWindow(d, vi->screen), 0, 0, W, H, 0, vi->depth, InputOutput, vi->visual, CWColormap, &swa);
    XMapWindow(d, win); XSync(d, False);
    PFNGLXCREATECONTEXTATTRIBSARBPROC create = (void *)glXGetProcAddressARB((const GLubyte *)"glXCreateContextAttribsARB");
    int ctx_attrs[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 4, GLX_CONTEXT_MINOR_VERSION_ARB, 5, GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None};
    GLXContext ctx = create(d, configs[0], NULL, True, ctx_attrs); if (!ctx || !glXMakeCurrent(d, win, ctx)) return 2;
    GLuint vao; glGenVertexArrays(1, &vao); glBindVertexArray(vao); glGenBuffers(1, &rect_vbo);

    const char *vs = "#version 330 core\nin vec3 pos;void main(){gl_Position=vec4(pos,1);}";
    GLuint solid = program(vs, "#version 330 core\nuniform vec4 color;out vec4 o;void main(){o=color;}");
    GLuint copy = program(vs, "#version 330 core\nuniform sampler2D src;uniform float scale;out vec4 o;void main(){o=texelFetch(src,ivec2(gl_FragCoord.xy),0)*scale;}");

    // Pass A: colour + packed depth/stencil textures.
    GLuint color_a = texture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE), depth_a = texture(GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8);
    GLuint fb_a; glGenFramebuffers(1, &fb_a); glBindFramebuffer(GL_FRAMEBUFFER, fb_a);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_a, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, depth_a, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return 3;
    // Pass B: RGBA16F.
    GLuint color_b = texture(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT), fb_b; glGenFramebuffers(1, &fb_b);
    glBindFramebuffer(GL_FRAMEBUFFER, fb_b); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_b, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return 3;

    static unsigned char rgba[W * H * 4]; static uint16_t half[W * H * 4]; static float depth[W * H];
    for (int frame = 0; frame < 3; frame++) {
        glViewport(0, 0, W, H);
        glBindFramebuffer(GL_FRAMEBUFFER, fb_a);
        glClearColor(0.2f, 0.3f, 0.4f, 1.0f); glClearDepth(1.0); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glUseProgram(solid);
        glUniform4f(glGetUniformLocation(solid, "color"), 1, 0, 0, 1); rect(0, 0, 64 + frame * 8, 128, W, H, 0.0f);
        glUniform4f(glGetUniformLocation(solid, "color"), 0, 1, 0, 1); rect(32, 32, 96, 96, W, H, -0.6f);   // nearer
        glUniform4f(glGetUniformLocation(solid, "color"), 0, 0, 1, 1); rect(40, 40, 120, 60, W, H, 0.5f);   // farther: hidden where covered
        glDisable(GL_DEPTH_TEST);
        // Mesa-side modification between GPU passes.
        unsigned char patch[16 * 16 * 4]; for (int i = 0; i < 16 * 16; i++) { patch[i * 4] = 10; patch[i * 4 + 1] = 200; patch[i * 4 + 2] = 30 + frame; patch[i * 4 + 3] = 255; }
        glBindTexture(GL_TEXTURE_2D, color_a); glTexSubImage2D(GL_TEXTURE_2D, 0, 100, 100, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, patch);
        // Pass B samples pass A.
        glBindFramebuffer(GL_FRAMEBUFFER, fb_b);
        const float zero[4] = {0, 0, 0, 0}; glClearBufferfv(GL_COLOR, 0, zero);
        glUseProgram(copy); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, color_a);
        glUniform1i(glGetUniformLocation(copy, "src"), 0); glUniform1f(glGetUniformLocation(copy, "scale"), 2.0f);
        rect(0, 0, W, H, W, H, 0);
        // Window: sample B, then a scissored clear (Mesa) and a blit from A.
        glBindFramebuffer(GL_FRAMEBUFFER, 0); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
        glBindTexture(GL_TEXTURE_2D, color_b); glUniform1f(glGetUniformLocation(copy, "scale"), 0.25f); rect(0, 0, W, H, W, H, 0);
        glEnable(GL_SCISSOR_TEST); glScissor(8, 8, 8, 8); glClearColor(1, 1, 0, 1); glClear(GL_COLOR_BUFFER_BIT); glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb_a); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, 32, 32, 96, 96, 128, 128, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        // Read everything back.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb_a); glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, rgba); uint64_t ha = hash(rgba, sizeof rgba);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb_b); glReadPixels(0, 0, W, H, GL_RGBA, GL_HALF_FLOAT, half); uint64_t hb = hash(half, sizeof half);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0); glReadBuffer(GL_BACK); glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, rgba); uint64_t hw = hash(rgba, sizeof rgba);
        glBindTexture(GL_TEXTURE_2D, depth_a); glGetTexImage(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
        // Depth quantised to 24 bits as stored by the texture.
        for (int i = 0; i < W * H; i++) depth[i] = (float)(int)(depth[i] * 16777215.0f + 0.5f);
        uint64_t hd = hash(depth, sizeof depth);
        const char *dump = getenv("RESIDENT_CONTRACT_DUMP");   // file prefix; one set per frame
        if (dump) {
            char path[512]; FILE *f;
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fb_a); glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            snprintf(path, sizeof path, "%s-%d-fb_a.rgba", dump, frame); if ((f = fopen(path, "wb"))) { fwrite(rgba, 1, sizeof rgba, f); fclose(f); }
            snprintf(path, sizeof path, "%s-%d-fb_b.half", dump, frame); if ((f = fopen(path, "wb"))) { fwrite(half, 1, sizeof half, f); fclose(f); }
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0); glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            snprintf(path, sizeof path, "%s-%d-window.rgba", dump, frame); if ((f = fopen(path, "wb"))) { fwrite(rgba, 1, sizeof rgba, f); fclose(f); }
            snprintf(path, sizeof path, "%s-%d-depth.f32", dump, frame); if ((f = fopen(path, "wb"))) { fwrite(depth, 1, sizeof depth, f); fclose(f); }
        }
        printf("RESIDENT_CONTRACT frame=%d fb_a=%016llx fb_b=%016llx window=%016llx depth=%016llx error=%x\n", frame,
               (unsigned long long)ha, (unsigned long long)hb, (unsigned long long)hw, (unsigned long long)hd, glGetError());
        glXSwapBuffers(d, win);
    }
    printf("RESIDENT_CONTRACT_DONE\n");
    return 0;
}
