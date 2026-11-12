// SPDX-License-Identifier: GPL-2.0-or-later

// The renderer's own context, context 0, is where a texture is made and written
// when a command names no guest context: a gralloc allocation, a texture the
// guest uploads before its first command. The host GL driver keeps what that
// work used until the context submits again, so textures the guest has deleted
// stay allocated for as long as the renderer stays quiet, and a guest that
// churns them through context 0 holds the host's budget for nothing.
//
// The renderer tracks context 0 like a guest context, and keeps tracking it
// across a device reset. Once the guest has released the textures and the
// context has been quiet for an idle interval, the service thread's trim
// reaches vrend's own context and the driver releases what it held: the memory
// returns to where it was within a few intervals. The service thread's loop is
// reproduced here: ask how long until a context could be due, wait that long,
// trim.

#include "host_gl_test_support.hpp"
#include "host_gpu_memory.hpp"
#include "require.hpp"
#include "virgl_renderer.hpp"

#include <sys/uio.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using uml::gpu::testing::pipeTexture2d;
using uml::gpu::testing::skipWithoutRenderer;
using uml::gpu::testing::virglBindRenderTarget;
using uml::gpu::testing::virglBindSamplerView;
using uml::gpu::testing::virglFormatR8g8b8a8Unorm;
using uml::testing::require;

constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;
constexpr std::uint32_t textureSize = 1024;
constexpr std::uint32_t bytesPerPixel = 4;
// 48 textures of 4 MiB.
constexpr std::uint32_t textureCount = 48;
// What the driver keeps for itself and never gives back: command pools,
// descriptor pools and the like, a few MiB.
constexpr std::uint64_t slackBytes = 24 * mebibyte;
// The textures held at least this much while they existed.
constexpr std::uint64_t heldBytes = 128 * mebibyte;
constexpr auto idleInterval = 1000ms;
// An idle context is reported twice, an interval apart, and the second trim is
// the one that releases what the first one's flush submitted. The rest is
// slack for a loaded host.
constexpr unsigned intervalsAllowed = 10;

[[nodiscard]] std::uint64_t used() noexcept
{
    return uml::platform::hostGpuAllocatedBytes();
}

[[nodiscard]] std::uint64_t above(std::uint64_t baseline) noexcept
{
    const std::uint64_t now = used();
    return now > baseline ? now - baseline : 0;
}

[[nodiscard]] double mib(std::uint64_t bytes) noexcept
{
    return static_cast<double>(bytes) / mebibyte;
}

} // namespace

int main()
{
    // The interval is the production default, whatever the environment says.
    setenv("UML_GPU_IDLE_TRIM_MS", "1000", 1);
    std::unique_ptr<uml::gpu::VirglRenderer> renderer;
    try {
        renderer = std::make_unique<uml::gpu::VirglRenderer>();
    } catch (const std::exception& error) {
        return skipWithoutRenderer(error);
    }

    // A guest driver resets the device when it probes, and the reset clears
    // the renderer's fence state and the contexts it tracks for idleness: the
    // renderer's own context has to survive it.
    renderer->resetFenceState();
    const std::uint64_t baseline = used();
    std::vector<std::vector<std::uint8_t>> storage(textureCount);
    std::vector<iovec> vectors(textureCount);
    for (std::uint32_t index = 0; index < textureCount; ++index) {
        const std::uint32_t resourceId = 100 + index;
        require(renderer->createResource3d(
                    uml::gpu::VirglRenderer::Resource3d {
                        .resourceId = resourceId,
                        .target = pipeTexture2d,
                        .format = virglFormatR8g8b8a8Unorm,
                        .bind = virglBindRenderTarget | virglBindSamplerView,
                        .width = textureSize,
                        .height = textureSize,
                        .depth = 1,
                        .arraySize = 1,
                        .lastLevel = 0,
                    }) == 0,
            "a texture could not be created");
        storage[index].assign(static_cast<std::size_t>(textureSize) *
                textureSize * bytesPerPixel,
            static_cast<std::uint8_t>(index + 1));
        // The renderer keeps this array, not a copy of it.
        vectors[index] = {
            .iov_base = storage[index].data(),
            .iov_len = storage[index].size(),
        };
        require(renderer->attachBacking(resourceId,
                    std::span<iovec>(&vectors[index], 1)) == 0,
            "the texture's guest storage could not be attached");
        const uml::gpu::VirglRenderer::Box box {
            .width = textureSize,
            .height = textureSize,
            .depth = 1,
        };
        // No guest context: the work runs on the renderer's own context.
        require(renderer->transferResource(true, resourceId, 0, 0,
                    textureSize * bytesPerPixel, 0, box, 0) == 0,
            "a texture could not be written");
    }
    const std::uint64_t held = above(baseline);
    require(held >= heldBytes,
        "the textures took no host GPU memory, so there is nothing to give "
        "back");

    for (std::uint32_t index = 0; index < textureCount; ++index) {
        const std::uint32_t resourceId = 100 + index;
        renderer->detachBacking(resourceId);
        renderer->unrefResource(resourceId);
    }
    const std::uint64_t pinned = above(baseline);

    // What the GPU service thread does: sleep until a context could be due,
    // then trim the ones that are.
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + intervalsAllowed * idleInterval;
    std::uint64_t remaining = pinned;
    bool tracked = false;
    unsigned wakeups = 0;
    while (remaining > slackBytes &&
        std::chrono::steady_clock::now() < deadline) {
        const auto delay = renderer->nextIdleTrimDelay();
        tracked = tracked || delay.has_value();
        std::this_thread::sleep_for(
            std::min<std::chrono::steady_clock::duration>(
                delay.value_or(idleInterval), idleInterval));
        renderer->trimIdleContexts();
        (void)renderer->pollRenderer();
        ++wakeups;
        // The driver releases on its own threads: give it a moment.
        std::this_thread::sleep_for(100ms);
        remaining = above(baseline);
    }
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    std::fprintf(stderr,
        "%u textures of %u MiB held %.1f MiB; %.1f MiB stayed after the guest "
        "released them; %.1f MiB above the start after %.1f s and %u "
        "wakeups of the trim loop (%s context was due)\n",
        textureCount,
        static_cast<unsigned>(textureSize * textureSize * bytesPerPixel /
            mebibyte),
        mib(held), mib(pinned), mib(remaining), seconds, wakeups,
        tracked ? "a" : "no");

    require(tracked,
        "the renderer never offered context 0 for a trim: the textures the "
        "guest made without a context of its own are still referenced when "
        "it has deleted them");
    require(remaining <= slackBytes,
        "the host GPU still holds the memory of textures the guest released "
        "on context 0 after the idle trim");
    std::fputs("Context 0 idle trim: ok\n", stderr);
    return 0;
}
