// SPDX-License-Identifier: GPL-2.0-or-later

#include "scanout_framebuffer.hpp"

#include <arm_neon.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace uml::gpu {
namespace {

constexpr std::uint8_t opaqueAlpha = 0xffU;

std::uint64_t displayDimensions(
    std::uint32_t width,
    std::uint32_t height) noexcept
{
    return static_cast<std::uint64_t>(width) << 32U | height;
}

std::uint32_t displayWidth(std::uint64_t dimensions) noexcept
{
    return static_cast<std::uint32_t>(dimensions >> 32U);
}

std::uint32_t displayHeight(std::uint64_t dimensions) noexcept
{
    return static_cast<std::uint32_t>(dimensions);
}

std::size_t framebufferSize(std::uint32_t width, std::uint32_t height)
{
    if (width == 0 || height == 0 ||
        width > static_cast<std::uint32_t>(
                    std::numeric_limits<int>::max()) ||
        height > static_cast<std::uint32_t>(
                     std::numeric_limits<int>::max()) ||
        width > std::numeric_limits<std::size_t>::max() / height /
            protocol::gpuBytesPerPixel) {
        throw std::invalid_argument("invalid Virtio-GPU display dimensions");
    }
    return static_cast<std::size_t>(width) * height *
        protocol::gpuBytesPerPixel;
}

/* Convert straight into the locked framebuffer, with no full-image staging
 * allocation or second copy. All supported formats are byte permutations;
 * the vector loop and scalar tail perform the same permutation. */
template<unsigned blue, unsigned green, unsigned red, unsigned alpha>
void permuteRectangle(
    std::uint8_t* destination,
    const std::uint8_t* source,
    std::uint32_t width,
    std::uint32_t height,
    std::size_t sourceRowBytes,
    std::size_t destinationRowBytes) noexcept
{
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto* input = source + y * sourceRowBytes;
        auto* output = destination + y * destinationRowBytes;
        std::uint32_t x = 0;
        for (; width - x >= 16U; x += 16U) {
            const auto offset = static_cast<std::size_t>(x) * 4U;
            const auto channels = vld4q_u8(input + offset);
            uint8x16x4_t pixels;
            pixels.val[0] = channels.val[blue];
            pixels.val[1] = channels.val[green];
            pixels.val[2] = channels.val[red];
            pixels.val[3] = channels.val[alpha];
            vst4q_u8(output + offset, pixels);
        }
        for (; x < width; ++x) {
            const auto offset = static_cast<std::size_t>(x) * 4U;
            output[offset] = input[offset + blue];
            output[offset + 1U] = input[offset + green];
            output[offset + 2U] = input[offset + red];
            output[offset + 3U] = input[offset + alpha];
        }
    }
}

using PixelConversion = void (*)(std::uint8_t*, const std::uint8_t*,
    std::uint32_t, std::uint32_t, std::size_t, std::size_t) noexcept;

PixelConversion pixelConversion(protocol::GpuFormat format)
{
    switch (format) {
    case protocol::GpuFormat::b8g8r8a8Unorm:
    case protocol::GpuFormat::b8g8r8x8Unorm:
        return nullptr;
    case protocol::GpuFormat::a8r8g8b8Unorm:
    case protocol::GpuFormat::x8r8g8b8Unorm:
        return &permuteRectangle<3, 2, 1, 0>;
    case protocol::GpuFormat::r8g8b8a8Unorm:
    case protocol::GpuFormat::r8g8b8x8Unorm:
        return &permuteRectangle<2, 1, 0, 3>;
    case protocol::GpuFormat::x8b8g8r8Unorm:
    case protocol::GpuFormat::a8b8g8r8Unorm:
        return &permuteRectangle<1, 2, 3, 0>;
    }
    throw std::invalid_argument("unsupported Virtio-GPU pixel format");
}

std::vector<std::uint8_t> initialPixels(
    std::uint32_t width,
    std::uint32_t height,
    FramebufferStorage storage)
{
    const auto size = framebufferSize(width, height);
    if (storage == FramebufferStorage::lazy)
        return {};
    return std::vector<std::uint8_t>(size);
}

} // namespace

ScanoutFramebuffer::ScanoutFramebuffer(
    std::uint32_t width,
    std::uint32_t height,
    FramebufferStorage storage)
    : dimensions_(displayDimensions(width, height))
    , storage_(storage)
    , pixels_(initialPixels(width, height, storage))
{
}

std::uint32_t ScanoutFramebuffer::width() const noexcept
{
    return displayWidth(dimensions_.load(std::memory_order_acquire));
}

std::uint32_t ScanoutFramebuffer::height() const noexcept
{
    return displayHeight(dimensions_.load(std::memory_order_acquire));
}

std::size_t ScanoutFramebuffer::rowBytes() const noexcept
{
    return static_cast<std::size_t>(width()) * protocol::gpuBytesPerPixel;
}

bool ScanoutFramebuffer::hasStorage() const noexcept
{
    return !pixels_.empty();
}

std::span<const std::uint8_t> ScanoutFramebuffer::pixels() const noexcept
{
    return pixels_;
}

std::uint8_t* ScanoutFramebuffer::data() noexcept
{
    return pixels_.empty() ? nullptr : pixels_.data();
}

void ScanoutFramebuffer::lock() noexcept
{
    mutex_.lock();
}

void ScanoutFramebuffer::unlock() noexcept
{
    mutex_.unlock();
}

void ScanoutFramebuffer::resize(
    std::uint32_t width,
    std::uint32_t height)
{
    const auto replacementDimensions = displayDimensions(width, height);
    if (dimensions_.load(std::memory_order_acquire) == replacementDimensions)
        return;
    /* The previous storage is freed with this vector, after the unlock. */
    auto replacement = initialPixels(width, height, storage_);
    std::lock_guard lock(mutex_);
    pixels_.swap(replacement);
    dimensions_.store(replacementDimensions, std::memory_order_release);
}

void ScanoutFramebuffer::clear() noexcept
{
    /* Released storage is freed with this vector, after the unlock. */
    std::vector<std::uint8_t> released;
    std::lock_guard lock(mutex_);
    if (storage_ == FramebufferStorage::lazy)
        released.swap(pixels_);
    else
        clearLocked();
}

void ScanoutFramebuffer::clearLocked() noexcept
{
    if (storage_ == FramebufferStorage::retained)
        std::fill(pixels_.begin(), pixels_.end(), 0);
    else
        std::vector<std::uint8_t>().swap(pixels_);
}

void ScanoutFramebuffer::ensureStorageLocked(std::uint64_t dimensions)
{
    if (pixels_.empty()) {
        pixels_.resize(framebufferSize(
            displayWidth(dimensions), displayHeight(dimensions)));
    }
}

ScanoutRectangle ScanoutFramebuffer::update(
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
    const auto framebufferDimensions =
        dimensions_.load(std::memory_order_acquire);
    const auto destinationWidth = displayWidth(framebufferDimensions);
    const auto destinationHeight = displayHeight(framebufferDimensions);
    const auto minimumSourceRowBytes =
        static_cast<std::size_t>(sourceWidth) *
        protocol::gpuBytesPerPixel;
    if (sourceRowBytes < minimumSourceRowBytes)
        throw std::invalid_argument("short Virtio-GPU source stride");
    if (sourceHeight != 0) {
        const auto precedingRows =
            static_cast<std::size_t>(sourceHeight - 1U);
        if (precedingRows != 0 &&
            sourceRowBytes >
                (std::numeric_limits<std::size_t>::max() -
                 minimumSourceRowBytes) /
                    precedingRows) {
            throw std::invalid_argument(
                "overflowing Virtio-GPU source layout");
        }
        const auto requiredSourceBytes =
            precedingRows * sourceRowBytes + minimumSourceRowBytes;
        if (requiredSourceBytes > source.size())
            throw std::invalid_argument("short Virtio-GPU resource");
    }
    if (sourceX >= sourceWidth || sourceY >= sourceHeight ||
        destinationX >= destinationWidth ||
        destinationY >= destinationHeight) {
        return {};
    }
    const auto rectangleWidth = std::min(
        {width, sourceWidth - sourceX, destinationWidth - destinationX});
    const auto rectangleHeight = std::min(
        {height, sourceHeight - sourceY, destinationHeight - destinationY});
    if (rectangleWidth == 0 || rectangleHeight == 0)
        return {};

    const auto convertedRowBytes =
        static_cast<std::size_t>(rectangleWidth) *
        protocol::gpuBytesPerPixel;
    const auto sourceOffset =
        static_cast<std::size_t>(sourceY) * sourceRowBytes +
        static_cast<std::size_t>(sourceX) * protocol::gpuBytesPerPixel;
    const auto convert = pixelConversion(sourceFormat);
    // Released after unlocking, including the exceptional overlapping case.
    std::vector<std::uint8_t> converted;
    std::lock_guard lock(mutex_);
    if (dimensions_.load(std::memory_order_acquire) != framebufferDimensions)
        return {};

    const auto sourceAddress = reinterpret_cast<std::uintptr_t>(source.data());
    const auto storageAddress = reinterpret_cast<std::uintptr_t>(pixels_.data());
    const bool overlaps = !pixels_.empty() && (sourceAddress < storageAddress ?
        storageAddress - sourceAddress < source.size() :
        sourceAddress - storageAddress < pixels_.size());
    const auto* rows = source.data() + sourceOffset;
    auto rowStride = sourceRowBytes;
    if (overlaps) {
        // A caller may update a rectangle of this framebuffer from another
        // rectangle of itself, in any format. Rows written first would
        // overwrite rows not yet read, and a row copied onto itself shifted
        // overlaps its own source, so preserve the input before writing.
        if (rectangleHeight >
            std::numeric_limits<std::size_t>::max() / convertedRowBytes) {
            throw std::invalid_argument("overflowing display update rectangle");
        }
        converted.resize(convertedRowBytes * rectangleHeight);
        if (convert != nullptr) {
            convert(converted.data(), rows, rectangleWidth, rectangleHeight,
                rowStride, convertedRowBytes);
        } else {
            for (std::uint32_t row = 0; row < rectangleHeight; ++row) {
                std::memcpy(
                    converted.data() +
                        static_cast<std::size_t>(row) * convertedRowBytes,
                    rows + static_cast<std::size_t>(row) * rowStride,
                    convertedRowBytes);
            }
        }
        rows = converted.data();
        rowStride = convertedRowBytes;
    }
    ensureStorageLocked(framebufferDimensions);
    const auto destinationOffset =
        (static_cast<std::size_t>(destinationY) * destinationWidth +
            destinationX) * protocol::gpuBytesPerPixel;
    if (convert != nullptr && !overlaps) {
        convert(pixels_.data() + destinationOffset, rows,
            rectangleWidth, rectangleHeight, rowStride,
            static_cast<std::size_t>(destinationWidth) * protocol::gpuBytesPerPixel);
    } else {
        for (std::uint32_t row = 0; row < rectangleHeight; ++row) {
            const auto rowOffset = static_cast<std::size_t>(row) *
                destinationWidth * protocol::gpuBytesPerPixel;
            std::memcpy(
                pixels_.data() + destinationOffset + rowOffset,
                rows + static_cast<std::size_t>(row) * rowStride,
                convertedRowBytes);
        }
    }
    return {.x = destinationX, .y = destinationY,
        .width = rectangleWidth, .height = rectangleHeight};
}

void ScanoutFramebuffer::fillSelfTestPattern()
{
    std::lock_guard lock(mutex_);
    fillSelfTestPatternLocked();
}

void ScanoutFramebuffer::fillSelfTestPatternLocked()
{
    alignas(16) static constexpr std::array<std::uint8_t, 16> pixelLanes {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    };
    constexpr std::uint8_t testRed = 0x5aU;

    const auto dimensions = dimensions_.load(std::memory_order_acquire);
    ensureStorageLocked(dimensions);
    const auto currentWidth = displayWidth(dimensions);
    const auto currentHeight = displayHeight(dimensions);
    for (std::uint32_t y = 0; y < currentHeight; ++y) {
        std::uint32_t x = 0;
        const uint8x16_t lane = vld1q_u8(pixelLanes.data());
        for (; currentWidth - x >= 16U; x += 16U) {
            const auto offset =
                (static_cast<std::size_t>(y) * currentWidth + x) * 4U;
            uint8x16x4_t pixels {};
            pixels.val[0] =
                vaddq_u8(lane, vdupq_n_u8(static_cast<std::uint8_t>(x)));
            pixels.val[1] =
                vdupq_n_u8(static_cast<std::uint8_t>(y));
            pixels.val[2] = vdupq_n_u8(testRed);
            pixels.val[3] = vdupq_n_u8(opaqueAlpha);
            vst4q_u8(pixels_.data() + offset, pixels);
        }
        for (; x < currentWidth; ++x) {
            const auto offset =
                (static_cast<std::size_t>(y) * currentWidth + x) * 4U;
            pixels_[offset] = static_cast<std::uint8_t>(x & 0xffU);
            pixels_[offset + 1U] = static_cast<std::uint8_t>(y & 0xffU);
            pixels_[offset + 2U] = testRed;
            pixels_[offset + 3U] = opaqueAlpha;
        }
    }
}

} // namespace uml::gpu
