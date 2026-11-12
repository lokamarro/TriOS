// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// What the Apple video decoder needs to know about an HEVC stream that
// VideoToolbox does not tell it: the order pictures leave in, which pictures
// are never shown, and the picture a stream means to show inside the coded
// one. It is plain C++ over the bitstream helpers the video encoder backend
// shares, so the checked-in streams can test it without VideoToolbox.

#include "video_encoder_bitstream.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace uml::platform::hevc {

using bitstream::Bytes;

inline constexpr unsigned nalRaslN = 8;
inline constexpr unsigned nalRaslR = 9;
inline constexpr unsigned nalBlaWLp = 16;
inline constexpr unsigned nalIdrWRadl = 19;
inline constexpr unsigned nalIdrNLp = 20;
inline constexpr unsigned nalCra = 21;
inline constexpr unsigned nalVps = 32;
inline constexpr unsigned nalSps = 33;
inline constexpr unsigned nalPps = 34;
inline constexpr unsigned nalEndOfSequence = 36;
inline constexpr unsigned nalEndOfBitstream = 37;

// The two-byte NAL unit header: the type, the layer a unit belongs to (a
// decoder of the base layer ignores every other) and its temporal sub-layer.
[[nodiscard]] inline unsigned nalType(Bytes nal) { return nal.empty() ? 0 : (nal[0] >> 1) & 0x3f; }
[[nodiscard]] inline unsigned layerId(Bytes nal)
{
    return nal.size() < 2 ? 0 : ((nal[0] & 1u) << 5) | (unsigned(nal[1]) >> 3);
}
[[nodiscard]] inline unsigned temporalId(Bytes nal) { return nal.size() < 2 ? 0 : (nal[1] & 7u) - 1; }
// A coded slice segment of a defined type. Reserved types are ignored, as the
// standard asks a decoder to.
[[nodiscard]] inline bool isPicture(unsigned type) { return type <= nalRaslR || (type >= nalBlaWLp && type <= nalCra); }
// A random access point: a picture from which decoding can begin.
[[nodiscard]] inline bool isIrap(unsigned type) { return type >= nalBlaWLp && type <= 23; }

// Luma samples between the coded picture and the picture the stream shows.
struct Window {
    unsigned left {};
    unsigned right {};
    unsigned top {};
    unsigned bottom {};
};

struct Sps {
    unsigned id {};
    unsigned profile {};
    unsigned chromaFormat {};
    bool separateColourPlane {};
    unsigned width {};    // coded size
    unsigned height {};
    Window window;
    unsigned bitDepth {8};
    unsigned log2MaxPocLsb {4};
    // The limits of the highest temporal sub-layer, which is the one decoded.
    unsigned maxNumReorder {};
    unsigned maxLatencyIncreasePlus1 {};
    // Where the conformance window sits in the unit's RBSP, in bits.
    bool hasWindow {};
    std::size_t windowFlagBit {};
    std::size_t windowEndBit {};

    [[nodiscard]] unsigned visibleWidth() const noexcept { return width - window.left - window.right; }
    [[nodiscard]] unsigned visibleHeight() const noexcept { return height - window.top - window.bottom; }
};

struct Pps {
    unsigned id {};
    unsigned spsId {};
    bool outputFlagPresent {};
    unsigned extraSliceHeaderBits {};
};

// nullopt for a unit that is not that parameter set, is truncated, or states
// something the standard does not allow.
[[nodiscard]] std::optional<Sps> parseSps(Bytes nal);
[[nodiscard]] std::optional<Pps> parsePps(Bytes nal);

// The SPS with its conformance window removed: VideoToolbox honors only the
// right and bottom offsets, so the decoder hands it the whole coded picture
// and crops the copy itself. A unit with no window comes back unchanged.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> withoutWindow(Bytes nal);

// The VPS, SPS and PPS of a stream by identifier, which later slices refer to.
class ParameterSets {
public:
    enum class Result { rejected, unchanged, changed };
    // Stores the unit; changed means the stream now says something it did not.
    Result store(Bytes nal);
    [[nodiscard]] const Sps* sps(unsigned id) const;
    [[nodiscard]] const Pps* pps(unsigned id) const;
    // A format description needs at least one of each.
    [[nodiscard]] bool complete() const noexcept { return !vpsUnits_.empty() && !sps_.empty() && !pps_.empty(); }
    // Every stored unit in the order a format description takes them, with the
    // conformance windows removed.
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> descriptionUnits() const;
    void clear();

private:
    std::map<unsigned, std::vector<std::uint8_t>> vpsUnits_;
    std::map<unsigned, std::vector<std::uint8_t>> spsUnits_;
    std::map<unsigned, std::vector<std::uint8_t>> ppsUnits_;
    std::map<unsigned, Sps> sps_;
    std::map<unsigned, Pps> pps_;
};

// The slice segment header up to the picture order count.
struct SliceStart {
    unsigned type {};
    unsigned temporalId {};
    bool firstInPicture {};
    unsigned ppsId {};
    unsigned spsId {};
    bool output {true};   // pic_output_flag
    unsigned pocLsb {};
};

// nullopt for a first slice segment whose parameter sets are unknown or whose
// header is truncated. A later segment of a picture carries nothing the order
// needs, so it answers firstInPicture = false and nothing else.
[[nodiscard]] std::optional<SliceStart> parseSliceStart(Bytes nal, const ParameterSets& sets);

// The picture order count of each picture (clause 8.3.1) and which pictures a
// decoder starting where this one did would never show.
class PictureOrder {
public:
    struct Verdict {
        bool decode {true};          // false: a RASL picture whose references precede the start
        bool output {true};          // PicOutputFlag
        bool startsSequence {};      // an IRAP picture with NoRaslOutputFlag: nothing before it is reordered with it
        int poc {};
    };
    [[nodiscard]] Verdict next(const SliceStart& slice, const Sps& sps);
    // The next picture starts a coded video sequence, as after an end of
    // sequence unit: a CRA picture then drops its leading pictures.
    void endSequence() noexcept { atStart_ = true; }
    // Back to the start of a bitstream, as after a seek.
    void reset() noexcept { *this = PictureOrder(); }

private:
    bool atStart_ {true};
    bool leadingDropped_ {};   // the associated IRAP picture had NoRaslOutputFlag
    unsigned prevLsb_ {};
    int prevMsb_ {};
};

// The output order bumping process of clause C.5.2, over the pictures that are
// waiting to be shown. A picture leaves when more of them wait than the stream
// may reorder, or when it has waited longer than the stream allows.
class ReorderBuffer {
public:
    using Id = std::uint64_t;
    // Adds a picture that is to be shown and appends the pictures that leave.
    void add(Id id, int poc, unsigned maxNumReorder, unsigned maxLatencyIncreasePlus1,
             std::vector<Id>& released);
    // Appends every waiting picture in output order.
    void flush(std::vector<Id>& released);
    [[nodiscard]] std::size_t size() const noexcept { return waiting_.size(); }
    void clear() noexcept { waiting_.clear(); }

private:
    struct Entry {
        Id id;
        int poc;
        unsigned latency;
    };
    void bump(std::vector<Id>& released);
    std::vector<Entry> waiting_;
};

} // namespace uml::platform::hevc
