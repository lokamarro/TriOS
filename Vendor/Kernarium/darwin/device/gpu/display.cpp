// SPDX-License-Identifier: GPL-2.0-or-later

#include "display.hpp"

#include "native_display.hpp"
#if UML_GPU_HAS_OFFSCREEN
#include "offscreen_display.hpp"
#endif
#if UML_GPU_HAS_VNC
#include "vnc_display.hpp"
#endif

#include <stdexcept>
#include <utility>

namespace uml::gpu {

std::unique_ptr<Display> makeDisplay(
    DisplayMode mode,
    const std::string& vncBindAddress,
    std::uint16_t vncPort,
    std::string vncPassword,
    std::uint32_t width,
    std::uint32_t height,
    uml::input::vnc::VncInputSink inputSink,
    void* nativeView,
    uml::input::vnc::PointerMode pointerMode,
    std::uint32_t maximumDimension,
    bool autoResizeGuest,
    OffscreenDisplayOptions offscreen)
{
#if !UML_GPU_HAS_OFFSCREEN
    (void)offscreen;
#endif
#if !UML_GPU_HAS_VNC
    (void)vncBindAddress;
    (void)vncPort;
    (void)vncPassword;
#endif
    switch (mode) {
    case DisplayMode::vnc:
#if UML_GPU_HAS_VNC
        return std::make_unique<VncDisplay>(
            vncBindAddress,
            vncPort,
            std::move(vncPassword),
            width,
            height,
            std::move(inputSink),
            pointerMode);
#else
        throw std::invalid_argument(
            "VNC display is unavailable on this Apple platform");
#endif
    case DisplayMode::native:
        return std::make_unique<NativeDisplay>(
            width,
            height,
            std::move(inputSink),
            nativeView,
            pointerMode,
            maximumDimension,
            autoResizeGuest);
    case DisplayMode::offscreen:
#if UML_GPU_HAS_OFFSCREEN
        return std::make_unique<NativeDisplay>(makeOffscreenDisplayState(
            width, height, maximumDimension, offscreen));
#else
        throw std::invalid_argument(
            "The windowless display is unavailable on this Apple platform");
#endif
    }
    throw std::invalid_argument("unsupported Virtio-GPU display mode");
}

} // namespace uml::gpu
