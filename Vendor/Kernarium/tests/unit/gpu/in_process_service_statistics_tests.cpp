// SPDX-License-Identifier: GPL-2.0-or-later

/* The frame counters of the shipped service's display, read through the
 * service. A windowless display counts the frames it presents, as a screen
 * does, so a scripted guest that shows frames on it moves the count, and a
 * stopped service reports none. The graphics memory a guest's contexts hold
 * is read the same way, and a guest that holds none is none. */

#include "frame_guest.hpp"
#include "gpu_test_support.hpp"
#include "in_process_service.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

namespace {

using uml::gpu::DisplayStatistics;
using uml::gpu::testing::FrameGuest;
using uml::gpu::testing::Pixel;
using uml::gpu::testing::takeEndpoints;
using uml::testing::require;
using uml::testing::waitUntil;

constexpr int skippedExitCode = 77;
constexpr std::uint64_t deviceMemoryLimit = 4ULL << 30;
constexpr Pixel firstFrame {0x13U, 0x57U, 0x9bU, 0xffU};
constexpr Pixel secondFrame {0xe1U, 0x4dU, 0x27U, 0xffU};

/* The presented count once it passes `above`, or what the count is after the
 * wait. */
std::uint64_t awaitPresented(
    uml::gpu::InProcessGpuService& service,
    std::uint64_t above)
{
    std::uint64_t presented = 0;
    (void)waitUntil(
        [&] {
            const auto statistics = service.displayStatistics();
            presented = statistics ? statistics->presentedFrames : 0;
            return presented > above;
        },
        std::chrono::seconds {10},
        std::chrono::milliseconds {10});
    return presented;
}

} // namespace

int main()
{
    uml::gpu::InProcessGpuOptions options;
    options.displayMode = uml::gpu::DisplayMode::offscreen;
    options.vncPort = 0;
    options.deviceMemoryLimit = deviceMemoryLimit;
    options.width = 64;
    options.height = 64;
    std::unique_ptr<uml::gpu::InProcessGpuService> service;
    try {
        service = std::make_unique<uml::gpu::InProcessGpuService>(
            std::move(options));
    } catch (const std::exception& error) {
        std::fprintf(
            stderr, "renderer unavailable: %s; skipping\n", error.what());
        return skippedExitCode;
    }

    /* The display counts from zero, and nothing counts a frame no guest
     * showed. */
    const auto idle = service->displayStatistics();
    require(idle.has_value(), "the service has no display counters");
    require(idle->guestFrames == 0 && idle->presentedFrames == 0 &&
            idle->droppedFrames == 0 && idle->cpuFallbackFrames == 0,
        "a display no guest has drawn on counted a frame");

    /* The refusals the guest was answered with are the display's too, and
     * a guest that shows frames and asks for nothing is refused nothing. */
    require(service->memoryRefusals() == std::optional<std::uint64_t> {0},
        "a display no guest was refused on counted a refusal");

    const auto none =
        service->graphicsMemory(8, std::chrono::milliseconds {0}, 0);
    require(none.has_value() && none->holders.empty() &&
            none->contextBytes == 0,
        "a guest that holds no graphics memory was credited some");

    /* The device memory budget is the service's too: B once the renderer
     * starts, with the pool's count of what the process holds against it. */
    auto endpoints = takeEndpoints(*service);
    FrameGuest guest {endpoints->transport};
    guest.show(firstFrame);
    const auto first = awaitPresented(*service, 0);
    require(first != 0, "the first frame was never counted as presented");
    guest.show(secondFrame);
    const auto second = awaitPresented(*service, first);
    require(second > first, "the second frame was never counted");

    /* The counters are the same whether or not the guest's requests are
     * held, and they only grow. */
    require(
        service->holdRequests(std::chrono::milliseconds {100}),
        "the service would not hold");
    const auto held = service->displayStatistics();
    require(held.has_value() && held->presentedFrames >= second,
        "holding requests hid or reset the counters");
    require(service->memoryRefusals() == std::optional<std::uint64_t> {0},
        "showing frames counted a refusal");
    const auto heldMemory =
        service->graphicsMemory(8, std::chrono::milliseconds {0}, 0);
    require(heldMemory.has_value() && heldMemory->holders.empty(),
        "holding requests hid the graphics memory");
    const auto memory = service->deviceMemory();
    require(memory.has_value() && memory->budgetBytes == deviceMemoryLimit &&
            memory->usageBytes > 0,
        "the service did not report its device memory budget and usage");
    service->releaseRequests();

    endpoints->close();
    service->stop();
    require(!service->displayStatistics().has_value(),
        "a stopped service reported its display's counters");
    require(!service->memoryRefusals().has_value(),
        "a stopped service reported its display's refusals");
    require(!service->graphicsMemory(8, std::chrono::milliseconds {0}, 0),
        "a stopped service reported its graphics memory");
    return 0;
}
