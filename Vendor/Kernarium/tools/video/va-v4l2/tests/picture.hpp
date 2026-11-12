// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <vector>

#include "../v4l2_encoder.hpp"
#include "fake_v4l2.hpp"

namespace uml::va::test {

// A picture whose every sample can be told from its neighbours, in buffers
// whose rows are longer than the picture is wide.
struct Picture {
    unsigned width {};
    unsigned height {};
    std::size_t lumaPitch {};
    std::size_t chromaPitch {};
    std::vector<std::uint8_t> luma;
    std::vector<std::uint8_t> chroma;

    Picture(unsigned w, unsigned h, unsigned seed) : width(w), height(h), lumaPitch(w + 32), chromaPitch(w + 40)
    {
        luma.assign(lumaPitch * h, 0xee);
        chroma.assign(chromaPitch * ((h + 1) / 2), 0xdd);
        for (unsigned row = 0; row < h; ++row)
            for (unsigned column = 0; column < w; ++column)
                luma[row * lumaPitch + column] = std::uint8_t(column * 7 + row * 13 + seed);
        for (unsigned row = 0; row < (h + 1) / 2; ++row)
            for (unsigned column = 0; column < w; ++column)
                chroma[row * chromaPitch + column] = std::uint8_t(column * 3 + row * 5 + seed * 11);
    }
    RawFrame frame() const
    {
        return {luma.data(), lumaPitch, chroma.data(), chromaPitch, width, height};
    }
    std::uint64_t checksum() const { return checksum(width, height); }
    // The checksum of the top-left part of the picture, which is what an
    // encoder sees of a surface that carries more rows than the stream.
    std::uint64_t checksum(unsigned visibleWidth, unsigned visibleHeight) const
    {
        return FakeEncoderNode::checksum(luma.data(), lumaPitch, chroma.data(), chromaPitch, visibleWidth,
                                         visibleHeight);
    }
};

}
