// SPDX-License-Identifier: GPL-2.0-or-later
#include "h264_stream.hpp"
#include "rbsp_writer.hpp"

#include <algorithm>

namespace uml::platform::h264 {
namespace {

using bitstream::BitReader;

// Offsets and picture sizes beyond this are not a picture any decoder takes.
constexpr unsigned largestMacroblocks = 1u << 12;

// MaxDpbMbs of Table A-1.
unsigned maxDpbMacroblocks(const Sps& sps)
{
    const bool baseline = sps.profile == 66 || sps.profile == 77 || sps.profile == 88;
    switch (sps.level) {
    case 9: case 10: return 396;
    case 11: return baseline && (sps.constraintFlags & 0x10) ? 396 : 900;   // level 1b
    case 12: case 13: case 20: return 2376;
    case 21: return 4752;
    case 22: case 30: return 8100;
    case 31: return 18000;
    case 32: return 20480;
    case 40: case 41: return 32768;
    case 42: return 34816;
    case 50: return 110400;
    case 51: case 52: return 184320;
    default: return 696320;   // levels 6 to 6.2, and anything this does not know
    }
}

// The profiles whose SPS carries chroma_format_idc and the scaling matrices.
bool hasChromaFormat(unsigned profile)
{
    switch (profile) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86:
    case 118: case 128: case 138: case 139: case 134: case 135: return true;
    default: return false;
    }
}

} // namespace

unsigned Sps::dpbFrames() const noexcept
{
    unsigned frames;
    if (bitstreamRestriction) {
        frames = std::max(maxDecFrameBuffering, maxNumRefFrames);
    } else {
        const unsigned macroblocks = widthMacroblocks * heightMacroblocks;
        frames = macroblocks ? std::min(16u, maxDpbMacroblocks(*this) / macroblocks) : 16u;
        frames = std::max(frames, maxNumRefFrames);
    }
    return std::max(frames, 1u);
}

unsigned Sps::reorderFrames() const noexcept
{
    if (bitstreamRestriction) return std::min(maxNumReorderFrames, dpbFrames());
    // Without a statement the standard infers the size of the buffer, except
    // where the profile or the order count says nothing is ever reordered.
    if (pocType == 2 || profile == 66) return 0;
    const bool intra = profile == 44 || profile == 86 || profile == 100 || profile == 110 ||
                       profile == 122 || profile == 244;
    if (intra && (constraintFlags & 0x10)) return 0;
    return dpbFrames();
}

bool SliceHeader::hasMmco5() const noexcept
{
    return std::any_of(mmco.begin(), mmco.end(), [](const Mmco& m) { return m.operation == 5; });
}

std::optional<Sps> parseSps(Bytes nal)
{
    if (nal.size() < 4 || nalType(nal) != nalSps) return std::nullopt;
    const auto payload = bitstream::rbsp(nal, 1);
    BitReader reader(payload);
    Sps sps;
    sps.profile = reader.bits(8);
    sps.constraintFlags = reader.bits(8);
    sps.level = reader.bits(8);
    sps.id = reader.unsignedExpGolomb();
    if (sps.id > 31) return std::nullopt;
    if (hasChromaFormat(sps.profile)) {
        sps.chromaFormat = reader.unsignedExpGolomb();
        if (sps.chromaFormat > 3) return std::nullopt;
        if (sps.chromaFormat == 3) sps.separateColourPlane = reader.bits(1) != 0;
        const unsigned luma = reader.unsignedExpGolomb();
        const unsigned chroma = reader.unsignedExpGolomb();
        if (luma > 6 || chroma > 6) return std::nullopt;
        sps.bitDepth = 8 + luma;
        reader.bits(1);   // qpprime_y_zero_transform_bypass_flag
        if (reader.bits(1)) {   // seq_scaling_matrix_present_flag
            for (unsigned i = 0; i < (sps.chromaFormat != 3 ? 8u : 12u) && reader.ok(); ++i)
                if (reader.bits(1)) bitstream::detail::skipH264ScalingList(reader, i < 6 ? 16 : 64);
        }
    }
    const unsigned frameNumBits = reader.unsignedExpGolomb() + 4;
    if (frameNumBits > 16) return std::nullopt;
    sps.log2MaxFrameNum = frameNumBits;
    sps.pocType = reader.unsignedExpGolomb();
    if (sps.pocType > 2) return std::nullopt;
    if (sps.pocType == 0) {
        const unsigned bits = reader.unsignedExpGolomb() + 4;
        if (bits > 16) return std::nullopt;
        sps.log2MaxPocLsb = bits;
    } else if (sps.pocType == 1) {
        sps.deltaPicOrderAlwaysZero = reader.bits(1) != 0;
        sps.offsetForNonRefPic = reader.signedExpGolomb();
        sps.offsetForTopToBottomField = reader.signedExpGolomb();
        const unsigned cycle = reader.unsignedExpGolomb();
        if (cycle > 255) return std::nullopt;
        for (unsigned i = 0; i < cycle && reader.ok(); ++i) sps.offsetForRefFrame.push_back(reader.signedExpGolomb());
    }
    sps.maxNumRefFrames = reader.unsignedExpGolomb();
    if (sps.maxNumRefFrames > 16) return std::nullopt;
    sps.gapsAllowed = reader.bits(1) != 0;
    sps.widthMacroblocks = reader.unsignedExpGolomb() + 1;
    const unsigned mapUnits = reader.unsignedExpGolomb() + 1;
    sps.frameMbsOnly = reader.bits(1) != 0;
    if (!sps.frameMbsOnly) reader.bits(1);   // mb_adaptive_frame_field_flag
    sps.heightMacroblocks = mapUnits * (sps.frameMbsOnly ? 1 : 2);
    reader.bits(1);   // direct_8x8_inference_flag
    if (!reader.ok() || sps.widthMacroblocks > largestMacroblocks || sps.heightMacroblocks > largestMacroblocks)
        return std::nullopt;
    sps.windowFlagBit = reader.position();
    sps.hasWindow = reader.bits(1) != 0;
    if (sps.hasWindow) {
        const unsigned left = reader.unsignedExpGolomb();
        const unsigned right = reader.unsignedExpGolomb();
        const unsigned top = reader.unsignedExpGolomb();
        const unsigned bottom = reader.unsignedExpGolomb();
        if (!reader.ok()) return std::nullopt;
        // CropUnitX and CropUnitY (7.4.2.1.1).
        const unsigned chromaArrayType = sps.separateColourPlane ? 0 : sps.chromaFormat;
        const unsigned unitX = chromaArrayType == 0 || chromaArrayType == 3 ? 1 : 2;
        const unsigned unitY = (chromaArrayType == 0 || chromaArrayType >= 2 ? 1 : 2) * (sps.frameMbsOnly ? 1 : 2);
        const unsigned long long horizontal = (unsigned long long)(left + right) * unitX;
        const unsigned long long vertical = (unsigned long long)(top + bottom) * unitY;
        if (horizontal >= sps.width() || vertical >= sps.height()) return std::nullopt;
        sps.window = {left * unitX, right * unitX, top * unitY, bottom * unitY};
    }
    sps.windowEndBit = reader.position();
    if (!reader.ok()) return std::nullopt;
    if (const auto limits = bitstream::parseH264SpsLimits(nal); limits && limits->bitstreamRestriction) {
        sps.bitstreamRestriction = true;
        sps.maxNumReorderFrames = limits->maxNumReorderFrames;
        sps.maxDecFrameBuffering = limits->maxDecFrameBuffering;
    }
    return sps;
}

std::optional<Pps> parsePps(Bytes nal)
{
    if (nal.size() < 3 || nalType(nal) != nalPps) return std::nullopt;
    const auto payload = bitstream::rbsp(nal, 1);
    BitReader reader(payload);
    Pps pps;
    pps.id = reader.unsignedExpGolomb();
    pps.spsId = reader.unsignedExpGolomb();
    if (pps.id > 255 || pps.spsId > 31) return std::nullopt;
    reader.bits(1);   // entropy_coding_mode_flag
    pps.bottomFieldPicOrderInFramePresent = reader.bits(1) != 0;
    if (reader.unsignedExpGolomb() != 0) return std::nullopt;   // slice groups
    for (auto& count : pps.numRefIdxDefault) {
        count = reader.unsignedExpGolomb() + 1;
        if (count > 32) return std::nullopt;
    }
    pps.weightedPred = reader.bits(1) != 0;
    pps.weightedBipredIdc = reader.bits(2);
    reader.signedExpGolomb();   // pic_init_qp_minus26
    reader.signedExpGolomb();   // pic_init_qs_minus26
    reader.signedExpGolomb();   // chroma_qp_index_offset
    reader.bits(1);             // deblocking_filter_control_present_flag
    reader.bits(1);             // constrained_intra_pred_flag
    pps.redundantPicCntPresent = reader.bits(1) != 0;
    if (!reader.ok()) return std::nullopt;
    return pps;
}

std::optional<std::vector<std::uint8_t>> withoutCropping(Bytes nal)
{
    const auto sps = parseSps(nal);
    if (!sps) return std::nullopt;
    if (!sps->hasWindow) return std::vector<std::uint8_t>(nal.begin(), nal.end());
    const auto payload = bitstream::rbsp(nal, 1);
    // The stop bit is the last bit set; everything before it is the syntax.
    std::size_t stop = payload.size() * 8;
    while (stop > 0 && !((payload[(stop - 1) / 8] >> (7 - (stop - 1) % 8)) & 1)) --stop;
    if (stop == 0 || stop - 1 < sps->windowEndBit) return std::nullopt;
    --stop;
    bitstream::RbspWriter writer;
    writer.copy(payload, 0, sps->windowFlagBit);
    writer.bit(false);   // frame_cropping_flag
    writer.copy(payload, sps->windowEndBit, stop);
    writer.bit(true);    // rbsp_stop_one_bit
    const auto escaped = bitstream::escapeEmulation(writer.bytes());
    std::vector<std::uint8_t> out(nal.begin(), nal.begin() + 1);
    out.insert(out.end(), escaped.begin(), escaped.end());
    return out;
}

ParameterSets::Result ParameterSets::store(Bytes nal)
{
    if (nal.size() < 3) return Result::rejected;
    const auto replace = [&nal](std::map<unsigned, std::vector<std::uint8_t>>& units, unsigned id) {
        auto& saved = units[id];
        if (std::equal(saved.begin(), saved.end(), nal.begin(), nal.end())) return Result::unchanged;
        saved.assign(nal.begin(), nal.end());
        return Result::changed;
    };
    switch (nalType(nal)) {
    case nalSps: {
        const auto sps = parseSps(nal);
        if (!sps) return Result::rejected;
        sps_[sps->id] = *sps;
        return replace(spsUnits_, sps->id);
    }
    case nalPps: {
        const auto pps = parsePps(nal);
        if (!pps) return Result::rejected;
        pps_[pps->id] = *pps;
        return replace(ppsUnits_, pps->id);
    }
    default: return Result::rejected;
    }
}

const Sps* ParameterSets::sps(unsigned id) const
{
    const auto found = sps_.find(id);
    return found == sps_.end() ? nullptr : &found->second;
}

const Pps* ParameterSets::pps(unsigned id) const
{
    const auto found = pps_.find(id);
    return found == pps_.end() ? nullptr : &found->second;
}

std::vector<std::vector<std::uint8_t>> ParameterSets::descriptionUnits() const
{
    std::vector<std::vector<std::uint8_t>> result;
    for (const auto& [id, unit] : spsUnits_) {
        auto stripped = withoutCropping(unit);
        result.push_back(stripped ? std::move(*stripped) : unit);
    }
    for (const auto& [id, unit] : ppsUnits_) result.push_back(unit);
    return result;
}

void ParameterSets::clear()
{
    spsUnits_.clear();
    ppsUnits_.clear();
    sps_.clear();
    pps_.clear();
}

std::optional<SliceHeader> parseSliceHeader(Bytes nal, const ParameterSets& sets)
{
    if (nal.size() < 2 || !isPicture(nalType(nal))) return std::nullopt;
    const auto payload = bitstream::rbsp(nal, 1);
    BitReader reader(payload);
    SliceHeader slice;
    slice.type = nalType(nal);
    slice.refIdc = nalRefIdc(nal);
    slice.firstMb = reader.unsignedExpGolomb();
    const unsigned type = reader.unsignedExpGolomb();
    if (type > 9) return std::nullopt;
    slice.sliceType = type % 5;
    slice.ppsId = reader.unsignedExpGolomb();
    const auto* pps = sets.pps(slice.ppsId);
    const auto* sps = pps ? sets.sps(pps->spsId) : nullptr;
    if (!sps || !reader.ok()) return std::nullopt;
    slice.spsId = sps->id;
    if (sps->separateColourPlane) reader.bits(2);   // colour_plane_id
    slice.frameNum = reader.bits(sps->log2MaxFrameNum);
    if (!sps->frameMbsOnly) {
        slice.fieldPic = reader.bits(1) != 0;
        if (slice.fieldPic) slice.bottomField = reader.bits(1) != 0;
    }
    if (slice.idr()) slice.idrPicId = reader.unsignedExpGolomb();
    if (sps->pocType == 0) {
        slice.pocLsb = reader.bits(sps->log2MaxPocLsb);
        if (pps->bottomFieldPicOrderInFramePresent && !slice.fieldPic) slice.deltaPocBottom = reader.signedExpGolomb();
    } else if (sps->pocType == 1 && !sps->deltaPicOrderAlwaysZero) {
        slice.deltaPoc[0] = reader.signedExpGolomb();
        if (pps->bottomFieldPicOrderInFramePresent && !slice.fieldPic) slice.deltaPoc[1] = reader.signedExpGolomb();
    }
    if (pps->redundantPicCntPresent) slice.redundantPicCnt = reader.unsignedExpGolomb();
    const bool predicted = slice.sliceType == 0 || slice.sliceType == 1 || slice.sliceType == 3;
    const bool bipredicted = slice.sliceType == 1;
    unsigned activeReferences[2] = {pps->numRefIdxDefault[0], pps->numRefIdxDefault[1]};
    if (bipredicted) reader.bits(1);   // direct_spatial_mv_pred_flag
    if (predicted) {
        if (reader.bits(1)) {   // num_ref_idx_active_override_flag
            activeReferences[0] = reader.unsignedExpGolomb() + 1;
            if (bipredicted) activeReferences[1] = reader.unsignedExpGolomb() + 1;
        }
        if (activeReferences[0] > 32 || activeReferences[1] > 32) return std::nullopt;
        // ref_pic_list_modification()
        for (unsigned list = 0; list < (bipredicted ? 2u : 1u); ++list) {
            if (!reader.bits(1)) continue;
            for (unsigned operations = 0; reader.ok(); ++operations) {
                if (operations > 66) return std::nullopt;
                const unsigned idc = reader.unsignedExpGolomb();
                if (idc == 3) break;
                if (idc > 5) return std::nullopt;
                reader.unsignedExpGolomb();
            }
        }
        // pred_weight_table()
        if ((pps->weightedPred && (slice.sliceType == 0 || slice.sliceType == 3)) ||
            (pps->weightedBipredIdc == 1 && bipredicted)) {
            const bool chroma = !sps->separateColourPlane && sps->chromaFormat != 0;
            reader.unsignedExpGolomb();   // luma_log2_weight_denom
            if (chroma) reader.unsignedExpGolomb();
            for (unsigned list = 0; list < (bipredicted ? 2u : 1u); ++list)
                for (unsigned i = 0; i < activeReferences[list] && reader.ok(); ++i) {
                    if (reader.bits(1)) { reader.signedExpGolomb(); reader.signedExpGolomb(); }
                    if (chroma && reader.bits(1))
                        for (unsigned j = 0; j < 4; ++j) reader.signedExpGolomb();
                }
        }
    }
    if (slice.refIdc) {
        // dec_ref_pic_marking()
        if (slice.idr()) {
            slice.noOutputOfPriorPics = reader.bits(1) != 0;
            slice.longTermReference = reader.bits(1) != 0;
        } else {
            slice.adaptiveMarking = reader.bits(1) != 0;
            for (unsigned operations = 0; slice.adaptiveMarking && reader.ok(); ++operations) {
                if (operations > 66) return std::nullopt;
                Mmco op;
                op.operation = reader.unsignedExpGolomb();
                if (op.operation == 0) break;
                if (op.operation > 6) return std::nullopt;
                if (op.operation == 1 || op.operation == 3) op.first = reader.unsignedExpGolomb();
                if (op.operation == 2) op.first = reader.unsignedExpGolomb();
                if (op.operation == 3) op.second = reader.unsignedExpGolomb();
                if (op.operation == 6 || op.operation == 4) op.first = reader.unsignedExpGolomb();
                slice.mmco.push_back(op);
            }
        }
    }
    if (!reader.ok()) return std::nullopt;
    return slice;
}

bool startsPicture(const SliceHeader& previous, const SliceHeader& slice)
{
    return previous.frameNum != slice.frameNum || previous.ppsId != slice.ppsId ||
           previous.fieldPic != slice.fieldPic || previous.bottomField != slice.bottomField ||
           (previous.refIdc == 0) != (slice.refIdc == 0) ||
           previous.pocLsb != slice.pocLsb || previous.deltaPocBottom != slice.deltaPocBottom ||
           previous.deltaPoc[0] != slice.deltaPoc[0] || previous.deltaPoc[1] != slice.deltaPoc[1] ||
           previous.idr() != slice.idr() || (slice.idr() && previous.idrPicId != slice.idrPicId);
}

int OutputOrder::pictureOrderCount(const SliceHeader& slice, const Sps& sps, bool mmco5, int& top)
{
    const int maxFrameNum = 1 << sps.log2MaxFrameNum;
    const bool idr = slice.idr();
    int bottom = 0;
    top = 0;
    if (sps.pocType == 0) {
        const int prevMsb = idr ? 0 : prevPocMsb_;
        const int prevLsb = idr ? 0 : prevPocLsb_;
        const int maxLsb = 1 << sps.log2MaxPocLsb;
        const int lsb = int(slice.pocLsb);
        int msb = prevMsb;
        if (lsb < prevLsb && prevLsb - lsb >= maxLsb / 2) msb = prevMsb + maxLsb;
        else if (lsb > prevLsb && lsb - prevLsb > maxLsb / 2) msb = prevMsb - maxLsb;
        top = msb + lsb;
        bottom = top + slice.deltaPocBottom;
        if (slice.refIdc) {
            prevPocMsb_ = msb;
            prevPocLsb_ = lsb;
        }
    } else {
        const int frameNum = int(slice.frameNum);
        const int offset = idr ? 0
            : (int(prevFrameNum_) > frameNum ? prevFrameNumOffset_ + maxFrameNum : prevFrameNumOffset_);
        if (sps.pocType == 1) {
            const int cycle = int(sps.offsetForRefFrame.size());
            int absFrameNum = cycle ? offset + frameNum : 0;
            if (!slice.refIdc && absFrameNum > 0) --absFrameNum;
            int expected = 0;
            if (absFrameNum > 0) {
                int perCycle = 0;
                for (const int step : sps.offsetForRefFrame) perCycle += step;
                const int cycles = (absFrameNum - 1) / cycle;
                const int inCycle = (absFrameNum - 1) % cycle;
                expected = cycles * perCycle;
                for (int i = 0; i <= inCycle; ++i) expected += sps.offsetForRefFrame[std::size_t(i)];
            }
            if (!slice.refIdc) expected += sps.offsetForNonRefPic;
            top = expected + slice.deltaPoc[0];
            bottom = top + sps.offsetForTopToBottomField + slice.deltaPoc[1];
        } else {
            const int count = idr ? 0 : (slice.refIdc ? 2 * (offset + frameNum) : 2 * (offset + frameNum) - 1);
            top = bottom = count;
        }
        prevFrameNumOffset_ = offset;
        prevFrameNum_ = slice.frameNum;
    }
    int poc = std::min(top, bottom);
    if (mmco5) {
        // The picture is taken to have had frame_num 0 and the lowest count 0.
        top -= poc;
        poc = 0;
        prevPocMsb_ = 0;
        prevPocLsb_ = top;
        prevFrameNumOffset_ = 0;
        prevFrameNum_ = 0;
        prevRefFrameNum_ = 0;
    } else if (slice.refIdc) {
        prevRefFrameNum_ = slice.frameNum;
    }
    return poc;
}

void OutputOrder::slidingWindow(const Sps& sps, unsigned frameNum)
{
    unsigned shortTerm = 0, longTerm = 0;
    for (const auto& frame : dpb_) {
        shortTerm += frame.shortTerm;
        longTerm += frame.longTerm;
    }
    if (!shortTerm || shortTerm + longTerm < std::max(sps.maxNumRefFrames, 1u)) return;
    // The short-term frame with the lowest FrameNumWrap is the oldest.
    Frame* oldest = nullptr;
    int oldestWrap = 0;
    for (auto& frame : dpb_) {
        if (!frame.shortTerm) continue;
        const int wrap = frame.frameNum > frameNum ? int(frame.frameNum) - (1 << sps.log2MaxFrameNum)
                                                   : int(frame.frameNum);
        if (!oldest || wrap < oldestWrap) {
            oldest = &frame;
            oldestWrap = wrap;
        }
    }
    if (oldest) oldest->shortTerm = false;
}

void OutputOrder::fillFrameNumGap(const SliceHeader& slice, const Sps& sps)
{
    const unsigned maxFrameNum = 1u << sps.log2MaxFrameNum;
    const unsigned missing = (slice.frameNum + maxFrameNum - prevRefFrameNum_ - 1) % maxFrameNum;
    // Only as many frames as the window holds can still matter.
    const unsigned count = std::min(missing, 16u);
    unsigned frameNum = (slice.frameNum + maxFrameNum - count) % maxFrameNum;
    for (unsigned i = 0; i < count; ++i, frameNum = (frameNum + 1) % maxFrameNum) {
        slidingWindow(sps, frameNum);
        // A frame that is not there: it holds a place and is never shown.
        Frame frame;
        frame.shortTerm = true;
        frame.frameNum = frameNum;
        dpb_.push_back(frame);
        if (sps.pocType != 0) {
            if (prevFrameNum_ > frameNum) prevFrameNumOffset_ += int(maxFrameNum);
            prevFrameNum_ = frameNum;
        }
        prevRefFrameNum_ = frameNum;
    }
    dpb_.erase(std::remove_if(dpb_.begin(), dpb_.end(),
                              [](const Frame& frame) { return !frame.reference() && !frame.output; }),
               dpb_.end());
}

void OutputOrder::mark(const SliceHeader& slice, const Sps& sps, Frame& current)
{
    const int maxFrameNum = 1 << sps.log2MaxFrameNum;
    const auto wrap = [&](const Frame& frame) {
        return int(frame.frameNum) > int(slice.frameNum) ? int(frame.frameNum) - maxFrameNum : int(frame.frameNum);
    };
    const auto freeIndex = [&](unsigned index) {
        for (auto& frame : dpb_) if (frame.longTerm && frame.longTermIdx == index) frame.longTerm = false;
    };
    if (slice.idr()) {
        current.longTerm = slice.longTermReference;
        current.shortTerm = !current.longTerm;
        current.longTermIdx = 0;
        maxLongTermIdx_ = current.longTerm ? 0 : -1;
        return;
    }
    current.shortTerm = true;
    if (!slice.adaptiveMarking) {
        slidingWindow(sps, slice.frameNum);
        return;
    }
    for (const auto& op : slice.mmco) {
        switch (op.operation) {
        case 1: {
            const int picNum = int(slice.frameNum) - int(op.first + 1);
            for (auto& frame : dpb_) if (frame.shortTerm && wrap(frame) == picNum) frame.shortTerm = false;
            break;
        }
        case 2:
            for (auto& frame : dpb_) if (frame.longTerm && frame.longTermIdx == op.first) frame.longTerm = false;
            break;
        case 3: {
            const int picNum = int(slice.frameNum) - int(op.first + 1);
            freeIndex(op.second);
            for (auto& frame : dpb_) {
                if (!frame.shortTerm || wrap(frame) != picNum) continue;
                frame.shortTerm = false;
                frame.longTerm = true;
                frame.longTermIdx = op.second;
            }
            break;
        }
        case 4:
            maxLongTermIdx_ = int(op.first) - 1;
            for (auto& frame : dpb_)
                if (frame.longTerm && int(frame.longTermIdx) > maxLongTermIdx_) frame.longTerm = false;
            break;
        case 5:
            for (auto& frame : dpb_) frame.shortTerm = frame.longTerm = false;
            maxLongTermIdx_ = -1;
            break;
        case 6:
            freeIndex(op.first);
            current.shortTerm = false;
            current.longTerm = true;
            current.longTermIdx = op.first;
            break;
        default: break;
        }
    }
}

void OutputOrder::add(Id id, const SliceHeader& slice, const Sps& sps, std::vector<Id>& released)
{
    if (slice.fieldPic) {
        // Field pictures are not modelled: they leave in decoding order, after
        // everything that is waiting, and what they refer to is not tracked.
        flush(released);
        dpb_.clear();
        released.push_back(id);
        return;
    }
    const unsigned maxFrameNum = 1u << sps.log2MaxFrameNum;
    if (!slice.idr() && sps.gapsAllowed && slice.frameNum != prevRefFrameNum_ &&
        slice.frameNum != (prevRefFrameNum_ + 1) % maxFrameNum)
        fillFrameNumGap(slice, sps);
    const bool mmco5 = slice.hasMmco5();
    int top = 0;
    Frame current;
    current.id = id;
    current.output = true;
    current.poc = pictureOrderCount(slice, sps, mmco5, top);
    current.frameNum = mmco5 ? 0 : slice.frameNum;
    lastPoc_ = current.poc;
    // C.4.4: a picture that starts over sends out everything before it, whatever
    // no_output_of_prior_pics_flag says, because a frame the decoder has is a
    // frame its client is owed.
    if (slice.idr() || mmco5) {
        flush(released);
        dpb_.clear();
    }
    if (slice.refIdc) mark(slice, sps, current);
    forget();

    const auto lowest = [&](bool includeCurrent) {
        Frame* best = nullptr;
        for (auto& frame : dpb_) if (frame.output && (!best || frame.poc < best->poc)) best = &frame;
        if (includeCurrent && current.output && (!best || current.poc < best->poc)) best = &current;
        return best;
    };
    const auto show = [&](Frame& frame) {
        released.push_back(frame.id);
        frame.output = false;
    };
    // More frames wait than the stream may reorder: the lowest leaves.
    const unsigned reorder = sps.reorderFrames();
    for (;;) {
        std::size_t waiting = current.output ? 1 : 0;
        for (const auto& frame : dpb_) waiting += frame.output;
        if (waiting <= reorder) break;
        show(*lowest(true));
        forget();
    }
    if (!current.output && !current.reference()) return;
    // The buffer has no room: frames leave until it has, or the current one does.
    const std::size_t room = sps.dpbFrames();
    while (dpb_.size() >= room) {
        Frame* next = lowest(!current.reference());
        if (!next) break;
        const bool isCurrent = next == &current;
        show(*next);
        forget();
        if (isCurrent && !current.reference()) return;
    }
    dpb_.push_back(current);
}

void OutputOrder::forget()
{
    dpb_.erase(std::remove_if(dpb_.begin(), dpb_.end(),
                              [](const Frame& frame) { return !frame.reference() && !frame.output; }),
               dpb_.end());
}

void OutputOrder::flush(std::vector<Id>& released)
{
    std::vector<Frame*> waiting;
    for (auto& frame : dpb_) if (frame.output) waiting.push_back(&frame);
    std::stable_sort(waiting.begin(), waiting.end(), [](const Frame* a, const Frame* b) { return a->poc < b->poc; });
    for (auto* frame : waiting) {
        released.push_back(frame->id);
        frame->output = false;
    }
    forget();
}

void OutputOrder::reset() { *this = OutputOrder(); }

std::size_t OutputOrder::waiting() const noexcept
{
    return std::size_t(std::count_if(dpb_.begin(), dpb_.end(), [](const Frame& frame) { return frame.output; }));
}

} // namespace uml::platform::h264
