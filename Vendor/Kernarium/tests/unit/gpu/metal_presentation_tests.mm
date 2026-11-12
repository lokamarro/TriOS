// SPDX-License-Identifier: GPL-2.0-or-later

/* A guest mode need not match the display's framebuffer: glmark2 sets
 * 4096x2160 on a 1280x720 display, and a scanout can show a rectangle of a
 * larger source. The native presenter scales such a scanout in its own Metal
 * pass instead of refusing it into the CPU fallback. Each case presents a
 * texture with one colour per quadrant and reads back the drawable that the
 * presenter drew into. The CPU path's framebuffer and texture exist only once
 * it is written, so its frames before that present the clear alone. The
 * frame mirror for Picture in Picture is checked the same way, from the
 * pixels it delivers, and so is a layer that belongs to no window, which the
 * offscreen display draws into. */

#include "frame_tap.hpp"
#include "gpu_metal_test_support.hpp"
#include "metal_presentation.hpp"
#include "wait_until.hpp"

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using uml::gpu::DisplayRotation;
using uml::gpu::MetalPresentation;
using uml::gpu::MetalScanout;
using uml::gpu::PresentOutcome;

/* A layer that belongs to no window, which keeps the drawable it hands out so
 * that a test can read back what was drawn into it. */
@interface CapturingMetalLayer : CAMetalLayer
@property(nonatomic, strong) id<CAMetalDrawable> lastDrawable;
@end

@implementation CapturingMetalLayer
- (id<CAMetalDrawable>)nextDrawable
{
    id<CAMetalDrawable> drawable = [super nextDrawable];
    self.lastDrawable = drawable;
    return drawable;
}
@end

@interface PresentationTestDelegate : NSObject <MTKViewDelegate>
@property(nonatomic) MetalPresentation* presentation;
@property(nonatomic, strong) id<CAMetalDrawable> drawable;
@end

@implementation PresentationTestDelegate
- (void)mtkView:(MTKView*)view drawableSizeWillChange:(CGSize)size
{
    (void)view;
    (void)size;
}

- (void)drawInMTKView:(MTKView*)view
{
    self.drawable = view.currentDrawable;
    self.presentation->draw((__bridge void*)view);
}
@end

namespace {

constexpr int skipped = 77;
constexpr std::uint32_t framebufferWidth = 64;
constexpr std::uint32_t framebufferHeight = 36;
constexpr std::uint32_t sourceWidth = 128;
constexpr std::uint32_t sourceHeight = 72;

using Pixel = std::array<std::uint8_t, 4>;
/* BGRA, one colour per quadrant of the source. */
constexpr Pixel red {0, 0, 255, 255};
constexpr Pixel green {0, 255, 0, 255};
constexpr Pixel blue {255, 0, 0, 255};
constexpr Pixel white {255, 255, 255, 255};
constexpr Pixel black {0, 0, 0, 255};

void require(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "metal-presentation: %s\n", message);
        std::_Exit(1);
    }
}

id<MTLTexture> makeQuadrantTexture(id<MTLDevice> device)
{
    id<MTLTexture> texture =
        uml::gpu::testing::sharedTexture(device, sourceWidth, sourceHeight);
    std::vector<std::uint8_t> pixels(sourceWidth * sourceHeight * 4);
    for (std::uint32_t y = 0; y < sourceHeight; ++y) {
        for (std::uint32_t x = 0; x < sourceWidth; ++x) {
            const bool right = x >= sourceWidth / 2;
            const bool bottom = y >= sourceHeight / 2;
            const Pixel& colour = bottom ? (right ? white : blue) :
                                           (right ? green : red);
            std::memcpy(&pixels[(y * sourceWidth + x) * 4], colour.data(), 4);
        }
    }
    uml::gpu::testing::writeTexture(texture, pixels);
    return texture;
}

class Harness final {
public:
    Harness(MetalPresentation& presentation, id<MTLTexture> source)
        : presentation_(presentation)
        , source_(source)
    {
        id<MTLDevice> device =
            (__bridge id<MTLDevice>)presentation.device();
        queue_ = [device newCommandQueue];
        view_ = [[MTKView alloc]
            initWithFrame:NSMakeRect(0, 0, framebufferWidth, framebufferHeight)
            device:device];
        view_.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        view_.framebufferOnly = NO;
        view_.paused = YES;
        view_.enableSetNeedsDisplay = NO;
        view_.autoResizeDrawable = NO;
        view_.drawableSize = CGSizeMake(framebufferWidth, framebufferHeight);
        view_.clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
        delegate_ = [[PresentationTestDelegate alloc] init];
        delegate_.presentation = &presentation;
        view_.delegate = delegate_;
    }

    [[nodiscard]] PresentOutcome present(MetalScanout scanout)
    {
        scanout.texture = (__bridge void*)source_;
        scanout.sourceWidth = scanout.sourceWidth == 0 ?
            sourceWidth : scanout.sourceWidth;
        scanout.sourceHeight = scanout.sourceHeight == 0 ?
            sourceHeight : scanout.sourceHeight;
        scanout.damageWidth = scanout.width;
        scanout.damageHeight = scanout.height;
        return presentation_.present(scanout);
    }

    /* Draws the newest frame and returns the drawable's pixels, or nothing
     * when this host gives an offscreen view no drawables. A draw can find
     * the previous frame still retiring, so it is retried until a new frame
     * completes. */
    [[nodiscard]] bool draw(std::vector<std::uint8_t>& pixels)
    {
        const auto target = presentation_.statistics().presentedFrames + 1;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            delegate_.drawable = nil;
            [view_ draw];
            if (delegate_.drawable == nil)
                return false;
            const auto frameDeadline = std::chrono::steady_clock::now() +
                std::chrono::seconds(1);
            while (presentation_.statistics().presentedFrames < target &&
                   std::chrono::steady_clock::now() < frameDeadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (presentation_.statistics().presentedFrames >= target) {
                readBack(delegate_.drawable.texture, pixels);
                return true;
            }
        }
        require(false, "the presenter never completed a frame");
        return false;
    }

private:
    void readBack(id<MTLTexture> texture, std::vector<std::uint8_t>& pixels)
    {
        require(texture.width == framebufferWidth &&
                texture.height == framebufferHeight,
            "the drawable is not the requested size");
        pixels = uml::gpu::testing::readTexture(
            queue_, texture, framebufferWidth, framebufferHeight,
            framebufferWidth * 4U);
    }

    MetalPresentation& presentation_;
    id<MTLTexture> source_;
    id<MTLCommandQueue> queue_;
    MTKView* view_;
    PresentationTestDelegate* delegate_;
};

[[nodiscard]] bool pixelIs(
    const std::vector<std::uint8_t>& pixels,
    std::uint32_t x,
    std::uint32_t y,
    const Pixel& expected)
{
    const auto* pixel = &pixels[(y * framebufferWidth + x) * 4];
    for (std::size_t channel = 0; channel < expected.size(); ++channel) {
        const int difference = static_cast<int>(pixel[channel]) -
            static_cast<int>(expected[channel]);
        if (difference < -2 || difference > 2) {
            std::fprintf(stderr,
                "metal-presentation: pixel %u,%u is %u,%u,%u,%u, expected "
                "%u,%u,%u,%u\n",
                x, y, pixel[0], pixel[1], pixel[2], pixel[3],
                expected[0], expected[1], expected[2], expected[3]);
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool accepted(PresentOutcome outcome)
{
    return outcome != PresentOutcome::unsupported;
}

[[nodiscard]] bool hasCpuPixels(MetalPresentation& presentation)
{
    auto& framebuffer = presentation.framebuffer();
    std::lock_guard lock(framebuffer);
    return framebuffer.hasStorage();
}

/* Writes one colour over the CPU framebuffer's whole mode, as a readback
 * does, and publishes it. */
void writeCpuFrame(MetalPresentation& presentation, const Pixel& colour)
{
    auto& framebuffer = presentation.framebuffer();
    const auto width = framebuffer.width();
    const auto height = framebuffer.height();
    const auto source = uml::gpu::testing::solidImage(colour, width, height);
    const auto written = framebuffer.update(
        source,
        width,
        height,
        static_cast<std::size_t>(width) * colour.size(),
        uml::gpu::protocol::GpuFormat::b8g8r8a8Unorm,
        0,
        0,
        0,
        0,
        width,
        height);
    require(written.width == width && written.height == height,
        "the CPU framebuffer refused a whole-mode write");
    presentation.publishCpuFrame();
}

[[nodiscard]] bool allOf(
    const std::vector<std::uint8_t>& pixels,
    const Pixel& expected)
{
    return pixelIs(pixels, 0, 0, expected) &&
        pixelIs(pixels, framebufferWidth - 1, 0, expected) &&
        pixelIs(pixels, framebufferWidth / 2, framebufferHeight / 2,
            expected) &&
        pixelIs(pixels, 0, framebufferHeight - 1, expected) &&
        pixelIs(pixels, framebufferWidth - 1, framebufferHeight - 1,
            expected);
}

void checkCpuFrames(
    MetalPresentation& presentation,
    Harness& harness,
    std::vector<std::uint8_t>& pixels)
{
    /* A display that has only shown Metal textures holds no CPU pixels, and
     * a CPU frame without them presents the clear. It must still retire, or
     * the next draw could never complete. */
    require(!hasCpuPixels(presentation),
        "the direct path allocated the CPU framebuffer");
    presentation.publishCpuFrame();
    require(harness.draw(pixels), "a CPU frame without pixels was not drawn");
    require(allOf(pixels, black), "a CPU frame without pixels was not black");

    writeCpuFrame(presentation, green);
    require(hasCpuPixels(presentation),
        "a CPU write did not allocate the framebuffer");
    require(harness.draw(pixels), "the first CPU frame was not drawn");
    require(allOf(pixels, green), "the first CPU frame was not shown");

    /* A new mode gives up the old one's pixels and starts black. */
    require(presentation.resize(framebufferWidth / 2, framebufferHeight / 2),
        "the CPU framebuffer did not take a new mode");
    require(!hasCpuPixels(presentation),
        "a new mode kept the previous mode's pixels");
    require(harness.draw(pixels), "the first frame of a new mode was not drawn");
    require(allOf(pixels, black), "the first frame of a new mode was not black");
    writeCpuFrame(presentation, blue);
    require(harness.draw(pixels), "a CPU frame of a new mode was not drawn");
    require(allOf(pixels, blue),
        "a CPU frame of a new mode was not scaled into the view");

    /* So does a cleared display. */
    presentation.framebuffer().clear();
    presentation.publishCpuFrame();
    require(!hasCpuPixels(presentation), "clearing kept the CPU pixels");
    require(harness.draw(pixels), "a cleared CPU frame was not drawn");
    require(allOf(pixels, black), "a cleared CPU frame was not black");
    writeCpuFrame(presentation, red);
    require(harness.draw(pixels), "a CPU frame after a clear was not drawn");
    require(allOf(pixels, red), "a CPU frame after a clear was not shown");
}

void checkClearSupersededByNativeFrame(
    MetalPresentation& presentation, Harness& harness,
    std::vector<std::uint8_t>& pixels)
{
    // Keep the drawable tiny while making the obsolete fallback allocation
    // large enough to distinguish it from command and pipeline bookkeeping.
    require(presentation.resize(2048, 1152), "fallback mode did not change");
    writeCpuFrame(presentation, green);
    require(harness.draw(pixels), "fallback frame did not finish");
    id<MTLDevice> device = (__bridge id<MTLDevice>)presentation.device();
    const auto allocated = device.currentAllocatedSize;

    presentation.framebuffer().clear();
    presentation.publishCpuFrame();
    require(accepted(harness.present({.width = sourceWidth, .height = sourceHeight})),
        "native frame after a clear was refused");
    // Do not draw the clear: the next native frame superseded it. Releasing
    // the empty framebuffer's texture must not depend on drawing that clear.
    require(uml::testing::waitUntil([&] {
        return device.currentAllocatedSize + (8U << 20) < allocated;
    }, std::chrono::seconds(5)), "a superseded clear retained the CPU fallback texture");
    require(harness.draw(pixels), "native frame after a clear did not finish");
    require(pixelIs(pixels, 16, 9, red) && pixelIs(pixels, 48, 27, white),
        "clearing the fallback changed native presentation");

    // A later partial CPU write starts on the cleared black image and still
    // creates a usable fallback texture, without reviving the old pixels.
    const auto written = presentation.framebuffer().update(red, 1, 1, 4,
        uml::gpu::protocol::GpuFormat::b8g8r8a8Unorm, 0, 0, 0, 0, 1, 1);
    require(written.width == 1 && written.height == 1, "partial CPU write was refused");
    presentation.publishCpuFrame();
    require(harness.draw(pixels), "partial CPU frame after native did not finish");
    require(pixelIs(pixels, 32, 18, black),
        "a partial CPU update revived cleared fallback pixels");
    writeCpuFrame(presentation, blue);
    require(harness.draw(pixels) && allOf(pixels, blue),
        "fallback texture could not be recreated after native presentation");
    require(presentation.resize(framebufferWidth, framebufferHeight),
        "fallback mode did not return");
}

/* A native frame replaces a CPU one, as a guest's GPU scanout replaces the
 * console it drew first. The CPU frame's texture is then a copy nothing
 * shows, while the framebuffer keeps its pixels for the next CPU frame. */
void checkNativeFrameReleasesFallback(
    MetalPresentation& presentation, Harness& harness,
    std::vector<std::uint8_t>& pixels)
{
    require(presentation.resize(2048, 1152), "fallback mode did not change");
    writeCpuFrame(presentation, green);
    require(harness.draw(pixels) && allOf(pixels, green),
        "the console frame was not shown");
    id<MTLDevice> device = (__bridge id<MTLDevice>)presentation.device();
    const auto allocated = device.currentAllocatedSize;

    require(accepted(harness.present({.width = sourceWidth, .height = sourceHeight})),
        "native frame after a CPU frame was refused");
    require(uml::testing::waitUntil([&] {
        return device.currentAllocatedSize + (8U << 20) < allocated;
    }, std::chrono::seconds(5)), "a native frame retained the CPU fallback texture");
    require(harness.draw(pixels), "native frame after a CPU frame did not finish");
    require(pixelIs(pixels, 16, 9, red) && pixelIs(pixels, 48, 27, white),
        "releasing the fallback changed native presentation");
    require(hasCpuPixels(presentation), "a native frame dropped the CPU pixels");

    // Back on the CPU path, the kept pixels return in a new texture.
    presentation.publishCpuFrame();
    require(harness.draw(pixels) && allOf(pixels, green),
        "the CPU pixels did not return after native presentation");
    require(presentation.resize(framebufferWidth, framebufferHeight),
        "fallback mode did not return");
}

/* What the mirror delivered: the newest frame, and how many arrived. */
struct TapRecorder final {
    struct Frame final {
        std::uint32_t width {};
        std::uint32_t height {};
        std::vector<std::uint8_t> pixels;

        [[nodiscard]] bool is(
            double x, double y, const Pixel& expected) const
        {
            const auto column = static_cast<std::uint32_t>(x * width);
            const auto row = static_cast<std::uint32_t>(y * height);
            const auto* pixel = &pixels[(row * width + column) * 4];
            for (std::size_t channel = 0; channel < expected.size();
                 ++channel) {
                const int difference = static_cast<int>(pixel[channel]) -
                    static_cast<int>(expected[channel]);
                if (difference < -3 || difference > 3)
                    return false;
            }
            return true;
        }
    };

    std::mutex mutex;
    Frame latest;
    std::uint64_t count {};

    static void sink(void* context, void* pixelBuffer) noexcept
    {
        auto& recorder = *static_cast<TapRecorder*>(context);
        const auto buffer = static_cast<CVPixelBufferRef>(pixelBuffer);
        if (CVPixelBufferLockBaseAddress(
                buffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
            return;
        }
        Frame frame;
        frame.width = static_cast<std::uint32_t>(
            CVPixelBufferGetWidth(buffer));
        frame.height = static_cast<std::uint32_t>(
            CVPixelBufferGetHeight(buffer));
        const auto stride = CVPixelBufferGetBytesPerRow(buffer);
        const auto* base = static_cast<const std::uint8_t*>(
            CVPixelBufferGetBaseAddress(buffer));
        frame.pixels.resize(
            static_cast<std::size_t>(frame.width) * frame.height * 4U);
        for (std::uint32_t row = 0; row < frame.height; ++row) {
            std::memcpy(
                &frame.pixels[static_cast<std::size_t>(row) * frame.width * 4U],
                base + static_cast<std::size_t>(row) * stride,
                static_cast<std::size_t>(frame.width) * 4U);
        }
        CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
        std::lock_guard lock(recorder.mutex);
        recorder.latest = std::move(frame);
        ++recorder.count;
    }

    [[nodiscard]] std::uint64_t delivered()
    {
        std::lock_guard lock(mutex);
        return count;
    }

    [[nodiscard]] bool awaitCount(
        std::uint64_t wanted,
        std::chrono::seconds limit = std::chrono::seconds(60))
    {
        return uml::testing::waitUntil(
            [&] { return delivered() >= wanted; },
            limit,
            std::chrono::milliseconds(5));
    }

    /* Waits for a delivered frame that satisfies `accept`. The limit only
     * fails a mirror that never delivers; a frame arrives in tens of
     * milliseconds. */
    [[nodiscard]] bool await(
        const std::function<bool(const Frame&)>& accept,
        std::chrono::seconds limit = std::chrono::seconds(60))
    {
        return uml::testing::waitUntil(
            [&] {
                std::lock_guard lock(mutex);
                return count != 0 && accept(latest);
            },
            limit,
            std::chrono::milliseconds(5));
    }
};

/* The mirror delivers the guest's display, upright and aspect-preserved,
 * for the same frames the window shows, and nothing once it is stopped. */
void checkFrameTap(MetalPresentation& presentation, Harness& harness)
{
    TapRecorder recorder;
    uml::gpu::setFrameTap(
        &TapRecorder::sink,
        &recorder,
        {.maximumWidth = 64, .framesPerSecond = 60});

    require(accepted(harness.present({
                .width = sourceWidth, .height = sourceHeight})),
        "a scanout larger than the framebuffer was refused");
    require(recorder.await([](const TapRecorder::Frame& frame) {
        return frame.width == 64 && frame.height == 36 &&
            frame.is(0.25, 0.25, red) && frame.is(0.75, 0.25, green) &&
            frame.is(0.25, 0.75, blue) && frame.is(0.75, 0.75, white);
    }), "the mirror did not deliver the scanout, scaled to its size");

    /* A display that stops changing is sent again, so that a consumer of the
     * stream sees it live. */
    require(recorder.awaitCount(recorder.delivered() + 2),
        "the mirror did not repeat a static display");

    require(accepted(harness.present({
                .width = sourceWidth,
                .height = sourceHeight,
                .flipped = true})),
        "a bottom-up scanout was refused");
    require(recorder.await([](const TapRecorder::Frame& frame) {
        return frame.is(0.25, 0.25, blue) && frame.is(0.75, 0.25, white) &&
            frame.is(0.25, 0.75, red) && frame.is(0.75, 0.75, green);
    }), "the mirror did not turn a bottom-up scanout the right way up");

    /* A rectangle keeps its aspect: the top half is 128 by 36. */
    require(accepted(harness.present({
                .width = sourceWidth, .height = sourceHeight / 2})),
        "a scanout rectangle was refused");
    require(recorder.await([](const TapRecorder::Frame& frame) {
        return frame.width == 64 && frame.height == 18 &&
            frame.is(0.25, 0.5, red) && frame.is(0.75, 0.5, green);
    }), "the mirror did not take the rectangle's own aspect");

    /* The user's rotation turns the mirror as it turns the window. */
    require(presentation.setRotation(DisplayRotation::clockwise90),
        "rotation did not change");
    require(accepted(harness.present({
                .width = sourceWidth, .height = sourceHeight})),
        "a rotated scanout was refused");
    require(recorder.await([](const TapRecorder::Frame& frame) {
        return frame.width < frame.height &&
            frame.is(0.25, 0.25, blue) && frame.is(0.75, 0.25, red) &&
            frame.is(0.25, 0.75, white) && frame.is(0.75, 0.75, green);
    }), "the mirror did not follow the display's rotation");
    require(presentation.setRotation(DisplayRotation::upright),
        "rotation did not return");

    /* Stopping returns only once no delivery is running or will start, so
     * the receiver may go away: nothing arrives afterwards, whatever the
     * guest shows. */
    uml::gpu::setFrameTap(nullptr, nullptr);
    const auto stopped = recorder.delivered();
    require(accepted(harness.present({
                .width = sourceWidth, .height = sourceHeight})),
        "a scanout was refused after the mirror stopped");
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    require(recorder.delivered() == stopped,
        "the mirror delivered a frame after it was stopped");
}

/* The windowless display draws into a layer of its own instead of an
 * MTKView: the drawable's pixels must be the view path's, and the layer must
 * keep recycling its three drawables, because a layer outside any window that
 * stopped handing them out would stall each draw for the drawable timeout. */
void checkLayerDraw(MetalPresentation& presentation, Harness& harness)
{
    id<MTLDevice> device = (__bridge id<MTLDevice>)presentation.device();
    id<MTLCommandQueue> queue = [device newCommandQueue];
    CapturingMetalLayer* layer = [CapturingMetalLayer layer];
    layer.device = device;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = NO;
    layer.maximumDrawableCount = 3;
    layer.drawableSize = CGSizeMake(framebufferWidth, framebufferHeight);

    /* One draw, with the previous frame still retiring retried, as the
     * view's draw is. Returns the time it took. */
    const auto drawOnce = [&]() -> std::chrono::steady_clock::duration {
        const auto begin = std::chrono::steady_clock::now();
        const auto target = presentation.statistics().presentedFrames + 1;
        const auto deadline = begin + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            layer.lastDrawable = nil;
            presentation.drawLayer((__bridge void*)layer);
            if (uml::testing::waitUntil(
                    [&] {
                        return presentation.statistics().presentedFrames >=
                            target;
                    },
                    std::chrono::seconds(1))) {
                return std::chrono::steady_clock::now() - begin;
            }
        }
        require(false, "the layer never completed a frame");
        return {};
    };

    require(accepted(harness.present({
                .width = sourceWidth, .height = sourceHeight})),
        "a scanout larger than the framebuffer was refused");
    drawOnce();
    require(layer.lastDrawable != nil, "the layer handed out no drawable");
    const auto pixels = uml::gpu::testing::readTexture(
        queue, layer.lastDrawable.texture, framebufferWidth, framebufferHeight,
        framebufferWidth * 4U);
    require(pixelIs(pixels, 16, 9, red) && pixelIs(pixels, 48, 9, green) &&
            pixelIs(pixels, 16, 27, blue) && pixelIs(pixels, 48, 27, white),
        "a scanout drawn into a windowless layer was not scaled into it");

    /* Many more frames than the layer has drawables, each shown before the
     * next is offered, as a guest at the display's pace does. */
    constexpr int frames = 300;
    std::chrono::steady_clock::duration slowest {};
    for (int frame = 0; frame < frames; ++frame) {
        require(accepted(harness.present({
                    .width = sourceWidth, .height = sourceHeight})),
            "a scanout was refused while the layer recycled its drawables");
        slowest = std::max(slowest, drawOnce());
    }
    require(slowest < std::chrono::milliseconds(500),
        "a windowless layer stopped recycling its drawables");
}

/* A CPU-rendered display, which is what a guest without a direct scanout
 * shows, is mirrored too. */
void checkFrameTapCpu(MetalPresentation& presentation)
{
    TapRecorder recorder;
    uml::gpu::setFrameTap(
        &TapRecorder::sink,
        &recorder,
        {.maximumWidth = 64, .framesPerSecond = 60});
    writeCpuFrame(presentation, green);
    const auto width = presentation.framebuffer().width();
    const auto height = presentation.framebuffer().height();
    require(recorder.await([&](const TapRecorder::Frame& frame) {
        return frame.width == width && frame.height == height &&
            frame.is(0.1, 0.1, green) && frame.is(0.9, 0.9, green);
    }), "the mirror did not deliver the CPU framebuffer");
    writeCpuFrame(presentation, red);
    require(recorder.await([](const TapRecorder::Frame& frame) {
        return frame.is(0.5, 0.5, red);
    }), "the mirror did not follow a new CPU frame");
    uml::gpu::setFrameTap(nullptr, nullptr);
}

int run()
{
    std::unique_ptr<MetalPresentation> presentation;
    try {
        presentation = std::make_unique<MetalPresentation>(
            framebufferWidth, framebufferHeight);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "metal-presentation: skipped: %s\n", error.what());
        return skipped;
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)presentation->device();
    Harness harness {*presentation, makeQuadrantTexture(device)};

    /* Rejected before any pixel check so a host without offscreen drawables
     * still answers whether the presenter takes the frame. */
    require(accepted(harness.present({
                .width = sourceWidth, .height = sourceHeight})),
        "a scanout larger than the framebuffer was refused");
    require(!accepted(harness.present({
                .sourceX = 100, .width = 64, .height = 36})),
        "a scanout reaching past its source was accepted");
    require(!accepted(harness.present({
                .sourceWidth = sourceWidth * 2,
                .sourceHeight = sourceHeight * 2,
                .width = sourceWidth * 2,
                .height = sourceHeight * 2})),
        "a texture smaller than its source was accepted");

    /* The mirror renders into its own surfaces, so it needs no drawable. */
    checkFrameTap(*presentation, harness);

    std::vector<std::uint8_t> pixels;
    require(accepted(harness.present({
                .width = sourceWidth, .height = sourceHeight})),
        "a scanout larger than the framebuffer was refused");
    if (!harness.draw(pixels)) {
        std::fputs(
            "metal-presentation: skipped pixel checks: no offscreen drawable\n",
            stderr);
        return skipped;
    }
    require(pixelIs(pixels, 16, 9, red) && pixelIs(pixels, 48, 9, green) &&
            pixelIs(pixels, 16, 27, blue) && pixelIs(pixels, 48, 27, white),
        "a scanout larger than the framebuffer was not scaled into it");
    checkLayerDraw(*presentation, harness);

    /* Columns 16-79 of the top half: the red-green boundary at column 64
     * lands three quarters of the way across. */
    require(accepted(harness.present({
                .sourceX = 16,
                .width = sourceWidth / 2,
                .height = sourceHeight / 2})),
        "a scanout rectangle inside its source was refused");
    require(harness.draw(pixels), "the sub-rectangle frame was not drawn");
    require(pixelIs(pixels, 0, 0, red) && pixelIs(pixels, 38, 18, red) &&
            pixelIs(pixels, 56, 18, green) && pixelIs(pixels, 63, 35, green),
        "a scanout rectangle did not show only its own pixels");

    require(accepted(harness.present({
                .width = sourceWidth,
                .height = sourceHeight,
                .flipped = true})),
        "a bottom-up scanout larger than the framebuffer was refused");
    require(harness.draw(pixels), "the bottom-up frame was not drawn");
    require(pixelIs(pixels, 16, 9, blue) && pixelIs(pixels, 48, 9, white) &&
            pixelIs(pixels, 16, 27, red) && pixelIs(pixels, 48, 27, green),
        "a bottom-up scanout was not turned the right way up");

    /* The guest's top-left quadrant of a bottom-up source is stored in the
     * texture's bottom half. */
    require(accepted(harness.present({
                .width = sourceWidth / 2,
                .height = sourceHeight / 2,
                .flipped = true})),
        "a bottom-up scanout rectangle was refused");
    require(harness.draw(pixels), "the bottom-up rectangle was not drawn");
    require(pixelIs(pixels, 0, 0, blue) && pixelIs(pixels, 63, 35, blue),
        "a bottom-up scanout rectangle showed the wrong texels");

    /* A rectangle keeps its own aspect: the source's top half letterboxes
     * into the framebuffer's 16:9 drawable rather than stretching over it. */
    require(accepted(harness.present({
                .width = sourceWidth, .height = sourceHeight / 2})),
        "a scanout with the framebuffer's width and half its aspect was "
        "refused");
    require(harness.draw(pixels), "the letterboxed frame was not drawn");
    require(pixelIs(pixels, 16, 4, black) && pixelIs(pixels, 16, 31, black) &&
            pixelIs(pixels, 16, 18, red) && pixelIs(pixels, 48, 18, green),
        "a scanout rectangle was not aspect-fitted at its own aspect");

    checkCpuFrames(*presentation, harness, pixels);
    checkClearSupersededByNativeFrame(*presentation, harness, pixels);
    checkNativeFrameReleasesFallback(*presentation, harness, pixels);

    /* A clockwise quarter-turn shows the guest's top-left at the display's
     * top-right, pillarboxed at the rotated aspect. */
    require(presentation->setRotation(DisplayRotation::clockwise90),
        "rotation did not change");
    require(accepted(harness.present({
                .width = sourceWidth, .height = sourceHeight})),
        "a rotated scanout was refused");
    require(harness.draw(pixels), "the rotated frame was not drawn");
    require(pixelIs(pixels, 2, 18, black) && pixelIs(pixels, 61, 18, black) &&
            pixelIs(pixels, 24, 2, blue) && pixelIs(pixels, 40, 2, red) &&
            pixelIs(pixels, 24, 33, white) && pixelIs(pixels, 40, 33, green),
        "a rotated scanout was not scaled at its rotated aspect");

    require(presentation->setRotation(DisplayRotation::upright),
        "rotation did not return");
    checkFrameTapCpu(*presentation);

    presentation->shutdown();
    return EXIT_SUCCESS;
}

} // namespace

int main()
{
    @autoreleasepool {
        return run();
    }
}
