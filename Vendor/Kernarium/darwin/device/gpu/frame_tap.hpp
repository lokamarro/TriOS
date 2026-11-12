// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::gpu {

/* Receives one copy of the guest's display as a CVPixelBufferRef (an
 * IOSurface-backed BGRA buffer from a pool), upright and aspect-preserved. The
 * buffer is borrowed for the call: a receiver that keeps it retains it. The
 * sink runs on a host thread, must return promptly, and must not throw. */
using FrameTapSink = void (*)(void* context, void* pixelBuffer);

struct FrameTapOptions final {
    /* The copy is no wider than this, and no larger than the guest's display. */
    std::uint32_t maximumWidth {960};
    /* The most frames delivered each second. A static display is re-sent about
     * once a second so that a consumer sees a live stream. */
    std::uint32_t framesPerSecond {30};
};

/* Mirrors the guest's display to `sink` for a view of it that the application
 * shows while its own window cannot, such as Picture in Picture. Nothing is
 * rendered while no sink is set. A null sink stops the mirror; when this
 * returns, no call to the previous sink is running or will start, so its
 * context may be released. The sink must not call back into this function.
 *
 * One mirror exists per process: a guest has one display. */
void setFrameTap(
    FrameTapSink sink,
    void* context,
    FrameTapOptions options = {}) noexcept;

} // namespace uml::gpu
