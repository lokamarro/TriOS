// SPDX-License-Identifier: GPL-2.0-or-later

#include "require.hpp"
#include "scanout_framebuffer.hpp"
#include "vnc_display.hpp"
#include "vnc_display_testing.hpp"
#include "wait_until.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <mutex>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

using uml::testing::require;
using uml::testing::requireThrows;

[[nodiscard]] std::size_t descriptorCount()
{
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator {"/dev/fd"}) {
        (void)entry;
        ++count;
    }
    return count;
}

void packedBgraUpdatesRespectOffsetsAndPaddedStride()
{
    constexpr std::uint32_t sourceWidth = 5;
    constexpr std::uint32_t sourceHeight = 4;
    constexpr std::size_t sourceRowBytes = 24;
    std::array<std::uint8_t, sourceRowBytes * sourceHeight> source {};
    std::fill(source.begin(), source.end(), 0xeeU);
    for (std::uint32_t y = 0; y < sourceHeight; ++y) {
        for (std::uint32_t x = 0; x < sourceWidth; ++x) {
            const auto offset =
                static_cast<std::size_t>(y) * sourceRowBytes +
                static_cast<std::size_t>(x) *
                    uml::gpu::protocol::gpuBytesPerPixel;
            source[offset] = static_cast<std::uint8_t>(0x10U + x);
            source[offset + 1U] = static_cast<std::uint8_t>(0x20U + y);
            source[offset + 2U] =
                static_cast<std::uint8_t>(0x30U + x + y);
            source[offset + 3U] = static_cast<std::uint8_t>(0x40U + x);
        }
    }

    uml::gpu::ScanoutFramebuffer framebuffer {4, 3};
    for (const auto format : {
             uml::gpu::protocol::GpuFormat::b8g8r8a8Unorm,
             uml::gpu::protocol::GpuFormat::b8g8r8x8Unorm}) {
        framebuffer.clear();
        const auto rectangle = framebuffer.update(
            source,
            sourceWidth,
            sourceHeight,
            sourceRowBytes,
            format,
            1,
            1,
            2,
            1,
            2,
            2);
        require(rectangle.x == 2);
        require(rectangle.y == 1);
        require(rectangle.width == 2);
        require(rectangle.height == 2);

        std::lock_guard lock(framebuffer);
        const auto pixels = framebuffer.pixels();
        for (std::uint32_t y = 0; y < framebuffer.height(); ++y) {
            for (std::uint32_t x = 0; x < framebuffer.width(); ++x) {
                const auto destinationOffset =
                    (static_cast<std::size_t>(y) * framebuffer.width() + x) *
                    uml::gpu::protocol::gpuBytesPerPixel;
                if (x < 2 || y < 1) {
                    for (std::size_t byte = 0;
                         byte < uml::gpu::protocol::gpuBytesPerPixel;
                         ++byte) {
                        require(pixels[destinationOffset + byte] == 0);
                    }
                    continue;
                }
                const auto sourceX = 1U + x - 2U;
                const auto sourceY = 1U + y - 1U;
                const auto sourceOffset =
                    static_cast<std::size_t>(sourceY) * sourceRowBytes +
                    static_cast<std::size_t>(sourceX) *
                        uml::gpu::protocol::gpuBytesPerPixel;
                require(std::equal(
                    source.begin() + sourceOffset,
                    source.begin() + sourceOffset +
                        uml::gpu::protocol::gpuBytesPerPixel,
                    pixels.begin() + destinationOffset));
            }
        }
    }
}

void postListenThreadFailureReleasesServer()
{
    // Initialize process-lifetime library state before measuring ownership.
    {
        uml::gpu::VncDisplay warm {"127.0.0.1", 0, {}, 64, 64};
    }

    for (unsigned iteration = 0; iteration < 16; ++iteration) {
        const auto before = descriptorCount();
        uml::gpu::testing::failNextEventThreadStart();
        requireThrows<std::runtime_error>([] {
            uml::gpu::VncDisplay display {"127.0.0.1", 0, {}, 64, 64};
        });
        const auto listenedPort =
            uml::gpu::testing::failedEventThreadPort();
        require(listenedPort != 0);
        require(descriptorCount() == before);
        {
            uml::gpu::VncDisplay retry {
                "127.0.0.1", listenedPort, {}, 64, 64};
            require(retry.port() == listenedPort);
        }
        require(descriptorCount() == before);
    }
}

void dimensionsMustFitLibVncServerAbi()
{
    const auto before = descriptorCount();
    requireThrows<std::invalid_argument>([] {
        uml::gpu::VncDisplay display {
            "127.0.0.1",
            0,
            {},
            static_cast<std::uint32_t>(
                std::numeric_limits<int>::max()) + 1U,
            1};
    });
    require(descriptorCount() == before);
}

/* The display's event thread notices a client that connected or left only
 * in a pass of its loop that starts after the event: a pass already in
 * progress may have read its sockets before it. So once the loop has finished
 * two passes after the event, the display has seen it, and what it did about
 * it can be checked once. Counting passes rather than polling the outcome
 * against a clock leaves nothing for a loaded host to starve: the thread may
 * get no time for seconds, and the test then simply waits for it. The
 * deadline is only a bound on a thread that never runs again. */
void awaitClientEvent(const uml::gpu::VncDisplay& display)
{
    constexpr std::uint64_t passesPastTheEvent = 2;
    constexpr auto deadlineSpan = std::chrono::seconds(180);
    const auto target =
        display.eventLoopPassesForTesting() + passesPastTheEvent;
    require(
        uml::testing::waitUntil(
            [&] { return display.eventLoopPassesForTesting() >= target; },
            deadlineSpan),
        "the VNC event thread stopped running");
}

[[nodiscard]] int connectClient(const std::uint16_t port)
{
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0)
        throw std::runtime_error("VNC test socket failed");
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(
            descriptor,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) != 0) {
        ::close(descriptor);
        throw std::runtime_error("VNC test connect failed");
    }
    return descriptor;
}

void disconnectClient(int& descriptor) noexcept
{
    if (descriptor < 0)
        return;
    (void)::shutdown(descriptor, SHUT_RDWR);
    (void)::close(descriptor);
    descriptor = -1;
}

void readbackTracksClientDemandAndReconnects()
{
    /* A scanout the Metal readback cannot convert - here one with no
     * texture - falls back to the CPU framebuffer at the scanout's extent,
     * and that fallback follows client demand. The mode fits at its own
     * size, centred, so the CPU path copies it synchronously. */
    constexpr std::uint32_t extent = 64;
    constexpr std::uint32_t scanoutWidth = 64;
    constexpr std::uint32_t scanoutHeight = 32;
    uml::gpu::VncDisplay display {
        "127.0.0.1", 0, {}, extent, extent};
    const uml::gpu::MetalScanout scanout {
        .width = scanoutWidth,
        .height = scanoutHeight,
    };
    require(display.guestScanoutChanged(scanoutWidth, scanoutHeight));
    require(display.presentMetal(scanout) ==
        uml::gpu::PresentOutcome::dropped);
    require(!display.needsFullCpuFallback());

    int client = connectClient(display.port());
    awaitClientEvent(display);
    require(display.presentMetal(scanout) ==
        uml::gpu::PresentOutcome::unsupported);
    require(display.needsFullCpuFallback());

    const std::size_t frameRowBytes =
        static_cast<std::size_t>(extent) * uml::gpu::protocol::gpuBytesPerPixel;
    std::vector<std::uint8_t> frame(frameRowBytes * extent, 0x5aU);
    /* Copies the top-left width x scanoutHeight of the frame to the origin. */
    const auto update = [&](std::uint32_t width) {
        display.update(
            frame,
            extent,
            extent,
            frameRowBytes,
            uml::gpu::protocol::GpuFormat::b8g8r8a8Unorm,
            0,
            0,
            0,
            0,
            width,
            scanoutHeight);
    };
    update(scanoutWidth - 1U);
    require(display.needsFullCpuFallback());
    update(scanoutWidth);
    require(!display.needsFullCpuFallback());

    disconnectClient(client);
    awaitClientEvent(display);
    require(display.presentMetal(scanout) ==
        uml::gpu::PresentOutcome::dropped);

    client = connectClient(display.port());
    awaitClientEvent(display);
    require(display.presentMetal(scanout) ==
        uml::gpu::PresentOutcome::unsupported);
    require(display.needsFullCpuFallback());
    disconnectClient(client);
}

void scaledReadbackIsDamageBounded()
{
    uml::gpu::VncDisplay display {"127.0.0.1", 0, {}, 160, 90};
    int client = connectClient(display.port());
    awaitClientEvent(display);
    require(display.needsFullCpuFallback());
    uml::gpu::testing::verifyScaledReadbackIsDamageBounded(display);
    disconnectClient(client);
}

void clearDiscardsEarlierReadbacks()
{
    uml::gpu::VncDisplay display {"127.0.0.1", 0, {}, 64, 64};
    int client = connectClient(display.port());
    awaitClientEvent(display);
    require(display.needsFullCpuFallback());
    uml::gpu::testing::verifyClearDiscardsEarlierReadbacks(display);
    disconnectClient(client);
}

void privateMetalTextureReadbackIsDamageBounded()
{
    /* A Venus scanout has no guest-visible Virgl resource, so the display
     * converts the private Metal texture itself. A scanout is still dropped
     * while no client is watching, so connect one before presenting. */
    uml::gpu::VncDisplay display {"127.0.0.1", 0, {}, 64, 64};
    int client = connectClient(display.port());
    awaitClientEvent(display);
    require(display.needsFullCpuFallback());
    uml::gpu::testing::verifyPrivateMetalTextureReadback(display);
    disconnectClient(client);
}

/* The device counts the refusals it answers the guest with on the display,
 * which a new guest on the same machine does not start again, like the frame
 * counters. */
void memoryRefusalsBelongToTheDisplay()
{
    uml::gpu::VncDisplay display {"127.0.0.1", 0, {}, 64, 64};
    require(display.memoryRefusals() == 0);
    display.recordMemoryRefusal();
    display.recordMemoryRefusal();
    require(display.memoryRefusals() == 2);
    display.resetGuestSession();
    display.clear();
    require(display.memoryRefusals() == 2,
        "a new guest or a cleared display reset the refusals");
}

} // namespace

int main()
{
    memoryRefusalsBelongToTheDisplay();
    packedBgraUpdatesRespectOffsetsAndPaddedStride();
    dimensionsMustFitLibVncServerAbi();
    readbackTracksClientDemandAndReconnects();
    postListenThreadFailureReleasesServer();
    privateMetalTextureReadbackIsDamageBounded();
    scaledReadbackIsDamageBounded();
    clearDiscardsEarlierReadbacks();
    std::puts("VNC display lifetime tests passed");
    return 0;
}
