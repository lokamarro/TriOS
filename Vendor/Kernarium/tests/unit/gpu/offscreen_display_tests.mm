// SPDX-License-Identifier: GPL-2.0-or-later

/* DisplayMode::offscreen presents the guest into a Metal layer that belongs to
 * no window, on a host with no screen to pace it and no input to take, and
 * keeps the frame mirror that Picture in Picture runs when asked to. The
 * drawable size follows the options, the display presents every frame the
 * guest offers without a display link, a burst of frames keeps being shown
 * (the layer's three drawables keep recycling), and the display and its mirror
 * go away with the display. The pixels it draws are checked in
 * metal_presentation_tests.mm, which drives the same presentation into a
 * layer. A host without a Metal device, or one that gives a windowless layer
 * no drawables, skips. */

#include "display.hpp"
#include "frame_tap_registry.hpp"
#include "gpu_metal_test_support.hpp"
#include "offscreen_display.hpp"
#include "require.hpp"
#include "wait_until.hpp"
#include "watchdog.hpp"

#import <Metal/Metal.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <thread>

using uml::gpu::Display;
using uml::gpu::DisplayMode;
using uml::gpu::DisplaySize;
using uml::gpu::OffscreenDisplayOptions;
using uml::testing::require;

namespace {

constexpr int skipped = 77;
constexpr std::uint32_t width = 320;
constexpr std::uint32_t height = 180;
constexpr auto frameTimeout = std::chrono::seconds(10);

std::unique_ptr<Display> makeOffscreen(
    const OffscreenDisplayOptions& options = {})
{
    return uml::gpu::makeDisplay(
        DisplayMode::offscreen,
        "",
        0,
        {},
        width,
        height,
        {},
        nullptr,
        uml::input::vnc::PointerMode::mouse,
        uml::gpu::protocol::maximumDisplayDimension,
        true,
        options);
}

/* Offers the texture's content as the whole scanout. */
[[nodiscard]] bool offer(Display& display, id<MTLTexture> texture)
{
    return uml::gpu::presentAccepted(display.presentMetal(
        uml::gpu::testing::wholeScanout(texture, width, height)));
}

[[nodiscard]] bool framesPresented(
    Display& display,
    std::uint64_t count,
    std::chrono::steady_clock::duration timeout = frameTimeout)
{
    return uml::testing::waitUntil(
        [&] { return display.statistics().presentedFrames >= count; },
        timeout);
}

/* The drawables follow the option where it names both dimensions, and the
 * guest mode otherwise, and are clamped as a window's size is. */
void checkDrawableSize()
{
    constexpr DisplaySize mode {width, height};
    constexpr auto maximum = uml::gpu::protocol::maximumDisplayDimension;

    require(uml::gpu::offscreenDrawableSize({}, mode, maximum) == mode,
        "the drawables did not default to the guest mode");
    require(uml::gpu::offscreenDrawableSize(
                {.drawableWidth = 2266, .drawableHeight = 1374},
                mode,
                maximum) == DisplaySize {2266, 1374},
        "the drawables did not take the size option");
    require(uml::gpu::offscreenDrawableSize(
                {.drawableWidth = 2266}, mode, maximum) == mode,
        "half a size option was not ignored");
    require(uml::gpu::offscreenDrawableSize(
                {.drawableWidth = 1}, mode, maximum) == mode,
        "an option naming one dimension was not ignored");
    const auto clamped = uml::gpu::offscreenDrawableSize(
        {.drawableWidth = maximum * 2, .drawableHeight = 8}, mode, maximum);
    require(clamped.width == maximum &&
            clamped.height ==
                uml::gpu::DisplayResizeNotifier::minimumDimension,
        "the drawables were not clamped to the display's limits");
}

/* The mode produces a display of the guest's size that takes no input, opens
 * no VNC server, and never asks the guest to resize. */
void checkMode()
{
    auto display = makeOffscreen();
    require(display != nullptr, "the offscreen mode made no display");
    require(display->width() == width && display->height() == height,
        "the offscreen display is not the guest's size");
    require(!display->vncPort().has_value(),
        "the offscreen display opened a VNC server");
    require(!display->drainResizeEvent(),
        "the offscreen display reported a host resize");

    /* A mode this build has no meaning for stays refused. */
    bool refused = false;
    try {
        (void)uml::gpu::makeDisplay(
            static_cast<DisplayMode>(0x7f), "", 0, {}, width, height);
    } catch (const std::exception&) {
        refused = true;
    }
    require(refused, "an unknown display mode made a display");
}

/* The first frame is the one that proves the host gives a windowless layer
 * drawables. */
[[nodiscard]] bool checkFirstFrame(Display& display, id<MTLTexture> texture)
{
    require(offer(display, texture),
        "the offscreen display refused a scanout on its Metal device");
    if (!framesPresented(display, 1)) {
        std::fputs(
            "offscreen-display: skipped: no drawable for a windowless layer\n",
            stderr);
        return false;
    }
    const auto statistics = display.statistics();
    require(statistics.guestFrames >= 1 && statistics.cpuFallbackFrames == 0,
        "the first scanout was counted wrongly");
    return true;
}

/* A guest frame is shown as it arrives, with no display link: a long run of
 * frames at the pace of a 144 Hz guest keeps completing, and the display ends
 * up showing the last of them. */
void checkBurst(Display& display, id<MTLTexture> texture)
{
    constexpr int frames = 600;
    const auto begin = display.statistics().presentedFrames;
    for (int frame = 0; frame < frames; ++frame) {
        uml::gpu::testing::fillTexture(
            texture,
            {static_cast<std::uint8_t>(frame),
             static_cast<std::uint8_t>(frame >> 1),
             static_cast<std::uint8_t>(frame >> 2),
             255});
        require(offer(display, texture),
            "the display refused a scanout during a burst");
        std::this_thread::sleep_for(std::chrono::microseconds(6900));
    }
    require(display.statistics().guestFrames >= frames,
        "the display did not count the frames the guest offered");
    require(framesPresented(display, begin + frames / 8),
        "the display stopped presenting during a burst");

    /* After the burst, one more frame is still shown: a draw that found no
     * drawable or an unfinished frame must have been asked for again. */
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto settled = display.statistics().presentedFrames;
    require(offer(display, texture), "the display refused a scanout");
    require(framesPresented(display, settled + 1),
        "the display never showed the frame that followed a burst");
}

/* A CPU-rendered scanout, which is what a guest without a direct one shows,
 * reaches the layer too. */
void checkCpuFrame(Display& display)
{
    const auto before = display.statistics().presentedFrames;
    const auto image = uml::gpu::testing::solidImage({0, 0, 255, 255}, width, height);
    display.update(
        image,
        width,
        height,
        static_cast<std::size_t>(width) * 4U,
        uml::gpu::protocol::GpuFormat::b8g8r8a8Unorm,
        0,
        0,
        0,
        0,
        width,
        height);
    require(framesPresented(display, before + 1),
        "a CPU frame was not presented into the windowless layer");
}

/* The mirror runs while the display lives and stops with it, and is off
 * unless it was asked for. */
void checkMirror(id<MTLTexture> texture)
{
    auto& registry = uml::gpu::FrameTapRegistry::instance();
    require(!registry.armed(), "a frame mirror was armed before any display");
    {
        auto display = makeOffscreen();
        require(!registry.armed(),
            "the frame mirror was armed without being asked for");
    }
    {
        auto display = makeOffscreen(
            {.mirrorWidth = 400, .mirrorFramesPerSecond = 15});
        const auto armed = registry.snapshot();
        require(armed.has_value() && registry.armed(),
            "the frame mirror was not armed");
        require(armed->options.maximumWidth == 400 &&
                armed->options.framesPerSecond == 15,
            "the frame mirror did not take its options");
        /* Frames reach the mirror's pool and its receiver holds the last two. */
        require(offer(*display, texture), "the display refused a scanout");
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
    require(!registry.armed(), "the frame mirror outlived its display");
    {
        auto display = makeOffscreen({.mirrorWidth = 400});
        require(!registry.armed(),
            "half a mirror option armed the frame mirror");
    }
}

/* The display goes away while frames are still being drawn, with the mirror
 * on, again and again, without a hang or a use of the freed layer. */
void checkTeardownUnderLoad(id<MTLTexture> texture)
{
    uml::testing::Watchdog watchdog(
        "tearing offscreen displays down under load",
        std::chrono::seconds(120));
    for (int round = 0; round < 25; ++round) {
        auto display = makeOffscreen(
            {.drawableWidth = 1133,
             .drawableHeight = 687,
             .mirrorWidth = 400,
             .mirrorFramesPerSecond = 30});
        for (int frame = 0; frame < 6 + round; ++frame)
            require(offer(*display, texture), "a scanout was refused");
        std::this_thread::sleep_for(std::chrono::milliseconds(round % 4));
    }
    require(!uml::gpu::FrameTapRegistry::instance().armed(),
        "the frame mirror outlived the displays");
}

int run()
{
    checkDrawableSize();

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
        std::fputs("offscreen-display: skipped: no Metal device\n", stderr);
        return skipped;
    }
    uml::testing::Watchdog watchdog(
        "the offscreen display test", std::chrono::seconds(80));
    checkMode();

    id<MTLTexture> texture =
        uml::gpu::testing::sharedTexture(device, width, height);
    uml::gpu::testing::fillTexture(texture, {255, 0, 0, 255});
    {
        auto display = makeOffscreen(
            {.drawableWidth = 640, .drawableHeight = 360});
        if (!checkFirstFrame(*display, texture))
            return skipped;
        checkBurst(*display, texture);
        checkCpuFrame(*display);
    }
    checkMirror(texture);
    checkTeardownUnderLoad(texture);
    return EXIT_SUCCESS;
}

} // namespace

int main()
{
    @autoreleasepool {
        return run();
    }
}
