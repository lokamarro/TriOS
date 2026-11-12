// SPDX-License-Identifier: GPL-2.0-or-later
#include "vp9.hpp"

#include <algorithm>
#include <cstring>

namespace uml::va {
namespace {

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> data) noexcept : data_(data) {}
    unsigned read(unsigned bits) noexcept
    {
        unsigned value = 0;
        for (unsigned i = 0; i < bits; ++i) value = (value << 1) | bit();
        return value;
    }
    unsigned bit() noexcept
    {
        if (position_ >= data_.size() * 8) { overrun_ = true; return 0; }
        const auto byte = data_[position_ / 8];
        const auto shift = 7 - position_ % 8;
        ++position_;
        return unsigned(byte >> shift) & 1u;
    }
    bool overrun() const noexcept { return overrun_; }

private:
    std::span<const std::uint8_t> data_;
    std::size_t position_ {};
    bool overrun_ {};
};

constexpr unsigned kFrameMarker = 2;
constexpr std::uint8_t kSyncCode[3] {0x49, 0x83, 0x42};

bool readSyncCode(BitReader& reader) noexcept
{
    for (const auto expected : kSyncCode)
        if (reader.read(8) != expected) return false;
    return true;
}

// Returns the bit depth, or 0 when the colour configuration is one this
// driver does not carry (which only matters for profiles it does not accept).
unsigned readColorConfig(BitReader& reader, unsigned profile) noexcept
{
    unsigned depth = 8;
    if (profile >= 2) depth = reader.read(1) ? 12 : 10;
    const auto colorSpace = reader.read(3);
    if (colorSpace != 7 /* CS_RGB */) {
        reader.read(1); // color range
        if (profile == 1 || profile == 3) {
            reader.read(1); // subsampling_x
            reader.read(1); // subsampling_y
            reader.read(1); // reserved
        }
    } else if (profile == 1 || profile == 3) {
        reader.read(1); // reserved
    }
    return depth;
}

void readFrameSize(BitReader& reader, unsigned& width, unsigned& height) noexcept
{
    width = reader.read(16) + 1;
    height = reader.read(16) + 1;
}

void readRenderSize(BitReader& reader) noexcept
{
    if (reader.read(1)) { reader.read(16); reader.read(16); }
}

}

Vp9FrameHeader Vp9Context::parse(std::span<const std::uint8_t> frame) noexcept
{
    Vp9FrameHeader header;
    if (frame.empty()) return header;
    BitReader reader(frame);
    if (reader.read(2) != kFrameMarker) return header;
    const auto low = reader.read(1);
    const auto high = reader.read(1);
    header.profile = (high << 1) | low;
    if (header.profile == 3 && reader.read(1)) return header;
    header.showExistingFrame = reader.read(1) != 0;
    if (header.showExistingFrame) {
        header.frameToShowMapIndex = reader.read(3);
        header.showFrame = true;
        header.width = width_[header.frameToShowMapIndex];
        header.height = height_[header.frameToShowMapIndex];
        header.valid = !reader.overrun();
        return header;
    }
    header.keyFrame = reader.read(1) == 0;
    header.showFrame = reader.read(1) != 0;
    header.errorResilient = reader.read(1) != 0;
    if (header.keyFrame) {
        if (!readSyncCode(reader)) return header;
        header.bitDepth = readColorConfig(reader, header.profile);
        readFrameSize(reader, header.width, header.height);
        readRenderSize(reader);
        header.refreshFrameFlags = 0xff;
    } else {
        header.intraOnly = header.showFrame ? false : reader.read(1) != 0;
        if (!header.errorResilient) reader.read(2); // reset_frame_context
        if (header.intraOnly) {
            if (!readSyncCode(reader)) return header;
            header.bitDepth = header.profile > 0 ? readColorConfig(reader, header.profile) : 8;
            header.refreshFrameFlags = reader.read(8);
            readFrameSize(reader, header.width, header.height);
            readRenderSize(reader);
        } else {
            header.refreshFrameFlags = reader.read(8);
            unsigned slots[3] {};
            for (auto& slot : slots) { slot = reader.read(3); reader.read(1); }
            bool found = false;
            for (const auto slot : slots) {
                if (reader.read(1)) {
                    header.width = width_[slot];
                    header.height = height_[slot];
                    found = true;
                    break;
                }
            }
            if (!found) readFrameSize(reader, header.width, header.height);
            readRenderSize(reader);
        }
    }
    if (reader.overrun() || !header.width || !header.height) return header;
    header.valid = true;
    for (unsigned slot = 0; slot < 8; ++slot) {
        if (!(header.refreshFrameFlags & (1u << slot))) continue;
        width_[slot] = header.width;
        height_[slot] = header.height;
    }
    return header;
}

std::vector<std::span<const std::uint8_t>> vp9SuperframeParts(std::span<const std::uint8_t> packet) noexcept
{
    std::vector<std::span<const std::uint8_t>> parts;
    if (packet.empty()) return parts;
    const auto marker = packet.back();
    if ((marker & 0xe0) == 0xc0) {
        const std::size_t sizeBytes = ((marker >> 3) & 3u) + 1u;
        const std::size_t count = (marker & 7u) + 1u;
        const std::size_t index = 2 + count * sizeBytes;
        if (packet.size() > index && packet[packet.size() - index] == marker) {
            auto sizes = packet.subspan(packet.size() - index + 1, count * sizeBytes);
            std::size_t offset = 0;
            bool good = true;
            for (std::size_t i = 0; i < count && good; ++i) {
                std::size_t size = 0;
                for (std::size_t b = 0; b < sizeBytes; ++b)
                    size |= std::size_t(sizes[i * sizeBytes + b]) << (8 * b);
                if (!size || offset + size > packet.size() - index) good = false;
                else { parts.push_back(packet.subspan(offset, size)); offset += size; }
            }
            if (good && offset == packet.size() - index) return parts;
            parts.clear();
        }
    }
    parts.push_back(packet);
    return parts;
}

std::uint8_t vp9ShowExistingFrameByte(unsigned slot) noexcept
{
    // frame_marker(2)=2, profile_low_bit=0, profile_high_bit=0,
    // show_existing_frame=1, frame_to_show_map_idx(3).
    return std::uint8_t(0x88u | (slot & 7u));
}

std::vector<std::uint8_t> vp9BuildSuperframe(std::span<const std::span<const std::uint8_t>> frames)
{
    std::vector<std::uint8_t> packet;
    if (frames.empty() || frames.size() > 8) return packet;
    std::size_t total = 0;
    std::size_t largest = 0;
    for (const auto& frame : frames) {
        total += frame.size();
        largest = std::max(largest, frame.size());
    }
    if (frames.size() == 1) {
        packet.assign(frames[0].begin(), frames[0].end());
        return packet;
    }
    unsigned sizeBytes = 1;
    while (sizeBytes < 4 && largest >= (std::size_t(1) << (8 * sizeBytes))) ++sizeBytes;
    packet.reserve(total + 2 + frames.size() * sizeBytes);
    for (const auto& frame : frames) packet.insert(packet.end(), frame.begin(), frame.end());
    const auto marker = std::uint8_t(0xc0u | ((sizeBytes - 1u) << 3) | unsigned(frames.size() - 1));
    packet.push_back(marker);
    for (const auto& frame : frames)
        for (unsigned b = 0; b < sizeBytes; ++b)
            packet.push_back(std::uint8_t((frame.size() >> (8 * b)) & 0xffu));
    packet.push_back(marker);
    return packet;
}

}
