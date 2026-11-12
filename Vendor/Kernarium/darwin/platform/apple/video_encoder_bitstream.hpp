// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Annex B and parameter-set helpers of the Apple video encoder backend. They
// are plain C++ so that the checked-in decoder fixtures can test them without
// VideoToolbox, and so that discovery and the encoder read a coded stream the
// same way.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace uml::platform::bitstream {

using Bytes = std::span<const std::uint8_t>;

// The units of an Annex B byte stream, each without its start code and with its
// NAL unit header. Three- and four-byte start codes both separate units, and
// the zero bytes that lead a four-byte start code are not part of the unit
// before it.
[[nodiscard]] inline std::vector<Bytes> splitAnnexB(Bytes stream)
{
    std::vector<Bytes> units;
    const auto prefix = [stream](std::size_t at) -> std::size_t {
        if (at + 3 <= stream.size() && stream[at] == 0 && stream[at + 1] == 0 && stream[at + 2] == 1)
            return 3;
        return 0;
    };
    std::size_t begin = stream.size();
    for (std::size_t i = 0; i < stream.size();) {
        const auto length = prefix(i);
        if (!length) {
            ++i;
            continue;
        }
        if (begin < i) {
            auto end = i;
            while (end > begin && stream[end - 1] == 0) --end;   // a four-byte start code's leading zero
            if (end > begin) units.push_back(stream.subspan(begin, end - begin));
        }
        begin = i + length;
        i = begin;
    }
    if (begin < stream.size()) units.push_back(stream.subspan(begin));
    return units;
}

[[nodiscard]] inline unsigned h264NalType(Bytes nal) { return nal.empty() ? 0 : nal[0] & 0x1f; }
[[nodiscard]] inline unsigned hevcNalType(Bytes nal) { return nal.empty() ? 0 : (nal[0] >> 1) & 0x3f; }

inline constexpr unsigned h264NalSps = 7;
inline constexpr unsigned h264NalPps = 8;
inline constexpr unsigned h264NalIdr = 5;
inline constexpr unsigned hevcNalVps = 32;
inline constexpr unsigned hevcNalSps = 33;
inline constexpr unsigned hevcNalPps = 34;
inline constexpr unsigned hevcNalPrefixSei = 39;

// Raw byte sequence payload: the unit with its emulation prevention bytes
// removed, after `skip` header bytes.
[[nodiscard]] inline std::vector<std::uint8_t> rbsp(Bytes nal, std::size_t skip)
{
    std::vector<std::uint8_t> out;
    if (nal.size() <= skip) return out;
    out.reserve(nal.size() - skip);
    unsigned zeros = 0;
    for (std::size_t i = skip; i < nal.size(); ++i) {
        if (zeros >= 2 && nal[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = nal[i] == 0 ? zeros + 1 : 0;
        out.push_back(nal[i]);
    }
    return out;
}

class BitReader {
public:
    explicit BitReader(Bytes bytes) : bytes_(bytes) {}
    [[nodiscard]] bool ok() const noexcept { return !overrun_; }
    // Bits consumed so far.
    [[nodiscard]] std::size_t position() const noexcept { return position_; }
    std::uint32_t bits(unsigned count)
    {
        std::uint32_t value = 0;
        while (count-- > 0) {
            if (position_ >= bytes_.size() * 8) {
                overrun_ = true;
                return 0;
            }
            value = (value << 1) | ((bytes_[position_ / 8] >> (7 - position_ % 8)) & 1u);
            ++position_;
        }
        return value;
    }
    std::uint32_t unsignedExpGolomb()
    {
        unsigned zeros = 0;
        while (!overrun_ && bits(1) == 0) {
            if (++zeros > 31) {
                overrun_ = true;
                return 0;
            }
        }
        if (overrun_) return 0;
        return ((1u << zeros) - 1) + (zeros ? bits(zeros) : 0);
    }
    std::int32_t signedExpGolomb()
    {
        const std::uint32_t code = unsignedExpGolomb();
        return (code & 1) ? std::int32_t((code + 1) / 2) : -std::int32_t(code / 2);
    }
private:
    Bytes bytes_;
    std::size_t position_ {};
    bool overrun_ {};
};

struct H264SpsInfo {
    unsigned profileIdc {};
    unsigned constraintFlags {};   // the byte after profile_idc: constraint_set0..5 and reserved bits
    unsigned levelIdc {};
};

// The SPS NAL unit (with its header byte).
[[nodiscard]] inline std::optional<H264SpsInfo> parseH264Sps(Bytes nal)
{
    if (nal.size() < 4 || h264NalType(nal) != h264NalSps) return std::nullopt;
    return H264SpsInfo {nal[1], nal[2], nal[3]};
}

// Everything the level limits of H.264 Annex A read from an SPS.
struct H264SpsLimits {
    unsigned profileIdc {};
    unsigned constraintFlags {};
    unsigned levelIdc {};
    unsigned pocType {};
    unsigned maxNumRefFrames {};
    unsigned widthMacroblocks {};
    unsigned heightMacroblocks {};   // of a frame
    bool frameMbsOnly {};
    bool vuiPresent {};
    bool timingInfo {};
    std::uint32_t numUnitsInTick {};
    std::uint32_t timeScale {};
    // HRD parameters of either kind. The largest bit rate and CPB size stated.
    bool hrd {};
    std::uint64_t hrdBitRate {};
    std::uint64_t hrdCpbSize {};
    bool bitstreamRestriction {};
    unsigned log2MaxMvLengthVertical {};
    unsigned maxNumReorderFrames {};
    unsigned maxDecFrameBuffering {};
};

namespace detail {
inline void skipH264ScalingList(BitReader& reader, unsigned size)
{
    int last = 8, next = 8;
    for (unsigned j = 0; j < size && reader.ok(); ++j) {
        if (next != 0) next = (last + reader.signedExpGolomb() + 256) % 256;
        last = next == 0 ? last : next;
    }
}

// hrd_parameters(); false for a count the syntax does not allow.
inline bool readH264Hrd(BitReader& reader, H264SpsLimits& limits)
{
    const unsigned schedules = reader.unsignedExpGolomb() + 1;
    const unsigned bitRateScale = reader.bits(4), cpbSizeScale = reader.bits(4);
    if (schedules > 32) return false;
    limits.hrd = true;
    for (unsigned i = 0; i < schedules && reader.ok(); ++i) {
        const std::uint64_t rate = (std::uint64_t(reader.unsignedExpGolomb()) + 1) << (6 + bitRateScale);
        const std::uint64_t size = (std::uint64_t(reader.unsignedExpGolomb()) + 1) << (4 + cpbSizeScale);
        reader.bits(1);   // cbr_flag
        limits.hrdBitRate = std::max(limits.hrdBitRate, rate);
        limits.hrdCpbSize = std::max(limits.hrdCpbSize, size);
    }
    reader.bits(20);   // the four delay lengths
    return true;
}
} // namespace detail

// The SPS NAL unit (with its header byte) read to the end of its VUI. nullopt
// for a unit it cannot walk.
[[nodiscard]] inline std::optional<H264SpsLimits> parseH264SpsLimits(Bytes nal)
{
    if (nal.size() < 4 || h264NalType(nal) != h264NalSps) return std::nullopt;
    const auto payload = rbsp(nal, 1);
    BitReader reader(payload);
    H264SpsLimits limits;
    limits.profileIdc = reader.bits(8);
    limits.constraintFlags = reader.bits(8);
    limits.levelIdc = reader.bits(8);
    reader.unsignedExpGolomb();   // seq_parameter_set_id
    switch (limits.profileIdc) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118: case 128: case 138: case 139: case 134: case 135: {
        const unsigned chroma = reader.unsignedExpGolomb();
        if (chroma == 3) reader.bits(1);
        reader.unsignedExpGolomb();   // bit_depth_luma_minus8
        reader.unsignedExpGolomb();   // bit_depth_chroma_minus8
        reader.bits(1);               // qpprime_y_zero_transform_bypass_flag
        if (reader.bits(1)) {         // seq_scaling_matrix_present_flag
            for (unsigned i = 0; i < (chroma != 3 ? 8u : 12u) && reader.ok(); ++i)
                if (reader.bits(1)) detail::skipH264ScalingList(reader, i < 6 ? 16 : 64);
        }
        break;
    }
    default: break;
    }
    reader.unsignedExpGolomb();   // log2_max_frame_num_minus4
    limits.pocType = reader.unsignedExpGolomb();
    if (limits.pocType == 0) {
        reader.unsignedExpGolomb();
    } else if (limits.pocType == 1) {
        reader.bits(1);
        reader.signedExpGolomb();
        reader.signedExpGolomb();
        const unsigned cycle = reader.unsignedExpGolomb();
        if (cycle > 255) return std::nullopt;
        for (unsigned i = 0; i < cycle && reader.ok(); ++i) reader.signedExpGolomb();
    }
    limits.maxNumRefFrames = reader.unsignedExpGolomb();
    reader.bits(1);   // gaps_in_frame_num_value_allowed_flag
    limits.widthMacroblocks = reader.unsignedExpGolomb() + 1;
    const unsigned mapUnits = reader.unsignedExpGolomb() + 1;
    limits.frameMbsOnly = reader.bits(1) != 0;
    if (!limits.frameMbsOnly) reader.bits(1);   // mb_adaptive_frame_field_flag
    limits.heightMacroblocks = mapUnits * (limits.frameMbsOnly ? 1 : 2);
    reader.bits(1);   // direct_8x8_inference_flag
    if (reader.bits(1)) for (int i = 0; i < 4; ++i) reader.unsignedExpGolomb();   // frame cropping
    limits.vuiPresent = reader.bits(1) != 0;
    if (!reader.ok()) return std::nullopt;
    if (!limits.vuiPresent) return limits;
    if (reader.bits(1) && reader.bits(8) == 255) reader.bits(32);   // aspect ratio
    if (reader.bits(1)) reader.bits(1);                              // overscan
    if (reader.bits(1)) {                                            // video_signal_type_present_flag
        reader.bits(4);                                              // video_format, video_full_range_flag
        if (reader.bits(1)) reader.bits(24);                         // colour description
    }
    if (reader.bits(1)) {                                            // chroma_loc_info_present_flag
        reader.unsignedExpGolomb();
        reader.unsignedExpGolomb();
    }
    if (reader.bits(1)) {                                            // timing_info_present_flag
        limits.timingInfo = true;
        limits.numUnitsInTick = reader.bits(32);
        limits.timeScale = reader.bits(32);
        reader.bits(1);                                              // fixed_frame_rate_flag
    }
    const bool nalHrd = reader.bits(1) != 0;
    if (nalHrd && !detail::readH264Hrd(reader, limits)) return std::nullopt;
    const bool vclHrd = reader.bits(1) != 0;
    if (vclHrd && !detail::readH264Hrd(reader, limits)) return std::nullopt;
    if (nalHrd || vclHrd) reader.bits(1);                            // low_delay_hrd_flag
    reader.bits(1);                                                  // pic_struct_present_flag
    limits.bitstreamRestriction = reader.bits(1) != 0;
    if (limits.bitstreamRestriction) {
        reader.bits(1);                                              // motion_vectors_over_pic_boundaries_flag
        reader.unsignedExpGolomb();                                  // max_bytes_per_pic_denom
        reader.unsignedExpGolomb();                                  // max_bits_per_mb_denom
        reader.unsignedExpGolomb();                                  // log2_max_mv_length_horizontal
        limits.log2MaxMvLengthVertical = reader.unsignedExpGolomb();
        limits.maxNumReorderFrames = reader.unsignedExpGolomb();
        limits.maxDecFrameBuffering = reader.unsignedExpGolomb();
    }
    if (!reader.ok()) return std::nullopt;
    return limits;
}

struct HevcSpsInfo {
    unsigned profileIdc {};
    bool highTier {};
    unsigned levelIdc {};   // general_level_idc: 30 times the level number
};

// The SPS NAL unit (with its two-byte header).
[[nodiscard]] inline std::optional<HevcSpsInfo> parseHevcSps(Bytes nal)
{
    if (nal.size() < 4 || hevcNalType(nal) != hevcNalSps) return std::nullopt;
    const auto payload = rbsp(nal, 2);
    // sps_video_parameter_set_id and the sub-layer count share the first byte;
    // profile_tier_level follows: one byte of space, tier and profile, four of
    // compatibility flags and six of constraint flags, then the level.
    if (payload.size() < 1 + 1 + 4 + 6 + 1) return std::nullopt;
    const auto profile = payload[1];
    return HevcSpsInfo {unsigned(profile & 0x1f), (profile & 0x20) != 0, payload[12]};
}

struct HevcSpsFormat {
    unsigned chromaFormat {};   // chroma_format_idc: 1 is 4:2:0
    unsigned bitDepthLuma {};
    unsigned bitDepthChroma {};
    bool vuiPresent {};
    bool colorDescription {};   // colour_primaries, transfer_characteristics and matrix_coeffs are stated
    unsigned primaries {};
    unsigned transfer {};
    unsigned matrix {};
    bool fullRange {};
    // What the level limits of H.265 Annex A read.
    bool highTier {};
    unsigned levelIdc {};
    unsigned maxSubLayersMinus1 {};
    bool subLayerLevels {};          // sub-layers state profile or level of their own
    unsigned width {};               // coded picture size in luma samples
    unsigned height {};
    unsigned maxDecPicBuffering {};  // of the highest sub-layer
    unsigned maxNumReorder {};
    bool timingInfo {};
    std::uint32_t numUnitsInTick {};
    std::uint32_t timeScale {};
    bool hrd {};                     // the VUI carries hrd_parameters, which are not walked
};

// The bit depths, the picture size, the DPB size and the VUI colour
// description of an HEVC SPS NAL unit (with its two-byte header). nullopt for
// a stream it cannot walk to the VUI: one with scaling list data, which the
// hardware encoder does not write.
[[nodiscard]] inline std::optional<HevcSpsFormat> parseHevcSpsFormat(Bytes nal)
{
    if (nal.size() < 4 || hevcNalType(nal) != hevcNalSps) return std::nullopt;
    const auto payload = rbsp(nal, 2);
    BitReader reader(payload);
    reader.bits(4);   // sps_video_parameter_set_id
    const unsigned subLayers = reader.bits(3);
    reader.bits(1);   // sps_temporal_id_nesting_flag
    // profile_tier_level: the general part is 88 bits and the level 8; every
    // sub-layer then adds a profile of 88 bits and a level of 8 when present.
    HevcSpsFormat format;
    format.maxSubLayersMinus1 = subLayers;
    format.highTier = (reader.bits(32) >> 29 & 1) != 0;
    reader.bits(32);
    format.levelIdc = reader.bits(32) & 0xff;
    if (subLayers > 7) return std::nullopt;
    bool subProfile[8] = {}, subLevel[8] = {};
    for (unsigned i = 0; i < subLayers; ++i) {
        subProfile[i] = reader.bits(1) != 0;
        subLevel[i] = reader.bits(1) != 0;
        format.subLayerLevels = format.subLayerLevels || subProfile[i] || subLevel[i];
    }
    if (subLayers > 0)
        for (unsigned i = subLayers; i < 8; ++i) reader.bits(2);
    for (unsigned i = 0; i < subLayers; ++i) {
        if (subProfile[i]) for (unsigned part = 0; part < 11; ++part) reader.bits(8);
        if (subLevel[i]) reader.bits(8);
    }
    reader.unsignedExpGolomb();   // sps_seq_parameter_set_id
    format.chromaFormat = reader.unsignedExpGolomb();
    if (format.chromaFormat == 3) reader.bits(1);
    format.width = reader.unsignedExpGolomb();
    format.height = reader.unsignedExpGolomb();
    if (reader.bits(1)) for (int i = 0; i < 4; ++i) reader.unsignedExpGolomb();   // conformance window
    format.bitDepthLuma = reader.unsignedExpGolomb() + 8;
    format.bitDepthChroma = reader.unsignedExpGolomb() + 8;
    const unsigned pocBits = reader.unsignedExpGolomb() + 4;
    const bool orderingPerLayer = reader.bits(1) != 0;
    for (unsigned i = orderingPerLayer ? 0 : subLayers; i <= subLayers; ++i) {
        format.maxDecPicBuffering = reader.unsignedExpGolomb() + 1;
        format.maxNumReorder = reader.unsignedExpGolomb();
        reader.unsignedExpGolomb();   // sps_max_latency_increase_plus1
    }
    for (int i = 0; i < 4; ++i) reader.unsignedExpGolomb();   // coding and transform block sizes
    reader.unsignedExpGolomb();                               // max_transform_hierarchy_depth_inter
    reader.unsignedExpGolomb();                               // max_transform_hierarchy_depth_intra
    if (reader.bits(1) && reader.bits(1)) return std::nullopt;   // scaling_list_data
    reader.bits(2);                                           // amp, sample adaptive offset
    if (reader.bits(1)) {                                     // pcm
        reader.bits(8);
        reader.unsignedExpGolomb();
        reader.unsignedExpGolomb();
        reader.bits(1);
    }
    const unsigned sets = reader.unsignedExpGolomb();
    if (sets > 64) return std::nullopt;
    struct Set { std::vector<int> negative, positive; };   // DeltaPocS0, DeltaPocS1
    std::vector<Set> rps(sets);
    for (unsigned index = 0; index < sets && reader.ok(); ++index) {
        Set& set = rps[index];
        if (index != 0 && reader.bits(1)) {   // inter_ref_pic_set_prediction_flag, with delta_idx_minus1 = 0
            const Set& ref = rps[index - 1];
            const int sign = reader.bits(1) ? -1 : 1;
            const int deltaRps = sign * int(reader.unsignedExpGolomb() + 1);
            const std::size_t count = ref.negative.size() + ref.positive.size() + 1;
            std::vector<bool> use(count, true);
            for (std::size_t j = 0; j < count; ++j)
                if (!reader.bits(1)) use[j] = reader.bits(1) != 0;
            const std::size_t negatives = ref.negative.size();
            for (std::size_t j = ref.positive.size(); j-- > 0;) {
                const int delta = ref.positive[j] + deltaRps;
                if (delta < 0 && use[negatives + j]) set.negative.push_back(delta);
            }
            if (deltaRps < 0 && use[count - 1]) set.negative.push_back(deltaRps);
            for (std::size_t j = 0; j < negatives; ++j) {
                const int delta = ref.negative[j] + deltaRps;
                if (delta < 0 && use[j]) set.negative.push_back(delta);
            }
            for (std::size_t j = negatives; j-- > 0;) {
                const int delta = ref.negative[j] + deltaRps;
                if (delta > 0 && use[j]) set.positive.push_back(delta);
            }
            if (deltaRps > 0 && use[count - 1]) set.positive.push_back(deltaRps);
            for (std::size_t j = 0; j < ref.positive.size(); ++j) {
                const int delta = ref.positive[j] + deltaRps;
                if (delta > 0 && use[negatives + j]) set.positive.push_back(delta);
            }
        } else {
            const unsigned negatives = reader.unsignedExpGolomb();
            const unsigned positives = reader.unsignedExpGolomb();
            if (negatives > 16 || positives > 16) return std::nullopt;
            int poc = 0;
            for (unsigned j = 0; j < negatives; ++j) {
                poc -= int(reader.unsignedExpGolomb()) + 1;
                reader.bits(1);
                set.negative.push_back(poc);
            }
            poc = 0;
            for (unsigned j = 0; j < positives; ++j) {
                poc += int(reader.unsignedExpGolomb()) + 1;
                reader.bits(1);
                set.positive.push_back(poc);
            }
        }
    }
    if (reader.bits(1)) {   // long_term_ref_pics_present_flag
        const unsigned longTerm = reader.unsignedExpGolomb();
        if (longTerm > 32) return std::nullopt;
        for (unsigned i = 0; i < longTerm; ++i) {
            reader.bits(pocBits);
            reader.bits(1);
        }
    }
    reader.bits(2);   // sps_temporal_mvp_enabled_flag, strong_intra_smoothing_enabled_flag
    format.vuiPresent = reader.bits(1) != 0;
    if (!reader.ok()) return std::nullopt;
    if (!format.vuiPresent) return format;
    if (reader.bits(1) && reader.bits(8) == 255) reader.bits(32);   // aspect ratio
    if (reader.bits(1)) reader.bits(1);                              // overscan
    if (reader.bits(1)) {                                            // video_signal_type_present_flag
        reader.bits(3);                                              // video_format
        format.fullRange = reader.bits(1) != 0;
        if (reader.bits(1)) {
            format.colorDescription = true;
            format.primaries = reader.bits(8);
            format.transfer = reader.bits(8);
            format.matrix = reader.bits(8);
        }
    }
    if (reader.bits(1)) {                                            // chroma_loc_info_present_flag
        reader.unsignedExpGolomb();
        reader.unsignedExpGolomb();
    }
    reader.bits(3);                                                  // neutral chroma, field_seq, frame_field_info
    if (reader.bits(1)) for (int i = 0; i < 4; ++i) reader.unsignedExpGolomb();   // default display window
    if (reader.bits(1)) {                                            // vui_timing_info_present_flag
        format.timingInfo = true;
        format.numUnitsInTick = reader.bits(32);
        format.timeScale = reader.bits(32);
        if (reader.bits(1)) reader.unsignedExpGolomb();              // poc_proportional_to_timing_flag
        format.hrd = reader.bits(1) != 0;                            // vui_hrd_parameters_present_flag
    }
    if (!reader.ok()) return std::nullopt;
    return format;
}

struct HevcPpsInfo {
    bool tilesEnabled {};
};

// The PPS NAL unit (with its two-byte header) read to tiles_enabled_flag.
[[nodiscard]] inline std::optional<HevcPpsInfo> parseHevcPps(Bytes nal)
{
    if (nal.size() < 4 || hevcNalType(nal) != hevcNalPps) return std::nullopt;
    const auto payload = rbsp(nal, 2);
    BitReader reader(payload);
    reader.unsignedExpGolomb();   // pps_pic_parameter_set_id
    reader.unsignedExpGolomb();   // pps_seq_parameter_set_id
    reader.bits(1);               // dependent_slice_segments_enabled_flag
    reader.bits(1);               // output_flag_present_flag
    reader.bits(3);               // num_extra_slice_header_bits
    reader.bits(2);               // sign_data_hiding_enabled_flag, cabac_init_present_flag
    reader.unsignedExpGolomb();   // num_ref_idx_l0_default_active_minus1
    reader.unsignedExpGolomb();   // num_ref_idx_l1_default_active_minus1
    reader.signedExpGolomb();     // init_qp_minus26
    reader.bits(2);               // constrained_intra_pred_flag, transform_skip_enabled_flag
    if (reader.bits(1)) reader.unsignedExpGolomb();   // cu_qp_delta_enabled_flag, diff_cu_qp_delta_depth
    reader.signedExpGolomb();     // pps_cb_qp_offset
    reader.signedExpGolomb();     // pps_cr_qp_offset
    reader.bits(4);               // slice chroma offsets, weighted prediction (two), transquant bypass
    HevcPpsInfo info;
    info.tilesEnabled = reader.bits(1) != 0;
    if (!reader.ok()) return std::nullopt;
    return info;
}

// entropy_coding_mode_flag of an H.264 PPS NAL unit: true for CABAC.
[[nodiscard]] inline std::optional<bool> h264Cabac(Bytes nal)
{
    if (nal.size() < 3 || h264NalType(nal) != h264NalPps) return std::nullopt;
    const auto payload = rbsp(nal, 1);
    BitReader reader(payload);
    reader.unsignedExpGolomb();   // pic_parameter_set_id
    reader.unsignedExpGolomb();   // seq_parameter_set_id
    const bool cabac = reader.bits(1) != 0;
    if (!reader.ok()) return std::nullopt;
    return cabac;
}

struct SeiMessage {
    unsigned type {};
    std::vector<std::uint8_t> payload;
};

// The messages of an HEVC SEI NAL unit.
[[nodiscard]] inline std::vector<SeiMessage> hevcSeiMessages(Bytes nal)
{
    std::vector<SeiMessage> messages;
    if (nal.size() < 4) return messages;
    const auto payload = rbsp(nal, 2);
    std::size_t at = 0;
    while (at + 2 <= payload.size() && payload[at] != 0x80) {
        unsigned type = 0, size = 0;
        while (at < payload.size() && payload[at] == 0xff) { type += 255; ++at; }
        if (at >= payload.size()) break;
        type += payload[at++];
        while (at < payload.size() && payload[at] == 0xff) { size += 255; ++at; }
        if (at >= payload.size()) break;
        size += payload[at++];
        if (at + size > payload.size()) break;
        messages.push_back({type, {payload.begin() + std::ptrdiff_t(at), payload.begin() + std::ptrdiff_t(at + size)}});
        at += size;
    }
    return messages;
}

// The unit's payload with emulation prevention bytes inserted, so that no
// three-byte start code prefix appears inside it.
[[nodiscard]] inline std::vector<std::uint8_t> escapeEmulation(Bytes payload)
{
    std::vector<std::uint8_t> out;
    out.reserve(payload.size() + payload.size() / 64 + 1);
    unsigned zeros = 0;
    for (const auto byte : payload) {
        if (zeros >= 2 && byte <= 3) {
            out.push_back(3);
            zeros = 0;
        }
        out.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }
    return out;
}

// The level an H.264 SPS NAL unit (with its header byte) states, as level_idc.
// Level 1b is 9: a Baseline, Main or Extended stream writes it as level 1.1
// with constraint_set3_flag set, and the other profiles as level_idc 9.
[[nodiscard]] inline std::optional<unsigned> h264Level(Bytes nal)
{
    if (nal.size() < 4 || h264NalType(nal) != h264NalSps) return std::nullopt;
    const bool oneB = nal[3] == 11 && (nal[2] & 0x10) && (nal[1] == 66 || nal[1] == 77 || nal[1] == 88);
    return oneB ? 9u : unsigned(nal[3]);
}

// States `level` in an H.264 SPS, higher or lower than it stated. Level 1b is
// written as level_idc 9 and, for Baseline, Main and Extended, as level 1.1
// with constraint_set3_flag; leaving it clears the flag. False, and the unit
// untouched, when it already states `level` or is not an SPS.
inline bool setH264Level(std::vector<std::uint8_t>& nal, unsigned level)
{
    const auto current = h264Level(nal);
    if (!current || *current == level || level > 255) return false;
    const bool flagged = nal[1] == 66 || nal[1] == 77 || nal[1] == 88;
    if (*current == 9 && nal[3] == 11) nal[2] = std::uint8_t(nal[2] & ~0x10u);   // no longer level 1b
    if (level == 9 && flagged) {
        nal[3] = 11;
        nal[2] = std::uint8_t(nal[2] | 0x10u);
    } else {
        nal[3] = std::uint8_t(level);
    }
    return true;
}

// general_level_idc sits at a fixed place in the profile_tier_level of an HEVC
// VPS (after four bytes of header fields) and SPS (after one), counted in
// payload bytes without emulation prevention.
[[nodiscard]] inline std::optional<std::size_t> hevcLevelOffset(Bytes nal)
{
    switch (hevcNalType(nal)) {
    case hevcNalVps: return 4 + 11;
    case hevcNalSps: return 1 + 11;
    default: return std::nullopt;
    }
}

// The general_level_idc an HEVC VPS or SPS NAL unit (with its two-byte header)
// states.
[[nodiscard]] inline std::optional<unsigned> hevcLevel(Bytes nal)
{
    const auto offset = hevcLevelOffset(nal);
    if (!offset || nal.size() < 4) return std::nullopt;
    const auto payload = rbsp(nal, 2);
    if (payload.size() <= *offset) return std::nullopt;
    return unsigned(payload[*offset]);
}

// States `level` in an HEVC VPS or SPS, as setH264Level does for H.264.
inline bool setHevcLevel(std::vector<std::uint8_t>& nal, unsigned level)
{
    const auto offset = hevcLevelOffset(nal);
    if (!offset || nal.size() < 4 || level > 255) return false;
    auto payload = rbsp(nal, 2);
    if (payload.size() <= *offset || payload[*offset] == level) return false;
    payload[*offset] = std::uint8_t(level);
    const auto escaped = escapeEmulation(payload);
    nal.resize(2);
    nal.insert(nal.end(), escaped.begin(), escaped.end());
    return true;
}

// Appends `nal` to `out` behind a four-byte start code.
inline void appendAnnexB(std::vector<std::uint8_t>& out, const std::uint8_t* nal, std::size_t size)
{
    static constexpr std::uint8_t startCode[4] = {0, 0, 0, 1};
    out.insert(out.end(), startCode, startCode + 4);
    out.insert(out.end(), nal, nal + size);
}

} // namespace uml::platform::bitstream
