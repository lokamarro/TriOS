// SPDX-License-Identifier: GPL-2.0-or-later

/* The Venus scanout is a private Metal texture with no guest-visible Virgl
 * resource, so a VNC client can only ever see it through the display's native
 * readback. `gpu-vnc-lifetime` pins that readback's pixels, but it compiles
 * the display source into a test target with its own definitions, so it
 * cannot tell whether the shipped emulator contains that path at all. This
 * test links the production display library and reads the pixels back over a
 * real RFB connection, which is the contract a guest actually depends on. It
 * also pins what a client sees when it connects after scanouts were dropped
 * because nobody was watching: the newest scanout, never an older frame.
 *
 * A guest mode need not match the VNC framebuffer: glmark2-es2-drm picks
 * 4096x2160 on a 1280x720 display. The client must see the whole guest image
 * aspect-fitted and centred, from the native readback and from the CPU path
 * alike, and its pointer must land on the guest pixel it points at. */

#include "display.hpp"
#include "display_geometry.hpp"
#include "gpu_metal_test_support.hpp"
#include "require.hpp"
#include "vnc_test_client.hpp"
#include "wait_until.hpp"

#import <Metal/Metal.h>

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <netinet/in.h>
#include <span>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

using uml::gpu::testing::solidImage;
using uml::gpu::testing::uploadPrivateTexture;
using uml::gpu::testing::wholeScanout;
using uml::testing::require;

constexpr std::uint32_t displayExtent = 64;
constexpr std::uint32_t cursorExtent = 32;
using Pixel = std::array<std::uint8_t, 4>;
constexpr Pixel expectedPixel {0x19U, 0x38U, 0x57U, 0xffU};
constexpr Pixel idlePixel {0x6aU, 0x21U, 0xc4U, 0xffU};
/* BGRA. Each corner of a fitted guest image has its own colour. */
constexpr Pixel backgroundPixel {0x80U, 0x80U, 0x80U, 0xffU};
constexpr Pixel topLeftPixel {0x00U, 0x00U, 0xffU, 0xffU};
constexpr Pixel topRightPixel {0x00U, 0xffU, 0x00U, 0xffU};
constexpr Pixel bottomLeftPixel {0xffU, 0x00U, 0x00U, 0xffU};
constexpr Pixel bottomRightPixel {0xffU, 0xffU, 0xffU, 0xffU};
constexpr Pixel blackPixel {0x00U, 0x00U, 0x00U, 0xffU};
constexpr std::uint32_t vncWidth = 1280;
constexpr std::uint32_t vncHeight = 720;
constexpr auto operationTimeout = uml::gpu::testing::vncClientTimeout;
using Connection = uml::gpu::testing::VncConnection;

/* A Venus scanout is an optimal render target in a private Metal heap;
 * nothing on the host can read it without a blit. */
id<MTLTexture> makePrivateScanoutTexture(
    id<MTLDevice> device,
    const Pixel& pixel)
{
    return uploadPrivateTexture(
        device,
        solidImage(pixel, displayExtent, displayExtent),
        displayExtent,
        displayExtent);
}

[[nodiscard]] uml::gpu::MetalScanout wholeScanout(id<MTLTexture> texture)
{
    return wholeScanout(texture, displayExtent, displayExtent);
}

void requireImage(
    const std::vector<std::uint8_t>& image,
    const Pixel& pixel,
    const char* message)
{
    for (std::uint32_t y = 0; y < displayExtent; ++y) {
        for (std::uint32_t x = 0; x < displayExtent; ++x) {
            /* LibVNCServer composites its default cursor into the pixels it
             * sends a client that asked for no cursor encoding, so the
             * top-left corner is the server's, not the scanout's. */
            if (x < cursorExtent && y < cursorExtent)
                continue;
            const auto offset =
                (static_cast<std::size_t>(y) * displayExtent + x) * 4U;
            if (image[offset] == pixel[0] &&
                image[offset + 1U] == pixel[1] &&
                image[offset + 2U] == pixel[2]) {
                continue;
            }
            std::fprintf(
                stderr,
                "VNC pixel at %u,%u is %02x%02x%02x, expected %02x%02x%02x\n",
                x, y, image[offset], image[offset + 1U],
                image[offset + 2U], pixel[0], pixel[1], pixel[2]);
            require(false, message);
        }
    }
}

template<typename Predicate>
void waitFor(Predicate predicate, const char* message)
{
    require(uml::testing::waitUntil(predicate, operationTimeout), message);
}

struct ConnectedDisplay final {
    std::unique_ptr<uml::gpu::Display> display;
    std::unique_ptr<Connection> connection;
};

/* A VNC display with one client, counted before the display returns: the
 * display drops a scanout while no client is watching. */
[[nodiscard]] ConnectedDisplay connectedDisplay(
    std::uint32_t width = vncWidth,
    std::uint32_t height = vncHeight,
    uml::input::vnc::VncInputSink sink = {},
    uml::input::vnc::PointerMode pointerMode =
        uml::input::vnc::PointerMode::mouse)
{
    ConnectedDisplay connected;
    connected.display = uml::gpu::makeDisplay(
        uml::gpu::DisplayMode::vnc, "127.0.0.1", 0, {}, width, height,
        std::move(sink), nullptr, pointerMode);
    require(connected.display != nullptr, "VNC display creation failed");
    const auto port = connected.display->vncPort();
    require(port.has_value() && *port != 0, "VNC display has no port");
    connected.connection = std::make_unique<Connection>(*port);
    connected.connection->handshake();
    waitFor(
        [&] { return connected.display->needsFullCpuFallback(); },
        "no VNC client was counted");
    return connected;
}

void shippedVncDisplayReadsBackAVenusScanout()
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        require(device != nil, "Metal device unavailable");
        auto connected = connectedDisplay(displayExtent, displayExtent);
        auto& display = *connected.display;

        id<MTLTexture> texture =
            makePrivateScanoutTexture(device, expectedPixel);
        const auto outcome = display.presentMetal(wholeScanout(texture));
        require(
            outcome == uml::gpu::PresentOutcome::presented,
            "the shipped VNC display refused a native Metal scanout");
        require(
            display.statistics().guestFrames == 1,
            "the shipped VNC display did not count the guest frame");
        waitFor(
            [&] { return display.statistics().presentedFrames != 0; },
            "the native Metal readback never completed");
        require(
            display.statistics().presentedFrames == 1,
            "the native Metal readback completed more than once");

        requireImage(
            connected.connection->readFramebuffer(),
            expectedPixel,
            "the VNC framebuffer did not carry the scanout's pixels");
    }
}

/* With no client the display drops each scanout without reading it back, so
 * its framebuffer keeps the last frame an earlier client saw. A client that
 * connects later must still receive the scanout current at its connection,
 * even when the guest does not flush again. */
void clientAfterAnIdleIntervalSeesTheNewestScanout()
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        require(device != nil, "Metal device unavailable");
        auto connected = connectedDisplay(displayExtent, displayExtent);
        auto& display = *connected.display;

        id<MTLTexture> seen = makePrivateScanoutTexture(device, expectedPixel);
        id<MTLTexture> idle = makePrivateScanoutTexture(device, idlePixel);
        require(
            display.presentMetal(wholeScanout(seen)) ==
                uml::gpu::PresentOutcome::presented,
            "the shipped VNC display refused a native Metal scanout");
        waitFor(
            [&] { return display.statistics().presentedFrames != 0; },
            "the native Metal readback never completed");
        requireImage(
            connected.connection->readFramebuffer(),
            expectedPixel,
            "the watching client did not see the scanout");
        connected.connection.reset();

        /* Presenting the frame the framebuffer already holds is harmless
         * while the departed client is still counted. Only a display with no
         * client drops a frame without offering it to the readback, which
         * counts every frame it is offered. */
        waitFor(
            [&] {
                const auto offered = display.statistics().guestFrames;
                return display.presentMetal(wholeScanout(seen)) ==
                        uml::gpu::PresentOutcome::dropped &&
                    display.statistics().guestFrames == offered;
            },
            "the display kept counting a departed client");
        require(
            display.presentMetal(wholeScanout(idle)) ==
                uml::gpu::PresentOutcome::dropped,
            "the display read back a scanout with no client");

        Connection late(*display.vncPort());
        late.handshake();
        requireImage(
            late.readFramebuffer(),
            idlePixel,
            "a client connecting after an idle interval saw an older frame");
    }
}

/* The guest image: background with a coloured block in each corner, stored
 * bottom row first when flipped. */
[[nodiscard]] std::vector<std::uint8_t> cornerImage(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t block,
    bool flipped = false)
{
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(width) * height * 4U);
    for (std::uint32_t y = 0; y < height; ++y) {
        const bool top = y < block;
        const bool bottom = y >= height - block;
        for (std::uint32_t x = 0; x < width; ++x) {
            const bool left = x < block;
            const bool right = x >= width - block;
            const Pixel& pixel = top && left ? topLeftPixel :
                top && right                 ? topRightPixel :
                bottom && left               ? bottomLeftPixel :
                bottom && right              ? bottomRightPixel :
                                               backgroundPixel;
            const auto row = flipped ? height - 1U - y : y;
            std::memcpy(
                &pixels[(static_cast<std::size_t>(row) * width + x) * 4U],
                pixel.data(),
                pixel.size());
        }
    }
    return pixels;
}

id<MTLTexture> makeCornerTexture(
    id<MTLDevice> device,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t block,
    bool flipped = false)
{
    return uploadPrivateTexture(
        device, cornerImage(width, height, block, flipped), width, height);
}

/* Flushes a 2D guest's whole width x height image, as a scanout flush does. */
void updateWhole(
    uml::gpu::Display& display,
    std::span<const std::uint8_t> pixels,
    std::uint32_t width,
    std::uint32_t height)
{
    display.update(
        pixels,
        width,
        height,
        static_cast<std::size_t>(width) * 4U,
        uml::gpu::protocol::GpuFormat::b8g8r8x8Unorm,
        0,
        0,
        0,
        0,
        width,
        height);
}

void requirePixel(
    const std::vector<std::uint8_t>& image,
    std::uint32_t x,
    std::uint32_t y,
    const Pixel& pixel,
    const char* message)
{
    const auto offset = (static_cast<std::size_t>(y) * vncWidth + x) * 4U;
    if (image[offset] == pixel[0] && image[offset + 1U] == pixel[1] &&
        image[offset + 2U] == pixel[2]) {
        return;
    }
    std::fprintf(
        stderr,
        "VNC pixel at %u,%u is %02x%02x%02x, expected %02x%02x%02x\n",
        x, y, image[offset], image[offset + 1U], image[offset + 2U],
        pixel[0], pixel[1], pixel[2]);
    require(false, message);
}

/* A guest image with corner blocks of block texels, fitted into placement:
 * each block keeps its colour at its centre, the image's edge rows and
 * columns are the guest's, and everything outside the placement is black. */
void requireFittedImage(
    const std::vector<std::uint8_t>& image,
    uml::gpu::DisplayRectangle placement,
    double blockPixels,
    const char* message)
{
    const auto near = static_cast<std::uint32_t>(blockPixels / 2.0);
    const auto left = placement.x + near;
    const auto right = placement.x + placement.width - 1U - near;
    const auto top = placement.y + near;
    const auto bottom = placement.y + placement.height - 1U - near;
    const auto middleX = placement.x + placement.width / 2U;
    const auto middleY = placement.y + placement.height / 2U;
    requirePixel(image, left, top, topLeftPixel, message);
    requirePixel(image, right, top, topRightPixel, message);
    requirePixel(image, left, bottom, bottomLeftPixel, message);
    requirePixel(image, right, bottom, bottomRightPixel, message);
    requirePixel(image, middleX, middleY, backgroundPixel, message);
    requirePixel(image, middleX, placement.y, backgroundPixel, message);
    requirePixel(
        image, middleX, placement.y + placement.height - 1U,
        backgroundPixel, message);
    requirePixel(image, placement.x, middleY, backgroundPixel, message);
    requirePixel(
        image, placement.x + placement.width - 1U, middleY,
        backgroundPixel, message);
    if (placement.y != 0) {
        requirePixel(image, middleX, 0, blackPixel, message);
        requirePixel(image, middleX, placement.y - 1U, blackPixel, message);
        requirePixel(
            image, middleX, placement.y + placement.height, blackPixel,
            message);
        requirePixel(image, middleX, vncHeight - 1U, blackPixel, message);
    }
    if (placement.x != 0) {
        requirePixel(image, placement.x - 1U, middleY, blackPixel, message);
        requirePixel(
            image, placement.x + placement.width, middleY, blackPixel,
            message);
        requirePixel(image, vncWidth - 1U, middleY, blackPixel, message);
    }
}

/* glmark2-es2-drm's 4096x2160 mode on a 1280x720 framebuffer: the whole
 * image, scaled by 5/16 on the GPU, fills the width and leaves 23 black rows
 * above and 22 below. The native readback handles a bottom-up texture the
 * same way, and neither falls back to the CPU. */
void oversizedNativeScanoutIsAspectFitted()
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        require(device != nil, "Metal device unavailable");
        auto connected = connectedDisplay();
        auto& display = *connected.display;
        constexpr std::uint32_t width = 4096;
        constexpr std::uint32_t height = 2160;
        constexpr std::uint32_t block = 256;
        const auto placement = uml::gpu::aspectFitPixels(
            {vncWidth, vncHeight}, {width, height});
        require(
            placement == uml::gpu::DisplayRectangle {0, 23, 1280, 675},
            "the oversized mode's placement changed");
        require(
            display.guestScanoutChanged(width, height),
            "the VNC display refused an oversized mode");

        id<MTLTexture> upright = makeCornerTexture(device, width, height, block);
        require(
            display.presentMetal(wholeScanout(upright, width, height)) ==
                uml::gpu::PresentOutcome::presented,
            "the VNC display refused an oversized native scanout");
        waitFor(
            [&] { return display.statistics().presentedFrames == 1; },
            "the scaled native readback never completed");
        requireFittedImage(
            connected.connection->readFramebuffer(),
            placement,
            block * 5.0 / 16.0,
            "an oversized native scanout was not aspect-fitted");

        id<MTLTexture> flipped =
            makeCornerTexture(device, width, height, block, true);
        require(
            display.presentMetal(
                wholeScanout(flipped, width, height, true)) ==
                uml::gpu::PresentOutcome::presented,
            "the VNC display refused a bottom-up native scanout");
        waitFor(
            [&] { return display.statistics().presentedFrames == 2; },
            "the bottom-up native readback never completed");
        requireFittedImage(
            connected.connection->readFramebuffer(),
            placement,
            block * 5.0 / 16.0,
            "a bottom-up native scanout was not turned upright");

        const auto statistics = display.statistics();
        require(
            statistics.guestFrames == 2 &&
                statistics.cpuFallbackFrames == 0,
            "an oversized native scanout took the CPU fallback");
    }
}

/* A 2D guest at the same mode reaches the display through update(). The CPU
 * image is staged in a host texture and scaled by the same pass, so the client
 * sees the same fitted image, and the frame is not counted as a native one. */
void oversizedCpuScanoutIsAspectFitted()
{
    @autoreleasepool {
        auto connected = connectedDisplay();
        auto& display = *connected.display;
        constexpr std::uint32_t width = 4096;
        constexpr std::uint32_t height = 2160;
        constexpr std::uint32_t block = 256;
        require(
            display.guestScanoutChanged(width, height),
            "the VNC display refused an oversized mode");
        const auto pixels = cornerImage(width, height, block);
        updateWhole(display, pixels, width, height);
        waitFor(
            [&] { return !display.needsFullCpuFallback(); },
            "the scaled CPU frame never reached the framebuffer");
        requireFittedImage(
            connected.connection->readFramebuffer(),
            uml::gpu::aspectFitPixels({vncWidth, vncHeight}, {width, height}),
            block * 5.0 / 16.0,
            "an oversized CPU scanout was not aspect-fitted");
        const auto statistics = display.statistics();
        require(
            statistics.guestFrames == 0 && statistics.presentedFrames == 0,
            "a staged CPU frame was counted as a native one");
    }
}

/* A scaled CPU frame flushed while nobody watches is kept staged, and a
 * client that connects later receives it fitted at once, still without
 * counting it as a native frame. */
void lateClientReceivesAStagedCpuFrame()
{
    @autoreleasepool {
        auto display = uml::gpu::makeDisplay(
            uml::gpu::DisplayMode::vnc, "127.0.0.1", 0, {}, vncWidth,
            vncHeight, {}, nullptr, uml::input::vnc::PointerMode::mouse);
        require(display != nullptr, "VNC display creation failed");
        constexpr std::uint32_t width = 4096;
        constexpr std::uint32_t height = 2160;
        constexpr std::uint32_t block = 256;
        require(
            display->guestScanoutChanged(width, height),
            "the VNC display refused an oversized mode");
        const auto pixels = cornerImage(width, height, block);
        updateWhole(*display, pixels, width, height);
        const auto port = display->vncPort();
        require(port.has_value() && *port != 0, "VNC display has no port");
        Connection connection {*port};
        connection.handshake();
        requireFittedImage(
            connection.readFramebuffer(),
            uml::gpu::aspectFitPixels({vncWidth, vncHeight}, {width, height}),
            block * 5.0 / 16.0,
            "a late client did not receive the staged CPU frame");
        const auto statistics = display->statistics();
        require(
            statistics.guestFrames == 0 && statistics.presentedFrames == 0,
            "a refreshed staged CPU frame was counted as a native one");
    }
}

/* A mode smaller than the framebuffer is magnified into the middle, with
 * nearest filtering, and a guest reset returns to the preferred mode, which
 * is copied pixel for pixel. */
void smallerModesAreMagnifiedAndResetRestoresThePreferredMode()
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        require(device != nil, "Metal device unavailable");
        auto connected = connectedDisplay();
        auto& display = *connected.display;

        id<MTLTexture> small = makeCornerTexture(device, 640, 480, 64);
        require(
            display.guestScanoutChanged(640, 480),
            "the VNC display refused a smaller mode");
        require(
            display.presentMetal(wholeScanout(small, 640, 480)) ==
                uml::gpu::PresentOutcome::presented,
            "the VNC display refused a smaller native scanout");
        waitFor(
            [&] { return display.statistics().presentedFrames == 1; },
            "the magnified native readback never completed");
        const auto pillarbox = uml::gpu::aspectFitPixels(
            {vncWidth, vncHeight}, {640, 480});
        require(
            pillarbox == uml::gpu::DisplayRectangle {160, 0, 960, 720},
            "the smaller mode's placement changed");
        requireFittedImage(
            connected.connection->readFramebuffer(),
            pillarbox,
            64 * 1.5,
            "a smaller native scanout was not magnified into a pillarbox");

        display.resetGuestSession();
        display.clear();
        id<MTLTexture> preferred =
            makeCornerTexture(device, vncWidth, vncHeight, 64);
        require(
            display.presentMetal(wholeScanout(preferred, vncWidth, vncHeight)) ==
                uml::gpu::PresentOutcome::presented,
            "the VNC display refused the preferred mode after a reset");
        waitFor(
            [&] { return display.statistics().presentedFrames == 2; },
            "the preferred-mode readback never completed");
        const auto image = connected.connection->readFramebuffer();
        requireFittedImage(
            image,
            {0, 0, vncWidth, vncHeight},
            64,
            "the preferred mode did not fill the framebuffer after a reset");
        const auto expected = cornerImage(vncWidth, vncHeight, 64);
        for (std::uint32_t y = cursorExtent; y < vncHeight; y += 7U) {
            for (std::uint32_t x = 0; x < vncWidth; x += 5U) {
                const auto offset =
                    (static_cast<std::size_t>(y) * vncWidth + x) * 4U;
                require(
                    std::memcmp(&image[offset], &expected[offset], 3) == 0,
                    "the preferred mode was not copied pixel for pixel");
            }
        }
    }
}

class RecordedInput final {
public:
    [[nodiscard]] uml::input::vnc::VncInputSink sink()
    {
        return [this](std::span<const uml::input::vnc::VncInputEvent> events) {
            std::lock_guard lock(mutex_);
            batches_.emplace_back(events.begin(), events.end());
        };
    }

    /* Waits for the next count batches and returns them. */
    [[nodiscard]] std::vector<std::vector<uml::input::vnc::VncInputEvent>>
    take(std::size_t count)
    {
        waitFor(
            [&] {
                std::lock_guard lock(mutex_);
                return batches_.size() >= count;
            },
            "the VNC display delivered no pointer input");
        std::lock_guard lock(mutex_);
        auto taken = std::move(batches_);
        batches_.clear();
        return taken;
    }

private:
    std::mutex mutex_;
    std::vector<std::vector<uml::input::vnc::VncInputEvent>> batches_;
};

[[nodiscard]] std::int32_t eventValue(
    const std::vector<uml::input::vnc::VncInputEvent>& batch,
    std::uint16_t type,
    std::uint16_t code)
{
    for (const auto& event : batch) {
        if (event.type == type && event.code == code)
            return event.value;
    }
    require(false, "a VNC pointer report lacked an expected event");
    return 0;
}

/* The centre of VNC pixel 640,360 is the centre of the fitted 4096x2160
 * image, and the bars around it are not the guest's. RFB delivers events in
 * order, so a bar event that reached the guest would arrive before the next
 * event's report. */
void pointerInputMapsThroughTheFittedRectangle()
{
    namespace input = uml::input::vnc;
    RecordedInput mouseInput;
    auto mouse = connectedDisplay(vncWidth, vncHeight, mouseInput.sink());
    require(
        mouse.display->guestScanoutChanged(4096, 2160),
        "the VNC display refused an oversized mode");
    mouse.connection->pointer(0, 640, 10);
    mouse.connection->pointer(1, 640, 10);
    mouse.connection->pointer(0, 640, 10);
    mouse.connection->pointer(0, 640, 360);
    auto batches = mouseInput.take(1);
    require(batches.size() == 1, "a pointer in a letterbox bar reached the guest");
    require(
        eventValue(batches[0], input::eventAbsolute, input::absoluteX) ==
                input::normalizedAbsoluteCoordinate(640.5 / 1280.0, 1.0) &&
            eventValue(batches[0], input::eventAbsolute, input::absoluteY) ==
                input::normalizedAbsoluteCoordinate(0.5, 1.0),
        "a VNC pointer did not map through the fitted rectangle");

    RecordedInput touchInput;
    auto touch = connectedDisplay(
        vncWidth,
        vncHeight,
        touchInput.sink(),
        input::PointerMode::touchscreen);
    require(
        touch.display->guestScanoutChanged(4096, 2160),
        "the VNC display refused an oversized mode");
    touch.connection->pointer(1, 640, 10);
    touch.connection->pointer(1, 640, 360);
    batches = touchInput.take(2);
    require(
        batches.size() == 2 &&
            eventValue(batches[0], input::eventAbsolute, input::absoluteY) ==
                0 &&
            eventValue(batches[0], input::eventKey, input::buttonTouch) == 1 &&
            eventValue(batches[1], input::eventAbsolute, input::absoluteY) ==
                input::normalizedAbsoluteCoordinate(0.5, 1.0),
        "a touch entering the fitted rectangle did not begin at its edge");
    touch.connection->pointer(0, 640, 360);
    batches = touchInput.take(1);
    require(
        batches.size() == 1 &&
            eventValue(batches[0], input::eventKey, input::buttonTouch) == 0,
        "lifting a touch inside the guest did not end its contact");
}

} // namespace

int main()
{
    try {
        shippedVncDisplayReadsBackAVenusScanout();
        clientAfterAnIdleIntervalSeesTheNewestScanout();
        oversizedNativeScanoutIsAspectFitted();
        oversizedCpuScanoutIsAspectFitted();
        lateClientReceivesAStagedCpuFrame();
        smallerModesAreMagnifiedAndResetRestoresThePreferredMode();
        pointerInputMapsThroughTheFittedRectangle();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "VNC product display test: %s\n", error.what());
        return 1;
    }
    std::puts("VNC product display tests passed");
    return 0;
}
