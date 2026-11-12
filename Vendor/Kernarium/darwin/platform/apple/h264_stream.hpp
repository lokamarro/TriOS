// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// What the Apple video decoder needs to know about an H.264 stream that
// VideoToolbox does not tell it: the order pictures are shown in, and the
// picture a stream means to show inside the coded one. It is plain C++ over the
// bitstream helpers the video encoder backend shares, so the checked-in streams
// can test it without VideoToolbox.

#include "video_encoder_bitstream.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace uml::platform::h264 {

using bitstream::Bytes;

inline constexpr unsigned nalSlice = 1;
inline constexpr unsigned nalIdr = 5;
inline constexpr unsigned nalSei = 6;
inline constexpr unsigned nalSps = 7;
inline constexpr unsigned nalPps = 8;
inline constexpr unsigned nalAud = 9;
inline constexpr unsigned nalEndOfSequence = 10;
inline constexpr unsigned nalEndOfStream = 11;

[[nodiscard]] inline unsigned nalType(Bytes nal) { return nal.empty() ? 0 : nal[0] & 0x1f; }
[[nodiscard]] inline unsigned nalRefIdc(Bytes nal) { return nal.empty() ? 0 : (nal[0] >> 5) & 3; }
// A coded slice of the base view. Slice data partitions, the extensions and
// everything else a hardware decoder of the base view ignores are not pictures.
[[nodiscard]] inline bool isPicture(unsigned type) { return type == nalSlice || type == nalIdr; }

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
    unsigned constraintFlags {};   // the byte after profile_idc
    unsigned level {};
    unsigned chromaFormat {1};
    bool separateColourPlane {};
    unsigned bitDepth {8};
    unsigned log2MaxFrameNum {4};
    unsigned pocType {};
    unsigned log2MaxPocLsb {4};
    bool deltaPicOrderAlwaysZero {};
    int offsetForNonRefPic {};
    int offsetForTopToBottomField {};
    std::vector<int> offsetForRefFrame;
    unsigned maxNumRefFrames {};
    bool gapsAllowed {};
    unsigned widthMacroblocks {};
    unsigned heightMacroblocks {};   // of a frame
    bool frameMbsOnly {true};
    Window window;
    // Where the cropping lives in the unit's RBSP, in bits.
    bool hasWindow {};
    std::size_t windowFlagBit {};
    std::size_t windowEndBit {};
    // The VUI's limits on the output order, when it states them.
    bool bitstreamRestriction {};
    unsigned maxNumReorderFrames {};
    unsigned maxDecFrameBuffering {};

    [[nodiscard]] unsigned width() const noexcept { return widthMacroblocks * 16; }    // coded size
    [[nodiscard]] unsigned height() const noexcept { return heightMacroblocks * 16; }
    [[nodiscard]] unsigned visibleWidth() const noexcept { return width() - window.left - window.right; }
    [[nodiscard]] unsigned visibleHeight() const noexcept { return height() - window.top - window.bottom; }
    // The number of frames the decoded picture buffer holds: what the stream
    // states, or what its level allows for a picture of its size.
    [[nodiscard]] unsigned dpbFrames() const noexcept;
    // How many frames may follow a frame in decoding order and precede it in
    // output order: what the stream states, or what its profile and level allow.
    [[nodiscard]] unsigned reorderFrames() const noexcept;
};

struct Pps {
    unsigned id {};
    unsigned spsId {};
    bool bottomFieldPicOrderInFramePresent {};
    unsigned numRefIdxDefault[2] {1, 1};
    bool weightedPred {};
    unsigned weightedBipredIdc {};
    bool redundantPicCntPresent {};
};

// nullopt for a unit that is not that parameter set, is truncated, or states
// something the standard does not allow (or, for a PPS, slice groups, which no
// hardware decoder of the profiles listed takes).
[[nodiscard]] std::optional<Sps> parseSps(Bytes nal);
[[nodiscard]] std::optional<Pps> parsePps(Bytes nal);

// The SPS with its frame cropping removed: VideoToolbox is not relied on to
// honor the left and top offsets, so the decoder hands it the whole coded
// picture and crops the copy itself. A unit with no cropping comes back unchanged.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> withoutCropping(Bytes nal);

// The SPS and PPS of a stream by identifier, which later slices refer to.
class ParameterSets {
public:
    enum class Result { rejected, unchanged, changed };
    // Stores the unit; changed means the stream now says something it did not.
    Result store(Bytes nal);
    [[nodiscard]] const Sps* sps(unsigned id) const;
    [[nodiscard]] const Pps* pps(unsigned id) const;
    // A format description needs at least one of each.
    [[nodiscard]] bool complete() const noexcept { return !spsUnits_.empty() && !ppsUnits_.empty(); }
    // Every stored unit in the order a format description takes them, with the
    // cropping removed.
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> descriptionUnits() const;
    void clear();

private:
    std::map<unsigned, std::vector<std::uint8_t>> spsUnits_;
    std::map<unsigned, std::vector<std::uint8_t>> ppsUnits_;
    std::map<unsigned, Sps> sps_;
    std::map<unsigned, Pps> pps_;
};

// One memory management control operation of a picture's reference marking.
struct Mmco {
    unsigned operation {};
    unsigned first {};    // difference_of_pic_nums_minus1, long_term_pic_num, long_term_frame_idx or max_long_term_frame_idx_plus1
    unsigned second {};   // long_term_frame_idx of operation 3
};

// A slice header, up to and including the marking of reference pictures.
struct SliceHeader {
    unsigned type {};         // nal_unit_type
    unsigned refIdc {};       // nal_ref_idc
    unsigned spsId {};
    unsigned ppsId {};
    unsigned firstMb {};
    unsigned sliceType {};    // 0 P, 1 B, 2 I, 3 SP, 4 SI
    unsigned frameNum {};
    bool fieldPic {};
    bool bottomField {};
    unsigned idrPicId {};
    unsigned pocLsb {};
    int deltaPocBottom {};
    int deltaPoc[2] {};
    unsigned redundantPicCnt {};
    bool noOutputOfPriorPics {};
    bool longTermReference {};
    bool adaptiveMarking {};
    std::vector<Mmco> mmco;

    [[nodiscard]] bool idr() const noexcept { return type == nalIdr; }
    [[nodiscard]] bool hasMmco5() const noexcept;
};

// nullopt for a slice whose parameter sets are unknown or whose header is
// truncated.
[[nodiscard]] std::optional<SliceHeader> parseSliceHeader(Bytes nal, const ParameterSets& sets);

// Whether `slice` is the first of another picture than the one `previous`
// belongs to (clause 7.4.1.2.4).
[[nodiscard]] bool startsPicture(const SliceHeader& previous, const SliceHeader& slice);

// The picture order count of each frame (clause 8.2.1) and the order frames
// leave the decoded picture buffer in (clause C.4): a frame waits until more
// frames wait than the stream may reorder, or until the buffer has no room for
// the next one, and then the waiting frame with the lowest count leaves. An
// IDR picture and a picture with memory management operation 5 send everything
// before them out first.
//
// A picture coded as a field is not modelled: it leaves at once, after
// everything that waits, in the order it was decoded.
class OutputOrder {
public:
    using Id = std::uint64_t;
    // Adds the next decoded frame, in decoding order, and appends the frames
    // that leave. `slice` is the picture's first slice.
    void add(Id id, const SliceHeader& slice, const Sps& sps, std::vector<Id>& released);
    // Appends every waiting frame in output order. What the pictures refer to
    // is kept, so decoding goes on from where it was.
    void flush(std::vector<Id>& released);
    // Back to the start of a bitstream, as after a seek.
    void reset();
    [[nodiscard]] std::size_t waiting() const noexcept;
    // The picture order count the last frame was given.
    [[nodiscard]] int lastPoc() const noexcept { return lastPoc_; }

private:
    struct Frame {
        Id id {};
        int poc {};
        bool output {};          // still to be shown
        bool shortTerm {};
        bool longTerm {};
        unsigned frameNum {};
        unsigned longTermIdx {};
        [[nodiscard]] bool reference() const noexcept { return shortTerm || longTerm; }
    };
    int pictureOrderCount(const SliceHeader& slice, const Sps& sps, bool mmco5, int& top);
    void fillFrameNumGap(const SliceHeader& slice, const Sps& sps);
    void slidingWindow(const Sps& sps, unsigned frameNum);
    void mark(const SliceHeader& slice, const Sps& sps, Frame& current);
    void showLowest(std::vector<Id>& released, Frame* current);
    void forget();

    std::vector<Frame> dpb_;
    // Picture order count state (8.2.1).
    int prevPocMsb_ {};
    int prevPocLsb_ {};
    int prevFrameNumOffset_ {};
    unsigned prevFrameNum_ {};
    unsigned prevRefFrameNum_ {};
    int maxLongTermIdx_ {-1};
    int lastPoc_ {};
};

} // namespace uml::platform::h264
