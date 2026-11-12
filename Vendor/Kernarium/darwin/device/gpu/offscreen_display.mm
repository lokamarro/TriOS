// SPDX-License-Identifier: GPL-2.0-or-later

#include "offscreen_display.hpp"

#include "display_geometry.hpp"
#include "frame_tap.hpp"

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <utility>

namespace uml::gpu {
namespace {

/* The mirror's receiver. Picture in Picture hands each frame to a display
 * layer that holds the ones it has queued, so with the frame being rendered
 * the mirror's pool of three buffers is all in use. This keeps the last two
 * frames it was given and drops the rest, which holds the pool at that size. */
class MirrorHold final {
public:
    static void receive(void* context, void* pixelBuffer)
    {
        static_cast<MirrorHold*>(context)->keep(
            static_cast<CVPixelBufferRef>(pixelBuffer));
    }

    void keep(CVPixelBufferRef frame)
    {
        std::lock_guard lock(mutex_);
        CVPixelBufferRelease(held_[0]);
        held_[0] = held_[1];
        held_[1] = CVPixelBufferRetain(frame);
    }

    /* Only once the mirror has stopped delivering. */
    void release()
    {
        std::lock_guard lock(mutex_);
        for (auto& frame : held_) {
            CVPixelBufferRelease(frame);
            frame = nullptr;
        }
    }

private:
    std::mutex mutex_;
    CVPixelBufferRef held_[2] {};
};

} // namespace

struct OffscreenDisplayState final :
    public NativeDisplayStateCore,
    public std::enable_shared_from_this<OffscreenDisplayState> {
    OffscreenDisplayState(
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t maximumDimension,
        const OffscreenDisplayOptions& displayOptions)
        /* No host window ever resizes, so the guest keeps its own mode. */
        : NativeDisplayStateCore(width, height, maximumDimension, false)
        , options(displayOptions)
    {
    }

    void start(std::uint32_t maximumDimension);
    void warmDrawables() noexcept;
    void requestRedraw() noexcept;
    [[nodiscard]] bool guestScanoutChanged(
        std::uint32_t width,
        std::uint32_t height) noexcept override;
    void resetGuestSession() noexcept override;
    void shutdown() noexcept override;

    OffscreenDisplayOptions options;
    CAMetalLayer* layer {};
    dispatch_queue_t queue {};
    std::atomic_bool running {true};
    std::atomic_bool redrawScheduled {};
    MirrorHold mirrorHold;
    bool mirrorStarted {};
};

void OffscreenDisplayState::start(std::uint32_t maximumDimension)
{
    const auto drawable = offscreenDrawableSize(
        options, geometryPolicy.advertisedSize(), maximumDimension);

    layer = [CAMetalLayer layer];
    layer.device = (__bridge id<MTLDevice>)presentation.device();
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = YES;
    layer.maximumDrawableCount = 3;
    layer.drawableSize = CGSizeMake(drawable.width, drawable.height);
    layer.opaque = YES;

    dispatch_queue_attr_t attributes = dispatch_queue_attr_make_with_qos_class(
        DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0);
    queue = dispatch_queue_create(
        "org.linux-um-darwin.offscreen-display", attributes);

    warmDrawables();
    presentation.setRedrawCallback([weak = weak_from_this()] {
        if (auto locked = weak.lock())
            locked->requestRedraw();
    });
    if (options.mirrorWidth != 0 && options.mirrorFramesPerSecond != 0) {
        setFrameTap(
            &MirrorHold::receive,
            &mirrorHold,
            {.maximumWidth = options.mirrorWidth,
             .framesPerSecond = options.mirrorFramesPerSecond});
        mirrorStarted = true;
    }
    std::fprintf(
        stderr,
        "Offscreen display: %ux%u drawables, frame mirror %s\n",
        drawable.width,
        drawable.height,
        mirrorStarted ? "on" : "off");
}

/* A view that presents has rendered into all three of its drawables within a
 * few frames and keeps their backing for as long as it lives; a layer that is
 * only asked for a drawable when the guest draws would show two at idle. Clear
 * and present each once, so that the display holds what the device's does
 * whatever the guest's frame rate. */
void OffscreenDisplayState::warmDrawables() noexcept
{
    @autoreleasepool {
        id<MTLCommandQueue> warm = [layer.device newCommandQueue];
        if (warm == nil)
            return;
        NSMutableArray<id<CAMetalDrawable>>* drawables = [NSMutableArray array];
        for (int count = 0; count < 3; ++count) {
            id<CAMetalDrawable> drawable = [layer nextDrawable];
            if (drawable == nil)
                break;
            [drawables addObject:drawable];
        }
        dispatch_semaphore_t finished = dispatch_semaphore_create(0);
        for (id<CAMetalDrawable> drawable in drawables) {
            MTLRenderPassDescriptor* pass =
                [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = drawable.texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor =
                MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
            id<MTLCommandBuffer> buffer = [warm commandBuffer];
            id<MTLRenderCommandEncoder> encoder =
                [buffer renderCommandEncoderWithDescriptor:pass];
            if (encoder == nil)
                continue;
            [encoder endEncoding];
            [buffer presentDrawable:drawable];
            [buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
                dispatch_semaphore_signal(finished);
            }];
            [buffer commit];
        }
        /* Bounded: a drawable that never completes is not worth a hang. */
        for (NSUInteger index = 0; index < drawables.count; ++index) {
            if (dispatch_semaphore_wait(
                    finished,
                    dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)) != 0) {
                break;
            }
        }
    }
}

void OffscreenDisplayState::requestRedraw() noexcept
{
    if (!running.load(std::memory_order_acquire) ||
        redrawScheduled.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    auto state = shared_from_this();
    dispatch_async(queue, ^{
        /* Cleared before the draw, so a frame that arrives during it asks
         * for another. */
        state->redrawScheduled.store(false, std::memory_order_release);
        if (state->running.load(std::memory_order_acquire)) {
            state->presentation.drawLayer(
                (__bridge void*)state->layer);
        }
    });
}

bool OffscreenDisplayState::guestScanoutChanged(
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    bool publishFrame = false;
    {
        std::lock_guard lock(resizeLifecycleMutex);
        const auto& framebuffer = presentation.framebuffer();
        if (framebuffer.width() != width || framebuffer.height() != height) {
            if (!presentation.resize(width, height, false))
                return false;
            publishFrame = true;
        }
        if (geometryPolicy.guestScanoutChanged(width, height))
            publishFrame = true;
    }
    if (publishFrame)
        presentation.publishCpuFrame();
    return true;
}

void OffscreenDisplayState::resetGuestSession() noexcept
{
    std::lock_guard lock(resizeLifecycleMutex);
    const auto latestHost = resizeNotifier.drain();
    (void)geometryPolicy.hostSizeSettled(
        latestHost.width, latestHost.height);
    const auto restored = geometryPolicy.settledHostSize();
    const auto& framebuffer = presentation.framebuffer();
    const DisplaySize presented {
        .width = framebuffer.width(),
        .height = framebuffer.height(),
    };
    const bool presentationReady = presented == restored ||
        presentation.resize(restored.width, restored.height, false);
    geometryPolicy.resetGuestSession(
        presentationReady ? restored : presented);
    resizeCommitPending.store(false, std::memory_order_release);
}

void OffscreenDisplayState::shutdown() noexcept
{
    if (!running.exchange(false, std::memory_order_acq_rel))
        return;
    if (mirrorStarted) {
        setFrameTap(nullptr, nullptr);
        mirrorHold.release();
    }
    presentation.shutdown();
    /* A draw that was already queued is over once this returns; one that is
     * queued after it sees the display stopped and draws nothing. The queue
     * and the layer stay until the state is destroyed, because a redraw
     * request that passed the running check before this ran may still be on
     * its way to the queue. */
    if (queue != nullptr)
        dispatch_sync(queue, ^{});
}

std::shared_ptr<NativeDisplayStateCore> makeOffscreenDisplayState(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t maximumDimension,
    const OffscreenDisplayOptions& options)
{
    auto state = std::make_shared<OffscreenDisplayState>(
        width, height, maximumDimension, options);
    state->start(maximumDimension);
    return state;
}

} // namespace uml::gpu
