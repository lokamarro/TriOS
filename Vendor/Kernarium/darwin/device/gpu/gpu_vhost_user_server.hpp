// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "display.hpp"
#include "vhost_user_server.hpp"
#include "../input/vnc/vnc_input.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace uml::gpu {

/* The display a Virtio-GPU backend presents on, with the dimension limit its
 * EDID setting implies. */
[[nodiscard]] std::unique_ptr<Display> makeGpuDisplay(
    const std::string& vncBindAddress,
    std::uint16_t vncPort,
    std::string vncPassword,
    std::uint32_t width,
    std::uint32_t height,
    uml::input::vnc::VncInputSink inputSink = {},
    DisplayMode displayMode = DisplayMode::vnc,
    void* nativeView = nullptr,
    std::uint32_t dotsPerInch = 0,
    uml::input::vnc::PointerMode pointerMode =
        uml::input::vnc::PointerMode::mouse,
    bool autoResizeGuest = true,
    OffscreenDisplayOptions offscreen = {});

/* The Virtio-GPU backend for one guest: a vhost-user server whose device
 * renders through virglrenderer and presents on `display`. The device only
 * borrows the display, resets it when it goes, and leaves it to the caller,
 * so a display that other guests present on after this one survives it. */
[[nodiscard]] std::unique_ptr<VhostUserServer> makeGpuVhostUserServer(
    std::shared_ptr<Display> display,
    std::string vncBindAddress,
    std::uint32_t dotsPerInch = 0,
    bool selfTestPattern = false);

/* The same with a display of its own, which goes with the server. */
[[nodiscard]] std::unique_ptr<VhostUserServer> makeGpuVhostUserServer(
    std::string vncBindAddress,
    std::uint16_t vncPort,
    std::string vncPassword,
    std::uint32_t width,
    std::uint32_t height,
    bool selfTestPattern,
    uml::input::vnc::VncInputSink inputSink = {},
    DisplayMode displayMode = DisplayMode::vnc,
    void* nativeView = nullptr,
    std::uint32_t dotsPerInch = 0,
    uml::input::vnc::PointerMode pointerMode =
        uml::input::vnc::PointerMode::mouse,
    bool autoResizeGuest = true,
    OffscreenDisplayOptions offscreen = {});

} // namespace uml::gpu
