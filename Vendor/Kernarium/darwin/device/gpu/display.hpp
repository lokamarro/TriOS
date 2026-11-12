// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "context_memory_ledger.hpp"
#include "protocol.hpp"
#include "../input/vnc/vnc_input.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace uml::gpu {

enum class DisplayMode {
    vnc,
    native,
    /* The native display's presentation into a Metal layer that belongs to
     * no window, on macOS. See OffscreenDisplayOptions. */
    offscreen,
};

/* What DisplayMode::offscreen presents into and keeps alive. A window and a
 * screen are the things a measurement run cannot have: the iPad allocates the
 * memory of its presentation (drawables of the view's size and, in Picture in
 * Picture, the pool of the frame mirror) and a run on a Mac with a VNC
 * display allocates none of it. The offscreen display makes the same
 * allocations with nothing to show them on, takes no input, and presents each
 * guest frame as it arrives, since there is no display to pace it. */
struct OffscreenDisplayOptions {
    /* The drawables' size in pixels: the size of the view the iPad shows the
     * guest in. Zero in either dimension keeps the display's own size. */
    std::uint32_t drawableWidth {};
    std::uint32_t drawableHeight {};
    /* The frame mirror (frame_tap.hpp) that Picture in Picture runs, kept
     * running at this width and rate with a receiver that holds the last two
     * frames, as Picture in Picture's display layer does.
     * Zero in either leaves the mirror off, as it is without Picture in
     * Picture. */
    std::uint32_t mirrorWidth {};
    std::uint32_t mirrorFramesPerSecond {};

    [[nodiscard]] bool operator==(const OffscreenDisplayOptions&) const =
        default;
};

struct MetalScanout {
    void* texture {};
    std::uint32_t sourceWidth {};
    std::uint32_t sourceHeight {};
    std::uint32_t sourceX {};
    std::uint32_t sourceY {};
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t damageX {};
    std::uint32_t damageY {};
    std::uint32_t damageWidth {};
    std::uint32_t damageHeight {};
    /* The texture stores the image bottom row first. */
    bool flipped {};
};

/* A direct presentation either cannot be expressed by this display at all, or
 * it was accepted. Acceptance is deliberately two outcomes: the guest must not
 * distinguish them, because a dropped frame is a normal result of rendering
 * faster than the host presents, and treating it as a failure would send the
 * scanout flush down the CPU readback path that this decoupling exists to
 * avoid. */
enum class PresentOutcome {
    unsupported,
    presented,
    dropped,
};

[[nodiscard]] constexpr bool presentAccepted(PresentOutcome outcome) noexcept
{
    return outcome != PresentOutcome::unsupported;
}

/* Guest and host rendering are independent timelines once a display accepts
 * direct presentation, so their frame counts are reported separately rather
 * than as one rate. */
struct DisplayStatistics {
    /* Scanout flushes the guest offered to this display. */
    std::uint64_t guestFrames {};
    /* Frames the host actually finished presenting. */
    std::uint64_t presentedFrames {};
    /* Guest frames retired without ever being presented, because a newer one
     * replaced them or no free surface was available. */
    std::uint64_t droppedFrames {};
    /* Guest frames that could not use direct presentation and took the CPU
     * readback path instead. */
    std::uint64_t cpuFallbackFrames {};
};

class Display {
public:
    virtual ~Display() = default;

    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

    [[nodiscard]] virtual std::uint32_t width() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t height() const noexcept = 0;
    /* Native AppKit/UIKit surfaces publish resize readiness from the UI
     * thread. The transport drains and applies it on its GPU owner thread. */
    [[nodiscard]] virtual int resizeEventDescriptor() const noexcept
    {
        return -1;
    }
    [[nodiscard]] virtual bool drainResizeEvent() { return false; }
    /* SET_SCANOUT is the point at which a guest mode becomes active. Native
     * displays keep that content geometry separate from their host-preferred
     * mode so either side can resize without distorting the other. Returning
     * false rejects the modeset when presentation storage cannot be prepared
     * transactionally. */
    [[nodiscard]] virtual bool guestScanoutChanged(
        std::uint32_t,
        std::uint32_t) noexcept
    {
        return true;
    }
    /* A new guest boot seeds presentation from the latest settled host
     * geometry while retaining the configured host-to-guest policy. */
    virtual void resetGuestSession() noexcept {}
    /* Serialize a guest config notification against the beginning of an
     * interactive host transition. A successful claim remains held until the
     * transport calls releaseResizeNotification(). */
    [[nodiscard]] virtual bool claimResizeNotification() noexcept
    {
        return true;
    }
    virtual void releaseResizeNotification() noexcept {}
    [[nodiscard]] virtual std::optional<std::uint16_t> vncPort() const noexcept
    {
        return std::nullopt;
    }
    virtual void clear() noexcept = 0;
    /* Whether the guest has its primary scanout on. The device sets it when
     * the guest binds a resource to the scanout and clears it when the guest
     * unbinds it, which an Android screen turning off does through its
     * disabled CRTC, and when the guest is reset or gone. It is read from
     * any thread, by the application waiting for a guest's screen to go off. */
    [[nodiscard]] bool guestScanoutActive() const noexcept
    {
        return guestScanoutActive_.load(std::memory_order_acquire);
    }
    void setGuestScanoutActive(bool active) noexcept
    {
        guestScanoutActive_.store(active, std::memory_order_release);
    }
    virtual void update(
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
        std::uint32_t height) = 0;
    virtual void fillSelfTestPattern() = 0;
    /* Apple renderer backends expose a scanout as the native MTLTexture they
     * already render into. The opaque pointer keeps AppKit, UIKit, and
     * Objective-C types out of the platform-neutral device contract. */
    [[nodiscard]] virtual PresentOutcome presentMetal(
        const MetalScanout& scanout) noexcept
    {
        (void)scanout;
        return PresentOutcome::unsupported;
    }
    /* A native display can switch from direct GPU presentation to CPU
     * fallback after prior frames never populated its CPU framebuffer. */
    [[nodiscard]] virtual bool needsFullCpuFallback() const noexcept
    {
        return false;
    }
    /* Counters for the guest and host timelines. A display that never presents
     * directly still counts the guest frames it was offered. */
    [[nodiscard]] virtual DisplayStatistics statistics() const noexcept
    {
        return {};
    }
    /* The device records its own CPU readback frames here, because only the
     * device knows a direct presentation was refused. */
    virtual void recordCpuFallbackFrame() noexcept {}
    /* The device counts here each time it tells the guest that the host GPU
     * has no room for a graphics allocation: a refusal that the host's
     * reclaim and retry did not avoid, because only the device knows what the
     * guest was answered. Like the frame counters it belongs to the display,
     * so a guest that restarts does not reset it. Read from any thread. */
    void recordMemoryRefusal() noexcept
    {
        memoryRefusals_.fetch_add(1, std::memory_order_relaxed);
        /* A guest that asked to hear of refusals is told through the
         * ledger's observer. */
        graphicsLedger_.notify();
    }
    [[nodiscard]] std::uint64_t memoryRefusals() const noexcept
    {
        return memoryRefusals_.load(std::memory_order_relaxed);
    }
    /* What the guest's contexts hold of the host GPU's memory, by process.
     * The device keeps it for as long as its guest runs, and clears it when
     * that guest is gone; it lives here because the display outlives the
     * connection and any thread may read it. */
    [[nodiscard]] ContextMemoryLedger& graphicsLedger() noexcept
    {
        return graphicsLedger_;
    }
    [[nodiscard]] const ContextMemoryLedger& graphicsLedger() const noexcept
    {
        return graphicsLedger_;
    }

    /* Whether a reclaimer in the guest acts on the memory reports the device
     * sends (the Virtio-GPU memory report's GUEST_RECLAIMS flag), so the
     * host's own reclaimer stands down. The device sets it with each query of
     * the guest and clears it when the connection ends; it lives here because
     * the display outlives the connection and any thread reads it. */
    void setGuestReclaims(bool reclaims) noexcept
    {
        guestReclaims_.store(reclaims, std::memory_order_relaxed);
    }
    [[nodiscard]] bool guestReclaims() const noexcept
    {
        return guestReclaims_.load(std::memory_order_relaxed);
    }

protected:
    Display() = default;

private:
    std::atomic_bool guestScanoutActive_ {};
    std::atomic_bool guestReclaims_ {};
    std::atomic<std::uint64_t> memoryRefusals_ {};
    ContextMemoryLedger graphicsLedger_;
};

[[nodiscard]] std::unique_ptr<Display> makeDisplay(
    DisplayMode mode,
    const std::string& vncBindAddress,
    std::uint16_t vncPort,
    std::string vncPassword,
    std::uint32_t width,
    std::uint32_t height,
    uml::input::vnc::VncInputSink inputSink = {},
    void* nativeView = nullptr,
    uml::input::vnc::PointerMode pointerMode =
        uml::input::vnc::PointerMode::mouse,
    std::uint32_t maximumDimension = protocol::maximumDisplayDimension,
    bool autoResizeGuest = true,
    OffscreenDisplayOptions offscreen = {});

} // namespace uml::gpu
