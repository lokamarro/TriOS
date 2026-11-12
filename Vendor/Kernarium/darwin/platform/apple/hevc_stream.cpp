// SPDX-License-Identifier: GPL-2.0-or-later
#include "hevc_stream.hpp"
#include "rbsp_writer.hpp"

#include <algorithm>

namespace uml::platform::hevc {
namespace {

using bitstream::BitReader;
using BitWriter = bitstream::RbspWriter;

// Skips n bits of a reader whose reads are at most 32 bits wide.
void skip(BitReader& reader, unsigned count)
{
    for (; count >= 32; count -= 32) reader.bits(32);
    if (count) reader.bits(count);
}

// Offsets and picture sizes beyond this are not a picture any decoder takes.
constexpr unsigned largestDimension = 1u << 16;

} // namespace

std::optional<Sps> parseSps(Bytes nal)
{
    if (nal.size() < 4 || nalType(nal) != nalSps) return std::nullopt;
    const auto payload = bitstream::rbsp(nal, 2);
    BitReader reader(payload);
    reader.bits(4);   // sps_video_parameter_set_id
    const unsigned subLayers = reader.bits(3);   // sps_max_sub_layers_minus1
    if (subLayers > 6) return std::nullopt;
    reader.bits(1);   // sps_temporal_id_nesting_flag
    Sps sps;
    // profile_tier_level: the general part is 88 bits and the level 8; every
    // sub-layer adds a profile of 88 bits and a level of 8 when it states them.
    reader.bits(2);
    reader.bits(1);
    sps.profile = reader.bits(5);
    skip(reader, 32 + 48);
    reader.bits(8);   // general_level_idc
    bool subLayerProfile[7] = {}, subLayerLevel[7] = {};
    for (unsigned i = 0; i < subLayers; ++i) {
        subLayerProfile[i] = reader.bits(1) != 0;
        subLayerLevel[i] = reader.bits(1) != 0;
    }
    if (subLayers > 0)
        for (unsigned i = subLayers; i < 8; ++i) reader.bits(2);
    for (unsigned i = 0; i < subLayers; ++i) {
        if (subLayerProfile[i]) skip(reader, 88);
        if (subLayerLevel[i]) reader.bits(8);
    }
    sps.id = reader.unsignedExpGolomb();
    sps.chromaFormat = reader.unsignedExpGolomb();
    if (sps.id > 15 || sps.chromaFormat > 3) return std::nullopt;
    if (sps.chromaFormat == 3) sps.separateColourPlane = reader.bits(1) != 0;
    sps.width = reader.unsignedExpGolomb();
    sps.height = reader.unsignedExpGolomb();
    if (!sps.width || !sps.height || sps.width > largestDimension || sps.height > largestDimension)
        return std::nullopt;
    sps.windowFlagBit = reader.position();
    sps.hasWindow = reader.bits(1) != 0;
    if (sps.hasWindow) {
        // Offsets count chroma samples: two luma samples across for 4:2:0 and
        // 4:2:2, and two down for 4:2:0.
        const unsigned across = !sps.separateColourPlane && (sps.chromaFormat == 1 || sps.chromaFormat == 2) ? 2 : 1;
        const unsigned down = !sps.separateColourPlane && sps.chromaFormat == 1 ? 2 : 1;
        unsigned offsets[4];
        for (auto& offset : offsets) {
            offset = reader.unsignedExpGolomb();
            if (offset > largestDimension) return std::nullopt;
        }
        sps.window = {offsets[0] * across, offsets[1] * across, offsets[2] * down, offsets[3] * down};
        if (sps.window.left + sps.window.right >= sps.width || sps.window.top + sps.window.bottom >= sps.height)
            return std::nullopt;
    }
    sps.windowEndBit = reader.position();
    sps.bitDepth = reader.unsignedExpGolomb() + 8;
    const unsigned chromaDepth = reader.unsignedExpGolomb() + 8;
    sps.log2MaxPocLsb = reader.unsignedExpGolomb() + 4;
    if (sps.bitDepth > 16 || chromaDepth > 16 || sps.log2MaxPocLsb > 16) return std::nullopt;
    const bool perSubLayer = reader.bits(1) != 0;   // sps_sub_layer_ordering_info_present_flag
    for (unsigned i = perSubLayer ? 0 : subLayers; i <= subLayers; ++i) {
        reader.unsignedExpGolomb();   // sps_max_dec_pic_buffering_minus1
        sps.maxNumReorder = reader.unsignedExpGolomb();
        sps.maxLatencyIncreasePlus1 = reader.unsignedExpGolomb();
    }
    if (!reader.ok() || sps.maxNumReorder > 16) return std::nullopt;
    return sps;
}

std::optional<Pps> parsePps(Bytes nal)
{
    if (nal.size() < 4 || nalType(nal) != nalPps) return std::nullopt;
    const auto payload = bitstream::rbsp(nal, 2);
    BitReader reader(payload);
    Pps pps;
    pps.id = reader.unsignedExpGolomb();
    pps.spsId = reader.unsignedExpGolomb();
    reader.bits(1);   // dependent_slice_segments_enabled_flag
    pps.outputFlagPresent = reader.bits(1) != 0;
    pps.extraSliceHeaderBits = reader.bits(3);
    if (!reader.ok() || pps.id > 63 || pps.spsId > 15) return std::nullopt;
    return pps;
}

std::optional<std::vector<std::uint8_t>> withoutWindow(Bytes nal)
{
    const auto sps = parseSps(nal);
    if (!sps) return std::nullopt;
    if (!sps->hasWindow) return std::vector<std::uint8_t>(nal.begin(), nal.end());
    const auto payload = bitstream::rbsp(nal, 2);
    // The stop bit is the last bit set; everything before it is the syntax.
    std::size_t stop = payload.size() * 8;
    while (stop > 0 && !((payload[(stop - 1) / 8] >> (7 - (stop - 1) % 8)) & 1)) --stop;
    if (stop == 0 || stop - 1 < sps->windowEndBit) return std::nullopt;
    --stop;
    BitWriter writer;
    writer.copy(payload, 0, sps->windowFlagBit);
    writer.bit(false);   // conformance_window_flag
    writer.copy(payload, sps->windowEndBit, stop);
    writer.bit(true);    // rbsp_stop_one_bit
    const auto escaped = bitstream::escapeEmulation(writer.bytes());
    std::vector<std::uint8_t> out(nal.begin(), nal.begin() + 2);
    out.insert(out.end(), escaped.begin(), escaped.end());
    return out;
}

ParameterSets::Result ParameterSets::store(Bytes nal)
{
    if (nal.size() < 3 || layerId(nal) != 0) return Result::rejected;
    const auto replace = [&nal](std::map<unsigned, std::vector<std::uint8_t>>& units, unsigned id) {
        auto& saved = units[id];
        if (std::equal(saved.begin(), saved.end(), nal.begin(), nal.end())) return Result::unchanged;
        saved.assign(nal.begin(), nal.end());
        return Result::changed;
    };
    switch (nalType(nal)) {
    case nalVps: {
        const auto payload = bitstream::rbsp(nal, 2);
        if (payload.empty()) return Result::rejected;
        return replace(vpsUnits_, payload[0] >> 4);
    }
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
    for (const auto& [id, unit] : vpsUnits_) result.push_back(unit);
    for (const auto& [id, unit] : spsUnits_) {
        auto stripped = withoutWindow(unit);
        result.push_back(stripped ? std::move(*stripped) : unit);
    }
    for (const auto& [id, unit] : ppsUnits_) result.push_back(unit);
    return result;
}

void ParameterSets::clear()
{
    vpsUnits_.clear();
    spsUnits_.clear();
    ppsUnits_.clear();
    sps_.clear();
    pps_.clear();
}

std::optional<SliceStart> parseSliceStart(Bytes nal, const ParameterSets& sets)
{
    if (nal.size() < 3) return std::nullopt;
    const auto payload = bitstream::rbsp(nal, 2);
    BitReader reader(payload);
    SliceStart slice;
    slice.type = nalType(nal);
    slice.temporalId = temporalId(nal);
    slice.firstInPicture = reader.bits(1) != 0;
    if (!reader.ok()) return std::nullopt;
    if (!slice.firstInPicture) return slice;
    if (isIrap(slice.type)) reader.bits(1);   // no_output_of_prior_pics_flag
    slice.ppsId = reader.unsignedExpGolomb();
    const auto* pps = sets.pps(slice.ppsId);
    const auto* sps = pps ? sets.sps(pps->spsId) : nullptr;
    if (!sps) return std::nullopt;
    slice.spsId = sps->id;
    reader.bits(pps->extraSliceHeaderBits);   // slice_reserved_flag
    reader.unsignedExpGolomb();               // slice_type
    if (pps->outputFlagPresent) slice.output = reader.bits(1) != 0;
    if (sps->separateColourPlane) reader.bits(2);   // colour_plane_id
    if (slice.type != nalIdrWRadl && slice.type != nalIdrNLp)
        slice.pocLsb = reader.bits(sps->log2MaxPocLsb);
    if (!reader.ok()) return std::nullopt;
    return slice;
}

PictureOrder::Verdict PictureOrder::next(const SliceStart& slice, const Sps& sps)
{
    Verdict verdict;
    const bool irap = isIrap(slice.type);
    const bool idr = slice.type == nalIdrWRadl || slice.type == nalIdrNLp;
    if (irap) {
        // IDR and BLA pictures always begin a coded video sequence; a CRA
        // picture does when it is the first picture or follows an end of
        // sequence, and its leading pictures then lack their references.
        leadingDropped_ = idr || slice.type < nalCra || atStart_;
        verdict.startsSequence = leadingDropped_;
    }
    atStart_ = false;
    if ((slice.type == nalRaslN || slice.type == nalRaslR) && leadingDropped_) {
        verdict.decode = false;
        verdict.output = false;
        return verdict;
    }
    const unsigned lsb = idr ? 0 : slice.pocLsb;
    const int maxLsb = 1 << sps.log2MaxPocLsb;
    int msb = 0;
    if (!(irap && leadingDropped_)) {
        msb = prevMsb_;
        if (int(lsb) < int(prevLsb_) && int(prevLsb_) - int(lsb) >= maxLsb / 2) msb += maxLsb;
        else if (int(lsb) > int(prevLsb_) && int(lsb) - int(prevLsb_) > maxLsb / 2) msb -= maxLsb;
    }
    verdict.poc = msb + int(lsb);
    verdict.output = slice.output;
    // The reference for the next count is the previous picture of the lowest
    // sub-layer that is not a leading picture or a sub-layer non-reference one.
    const bool leading = slice.type >= 6 && slice.type <= nalRaslR;
    const bool nonReference = slice.type <= 14 && slice.type % 2 == 0;
    if (slice.temporalId == 0 && !leading && !nonReference) {
        prevLsb_ = lsb;
        prevMsb_ = msb;
    }
    return verdict;
}

void ReorderBuffer::add(Id id, int poc, unsigned maxNumReorder, unsigned maxLatencyIncreasePlus1,
                        std::vector<Id>& released)
{
    // A picture that waits behind this one in output order has now been
    // overtaken by one more picture.
    for (auto& entry : waiting_)
        if (entry.poc > poc) ++entry.latency;
    waiting_.push_back({id, poc, 0});
    const unsigned maxLatency = maxNumReorder + maxLatencyIncreasePlus1 - 1;
    const auto tooLate = [&] {
        return maxLatencyIncreasePlus1 != 0 &&
               std::any_of(waiting_.begin(), waiting_.end(),
                           [maxLatency](const Entry& entry) { return entry.latency >= maxLatency; });
    };
    while (!waiting_.empty() && (waiting_.size() > maxNumReorder || tooLate())) bump(released);
}

void ReorderBuffer::flush(std::vector<Id>& released)
{
    while (!waiting_.empty()) bump(released);
}

void ReorderBuffer::bump(std::vector<Id>& released)
{
    const auto first = std::min_element(waiting_.begin(), waiting_.end(),
                                        [](const Entry& a, const Entry& b) { return a.poc < b.poc; });
    released.push_back(first->id);
    waiting_.erase(first);
}

} // namespace uml::platform::hevc
