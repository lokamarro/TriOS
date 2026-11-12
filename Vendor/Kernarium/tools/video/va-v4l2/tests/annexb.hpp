// SPDX-License-Identifier: GPL-2.0-or-later
// What a test can read out of an Annex B stream without a decoder: the NAL
// units, and the picture size, profile and level an H.264 or HEVC sequence
// parameter set declares, cropping included.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace annexb {

struct Bits {
    std::vector<std::uint8_t> data;
    std::size_t position {};
    bool failed {};

    // The NAL unit payload with the emulation prevention bytes removed.
    explicit Bits(const std::uint8_t* bytes, std::size_t size)
    {
        int zeros = 0;
        for (std::size_t index = 0; index < size; ++index) {
            if (zeros >= 2 && bytes[index] == 3) { zeros = 0; continue; }
            zeros = bytes[index] == 0 ? zeros + 1 : 0;
            data.push_back(bytes[index]);
        }
    }
    unsigned bit()
    {
        if (position >= data.size() * 8) { failed = true; return 0; }
        const unsigned value = (data[position / 8] >> (7 - position % 8)) & 1u;
        ++position;
        return value;
    }
    std::uint64_t u(unsigned count)
    {
        std::uint64_t value = 0;
        for (unsigned index = 0; index < count; ++index) value = (value << 1) | bit();
        return value;
    }
    std::uint64_t ue()
    {
        unsigned zeros = 0;
        while (!failed && !bit()) {
            if (++zeros > 32) { failed = true; return 0; }
        }
        return ((std::uint64_t(1) << zeros) - 1) + u(zeros);
    }
    std::int64_t se()
    {
        const std::uint64_t value = ue();
        return (value & 1) ? std::int64_t((value + 1) / 2) : -std::int64_t(value / 2);
    }
};

struct StreamInfo {
    bool found {};
    unsigned profile {};
    unsigned level {};
    unsigned codedWidth {};
    unsigned codedHeight {};
    unsigned width {};
    unsigned height {};
};

inline void skipScalingList(Bits& bits, unsigned size)
{
    std::int64_t last = 8, next = 8;
    for (unsigned index = 0; index < size && !bits.failed; ++index) {
        if (next != 0) next = (last + bits.se() + 256) % 256;
        last = next == 0 ? last : next;
    }
}

inline StreamInfo parseH264Sps(const std::uint8_t* payload, std::size_t size)
{
    StreamInfo info;
    Bits bits(payload, size);
    info.profile = unsigned(bits.u(8));
    bits.u(8);
    info.level = unsigned(bits.u(8));
    bits.ue();
    unsigned chroma = 1;
    switch (info.profile) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118: case 128: case 138: case 139:
    case 134: case 135:
        chroma = unsigned(bits.ue());
        if (chroma == 3) bits.u(1);
        bits.ue();
        bits.ue();
        bits.u(1);
        if (bits.u(1)) {
            const unsigned lists = chroma != 3 ? 8 : 12;
            for (unsigned index = 0; index < lists && !bits.failed; ++index)
                if (bits.u(1)) skipScalingList(bits, index < 6 ? 16 : 64);
        }
        break;
    default:
        break;
    }
    bits.ue();
    const std::uint64_t pocType = bits.ue();
    if (pocType == 0) {
        bits.ue();
    } else if (pocType == 1) {
        bits.u(1);
        bits.se();
        bits.se();
        const std::uint64_t cycle = bits.ue();
        for (std::uint64_t index = 0; index < cycle && !bits.failed; ++index) bits.se();
    }
    bits.ue();
    bits.u(1);
    const unsigned mbsWide = unsigned(bits.ue()) + 1;
    const unsigned mbsHigh = unsigned(bits.ue()) + 1;
    const unsigned frameOnly = unsigned(bits.u(1));
    if (!frameOnly) bits.u(1);
    bits.u(1);
    unsigned crop[4] {};
    if (bits.u(1))
        for (auto& value : crop) value = unsigned(bits.ue());
    if (bits.failed) return info;
    const unsigned unitX = chroma == 0 || chroma == 3 ? 1 : 2;
    const unsigned unitY = (chroma == 1 ? 2 : 1) * (2 - frameOnly);
    info.codedWidth = mbsWide * 16;
    info.codedHeight = mbsHigh * 16 * (2 - frameOnly);
    info.width = info.codedWidth - (crop[0] + crop[1]) * unitX;
    info.height = info.codedHeight - (crop[2] + crop[3]) * unitY;
    info.found = true;
    return info;
}

inline StreamInfo parseHevcSps(const std::uint8_t* payload, std::size_t size)
{
    StreamInfo info;
    Bits bits(payload, size);
    bits.u(4);
    const unsigned subLayers = unsigned(bits.u(3));
    bits.u(1);
    // profile_tier_level: the general part, then the sub-layer flags.
    bits.u(3);
    info.profile = unsigned(bits.u(5));
    bits.u(32);
    bits.u(48);
    info.level = unsigned(bits.u(8));
    bool profilePresent[8] {}, levelPresent[8] {};
    for (unsigned index = 0; index < subLayers; ++index) {
        profilePresent[index] = bits.u(1);
        levelPresent[index] = bits.u(1);
    }
    if (subLayers > 0)
        for (unsigned index = subLayers; index < 8; ++index) bits.u(2);
    for (unsigned index = 0; index < subLayers; ++index) {
        if (profilePresent[index]) { bits.u(32); bits.u(32); bits.u(24); }
        if (levelPresent[index]) bits.u(8);
    }
    bits.ue();
    const unsigned chroma = unsigned(bits.ue());
    if (chroma == 3) bits.u(1);
    info.codedWidth = unsigned(bits.ue());
    info.codedHeight = unsigned(bits.ue());
    unsigned crop[4] {};
    if (bits.u(1))
        for (auto& value : crop) value = unsigned(bits.ue());
    if (bits.failed) return info;
    const unsigned unitX = chroma == 0 || chroma == 3 ? 1 : 2;
    const unsigned unitY = chroma == 1 ? 2 : 1;
    info.width = info.codedWidth - (crop[0] + crop[1]) * unitX;
    info.height = info.codedHeight - (crop[2] + crop[3]) * unitY;
    info.found = true;
    return info;
}

struct Nal {
    unsigned type {};
    const std::uint8_t* payload {};
    std::size_t size {};
};

inline std::vector<Nal> splitNals(const std::vector<std::uint8_t>& data, bool hevc)
{
    std::vector<Nal> out;
    const std::size_t header = hevc ? 2 : 1;
    std::size_t index = 0;
    std::vector<std::size_t> starts;
    while (index + 3 <= data.size()) {
        if (data[index] == 0 && data[index + 1] == 0 && data[index + 2] == 1) {
            starts.push_back(index + 3);
            index += 3;
        } else {
            ++index;
        }
    }
    for (std::size_t entry = 0; entry < starts.size(); ++entry) {
        std::size_t end = entry + 1 < starts.size() ? starts[entry + 1] - 3 : data.size();
        while (end > starts[entry] && data[end - 1] == 0) --end;
        if (end < starts[entry] + header) continue;
        Nal nal;
        nal.type = hevc ? (data[starts[entry]] >> 1) & 0x3f : data[starts[entry]] & 0x1f;
        nal.payload = data.data() + starts[entry] + header;
        nal.size = end - starts[entry] - header;
        out.push_back(nal);
    }
    return out;
}

}
