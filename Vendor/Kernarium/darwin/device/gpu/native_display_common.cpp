// SPDX-License-Identifier: GPL-2.0-or-later

#include "native_display.hpp"

#include "native_display_state.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace uml::gpu {

NativeDisplayStateCore::NativeDisplayStateCore(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t maximumDimension,
    bool autoResizeGuest)
    : presentation(
          DisplayResizeNotifier::clampDimension(width, maximumDimension),
          DisplayResizeNotifier::clampDimension(height, maximumDimension))
    , resizeNotifier(width, height, maximumDimension)
    , geometryPolicy(
          DisplayResizeNotifier::clampDimension(width, maximumDimension),
          DisplayResizeNotifier::clampDimension(height, maximumDimension),
          autoResizeGuest)
{
}

bool NativeDisplayStateCore::drainResizeEvent()
{
    std::lock_guard lock(resizeLifecycleMutex);
    const auto size = resizeNotifier.drain();
    if (resizeInProgress.load(std::memory_order_acquire))
        return false;
    const bool changed = geometryPolicy.hostSizeSettled(
        size.width, size.height);
    resizeCommitPending.store(false, std::memory_order_release);
    return changed;
}

std::optional<std::uint32_t> displayDimension(
    double dimension,
    std::uint32_t maximum) noexcept
{
    if (!std::isfinite(dimension) || dimension < 1.0)
        return std::nullopt;
    return static_cast<std::uint32_t>(std::min<double>(
        std::floor(dimension), maximum));
}

NativeDisplay::NativeDisplay(std::shared_ptr<NativeDisplayStateCore> state)
    : state_(std::move(state))
{
    if (!state_)
        throw std::invalid_argument("a native display needs a state");
}

NativeDisplay::~NativeDisplay()
{
    state_->shutdown();
}

std::uint32_t NativeDisplay::width() const noexcept
{
    return state_->geometryPolicy.advertisedSize().width;
}

std::uint32_t NativeDisplay::height() const noexcept
{
    return state_->geometryPolicy.advertisedSize().height;
}

int NativeDisplay::resizeEventDescriptor() const noexcept
{
    return state_->resizeNotifier.descriptor();
}

bool NativeDisplay::drainResizeEvent()
{
    return state_->drainResizeEvent();
}

bool NativeDisplay::guestScanoutChanged(
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    return state_->guestScanoutChanged(width, height);
}

void NativeDisplay::resetGuestSession() noexcept
{
    state_->resetGuestSession();
}

bool NativeDisplay::claimResizeNotification() noexcept
{
    state_->resizeLifecycleMutex.lock();
    if (!state_->geometryPolicy.autoResizeGuestEnabled() ||
        state_->resizeInProgress.load(std::memory_order_acquire) ||
        state_->resizeCommitPending.load(std::memory_order_acquire)) {
        state_->resizeLifecycleMutex.unlock();
        return false;
    }
    return true;
}

void NativeDisplay::releaseResizeNotification() noexcept
{
    state_->resizeLifecycleMutex.unlock();
}

void NativeDisplay::clear() noexcept
{
    state_->presentation.framebuffer().clear();
    state_->presentation.publishCpuFrame();
}

void NativeDisplay::update(
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
    std::uint32_t height)
{
    const auto rectangle = state_->presentation.framebuffer().update(
        source,
        sourceWidth,
        sourceHeight,
        sourceRowBytes,
        sourceFormat,
        sourceX,
        sourceY,
        destinationX,
        destinationY,
        width,
        height);
    if (rectangle.width != 0 && rectangle.height != 0)
        state_->presentation.publishCpuFrame();
}

void NativeDisplay::fillSelfTestPattern()
{
    state_->presentation.framebuffer().fillSelfTestPattern();
    state_->presentation.publishCpuFrame();
}

PresentOutcome NativeDisplay::presentMetal(
    const MetalScanout& scanout) noexcept
{
    return state_->presentation.present(scanout);
}

DisplayStatistics NativeDisplay::statistics() const noexcept
{
    return state_->presentation.statistics();
}

void NativeDisplay::recordCpuFallbackFrame() noexcept
{
    state_->presentation.recordCpuFallbackFrame();
}

} // namespace uml::gpu
