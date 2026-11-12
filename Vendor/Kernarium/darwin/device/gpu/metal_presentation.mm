// SPDX-License-Identifier: GPL-2.0-or-later

#include "metal_presentation.hpp"

#include "display_activity_log.hpp"
#include "display_geometry.hpp"
#ifndef UML_APP_STORE_RUNTIME
#include "frame_tap_registry.hpp"
#endif
#include "metal_scanout_shader.hpp"

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <stdexcept>
#include <thread>
#include <utility>

namespace uml::gpu {
namespace {

/* The CPU fallback texture holds exactly the framebuffer, top row first. */
[[nodiscard]] std::optional<ScanoutSampling> framebufferSampling(
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    return scanoutSampling({
        .texture = {.width = width, .height = height},
        .source = {.width = width, .height = height},
        .width = width,
        .height = height,
    });
}

#ifdef UML_APP_STORE_RUNTIME
/* An App Store runtime reports no frame statistics. */
[[nodiscard]] constexpr double frameStatisticsInterval() noexcept
{
    return 0.0;
}
#else
[[nodiscard]] double frameStatisticsInterval() noexcept
{
    static const double interval = [] {
        const char* value = std::getenv("UML_GPU_FRAME_STATS");
        if (value == nullptr) {
            /* The display activity log wants the same counters, and asking
             * for it is an explicit opt-in already. */
            return displayActivityLog().enabled() ? 1.0 : 0.0;
        }
        char* end = nullptr;
        const double seconds = std::strtod(value, &end);
        if (end == value || !(seconds > 0.0))
            return 0.0;
        return seconds;
    }();
    return interval;
}
#endif

} // namespace

struct MetalPresentationState final :
    public std::enable_shared_from_this<MetalPresentationState> {
    MetalPresentationState(std::uint32_t width, std::uint32_t height)
        : framebuffer(width, height)
        , contentWidth(width)
        , contentHeight(height)
    {
        device = MTLCreateSystemDefaultDevice();
        if (device == nil)
            throw std::runtime_error("Metal has no default GPU device");
        commandQueue = [device newCommandQueue];
        if (commandQueue == nil)
            throw std::runtime_error("Metal command queue creation failed");

        pipelines = makeScanoutPipelines(device, MTLPixelFormatBGRA8Unorm);
    }

    [[nodiscard]] id<MTLTexture> makeFallbackTexture(
        std::uint32_t width,
        std::uint32_t height) const noexcept
    {
        MTLTextureDescriptor* descriptor =
            [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                width:width
                height:height
                mipmapped:NO];
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.usage = MTLTextureUsageShaderRead;
        return [device newTextureWithDescriptor:descriptor];
    }

    void requestRedraw() noexcept
    {
        std::function<void()> callback;
        try {
            std::lock_guard lock(callbackMutex);
            callback = redrawCallback;
        } catch (...) {
            return;
        }
        if (callback) {
            try {
                callback();
            } catch (...) {
            }
        }
    }

    void publishCpuFrame() noexcept
    {
        {
            std::lock_guard lock(presentationMutex);
            if (fallbackTexture != nil) {
                std::lock_guard framebufferLock(framebuffer);
                if (!framebuffer.hasStorage()) {
                    // A clear may be superseded by a native frame before
                    // draw() sees it. Its obsolete texture goes now; Metal
                    // retains any resource an in-flight draw still uses.
                    fallbackTexture = nil;
                }
            }
            latestTexture = nil;
            latestIsCpu = true;
            ++latestContentGeneration;
            ++latestPresentationGeneration;
        }
#ifndef UML_APP_STORE_RUNTIME
        markTapDirty();
#endif
        requestRedraw();
    }

#ifndef UML_APP_STORE_RUNTIME
    /* The mirror (frame_tap.hpp): a second render of the latest guest
     * content into pool-backed IOSurface buffers, on its own thread, so that
     * it runs when there is no window to draw into. Nothing here runs while
     * no sink is set, beyond the thread's wait. An App Store runtime has no
     * mirror. */

    void startTap()
    {
        tapThread = std::thread([this] { tapLoop(); });
    }

    void stopTap() noexcept
    {
        if (!tapThread.joinable())
            return;
        FrameTapRegistry::instance().wake();
        tapThread.join();
        /* A frame still on the GPU releases its texture into the cache and
         * its buffer into the pool from its completion handler, so neither
         * may go while one is in flight. A GPU that does not finish in five
         * seconds keeps them: leaking a cache is better than a handler that
         * releases into a freed one. */
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (tapInFlight.load(std::memory_order_acquire) > 0 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (tapInFlight.load(std::memory_order_acquire) > 0)
            return;
        if (tapPool != nullptr) {
            CVPixelBufferPoolRelease(tapPool);
            tapPool = nullptr;
        }
        if (tapTextureCache != nullptr) {
            CFRelease(tapTextureCache);
            tapTextureCache = nullptr;
        }
    }

    void markTapDirty() noexcept
    {
        auto& registry = FrameTapRegistry::instance();
        if (!registry.armed())
            return;
        tapDirty.store(true, std::memory_order_release);
        registry.wake();
    }

    void tapLoop() noexcept
    {
        (void)::pthread_setname_np("uml-frame-tap");
        using clock = std::chrono::steady_clock;
        auto& registry = FrameTapRegistry::instance();
        auto lastFrame = clock::time_point {};
        while (running.load(std::memory_order_acquire)) {
            registry.waitArmed([this] {
                return !running.load(std::memory_order_relaxed);
            });
            if (!running.load(std::memory_order_acquire))
                break;
            /* A new guest frame, or a second without one: a static display
             * is sent again so that its consumer sees a live stream. */
            registry.wait(std::chrono::milliseconds(1000), [this] {
                return tapDirty.load(std::memory_order_relaxed) ||
                    !running.load(std::memory_order_relaxed);
            });
            if (!running.load(std::memory_order_acquire))
                break;
            const auto armed = registry.snapshot();
            if (!armed)
                continue;
            const auto interval = std::chrono::milliseconds(
                1000 / armed->options.framesPerSecond);
            const auto elapsed = clock::now() - lastFrame;
            if (elapsed < interval)
                std::this_thread::sleep_for(interval - elapsed);
            tapDirty.store(false, std::memory_order_release);
            renderTapFrame(*armed);
            lastFrame = clock::now();
        }
    }

    [[nodiscard]] bool prepareTapSurfaces(
        std::uint32_t width, std::uint32_t height) noexcept
    {
        if (tapTextureCache == nullptr &&
            CVMetalTextureCacheCreate(
                kCFAllocatorDefault, nil, device, nil, &tapTextureCache) !=
                kCVReturnSuccess) {
            tapTextureCache = nullptr;
            return false;
        }
        if (tapPool != nullptr && tapPoolWidth == width &&
            tapPoolHeight == height) {
            return true;
        }
        if (tapPool != nullptr) {
            CVPixelBufferPoolRelease(tapPool);
            tapPool = nullptr;
        }
        NSDictionary* poolAttributes = @{
            (__bridge NSString*)kCVPixelBufferPoolMinimumBufferCountKey: @3,
        };
        NSDictionary* bufferAttributes = @{
            (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey:
                @(kCVPixelFormatType_32BGRA),
            (__bridge NSString*)kCVPixelBufferWidthKey: @(width),
            (__bridge NSString*)kCVPixelBufferHeightKey: @(height),
            (__bridge NSString*)kCVPixelBufferIOSurfacePropertiesKey: @{},
            (__bridge NSString*)kCVPixelBufferMetalCompatibilityKey: @YES,
        };
        if (CVPixelBufferPoolCreate(
                kCFAllocatorDefault,
                (__bridge CFDictionaryRef)poolAttributes,
                (__bridge CFDictionaryRef)bufferAttributes,
                &tapPool) != kCVReturnSuccess) {
            tapPool = nullptr;
            return false;
        }
        tapPoolWidth = width;
        tapPoolHeight = height;
        return true;
    }

    void renderTapFrame(const FrameTapRegistry::Armed& armed) noexcept
    {
        /* The GPU finishes a copy long before the next is due; a backlog
         * means it is busy, and the mirror yields. */
        if (tapInFlight.load(std::memory_order_acquire) >= 2)
            return;
        @autoreleasepool {
            try {
                std::optional<ScanoutSampling> sampling;
                id<MTLTexture> source;
                DisplaySize content {};
                DisplayRotation rotation = DisplayRotation::upright;
                {
                    std::lock_guard lock(presentationMutex);
                    rotation = displayRotation;
                    if (latestIsCpu) {
                        sampling = framebufferSampling(
                            contentWidth, contentHeight);
                        source = uploadFallbackLocked();
                    } else {
                        sampling = latestSampling;
                        source = latestTexture;
                    }
                    /* The display as the window shows it, turned the way
                     * the user turned it. */
                    content = presentedDisplaySize(
                        sampling ?
                            sampling->content :
                            DisplaySize {contentWidth, contentHeight},
                        rotation);
                }
                if (content.width == 0 || content.height == 0)
                    return;
                /* Even dimensions: a video consumer rejects odd ones. */
                const std::uint32_t width = std::max<std::uint32_t>(
                    2U,
                    std::min(armed.options.maximumWidth, content.width) &
                        ~1U);
                const std::uint32_t height = std::max<std::uint32_t>(
                    2U,
                    static_cast<std::uint32_t>(std::llround(
                        static_cast<double>(width) * content.height /
                        content.width)) & ~1U);
                if (!prepareTapSurfaces(width, height))
                    return;
                CVPixelBufferRef pixelBuffer = nullptr;
                if (CVPixelBufferPoolCreatePixelBuffer(
                        kCFAllocatorDefault, tapPool, &pixelBuffer) !=
                        kCVReturnSuccess) {
                    return;
                }
                CVMetalTextureRef cvTexture = nullptr;
                if (CVMetalTextureCacheCreateTextureFromImage(
                        kCFAllocatorDefault,
                        tapTextureCache,
                        pixelBuffer,
                        nil,
                        MTLPixelFormatBGRA8Unorm,
                        width,
                        height,
                        0,
                        &cvTexture) != kCVReturnSuccess) {
                    CVPixelBufferRelease(pixelBuffer);
                    return;
                }
                id<MTLTexture> target = CVMetalTextureGetTexture(cvTexture);
                id<MTLCommandBuffer> buffer = [commandQueue commandBuffer];
                MTLRenderPassDescriptor* pass =
                    [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = target;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                pass.colorAttachments[0].clearColor =
                    MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
                id<MTLRenderCommandEncoder> encoder = buffer == nil ? nil :
                    [buffer renderCommandEncoderWithDescriptor:pass];
                if (encoder == nil) {
                    CVPixelBufferRelease(pixelBuffer);
                    CFRelease(cvTexture);
                    return;
                }
                if (source != nil && sampling && scanoutPassSamples(source)) {
                    encodeScanoutPass(
                        encoder,
                        pipelines.forTexture(source),
                        source,
                        *sampling,
                        rotation,
                        MTLViewport {
                            .originX = 0.0,
                            .originY = 0.0,
                            .width = static_cast<double>(width),
                            .height = static_cast<double>(height),
                            .znear = 0.0,
                            .zfar = 1.0,
                        });
                }
                [encoder endEncoding];
                tapInFlight.fetch_add(1, std::memory_order_acq_rel);
                const auto generation = armed.generation;
                auto state = shared_from_this();
                [buffer addCompletedHandler:^(id<MTLCommandBuffer> finished) {
                    if (finished.status == MTLCommandBufferStatusCompleted) {
                        (void)FrameTapRegistry::instance().deliver(
                            generation, pixelBuffer);
                    }
                    CVPixelBufferRelease(pixelBuffer);
                    CFRelease(cvTexture);
                    state->tapInFlight.fetch_sub(
                        1, std::memory_order_acq_rel);
                }];
                [buffer commit];
                if (++tapFrames % 64 == 0)
                    CVMetalTextureCacheFlush(tapTextureCache, 0);
            } catch (...) {
            }
        }
    }
#endif

    void drawableSizeChanged() noexcept
    {
        {
            std::lock_guard lock(presentationMutex);
            ++latestPresentationGeneration;
        }
        requestRedraw();
    }

    [[nodiscard]] DisplayRotation currentRotation() const noexcept
    {
        std::lock_guard lock(presentationMutex);
        return displayRotation;
    }

    [[nodiscard]] bool setRotation(DisplayRotation rotation) noexcept
    {
        {
            std::lock_guard lock(presentationMutex);
            if (displayRotation == rotation)
                return false;
            displayRotation = rotation;
            ++latestPresentationGeneration;
        }
        requestRedraw();
        return true;
    }

    [[nodiscard]] bool resize(
        std::uint32_t width,
        std::uint32_t height,
        bool shouldRequestRedraw) noexcept
    {
        @autoreleasepool {
            try {
                if (width == framebuffer.width() &&
                    height == framebuffer.height()) {
                    return false;
                }
                {
                    /* A CPU draw makes, verifies and uploads its fallback
                     * texture under this same lock, so framebuffer storage
                     * and the matching Metal allocation change as one
                     * snapshot. A new mode has neither until the CPU path
                     * writes to it. */
                    std::lock_guard lock(presentationMutex);
                    framebuffer.resize(width, height);
                    fallbackTexture = nil;
                    contentWidth = width;
                    contentHeight = height;
                    latestTexture = nil;
                    latestIsCpu = true;
                    ++latestContentGeneration;
                    ++latestPresentationGeneration;
                }
                if (shouldRequestRedraw)
                    requestRedraw();
                return true;
            } catch (...) {
                if (!resizeFailureReported.exchange(
                        true, std::memory_order_relaxed)) {
                    std::fputs("Native Metal display resize failed\n", stderr);
                }
                return false;
            }
        }
    }

    PresentOutcome present(const MetalScanout& scanout) noexcept
    {
        guestFrames.fetch_add(1, std::memory_order_relaxed);
        try {
            if (!running.load(std::memory_order_acquire) ||
                scanout.texture == nullptr) {
                return PresentOutcome::unsupported;
            }
            id<MTLTexture> texture = (__bridge id<MTLTexture>)scanout.texture;
            if (!scanoutPassSamples(texture) || texture.device != device ||
                texture.width < scanout.sourceWidth ||
                texture.height < scanout.sourceHeight) {
                displayActivityLog().record(
                    "present display=native reject=texture handle=%p "
                    "type=%lu layers=%lu size=%lux%lu need=%ux%u "
                    "same-device=%d",
                    scanout.texture,
                    static_cast<unsigned long>(texture.textureType),
                    static_cast<unsigned long>(texture.arrayLength),
                    static_cast<unsigned long>(texture.width),
                    static_cast<unsigned long>(texture.height),
                    scanout.sourceWidth,
                    scanout.sourceHeight,
                    texture.device == device ? 1 : 0);
                return PresentOutcome::unsupported;
            }
            /* The pass scales the scanout rectangle into the drawable at the
             * rectangle's own aspect, whatever size the framebuffer has. */
            const auto sampling = scanoutPassSampling(texture, scanout);
            if (!sampling) {
                displayActivityLog().record(
                    "present display=native reject=geometry texture=%p "
                    "source=%ux%u view=%ux%u+%u+%u",
                    scanout.texture,
                    scanout.sourceWidth,
                    scanout.sourceHeight,
                    scanout.width,
                    scanout.height,
                    scanout.sourceX,
                    scanout.sourceY);
                return PresentOutcome::unsupported;
            }
            bool superseded = false;
            {
                std::lock_guard lock(presentationMutex);
                superseded = latestContentGeneration >
                    presentedContentGeneration;
                latestTexture = texture;
                latestSampling = *sampling;
                latestIsCpu = false;
                /* A display-sized copy of pixels nothing shows any more,
                 * typically the console the guest drew before its GPU
                 * scanout. The framebuffer keeps them, and every CPU draw
                 * uploads all of it, so the next one makes a new texture. */
                fallbackTexture = nil;
                ++latestContentGeneration;
                ++latestPresentationGeneration;
            }
            if (superseded)
                droppedFrames.fetch_add(1, std::memory_order_relaxed);
            if (!directPathReported.exchange(true, std::memory_order_relaxed)) {
                std::fputs(
                    "Native display: direct Metal texture presentation\n",
                    stderr);
            }
#ifndef UML_APP_STORE_RUNTIME
            markTapDirty();
#endif
            requestRedraw();
            maybeReport();
            return superseded ? PresentOutcome::dropped
                              : PresentOutcome::presented;
        } catch (...) {
            return PresentOutcome::unsupported;
        }
    }

    void finishPresentation(
        std::uint64_t contentGeneration,
        std::uint64_t presentationGeneration) noexcept
    {
        bool pending = false;
        {
            std::lock_guard lock(presentationMutex);
            presentedContentGeneration = std::max(
                presentedContentGeneration, contentGeneration);
            presentedPresentationGeneration = std::max(
                presentedPresentationGeneration, presentationGeneration);
            presentationInFlight = false;
            pending = latestPresentationGeneration >
                presentedPresentationGeneration;
        }
        if (pending)
            requestRedraw();
    }

    /* The texture showing the framebuffer, freshly uploaded, or nil when
     * there is nothing to upload. Called with presentationMutex held, which
     * keeps the framebuffer at contentWidth by contentHeight. */
    [[nodiscard]] id<MTLTexture> uploadFallbackLocked() noexcept
    {
        std::lock_guard framebufferLock(framebuffer);
        if (!framebuffer.hasStorage()) {
            /* Cleared: the pass's clear is the whole picture. The texture
             * goes with the storage, and the first draw after the next
             * write makes another. */
            fallbackTexture = nil;
            return nil;
        }
        if (fallbackTexture == nil ||
            fallbackTexture.width != framebuffer.width() ||
            fallbackTexture.height != framebuffer.height()) {
            /* Made here, under the same locks as the upload, so that the
             * framebuffer and its texture stay one snapshot. Creating it
             * costs far less than the upload this thread makes anyway. */
            fallbackTexture = makeFallbackTexture(
                framebuffer.width(), framebuffer.height());
            if (fallbackTexture == nil) {
                if (!fallbackFailureReported.exchange(
                        true, std::memory_order_relaxed)) {
                    std::fputs(
                        "Native Metal display could not allocate its CPU "
                        "fallback texture\n",
                        stderr);
                }
                return nil;
            }
        }
        [fallbackTexture
            replaceRegion:MTLRegionMake2D(
                0, 0, framebuffer.width(), framebuffer.height())
            mipmapLevel:0
            withBytes:framebuffer.pixels().data()
            bytesPerRow:framebuffer.rowBytes()];
        return fallbackTexture;
    }

    /* Draws the newest frame into the next drawable of a view, or, with no
     * view, of a layer that belongs to no window. */
    void draw(MTKView* view, CAMetalLayer* layer) noexcept
    {
        @autoreleasepool {
            std::uint64_t contentGeneration = 0;
            std::uint64_t presentationGeneration = 0;
            std::optional<ScanoutSampling> activeSampling;
            DisplayRotation activeRotation = DisplayRotation::upright;
            bool useCpu = false;
            std::uint32_t cpuWidth = 0;
            std::uint32_t cpuHeight = 0;
            id<MTLTexture> sourceTexture;
            try {
                if (view == nil && layer == nil)
                    return;
                {
                    std::lock_guard lock(presentationMutex);
                    if (presentationInFlight ||
                        latestPresentationGeneration <=
                            presentedPresentationGeneration) {
                        return;
                    }
                    contentGeneration = latestContentGeneration;
                    presentationGeneration = latestPresentationGeneration;
                    activeRotation = displayRotation;
                    useCpu = latestIsCpu;
                    cpuWidth = contentWidth;
                    cpuHeight = contentHeight;
                    activeSampling = useCpu ?
                        framebufferSampling(cpuWidth, cpuHeight) :
                        std::optional<ScanoutSampling> {latestSampling};
                    /* A CPU frame finds its texture below, once the
                     * framebuffer is locked; it may have none. */
                    sourceTexture = useCpu ? nil : latestTexture;
                    if (!activeSampling || (!useCpu && sourceTexture == nil))
                        return;
                    presentationInFlight = true;
                }
                /* Acquiring a drawable can block until the display releases
                 * one, and this runs on the thread that owns the user
                 * interface. Claim the frame first so a redraw with nothing
                 * new to show never pays for it. */
                MTLRenderPassDescriptor* pass = nil;
                id<CAMetalDrawable> drawable = nil;
                MTLClearColor clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
                if (view != nil) {
                    pass = view.currentRenderPassDescriptor;
                    drawable = view.currentDrawable;
                    clearColor = view.clearColor;
                } else {
                    drawable = [layer nextDrawable];
                    if (drawable != nil) {
                        pass = [MTLRenderPassDescriptor renderPassDescriptor];
                        pass.colorAttachments[0].texture = drawable.texture;
                        pass.colorAttachments[0].storeAction =
                            MTLStoreActionStore;
                    }
                }
                if (pass == nil || drawable == nil) {
                    /* Release the claim without retiring the generation: the
                     * frame was not shown, and the next guest frame asks for
                     * this one again. */
                    std::lock_guard lock(presentationMutex);
                    presentationInFlight = false;
                    return;
                }
                if (useCpu) {
                    bool current = false;
                    {
                        std::lock_guard presentationLock(presentationMutex);
                        /* A resize since the claim is shown by its own
                         * generation. */
                        current = contentWidth == cpuWidth &&
                            contentHeight == cpuHeight;
                        if (current)
                            sourceTexture = uploadFallbackLocked();
                    }
                    if (!current) {
                        finishPresentation(
                            contentGeneration, presentationGeneration);
                        return;
                    }
                }
                /* A CPU frame with no pixels, the first after a resize or a
                 * clear, presents the pass's clear alone. It still retires
                 * its generation through the completed handler below. */
                const bool drawSource = sourceTexture != nil;
                MTLViewport viewport {};
                if (drawSource) {
                    const auto presented = presentedDisplaySize(
                        activeSampling->content, activeRotation);
                    const auto fitted = aspectFitRectangle(
                        static_cast<double>(drawable.texture.width),
                        static_cast<double>(drawable.texture.height),
                        static_cast<double>(presented.width),
                        static_cast<double>(presented.height));
                    if (fitted.width <= 0.0 || fitted.height <= 0.0) {
                        finishPresentation(
                            contentGeneration, presentationGeneration);
                        return;
                    }
                    viewport = {
                        .originX = fitted.x,
                        .originY = fitted.y,
                        .width = fitted.width,
                        .height = fitted.height,
                        .znear = 0.0,
                        .zfar = 1.0,
                    };
                }
                id<MTLCommandBuffer> buffer = [commandQueue commandBuffer];
                if (buffer == nil) {
                    finishPresentation(
                        contentGeneration, presentationGeneration);
                    return;
                }
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].clearColor = clearColor;
                id<MTLRenderCommandEncoder> encoder =
                    [buffer renderCommandEncoderWithDescriptor:pass];
                if (encoder == nil) {
                    finishPresentation(
                        contentGeneration, presentationGeneration);
                    return;
                }
                if (drawSource) {
                    encodeScanoutPass(
                        encoder,
                        pipelines.forTexture(sourceTexture),
                        sourceTexture,
                        *activeSampling,
                        activeRotation,
                        viewport);
                }
                [encoder endEncoding];
                [buffer presentDrawable:drawable];
                auto state = shared_from_this();
                [buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
                    state->presentedFrames.fetch_add(
                        1, std::memory_order_relaxed);
                    state->finishPresentation(
                        contentGeneration, presentationGeneration);
                }];
                [buffer commit];
            } catch (...) {
                if (!renderFailureReported.exchange(
                        true, std::memory_order_relaxed)) {
                    std::fputs("Native Metal display failed\n", stderr);
                }
                if (presentationGeneration != 0) {
                    finishPresentation(
                        contentGeneration, presentationGeneration);
                }
            }
        }
    }

    DisplayStatistics snapshot() const noexcept
    {
        return DisplayStatistics {
            .guestFrames = guestFrames.load(std::memory_order_relaxed),
            .presentedFrames = presentedFrames.load(std::memory_order_relaxed),
            .droppedFrames = droppedFrames.load(std::memory_order_relaxed),
            .cpuFallbackFrames =
                cpuFallbackFrames.load(std::memory_order_relaxed),
        };
    }

    void reportFinal() const noexcept
    {
        if (frameStatisticsInterval() <= 0.0)
            return;
        const DisplayStatistics total = snapshot();
        recordDisplayStatistics("native", true, total);
        std::fprintf(
            stderr,
            "Native display final: guest %llu presented %llu dropped %llu "
            "cpu-fallback %llu\n",
            static_cast<unsigned long long>(total.guestFrames),
            static_cast<unsigned long long>(total.presentedFrames),
            static_cast<unsigned long long>(total.droppedFrames),
            static_cast<unsigned long long>(total.cpuFallbackFrames));
    }

    void maybeReport() noexcept
    {
        const double interval = frameStatisticsInterval();
        if (interval <= 0.0)
            return;
        const auto now = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        auto previous = reportedAt.load(std::memory_order_relaxed);
        if (previous == 0) {
            reportedAt.compare_exchange_strong(
                previous, now, std::memory_order_relaxed);
            return;
        }
        const auto span = now - previous;
        if (static_cast<double>(span) < interval * 1e9 ||
            !reportedAt.compare_exchange_strong(
                previous, now, std::memory_order_relaxed)) {
            return;
        }
        const DisplayStatistics total = snapshot();
        const double seconds = static_cast<double>(span) / 1e9;
        const auto guest = total.guestFrames - reportedGuestFrames;
        const auto presented = total.presentedFrames - reportedPresentedFrames;
        const auto dropped = total.droppedFrames - reportedDroppedFrames;
        const auto fallback =
            total.cpuFallbackFrames - reportedCpuFallbackFrames;
        reportedGuestFrames = total.guestFrames;
        reportedPresentedFrames = total.presentedFrames;
        reportedDroppedFrames = total.droppedFrames;
        reportedCpuFallbackFrames = total.cpuFallbackFrames;
        recordDisplayStatistics("native", false, total);
        std::fprintf(
            stderr,
            "Native display frames: guest %.1f/s presented %.1f/s dropped "
            "%.1f/s (%.1f%%) cpu-fallback %.1f/s | totals guest %llu "
            "presented %llu dropped %llu cpu-fallback %llu\n",
            static_cast<double>(guest) / seconds,
            static_cast<double>(presented) / seconds,
            static_cast<double>(dropped) / seconds,
            guest == 0 ? 0.0 : 100.0 * static_cast<double>(dropped) /
                static_cast<double>(guest),
            static_cast<double>(fallback) / seconds,
            static_cast<unsigned long long>(total.guestFrames),
            static_cast<unsigned long long>(total.presentedFrames),
            static_cast<unsigned long long>(total.droppedFrames),
            static_cast<unsigned long long>(total.cpuFallbackFrames));
    }

    ScanoutFramebuffer framebuffer;
    id<MTLDevice> device;
    id<MTLCommandQueue> commandQueue;
    ScanoutPipelines pipelines;
#ifndef UML_APP_STORE_RUNTIME
    std::thread tapThread;
    std::atomic_bool tapDirty {};
    std::atomic_int tapInFlight {};
    CVMetalTextureCacheRef tapTextureCache {};
    CVPixelBufferPoolRef tapPool {};
    std::uint32_t tapPoolWidth {};
    std::uint32_t tapPoolHeight {};
    std::uint64_t tapFrames {};
#endif
    id<MTLTexture> fallbackTexture;
    id<MTLTexture> latestTexture;
    ScanoutSampling latestSampling;
    std::mutex callbackMutex;
    std::function<void()> redrawCallback;
    mutable std::mutex presentationMutex;
    std::uint64_t latestContentGeneration {1};
    std::uint64_t presentedContentGeneration {};
    std::uint64_t latestPresentationGeneration {1};
    std::uint64_t presentedPresentationGeneration {};
    std::uint32_t contentWidth;
    std::uint32_t contentHeight;
    DisplayRotation displayRotation {DisplayRotation::upright};
    bool latestIsCpu {true};
    bool presentationInFlight {};
    std::atomic_bool running {true};
    std::atomic_bool directPathReported {};
    std::atomic_bool renderFailureReported {};
    std::atomic_bool fallbackFailureReported {};
    std::atomic_bool resizeFailureReported {};
    std::atomic_uint64_t guestFrames {};
    std::atomic_uint64_t presentedFrames {};
    std::atomic_uint64_t droppedFrames {};
    std::atomic_uint64_t cpuFallbackFrames {};
    std::atomic_uint64_t reportedAt {};
    std::uint64_t reportedGuestFrames {};
    std::uint64_t reportedPresentedFrames {};
    std::uint64_t reportedDroppedFrames {};
    std::uint64_t reportedCpuFallbackFrames {};
};

MetalPresentation::MetalPresentation(
    std::uint32_t width, std::uint32_t height)
    : state_(std::make_shared<MetalPresentationState>(width, height))
{
#ifndef UML_APP_STORE_RUNTIME
    state_->startTap();
#endif
}

MetalPresentation::~MetalPresentation()
{
    shutdown();
}

void MetalPresentation::setRedrawCallback(std::function<void()> callback)
{
    std::lock_guard lock(state_->callbackMutex);
    state_->redrawCallback = std::move(callback);
}

void* MetalPresentation::device() const noexcept
{
    return (__bridge void*)state_->device;
}

ScanoutFramebuffer& MetalPresentation::framebuffer() noexcept
{
    return state_->framebuffer;
}

const ScanoutFramebuffer& MetalPresentation::framebuffer() const noexcept
{
    return state_->framebuffer;
}

bool MetalPresentation::resize(
    std::uint32_t width,
    std::uint32_t height,
    bool requestRedraw) noexcept
{
    return state_->resize(width, height, requestRedraw);
}

DisplayRotation MetalPresentation::rotation() const noexcept
{
    return state_->currentRotation();
}

bool MetalPresentation::setRotation(DisplayRotation rotation) noexcept
{
    return state_->setRotation(rotation);
}

void MetalPresentation::drawableSizeChanged() noexcept
{
    state_->drawableSizeChanged();
}

void MetalPresentation::publishCpuFrame() noexcept
{
    state_->publishCpuFrame();
}

PresentOutcome MetalPresentation::present(
    const MetalScanout& scanout) noexcept
{
    return state_->present(scanout);
}

void MetalPresentation::draw(void* nativeView) noexcept
{
    state_->draw((__bridge MTKView*)nativeView, nil);
}

void MetalPresentation::drawLayer(void* metalLayer) noexcept
{
    state_->draw(nil, (__bridge CAMetalLayer*)metalLayer);
}

DisplayStatistics MetalPresentation::statistics() const noexcept
{
    return state_->snapshot();
}

void MetalPresentation::recordCpuFallbackFrame() noexcept
{
    state_->cpuFallbackFrames.fetch_add(1, std::memory_order_relaxed);
}

void MetalPresentation::shutdown() noexcept
{
    if (!state_ ||
        !state_->running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    state_->reportFinal();
#ifndef UML_APP_STORE_RUNTIME
    state_->stopTap();
#endif
    std::lock_guard lock(state_->callbackMutex);
    state_->redrawCallback = {};
}

} // namespace uml::gpu
