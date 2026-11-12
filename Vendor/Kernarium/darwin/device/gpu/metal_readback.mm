// SPDX-License-Identifier: GPL-2.0-or-later

#include "metal_readback.hpp"

#include "display_activity_log.hpp"
#include "metal_scanout_shader.hpp"

#if UML_GPU_VNC_STARTUP_TESTING
#include "vnc_display_testing.hpp"
#endif

#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

namespace uml::gpu {
namespace {

struct ReadbackRequest {
    id<MTLTexture> texture;
    protocol::GpuFormat format {};
    std::uint32_t sourceX {};
    std::uint32_t sourceY {};
    std::uint32_t width {};
    std::uint32_t height {};
    /* The damage's origin relative to the view. */
    std::uint32_t destinationX {};
    std::uint32_t destinationY {};
    /* The scanout's view of the texture, which damage never leaves. */
    std::uint32_t viewX {};
    std::uint32_t viewY {};
    std::uint32_t viewWidth {};
    std::uint32_t viewHeight {};
    /* Where the view goes in the display's framebuffer. */
    DisplayRectangle placement;
    std::uint64_t epoch {};
    /* How the scanout pass samples the view; set only for a request that is
     * drawn rather than copied. */
    std::optional<ScanoutSampling> sampling;
    std::uint32_t generation {};
    ReadbackSource source {};

    [[nodiscard]] ReadbackTarget target() const noexcept
    {
        return {.placement = placement, .epoch = epoch};
    }
};

/* The part of a request that completion publishes: which pixels were read,
 * and where they go. */
struct ReadbackCopy {
    protocol::GpuFormat format {};
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t destinationX {};
    std::uint32_t destinationY {};
    ReadbackTarget target;
    std::uint32_t generation {};
    ReadbackSource source {};
    bool complete {};
};

[[nodiscard]] bool coversView(const ReadbackRequest& request) noexcept
{
    return request.sourceX == request.viewX &&
        request.sourceY == request.viewY &&
        request.width == request.viewWidth &&
        request.height == request.viewHeight;
}

void coverView(ReadbackRequest& request) noexcept
{
    request.sourceX = request.viewX;
    request.sourceY = request.viewY;
    request.width = request.viewWidth;
    request.height = request.viewHeight;
    request.destinationX = 0;
    request.destinationY = 0;
}

/* A newer request replaces one still waiting for the in-flight readback. Its
 * texture holds the whole newest image, so reading the union of both damages
 * from it shows everything the replaced request would have. A different view
 * or target has no common damage, so the newer request reads its whole
 * view. */
void absorb(ReadbackRequest& next, const ReadbackRequest& replaced) noexcept
{
    if (next.viewX != replaced.viewX || next.viewY != replaced.viewY ||
        next.viewWidth != replaced.viewWidth ||
        next.viewHeight != replaced.viewHeight ||
        next.target() != replaced.target()) {
        coverView(next);
        return;
    }
    const auto left = std::min(next.sourceX, replaced.sourceX);
    const auto top = std::min(next.sourceY, replaced.sourceY);
    const auto right = std::max(
        next.sourceX + next.width, replaced.sourceX + replaced.width);
    const auto bottom = std::max(
        next.sourceY + next.height, replaced.sourceY + replaced.height);
    next.sourceX = left;
    next.sourceY = top;
    next.width = right - left;
    next.height = bottom - top;
    next.destinationX = left - next.viewX;
    next.destinationY = top - next.viewY;
}

[[nodiscard]] MetalScanout wholeView(MetalScanout scanout) noexcept
{
    scanout.damageX = scanout.sourceX;
    scanout.damageY = scanout.sourceY;
    scanout.damageWidth = scanout.width;
    scanout.damageHeight = scanout.height;
    return scanout;
}

[[nodiscard]] std::optional<protocol::GpuFormat>
readbackFormat(MTLPixelFormat format) noexcept
{
    switch (format) {
    case MTLPixelFormatBGRA8Unorm:
    case MTLPixelFormatBGRA8Unorm_sRGB:
        return protocol::GpuFormat::b8g8r8a8Unorm;
    case MTLPixelFormatRGBA8Unorm:
    case MTLPixelFormatRGBA8Unorm_sRGB:
        return protocol::GpuFormat::r8g8b8a8Unorm;
    default:
        return std::nullopt;
    }
}

/* The drawn image keeps the source's encoding: an sRGB texture is sampled in
 * linear light and written back as sRGB, so its bytes stay comparable to a
 * copied one. */
[[nodiscard]] MTLPixelFormat drawnFormat(MTLPixelFormat source) noexcept
{
    return source == MTLPixelFormatBGRA8Unorm_sRGB ||
            source == MTLPixelFormatRGBA8Unorm_sRGB ?
        MTLPixelFormatBGRA8Unorm_sRGB :
        MTLPixelFormatBGRA8Unorm;
}

[[nodiscard]] bool rangeFits(
    std::uint32_t origin,
    std::uint32_t extent,
    std::uint32_t limit) noexcept
{
    return origin <= limit && extent <= limit - origin;
}

[[nodiscard]] std::optional<std::size_t> alignedRowBytes(
    std::uint32_t width,
    std::size_t alignment) noexcept
{
    if (alignment == 0) {
        return std::nullopt;
    }
    const auto bytes =
        static_cast<std::size_t>(width) * protocol::gpuBytesPerPixel;
    const auto remainder = bytes % alignment;
    if (remainder == 0)
        return bytes;
    const auto padding = alignment - remainder;
    if (bytes > std::numeric_limits<std::size_t>::max() - padding)
        return std::nullopt;
    return bytes + padding;
}

/* The pixels of a drawn placement that the damage can reach, relative to the
 * placement. A destination pixel samples the texels within one of its centre
 * under either filter, so the scaled damage grows by one pixel on each side
 * before it is clamped to the placement. */
[[nodiscard]] DisplayRectangle drawnFootprint(
    const ReadbackRequest& request) noexcept
{
    const auto span = [](std::uint32_t origin,
                          std::uint32_t length,
                          std::uint32_t view,
                          std::uint32_t placed) {
        const double scale = static_cast<double>(placed) / view;
        const double first = std::floor(origin * scale) - 1.0;
        const double last = std::ceil((origin + length) * scale) + 1.0;
        const auto clamp = [placed](double value) {
            return static_cast<std::uint32_t>(
                std::clamp(value, 0.0, static_cast<double>(placed)));
        };
        const auto start = clamp(first);
        return std::pair {start, clamp(last) - start};
    };
    const auto [x, width] = span(
        request.destinationX, request.width, request.viewWidth,
        request.placement.width);
    const auto [y, height] = span(
        request.destinationY, request.height, request.viewHeight,
        request.placement.height);
    return {.x = x, .y = y, .width = width, .height = height};
}

} // namespace

struct MetalReadbackState final :
    public std::enable_shared_from_this<MetalReadbackState> {
    explicit MetalReadbackState(MetalReadback::PublishCallback callback)
        : publish(std::move(callback))
    {
    }

    [[nodiscard]] std::optional<ReadbackRequest> request(
        const MetalScanout& scanout,
        ReadbackTarget target) noexcept
    {
        const auto placement = target.placement;
        auto& activity = displayActivityLog();
        if (scanout.texture == nullptr || scanout.sourceWidth == 0 ||
            scanout.sourceHeight == 0 || scanout.width == 0 ||
            scanout.height == 0 || scanout.damageWidth == 0 ||
            scanout.damageHeight == 0 || placement.width == 0 ||
            placement.height == 0 ||
            !rangeFits(
                scanout.sourceX, scanout.width, scanout.sourceWidth) ||
            !rangeFits(
                scanout.sourceY, scanout.height, scanout.sourceHeight) ||
            scanout.damageX < scanout.sourceX ||
            scanout.damageY < scanout.sourceY ||
            !rangeFits(
                scanout.damageX, scanout.damageWidth,
                scanout.sourceX + scanout.width) ||
            !rangeFits(
                scanout.damageY, scanout.damageHeight,
                scanout.sourceY + scanout.height)) {
            activity.record(
                "readback reject=geometry texture=%p source=%ux%u "
                "view=%ux%u+%u+%u damage=%ux%u+%u+%u placement=%ux%u+%u+%u",
                scanout.texture,
                scanout.sourceWidth,
                scanout.sourceHeight,
                scanout.width,
                scanout.height,
                scanout.sourceX,
                scanout.sourceY,
                scanout.damageWidth,
                scanout.damageHeight,
                scanout.damageX,
                scanout.damageY,
                placement.width,
                placement.height,
                placement.x,
                placement.y);
            return std::nullopt;
        }

        id<MTLTexture> texture = (__bridge id<MTLTexture>)scanout.texture;
        if (!scanoutPassSamples(texture) ||
            texture.width < scanout.sourceWidth ||
            texture.height < scanout.sourceHeight) {
            activity.record(
                "readback reject=texture handle=%p type=%lu layers=%lu "
                "samples=%lu size=%lux%lu need=%ux%u",
                scanout.texture,
                static_cast<unsigned long>(texture.textureType),
                static_cast<unsigned long>(texture.arrayLength),
                static_cast<unsigned long>(texture.sampleCount),
                static_cast<unsigned long>(texture.width),
                static_cast<unsigned long>(texture.height),
                scanout.sourceWidth,
                scanout.sourceHeight);
            return std::nullopt;
        }
        const auto format = readbackFormat(texture.pixelFormat);
        if (!format) {
            activity.record(
                "readback reject=format handle=%p pixelFormat=%lu "
                "storage=%lu usage=%lu",
                scanout.texture,
                static_cast<unsigned long>(texture.pixelFormat),
                static_cast<unsigned long>(texture.storageMode),
                static_cast<unsigned long>(texture.usage));
            return std::nullopt;
        }

        /* A copy keeps the texture's size and row order, so a scaled or
         * bottom-up view is drawn instead, which samples the texture. */
        std::optional<ScanoutSampling> sampling;
        if (scanout.flipped || placement.width != scanout.width ||
            placement.height != scanout.height) {
            if ((texture.usage & MTLTextureUsageShaderRead) == 0) {
                activity.record(
                    "readback reject=usage handle=%p usage=%lu",
                    scanout.texture,
                    static_cast<unsigned long>(texture.usage));
                return std::nullopt;
            }
            sampling = scanoutPassSampling(texture, scanout);
            if (!sampling) {
                activity.record(
                    "readback reject=sampling handle=%p", scanout.texture);
                return std::nullopt;
            }
        }
        activity.record(
            "readback accept handle=%p pixelFormat=%lu storage=%lu usage=%lu "
            "size=%lux%lu damage=%ux%u+%u+%u placement=%ux%u+%u+%u "
            "path=%s",
            scanout.texture,
            static_cast<unsigned long>(texture.pixelFormat),
            static_cast<unsigned long>(texture.storageMode),
            static_cast<unsigned long>(texture.usage),
            static_cast<unsigned long>(texture.width),
            static_cast<unsigned long>(texture.height),
            scanout.damageWidth,
            scanout.damageHeight,
            scanout.damageX,
            scanout.damageY,
            placement.width,
            placement.height,
            placement.x,
            placement.y,
            sampling ? "draw" : "copy");

        return ReadbackRequest {
            .texture = texture,
            .format = *format,
            .sourceX = scanout.damageX,
            .sourceY = scanout.damageY,
            .width = scanout.damageWidth,
            .height = scanout.damageHeight,
            .destinationX = scanout.damageX - scanout.sourceX,
            .destinationY = scanout.damageY - scanout.sourceY,
            .viewX = scanout.sourceX,
            .viewY = scanout.sourceY,
            .viewWidth = scanout.width,
            .viewHeight = scanout.height,
            .placement = placement,
            .epoch = target.epoch,
            .sampling = sampling,
        };
    }

    [[nodiscard]] bool ensureDeviceLocked(id<MTLDevice> requested) noexcept
    {
        if (requested == nil)
            return false;
        if (device == nil) {
            device = requested;
            commandQueue = [device newCommandQueue];
            if (commandQueue == nil) {
                device = nil;
                return false;
            }
            return true;
        }
        return requested == device;
    }

    /* The draw target of a scaled placement, and its pipelines, follow the
     * placement's size and the source's encoding. */
    [[nodiscard]] id<MTLRenderPipelineState> preparedDrawLocked(
        const ReadbackRequest& request)
    {
        const auto format = drawnFormat(request.texture.pixelFormat);
        auto& pipelines = format == MTLPixelFormatBGRA8Unorm_sRGB ?
            srgbPipelines : unormPipelines;
        if (!pipelines)
            pipelines = makeScanoutPipelines(device, format);
        if (drawTarget == nil || drawTarget.pixelFormat != format ||
            drawTarget.width != request.placement.width ||
            drawTarget.height != request.placement.height) {
            MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:format
                width:request.placement.width
                height:request.placement.height
                mipmapped:NO];
            descriptor.storageMode = MTLStorageModePrivate;
            descriptor.usage = MTLTextureUsageRenderTarget;
            drawTarget = [device newTextureWithDescriptor:descriptor];
            if (drawTarget == nil)
                return nil;
        }
        return pipelines->forTexture(request.texture);
    }

    [[nodiscard]] bool scheduleLocked(ReadbackRequest request) noexcept
    {
        @autoreleasepool {
            try {
                if (!ensureDeviceLocked(request.texture.device))
                    return false;

                id<MTLTexture> copySource = request.texture;
                MTLOrigin copyOrigin =
                    MTLOriginMake(request.sourceX, request.sourceY, 0);
                ReadbackCopy copy {
                    .format = request.format,
                    .width = request.width,
                    .height = request.height,
                    .destinationX =
                        request.placement.x + request.destinationX,
                    .destinationY =
                        request.placement.y + request.destinationY,
                    .target = request.target(),
                    .generation = request.generation,
                    .source = request.source,
                    .complete = coversView(request),
                };
                id<MTLRenderPipelineState> pipeline;
                DisplayRectangle footprint;
                if (request.sampling) {
                    pipeline = preparedDrawLocked(request);
                    if (pipeline == nil)
                        return false;
                    footprint = drawnFootprint(request);
                    if (footprint.width == 0 || footprint.height == 0)
                        return false;
                    copySource = drawTarget;
                    copyOrigin = MTLOriginMake(footprint.x, footprint.y, 0);
                    copy.format = protocol::GpuFormat::b8g8r8a8Unorm;
                    copy.width = footprint.width;
                    copy.height = footprint.height;
                    copy.destinationX = request.placement.x + footprint.x;
                    copy.destinationY = request.placement.y + footprint.y;
                }

                const auto alignment = static_cast<std::size_t>(
                    [device minimumLinearTextureAlignmentForPixelFormat:
                        copySource.pixelFormat]);
                const auto requestedRowBytes =
                    alignedRowBytes(copy.width, alignment);
                if (!requestedRowBytes ||
                    copy.height >
                        std::numeric_limits<std::size_t>::max() /
                            *requestedRowBytes) {
                    return false;
                }
                const auto requestedSize = *requestedRowBytes * copy.height;
                if (buffer == nil || buffer.length < requestedSize) {
                    buffer = [device
                        newBufferWithLength:requestedSize
                        options:MTLResourceStorageModeShared];
                    if (buffer == nil)
                        return false;
                }

                id<MTLCommandBuffer> commands =
                    [commandQueue commandBuffer];
                if (commands == nil)
                    return false;
                if (request.sampling) {
                    MTLRenderPassDescriptor* pass =
                        [MTLRenderPassDescriptor renderPassDescriptor];
                    pass.colorAttachments[0].texture = drawTarget;
                    pass.colorAttachments[0].loadAction =
                        MTLLoadActionDontCare;
                    pass.colorAttachments[0].storeAction =
                        MTLStoreActionStore;
                    id<MTLRenderCommandEncoder> draw =
                        [commands renderCommandEncoderWithDescriptor:pass];
                    if (draw == nil)
                        return false;
                    const MTLViewport viewport {
                        .originX = 0.0,
                        .originY = 0.0,
                        .width = static_cast<double>(
                            request.placement.width),
                        .height = static_cast<double>(
                            request.placement.height),
                        .znear = 0.0,
                        .zfar = 1.0,
                    };
                    const MTLScissorRect scissor {
                        .x = footprint.x,
                        .y = footprint.y,
                        .width = footprint.width,
                        .height = footprint.height,
                    };
                    [draw setScissorRect:scissor];
                    encodeScanoutPass(
                        draw,
                        pipeline,
                        request.texture,
                        *request.sampling,
                        DisplayRotation::upright,
                        viewport);
                    [draw endEncoding];
                }
                id<MTLBlitCommandEncoder> encoder =
                    [commands blitCommandEncoder];
                if (encoder == nil)
                    return false;
                [encoder
                    copyFromTexture:copySource
                    sourceSlice:0
                    sourceLevel:0
                    sourceOrigin:copyOrigin
                    sourceSize:MTLSizeMake(copy.width, copy.height, 1)
                    toBuffer:buffer
                    destinationOffset:0
                    destinationBytesPerRow:*requestedRowBytes
                    destinationBytesPerImage:requestedSize];
                [encoder endEncoding];

                id<MTLBuffer> completedBuffer = buffer;
                auto state = shared_from_this();
                [commands addCompletedHandler:^(id<MTLCommandBuffer> result) {
                    state->complete(
                        result,
                        completedBuffer,
                        copy,
                        *requestedRowBytes,
                        requestedSize);
                }];
                activeCommands = commands;
                inFlight = true;
                [commands commit];
                return true;
            } catch (...) {
                return false;
            }
        }
    }

    void complete(
        id<MTLCommandBuffer> commands,
        id<MTLBuffer> completedBuffer,
        const ReadbackCopy& copy,
        std::size_t rowBytes,
        std::size_t size) noexcept
    {
        @autoreleasepool {
#if UML_GPU_VNC_STARTUP_TESTING
            testing::readbackCompletionCheckpoint();
#endif
            bool mayPublish = false;
            bool published = false;
            const bool guest = copy.source == ReadbackSource::guest;
            {
                std::lock_guard lock(mutex);
                mayPublish = running &&
                    commands.status == MTLCommandBufferStatusCompleted;
                callbackActive = mayPublish;
            }
            if (mayPublish) {
                try {
                    published = publish(MetalReadbackFrame {
                        .pixels = std::span<const std::uint8_t>(
                            static_cast<const std::uint8_t*>(
                                completedBuffer.contents),
                            size),
                        .rowBytes = rowBytes,
                        .format = copy.format,
                        .width = copy.width,
                        .height = copy.height,
                        .destinationX = copy.destinationX,
                        .destinationY = copy.destinationY,
                        .target = copy.target,
                        .generation = copy.generation,
                        .complete = copy.complete,
                    });
                    if (guest) {
                        (published ? presentedFrames : droppedFrames)
                            .fetch_add(1, std::memory_order_relaxed);
                    }
                } catch (...) {
                    reportFailure();
                }
            } else if (commands.status != MTLCommandBufferStatusCompleted) {
                displayActivityLog().record(
                    "readback incomplete status=%lu error=%ld",
                    static_cast<unsigned long>(commands.status),
                    static_cast<long>(commands.error.code));
                reportFailure();
            }

            std::lock_guard lock(mutex);
            callbackActive = false;
            activeCommands = nil;
            inFlight = false;
            /* A frame that never reached the framebuffer leaves it without
             * pixels the next damage-bounded request would rely on. */
            if (!published)
                coverNext = true;
            if (!running || !pending) {
                stateChanged.notify_all();
                return;
            }
            auto next = std::move(*pending);
            pending.reset();
            if (!published)
                coverView(next);
            if (!scheduleLocked(std::move(next))) {
                coverNext = true;
                reportFailure();
            }
            stateChanged.notify_all();
        }
    }

    /* A request for a target other than the previous request's reads its
     * whole view: the framebuffer holds nothing of this target's image
     * outside the damage. */
    void placeLocked(ReadbackRequest& request) noexcept
    {
        if (coverNext || (lastTarget && *lastTarget != request.target()))
            coverView(request);
        lastTarget = request.target();
        coverNext = false;
    }

    PresentOutcome present(
        const MetalScanout& scanout,
        ReadbackTarget target,
        std::uint32_t generation,
        ReadbackSource source) noexcept
    {
        const bool guest = source == ReadbackSource::guest;
        if (guest)
            guestFrames.fetch_add(1, std::memory_order_relaxed);
        auto next = request(scanout, target);
        if (!next)
            return PresentOutcome::unsupported;
        next->generation = generation;
        next->source = source;

        std::lock_guard lock(mutex);
        reportStatisticsLocked(false);
        if (!running)
            return PresentOutcome::unsupported;
        latest.reset();
        placeLocked(*next);
        if (!inFlight) {
            if (!scheduleLocked(std::move(*next))) {
                displayActivityLog().record("readback reject=schedule");
                /* Nothing reached the framebuffer, so the next request
                 * must not rely on this one's pixels. */
                coverNext = true;
                return PresentOutcome::unsupported;
            }
            if (!directPathReported.exchange(
                    true, std::memory_order_relaxed)) {
                std::fputs(
                    "VNC display: asynchronous native Metal readback\n",
                    stderr);
            }
            return PresentOutcome::presented;
        }

        const bool superseded = pending.has_value();
        if (superseded)
            absorb(*next, *pending);
        pending = std::move(*next);
        if (superseded && guest)
            droppedFrames.fetch_add(1, std::memory_order_relaxed);
        return superseded ? PresentOutcome::dropped
                          : PresentOutcome::presented;
    }

    bool remember(
        const MetalScanout& scanout,
        ReadbackTarget target,
        ReadbackSource source) noexcept
    {
        auto next = request(wholeView(scanout), target);
        if (next)
            next->source = source;
        std::lock_guard lock(mutex);
        latest.reset();
        if (running)
            latest = std::move(next);
        return latest.has_value();
    }

    bool refresh(std::uint32_t generation) noexcept
    {
        std::lock_guard lock(mutex);
        if (!running || !latest)
            return false;
        auto next = std::move(*latest);
        latest.reset();
        next.generation = generation;
        placeLocked(next);
        if (!inFlight) {
            if (scheduleLocked(std::move(next)))
                return true;
            displayActivityLog().record("readback reject=schedule");
            coverNext = true;
            return false;
        }
        if (pending)
            absorb(next, *pending);
        pending = std::move(next);
        return true;
    }

    void forget() noexcept
    {
        std::lock_guard lock(mutex);
        latest.reset();
    }

    std::optional<MetalScanout> stage(
        std::span<const std::uint8_t> image,
        std::size_t rowBytes,
        DisplaySize mode,
        DisplayRectangle damage) noexcept
    {
        @autoreleasepool {
            try {
                if (mode.width == 0 || mode.height == 0 ||
                    rowBytes < static_cast<std::size_t>(mode.width) *
                            protocol::gpuBytesPerPixel ||
                    image.size() / rowBytes < mode.height) {
                    return std::nullopt;
                }
                id<MTLDevice> stagingDevice;
                {
                    std::lock_guard lock(mutex);
                    if (device == nil &&
                        !ensureDeviceLocked(MTLCreateSystemDefaultDevice())) {
                        return std::nullopt;
                    }
                    stagingDevice = device;
                }
                bool created = false;
                if (stagingTexture == nil ||
                    stagingTexture.width != mode.width ||
                    stagingTexture.height != mode.height) {
                    stagingTexture = nil;
                    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor
                        texture2DDescriptorWithPixelFormat:
                            MTLPixelFormatBGRA8Unorm
                        width:mode.width
                        height:mode.height
                        mipmapped:NO];
                    descriptor.storageMode = MTLStorageModeShared;
                    descriptor.usage = MTLTextureUsageShaderRead;
                    stagingTexture =
                        [stagingDevice newTextureWithDescriptor:descriptor];
                    if (stagingTexture == nil)
                        return std::nullopt;
                    created = true;
                }
                if (created) {
                    damage = {.width = mode.width, .height = mode.height};
                } else if (damage.width == 0 || damage.height == 0 ||
                           !rangeFits(damage.x, damage.width, mode.width) ||
                           !rangeFits(
                               damage.y, damage.height, mode.height)) {
                    return std::nullopt;
                }
                /* The guest's next flush may race the draw that reads this
                 * texture; it can tear that one readback, and the request
                 * the flush queues then reads the finished image. */
                [stagingTexture
                    replaceRegion:MTLRegionMake2D(
                        damage.x, damage.y, damage.width, damage.height)
                    mipmapLevel:0
                    withBytes:image.data() +
                        static_cast<std::size_t>(damage.y) * rowBytes +
                        static_cast<std::size_t>(damage.x) *
                            protocol::gpuBytesPerPixel
                    bytesPerRow:rowBytes];
                return MetalScanout {
                    .texture = (__bridge void*)stagingTexture,
                    .sourceWidth = mode.width,
                    .sourceHeight = mode.height,
                    .width = mode.width,
                    .height = mode.height,
                    .damageX = damage.x,
                    .damageY = damage.y,
                    .damageWidth = damage.width,
                    .damageHeight = damage.height,
                };
            } catch (...) {
                return std::nullopt;
            }
        }
    }

    void releaseStaging() noexcept
    {
        stagingTexture = nil;
    }

    void reportFailure() noexcept
    {
        if (!failureReported.exchange(true, std::memory_order_relaxed)) {
            std::fputs("VNC Metal readback failed\n", stderr);
        }
    }

    /* The guest and host frame counters are the only evidence that separates
     * a display which never received a scanout from one which received and
     * discarded every frame. */
    void reportStatisticsLocked(bool final) noexcept
    {
        auto& activity = displayActivityLog();
        if (!activity.enabled())
            return;
        const auto now = std::chrono::steady_clock::now();
        if (!final && now - reportedAt < std::chrono::seconds(1))
            return;
        reportedAt = now;
        recordDisplayStatistics("vnc", final, snapshot());
    }

    DisplayStatistics snapshot() const noexcept
    {
        return DisplayStatistics {
            .guestFrames = guestFrames.load(std::memory_order_relaxed),
            .presentedFrames =
                presentedFrames.load(std::memory_order_relaxed),
            .droppedFrames = droppedFrames.load(std::memory_order_relaxed),
            .cpuFallbackFrames =
                cpuFallbackFrames.load(std::memory_order_relaxed),
        };
    }

    void shutdown() noexcept
    {
        id<MTLCommandBuffer> commands;
        {
            std::lock_guard lock(mutex);
            if (!running)
                return;
            running = false;
            pending.reset();
            latest.reset();
            commands = activeCommands;
        }
        [commands waitUntilCompleted];
        std::unique_lock lock(mutex);
        stateChanged.wait(lock, [this] {
            return !inFlight && !callbackActive;
        });
        reportStatisticsLocked(true);
        publish = {};
    }

    MetalReadback::PublishCallback publish;
    mutable std::mutex mutex;
    std::condition_variable stateChanged;
    id<MTLDevice> device;
    id<MTLCommandQueue> commandQueue;
    id<MTLBuffer> buffer;
    id<MTLCommandBuffer> activeCommands;
    /* Scaled placements are drawn here before they are read back. */
    id<MTLTexture> drawTarget;
    std::optional<ScanoutPipelines> unormPipelines;
    std::optional<ScanoutPipelines> srgbPipelines;
    /* The display's CPU image of the guest mode, for a CPU fallback that
     * must be scaled. Owned by the display's thread. */
    id<MTLTexture> stagingTexture;
    std::optional<ReadbackRequest> pending;
    /* The whole view of the newest scanout nobody read back. */
    std::optional<ReadbackRequest> latest;
    std::optional<ReadbackTarget> lastTarget;
    bool coverNext {};
    bool running {true};
    bool inFlight {};
    bool callbackActive {};
    std::atomic_bool failureReported {};
    std::atomic_bool directPathReported {};
    std::atomic_uint64_t guestFrames {};
    std::atomic_uint64_t presentedFrames {};
    std::atomic_uint64_t droppedFrames {};
    std::atomic_uint64_t cpuFallbackFrames {};
    std::chrono::steady_clock::time_point reportedAt {};
};

MetalReadback::MetalReadback(PublishCallback publish)
    : state_(std::make_shared<MetalReadbackState>(std::move(publish)))
{
}

MetalReadback::~MetalReadback()
{
    shutdown();
}

PresentOutcome MetalReadback::present(
    const MetalScanout& scanout,
    ReadbackTarget target,
    std::uint32_t generation,
    ReadbackSource source) noexcept
{
    return state_->present(scanout, target, generation, source);
}

bool MetalReadback::remember(
    const MetalScanout& scanout,
    ReadbackTarget target,
    ReadbackSource source) noexcept
{
    return state_->remember(scanout, target, source);
}

bool MetalReadback::refresh(std::uint32_t generation) noexcept
{
    return state_->refresh(generation);
}

void MetalReadback::forget() noexcept
{
    state_->forget();
}

std::optional<MetalScanout> MetalReadback::stage(
    std::span<const std::uint8_t> image,
    std::size_t rowBytes,
    DisplaySize mode,
    DisplayRectangle damage) noexcept
{
    return state_->stage(image, rowBytes, mode, damage);
}

void MetalReadback::releaseStaging() noexcept
{
    state_->releaseStaging();
}

DisplayStatistics MetalReadback::statistics() const noexcept
{
    return state_->snapshot();
}

void MetalReadback::recordCpuFallbackFrame() noexcept
{
    state_->cpuFallbackFrames.fetch_add(1, std::memory_order_relaxed);
}

void MetalReadback::shutdown() noexcept
{
    if (state_)
        state_->shutdown();
}

#if UML_GPU_VNC_STARTUP_TESTING
bool MetalReadback::idleForTesting() const noexcept
{
    std::lock_guard lock(state_->mutex);
    return !state_->inFlight && !state_->pending;
}
#endif

} // namespace uml::gpu
