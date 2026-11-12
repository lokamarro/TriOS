// SPDX-License-Identifier: GPL-2.0-or-later

#include "display_resize_notifier.hpp"

#include <algorithm>

namespace uml::gpu {

DisplayResizeNotifier::DisplayResizeNotifier(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t maximumDimension)
    : wake_("display resize")
    , maximumDimension_(std::clamp(
          maximumDimension,
          minimumDimension,
          protocol::maximumDisplayDimension))
    , requested_(packDisplaySize({
          clampDimension(width, maximumDimension_),
          clampDimension(height, maximumDimension_),
      }))
{
}

std::uint32_t DisplayResizeNotifier::clampDimension(
    std::uint32_t dimension,
    std::uint32_t maximumDimension) noexcept
{
    const auto boundedMaximum = std::clamp(
        maximumDimension,
        minimumDimension,
        protocol::maximumDisplayDimension);
    return std::clamp(dimension, minimumDimension, boundedMaximum);
}

int DisplayResizeNotifier::descriptor() const noexcept
{
    return wake_.descriptor();
}

std::uint32_t DisplayResizeNotifier::maximumDimension() const noexcept
{
    return maximumDimension_;
}

void DisplayResizeNotifier::request(
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    publish(width, height, false);
}

void DisplayResizeNotifier::requestCommitted(
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    publish(width, height, true);
}

void DisplayResizeNotifier::publish(
    std::uint32_t width,
    std::uint32_t height,
    bool ensureNotification) noexcept
{
    if (width == 0 || height == 0)
        return;
    width = clampDimension(width, maximumDimension_);
    height = clampDimension(height, maximumDimension_);
    const auto encoded = packDisplaySize({width, height});
    if (requested_.exchange(encoded, std::memory_order_acq_rel) == encoded &&
        !ensureNotification) {
        return;
    }
    (void)wake_.notify();
}

DisplaySize DisplayResizeNotifier::drain() noexcept
{
    (void)wake_.drain();
    return unpackDisplaySize(requested_.load(std::memory_order_acquire));
}

} // namespace uml::gpu
