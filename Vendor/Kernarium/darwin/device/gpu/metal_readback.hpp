// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "display.hpp"
#include "display_geometry.hpp"
#include "protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>

#ifndef UML_GPU_VNC_STARTUP_TESTING
#define UML_GPU_VNC_STARTUP_TESTING 0
#endif

namespace uml::gpu {

/* Where a request's pixels go: the framebuffer rectangle its view is placed
 * in, and the display's tag for the framebuffer contents those pixels extend.
 * A display changes the epoch whenever the framebuffer stops holding what
 * earlier requests left there - it was blanked, re-placed, or written by
 * another path - and discards a frame that carries an older one. */
struct ReadbackTarget {
    DisplayRectangle placement;
    std::uint64_t epoch {};

    friend bool operator==(const ReadbackTarget&, const ReadbackTarget&) =
        default;
};

struct MetalReadbackFrame {
    std::span<const std::uint8_t> pixels;
    std::size_t rowBytes {};
    protocol::GpuFormat format {};
    std::uint32_t width {};
    std::uint32_t height {};
    /* Where the pixels belong in the display's framebuffer. */
    std::uint32_t destinationX {};
    std::uint32_t destinationY {};
    /* The target of the request this frame answers. */
    ReadbackTarget target;
    /* The caller's tag for the request this frame answers, and whether the
     * frame covers the scanout's whole view rather than only its damage. */
    std::uint32_t generation {};
    bool complete {};
};

/* A guest scanout is the guest's own frame. A staged one is the display's CPU
 * fallback copied into a host texture, which the device counts itself, so it
 * does not enter the guest, presented, or dropped frame counters. */
enum class ReadbackSource : std::uint8_t {
    guest,
    staged,
};

struct MetalReadbackState;

/* Damage-bounded, newest-frame-wins conversion from private Metal textures to
 * CPU-visible pixels. VNC necessarily consumes host memory, but the guest GPU
 * timeline must not wait for that readback or for the VNC encoder.
 *
 * Each request places the scanout's view in a framebuffer rectangle. A
 * rectangle of the view's own size is copied as it is. Any other size, or a
 * texture stored bottom row first, is drawn into a rectangle-sized texture by
 * the native presenter's scanout pass - nearest magnification, bilinear
 * minification - and only the part of it the damage reaches is read back, so
 * an oversized guest mode costs the GPU a scaled draw and the CPU only the
 * scaled pixels. */
class MetalReadback final {
public:
    /* Returns whether the frame reached the display's framebuffer. */
    using PublishCallback =
        std::function<bool(const MetalReadbackFrame&)>;

    explicit MetalReadback(PublishCallback publish);
    ~MetalReadback();

    MetalReadback(const MetalReadback&) = delete;
    MetalReadback& operator=(const MetalReadback&) = delete;

    /* Read back the scanout's damage into the target's placement. The
     * generation travels to the published frame. A request that replaces one
     * still waiting for the in-flight readback also covers the damage of the
     * one it replaces. One whose target differs from the previous request's,
     * or that follows a frame the display did not take, covers its whole
     * view. */
    [[nodiscard]] PresentOutcome present(
        const MetalScanout& scanout,
        ReadbackTarget target,
        std::uint32_t generation,
        ReadbackSource source = ReadbackSource::guest) noexcept;
    /* Keep the scanout's whole view, without reading it, for a later
     * refresh() that counts it as the given source. Returns false, and keeps
     * nothing, for a scanout this readback cannot convert. */
    bool remember(
        const MetalScanout& scanout,
        ReadbackTarget target,
        ReadbackSource source = ReadbackSource::guest) noexcept;
    /* Read back the whole view of the remembered scanout, if any, and
     * release it. The request is not a guest frame. */
    [[nodiscard]] bool refresh(std::uint32_t generation) noexcept;
    void forget() noexcept;
    /* Copy the damaged rectangle of the display's CPU image of the whole
     * guest mode, BGRA with rowBytes per row, into a host texture of that
     * mode, and return the texture as a scanout of the mode damaged by that
     * rectangle. A texture created for a new mode is filled from the whole
     * image instead. Returns nothing when Metal cannot hold the image. Only
     * the display's own thread stages. */
    [[nodiscard]] std::optional<MetalScanout> stage(
        std::span<const std::uint8_t> image,
        std::size_t rowBytes,
        DisplaySize mode,
        DisplayRectangle damage) noexcept;
    void releaseStaging() noexcept;
    [[nodiscard]] DisplayStatistics statistics() const noexcept;
    void recordCpuFallbackFrame() noexcept;
    void shutdown() noexcept;
#if UML_GPU_VNC_STARTUP_TESTING
    /* No readback is in flight or queued behind one. */
    [[nodiscard]] bool idleForTesting() const noexcept;
#endif

private:
    std::shared_ptr<MetalReadbackState> state_;
};

} // namespace uml::gpu
