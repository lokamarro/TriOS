// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace uml::va {

// The part of a VP9 uncompressed frame header this driver needs in order to
// turn a sequence of VA-API picture submissions back into the packets a
// stateful decoder expects. Everything else in the header is the hardware
// decoder's business.
struct Vp9FrameHeader {
    unsigned profile {};
    bool showExistingFrame {};
    unsigned frameToShowMapIndex {};
    bool keyFrame {};
    bool showFrame {};
    bool intraOnly {};
    bool errorResilient {};
    unsigned refreshFrameFlags {};
    unsigned width {};
    unsigned height {};
    unsigned bitDepth {8};
    bool valid {};
};

// Frame sizes carried by the eight reference slots. A VP9 inter frame may take
// its size from a reference instead of coding it, so a parser that wants the
// frame size has to follow the slots across frames.
class Vp9Context {
public:
    // Parses one complete VP9 frame and, unless the frame only asks for an
    // already decoded one to be shown, updates the reference slot sizes.
    Vp9FrameHeader parse(std::span<const std::uint8_t> frame) noexcept;

private:
    unsigned width_[8] {};
    unsigned height_[8] {};
};

// Splits a superframe into its frames. A frame that carries no superframe index
// yields itself.
std::vector<std::span<const std::uint8_t>> vp9SuperframeParts(std::span<const std::uint8_t> packet) noexcept;

// The single byte that asks a decoder to show the frame held in `slot` again.
std::uint8_t vp9ShowExistingFrameByte(unsigned slot) noexcept;

// Concatenates `frames` into one superframe with the trailing index VP9
// defines. A single frame is returned unchanged.
std::vector<std::uint8_t> vp9BuildSuperframe(std::span<const std::span<const std::uint8_t>> frames);

}
