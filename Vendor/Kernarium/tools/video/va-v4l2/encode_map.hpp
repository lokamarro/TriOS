// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <va/va.h>
#include <va/va_enc_h264.h>
#include <va/va_enc_hevc.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "v4l2_encoder.hpp"

namespace uml::va {

// A VA-API encode profile the encoder node can serve, and the V4L2 profile
// menu items that stand for it, best first.
struct EncodeProfile {
    VAProfile profile {VAProfileNone};
    std::uint32_t codec {};
    std::vector<unsigned> menu;
};

// What the driver advertises for encoding: derived from what the node
// publishes, never from a fixed table, so a profile or rate-control mode the
// host did not verify is not offered.
struct EncodeSupport {
    std::shared_ptr<const EncoderCapabilities> capabilities;
    std::vector<EncodeProfile> profiles;
    // The union of VA_RC_* modes the node can serve.
    unsigned rateControl {};

    const EncodeProfile* profile(VAProfile profile) const noexcept;
    // The frame sizes the node accepts for a profile's codec.
    const EncodeCodec* limits(VAProfile profile) const noexcept;
    bool any() const noexcept { return !profiles.empty() && rateControl; }
};

EncodeSupport deriveEncodeSupport(std::shared_ptr<const EncoderCapabilities> capabilities);

// What the buffers of a VA-API encode picture say about the stream, reduced to
// the parts the V4L2 encoder can act on. A buffer a client sends only on some
// pictures leaves the earlier values in place.
struct SequenceInfo {
    bool valid {};
    // The picture size, and the part of it that is visible once the cropping
    // the sequence parameters ask for is applied.
    unsigned codedWidth {};
    unsigned codedHeight {};
    unsigned width {};
    unsigned height {};
    unsigned intraIdrPeriod {};
    unsigned ipPeriod {1};
    // level_idc as the bitstream carries it: ten times the level for H.264, thirty
    // times for HEVC.
    unsigned level {};
    // The HEVC high tier, which sets a higher bit rate ceiling for the level.
    bool highTier {};
    std::uint32_t bitsPerSecond {};
    // Frames per second as the VUI timing gives it, when it does.
    unsigned timingNumerator {};
    unsigned timingDenominator {};
};

struct PictureInfo {
    bool valid {};
    bool idr {};
    bool cabac {};
    bool transform8x8 {};
    int initQp {26};
    VABufferID codedBuffer {VA_INVALID_ID};
};

struct SliceInfo {
    bool valid {};
    int qpDelta {};
};

struct RateInfo {
    bool valid {};
    std::uint32_t bitsPerSecond {};
    unsigned targetPercentage {100};
    unsigned windowMs {};
    unsigned minQp {};
    unsigned maxQp {};
    unsigned qualityFactor {};
};

struct FrameRateInfo {
    bool valid {};
    unsigned numerator {};
    unsigned denominator {1};
};

struct HrdInfo {
    bool valid {};
    std::uint32_t bufferBits {};
};

struct EncodeState {
    SequenceInfo sequence;
    PictureInfo picture;
    SliceInfo slice;
    RateInfo rate;
    FrameRateInfo frameRate;
    HrdInfo hrd;
};

// Reads one VA-API buffer into the state. False when the buffer is too short
// to be what its type says.
bool readH264Sequence(const void* data, std::size_t size, SequenceInfo& sequence);
bool readH264Picture(const void* data, std::size_t size, PictureInfo& picture);
bool readH264Slice(const void* data, std::size_t size, SliceInfo& slice);
// HEVC has no cropping in its sequence parameters, so the stream is the coded
// size. Only 8-bit 4:2:0 reads: that is all Main is.
bool readHevcSequence(const void* data, std::size_t size, SequenceInfo& sequence);
bool readHevcPicture(const void* data, std::size_t size, PictureInfo& picture);
bool readHevcSlice(const void* data, std::size_t size, SliceInfo& slice);
// A VAEncMiscParameterBuffer. Types the driver does not act on leave the state
// alone and still count as read.
bool readMisc(const void* data, std::size_t size, EncodeState& state);

// The frame rate the encoder should run at: a rate control buffer's, else the
// VUI timing's, else 30.
FrameRateInfo effectiveFrameRate(const EncodeState& state) noexcept;

// The rates a mode asks the node for. In constant bit rate both are the bit
// rate. In variable bit rate the client gives the peak and the percentage of
// it to aim for.
struct Rates {
    std::uint32_t average {};
    std::uint32_t peak {};
};
Rates ratesFor(unsigned rateControl, const EncodeState& state) noexcept;

// The V4L2 level menu item for a codec's level_idc: the level itself when the
// node offers it, otherwise the next one up, because a stream at a lower level
// than it needs would not decode. Returns false when the node offers no level.
bool encodeLevelItem(const EncoderCapabilities& capabilities, std::uint32_t codec, unsigned levelIdc,
                     unsigned& item) noexcept;

// Controls set before the stream starts, in the order they are applied.
std::vector<ControlValue> startupControls(const EncodeSupport& support, VAProfile profile, unsigned rateControl,
                                          const EncodeState& state);
// Controls that change from one picture to the next without stopping the
// stream: the rates, the quality, the constant quantiser and the upper
// quantiser limit. The frame rate is not a control and is handled apart.
std::vector<ControlValue> dynamicControls(const EncodeSupport& support, VAProfile profile, unsigned rateControl,
                                          const EncodeState& before, const EncodeState& after);

}
