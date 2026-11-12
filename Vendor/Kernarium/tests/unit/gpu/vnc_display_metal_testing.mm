// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu_metal_test_support.hpp"
#include "require.hpp"
#include "vnc_display.hpp"
#include "vnc_display_testing.hpp"
#include "wait_until.hpp"

#import <Metal/Metal.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace uml::gpu::testing {
namespace {

using uml::testing::require;

constexpr std::array<std::uint8_t, 4> expectedPixel {
    0x19U, 0x38U, 0x57U, 0xffU};

} // namespace

void verifyPrivateMetalTextureReadback(VncDisplay& display)
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        require(device != nil, "Metal device unavailable");

        const auto width = display.width();
        const auto height = display.height();
        id<MTLTexture> texture = uploadPrivateTexture(
            device, solidImage(expectedPixel, width, height), width, height,
            true);

        constexpr std::uint32_t damageX = 7;
        constexpr std::uint32_t damageY = 11;
        constexpr std::uint32_t damageWidth = 13;
        constexpr std::uint32_t damageHeight = 9;
        require(presentAccepted(display.presentMetal(damagedScanout(
                    texture,
                    width,
                    height,
                    Damage {damageX, damageY, damageWidth, damageHeight}))),
                "private Metal texture readback was rejected");

        require(uml::testing::waitUntil(
                    [&] { return display.statistics().presentedFrames != 0; },
                    std::chrono::seconds(2)) &&
                    display.statistics().presentedFrames == 1,
                "private Metal texture readback did not complete");
        require(display.pixelForTesting(damageX, damageY) == expectedPixel,
                "private Metal texture readback pixel mismatch");
        require(display.pixelForTesting(0, 0) ==
                    std::array<std::uint8_t, 4> {},
                "Metal readback exceeded its damage rectangle");
    }
}

/* A 640x360 mode on a 160x90 framebuffer is minified 4:1 on the GPU. A flush
 * damaging one 64x64 block redraws and reads back only that block's
 * footprint, 16x16 pixels and one more on each side, even though the rest of
 * the texture has changed too. */
void verifyScaledReadbackIsDamageBounded(VncDisplay& display)
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        require(device != nil, "Metal device unavailable");
        constexpr std::uint32_t width = 640;
        constexpr std::uint32_t height = 360;
        constexpr std::array<std::uint8_t, 4> before {
            0x21U, 0x42U, 0x63U, 0xffU};
        constexpr std::array<std::uint8_t, 4> after {
            0x7aU, 0x15U, 0x3cU, 0xffU};
        require(display.width() == 160 && display.height() == 90,
                "scaled readback test expects a 160x90 display");
        require(display.guestScanoutChanged(width, height),
                "the VNC display refused the scaled mode");

        id<MTLTexture> texture = sharedTexture(device, width, height);
        const auto present = [&](std::uint32_t x,
                                 std::uint32_t y,
                                 std::uint32_t damageWidth,
                                 std::uint32_t damageHeight,
                                 std::uint64_t frames) {
            require(presentAccepted(display.presentMetal(damagedScanout(
                        texture,
                        width,
                        height,
                        Damage {x, y, damageWidth, damageHeight}))),
                    "scaled readback was rejected");
            require(uml::testing::waitUntil(
                        [&] {
                            return display.statistics().presentedFrames >=
                                frames;
                        },
                        std::chrono::seconds(2)) &&
                        display.statistics().presentedFrames == frames,
                    "scaled readback did not complete");
        };

        fillTexture(texture, before);
        present(0, 0, width, height, 1);
        require(display.pixelForTesting(0, 0) == before &&
                    display.pixelForTesting(159, 89) == before,
                "a scaled mode did not fill its placement");

        /* Damage 64x64 at 320,160 reaches pixels 79..96 by 39..56. */
        fillTexture(texture, after);
        present(320, 160, 64, 64, 2);
        require(display.pixelForTesting(80, 40) == after &&
                    display.pixelForTesting(95, 55) == after,
                "scaled damage was not redrawn");
        require(display.pixelForTesting(78, 47) == before &&
                    display.pixelForTesting(97, 47) == before &&
                    display.pixelForTesting(88, 38) == before &&
                    display.pixelForTesting(88, 57) == before &&
                    display.pixelForTesting(0, 0) == before,
                "a scaled readback exceeded its damage footprint");
    }
}

/* clear() blanks the framebuffer, which already holds a complete frame,
 * while one readback is in flight and another waits behind it. Neither may
 * repaint the blank or stand for the complete frame a CPU fallback relies
 * on, and the next scanout, damaged in one corner, is read back whole. A CPU
 * fallback copy made while a readback is in flight is not painted over
 * either. */
void verifyClearDiscardsEarlierReadbacks(VncDisplay& display)
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        require(device != nil, "Metal device unavailable");
        const auto width = display.width();
        const auto height = display.height();
        constexpr std::array<std::uint8_t, 4> blank {};
        constexpr std::array<std::uint8_t, 4> stale {
            0x2dU, 0x5eU, 0x8fU, 0xffU};
        constexpr std::array<std::uint8_t, 4> fresh {
            0x91U, 0x0cU, 0x47U, 0xffU};
        constexpr std::array<std::uint8_t, 4> copied {
            0x0bU, 0x6aU, 0xc4U, 0xffU};
        require(display.needsFullCpuFallback(),
                "clear test expects a client with no complete frame");

        id<MTLTexture> texture = sharedTexture(device, width, height);
        const auto present = [&](std::uint32_t x,
                                 std::uint32_t y,
                                 std::uint32_t damageWidth,
                                 std::uint32_t damageHeight) {
            require(presentAccepted(display.presentMetal(damagedScanout(
                        texture,
                        width,
                        height,
                        Damage {x, y, damageWidth, damageHeight}))),
                    "clear test readback was rejected");
        };
        const auto waitUntil = [](auto predicate, const char* message) {
            require(uml::testing::waitUntil(predicate, std::chrono::seconds(5)),
                    message);
        };

        fillTexture(texture, stale);
        present(0, 0, width, height);
        waitUntil([&] { return !display.needsFullCpuFallback(); },
                  "the first readback was not a complete frame");
        waitUntil([&] { return display.readbackIdleForTesting(); },
                  "the first readback did not finish");

        holdReadbackCompletions();
        present(0, 0, width, height);
        waitUntil([] { return heldReadbackCompletions() == 1; },
                  "the first readback did not complete on the GPU");
        present(0, 0, 8, 8);
        display.clear();
        releaseReadbackCompletions();
        waitUntil([&] { return display.readbackIdleForTesting(); },
                  "readbacks from before clear() did not finish");
        require(display.pixelForTesting(0, 0) == blank &&
                    display.pixelForTesting(width - 1U, height - 1U) ==
                        blank,
                "a readback from before clear() repainted the framebuffer");
        require(display.needsFullCpuFallback(),
                "after clear() the CPU fallback may send damage only");

        fillTexture(texture, fresh);
        present(0, 0, 1, 1);
        waitUntil([&] { return display.readbackIdleForTesting(); },
                  "the readback after clear() did not finish");
        require(display.pixelForTesting(0, 0) == fresh &&
                    display.pixelForTesting(width - 1U, height - 1U) ==
                        fresh,
                "the first readback after clear() did not cover its view");
        require(!display.needsFullCpuFallback(),
                "the first readback after clear() was not a complete frame");

        fillTexture(texture, stale);
        holdReadbackCompletions();
        present(0, 0, width, height);
        waitUntil([] { return heldReadbackCompletions() == 1; },
                  "the readback before the CPU copy did not complete");
        const std::size_t rowBytes =
            static_cast<std::size_t>(width) * protocol::gpuBytesPerPixel;
        const auto image = solidImage(copied, width, height);
        display.update(
            image,
            width,
            height,
            rowBytes,
            protocol::GpuFormat::b8g8r8a8Unorm,
            0,
            0,
            0,
            0,
            width,
            height);
        releaseReadbackCompletions();
        waitUntil([&] { return display.readbackIdleForTesting(); },
                  "the readback before the CPU copy did not finish");
        require(display.pixelForTesting(0, 0) == copied &&
                    display.pixelForTesting(width - 1U, height - 1U) ==
                        copied,
                "a readback from before a CPU copy painted over it");
    }
}

} // namespace uml::gpu::testing
