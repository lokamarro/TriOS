// SPDX-License-Identifier: GPL-2.0-or-later

#include "host_gl_provider.hpp"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

// EGL display creation must precede libepoxy's version discovery. The packaged
// provider exports these platform bootstrap entry points directly; all later
// EGL and GL dispatch continues through libepoxy.
#undef eglGetError
extern "C" EGLint eglGetError(void);
#undef eglGetPlatformDisplay
extern "C" EGLDisplay eglGetPlatformDisplay(
    EGLenum platform,
    void* nativeDisplay,
    const EGLAttrib* attributes);
#undef eglGetProcAddress
extern "C" __eglMustCastToProperFunctionPointerType eglGetProcAddress(
    const char* name);
#undef eglQueryString
extern "C" const char* eglQueryString(EGLDisplay display, EGLint name);

extern "C" {
#include <virglrenderer.h>
}

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace uml::gpu {
namespace {

constexpr EGLenum kClientApi = EGL_OPENGL_API;
constexpr EGLint kRenderableType = EGL_OPENGL_BIT;
#if UML_GPU_SOFTWARE_RENDERER
// The iOS Simulator has no Metal 4 device, so its provider is a Mesa software
// rasterizer on the CPU: llvmpipe, which Mesa prefers once software rendering
// is forced, or softpipe where GALLIUM_DRIVER names it (the Gallium drivers of
// dependencies/build-apple-gpu.sh). Mesa's surfaceless platform reaches either
// through its software device.
constexpr const char* kProviderName = "software rasterizer";
constexpr const char* kDriverOverrideVariable = "LIBGL_ALWAYS_SOFTWARE";
constexpr const char* kDriverOverride = "true";
constexpr const char* kProviderMarker =
    "Virgl software rasterizer provider: software\n";
#else
constexpr const char* kProviderName = "Zink/KosmicKrisp";
constexpr const char* kDriverOverrideVariable = "MESA_LOADER_DRIVER_OVERRIDE";
constexpr const char* kDriverOverride = "zink";
constexpr const char* kProviderMarker =
    "Virgl Zink/KosmicKrisp provider: hardware\n";
#endif

void reportEglFailure(const char* operation) noexcept
{
    std::fprintf(
        stderr,
        "%s EGL %s failed: 0x%04x\n",
        kProviderName,
        operation,
        static_cast<unsigned int>(eglGetError()));
}

// The rendering API an EGL thread is bound to is per-thread state whose
// default is EGL_OPENGL_ES_API, so binding it once where the display is
// created leaves every other thread asking for the wrong client API. vrend
// creates a context's shared sub-context on whichever thread services the
// guest queue, and a GLES request cannot share with this provider's desktop
// contexts: EGL answers EGL_BAD_MATCH. Select the API on each thread that
// reaches EGL instead.
bool bindClientApi() noexcept
{
    if (eglBindAPI(kClientApi) == EGL_TRUE)
        return true;
    reportEglFailure("API binding");
    return false;
}

bool hasExtension(const char* extensions, std::string_view name) noexcept
{
    if (extensions == nullptr)
        return false;
    std::string_view remaining(extensions);
    while (!remaining.empty()) {
        const auto end = remaining.find(' ');
        if (remaining.substr(0, end) == name)
            return true;
        if (end == std::string_view::npos)
            break;
        remaining.remove_prefix(end + 1);
    }
    return false;
}

// The device this provider renders on, named explicitly so that its display is
// its own. EGL keeps one display per platform, native display and attribute
// list, and does not count eglInitialize calls on it: any other client in the
// process that opens the default surfaceless display gets the same display,
// and its eglTerminate would retire it under every guest context. A display
// keyed by an explicit device is one such a client never obtains. Apple hosts
// have no DRM devices, so Mesa's surfaceless platform renders zink on its
// software device, and that is the device to name.
EGLDeviceEXT softwareDevice()
{
    const char* clientExtensions =
        eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!hasExtension(clientExtensions, "EGL_EXT_device_enumeration") ||
        !hasExtension(clientExtensions, "EGL_EXT_explicit_device")) {
        std::fprintf(
            stderr,
            "%s EGL offers no explicit device selection\n",
            kProviderName);
        return EGL_NO_DEVICE_EXT;
    }
    const auto queryDevices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(
        eglGetProcAddress("eglQueryDevicesEXT"));
    const auto queryDeviceString =
        reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(
            eglGetProcAddress("eglQueryDeviceStringEXT"));
    EGLint count = 0;
    if (queryDevices == nullptr || queryDeviceString == nullptr ||
        queryDevices(0, nullptr, &count) != EGL_TRUE || count <= 0) {
        reportEglFailure("device enumeration");
        return EGL_NO_DEVICE_EXT;
    }
    std::vector<EGLDeviceEXT> devices(static_cast<std::size_t>(count));
    if (queryDevices(count, devices.data(), &count) != EGL_TRUE) {
        reportEglFailure("device enumeration");
        return EGL_NO_DEVICE_EXT;
    }
    devices.resize(static_cast<std::size_t>(std::max(count, 0)));
    for (const auto device : devices) {
        if (hasExtension(
                queryDeviceString(device, EGL_EXTENSIONS),
                "EGL_MESA_device_software")) {
            return device;
        }
    }
    std::fprintf(stderr, "%s EGL has no software device\n", kProviderName);
    return EGL_NO_DEVICE_EXT;
}

std::string glString(GLenum name)
{
    const auto* value = glGetString(name);
    return value == nullptr ? std::string {} :
        std::string(reinterpret_cast<const char*>(value));
}

} // namespace

HostGlProvider::~HostGlProvider()
{
    std::lock_guard lock(mutex_);
    void* publishedDisplay = display_.load(std::memory_order_acquire);
    if (publishedDisplay == nullptr)
        return;
    const auto eglDisplay = static_cast<EGLDisplay>(publishedDisplay);
    // eglMakeCurrent releases the context of the calling thread's bound API.
    (void)bindClientApi();
    (void)eglMakeCurrent(
        eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    (void)eglTerminate(eglDisplay);
    display_.store(nullptr, std::memory_order_release);
    config_ = nullptr;
    sharedContext_ = nullptr;
}

void* HostGlProvider::display() noexcept
{
    try {
        std::lock_guard lock(mutex_);
        if (!bindClientApi())
            return nullptr;
        if (void* publishedDisplay =
                display_.load(std::memory_order_acquire)) {
            return publishedDisplay;
        }
        // Mesa selects its Gallium driver from the process environment before
        // the display is created, and the packaged provider offers exactly one
        // driver. Set it here rather than inheriting an ambient choice, but
        // leave a value that already names it alone: another component may
        // hold a copy of the environment taken earlier, and an application
        // that has one sets this value at launch for that reason.
        const char* driverOverride = std::getenv(kDriverOverrideVariable);
        if (driverOverride == nullptr ||
            std::string_view(driverOverride) != kDriverOverride) {
            ::setenv(kDriverOverrideVariable, kDriverOverride, 1);
        }
        const EGLDeviceEXT device = softwareDevice();
        if (device == EGL_NO_DEVICE_EXT)
            return nullptr;
        const EGLAttrib displayAttributes[] {
            EGL_DEVICE_EXT, reinterpret_cast<EGLAttrib>(device),
            EGL_NONE,
        };
        const auto eglDisplay = eglGetPlatformDisplay(
            EGL_PLATFORM_SURFACELESS_MESA, nullptr, displayAttributes);
        if (eglDisplay == EGL_NO_DISPLAY) {
            reportEglFailure("display creation");
            return nullptr;
        }
        EGLint major = 0;
        EGLint minor = 0;
        if (eglInitialize(eglDisplay, &major, &minor) != EGL_TRUE) {
            reportEglFailure("initialization");
            (void)eglTerminate(eglDisplay);
            return nullptr;
        }
        const EGLint configAttributes[] {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, kRenderableType,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, 8,
            EGL_NONE,
        };
        EGLConfig eglConfig = nullptr;
        EGLint count = 0;
        if (eglChooseConfig(
                eglDisplay,
                configAttributes,
                &eglConfig,
                1,
                &count) != EGL_TRUE ||
            count != 1 || eglConfig == nullptr) {
            reportEglFailure("configuration selection");
            (void)eglTerminate(eglDisplay);
            return nullptr;
        }
        config_ = eglConfig;
        std::fputs(kProviderMarker, stderr);
        display_.store(eglDisplay, std::memory_order_release);
        return eglDisplay;
    } catch (...) {
        return nullptr;
    }
}

HostGlProvider::ContextProbe HostGlProvider::probeContext(
    std::uint32_t major,
    std::uint32_t minor) noexcept
{
    ContextProbe result {
        .requestedMajor = major,
        .requestedMinor = minor,
    };
    if (display() == nullptr) {
        result.eglError = static_cast<std::uint32_t>(eglGetError());
        return result;
    }

    std::lock_guard lock(mutex_);
    const auto eglDisplay = static_cast<EGLDisplay>(
        display_.load(std::memory_order_acquire));
    const auto eglConfig = static_cast<EGLConfig>(config_);
    const EGLDisplay previousDisplay = eglGetCurrentDisplay();
    const EGLContext previousContext = eglGetCurrentContext();
    const EGLSurface previousDraw = eglGetCurrentSurface(EGL_DRAW);
    const EGLSurface previousRead = eglGetCurrentSurface(EGL_READ);
    EGLContext context = EGL_NO_CONTEXT;
    auto restore = [&] {
        if (previousDisplay != EGL_NO_DISPLAY) {
            (void)eglMakeCurrent(
                previousDisplay,
                previousDraw,
                previousRead,
                previousContext);
        } else {
            (void)eglMakeCurrent(
                eglDisplay,
                EGL_NO_SURFACE,
                EGL_NO_SURFACE,
                EGL_NO_CONTEXT);
        }
        if (context != EGL_NO_CONTEXT)
            (void)eglDestroyContext(eglDisplay, context);
    };

    try {
        const EGLint attributes[] {
            EGL_CONTEXT_MAJOR_VERSION_KHR, static_cast<EGLint>(major),
            EGL_CONTEXT_MINOR_VERSION_KHR, static_cast<EGLint>(minor),
            EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
            EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
            EGL_NONE,
        };
        context = eglCreateContext(
            eglDisplay, eglConfig, EGL_NO_CONTEXT, attributes);
        if (context == EGL_NO_CONTEXT) {
            result.eglError = static_cast<std::uint32_t>(eglGetError());
            return result;
        }
        if (eglMakeCurrent(
                eglDisplay,
                EGL_NO_SURFACE,
                EGL_NO_SURFACE,
                context) != EGL_TRUE) {
            result.eglError = static_cast<std::uint32_t>(eglGetError());
            restore();
            return result;
        }

        GLint actualMajor = 0;
        GLint actualMinor = 0;
        GLint extensionCount = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &actualMajor);
        glGetIntegerv(GL_MINOR_VERSION, &actualMinor);
        glGetIntegerv(GL_NUM_EXTENSIONS, &extensionCount);
        result.actualMajor = actualMajor < 0 ? 0U :
            static_cast<std::uint32_t>(actualMajor);
        result.actualMinor = actualMinor < 0 ? 0U :
            static_cast<std::uint32_t>(actualMinor);
        result.vendor = glString(GL_VENDOR);
        result.renderer = glString(GL_RENDERER);
        result.version = glString(GL_VERSION);
        result.shadingLanguageVersion =
            glString(GL_SHADING_LANGUAGE_VERSION);
        if (extensionCount > 0) {
            result.extensions.reserve(
                static_cast<std::size_t>(extensionCount));
            for (GLint index = 0; index < extensionCount; ++index) {
                const auto* extension = glGetStringi(
                    GL_EXTENSIONS, static_cast<GLuint>(index));
                if (extension != nullptr) {
                    result.extensions.emplace_back(
                        reinterpret_cast<const char*>(extension));
                }
            }
            std::ranges::sort(result.extensions);
            result.extensions.erase(
                std::unique(
                    result.extensions.begin(), result.extensions.end()),
                result.extensions.end());
        }
        result.created = true;
        result.eglError = EGL_SUCCESS;
        restore();
        return result;
    } catch (...) {
        result = ContextProbe {
            .requestedMajor = major,
            .requestedMinor = minor,
            .eglError = EGL_BAD_ALLOC,
        };
        restore();
        return result;
    }
}

std::uint64_t HostGlProvider::releaseCachedMemory() noexcept
{
    // No display, no driver screen: nothing is cached.
    if (display_.load(std::memory_order_acquire) == nullptr)
        return 0;
    void* entry = releaseCachedMemory_.load(std::memory_order_acquire);
    if (entry == nullptr) {
        entry = reinterpret_cast<void*>(
            eglGetProcAddress("eglReleaseCachedMemoryMESA"));
        if (entry == nullptr)
            return 0;
        releaseCachedMemory_.store(entry, std::memory_order_release);
    }
    const EGLint released =
        reinterpret_cast<EGLint (*)(void)>(entry)();
    return released > 0 ? static_cast<std::uint64_t>(released) : 0;
}

void* HostGlProvider::createContext(
    const virgl_renderer_gl_ctx_param& parameters) noexcept
{
    try {
        // vrend probes desktop versions from 4.6 downwards and never asks for
        // a compatibility profile on this provider.
        if (parameters.version != 2 || parameters.compat_ctx ||
            parameters.major_ver < 3 ||
            parameters.major_ver > 4) {
            return nullptr;
        }
        // display() also binds the calling thread to the desktop API, which a
        // core profile mask needs: it is a bad attribute under OpenGL ES.
        if (display() == nullptr)
            return nullptr;
        std::lock_guard lock(mutex_);
        const auto eglDisplay = static_cast<EGLDisplay>(
            display_.load(std::memory_order_acquire));
        const auto eglConfig = static_cast<EGLConfig>(config_);
        EGLContext shared = EGL_NO_CONTEXT;
        if (parameters.shared) {
            shared = static_cast<EGLContext>(sharedContext_);
            if (shared == EGL_NO_CONTEXT)
                return nullptr;
        }
        const EGLint attributes[] {
            EGL_CONTEXT_MAJOR_VERSION_KHR,
            static_cast<EGLint>(parameters.major_ver),
            EGL_CONTEXT_MINOR_VERSION_KHR,
            static_cast<EGLint>(parameters.minor_ver),
            EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR,
            EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
            EGL_NONE,
        };
        const auto context = eglCreateContext(
            eglDisplay, eglConfig, shared, attributes);
        if (context == EGL_NO_CONTEXT) {
            reportEglFailure("context creation");
            return nullptr;
        }
        if (!parameters.shared)
            sharedContext_ = context;
        return context;
    } catch (...) {
        return nullptr;
    }
}

void HostGlProvider::destroyContext(void* context) noexcept
{
    try {
        if (context == nullptr)
            return;
        std::lock_guard lock(mutex_);
        if (sharedContext_ == context)
            sharedContext_ = nullptr;
        void* publishedDisplay = display_.load(std::memory_order_acquire);
        if (publishedDisplay != nullptr) {
            (void)eglDestroyContext(
                static_cast<EGLDisplay>(publishedDisplay),
                static_cast<EGLContext>(context));
        }
    } catch (...) {
    }
}

int HostGlProvider::makeCurrent(void* context) noexcept
{
    void* publishedDisplay = display_.load(std::memory_order_acquire);
    if (publishedDisplay == nullptr)
        return -EIO;
    if (!bindClientApi())
        return -EIO;
    if (eglMakeCurrent(
            static_cast<EGLDisplay>(publishedDisplay),
            EGL_NO_SURFACE,
            EGL_NO_SURFACE,
            static_cast<EGLContext>(context)) == EGL_TRUE) {
        return 0;
    }
    reportEglFailure("make-current");
    return -EIO;
}

} // namespace uml::gpu
