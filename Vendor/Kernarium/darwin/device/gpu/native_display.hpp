// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "display.hpp"

#include <cstdint>
#include <memory>
#include <span>

namespace uml::gpu {

struct NativeDisplayStateCore;

class NativeDisplay final : public Display {
public:
    NativeDisplay(
        std::uint32_t width,
        std::uint32_t height,
        uml::input::vnc::VncInputSink inputSink = {},
        void* nativeView = nullptr,
        uml::input::vnc::PointerMode pointerMode =
            uml::input::vnc::PointerMode::mouse,
        std::uint32_t maximumDimension =
            protocol::maximumDisplayDimension,
        bool autoResizeGuest = true);
    /* A display over a state the caller made, such as the windowless one
     * (offscreen_display.hpp). */
    explicit NativeDisplay(std::shared_ptr<NativeDisplayStateCore> state);
    ~NativeDisplay() override;

    NativeDisplay(const NativeDisplay&) = delete;
    NativeDisplay& operator=(const NativeDisplay&) = delete;

    [[nodiscard]] std::uint32_t width() const noexcept override;
    [[nodiscard]] std::uint32_t height() const noexcept override;
    [[nodiscard]] int resizeEventDescriptor() const noexcept override;
    [[nodiscard]] bool drainResizeEvent() override;
    [[nodiscard]] bool guestScanoutChanged(
        std::uint32_t width,
        std::uint32_t height) noexcept override;
    void resetGuestSession() noexcept override;
    [[nodiscard]] bool claimResizeNotification() noexcept override;
    void releaseResizeNotification() noexcept override;
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
    [[nodiscard]] bool needsFullCpuFallback() const noexcept override
    {
        return true;
    }
    [[nodiscard]] DisplayStatistics statistics() const noexcept override;
    void recordCpuFallbackFrame() noexcept override;

private:
    std::shared_ptr<NativeDisplayStateCore> state_;
};

} // namespace uml::gpu
