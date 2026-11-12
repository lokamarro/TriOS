// SPDX-License-Identifier: GPL-2.0-or-later

#include "display_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace uml::gpu {

AspectFitRectangle aspectFitRectangle(
    double containerWidth,
    double containerHeight,
    double contentWidth,
    double contentHeight) noexcept
{
    if (!std::isfinite(containerWidth) ||
        !std::isfinite(containerHeight) ||
        !std::isfinite(contentWidth) ||
        !std::isfinite(contentHeight) ||
        containerWidth <= 0.0 || containerHeight <= 0.0 ||
        contentWidth <= 0.0 || contentHeight <= 0.0) {
        return {};
    }
    const double scale = std::min(
        containerWidth / contentWidth,
        containerHeight / contentHeight);
    const double width = contentWidth * scale;
    const double height = contentHeight * scale;
    return {
        .x = (containerWidth - width) / 2.0,
        .y = (containerHeight - height) / 2.0,
        .width = width,
        .height = height,
    };
}

DisplayRectangle aspectFitPixels(
    DisplaySize container,
    DisplaySize content) noexcept
{
    const auto fitted = aspectFitRectangle(
        container.width, container.height, content.width, content.height);
    if (fitted.width <= 0.0 || fitted.height <= 0.0)
        return {};
    const auto span = [](double origin,
                          double length,
                          std::uint32_t limit) {
        const auto edge = [limit](double value) {
            return static_cast<std::uint32_t>(std::clamp(
                std::round(value), 0.0, static_cast<double>(limit)));
        };
        auto first = edge(origin);
        auto last = edge(origin + length);
        if (first == last) {
            if (last < limit)
                ++last;
            else
                --first;
        }
        return std::pair {first, last - first};
    };
    const auto [x, width] = span(fitted.x, fitted.width, container.width);
    const auto [y, height] = span(fitted.y, fitted.height, container.height);
    return {.x = x, .y = y, .width = width, .height = height};
}

DisplayRotation rotateDisplay(
    DisplayRotation rotation,
    int quarterTurns) noexcept
{
    constexpr int rotationCount = 4;
    int value = static_cast<int>(rotation) + quarterTurns % rotationCount;
    value %= rotationCount;
    if (value < 0)
        value += rotationCount;
    return static_cast<DisplayRotation>(value);
}

DisplaySize presentedDisplaySize(
    DisplaySize guestSize,
    DisplayRotation rotation) noexcept
{
    if ((static_cast<std::uint8_t>(rotation) & 1U) == 0)
        return guestSize;
    return {.width = guestSize.height, .height = guestSize.width};
}

DisplayPoint guestPointFromPresentedPoint(
    DisplayPoint point,
    DisplayRotation rotation) noexcept
{
    switch (rotation) {
    case DisplayRotation::upright:
        return point;
    case DisplayRotation::clockwise90:
        return {.x = point.y, .y = 1.0 - point.x};
    case DisplayRotation::upsideDown:
        return {.x = 1.0 - point.x, .y = 1.0 - point.y};
    case DisplayRotation::counterclockwise90:
        return {.x = 1.0 - point.y, .y = point.x};
    }
    return point;
}

std::optional<ScanoutSampling> scanoutSampling(
    const ScanoutRegion& region) noexcept
{
    const auto fits = [](std::uint32_t offset,
                          std::uint32_t length,
                          std::uint32_t limit) {
        return length != 0 &&
            static_cast<std::uint64_t>(offset) + length <= limit;
    };
    if (region.source.width == 0 || region.source.height == 0 ||
        region.texture.width < region.source.width ||
        region.texture.height < region.source.height ||
        !fits(region.x, region.width, region.source.width) ||
        !fits(region.y, region.height, region.source.height)) {
        return std::nullopt;
    }
    const double textureWidth = region.texture.width;
    const double textureHeight = region.texture.height;
    const double left = region.x;
    const double right = left + region.width;
    /* Texture rows holding the rectangle's top and bottom edges. A flipped
     * source keeps its bottom row in texture row 0, so its top edge is the
     * higher row. */
    const double top = region.flipped ?
        static_cast<double>(region.source.height) - region.y :
        static_cast<double>(region.y);
    const double bottom = region.flipped ?
        top - region.height : top + region.height;
    const double lowRow = std::min(top, bottom);
    const double highRow = std::max(top, bottom);
    return ScanoutSampling {
        .content = {.width = region.width, .height = region.height},
        .origin = {.x = left / textureWidth, .y = top / textureHeight},
        .extent = {
            .x = (right - left) / textureWidth,
            .y = (bottom - top) / textureHeight,
        },
        .minimum = {
            .x = (left + 0.5) / textureWidth,
            .y = (lowRow + 0.5) / textureHeight,
        },
        .maximum = {
            .x = (right - 0.5) / textureWidth,
            .y = (highRow - 0.5) / textureHeight,
        },
    };
}

std::optional<ClippedDisplaySegment> clipDisplaySegment(
    AspectFitRectangle rectangle,
    DisplayPoint start,
    DisplayPoint end) noexcept
{
    const double right = rectangle.x + rectangle.width;
    const double top = rectangle.y + rectangle.height;
    if (!std::isfinite(rectangle.x) || !std::isfinite(rectangle.y) ||
        !std::isfinite(rectangle.width) ||
        !std::isfinite(rectangle.height) || !std::isfinite(right) ||
        !std::isfinite(top) || rectangle.width <= 0.0 ||
        rectangle.height <= 0.0 || !std::isfinite(start.x) ||
        !std::isfinite(start.y) || !std::isfinite(end.x) ||
        !std::isfinite(end.y)) {
        return std::nullopt;
    }

    const double deltaX = end.x - start.x;
    const double deltaY = end.y - start.y;
    double entry = 0.0;
    double exit = 1.0;
    const auto clip = [&](double direction, double distance) {
        if (direction == 0.0)
            return distance >= 0.0;
        const double fraction = distance / direction;
        if (direction < 0.0) {
            if (fraction > exit)
                return false;
            entry = std::max(entry, fraction);
        } else {
            if (fraction < entry)
                return false;
            exit = std::min(exit, fraction);
        }
        return true;
    };
    if (!clip(-deltaX, start.x - rectangle.x) ||
        !clip(deltaX, right - start.x) ||
        !clip(-deltaY, start.y - rectangle.y) ||
        !clip(deltaY, top - start.y) || entry > exit) {
        return std::nullopt;
    }

    const auto inside = [&](DisplayPoint point) {
        return point.x >= rectangle.x && point.x <= right &&
            point.y >= rectangle.y && point.y <= top;
    };
    if (entry == exit && !inside(start) && !inside(end)) {
        /* Merely touching one corner has no path inside the display and must
         * not become a zero-duration guest contact. */
        return std::nullopt;
    }

    const auto pointAt = [&](double fraction) {
        return DisplayPoint {
            .x = std::clamp(
                start.x + fraction * deltaX, rectangle.x, right),
            .y = std::clamp(
                start.y + fraction * deltaY, rectangle.y, top),
        };
    };
    return ClippedDisplaySegment {
        .entry = pointAt(entry),
        .exit = pointAt(exit),
    };
}

namespace {

[[nodiscard]] bool contains(
    AspectFitRectangle rectangle,
    DisplayPoint point) noexcept
{
    return rectangle.width > 0.0 && rectangle.height > 0.0 &&
        point.x >= rectangle.x && point.x <= rectangle.x + rectangle.width &&
        point.y >= rectangle.y && point.y <= rectangle.y + rectangle.height;
}

[[nodiscard]] DisplayPoint guestPoint(
    AspectFitRectangle rectangle,
    DisplayPoint point,
    bool originAtBottom) noexcept
{
    const double fromTop = originAtBottom ?
        rectangle.y + rectangle.height - point.y : point.y - rectangle.y;
    return {
        .x = std::clamp((point.x - rectangle.x) / rectangle.width, 0.0, 1.0),
        .y = std::clamp(fromTop / rectangle.height, 0.0, 1.0),
    };
}

constexpr std::uint32_t pointerButtonMask = 7U;

} // namespace

GuestPointerSamples FittedPointer::mouse(
    AspectFitRectangle content,
    DisplayPoint location,
    std::uint32_t buttons) noexcept
{
    const bool originAtBottom = origin_ == Origin::bottomLeft;
    buttons &= pointerButtonMask;
    if (suppressed_) {
        if (buttons == 0)
            suppressed_ = false;
        return {};
    }
    if (!contains(content, location) && deliveredButtons_ == 0) {
        if (buttons != 0)
            suppressed_ = true;
        return {};
    }
    deliveredButtons_ = buttons;
    GuestPointerSamples result;
    result.samples[result.count++] = {
        .point = guestPoint(content, location, originAtBottom),
        .buttons = buttons,
    };
    return result;
}

GuestPointerSamples FittedPointer::touch(
    AspectFitRectangle content,
    DisplayPoint location,
    bool pressed) noexcept
{
    const bool originAtBottom = origin_ == Origin::bottomLeft;
    GuestPointerSamples result;
    if (suppressed_) {
        if (!pressed) {
            suppressed_ = false;
            previousTouch_.reset();
        }
        return result;
    }
    const auto deliver = [&](DisplayPoint point, bool active) {
        /* Lifting a contact the guest never saw reports nothing. */
        if (!active && deliveredButtons_ == 0)
            return;
        deliveredButtons_ = active ? 1U : 0U;
        result.samples[result.count++] = {
            .point = guestPoint(content, point, originAtBottom),
            .buttons = deliveredButtons_,
        };
    };
    const auto samePoint = [](DisplayPoint left, DisplayPoint right) {
        return left.x == right.x && left.y == right.y;
    };
    const bool inside = contains(content, location);
    const bool contactActive = deliveredButtons_ != 0;

    if (!pressed) {
        if (contactActive) {
            DisplayPoint release = location;
            if (!inside && previousTouch_) {
                if (const auto clipped = clipDisplaySegment(
                        content, *previousTouch_, location)) {
                    release = clipped->exit;
                }
            }
            deliver(release, false);
        } else if (previousTouch_) {
            if (const auto clipped = clipDisplaySegment(
                    content, *previousTouch_, location)) {
                deliver(clipped->entry, true);
                if (inside) {
                    if (!samePoint(clipped->entry, location))
                        deliver(location, true);
                    deliver(location, false);
                } else {
                    deliver(clipped->exit, false);
                }
            }
        }
        previousTouch_.reset();
        return result;
    }

    if (!previousTouch_) {
        previousTouch_ = location;
        if (inside)
            deliver(location, true);
        return result;
    }

    const auto clipped =
        clipDisplaySegment(content, *previousTouch_, location);
    if (contactActive) {
        if (inside) {
            deliver(location, true);
        } else {
            deliver(clipped ? clipped->exit : *previousTouch_, false);
        }
    } else if (clipped) {
        deliver(clipped->entry, true);
        if (inside) {
            if (!samePoint(clipped->entry, location))
                deliver(location, true);
        } else {
            deliver(clipped->exit, false);
        }
    }
    previousTouch_ = location;
    return result;
}

bool FittedPointer::release(bool suppressHeldGesture) noexcept
{
    const bool held = deliveredButtons_ != 0;
    if (!suppressHeldGesture)
        suppressed_ = false;
    else if (held)
        suppressed_ = true;
    deliveredButtons_ = 0;
    previousTouch_.reset();
    return held;
}

DisplayGeometryPolicy::DisplayGeometryPolicy(
    std::uint32_t width,
    std::uint32_t height,
    bool autoResizeGuest) noexcept
    : autoResizeGuest_(autoResizeGuest)
    , advertisedSize_(packDisplaySize({width, height}))
    , activeGuestSize_(packDisplaySize({width, height}))
    , settledHostSize_(packDisplaySize({width, height}))
{
}

DisplaySize DisplayGeometryPolicy::advertisedSize() const noexcept
{
    return unpackDisplaySize(advertisedSize_.load(std::memory_order_acquire));
}

DisplaySize DisplayGeometryPolicy::activeGuestSize() const noexcept
{
    return unpackDisplaySize(activeGuestSize_.load(std::memory_order_acquire));
}

DisplaySize DisplayGeometryPolicy::settledHostSize() const noexcept
{
    return unpackDisplaySize(settledHostSize_.load(std::memory_order_acquire));
}

bool DisplayGeometryPolicy::autoResizeGuestEnabled() const noexcept
{
    return autoResizeGuest_;
}

bool DisplayGeometryPolicy::hostSizeSettled(
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    const auto encoded = packDisplaySize({width, height});
    settledHostSize_.store(encoded, std::memory_order_release);
    if (!autoResizeGuestEnabled())
        return false;
    const auto previous = advertisedSize_.exchange(
        encoded, std::memory_order_acq_rel);
    if (previous == encoded)
        return false;
    if (activeGuestSize_.load(std::memory_order_acquire) == encoded &&
        guestResizedSinceAdvertised_.load(std::memory_order_acquire)) {
        /* AppKit follows every applied guest mode. Its resulting drawable
         * callback settles at the already-active guest size and must not feed
         * that size back to the guest as another resize request. A host that
         * returns to the active size before the guest applied anything is not
         * following it: the guest still holds the size advertised last, so
         * the returned size is advertised again. */
        return false;
    }
    guestResizedSinceAdvertised_.store(false, std::memory_order_release);
    return true;
}

bool DisplayGeometryPolicy::guestScanoutChanged(
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    if (width == 0 || height == 0)
        return false;
    const auto encoded = packDisplaySize({width, height});
    const bool changed =
        activeGuestSize_.exchange(encoded, std::memory_order_acq_rel) !=
        encoded;
    if (changed)
        guestResizedSinceAdvertised_.store(true, std::memory_order_release);
    return changed;
}

void DisplayGeometryPolicy::resetGuestSession(DisplaySize activeSize) noexcept
{
    const auto settled = settledHostSize_.load(std::memory_order_acquire);
    advertisedSize_.store(settled, std::memory_order_release);
    const auto active = activeSize.width == 0 || activeSize.height == 0 ?
        settled : packDisplaySize(activeSize);
    activeGuestSize_.store(active, std::memory_order_release);
    guestResizedSinceAdvertised_.store(false, std::memory_order_release);
}

} // namespace uml::gpu
