// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace uml::platform::bitstream {

// Appends bits, most significant first, and pads the last byte with zeros.
class RbspWriter {
public:
    void bit(bool value)
    {
        if (used_ % 8 == 0) bytes_.push_back(0);
        if (value) bytes_.back() = std::uint8_t(bytes_.back() | (0x80u >> (used_ % 8)));
        ++used_;
    }
    // Copies bits [from, to) of a payload.
    void copy(const std::vector<std::uint8_t>& payload, std::size_t from, std::size_t to)
    {
        for (std::size_t at = from; at < to; ++at) bit((payload[at / 8] >> (7 - at % 8)) & 1);
    }
    [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
    std::size_t used_ {};
};

} // namespace uml::platform::bitstream
