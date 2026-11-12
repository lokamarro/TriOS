// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace uml::gpu {

struct DisplaySize final {
    std::uint32_t width {};
    std::uint32_t height {};

    [[nodiscard]] bool operator==(const DisplaySize&) const noexcept = default;
};

/* One 64-bit word holding both dimensions, so a std::atomic_uint64_t publishes
 * a size without tearing it. */
[[nodiscard]] constexpr std::uint64_t packDisplaySize(DisplaySize size) noexcept
{
    return static_cast<std::uint64_t>(size.width) << 32U | size.height;
}

[[nodiscard]] constexpr DisplaySize unpackDisplaySize(
    std::uint64_t packed) noexcept
{
    return {
        .width = static_cast<std::uint32_t>(packed >> 32U),
        .height = static_cast<std::uint32_t>(packed),
    };
}

struct AspectFitRectangle final {
    double x {};
    double y {};
    double width {};
    double height {};
};

struct DisplayPoint final {
    double x {};
    double y {};
};

enum class DisplayRotation : std::uint8_t {
    upright,
    clockwise90,
    upsideDown,
    counterclockwise90,
};

struct ClippedDisplaySegment final {
    DisplayPoint entry;
    DisplayPoint exit;
};

[[nodiscard]] AspectFitRectangle aspectFitRectangle(
    double containerWidth,
    double containerHeight,
    double contentWidth,
    double contentHeight) noexcept;

/* A rectangle of whole framebuffer pixels. */
struct DisplayRectangle final {
    std::uint32_t x {};
    std::uint32_t y {};
    std::uint32_t width {};
    std::uint32_t height {};

    [[nodiscard]] bool operator==(const DisplayRectangle&) const noexcept =
        default;
};

/* The centred aspect fit of content in a fixed framebuffer, with its edges
 * rounded to whole pixels. Content of the framebuffer's own size fills it
 * exactly, and non-empty content never rounds to an empty rectangle. */
[[nodiscard]] DisplayRectangle aspectFitPixels(
    DisplaySize container,
    DisplaySize content) noexcept;

[[nodiscard]] DisplayRotation rotateDisplay(
    DisplayRotation rotation,
    int quarterTurns) noexcept;
[[nodiscard]] DisplaySize presentedDisplaySize(
    DisplaySize guestSize,
    DisplayRotation rotation) noexcept;
/* Maps a normalized, top-left-origin point from the rotated presentation back
 * into the guest's unchanged scanout coordinate system. */
[[nodiscard]] DisplayPoint guestPointFromPresentedPoint(
    DisplayPoint point,
    DisplayRotation rotation) noexcept;

/* A guest scanout as a presenter samples it: the rectangle of the guest's
 * source image that the scanout shows, and the texture that stores the
 * source. A flipped texture stores the source bottom row first. */
struct ScanoutRegion final {
    DisplaySize texture;
    DisplaySize source;
    std::uint32_t x {};
    std::uint32_t y {};
    std::uint32_t width {};
    std::uint32_t height {};
    bool flipped {};
};

struct TextureCoordinate final {
    double x {};
    double y {};
};

/* Normalized texture coordinates that stretch a scanout rectangle over a
 * presenter's top-left-origin unit square: origin + point * extent, clamped
 * to [minimum, maximum], samples the rectangle, with a flip folded into a
 * negative vertical extent. The clamp keeps a filtered sample on the centres
 * of the rectangle's edge texels, so no pixel outside it bleeds in. content is
 * the rectangle's size, the one a presenter aspect-fits. */
struct ScanoutSampling final {
    DisplaySize content;
    TextureCoordinate origin;
    TextureCoordinate extent;
    TextureCoordinate minimum;
    TextureCoordinate maximum;
};

/* Returns nothing when the region cannot be sampled: an empty rectangle or
 * source, a rectangle reaching outside the source, or a texture smaller than
 * the source. */
[[nodiscard]] std::optional<ScanoutSampling> scanoutSampling(
    const ScanoutRegion& region) noexcept;

/* Returns the portion of a pointer path inside a display rectangle. Entry and
 * exit lie exactly on the rectangle boundary when the corresponding endpoint
 * is outside. This lets native touchscreen adapters synthesize contact at the
 * same boundary a physical finger would cross. */
[[nodiscard]] std::optional<ClippedDisplaySegment> clipDisplaySegment(
    AspectFitRectangle rectangle,
    DisplayPoint start,
    DisplayPoint end) noexcept;

/* One pointer report for the guest: a position in the guest's normalized,
 * top-left-origin square and the buttons held with it. A touch contact is
 * button 1. */
struct GuestPointerSample final {
    DisplayPoint point;
    std::uint32_t buttons {};
};

/* The reports that answer one host pointer event, in order. */
struct GuestPointerSamples final {
    std::array<GuestPointerSample, 3> samples {};
    std::size_t count {};

    [[nodiscard]] const GuestPointerSample* begin() const noexcept
    {
        return samples.data();
    }
    [[nodiscard]] const GuestPointerSample* end() const noexcept
    {
        return samples.data() + count;
    }
};

/* Maps host pointer events onto a guest shown in a fitted rectangle, by the
 * native display's rules for the bars around it. A mouse outside the
 * rectangle reaches the guest only while it drags buttons first pressed
 * inside, clamped to the edge; buttons pressed outside stay with the host
 * until all are released. A touch that starts outside begins a guest contact
 * where its path enters the rectangle, and a contact ends where the path
 * leaves. Host positions and the rectangle share one coordinate system,
 * whose origin is at the top left or, as in an AppKit view, the bottom
 * left; the samples are always in the guest's top-left-origin square. */
class FittedPointer final {
public:
    enum class Origin : std::uint8_t { topLeft, bottomLeft };

    explicit FittedPointer(Origin origin = Origin::topLeft) noexcept
        : origin_(origin)
    {
    }

    /* buttons holds the primary, middle and secondary buttons in bits 0-2. */
    [[nodiscard]] GuestPointerSamples mouse(
        AspectFitRectangle content,
        DisplayPoint location,
        std::uint32_t buttons) noexcept;
    [[nodiscard]] GuestPointerSamples touch(
        AspectFitRectangle content,
        DisplayPoint location,
        bool pressed) noexcept;
    /* Ends the contact or buttons the guest holds, for a mode change that
     * moves the rectangle under them. Returns whether any were held, so the
     * caller releases them in the guest. With suppressHeldGesture the rest
     * of that host gesture then stays outside the guest; without it, as when
     * the window loses focus, the next event starts a gesture afresh. */
    [[nodiscard]] bool release(bool suppressHeldGesture) noexcept;

private:
    Origin origin_;
    std::optional<DisplayPoint> previousTouch_;
    std::uint32_t deliveredButtons_ {};
    bool suppressed_ {};
};

/* Separates the host geometry advertised through Virtio-GPU from the guest's
 * active scanout. Host geometry changes only when automatic resize is
 * configured, while every guest scanout independently updates presentation.
 * All mutation is owned by the GPU transport thread; packed atomic snapshots
 * are also safe for AppKit/UIKit input and layout callbacks. */
class DisplayGeometryPolicy final {
public:
    DisplayGeometryPolicy(
        std::uint32_t width,
        std::uint32_t height,
        bool autoResizeGuest) noexcept;

    [[nodiscard]] DisplaySize advertisedSize() const noexcept;
    [[nodiscard]] DisplaySize activeGuestSize() const noexcept;
    [[nodiscard]] DisplaySize settledHostSize() const noexcept;
    [[nodiscard]] bool autoResizeGuestEnabled() const noexcept;

    /* Records every settled host geometry. Returns true only when it should be
     * advertised to the guest as a new preferred mode. */
    [[nodiscard]] bool hostSizeSettled(
        std::uint32_t width,
        std::uint32_t height) noexcept;
    [[nodiscard]] bool guestScanoutChanged(
        std::uint32_t width,
        std::uint32_t height) noexcept;
    /* Keep published input geometry aligned with presentation storage across
     * a guest reset. Normally activeSize is the settled host size; callers may
     * retain the previous presentation size if allocating it failed. */
    void resetGuestSession(DisplaySize activeSize) noexcept;

private:
    const bool autoResizeGuest_;
    std::atomic_uint64_t advertisedSize_;
    std::atomic_uint64_t activeGuestSize_;
    std::atomic_uint64_t settledHostSize_;
    /* Whether the guest applied a different scanout size since the last
     * advertised resize. Only such a guest change can be followed by a host
     * window, so only then does settling at the active guest size mean
     * following the guest rather than returning to it. */
    std::atomic_bool guestResizedSinceAdvertised_ {false};
};

} // namespace uml::gpu
