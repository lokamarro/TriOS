// SPDX-License-Identifier: GPL-2.0-or-later
// GPU composition with EGL double buffering on X11 and page-flipped GBM on KMS.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <cairo.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

using Clock = std::chrono::steady_clock;
static volatile std::sig_atomic_t stopped = 0;
static void stop(int) { stopped = 1; }
static void require(bool success, const char *operation)
{
    if (!success) throw std::runtime_error(operation);
}
class DisplayCanvas {
public:
    int width = 0, height = 0;
    explicit DisplayCanvas(bool framebuffer) : kms(framebuffer)
    {
        try {
            if (std::filesystem::exists("/usr/share/uml-steam/vulkan-runtime.json")) {
                setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);
                setenv("GALLIUM_DRIVER", "zink", 1);
            }
            if (kms) openKms(); else openX11();
            openEgl();
        } catch (...) { release(); throw; }
    }
    ~DisplayCanvas() { release(); }
    void present()
    {
        require(eglSwapBuffers(egl, surface), "EGL swap buffers");
        if (!kms) return;
        gbm_bo *next = gbm_surface_lock_front_buffer(gbmSurface);
        require(next, "GBM front buffer");
        // KMS framebuffer identities follow GBM buffer lifetime, not frame lifetime.
        auto *framebuffer = static_cast<Framebuffer *>(gbm_bo_get_user_data(next));
        if (!framebuffer) {
            uint32_t handles[4]{gbm_bo_get_handle(next).u32, 0, 0, 0};
            uint32_t pitches[4]{gbm_bo_get_stride(next), 0, 0, 0}, offsets[4]{};
            uint32_t id = 0;
            if (drmModeAddFB2(fd, width, height, gbm_bo_get_format(next), handles, pitches, offsets, &id, 0)) {
                gbm_surface_release_buffer(gbmSurface, next);
                throw std::runtime_error("DRM GPU framebuffer creation");
            }
            framebuffer = new Framebuffer{fd, id};
            gbm_bo_set_user_data(next, framebuffer, [](gbm_bo *, void *data) {
                auto *fb = static_cast<Framebuffer *>(data);
                drmModeRmFB(fb->fd, fb->id);
                delete fb;
            });
        }
        if (!front) {
            require(drmModeSetCrtc(fd, crtc, framebuffer->id, 0, 0, &connector, 1, &mode) == 0,
                    "DRM initial GPU modeset");
        } else {
            bool pending = true;
            require(drmModePageFlip(fd, crtc, framebuffer->id, DRM_MODE_PAGE_FLIP_EVENT, &pending) == 0,
                    "DRM GPU page flip");
            drmEventContext events{};
            events.version = 2;
            events.page_flip_handler = [](int, unsigned, unsigned, unsigned, void *data) {
                *static_cast<bool *>(data) = false;
            };
            const auto deadline = Clock::now() + std::chrono::seconds(2);
            while (pending) {
                require(Clock::now() < deadline, "DRM page flip timed out");
                pollfd event{fd, POLLIN, 0};
                const int result = poll(&event, 1, 100);
                if (result < 0 && errno == EINTR) continue;
                require(result >= 0 && !(event.revents & (POLLERR | POLLHUP | POLLNVAL)), "DRM event poll");
                if (result > 0) require(drmHandleEvent(fd, &events) == 0, "DRM flip completion");
            }
            gbm_surface_release_buffer(gbmSurface, front);
        }
        front = next;
    }
private:
    struct Framebuffer { int fd; uint32_t id; };
    bool kms;
    Display *display = nullptr;
    Window window = 0;
    Colormap colormap = 0;
    int fd = -1;
    uint32_t crtc = 0, connector = 0;
    drmModeModeInfo mode{};
    gbm_device *gbm = nullptr;
    gbm_surface *gbmSurface = nullptr;
    gbm_bo *front = nullptr;
    EGLDisplay egl = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    void openX11()
    {
        display = XOpenDisplay(nullptr);
        require(display, "Gamescope Xwayland display is unavailable");
        const int screen = DefaultScreen(display);
        width = DisplayWidth(display, screen); height = DisplayHeight(display, screen);
    }
    void createWindow(EGLConfig config)
    {
        EGLint visualId = 0;
        require(eglGetConfigAttrib(egl, config, EGL_NATIVE_VISUAL_ID, &visualId), "EGL visual");
        XVisualInfo wanted{};
        wanted.visualid = visualId;
        int count = 0;
        XVisualInfo *visual = XGetVisualInfo(display, VisualIDMask, &wanted, &count);
        require(visual && count > 0, "X11 EGL visual");
        const Window root = RootWindow(display, visual->screen);
        colormap = XCreateColormap(display, root, visual->visual, AllocNone);
        XSetWindowAttributes attributes{};
        attributes.colormap = colormap;
        window = XCreateWindow(display, root, 0, 0, width, height, 0, visual->depth,
                               InputOutput, visual->visual, CWColormap, &attributes);
        XFree(visual);
        XStoreName(display, window, "Kernarium");
        unsigned long app = 769;
        XChangeProperty(display, window, XInternAtom(display, "STEAM_GAME", False), XA_CARDINAL,
                        32, PropModeReplace, reinterpret_cast<unsigned char *>(&app), 1);
        const Atom base = XInternAtom(display, "GAMESCOPECTRL_BASELAYER_APPID", False);
        Atom actual = None;
        int format = 0;
        unsigned long countValues = 0, after = 0;
        unsigned char *value = nullptr;
        XGetWindowProperty(display, root, base, 0, 1, False, AnyPropertyType,
                           &actual, &format, &countValues, &after, &value);
        if (actual == None)
            XChangeProperty(display, root, base, XA_CARDINAL, 32, PropModeReplace,
                            reinterpret_cast<unsigned char *>(&app), 1);
        if (value) XFree(value);
        XMapWindow(display, window);
        XFlush(display);
    }
    void openKms()
    {
        const auto deadline = Clock::now() + std::chrono::seconds(30);
        while (!std::filesystem::exists("/dev/dri/card0")) {
            require(!stopped && Clock::now() < deadline, "DRM boot display did not appear");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
        require(fd >= 0 && drmSetMaster(fd) == 0, "DRM master acquisition");
        drmModeRes *resources = drmModeGetResources(fd);
        require(resources, "DRM resources");
        if (resources->count_crtcs > 0) crtc = resources->crtcs[0];
        for (int i = 0; i < resources->count_connectors && !connector; ++i) {
            drmModeConnector *candidate = drmModeGetConnector(fd, resources->connectors[i]);
            if (!candidate) continue;
            if (candidate->connection == DRM_MODE_CONNECTED && candidate->count_modes > 0) {
                connector = candidate->connector_id;
                mode = candidate->modes[0];
            }
            drmModeFreeConnector(candidate);
        }
        drmModeFreeResources(resources);
        require(crtc && connector, "DRM has no connected boot scanout");
        width = mode.hdisplay; height = mode.vdisplay;
        gbm = gbm_create_device(fd);
        require(gbm, "GBM GPU device");
        gbmSurface = gbm_surface_create(gbm, width, height, GBM_FORMAT_XRGB8888,
                                        GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        require(gbmSurface, "GBM GPU surface");
    }
    void openEgl()
    {
        auto getPlatform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        require(getPlatform, "EGL platform display extension");
        egl = getPlatform(kms ? EGL_PLATFORM_GBM_KHR : EGL_PLATFORM_X11_EXT,
                          kms ? static_cast<void *>(gbm) : static_cast<void *>(display), nullptr);
        require(egl != EGL_NO_DISPLAY && eglInitialize(egl, nullptr, nullptr), "EGL GPU initialization");
        require(eglBindAPI(EGL_OPENGL_ES_API), "EGL OpenGL ES API");
        const EGLint attributes[]{EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0, EGL_NONE};
        std::array<EGLConfig, 64> configs{};
        EGLint count = 0;
        require(eglChooseConfig(egl, attributes, configs.data(), configs.size(), &count) && count > 0, "EGL GPU config");
        EGLConfig config = nullptr;
        for (int i = 0; i < count; ++i) {
            EGLint visual = 0, alpha = 0;
            eglGetConfigAttrib(egl, configs[i], EGL_NATIVE_VISUAL_ID, &visual);
            eglGetConfigAttrib(egl, configs[i], EGL_ALPHA_SIZE, &alpha);
            if ((!kms && alpha == 0) || (kms && static_cast<uint32_t>(visual) == GBM_FORMAT_XRGB8888)) {
                config = configs[i]; break;
            }
        }
        require(config, "EGL opaque scanout config");
        if (!kms) createWindow(config);
        surface = eglCreateWindowSurface(egl, config, kms ? reinterpret_cast<EGLNativeWindowType>(gbmSurface) : window, nullptr);
        const EGLint contextAttributes[]{EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        context = eglCreateContext(egl, config, EGL_NO_CONTEXT, contextAttributes);
        require(surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT && eglMakeCurrent(egl, surface, surface, context),
                "EGL GPU rendering context");
        eglSwapInterval(egl, 1);
        const char *renderer = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
        require(renderer && !std::strstr(renderer, "llvmpipe") && !std::strstr(renderer, "softpipe"),
                "Splash requires hardware GPU rendering");
        std::fprintf(stderr, "Kernarium GPU splash (%s): %s\n", kms ? "KMS" : "X11", renderer);
    }
    void release()
    {
        if (egl != EGL_NO_DISPLAY) {
            eglMakeCurrent(egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (context != EGL_NO_CONTEXT) eglDestroyContext(egl, context);
            if (surface != EGL_NO_SURFACE) eglDestroySurface(egl, surface);
            eglTerminate(egl);
        }
        if (front) gbm_surface_release_buffer(gbmSurface, front);
        if (gbmSurface) gbm_surface_destroy(gbmSurface);
        if (gbm) gbm_device_destroy(gbm);
        if (display) {
            if (window) XDestroyWindow(display, window);
            if (colormap) XFreeColormap(display, colormap);
            XCloseDisplay(display);
        }
        if (fd >= 0) { drmDropMaster(fd); close(fd); }
    }
};

struct State {
    std::string message = "Starting Steam";
    double fraction = -1;
    bool operator==(const State &) const = default;
};
// The producer atomically publishes two UTF-8 lines: fraction (-1 = busy), message.
static State readState(const std::string &path)
{
    State state;
    std::ifstream input(path);
    std::string fraction, message;
    if (std::getline(input, fraction) && std::getline(input, message) && message.size() <= 1024) {
        try {
            size_t used = 0;
            const double value = std::stod(fraction, &used);
            if (used == fraction.size() && std::isfinite(value)) {
                state.fraction = value < 0 ? -1 : std::clamp(value, 0.0, 1.0);
                state.message = message;
            }
        } catch (const std::exception &) { }
    }
    return state;
}
struct Vertex { float x, y, u, v, r, g, b; };
using Color = std::array<float, 3>;
class Renderer {
public:
    Renderer(int width, int height) : width(width), height(height), scale(std::min(width / 1280.0f, height / 720.0f))
    {
        const char *vertexSource = R"(
            attribute vec2 position; attribute vec2 uv; attribute vec3 color;
            uniform vec2 resolution; uniform vec2 origin; uniform vec2 extent;
            varying vec2 texcoord; varying vec3 tint;
            void main() {
                vec2 pixel = origin + position * extent;
                gl_Position = vec4(pixel.x / resolution.x * 2.0 - 1.0, 1.0 - pixel.y / resolution.y * 2.0, 0.0, 1.0);
                texcoord = uv; tint = color;
            })";
        const char *fragmentSource = R"(
            precision mediump float;
            uniform sampler2D atlas; varying vec2 texcoord; varying vec3 tint;
            void main() { gl_FragColor = vec4(tint, texture2D(atlas, texcoord).a); })";
        const GLuint vertex = shader(GL_VERTEX_SHADER, vertexSource), fragment = shader(GL_FRAGMENT_SHADER, fragmentSource);
        program = glCreateProgram();
        glAttachShader(program, vertex); glAttachShader(program, fragment);
        glBindAttribLocation(program, 0, "position"); glBindAttribLocation(program, 1, "uv"); glBindAttribLocation(program, 2, "color");
        glLinkProgram(program); glDeleteShader(vertex); glDeleteShader(fragment);
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        require(linked, "GPU splash shader link");
        glUseProgram(program);
        glUniform2f(glGetUniformLocation(program, "resolution"), width, height);
        origin = glGetUniformLocation(program, "origin"); extent = glGetUniformLocation(program, "extent");
        glUniform1i(glGetUniformLocation(program, "atlas"), 0);
        glViewport(0, 0, width, height);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        makeAtlas();
        glGenBuffers(1, &sceneBuffer); glGenBuffers(1, &barBuffer);
        std::vector<Vertex> bar;
        quad(bar, 0, 0, 1, 1, 2047.5f / 2048, 1023.5f / 1024, 2047.5f / 2048, 1023.5f / 1024, blue);
        glBindBuffer(GL_ARRAY_BUFFER, barBuffer);
        glBufferData(GL_ARRAY_BUFFER, bar.size() * sizeof(Vertex), bar.data(), GL_STATIC_DRAW);
    }
    ~Renderer()
    {
        glDeleteBuffers(1, &sceneBuffer); glDeleteBuffers(1, &barBuffer);
        glDeleteTextures(1, &atlas); glDeleteProgram(program);
    }
    void draw(const State &state, double elapsed)
    {
        if (state.message != message) {
            message = state.message;
            std::vector<Vertex> scene;
            text(scene, "K", 82 * scale, height * 0.40f, blue);
            text(scene, "Kernarium", 38 * scale, height * 0.50f, {0.94f, 0.96f, 1.0f});
            const float barWidth = std::min(520 * scale, width * 0.8f);
            quad(scene, (width - barWidth) / 2, height * 0.61f, barWidth, 4 * scale,
                 2047.5f / 2048, 1023.5f / 1024, 2047.5f / 2048, 1023.5f / 1024, {0.13f, 0.17f, 0.24f});
            std::string remaining = message;
            for (int line = 0; !remaining.empty() && line < 4; ++line) {
                size_t end = remaining.size() <= 74 ? remaining.size() : remaining.rfind(' ', 74);
                if (end == std::string::npos || end == 0) end = std::min<size_t>(74, remaining.size());
                text(scene, remaining.substr(0, end), 17 * scale, height * 0.69f + line * 25 * scale, {0.66f, 0.73f, 0.83f});
                remaining.erase(0, end);
                if (!remaining.empty() && remaining.front() == ' ') remaining.erase(0, 1);
            }
            sceneCount = scene.size();
            glBindBuffer(GL_ARRAY_BUFFER, sceneBuffer);
            glBufferData(GL_ARRAY_BUFFER, scene.size() * sizeof(Vertex), scene.data(), GL_STATIC_DRAW);
        }
        glClearColor(0.035f, 0.047f, 0.078f, 1); glClear(GL_COLOR_BUFFER_BIT);
        glUniform2f(origin, 0, 0); glUniform2f(extent, 1, 1);
        drawBuffer(sceneBuffer, sceneCount);
        const float barWidth = std::min(520 * scale, width * 0.8f);
        const float offset = state.fraction < 0 ? (std::sin(elapsed * 1.5) + 1) * 0.375 : 0;
        glUniform2f(origin, (width - barWidth) / 2 + barWidth * offset, height * 0.61f);
        glUniform2f(extent, barWidth * (state.fraction < 0 ? 0.25 : state.fraction), 4 * scale);
        drawBuffer(barBuffer, 6);
        require(glGetError() == GL_NO_ERROR, "GPU splash rendering");
    }
private:
    struct Glyph { cairo_text_extents_t metrics{}; float u = 0, v = 0; };
    std::array<Glyph, 96> glyphs{};
    static constexpr Color blue{0.39f, 0.71f, 1.0f};
    int width, height;
    float scale;
    GLuint program = 0, atlas = 0, sceneBuffer = 0, barBuffer = 0;
    GLint origin = -1, extent = -1;
    GLsizei sceneCount = 0;
    std::string message;
    static GLuint shader(GLenum type, const char *source)
    {
        GLuint result = glCreateShader(type);
        glShaderSource(result, 1, &source, nullptr); glCompileShader(result);
        GLint compiled = 0;
        glGetShaderiv(result, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            char log[2048]{}; glGetShaderInfoLog(result, sizeof(log), nullptr, log);
            std::fprintf(stderr, "%s\n", log);
            glDeleteShader(result);
            throw std::runtime_error("GPU splash shader compilation");
        }
        return result;
    }
    void makeAtlas()
    {
        // Rasterize glyphs once. All per-frame drawing and animation stays on GPU.
        cairo_surface_t *image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 2048, 1024);
        require(cairo_surface_status(image) == CAIRO_STATUS_SUCCESS, "Glyph atlas allocation");
        cairo_t *cr = cairo_create(image);
        cairo_select_font_face(cr, "DejaVu Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 96); cairo_set_source_rgba(cr, 1, 1, 1, 1);
        for (size_t i = 0; i < glyphs.size(); ++i) {
            const std::string character = i == 95 ? "•" : std::string(1, static_cast<char>(i + 32));
            auto &glyph = glyphs[i];
            cairo_text_extents(cr, character.c_str(), &glyph.metrics);
            const float x = (i % 16) * 128 + 8, y = (i / 16) * 128 + 8;
            glyph.u = x / 2048; glyph.v = y / 1024;
            cairo_move_to(cr, x - glyph.metrics.x_bearing, y - glyph.metrics.y_bearing);
            cairo_show_text(cr, character.c_str());
        }
        cairo_rectangle(cr, 2047, 1023, 1, 1); cairo_fill(cr);
        cairo_surface_flush(image);
        glGenTextures(1, &atlas); glBindTexture(GL_TEXTURE_2D, atlas);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Only alpha is sampled, so native Cairo RGB channel order is irrelevant.
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2048, 1024, 0, GL_RGBA, GL_UNSIGNED_BYTE, cairo_image_surface_get_data(image));
        cairo_destroy(cr); cairo_surface_destroy(image);
    }
    static void quad(std::vector<Vertex> &vertices, float x, float y, float w, float h,
                     float u, float v, float right, float bottom, Color color)
    {
        const Vertex a{x, y, u, v, color[0], color[1], color[2]}, b{x+w, y, right, v, color[0], color[1], color[2]},
                     c{x+w, y+h, right, bottom, color[0], color[1], color[2]}, d{x, y+h, u, bottom, color[0], color[1], color[2]};
        vertices.insert(vertices.end(), {a, b, c, a, c, d});
    }
    void text(std::vector<Vertex> &vertices, const std::string &value, float size, float y, Color color)
    {
        std::vector<size_t> indices;
        for (size_t i = 0; i < value.size(); ++i) {
            unsigned char ch = value[i];
            if (value.compare(i, 3, "•") == 0) { indices.push_back(95); i += 2; }
            else if (ch >= 32 && ch <= 126) indices.push_back(ch - 32);
            else if ((ch & 0xc0) != 0x80) indices.push_back('?' - 32);
        }
        const float factor = size / 96;
        float length = 0;
        for (size_t i : indices) length += glyphs[i].metrics.x_advance * factor;
        float x = (width - length) / 2;
        for (size_t i : indices) {
            const auto &glyph = glyphs[i];
            const auto &m = glyph.metrics;
            quad(vertices, x + m.x_bearing * factor, y + m.y_bearing * factor, m.width * factor, m.height * factor,
                 glyph.u, glyph.v, glyph.u + m.width / 2048, glyph.v + m.height / 1024, color);
            x += m.x_advance * factor;
        }
    }
    static void drawBuffer(GLuint buffer, GLsizei count)
    {
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        for (GLuint i = 0; i < 3; ++i) glEnableVertexAttribArray(i);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void *>(2 * sizeof(float)));
        glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void *>(4 * sizeof(float)));
        glDrawArrays(GL_TRIANGLES, 0, count);
    }
};
int main(int argc, char **argv)
{
    try {
        bool framebuffer = false;
        std::string status = "/run/kernarium-progress.status";
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--framebuffer") == 0) framebuffer = true;
            else if (std::strcmp(argv[i], "--status") == 0 && i + 1 < argc) status = argv[++i];
            else throw std::runtime_error("Usage: kernarium-screen [--framebuffer] [--status PATH]");
        }
        std::signal(SIGTERM, stop); std::signal(SIGINT, stop);
        DisplayCanvas canvas(framebuffer);
        Renderer renderer(canvas.width, canvas.height);
        const auto start = Clock::now();
        const auto deadline = start + std::chrono::seconds(framebuffer ? 600 : 3600);
        auto next = start, nextStatus = start;
        State state;
        std::optional<State> presented;
        while (!stopped && Clock::now() < deadline) {
            if (Clock::now() >= nextStatus) {
                if (framebuffer && std::filesystem::exists("/run/kernarium-stop-splash")) break;
                state = readState(status);
                nextStatus = Clock::now() + std::chrono::milliseconds(200);
            }
            // A determinate, unchanged frame remains in the compositor/scanout.
            // Busy animation changes only shader uniforms and never reads pixels.
            if (state.fraction < 0 || !presented || *presented != state) {
                renderer.draw(state, std::chrono::duration<double>(Clock::now() - start).count());
                canvas.present();
                presented = state;
            }
            next = std::max(next + std::chrono::milliseconds(state.fraction < 0 ? 33 : 200), Clock::now());
            std::this_thread::sleep_until(next);
        }
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "Kernarium splash: %s\n", error.what());
        return 1;
    }
}
