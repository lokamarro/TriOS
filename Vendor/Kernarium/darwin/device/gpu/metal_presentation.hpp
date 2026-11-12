// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "display.hpp"
#include "display_geometry.hpp"
#include "scanout_framebuffer.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace uml::gpu {

struct MetalPresentationState;

// Common newest-frame-wins Metal timeline used by the AppKit and UIKit shells.
// Objective-C and Metal types remain inside the implementation.
class MetalPresentation final {
public:
    MetalPresentation(std::uint32_t width, std::uint32_t height);
    ~MetalPresentation();

    MetalPresentation(const MetalPresentation&) = delete;
    MetalPresentation& operator=(const MetalPresentation&) = delete;

    void setRedrawCallback(std::function<void()> callback);
    [[nodiscard]] void* device() const noexcept;
    [[nodiscard]] ScanoutFramebuffer& framebuffer() noexcept;
    [[nodiscard]] const ScanoutFramebuffer& framebuffer() const noexcept;
    [[nodiscard]] bool resize(
        std::uint32_t width,
        std::uint32_t height,
        bool requestRedraw = true) noexcept;
    [[nodiscard]] DisplayRotation rotation() const noexcept;
    /* Rotates only host presentation. The guest scanout dimensions and
     * contents remain unchanged. */
    [[nodiscard]] bool setRotation(DisplayRotation rotation) noexcept;
    /* Re-present the latest guest content after the host drawable changes.
     * This is independent of the guest frame timeline and therefore must not
     * make a later guest frame look superseded. */
    void drawableSizeChanged() noexcept;
    void publishCpuFrame() noexcept;
    [[nodiscard]] PresentOutcome present(
        const MetalScanout& scanout) noexcept;
    void draw(void* nativeView) noexcept;
    /* The same into the next drawable of a CAMetalLayer, which need belong to
     * no window and then has no display link to ask for a frame: whoever owns
     * the layer calls this from the redraw callback. */
    void drawLayer(void* metalLayer) noexcept;
    [[nodiscard]] DisplayStatistics statistics() const noexcept;
    void recordCpuFallbackFrame() noexcept;
    void shutdown() noexcept;

private:
    std::shared_ptr<MetalPresentationState> state_;
};

} // namespace uml::gpu
