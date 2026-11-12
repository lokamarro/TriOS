// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "display.hpp"
#include "display_geometry.hpp"
#include "metal_readback.hpp"
#include "scanout_framebuffer.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#ifndef UML_GPU_VNC_STARTUP_TESTING
#define UML_GPU_VNC_STARTUP_TESTING 0
#endif

struct _rfbScreenInfo;
using rfbScreenInfoPtr = _rfbScreenInfo*;
struct _rfbClientRec;
using rfbClientPtr = _rfbClientRec*;

namespace uml::gpu {

/* A VNC framebuffer keeps the size it was created with, which the device
 * advertises as the preferred mode. Every mode the guest applies is shown
 * aspect-fitted and centred in it, black outside, and pointer input maps back
 * through the same rectangle. A mode the fit leaves at its own size, such as
 * the preferred one, is copied pixel for pixel; any other is scaled on the
 * GPU. */
class VncDisplay final : public Display {
public:
    VncDisplay(
        const std::string& bindAddress,
        std::uint16_t port,
        std::string password,
        std::uint32_t width,
        std::uint32_t height,
        uml::input::vnc::VncInputSink inputSink = {},
        uml::input::vnc::PointerMode pointerMode =
            uml::input::vnc::PointerMode::mouse);
    ~VncDisplay();

    VncDisplay(const VncDisplay&) = delete;
    VncDisplay& operator=(const VncDisplay&) = delete;

    [[nodiscard]] std::uint32_t width() const noexcept override;
    [[nodiscard]] std::uint32_t height() const noexcept override;
    // A requested port of zero asks LibVNCServer to bind an available port.
    // Callers use the post-bind value instead of racing a probe socket.
    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] std::optional<std::uint16_t> vncPort() const noexcept override;
    [[nodiscard]] bool guestScanoutChanged(
        std::uint32_t width,
        std::uint32_t height) noexcept override;
    void resetGuestSession() noexcept override;
    // Return the published scanout to a blank device-reset state. This is
    // allocation-free so transport teardown can call it from a noexcept reset.
    void clear() noexcept override;
    void update(
        std::span<const std::uint8_t> source,
        std::uint32_t sourceWidth,
        std::uint32_t sourceHeight,
        std::size_t sourceRowBytes,
        protocol::GpuFormat sourceFormat,
        std::uint32_t sourceX,
        std::uint32_t sourceY,
        std::uint32_t destinationX,
        std::uint32_t destinationY,
        std::uint32_t width,
        std::uint32_t height) override;
    void fillSelfTestPattern() override;
    [[nodiscard]] PresentOutcome presentMetal(
        const MetalScanout& scanout) noexcept override;
    [[nodiscard]] bool needsFullCpuFallback() const noexcept override;
    [[nodiscard]] DisplayStatistics statistics() const noexcept override;
    void recordCpuFallbackFrame() noexcept override;
#if UML_GPU_VNC_STARTUP_TESTING
    [[nodiscard]] std::array<std::uint8_t, 4> pixelForTesting(
        std::uint32_t x, std::uint32_t y) noexcept;
    [[nodiscard]] bool readbackIdleForTesting() const noexcept;
    /* How many passes of the event thread's loop have finished. The thread
     * notices a client that connected or left, and every other client event,
     * only in a pass that starts after it. */
    [[nodiscard]] std::uint64_t eventLoopPassesForTesting() const noexcept;
#endif

private:
    struct HeldClient {
        rfbClientPtr client {};
        std::uint32_t epoch {};
        std::chrono::steady_clock::time_point deadline {};
    };

    /* The framebuffer rectangle that shows the active guest mode. */
    [[nodiscard]] DisplayRectangle placement() const noexcept;
    /* Where a readback requested now belongs. */
    [[nodiscard]] ReadbackTarget readbackTarget() const noexcept;
    /* The framebuffer no longer extends what any earlier readback read, so
     * nothing read before now may reach it. Needs geometryMutex_. */
    void advanceFrameEpochLocked() noexcept;
    void applyGuestMode(std::uint32_t width, std::uint32_t height) noexcept;
    void releasePointerLocked() noexcept;
    void presentStaged(
        const MetalScanout& scanout,
        ReadbackTarget target) noexcept;
    [[nodiscard]] bool publishMetalFrame(
        const MetalReadbackFrame& frame) noexcept;
    void eventLoop();
    [[nodiscard]] std::optional<std::uint32_t> clientConnected() noexcept;
    void clientDisconnected(rfbClientPtr client) noexcept;
    void holdFirstUpdate(rfbClientPtr client) noexcept;
    void releaseHeldClients() noexcept;
    void handleKey(bool down, std::uint32_t keySymbol) noexcept;
    void handlePointer(int buttonMask, int x, int y) noexcept;
    void reportInputFailure() noexcept;
    void reportStagingFailure() noexcept;
    [[nodiscard]] std::uint32_t connectionEpoch() const noexcept;
    void recordFullFrame(std::uint32_t connectionEpoch) noexcept;

    uml::input::vnc::VncInputSink inputSink_;
    ScanoutFramebuffer framebuffer_;
    /* The framebuffer's size is the host's preferred geometry; the active
     * guest mode is tracked separately. */
    DisplayGeometryPolicy geometry_;
    /* Orders every framebuffer change outside the readback - a mode change,
     * clear(), a CPU fallback copy - against a readback frame requested
     * before it, which is then discarded. */
    std::mutex geometryMutex_;
    /* Changed only by the GPU thread, under geometryMutex_. */
    std::atomic<std::uint64_t> frameEpoch_ {};
    /* The CPU fallback's image of a guest mode that must be scaled. Owned by
     * the GPU thread. */
    std::unique_ptr<ScanoutFramebuffer> stagingImage_;
    std::string password_;
    std::array<char*, 2> passwordList_ {};
    rfbScreenInfoPtr screen_ {};
    uml::input::vnc::PointerMode pointerMode_;
    /* Pointer state is the event thread's, except that a mode change on the
     * GPU thread releases what the guest holds. */
    std::mutex pointerMutex_;
    FittedPointer pointer_;
    /* The upper half is a reconnect epoch and the lower half is the live
     * client count. Publishing them together prevents the GPU thread from
     * observing a first client without also observing its full-frame demand. */
    std::atomic<std::uint64_t> clientState_ {};
    std::atomic<std::uint32_t> fullFrameEpoch_ {};
    /* Orders the GPU thread's "remember, then see no client" against the
     * event thread's "count the first client, then refresh what was
     * remembered", so no scanout dropped for lack of a client escapes both. */
    std::mutex scanoutMutex_;
    bool idleFramesDropped_ {};
    /* Event-thread state: the newest connection epoch that began with a
     * stale framebuffer, and the clients whose first update waits for it. */
    std::uint32_t staleEpoch_ {};
    std::vector<HeldClient> heldClients_;
    std::atomic_bool running_ {true};
#if UML_GPU_VNC_STARTUP_TESTING
    std::atomic<std::uint64_t> eventLoopPasses_ {};
#endif
    std::atomic_bool inputFailureReported_ {};
    std::atomic_bool stagingFailureReported_ {};
    std::thread eventThread_;
    std::unique_ptr<MetalReadback> metalReadback_;
};

} // namespace uml::gpu
