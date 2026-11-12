// SPDX-License-Identifier: GPL-2.0-or-later
#include "convert.hpp"

#include <algorithm>
#include <cstring>

namespace uml::va {
namespace {

// Studio-swing YCbCr to RGB, in 16.16 fixed point. Luma is scaled by
// 255/219 and chroma by 255/224 before the matrix, which is what the
// coefficients below already fold in.
struct Matrix {
    int luma;   // (255/219) << 16
    int rv;
    int gu;
    int gv;
    int bu;
};

constexpr Matrix kBt601 {76309, 104597, -25675, -53279, 132201};
constexpr Matrix kBt709 {76309, 117489, -13975, -34925, 138438};

inline std::uint8_t clamp(int value) noexcept
{
    return std::uint8_t(value < 0 ? 0 : (value > 255 ? 255 : value));
}

}

std::optional<Region> regionWithin(std::int64_t x, std::int64_t y, std::uint64_t width, std::uint64_t height,
                                   unsigned limitWidth, unsigned limitHeight) noexcept
{
    if (x < 0 || y < 0 || std::uint64_t(x) > limitWidth || std::uint64_t(y) > limitHeight) return std::nullopt;
    if (width > limitWidth - std::uint64_t(x) || height > limitHeight - std::uint64_t(y)) return std::nullopt;
    return Region {unsigned(x), unsigned(y), unsigned(width), unsigned(height)};
}

void copyNv12(const ConstPlaneView source[2], const PlaneView destination[2], unsigned width, unsigned height) noexcept
{
    const unsigned chromaRows = (height + 1u) / 2u;
    const unsigned chromaBytes = ((width + 1u) / 2u) * 2u;
    for (unsigned row = 0; row < height; ++row)
        std::memcpy(destination[0].data + std::size_t(row) * destination[0].pitch,
                    source[0].data + std::size_t(row) * source[0].pitch, width);
    for (unsigned row = 0; row < chromaRows; ++row)
        std::memcpy(destination[1].data + std::size_t(row) * destination[1].pitch,
                    source[1].data + std::size_t(row) * source[1].pitch, chromaBytes);
}

void convertNv12ToArgb(const ConstPlaneView source[2], const Region& sourceRegion,
                       const PlaneView& destination, const Region& destinationRegion,
                       bool bt709) noexcept
{
    const Matrix& matrix = bt709 ? kBt709 : kBt601;
    const unsigned width = destinationRegion.width;
    const unsigned height = destinationRegion.height;
    if (!width || !height || !sourceRegion.width || !sourceRegion.height) return;
    const bool direct = width == sourceRegion.width && height == sourceRegion.height;
    for (unsigned row = 0; row < height; ++row) {
        const unsigned sourceRow = direct
            ? sourceRegion.y + row
            : sourceRegion.y + unsigned(std::uint64_t(row) * sourceRegion.height / height);
        const std::uint8_t* luma = source[0].data + std::size_t(sourceRow) * source[0].pitch;
        const std::uint8_t* chroma = source[1].data + std::size_t(sourceRow / 2) * source[1].pitch;
        std::uint8_t* out = destination.data + std::size_t(destinationRegion.y + row) * destination.pitch +
                            std::size_t(destinationRegion.x) * 4;
        for (unsigned column = 0; column < width; ++column) {
            const unsigned sourceColumn = direct
                ? sourceRegion.x + column
                : sourceRegion.x + unsigned(std::uint64_t(column) * sourceRegion.width / width);
            const int y = (int(luma[sourceColumn]) - 16) * matrix.luma;
            const int u = int(chroma[(sourceColumn & ~1u)]) - 128;
            const int v = int(chroma[(sourceColumn & ~1u) + 1]) - 128;
            const int r = (y + matrix.rv * v + 32768) >> 16;
            const int g = (y + matrix.gu * u + matrix.gv * v + 32768) >> 16;
            const int b = (y + matrix.bu * u + 32768) >> 16;
            out[4 * column + 0] = clamp(b);
            out[4 * column + 1] = clamp(g);
            out[4 * column + 2] = clamp(r);
            out[4 * column + 3] = 0xff;
        }
    }
}

void copyPacked32(const ConstPlaneView& source, const Region& sourceRegion,
                  const PlaneView& destination, const Region& destinationRegion) noexcept
{
    const unsigned width = destinationRegion.width;
    const unsigned height = destinationRegion.height;
    if (!width || !height || !sourceRegion.width || !sourceRegion.height) return;
    const bool direct = width == sourceRegion.width && height == sourceRegion.height;
    for (unsigned row = 0; row < height; ++row) {
        const unsigned sourceRow = direct
            ? sourceRegion.y + row
            : sourceRegion.y + unsigned(std::uint64_t(row) * sourceRegion.height / height);
        const std::uint8_t* in = source.data + std::size_t(sourceRow) * source.pitch;
        std::uint8_t* out = destination.data + std::size_t(destinationRegion.y + row) * destination.pitch +
                            std::size_t(destinationRegion.x) * 4;
        if (direct) {
            std::memcpy(out, in + std::size_t(sourceRegion.x) * 4, std::size_t(width) * 4);
            continue;
        }
        for (unsigned column = 0; column < width; ++column) {
            const unsigned sourceColumn = sourceRegion.x + unsigned(std::uint64_t(column) * sourceRegion.width / width);
            std::memcpy(out + std::size_t(column) * 4, in + std::size_t(sourceColumn) * 4, 4);
        }
    }
}


namespace {

// Studio-swing RGB to YCbCr, the inverse of the matrices above, in 16.16
// fixed point. The offsets are the 16 and 128 a limited-range encoder adds.
struct Forward {
    int yr, yg, yb;
    int ur, ug, ub;
    int vr, vg, vb;
};
constexpr Forward kForward601 {16829, 33039, 6416, -9714, -19070, 28784, 28784, -24103, -4681};
constexpr Forward kForward709 {11966, 40254, 4064, -6595, -22189, 28784, 28784, -26145, -2639};

}

void convertArgbToNv12(const ConstPlaneView& source, const Region& sourceRegion,
                       const PlaneView destination[2], const Region& destinationRegion,
                       bool bt709) noexcept
{
    if (!source.data || !destination[0].data || !destination[1].data) return;
    if (!sourceRegion.width || !sourceRegion.height) return;
    const Forward& matrix = bt709 ? kForward709 : kForward601;
    const unsigned width = destinationRegion.width & ~1u;
    const unsigned height = destinationRegion.height & ~1u;
    for (unsigned row = 0; row < height; row += 2) {
        for (unsigned column = 0; column < width; column += 2) {
            int sums[3] {};
            for (unsigned dy = 0; dy < 2; ++dy) {
                for (unsigned dx = 0; dx < 2; ++dx) {
                    const unsigned outputY = row + dy;
                    const unsigned outputX = column + dx;
                    const unsigned inputY = sourceRegion.y +
                        (destinationRegion.height ? outputY * sourceRegion.height / destinationRegion.height : 0);
                    const unsigned inputX = sourceRegion.x +
                        (destinationRegion.width ? outputX * sourceRegion.width / destinationRegion.width : 0);
                    const std::uint8_t* pixel = source.data + std::size_t(inputY) * source.pitch +
                                                std::size_t(inputX) * 4;
                    const int blue = pixel[0];
                    const int green = pixel[1];
                    const int red = pixel[2];
                    sums[0] += red;
                    sums[1] += green;
                    sums[2] += blue;
                    const int luma = (matrix.yr * red + matrix.yg * green + matrix.yb * blue + 32768) >> 16;
                    destination[0].data[std::size_t(destinationRegion.y + outputY) * destination[0].pitch +
                                        destinationRegion.x + outputX] = clamp(luma + 16);
                }
            }
            const int red = sums[0] / 4;
            const int green = sums[1] / 4;
            const int blue = sums[2] / 4;
            const int blueDifference = (matrix.ur * red + matrix.ug * green + matrix.ub * blue + 32768) >> 16;
            const int redDifference = (matrix.vr * red + matrix.vg * green + matrix.vb * blue + 32768) >> 16;
            std::uint8_t* chroma = destination[1].data +
                std::size_t((destinationRegion.y + row) / 2) * destination[1].pitch +
                (destinationRegion.x + column);
            chroma[0] = clamp(blueDifference + 128);
            chroma[1] = clamp(redDifference + 128);
        }
    }
}

}
