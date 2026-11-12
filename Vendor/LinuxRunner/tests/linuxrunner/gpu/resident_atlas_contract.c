// Resident texture formats contract (self-checking; run with and without
// LR_MESA_METAL_RESIDENT=1 in replace mode). Covers what toolkits use for text
// and UI: a wide single-row R8 glyph atlas (Blender allocates 16384 x N), an
// RG8 texture, GL_SRGB8_ALPHA8 render targets with GL_FRAMEBUFFER_SRGB on and
// off, and sampling an sRGB texture (decode) and with GL_SKIP_DECODE_EXT.
// usage: gpu-resident-atlas [atlas_width] [atlas_height]
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#define FW 256
static int failures;
static GLuint shader(GLenum kind, const char *src) {
    GLuint s = glCreateShader(kind); glShaderSource(s, 1, &src, NULL); glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[512]; glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "shader: %s\n", log); exit(2); }
    return s;
}
static GLuint program(const char *fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, shader(GL_VERTEX_SHADER, "#version 330 core\nin vec2 pos;void main(){gl_Position=vec4(pos,0,1);}"));
    glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs)); glBindAttribLocation(p, 0, "pos"); glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok); if (!ok) { fprintf(stderr, "link failed\n"); exit(2); }
    return p;
}
static void fullscreen(void) { glDrawArrays(GL_TRIANGLES, 0, 6); }
static GLuint target(GLenum internal, int w, int h, GLuint *texture) {
    GLuint fb; glGenTextures(1, texture); glBindTexture(GL_TEXTURE_2D, *texture);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fb); glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "fbo incomplete\n"); exit(2); }
    glViewport(0, 0, w, h); return fb;
}
static void check(const char *what, int got, int want, int tolerance) {
    if (abs(got - want) > tolerance) { if (failures++ < 12) printf("RESIDENT_ATLAS_MISMATCH %s got=%d want=%d\n", what, got, want); }
}

int main(int argc, char **argv) {
    int aw = argc > 1 ? atoi(argv[1]) : 16384, ah = argc > 2 ? atoi(argv[2]) : 2;
    Display *d = XOpenDisplay(NULL); if (!d) return 2;
    int attrs[] = {GLX_RENDER_TYPE, GLX_RGBA_BIT, GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT, GLX_DOUBLEBUFFER, True, GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};
    int n; GLXFBConfig *configs = glXChooseFBConfig(d, DefaultScreen(d), attrs, &n); if (!configs || !n) return 2;
    XVisualInfo *vi = glXGetVisualFromFBConfig(d, configs[0]);
    XSetWindowAttributes swa = {.colormap = XCreateColormap(d, RootWindow(d, vi->screen), vi->visual, AllocNone)};
    Window win = XCreateWindow(d, RootWindow(d, vi->screen), 0, 0, 64, 64, 0, vi->depth, InputOutput, vi->visual, CWColormap, &swa);
    XMapWindow(d, win); XSync(d, False);
    PFNGLXCREATECONTEXTATTRIBSARBPROC create = (void *)glXGetProcAddressARB((const GLubyte *)"glXCreateContextAttribsARB");
    int ctx_attrs[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 4, GLX_CONTEXT_MINOR_VERSION_ARB, 5, GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None};
    GLXContext ctx = create(d, configs[0], NULL, True, ctx_attrs); if (!ctx || !glXMakeCurrent(d, win, ctx)) return 2;
    GLuint vao, vbo; glGenVertexArrays(1, &vao); glBindVertexArray(vao); glGenBuffers(1, &vbo); glBindBuffer(GL_ARRAY_BUFFER, vbo);
    float quad[] = {-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1};
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL); glEnableVertexAttribArray(0);
    static unsigned char out[FW * 64 * 4];

    // 1. Wide R8 atlas, sampled with normalized coordinates (NEAREST) like a glyph cache.
    unsigned char *atlas = malloc((size_t)aw * ah);
    for (int y = 0; y < ah; y++) for (int x = 0; x < aw; x++) atlas[(size_t)y * aw + x] = (unsigned char)(x * 7 + y * 131);
    GLuint at; glGenTextures(1, &at); glBindTexture(GL_TEXTURE_2D, at); glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, aw, ah, 0, GL_RED, GL_UNSIGNED_BYTE, atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GLuint ct, fb = target(GL_RGBA8, FW, ah, &ct);
    // Fragment column x samples atlas texel x*stride+offset of row y.
    GLuint glyph = program("#version 330 core\nuniform sampler2D atlas;uniform vec2 size;uniform float stride,offset;out vec4 o;"
                           "void main(){vec2 t=vec2(floor(gl_FragCoord.x)*stride+offset+0.5,gl_FragCoord.y)/size;vec4 s=texture(atlas,t);o=vec4(s.r,s.g,s.b,s.a);}");
    glUseProgram(glyph); glUniform1i(glGetUniformLocation(glyph, "atlas"), 0); glUniform2f(glGetUniformLocation(glyph, "size"), aw, ah);
    int stride = aw / FW, offsets[] = {0, stride / 2, stride - 1};
    for (int k = 0; k < 3; k++) {
        glUniform1f(glGetUniformLocation(glyph, "stride"), stride); glUniform1f(glGetUniformLocation(glyph, "offset"), offsets[k]);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, at); fullscreen();
        glReadPixels(0, 0, FW, ah, GL_RGBA, GL_UNSIGNED_BYTE, out);
        for (int y = 0; y < ah; y++) for (int x = 0; x < FW; x++) {
            const unsigned char *p = out + ((size_t)y * FW + x) * 4; char what[64];
            snprintf(what, sizeof what, "r8 x=%d y=%d k=%d", x, y, k);
            check(what, p[0], atlas[(size_t)y * aw + x * stride + offsets[k]], 0); check(what, p[1], 0, 0); check(what, p[2], 0, 0); check(what, p[3], 255, 0);
        }
        // A Mesa-side update between GPU draws must reach the next sample (glyph cache growth).
        unsigned char patch[16]; memset(patch, 200 + k, sizeof patch);
        glBindTexture(GL_TEXTURE_2D, at); glTexSubImage2D(GL_TEXTURE_2D, 0, offsets[(k + 1) % 3], 0, 1, 1, GL_RED, GL_UNSIGNED_BYTE, patch);
        atlas[offsets[(k + 1) % 3]] = 200 + k;
    }

    // 2. RG8 texture sampled into RGBA8 (b=0, a=1).
    unsigned char rg[FW * 2]; for (int i = 0; i < FW; i++) { rg[2 * i] = i; rg[2 * i + 1] = 255 - i; }
    GLuint rgt; glGenTextures(1, &rgt); glBindTexture(GL_TEXTURE_2D, rgt);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, FW, 1, 0, GL_RG, GL_UNSIGNED_BYTE, rg);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glUniform2f(glGetUniformLocation(glyph, "size"), FW, 1); glUniform1f(glGetUniformLocation(glyph, "stride"), 1); glUniform1f(glGetUniformLocation(glyph, "offset"), 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fb); glViewport(0, 0, FW, 1); fullscreen();
    glReadPixels(0, 0, FW, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
    for (int x = 0; x < FW; x++) { check("rg8 r", out[4 * x], x, 0); check("rg8 g", out[4 * x + 1], 255 - x, 0); check("rg8 b", out[4 * x + 2], 0, 0); check("rg8 a", out[4 * x + 3], 255, 0); }

    // 3. sRGB render target: encode with GL_FRAMEBUFFER_SRGB, raw without.
    GLuint st; target(GL_SRGB8_ALPHA8, 64, 64, &st);
    GLuint solid = program("#version 330 core\nuniform vec4 color;out vec4 o;void main(){o=color;}");
    glUseProgram(solid); glUniform4f(glGetUniformLocation(solid, "color"), 0.5f, 0.25f, 0.0f, 1.0f);
    glEnable(GL_FRAMEBUFFER_SRGB); glViewport(0, 0, 64, 32); fullscreen(); glDisable(GL_FRAMEBUFFER_SRGB);
    glViewport(0, 32, 64, 32); fullscreen(); glViewport(0, 0, 64, 64);
    unsigned char px[4];
    glReadPixels(10, 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px); check("srgb encode r", px[0], 188, 1); check("srgb encode g", px[1], 137, 1); check("srgb encode b", px[2], 0, 0);
    glReadPixels(10, 50, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px); check("srgb raw r", px[0], 128, 1); check("srgb raw g", px[1], 64, 1);
    // GPU clear of the sRGB target with encoding on.
    glEnable(GL_FRAMEBUFFER_SRGB); glClearColor(0.5f, 0.5f, 0.5f, 1); glClear(GL_COLOR_BUFFER_BIT); glDisable(GL_FRAMEBUFFER_SRGB);
    glReadPixels(5, 5, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px); check("srgb clear r", px[0], 188, 1);

    // 4. Sampling the sRGB texture decodes (188 -> ~0.5); SKIP_DECODE samples raw bytes.
    glBindFramebuffer(GL_FRAMEBUFFER, fb); glViewport(0, 0, 64, 1); glUseProgram(glyph);
    glUniform2f(glGetUniformLocation(glyph, "size"), 64, 64); glUniform1f(glGetUniformLocation(glyph, "stride"), 1); glUniform1f(glGetUniformLocation(glyph, "offset"), 0);
    glBindTexture(GL_TEXTURE_2D, st); fullscreen(); glReadPixels(3, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px); check("srgb decode r", px[0], 128, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT); fullscreen();
    glReadPixels(3, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px); check("srgb skip-decode r", px[0], 188, 1);

    // 5. Scissored and write-masked clears (UI toolkits clear sub-rectangles), colour and depth.
    {
        GLuint ct2, cfb = target(GL_RGBA8, 64, 64, &ct2), dt;
        glGenTextures(1, &dt); glBindTexture(GL_TEXTURE_2D, dt); glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, 64, 64, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, dt, 0);
        glClearColor(1, 0, 0, 1); glClearDepth(1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST); glScissor(8, 16, 20, 10);
        glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE); glClearColor(0, 1, 1, 0.5f); glClearDepth(0.25); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE); glDisable(GL_SCISSOR_TEST);
        // A GPU draw after the clears must see them (depth test LESS at z=0.5 -> only outside the box passes).
        glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glUseProgram(solid); glUniform4f(glGetUniformLocation(solid, "color"), 0, 0, 1, 1);
        fullscreen(); glDisable(GL_DEPTH_TEST);
        glReadPixels(10, 20, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px); check("masked clear inside r", px[0], 255, 0); check("masked clear inside g", px[1], 255, 0); check("masked clear inside b", px[2], 0, 0); check("masked clear inside a", px[3], 128, 1);
        glReadPixels(40, 40, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px); check("outside box drawn b", px[2], 255, 0); check("outside box drawn r", px[0], 0, 0);
        float z; glReadPixels(10, 20, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &z); check("scissored depth clear", (int)(z * 1000 + 0.5f), 250, 0);
        glReadPixels(40, 40, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &z); check("depth outside box", (int)(z * 1000 + 0.5f), 500, 1);
    }

    // 6. glBlitFramebuffer: mirrored 2x NEAREST, scissored, to the window, and depth.
    {
        static unsigned char src[32 * 32 * 4];
        for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) { unsigned char *q = src + (y * 32 + x) * 4; q[0] = x * 8; q[1] = y * 8; q[2] = 77; q[3] = 255; }
        GLuint stex, sfb, dtex, dfb = target(GL_RGBA8, 64, 64, &dtex);
        glGenTextures(1, &stex); glBindTexture(GL_TEXTURE_2D, stex); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, src);
        GLuint sd; glGenTextures(1, &sd); glBindTexture(GL_TEXTURE_2D, sd); glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, 32, 32, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
        glGenFramebuffers(1, &sfb); glBindFramebuffer(GL_FRAMEBUFFER, sfb);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, stex, 0); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, sd, 0);
        glViewport(0, 0, 32, 32); glClearDepth(0.3); glClear(GL_DEPTH_BUFFER_BIT);
        GLuint dd; glGenTextures(1, &dd); glBindTexture(GL_TEXTURE_2D, dd); glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, 64, 64, 0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
        glBindFramebuffer(GL_FRAMEBUFFER, dfb); glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, dd, 0);
        glViewport(0, 0, 64, 64); glClearColor(0, 0, 0, 1); glClearDepth(1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, sfb); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dfb);
        glEnable(GL_SCISSOR_TEST); glScissor(0, 0, 48, 64);
        glBlitFramebuffer(0, 0, 32, 32, 64, 0, 0, 64, GL_COLOR_BUFFER_BIT, GL_NEAREST);   // mirrored in x, 2x
        glDisable(GL_SCISSOR_TEST);
        glBlitFramebuffer(0, 0, 32, 32, 0, 0, 32, 32, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, dfb);
        static unsigned char got[64 * 64 * 4]; glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, got);
        for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
            const unsigned char *g = got + (y * 64 + x) * 4; char what[48]; snprintf(what, sizeof what, "blit x=%d y=%d", x, y);
            if (x >= 48) { check(what, g[0], 0, 0); check(what, g[2], 0, 0); continue; }   // outside the scissor
            int sx = (int)((64 - (x + 0.5f)) / 2), sy = (int)((y + 0.5f) / 2);
            check(what, g[0], sx * 8, 0); check(what, g[1], sy * 8, 0); check(what, g[2], 77, 0);
        }
        float z = 0; glReadPixels(5, 5, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &z); check("depth blit", (int)(z * 1000 + 0.5f), 300, 0);
        glReadPixels(40, 40, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &z); check("depth blit outside", (int)(z * 1000 + 0.5f), 1000, 0);
        // To the window, then read it back.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, sfb); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, 32, 32, 0, 0, 32, 32, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0); glReadPixels(7, 9, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        check("window blit r", px[0], 56, 0); check("window blit g", px[1], 72, 0); check("window blit b", px[2], 77, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // 7. A Mesa-only draw (smooth line) in a small viewport between GPU work on the
    //    window: only its rectangle crosses over, and nothing around it is lost.
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0); glViewport(0, 0, 64, 64);
        glClearColor(0, 0, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(solid); glUniform4f(glGetUniformLocation(solid, "color"), 1, 0, 0, 1);
        glViewport(0, 0, 32, 32); fullscreen();
        float line[] = {-1, 0.0625f, 1, 0.0625f}; GLuint lvbo; glGenBuffers(1, &lvbo); glBindBuffer(GL_ARRAY_BUFFER, lvbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof line, line, GL_STATIC_DRAW); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
        glEnable(GL_LINE_SMOOTH); glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUniform4f(glGetUniformLocation(solid, "color"), 0, 1, 0, 1); glViewport(40, 40, 16, 16); glDrawArrays(GL_LINES, 0, 2);
        glDisable(GL_LINE_SMOOTH); glDisable(GL_BLEND);
        glBindBuffer(GL_ARRAY_BUFFER, vbo); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
        glUniform4f(glGetUniformLocation(solid, "color"), 1, 1, 0, 1); glViewport(0, 40, 16, 16); fullscreen();
        glViewport(0, 0, 64, 64);
        static unsigned char win[64 * 64 * 4]; glBindFramebuffer(GL_READ_FRAMEBUFFER, 0); glReadPixels(0, 0, 64, 64, GL_RGBA, GL_UNSIGNED_BYTE, win);
        #define WIN(x, y, k) win[((y) * 64 + (x)) * 4 + (k)]
        check("region: red quad", WIN(10, 10, 0), 255, 0); check("region: red quad b", WIN(10, 10, 2), 0, 0);
        check("region: blue clear", WIN(60, 10, 2), 255, 0); check("region: blue clear r", WIN(60, 10, 0), 0, 0);
        check("region: yellow after", WIN(8, 48, 0), 255, 0); check("region: yellow after g", WIN(8, 48, 1), 255, 0);
        if (WIN(48, 48, 1) < 128) check("region: smooth line drawn", WIN(48, 48, 1), 255, 127);
        check("region: beside line", WIN(48, 41, 2), 255, 0);
        #undef WIN
    }

    // 8. Depth rendered on the GPU into a packed depth/stencil texture, then
    //    sampled by a later pass (overlay-style), for 24/8 and 32F formats.
    {
        GLenum formats[2][3] = {{GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8}, {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT}};
        for (int f = 0; f < 2; f++) {
            GLuint color, dtex, fb = target(GL_RGBA8, 64, 64, &color);
            glGenTextures(1, &dtex); glBindTexture(GL_TEXTURE_2D, dtex);
            glTexImage2D(GL_TEXTURE_2D, 0, formats[f][0], 64, 64, 0, formats[f][1], formats[f][2], NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glFramebufferTexture2D(GL_FRAMEBUFFER, f ? GL_DEPTH_ATTACHMENT : GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, dtex, 0);
            glClearDepth(1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glUseProgram(solid); glUniform4f(glGetUniformLocation(solid, "color"), 1, 1, 1, 1);
            glViewport(0, 0, 32, 64); fullscreen(); glViewport(0, 0, 64, 64); glDisable(GL_DEPTH_TEST);   // left half depth 0.5, right half stays 1
            GLuint out, ofb = target(GL_RGBA8, 64, 64, &out);
            glUseProgram(glyph); glUniform2f(glGetUniformLocation(glyph, "size"), 64, 64); glUniform1f(glGetUniformLocation(glyph, "stride"), 1); glUniform1f(glGetUniformLocation(glyph, "offset"), 0);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, dtex); fullscreen();
            glReadPixels(10, 30, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px); check(f ? "depth32f sample left" : "depth24s8 sample left", px[0], 128, 1); check("depth sample left a", px[3], 255, 0);
            glReadPixels(50, 30, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px); check(f ? "depth32f sample right" : "depth24s8 sample right", px[0], 255, 0);
            glDeleteFramebuffers(1, &fb); glDeleteFramebuffers(1, &ofb);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    GLenum error = glGetError();
    // In resident mode the cases must have run on Metal, not passed by falling back to Mesa.
    unsigned long long committed = 0; int gpu_required = getenv("LR_MESA_METAL_RESIDENT") && getenv("LD_PRELOAD");
    int (*stats)(unsigned long long *, void *, void *, void *, void *, void *) = (void *)glXGetProcAddressARB((const GLubyte *)"lrMesaMetalStatistics");
    void *self = dlopen(NULL, RTLD_LAZY); if (self) { void *f = dlsym(self, "lrMesaMetalStatistics"); if (f) stats = f; }
    if (stats) stats(&committed, NULL, NULL, NULL, NULL, NULL);
    int gpu_ok = !gpu_required || committed >= 8;
    printf("RESIDENT_ATLAS atlas=%dx%d failures=%d error=%x metal_draws=%llu %s\n", aw, ah, failures, error, committed, failures || error || !gpu_ok ? "FAIL" : "PASS");
    return failures || error || !gpu_ok ? 1 : 0;
}
