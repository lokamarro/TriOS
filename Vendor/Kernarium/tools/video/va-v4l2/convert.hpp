// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace uml::va {

// A plane of an image in guest memory.
struct PlaneView {
    std::uint8_t* data {};
    std::size_t pitch {};
};
struct ConstPlaneView {
    const std::uint8_t* data {};
    std::size_t pitch {};
};

// A rectangle of one image, used for the crop a VA-API video-processing blit
// asks for.
struct Region {
    unsigned x {};
    unsigned y {};
    unsigned width {};
    unsigned height {};
};

// The rectangle an application asked for at (x, y), when it lies inside a
// limitWidth by limitHeight image. Application coordinates may be negative
// and sizes arbitrary, so the test is written so that no sum can wrap.
std::optional<Region> regionWithin(std::int64_t x, std::int64_t y, std::uint64_t width, std::uint64_t height,
                                   unsigned limitWidth, unsigned limitHeight) noexcept;

// Copies one NV12 image, honouring differing pitches.
void copyNv12(const ConstPlaneView source[2], const PlaneView destination[2], unsigned width, unsigned height) noexcept;

// Converts an NV12 region into a packed 32-bit image whose bytes are B, G, R,
// A in memory (DRM_FORMAT_ARGB8888, which VA-API calls VA_FOURCC_ARGB). The
// source is sampled with nearest neighbour when the regions differ in size.
// `bt709` picks the luma coefficients; both matrices assume the studio-swing
// range a hardware decoder produces.
void convertNv12ToArgb(const ConstPlaneView source[2], const Region& sourceRegion,
                       const PlaneView& destination, const Region& destinationRegion,
                       bool bt709) noexcept;

// Converts a packed 32-bit region (B, G, R, A in memory) back into NV12,
// with the same studio-swing matrices as the forward conversion. Chroma is
// averaged over each 2x2 block.
void convertArgbToNv12(const ConstPlaneView& source, const Region& sourceRegion,
                       const PlaneView destination[2], const Region& destinationRegion,
                       bool bt709) noexcept;

// Copies a packed 32-bit image region.
void copyPacked32(const ConstPlaneView& source, const Region& sourceRegion,
                  const PlaneView& destination, const Region& destinationRegion) noexcept;

}
