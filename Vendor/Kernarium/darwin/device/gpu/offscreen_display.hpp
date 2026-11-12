// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "display.hpp"
#include "display_geometry.hpp"
#include "display_resize_notifier.hpp"
#include "native_display_state.hpp"

#include <cstdint>
#include <memory>

namespace uml::gpu {

/* The size, in pixels, of the drawables of an offscreen display whose guest
 * mode is `display`: the option's where it names both dimensions, the mode's
 * otherwise, each clamped as a window's is. */
[[nodiscard]] inline DisplaySize offscreenDrawableSize(
    const OffscreenDisplayOptions& options,
    DisplaySize display,
    std::uint32_t maximumDimension) noexcept
{
    DisplaySize size {options.drawableWidth, options.drawableHeight};
    if (size.width == 0 || size.height == 0)
        size = display;
    return {
        DisplayResizeNotifier::clampDimension(size.width, maximumDimension),
        DisplayResizeNotifier::clampDimension(size.height, maximumDimension),
    };
}

/* The state of a NativeDisplay that presents into a CAMetalLayer belonging to
 * no window (DisplayMode::offscreen), on macOS.
 *
 * It drives the same MetalPresentation as the AppKit and UIKit shells, into
 * three BGRA8 drawables of options.drawableWidth by options.drawableHeight
 * (the display's own size where those are zero), the drawables an iPad's view
 * has. Each is cleared and presented once when the display starts, because a
 * view that presents has rendered into all three within a few frames, and a
 * layer asked for a drawable only when the guest draws would show two at idle.
 * A guest frame is presented as soon as the presentation has one: with no
 * screen there is no display link to ask for it, so a serial queue of the
 * display draws on request and a frame that arrives while one is drawing
 * replaces the one waiting, as in a window. Nothing reaches the guest from the
 * host: no input, no host resize.
 *
 * With options.mirrorWidth and options.mirrorFramesPerSecond set it also keeps
 * the frame mirror (frame_tap.hpp) running into a receiver that keeps the last
 * two frames it is given and drops the rest, which holds the mirror's pool of
 * three IOSurface buffers as Picture in Picture's display layer does. */
[[nodiscard]] std::shared_ptr<NativeDisplayStateCore> makeOffscreenDisplayState(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t maximumDimension,
    const OffscreenDisplayOptions& options);

} // namespace uml::gpu
