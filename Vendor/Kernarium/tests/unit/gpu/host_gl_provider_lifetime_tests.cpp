// SPDX-License-Identifier: GPL-2.0-or-later

#include "host_gl_provider.hpp"
#include "host_gl_test_support.hpp"
#include "protocol.hpp"
#include "require.hpp"
#include "virgl_renderer.hpp"
#include "wait_until.hpp"

#include <epoxy/egl.h>
#include <epoxy/gl.h>

// Another client of the process's EGL library opens the default surfaceless
// display the way the provider bootstraps its own, before libepoxy knows of any
// display.
#undef eglGetPlatformDisplay
extern "C" EGLDisplay eglGetPlatformDisplay(
    EGLenum platform,
    void* nativeDisplay,
    const EGLAttrib* attributes);

extern "C" {
#include <virglrenderer.h>
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using uml::gpu::protocol::virgl2CapsetId;
using uml::gpu::testing::glContextParameters;
using uml::gpu::testing::pipeTexture3d;
using uml::gpu::testing::virglBindRenderTarget;
using uml::gpu::testing::virglBindSamplerView;
using uml::gpu::testing::virglFormatB8g8r8a8Unorm;
using uml::testing::require;

// The highest desktop version vrend probes, and the one it then asks for again
// for every sub-context it creates.
constexpr int kProbedMajor = 4;
constexpr int kProbedMinor = 6;

// A component that copies the process environment early leaves that copy
// pointing at freed memory once the environment is rewritten, so an
// application with one sets Mesa's driver override at launch. The provider
// must leave such a value where it is, and still set the override where it
// is missing or names another driver.
void providerLeavesAMatchingDriverOverrideInPlace()
{
    constexpr std::string_view variable = "MESA_LOADER_DRIVER_OVERRIDE";
    static char launchEntry[] = "MESA_LOADER_DRIVER_OVERRIDE=zink";
    require(::putenv(launchEntry) == 0);
    const char* launchValue = std::getenv(variable.data());
    require(launchValue == launchEntry + variable.size() + 1);
    {
        uml::gpu::HostGlProvider provider;
        require(provider.display() != nullptr);
    }
    require(std::getenv(variable.data()) == launchValue);

    require(::setenv(variable.data(), "softpipe", 1) == 0);
    {
        uml::gpu::HostGlProvider provider;
        require(provider.display() != nullptr);
    }
    require(std::string_view(std::getenv(variable.data())) == "zink");

    require(::unsetenv(variable.data()) == 0);
    {
        uml::gpu::HostGlProvider provider;
        require(provider.display() != nullptr);
    }
    const char* value = std::getenv(variable.data());
    require(value != nullptr && std::string_view(value) == "zink");
}

void providerOwnsOneSharedContextTopology()
{
    uml::gpu::HostGlProvider provider;
    require(provider.display() != nullptr);
    require(provider.display() == provider.display());

    auto parameters = glContextParameters(kProbedMajor, kProbedMinor, true);
    require(provider.createContext(parameters) == nullptr);

    parameters.shared = false;
    void* root = provider.createContext(parameters);
    require(root != nullptr);
    require(provider.makeCurrent(root) == 0);

    parameters.shared = true;
    void* shared = provider.createContext(parameters);
    require(shared != nullptr);
    require(provider.makeCurrent(shared) == 0);
    require(provider.makeCurrent(nullptr) == 0);

    provider.destroyContext(shared);
    provider.destroyContext(root);
}

// vrend creates a virgl context's shared sub-context on whichever thread
// services the guest queue, never on the thread that created the display. The
// EGL rendering API a thread is bound to is per-thread state defaulting to
// GLES, so a provider that binds its desktop API only once answers
// EGL_BAD_MATCH there and every guest GL context creation fails.
void providerCreatesSharedContextsOnAnyThread()
{
    uml::gpu::HostGlProvider provider;
    auto parameters = glContextParameters(kProbedMajor, kProbedMinor);
    void* root = provider.createContext(parameters);
    require(root != nullptr);
    require(provider.makeCurrent(root) == 0);

    parameters.shared = true;
    void* shared = nullptr;
    int current = -1;
    std::thread consumer([&] {
        shared = provider.createContext(parameters);
        if (shared != nullptr)
            current = provider.makeCurrent(shared);
    });
    consumer.join();
    require(shared != nullptr);
    require(current == 0);

    provider.destroyContext(shared);
    require(provider.makeCurrent(nullptr) == 0);
    provider.destroyContext(root);
}

void providerRejectsUnsupportedContexts()
{
    uml::gpu::HostGlProvider provider;
    auto parameters = glContextParameters(3, 0, false, 1);
    require(provider.createContext(parameters) == nullptr);
    parameters.compat_ctx = 0;
    parameters.major_ver = 1;
    require(provider.createContext(parameters) == nullptr);
    // The provider serves desktop GL 3.x and 4.x core contexts.
    parameters.major_ver = kProbedMajor + 1;
    require(provider.createContext(parameters) == nullptr);
    parameters.major_ver = 3;
    parameters.version = 1;
    require(provider.createContext(parameters) == nullptr);
}

void providerReportsExplicitEsVersionRequests()
{
    uml::gpu::HostGlProvider provider;
    for (const auto [major, minor] : {
             std::pair {3U, 0U},
             std::pair {3U, 1U},
             std::pair {3U, 2U}}) {
        const auto probe = provider.probeContext(major, minor);
        require(probe.requestedMajor == major);
        require(probe.requestedMinor == minor);
        if (!probe.created) {
            require(probe.eglError != 0);
            continue;
        }
        require(
            probe.actualMajor > major ||
            (probe.actualMajor == major && probe.actualMinor >= minor));
        require(!probe.vendor.empty());
        require(!probe.renderer.empty());
        require(!probe.version.empty());
        require(!probe.shadingLanguageVersion.empty());
        require(std::ranges::is_sorted(probe.extensions));
        require(
            std::adjacent_find(
                probe.extensions.begin(), probe.extensions.end()) ==
            probe.extensions.end());
    }
}

void rendererRetiresFencesThroughTheExternalProvider()
{
    uml::gpu::VirglRenderer renderer;
    require(renderer.createFenceAndWait(0, 0, 1, nullptr) == 0);
    require(renderer.createContext(1, 1, "fence-test") == 0);
    require(renderer.createContextFence(1, 0, 1) == 0);
    int status = EBUSY;
    require(
        uml::testing::waitUntil(
            [&] {
                require(renderer.pollContextFences(1) == 0);
                status = renderer.contextFenceStatus(1, 0, 1);
                return status != EBUSY;
            },
            std::chrono::seconds(2),
            std::chrono::microseconds(100)),
        "the context fence was never retired");
    require(status == 0);
    renderer.destroyContext(1);
}

// Stands in for any other client of the process's EGL library, such as the
// application's own graphics self-test: it opens the default surfaceless
// display, initializes it and terminates it again. EGL does not count
// initializations, so that terminate retires the display for everyone it was
// handed to.
void terminateTheDefaultSurfacelessDisplay()
{
    const EGLDisplay other = eglGetPlatformDisplay(
        EGL_PLATFORM_SURFACELESS_MESA, nullptr, nullptr);
    require(other != EGL_NO_DISPLAY);
    EGLint major = 0;
    EGLint minor = 0;
    require(eglInitialize(other, &major, &minor) == EGL_TRUE);
    require(eglTerminate(other) == EGL_TRUE);
}

// The provider's display is its own. Once another client had terminated the
// default surfaceless display, which the provider used to share, every guest
// context creation failed with EGL_NOT_INITIALIZED and the running guest lost
// OpenGL.
void providerKeepsItsDisplayWhenAnotherClientTerminatesTheDefaultOne()
{
    uml::gpu::HostGlProvider provider;
    auto parameters = glContextParameters(kProbedMajor, kProbedMinor);
    void* root = provider.createContext(parameters);
    require(root != nullptr);
    require(provider.display() != eglGetPlatformDisplay(
        EGL_PLATFORM_SURFACELESS_MESA, nullptr, nullptr));

    terminateTheDefaultSurfacelessDisplay();

    require(provider.makeCurrent(root) == 0);
    const auto* renderer =
        reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    require(renderer != nullptr);
    // Naming the device must still reach the packaged zink driver.
    require(std::string_view(renderer).find("zink") != std::string_view::npos);
    parameters.shared = true;
    void* shared = provider.createContext(parameters);
    require(shared != nullptr);
    require(provider.makeCurrent(shared) == 0);
    require(provider.makeCurrent(nullptr) == 0);
    provider.destroyContext(shared);
    provider.destroyContext(root);
}

void rendererCreatesContextsAfterAnotherClientTerminatesTheDefaultDisplay()
{
    uml::gpu::VirglRenderer renderer;
    require(renderer.createContext(1, virgl2CapsetId, "before") == 0);
    terminateTheDefaultSurfacelessDisplay();
    require(renderer.createContext(2, virgl2CapsetId, "after") == 0);
    renderer.destroyContext(2);
    renderer.destroyContext(1);
}

// Serves virglrenderer from a provider the way the product's renderer does,
// and can refuse the next GL context vrend asks for, or every one, as a host
// whose GL stack has run out does.
struct RefusingHost {
    uml::gpu::HostGlProvider provider;
    // virglrenderer keeps a pointer to its callbacks until cleanup.
    virgl_renderer_callbacks callbacks {};
    bool refuseNextContext {};
    bool refuseEveryContext {};
    int createdContexts {};
    int refusedContexts {};
    int nullMakeCurrents {};
};

void* refusingHostCreateContext(
    void* cookie,
    int,
    virgl_renderer_gl_ctx_param* parameters)
{
    auto& host = *static_cast<RefusingHost*>(cookie);
    if (host.refuseNextContext || host.refuseEveryContext) {
        host.refuseNextContext = false;
        ++host.refusedContexts;
        return nullptr;
    }
    void* const context = host.provider.createContext(*parameters);
    if (context != nullptr)
        ++host.createdContexts;
    return context;
}

void refusingHostDestroyContext(void* cookie, void* context)
{
    static_cast<RefusingHost*>(cookie)->provider.destroyContext(context);
}

int refusingHostMakeCurrent(void* cookie, int, void* context)
{
    auto& host = *static_cast<RefusingHost*>(cookie);
    if (context == nullptr)
        ++host.nullMakeCurrents;
    return host.provider.makeCurrent(context);
}

void* refusingHostDisplay(void* cookie)
{
    return static_cast<RefusingHost*>(cookie)->provider.display();
}

void refusingHostIgnoresFence(void*, std::uint32_t) {}

void initialiseRenderer(RefusingHost& host)
{
    auto& callbacks = host.callbacks;
    callbacks.version = VIRGL_RENDERER_CALLBACKS_VERSION;
    callbacks.write_fence = &refusingHostIgnoresFence;
    callbacks.create_gl_context = &refusingHostCreateContext;
    callbacks.destroy_gl_context = &refusingHostDestroyContext;
    callbacks.make_current = &refusingHostMakeCurrent;
    callbacks.get_egl_display = &refusingHostDisplay;
    require(virgl_renderer_init(&host, 0, &callbacks) == 0);
}

// Virgl's wire encoding of a one-argument context command, from
// virgl_protocol.h: the command in the low byte, the payload length in dwords
// in the high half.
constexpr std::uint32_t kVirglSetSubContext = 28;
constexpr std::uint32_t kVirglCreateSubContext = 29;

std::array<std::uint32_t, 2> virglCommand(
    std::uint32_t command,
    std::uint32_t argument)
{
    return {command | (1U << 16U), argument};
}

// A guest asks for a sub-context for every GL context it creates, and each
// one needs a host GL context. When the host cannot create one, the request
// must fail and leave the thread with the guest context's own GL context. vrend
// used to make the missing context current, which releases the thread's
// context, and went on issuing GL with none.
void rendererRefusesASubContextWithoutAGlContext()
{
    RefusingHost host;
    initialiseRenderer(host);

    constexpr std::uint32_t context = 1;
    require(virgl_renderer_context_create_with_flags(
        context, virgl2CapsetId, 5, "guest") == 0);
    const int nullMakeCurrents = host.nullMakeCurrents;

    host.refuseNextContext = true;
    auto create = virglCommand(kVirglCreateSubContext, 1);
    require(virgl_renderer_submit_cmd(
        create.data(), context, static_cast<int>(create.size())) != 0);
    require(host.refusedContexts == 1);
    require(host.nullMakeCurrents == nullMakeCurrents);
    require(glGetString(GL_RENDERER) != nullptr);

    // The guest context keeps serving on the sub-context it has.
    auto select = virglCommand(kVirglSetSubContext, 0);
    require(virgl_renderer_submit_cmd(
        select.data(), context, static_cast<int>(select.size())) == 0);
    require(glGetString(GL_RENDERER) != nullptr);

    // A guest context whose first sub-context cannot be created is refused
    // before it runs any GL.
    host.refuseNextContext = true;
    require(virgl_renderer_context_create_with_flags(
        2, virgl2CapsetId, 5, "empty") != 0);
    require(host.refusedContexts == 2);
    require(host.nullMakeCurrents == nullMakeCurrents);

    virgl_renderer_context_destroy(context);
    virgl_renderer_cleanup(&host);
}

// Virgl's blit command and its length in dwords, from virgl_protocol.h.
constexpr std::uint32_t kVirglBlit = 16;
constexpr std::uint32_t kVirglBlitLength = 21;
constexpr std::uint32_t kVolumeSide = 4;

void createVolume(std::uint32_t context, std::uint32_t handle, std::uint32_t depth)
{
    virgl_renderer_resource_create_args arguments {};
    arguments.handle = handle;
    arguments.target = pipeTexture3d;
    arguments.format = virglFormatB8g8r8a8Unorm;
    arguments.bind = virglBindRenderTarget | virglBindSamplerView;
    arguments.width = kVolumeSide;
    arguments.height = kVolumeSide;
    arguments.depth = depth;
    arguments.array_size = 1;
    require(virgl_renderer_resource_create(&arguments, nullptr, 0) == 0);
    virgl_renderer_ctx_attach_resource(
        static_cast<int>(context), static_cast<int>(handle));
}

// A blit that halves a volume's depth, which vrend cannot express as a
// framebuffer blit and draws with its blitter instead.
std::array<std::uint32_t, kVirglBlitLength + 1> depthHalvingBlit(
    std::uint32_t destination,
    std::uint32_t source)
{
    constexpr std::uint32_t rgbaMaskNearest = 0xfU;
    return {
        kVirglBlit | (kVirglBlitLength << 16U),
        rgbaMaskNearest,
        0,
        0,
        destination,
        0,
        virglFormatB8g8r8a8Unorm,
        0,
        0,
        0,
        kVolumeSide,
        kVolumeSide,
        1,
        source,
        0,
        virglFormatB8g8r8a8Unorm,
        0,
        0,
        0,
        kVolumeSide,
        kVolumeSide,
        2,
    };
}

// vrend creates the one GL context its blitter shares on the first blit that
// needs it. A host that cannot create that context must cost the blit, not the
// process: vrend used to abort. The next blit asks for the context again.
void rendererSkipsABlitItHasNoGlContextFor()
{
    RefusingHost host;
    initialiseRenderer(host);

    constexpr std::uint32_t context = 1;
    require(virgl_renderer_context_create_with_flags(
        context, virgl2CapsetId, 5, "guest") == 0);
    constexpr std::uint32_t source = 1;
    constexpr std::uint32_t destination = 2;
    createVolume(context, source, 2);
    createVolume(context, destination, 1);
    auto blit = depthHalvingBlit(destination, source);

    host.refuseEveryContext = true;
    require(virgl_renderer_submit_cmd(
        blit.data(), context, static_cast<int>(blit.size())) == 0);
    host.refuseEveryContext = false;
    require(host.refusedContexts > 0);
    require(glGetString(GL_RENDERER) != nullptr);

    const int refused = host.refusedContexts;
    const int created = host.createdContexts;
    require(virgl_renderer_submit_cmd(
        blit.data(), context, static_cast<int>(blit.size())) == 0);
    require(host.refusedContexts == refused);
    require(host.createdContexts == created + 1);
    require(glGetString(GL_RENDERER) != nullptr);

    virgl_renderer_resource_unref(destination);
    virgl_renderer_resource_unref(source);
    virgl_renderer_context_destroy(context);
    virgl_renderer_cleanup(&host);
}

// A renderer whose own display is terminated under it can create no guest
// context, and a capability query that follows such a failure finds no GL
// context on its thread. Both must fail as errors: the query used to read the
// host renderer string through a null pointer and take the process down.
void rendererFailsCleanlyAfterLosingItsDisplay()
{
    uml::gpu::VirglRenderer renderer;
    require(renderer.createContext(1, virgl2CapsetId, "before") == 0);
    // The renderer's root context is current on the thread that created it.
    const EGLDisplay display = eglGetCurrentDisplay();
    require(display != EGL_NO_DISPLAY);
    require(eglTerminate(display) == EGL_TRUE);

    int created = 0;
    bool capsetRefused = false;
    // vrend serves the guest queue on a thread of its own.
    std::thread guestQueue([&] {
        created = renderer.createContext(2, virgl2CapsetId, "after");
        try {
            (void)renderer.capset(virgl2CapsetId);
        } catch (const std::runtime_error&) {
            capsetRefused = true;
        }
    });
    guestQueue.join();
    require(created != 0);
    require(capsetRefused);
    renderer.destroyContext(1);
}

} // namespace

int main()
{
    providerLeavesAMatchingDriverOverrideInPlace();
    providerRejectsUnsupportedContexts();
    providerReportsExplicitEsVersionRequests();
    providerOwnsOneSharedContextTopology();
    providerCreatesSharedContextsOnAnyThread();
    rendererRetiresFencesThroughTheExternalProvider();
    providerKeepsItsDisplayWhenAnotherClientTerminatesTheDefaultOne();
    rendererCreatesContextsAfterAnotherClientTerminatesTheDefaultDisplay();
    rendererRefusesASubContextWithoutAGlContext();
    rendererSkipsABlitItHasNoGlContextFor();
    // Last: it terminates the display every renderer in this process uses.
    rendererFailsCleanlyAfterLosingItsDisplay();
    std::puts("host GL provider lifetime tests passed");
    return 0;
}
