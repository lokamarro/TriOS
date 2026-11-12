// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "display_geometry.hpp"
#include "display_resize_notifier.hpp"
#include "metal_presentation.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>

namespace uml::gpu {

/*
 * The part of a NativeDisplay's state that the macOS and iOS windows share:
 * the presentation, the host resize notifier and geometry policy, and the
 * lock and flags of a host resize. NativeDisplay forwards to these directly;
 * each platform derives its window, view and input state from this.
 */
struct NativeDisplayStateCore {
    NativeDisplayStateCore(
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t maximumDimension,
        bool autoResizeGuest);
    virtual ~NativeDisplayStateCore() = default;

    NativeDisplayStateCore(const NativeDisplayStateCore&) = delete;
    NativeDisplayStateCore& operator=(const NativeDisplayStateCore&) = delete;

    [[nodiscard]] bool drainResizeEvent();
    [[nodiscard]] virtual bool guestScanoutChanged(
        std::uint32_t width,
        std::uint32_t height) noexcept = 0;
    virtual void resetGuestSession() noexcept = 0;
    virtual void shutdown() noexcept = 0;

    MetalPresentation presentation;
    DisplayResizeNotifier resizeNotifier;
    DisplayGeometryPolicy geometryPolicy;
    std::atomic_bool resizeInProgress {};
    std::atomic_bool resizeCommitPending {};
    std::mutex resizeLifecycleMutex;
};

// A view dimension as a whole display dimension no larger than maximum, or
// none when it is not a positive finite size.
[[nodiscard]] std::optional<std::uint32_t> displayDimension(
    double dimension,
    std::uint32_t maximum) noexcept;

} // namespace uml::gpu
