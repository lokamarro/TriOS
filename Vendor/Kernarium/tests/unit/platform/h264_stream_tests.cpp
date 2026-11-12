// SPDX-License-Identifier: GPL-2.0-or-later
#include "apple/h264_stream.hpp"
#include "require.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

/* The H.264 header readers and output order of the Apple video decoder, on the
 * checked-in streams and on headers built here. Nothing in this binary touches
 * VideoToolbox. */

namespace {
using namespace uml::platform::h264;
using uml::testing::require;
using Unit = std::vector<std::uint8_t>;

std::string fixtureDirectory;

Unit read(const std::string& name)
{
    std::ifstream file(fixtureDirectory + "/" + name, std::ios::binary);
    if (!file) throw std::runtime_error("missing video fixture: " + fixtureDirectory + "/" + name);
    return {std::istreambuf_iterator<char>(file), {}};
}

/* One coded picture of a stream, in decode order: the header its first slice
 * carries. */
struct Stream {
    ParameterSets sets;
    std::vector<SliceHeader> pictures;
};

Stream open(const std::string& name)
{
    Stream stream;
    const auto bytes = read(name);
    for (const auto unit : uml::platform::bitstream::splitAnnexB(bytes)) {
        const auto type = nalType(unit);
        if (type == nalSps || type == nalPps) {
            require(stream.sets.store(unit) != ParameterSets::Result::rejected, "a parameter set was rejected");
        } else if (isPicture(type)) {
            const auto slice = parseSliceHeader(unit, stream.sets);
            require(slice.has_value(), "a slice header could not be read");
            if (stream.pictures.empty() || startsPicture(stream.pictures.back(), *slice))
                stream.pictures.push_back(*slice);
        }
    }
    return stream;
}

/* What the decoder does with the pictures of a stream: the order they are
 * shown in, as indices of the pictures in decoding order, and how many had left
 * before the stream was drained. */
struct Shown {
    std::vector<unsigned> order;
    std::size_t beforeDrain {};
};

Shown shown(const Stream& stream, unsigned first = 0)
{
    OutputOrder order;
    std::vector<OutputOrder::Id> released;
    for (unsigned i = first; i < stream.pictures.size(); ++i) {
        const auto& picture = stream.pictures[i];
        const auto* sps = stream.sets.sps(picture.spsId);
        require(sps != nullptr, "a picture's SPS is unknown");
        order.add(i + 1, picture, *sps, released);
    }
    Shown result;
    result.beforeDrain = released.size();
    order.flush(released);
    for (const auto id : released) result.order.push_back(unsigned(id - 1));
    return result;
}

/* The display position of each picture of the order fixtures, which the
 * encoder's source frame number states: the Nth picture shown is source frame N
 * and its luma is 16 + 10 * N. The pictures are in the order the encoder coded
 * them, which a software decoder's output confirms, and the groups use
 * reference B-pictures that explicit operations unmark. */
const unsigned closedDisplay[] = {0, 4, 2, 1, 3, 8, 6, 5, 7, 11, 9, 10, 12, 16, 14, 13, 15, 19, 17, 18};

std::vector<unsigned> expected()
{
    std::vector<unsigned> order;
    for (unsigned i = 0; i < 20; ++i) order.push_back(i);
    std::sort(order.begin(), order.end(), [](unsigned a, unsigned b) { return closedDisplay[a] < closedDisplay[b]; });
    return order;
}

void print(const char* label, const std::vector<unsigned>& order)
{
    std::printf("%s:", label);
    for (const auto index : order) std::printf(" %u", index);
    std::printf("\n");
}

void parameterSets()
{
    const auto stream = open("h264-bframes.bin");
    require(stream.sets.complete(), "the fixture has an SPS and a PPS");
    const auto* sps = stream.sets.sps(0);
    const auto* pps = stream.sets.pps(0);
    require(sps != nullptr && pps != nullptr && pps->spsId == 0, "parameter sets by identifier");
    std::printf("h264-bframes: %ux%u, profile %u level %u, POC type %u, frame_num bits %u, refs %u, reorder %u, dpb %u\n",
                sps->width(), sps->height(), sps->profile, sps->level, sps->pocType, sps->log2MaxFrameNum,
                sps->maxNumRefFrames, sps->maxNumReorderFrames, sps->maxDecFrameBuffering);
    require(sps->width() == 64 && sps->height() == 64 && sps->bitDepth == 8 && !sps->hasWindow, "SPS picture format");
    require(sps->profile == 100 && sps->pocType == 0 && sps->frameMbsOnly, "SPS profile and order count");
    require(sps->bitstreamRestriction && sps->maxNumReorderFrames == 2 && sps->maxDecFrameBuffering == 4,
            "the VUI limits the order");
    require(sps->reorderFrames() == 2 && sps->dpbFrames() == 4, "the stream's own limits are used");

    // The same stream with its limits taken out falls back to what the level allows.
    const auto bare = open("h264-no-restriction.bin");
    const auto* bareSps = bare.sets.sps(0);
    require(bareSps != nullptr && !bareSps->bitstreamRestriction, "the limits are gone");
    require(bareSps->level == sps->level && bareSps->maxNumRefFrames == sps->maxNumRefFrames,
            "nothing but the limits changed");
    require(bareSps->dpbFrames() == 16 && bareSps->reorderFrames() == 16, "a 64 x 64 picture at level 1 fills sixteen frames");

    const auto still = open("h264-poc2.bin");
    require(still.sets.sps(0)->pocType == 2 && still.sets.sps(0)->reorderFrames() == 0,
            "order count type 2 never reorders");
}

void croppingWindow()
{
    const auto stream = open("h264-window.bin");
    const auto* sps = stream.sets.sps(0);
    require(sps != nullptr && sps->hasWindow, "the fixture states a frame cropping window");
    require(sps->width() == 128 && sps->height() == 96, "coded picture size");
    require(sps->window.left == 6 && sps->window.right == 2 && sps->window.top == 4 && sps->window.bottom == 2,
            "offsets in luma samples");
    require(sps->visibleWidth() == 120 && sps->visibleHeight() == 90, "visible picture size");
    // The unit as a format description takes it: whole coded picture, everything else as it was.
    const auto bytes = read("h264-window.bin");
    for (const auto unit : uml::platform::bitstream::splitAnnexB(bytes)) {
        if (nalType(unit) != nalSps) continue;
        const auto stripped = withoutCropping(unit);
        require(stripped.has_value(), "the window could be removed");
        const auto parsed = parseSps(*stripped);
        require(parsed.has_value() && !parsed->hasWindow, "the SPS states no window any more");
        require(parsed->width() == 128 && parsed->height() == 96 && parsed->profile == sps->profile &&
                parsed->level == sps->level && parsed->log2MaxFrameNum == sps->log2MaxFrameNum &&
                parsed->pocType == sps->pocType && parsed->maxNumRefFrames == sps->maxNumRefFrames &&
                parsed->bitstreamRestriction == sps->bitstreamRestriction &&
                parsed->maxNumReorderFrames == sps->maxNumReorderFrames,
                "the rest of the SPS is untouched");
        // An SPS with no window comes back as it was.
        const auto again = withoutCropping(*stripped);
        require(again.has_value() && *again == *stripped, "removing a window that is gone changes nothing");
    }
    for (const auto& unit : stream.sets.descriptionUnits()) {
        if (nalType(unit) != nalSps) continue;
        const auto parsed = parseSps(unit);
        require(parsed.has_value() && !parsed->hasWindow, "a format description is built without the window");
    }
}

void fixtureOrder()
{
    const auto stream = open("h264-bframes.bin");
    require(stream.pictures.size() == 20, "twenty pictures");
    require(stream.pictures.front().idr() && stream.pictures[12].idr(), "two closed groups");
    unsigned marking = 0;
    for (const auto& picture : stream.pictures) marking += !picture.mmco.empty();
    require(marking > 0, "the fixture unmarks pictures with explicit operations");
    const auto result = shown(stream);
    print("h264 B-pictures, VUI limits", result.order);
    require(result.order == expected(), "pictures leave in display order");
    // Two frames wait for the ones that precede them, as the VUI says.
    require(result.beforeDrain == 18, "the last two pictures wait until the stream is drained");

    // Without limits the level default holds more back, and the order is the same.
    const auto bare = open("h264-no-restriction.bin");
    const auto level = shown(bare);
    print("h264 B-pictures, level default", level.order);
    require(level.order == expected(), "the level default shows the same order");
    require(level.beforeDrain >= 12 && level.beforeDrain < 20,
            "a new group sends the one before it out, and the last frames wait");

    // Picture order count type 2 is decoding order, and nothing waits.
    const auto still = open("h264-poc2.bin");
    const auto order = shown(still);
    require(order.order.size() == still.pictures.size() && order.beforeDrain == still.pictures.size(),
            "type 2 shows every picture as it is decoded");
    for (unsigned i = 0; i < order.order.size(); ++i) require(order.order[i] == i, "type 2 is decoding order");

    // A decoder that starts at the second group.
    const auto second = shown(stream, 12);
    require(second.order.size() == 8, "eight pictures");
    std::vector<unsigned> display;
    for (const auto index : second.order) display.push_back(closedDisplay[index]);
    require(std::is_sorted(display.begin(), display.end()), "the second group alone is in order");
}

/* Bit-exact builders for headers a real encoder does not write. */
class Bits {
public:
    void u(unsigned count, std::uint32_t value)
    {
        for (unsigned i = count; i-- > 0;) bits_.push_back((value >> i) & 1);
    }
    void flag(bool value) { u(1, value); }
    void ue(std::uint32_t value)
    {
        const std::uint32_t code = value + 1;
        unsigned length = 0;
        while ((code >> length) > 1) ++length;
        u(length, 0);
        u(length + 1, code);
    }
    void se(int value) { ue(value > 0 ? std::uint32_t(2 * value - 1) : std::uint32_t(-2 * value)); }
    // The unit with its header byte, the stop bit and emulation prevention.
    Unit nal(unsigned header) const
    {
        auto bits = bits_;
        bits.push_back(true);
        while (bits.size() % 8) bits.push_back(false);
        Unit rbsp;
        for (std::size_t i = 0; i < bits.size(); i += 8) {
            unsigned byte = 0;
            for (unsigned j = 0; j < 8; ++j) byte = (byte << 1) | bits[i + j];
            rbsp.push_back(std::uint8_t(byte));
        }
        auto out = Unit {std::uint8_t(header)};
        const auto escaped = uml::platform::bitstream::escapeEmulation(rbsp);
        out.insert(out.end(), escaped.begin(), escaped.end());
        return out;
    }

private:
    std::vector<bool> bits_;
};

struct SpsOptions {
    unsigned profile {77};
    unsigned constraintFlags {};
    unsigned level {30};
    unsigned pocType {0};
    unsigned log2MaxFrameNum {4};
    unsigned log2MaxPocLsb {6};
    unsigned maxNumRefFrames {4};
    bool gapsAllowed {};
    unsigned widthMacroblocks {20};
    unsigned heightMacroblocks {15};
    bool frameMbsOnly {true};
    bool crop {};
    // VUI limits on the order, when reorder >= 0.
    int reorder {-1};
    unsigned maxDecFrameBuffering {4};
    // Order count type 1.
    bool deltaAlwaysZero {};
    int offsetForNonRef {};
    int offsetTopToBottom {};
    std::vector<int> offsetsForRefFrames;
};

Unit makeSps(const SpsOptions& o)
{
    Bits b;
    b.u(8, o.profile);
    b.u(8, o.constraintFlags);
    b.u(8, o.level);
    b.ue(0);   // seq_parameter_set_id
    if (o.profile >= 100) {
        b.ue(1);        // chroma_format_idc
        b.ue(0);        // bit_depth_luma_minus8
        b.ue(0);        // bit_depth_chroma_minus8
        b.flag(false);  // qpprime_y_zero_transform_bypass_flag
        b.flag(false);  // seq_scaling_matrix_present_flag
    }
    b.ue(o.log2MaxFrameNum - 4);
    b.ue(o.pocType);
    if (o.pocType == 0) {
        b.ue(o.log2MaxPocLsb - 4);
    } else if (o.pocType == 1) {
        b.flag(o.deltaAlwaysZero);
        b.se(o.offsetForNonRef);
        b.se(o.offsetTopToBottom);
        b.ue(unsigned(o.offsetsForRefFrames.size()));
        for (const int offset : o.offsetsForRefFrames) b.se(offset);
    }
    b.ue(o.maxNumRefFrames);
    b.flag(o.gapsAllowed);
    b.ue(o.widthMacroblocks - 1);
    b.ue((o.frameMbsOnly ? o.heightMacroblocks : o.heightMacroblocks / 2) - 1);
    b.flag(o.frameMbsOnly);
    if (!o.frameMbsOnly) b.flag(false);
    b.flag(true);   // direct_8x8_inference_flag
    b.flag(o.crop);
    if (o.crop) {
        b.ue(1); b.ue(2); b.ue(3); b.ue(4);
    }
    const bool vui = o.reorder >= 0;
    b.flag(vui);
    if (vui) {
        b.flag(false); b.flag(false);   // aspect ratio, overscan
        b.flag(false); b.flag(false);   // video signal type, chroma location
        b.flag(false);                  // timing info
        b.flag(false); b.flag(false);   // NAL and VCL HRD parameters
        b.flag(false);                  // pic_struct_present_flag
        b.flag(true);                   // bitstream_restriction_flag
        b.flag(true);                   // motion_vectors_over_pic_boundaries_flag
        b.ue(0); b.ue(0); b.ue(16); b.ue(16);
        b.ue(unsigned(o.reorder));
        b.ue(o.maxDecFrameBuffering);
    }
    return b.nal(0x67);
}

Unit makePps(bool bottomFieldPicOrder = false)
{
    Bits b;
    b.ue(0);   // pic_parameter_set_id
    b.ue(0);   // seq_parameter_set_id
    b.flag(false);   // entropy_coding_mode_flag
    b.flag(bottomFieldPicOrder);
    b.ue(0);   // num_slice_groups_minus1
    b.ue(0); b.ue(0);   // reference index defaults
    b.flag(false); b.u(2, 0);   // weighted prediction
    b.se(0); b.se(0); b.se(0);
    b.flag(true);    // deblocking_filter_control_present_flag
    b.flag(false);   // constrained_intra_pred_flag
    b.flag(false);   // redundant_pic_cnt_present_flag
    return b.nal(0x68);
}

struct SliceOptions {
    bool idr {};
    unsigned refIdc {2};
    unsigned sliceType {0};   // P; 2 is I
    unsigned frameNum {};
    unsigned pocLsb {};
    int deltaPocBottom {};
    int deltaPoc0 {};
    int deltaPoc1 {};
    bool field {};
    bool bottom {};
    std::vector<Mmco> mmco;
    bool adaptive {};
};

Unit makeSlice(const SpsOptions& sps, const SliceOptions& o, bool bottomFieldPicOrder = false)
{
    Bits b;
    b.ue(0);   // first_mb_in_slice
    b.ue(o.sliceType);
    b.ue(0);   // pic_parameter_set_id
    b.u(sps.log2MaxFrameNum, o.frameNum);
    if (!sps.frameMbsOnly) {
        b.flag(o.field);
        if (o.field) b.flag(o.bottom);
    }
    if (o.idr) b.ue(1);   // idr_pic_id
    if (sps.pocType == 0) {
        b.u(sps.log2MaxPocLsb, o.pocLsb);
        if (bottomFieldPicOrder && !o.field) b.se(o.deltaPocBottom);
    } else if (sps.pocType == 1 && !sps.deltaAlwaysZero) {
        b.se(o.deltaPoc0);
        if (bottomFieldPicOrder && !o.field) b.se(o.deltaPoc1);
    }
    if (o.sliceType == 0) {
        b.flag(false);   // num_ref_idx_active_override_flag
        b.flag(false);   // ref_pic_list_modification_flag_l0
    }
    if (o.refIdc) {
        if (o.idr) {
            b.flag(false);   // no_output_of_prior_pics_flag
            b.flag(false);   // long_term_reference_flag
        } else {
            b.flag(o.adaptive);
            if (o.adaptive) {
                for (const auto& op : o.mmco) {
                    b.ue(op.operation);
                    if (op.operation == 1 || op.operation == 3) b.ue(op.first);
                    if (op.operation == 2) b.ue(op.first);
                    if (op.operation == 3) b.ue(op.second);
                    if (op.operation == 4 || op.operation == 6) b.ue(op.first);
                }
                b.ue(0);
            }
        }
    }
    b.se(0);   // slice_qp_delta
    return b.nal((o.refIdc << 5) | (o.idr ? nalIdr : nalSlice));
}

/* Feeds the slices to a fresh buffer one picture each. Each step is the
 * picture order count the picture got and the pictures that left when it arrived,
 * numbered from one in decoding order; `tail` is what the end of the stream sends
 * out. */
struct Step {
    int poc {};
    std::vector<unsigned> left;
};

struct Run {
    std::vector<Step> steps;
    std::vector<unsigned> tail;

    [[nodiscard]] std::vector<unsigned> order() const
    {
        std::vector<unsigned> all;
        for (const auto& step : steps) all.insert(all.end(), step.left.begin(), step.left.end());
        all.insert(all.end(), tail.begin(), tail.end());
        return all;
    }
};

Run run(const SpsOptions& options, const std::vector<SliceOptions>& slices, bool bottomFieldPicOrder = false)
{
    ParameterSets sets;
    require(sets.store(makeSps(options)) != ParameterSets::Result::rejected, "the built SPS was rejected");
    require(sets.store(makePps(bottomFieldPicOrder)) != ParameterSets::Result::rejected, "the built PPS was rejected");
    const auto* sps = sets.sps(0);
    OutputOrder order;
    Run result;
    unsigned id = 0;
    for (const auto& slice : slices) {
        const auto unit = makeSlice(options, slice, bottomFieldPicOrder);
        const auto header = parseSliceHeader(unit, sets);
        require(header.has_value(), "a built slice header could not be read");
        std::vector<OutputOrder::Id> released;
        order.add(++id, *header, *sps, released);
        Step step {order.lastPoc(), {}};
        for (const auto value : released) step.left.push_back(unsigned(value));
        result.steps.push_back(step);
    }
    std::vector<OutputOrder::Id> rest;
    order.flush(rest);
    for (const auto value : rest) result.tail.push_back(unsigned(value));
    return result;
}

using Ids = std::vector<unsigned>;

void headers()
{
    // The slice header parser reads what it was written with, through the
    // fields that depend on the SPS.
    SpsOptions options;
    options.pocType = 1;
    options.offsetsForRefFrames = {4, 2};
    options.log2MaxFrameNum = 5;
    ParameterSets sets;
    require(sets.store(makeSps(options)) == ParameterSets::Result::changed, "SPS stored");
    require(sets.store(makePps(true)) == ParameterSets::Result::changed, "PPS stored");
    require(sets.store(makePps(true)) == ParameterSets::Result::unchanged, "the same PPS again");
    SliceOptions slice;
    slice.frameNum = 17;
    slice.deltaPoc0 = -3;
    slice.deltaPoc1 = 5;
    slice.adaptive = true;
    slice.mmco = {{1, 2, 0}, {3, 1, 1}, {2, 3, 0}, {4, 2, 0}, {6, 1, 0}};
    const auto header = parseSliceHeader(makeSlice(options, slice, true), sets);
    require(header.has_value(), "the header was read");
    require(header->frameNum == 17 && header->deltaPoc[0] == -3 && header->deltaPoc[1] == 5, "order count fields");
    require(header->adaptiveMarking && header->mmco.size() == 5, "five operations");
    require(header->mmco[1].operation == 3 && header->mmco[1].first == 1 && header->mmco[1].second == 1, "operation 3");
    require(header->mmco[4].operation == 6 && header->mmco[4].first == 1, "operation 6");
    require(!header->hasMmco5(), "no operation 5");
    slice.mmco.push_back({5, 0, 0});
    require(parseSliceHeader(makeSlice(options, slice, true), sets)->hasMmco5(), "operation 5");

    // Another slice of the same picture is not a new one.
    const auto again = parseSliceHeader(makeSlice(options, slice, true), sets);
    require(!startsPicture(*header, *again) || again->mmco.size() != header->mmco.size(), "a repeat is the same picture");
    SliceOptions next = slice;
    next.frameNum = 18;
    require(startsPicture(*again, *parseSliceHeader(makeSlice(options, next, true), sets)), "a new frame number");
    next = slice;
    next.refIdc = 0;
    require(startsPicture(*again, *parseSliceHeader(makeSlice(options, next, true), sets)), "a reference and a non-reference picture");

    // An unknown PPS or a truncated header is refused, not guessed.
    ParameterSets none;
    require(!parseSliceHeader(makeSlice(options, slice, true), none).has_value(), "no parameter sets");
    auto truncated = makeSlice(options, slice, true);
    truncated.resize(3);
    require(!parseSliceHeader(truncated, sets).has_value(), "a truncated header");
    // Slice groups are not decoded by any hardware this runs on.
    Bits groups;
    groups.ue(0); groups.ue(0); groups.flag(false); groups.flag(false); groups.ue(1);
    require(!parsePps(groups.nal(0x68)).has_value(), "slice groups are refused");
}

void pictureOrderCounts()
{
    // Type 0: the least significant bits wrap, the most significant follow.
    SpsOptions type0;
    type0.log2MaxPocLsb = 4;   // 16
    type0.reorder = 0;
    {
        std::vector<SliceOptions> pictures(6);
        pictures[0].idr = true; pictures[0].sliceType = 2;
        const unsigned lsb[] = {0, 6, 12, 2, 8, 14};
        for (unsigned i = 1; i < 6; ++i) { pictures[i].frameNum = i; pictures[i].pocLsb = lsb[i]; }
        const auto result = run(type0, pictures);
        const int expectedPoc[] = {0, 6, 12, 18, 24, 30};
        for (unsigned i = 0; i < 6; ++i) require(result.steps[i].poc == expectedPoc[i], "type 0 wraps");
        // A reorder depth of zero shows each picture as it arrives.
        for (unsigned i = 0; i < 6; ++i) require(result.steps[i].left == Ids {i + 1}, "nothing waits");
    }
    // Type 0 counts backwards for a picture that precedes the last reference.
    {
        std::vector<SliceOptions> pictures(3);
        pictures[0].idr = true; pictures[0].sliceType = 2;
        pictures[1].frameNum = 1; pictures[1].pocLsb = 14;
        pictures[2].frameNum = 2; pictures[2].pocLsb = 2;
        const auto result = run(type0, pictures);
        // 14 is eight or more above 0 in sixteen, so it is -2; 2 is more than eight above 14, so it is 18.
        require(result.steps[1].poc == -2, "a count below the start");
        require(result.steps[2].poc == 2, "and back up");
    }
    // Type 1: the counts of a cycle of two reference frames, offsets 4 and 2.
    {
        SpsOptions type1;
        type1.pocType = 1;
        type1.offsetsForRefFrames = {4, 2};
        type1.offsetForNonRef = -1;
        type1.offsetTopToBottom = 1;
        type1.reorder = 0;
        std::vector<SliceOptions> pictures(6);
        pictures[0].idr = true; pictures[0].sliceType = 2;
        pictures[1].frameNum = 1;
        pictures[2].frameNum = 2;
        pictures[3].frameNum = 3;
        pictures[4].frameNum = 4; pictures[4].refIdc = 0;
        pictures[5].frameNum = 4; pictures[5].deltaPoc0 = 3;
        const auto result = run(type1, pictures);
        // Frames into the cycle: 0; 4; 4 + 2; a second cycle starts, 6 + 4; a picture nothing refers to
        // counts one frame less, 6 + 4 - 1; and the fourth reference frame, 6 + 4 + 2, plus its delta of 3.
        const int expectedPoc[] = {0, 4, 6, 10, 9, 15};
        for (unsigned i = 0; i < 6; ++i) require(result.steps[i].poc == expectedPoc[i], "type 1 follows its cycle");
    }
    // Type 2: twice the frame number, one less for a picture nothing refers to,
    // and the frame number wraps into an offset.
    {
        SpsOptions type2;
        type2.pocType = 2;
        type2.log2MaxFrameNum = 4;
        std::vector<SliceOptions> pictures(5);
        pictures[0].idr = true; pictures[0].sliceType = 2;
        pictures[1].frameNum = 1;
        pictures[2].frameNum = 2; pictures[2].refIdc = 0;
        pictures[3].frameNum = 2;
        pictures[4].frameNum = 0;   // 16 with the wrap
        const auto result = run(type2, pictures);
        const int expectedPoc[] = {0, 2, 3, 4, 32};
        for (unsigned i = 0; i < 5; ++i) require(result.steps[i].poc == expectedPoc[i], "type 2 is twice the frame number");
        // And nothing is ever reordered.
        for (unsigned i = 0; i < 5; ++i) require(result.steps[i].left == Ids {i + 1}, "type 2 shows each picture at once");
    }
}

/* IDR, P (8), b (4), b (2), b (6): the second and third pictures after the P
 * precede it in display order. */
std::vector<SliceOptions> pyramid()
{
    std::vector<SliceOptions> pictures(5);
    pictures[0].idr = true; pictures[0].sliceType = 2;
    pictures[1].frameNum = 1; pictures[1].pocLsb = 8;
    pictures[2].frameNum = 2; pictures[2].pocLsb = 4; pictures[2].refIdc = 0;
    pictures[3].frameNum = 2; pictures[3].pocLsb = 2; pictures[3].refIdc = 0;
    pictures[4].frameNum = 2; pictures[4].pocLsb = 6; pictures[4].refIdc = 0;
    return pictures;
}

void bumping()
{
    SpsOptions options;
    options.reorder = 2;
    options.maxDecFrameBuffering = 4;
    options.maxNumRefFrames = 2;
    // The depth the stream states holds two pictures back: the lowest of three leaves.
    {
        const auto result = run(options, pyramid());
        require((result.steps[0].left.empty() && result.steps[1].left.empty() && result.steps[2].left == Ids {1} &&
                 result.steps[3].left == Ids {4} && result.steps[4].left == Ids {3}),
                "the lowest count leaves when more than two wait");
        require((result.tail == Ids {5, 2}), "and the rest at the end of the stream");
    }
    // A depth of zero shows each picture as it comes, whatever its count.
    {
        options.reorder = 0;
        const auto result = run(options, pyramid());
        for (unsigned i = 0; i < 5; ++i) require(result.steps[i].left == Ids {i + 1}, "no reordering");
        options.reorder = 2;
    }
    // A depth the buffer cannot hold is the size of the buffer. Two frames of two
    // references leave no room for a picture nothing refers to: the IDR picture
    // leaves, and so does the picture itself, being lower than the P picture.
    {
        SpsOptions small = options;
        small.reorder = 5;
        small.maxDecFrameBuffering = 2;
        const auto result = run(small, pyramid());
        require((result.steps[2].left == Ids {1, 3}), "no more wait than the buffer holds");
        require((result.steps[3].left == Ids {4} && result.steps[4].left == Ids {5}), "and the rest in order of count");
        require((result.tail == Ids {2}), "only the reference picture waits");
    }
    // An IDR picture sends everything before it out first, whatever the depth.
    {
        std::vector<SliceOptions> pictures(4);
        pictures[0].idr = true; pictures[0].sliceType = 2;
        pictures[1].frameNum = 1; pictures[1].pocLsb = 8;
        pictures[2].frameNum = 2; pictures[2].pocLsb = 4;
        pictures[3].idr = true; pictures[3].sliceType = 2;
        const auto result = run(options, pictures);
        require((result.steps[2].left == Ids {1} && result.steps[3].left == Ids {3, 2}),
                "an IDR picture shows what is waiting, in order of count");
        require((result.tail == Ids {4}), "and waits itself");
    }
    // Memory management operation 5 does the same, and counts from zero again.
    {
        std::vector<SliceOptions> pictures(5);
        pictures[0].idr = true; pictures[0].sliceType = 2;
        pictures[1].frameNum = 1; pictures[1].pocLsb = 8;
        pictures[2].frameNum = 2; pictures[2].pocLsb = 4; pictures[2].refIdc = 0;
        pictures[3].frameNum = 2; pictures[3].pocLsb = 20; pictures[3].adaptive = true; pictures[3].mmco = {{5, 0, 0}};
        pictures[4].frameNum = 1; pictures[4].pocLsb = 4;   // frame numbers start over too
        const auto result = run(options, pictures);
        require(result.steps[3].poc == 0, "the picture with operation 5 counts from zero");
        require((result.steps[2].left == Ids {1} && result.steps[3].left == Ids {3, 2}),
                "operation 5 shows everything before it, in order of count");
        require(result.steps[4].poc == 4 && result.steps[4].left.empty(), "and the next picture is counted from it");
        require((result.tail == Ids {4, 5}), "the picture with operation 5 comes before it");
    }
    // The defaults: a profile or an order count type that cannot reorder shows each picture at once.
    {
        SpsOptions baseline;
        baseline.profile = 66;
        const auto result = run(baseline, pyramid());
        for (unsigned i = 0; i < 5; ++i) require(result.steps[i].left == Ids {i + 1}, "Baseline does not reorder");
        SpsOptions main;   // 320 x 240 at level 3: sixteen frames
        const auto held = run(main, pyramid());
        for (const auto& step : held.steps) require(step.left.empty(), "Main without limits holds pictures back");
        require((held.tail == Ids {1, 4, 3, 5, 2}), "and shows them in order of count at the end");
    }
}

/* A long stream in the shape x264 writes: a reference P-picture, a reference
 * B-picture before it in display order, and two B-pictures nothing refers to,
 * with the frame number and the least significant bits of the count wrapping
 * many times over. With the stream's own limits or the level's, the pictures
 * must leave in the order of their counts. */
void longSequences()
{
    for (const bool stated : {true, false}) {
        SpsOptions options;
        options.log2MaxFrameNum = 4;
        options.log2MaxPocLsb = 5;
        options.maxNumRefFrames = 3;
        if (stated) {
            options.reorder = 2;
            options.maxDecFrameBuffering = 4;
        }
        std::vector<SliceOptions> pictures;
        std::vector<unsigned> display;   // the display position of each picture
        const auto add = [&](unsigned position, bool reference, unsigned frameNum) {
            SliceOptions slice;
            slice.refIdc = reference ? 2 : 0;
            slice.frameNum = frameNum % 16;
            slice.pocLsb = (2 * position) % 32;
            pictures.push_back(slice);
            display.push_back(position);
        };
        SliceOptions idr;
        idr.idr = true;
        idr.sliceType = 2;
        pictures.push_back(idr);
        display.push_back(0);
        unsigned frameNum = 1;
        for (unsigned group = 0; group < 40; ++group) {
            const unsigned base = group * 4;
            add(base + 4, true, frameNum++);
            add(base + 2, true, frameNum++);
            add(base + 1, false, frameNum);
            add(base + 3, false, frameNum);
        }
        const auto result = run(options, pictures);
        const auto all = result.order();
        require(all.size() == pictures.size(), "every picture is shown once");
        for (std::size_t k = 1; k < all.size(); ++k)
            require(display[all[k - 1] - 1] < display[all[k] - 1], "pictures leave in order of their counts");
        // The depth of the stream holds two pictures back, and the level's holds more.
        require(result.tail.size() >= 2, "pictures wait at the end of the stream");
        std::printf("long sequence, %s limits: %zu pictures wait at the end\n", stated ? "stated" : "level",
                    result.tail.size());
    }
}

void referenceMarking()
{
    // Frame numbers that skip, in a stream that allows it, hold places in the buffer
    // and are never shown.
    {
        SpsOptions gaps;
        gaps.reorder = 2;
        gaps.maxDecFrameBuffering = 4;
        gaps.maxNumRefFrames = 2;
        gaps.gapsAllowed = true;
        std::vector<SliceOptions> pictures(4);
        pictures[0].idr = true; pictures[0].sliceType = 2;
        pictures[1].frameNum = 1; pictures[1].pocLsb = 2;
        pictures[2].frameNum = 5; pictures[2].pocLsb = 4;   // frame numbers 2, 3 and 4 are missing
        pictures[3].frameNum = 6; pictures[3].pocLsb = 6;
        const auto result = run(gaps, pictures);
        require((result.order() == Ids {1, 2, 3, 4}), "only real pictures leave, in order");
    }
    // Field pictures are not modelled: each leaves after what waits, in decoding order.
    {
        SpsOptions interlaced;
        interlaced.frameMbsOnly = false;
        interlaced.heightMacroblocks = 16;
        interlaced.reorder = 2;
        std::vector<SliceOptions> pictures(4);
        pictures[0].idr = true; pictures[0].sliceType = 2;
        pictures[1].frameNum = 1; pictures[1].pocLsb = 8;
        pictures[2].frameNum = 2; pictures[2].field = true; pictures[2].pocLsb = 4;
        pictures[3].frameNum = 2; pictures[3].field = true; pictures[3].bottom = true; pictures[3].pocLsb = 5;
        const auto result = run(interlaced, pictures);
        require((result.order() == Ids {1, 2, 3, 4}), "fields leave in decoding order after the frames before them");
        require((result.steps[2].left == Ids {1, 2, 3}), "and send what was waiting out first");
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: uml_h264_stream_tests FIXTURE_DIRECTORY\n");
        return 2;
    }
    fixtureDirectory = argv[1];
    try {
        parameterSets();
        croppingWindow();
        headers();
        fixtureOrder();
        pictureOrderCounts();
        bumping();
        longSequences();
        referenceMarking();
        std::puts("h264 stream tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
