// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace uml::video {

/* The row pitch of a raw picture in a guest buffer.
 *
 * A guest allocator pads the rows of a buffer that the host GPU samples: the
 * Android guest's minigbm lays an emulated NV12, NV21 and P010 buffer out at a
 * pitch of a multiple of 16 bytes, an emulated YV12 buffer at a multiple of 32
 * bytes for luma and half of that for each chroma plane, and a packed 32-bit
 * buffer at a whole number of pixels that fills 16 bytes, because Metal reads
 * a texture over the buffer only at a row pitch that is a multiple of 16. Plane
 * heights stay tight, so a plane starts where the rows of the earlier planes
 * end. The device states this pitch in the plane formats it negotiates and
 * writes and reads every row at it, which makes one rule of the allocator, the
 * GPU and the codec. A client that honors the stride a V4L2 device reports
 * (every one does) needs no allocator of its own. */
inline constexpr std::uint32_t rawRowAlignment = 16;
inline constexpr std::uint32_t rawTriplanarLumaAlignment = 32;

[[nodiscard]] constexpr std::uint32_t rawPitch(std::uint32_t rowBytes,
                                               std::uint32_t alignment = rawRowAlignment) noexcept
{
    return (rowBytes + alignment - 1) / alignment * alignment;
}

/* Spreads tight rows of `rowBytes` over rows of `pitch` bytes, the padding
 * zero. A picture that is already tight is returned as it is, so an aligned
 * width costs no copy. A picture that is not whole rows, or a pitch shorter
 * than a row, is returned empty: the caller rejects the frame rather than
 * delivering skewed pixels. */
[[nodiscard]] inline std::vector<std::uint8_t> padRows(std::vector<std::uint8_t> tight,
                                                       std::size_t rowBytes, std::size_t pitch)
{
    if (rowBytes == 0 || tight.empty()) return tight;
    if (pitch < rowBytes || tight.size() % rowBytes != 0) return {};
    if (pitch == rowBytes) return tight;
    const auto rows = tight.size() / rowBytes;
    std::vector<std::uint8_t> padded(rows * pitch);
    for (std::size_t row = 0; row < rows; ++row)
        std::memcpy(padded.data() + row * pitch, tight.data() + row * rowBytes, rowBytes);
    return padded;
}

} // namespace uml::video
