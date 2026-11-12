// SPDX-License-Identifier: GPL-2.0-or-later
#include "apple/video_encoder_bitstream.hpp"
#include "apple/video_encoder_levels.hpp"
#include "require.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

/* The Annex B, parameter-set and SEI helpers of the Apple video encoder
 * backend, on the streams the decoder tests check in and on units built here.
 * Nothing in this binary touches VideoToolbox. */

namespace {
using namespace uml::platform::bitstream;
using namespace uml::platform;
using uml::testing::require;

std::string fixtureDirectory;

std::vector<std::uint8_t> read(const std::string& name)
{
    std::ifstream file(fixtureDirectory + "/" + name, std::ios::binary);
    if (!file) throw std::runtime_error("missing video fixture: " + fixtureDirectory + "/" + name);
    return {std::istreambuf_iterator<char>(file), {}};
}

using Unit = std::vector<std::uint8_t>;

std::vector<std::uint8_t> join(std::initializer_list<std::vector<std::uint8_t>> parts)
{
    std::vector<std::uint8_t> out;
    for (const auto& part : parts) out.insert(out.end(), part.begin(), part.end());
    return out;
}

void splitting()
{
    const Unit a {0x67, 1, 2, 3}, b {0x68, 4}, c {0x65, 0xff, 0x00, 0x7f};
    // Three- and four-byte start codes, and a unit whose last byte is zero
    // data that belongs to it only when no start code follows.
    const auto stream = join({{0, 0, 0, 1}, a, {0, 0, 1}, b, {0, 0, 0, 1}, c});
    const auto units = splitAnnexB(stream);
    require(units.size() == 3, "three units expected");
    require(Unit(units[0].begin(), units[0].end()) == a, "first unit");
    require(Unit(units[1].begin(), units[1].end()) == b, "second unit");
    require(Unit(units[2].begin(), units[2].end()) == c, "last unit keeps its own bytes");

    require(splitAnnexB({}).empty(), "empty stream has no units");
    const Unit onlyZeros {0, 0, 0, 0, 0};
    require(splitAnnexB(onlyZeros).empty(), "zeros are not a unit");
    const Unit leading {0, 0, 0, 0, 1, 0x09, 0x10};
    const auto lead = splitAnnexB(leading);
    require(lead.size() == 1 && lead[0].size() == 2 && lead[0][0] == 0x09, "leading zero bytes before a start code");

    // A unit's NAL header decides its type for both codecs.
    require(h264NalType(a) == h264NalSps && h264NalType(b) == h264NalPps && h264NalType(c) == h264NalIdr, "H.264 types");
    const Unit vps {0x40, 0x01, 0xaa}, sps {0x42, 0x01, 0xbb}, pps {0x44, 0x01, 0xcc}, sei {0x4e, 0x01, 0xdd};
    require(hevcNalType(vps) == hevcNalVps && hevcNalType(sps) == hevcNalSps && hevcNalType(pps) == hevcNalPps &&
            hevcNalType(sei) == hevcNalPrefixSei, "HEVC types");
}

void emulationPrevention()
{
    const Unit escaped {0x00, 0x00, 0x03, 0x01, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03};
    const auto plain = rbsp(escaped, 0);
    require(plain == Unit({0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00}), "prevention bytes are removed");
    require(rbsp(Unit {0x67, 0x00, 0x00, 0x03, 0x02}, 1) == Unit({0x00, 0x00, 0x02}), "header bytes are skipped");
    require(rbsp(Unit {0x67}, 4).empty(), "skipping past the unit leaves nothing");

    // The escape of any payload has no start-code prefix in it and reads back
    // as the payload.
    std::uint32_t state = 7;
    for (unsigned round = 0; round < 200; ++round) {
        Unit payload;
        for (unsigned i = 0; i < 64; ++i) {
            state = state * 1664525u + 1013904223u;
            // Favour the bytes that make a prefix.
            const unsigned pick = (state >> 24) & 7;
            payload.push_back(pick < 5 ? 0 : std::uint8_t(pick - 4));
        }
        const auto escapedPayload = escapeEmulation(payload);
        for (std::size_t i = 2; i < escapedPayload.size(); ++i)
            require(!(escapedPayload[i - 2] == 0 && escapedPayload[i - 1] == 0 && escapedPayload[i] <= 2), "escape left a prefix");
        require(rbsp(escapedPayload, 0) == payload, "escape does not read back");
    }
    // A payload that ends in zeros is not left to look like the start of a
    // start code: the trailing byte of an RBSP is nonzero, which callers add.
    const Unit trailing {0x01, 0x00, 0x00, 0x80};
    require(escapeEmulation(trailing) == trailing, "no escape is needed before a high byte");
}

void bitReading()
{
    const Unit bytes {0b1010'0110, 0b0100'0000};   // 1 | 010 | 011 | 00100 ...
    BitReader reader(bytes);
    require(reader.unsignedExpGolomb() == 0, "1 is 0");
    require(reader.unsignedExpGolomb() == 1, "010 is 1");
    require(reader.unsignedExpGolomb() == 2, "011 is 2");
    require(reader.unsignedExpGolomb() == 3, "00100 is 3");
    require(reader.ok(), "no overrun yet");
    BitReader plain(bytes);
    require(plain.bits(4) == 0b1010 && plain.bits(8) == 0b0110'0100 && plain.bits(4) == 0, "fixed-width reads");
    require(plain.ok(), "exactly the available bits");
    (void)plain.bits(1);
    require(!plain.ok(), "reading past the end is reported");
    const Unit zeros(8, 0);
    BitReader endless(zeros);
    (void)endless.unsignedExpGolomb();
    require(!endless.ok(), "a run of zeros without a one overruns");
}

void seiMessages()
{
    // Types and sizes of 255 and above use the 0xff continuation bytes.
    Unit payload;
    payload.insert(payload.end(), {137, 24});
    for (unsigned i = 0; i < 24; ++i) payload.push_back(std::uint8_t(i));
    payload.insert(payload.end(), {144, 4, 0x03, 0xe8, 0x01, 0xf4});
    payload.insert(payload.end(), {0xff, 0x05, 0xff, 0x02});   // type 260, size 257
    for (unsigned i = 0; i < 257; ++i) payload.push_back(std::uint8_t(i % 3 == 0 ? 0 : i));
    payload.push_back(0x80);
    Unit unit {0x4e, 0x01};
    const auto escaped = escapeEmulation(payload);
    unit.insert(unit.end(), escaped.begin(), escaped.end());

    const auto messages = hevcSeiMessages(unit);
    require(messages.size() == 3, "three messages");
    require(messages[0].type == 137 && messages[0].payload.size() == 24 && messages[0].payload[23] == 23, "mastering display message");
    require(messages[1].type == 144 && messages[1].payload == Unit({0x03, 0xe8, 0x01, 0xf4}), "content light message");
    require(messages[2].type == 260 && messages[2].payload.size() == 257, "extended type and size");
    // A message that claims more than the unit holds ends the list.
    Unit truncated {0x4e, 0x01, 137, 30, 1, 2, 3, 0x80};
    require(hevcSeiMessages(truncated).empty(), "a truncated message is not returned");
    require(hevcSeiMessages(Unit {0x4e, 0x01}).empty(), "a unit with no payload has no messages");
}

void fixtures()
{
    struct H264Case { const char* name; unsigned profile; unsigned constraints; bool cabac; };
    const H264Case h264[] = {
        {"h264-baseline", 66, 0x80, false},
        {"h264-constrained-baseline", 66, 0xc0, false},
        {"h264-main", 77, 0x40, true},
        {"h264-high", 100, 0x00, true},
        {"h264-high10", 110, 0x00, true},
    };
    for (const auto& c : h264) {
        const auto stream = read(std::string(c.name) + ".bin");
        const auto units = splitAnnexB(stream);
        require(units.size() >= 3, "access unit has parameter sets and a picture");
        bool sps = false, pps = false, picture = false;
        for (const auto unit : units) {
            if (const auto info = parseH264Sps(unit)) {
                sps = true;
                require(info->profileIdc == c.profile, "SPS profile_idc");
                require((info->constraintFlags & 0xc0) == (c.constraints & 0xc0), "SPS constraint_set0/1 flags");
            } else if (const auto cabac = h264Cabac(unit)) {
                pps = true;
                std::printf("%s: entropy %s\n", c.name, *cabac ? "CABAC" : "CAVLC");
                require(*cabac == c.cabac, "PPS entropy_coding_mode_flag");
            } else if (h264NalType(unit) == h264NalIdr) {
                picture = true;
            }
        }
        require(sps && pps && picture, "SPS, PPS and an IDR picture");
        // Annex B round trip through the appender.
        std::vector<std::uint8_t> rebuilt;
        for (const auto unit : units) appendAnnexB(rebuilt, unit.data(), unit.size());
        const auto again = splitAnnexB(rebuilt);
        require(again.size() == units.size(), "appending keeps the unit count");
        for (std::size_t i = 0; i < units.size(); ++i)
            require(Unit(again[i].begin(), again[i].end()) == Unit(units[i].begin(), units[i].end()), "appending keeps the unit");
    }

    struct HevcCase { const char* name; unsigned profile; };
    for (const auto& c : {HevcCase {"hevc-main", 1}, HevcCase {"hevc-main10", 2}}) {
        const auto stream = read(std::string(c.name) + ".bin");
        const auto units = splitAnnexB(stream);
        bool vps = false, sps = false, pps = false;
        for (const auto unit : units) {
            vps = vps || hevcNalType(unit) == hevcNalVps;
            pps = pps || hevcNalType(unit) == hevcNalPps;
            if (const auto info = parseHevcSps(unit)) {
                sps = true;
                std::printf("%s: level_idc %u tier %d\n", c.name, info->levelIdc, int(info->highTier));
                require(info->profileIdc == c.profile, "HEVC general_profile_idc");
                require(!info->highTier, "main tier");
                require(info->levelIdc >= 30 && info->levelIdc <= 186, "level_idc names a level");
            }
        }
        require(vps && sps && pps, "VPS, SPS and PPS");
        for (const auto unit : units) {
            if (hevcNalType(unit) != hevcNalSps) continue;
            const auto format = parseHevcSpsFormat(unit);
            require(format.has_value(), "the fixture's SPS can be read to its VUI");
            std::printf("%s: %u-bit, chroma format %u, VUI %d\n", c.name, format->bitDepthLuma, format->chromaFormat, int(format->vuiPresent));
            require(format->chromaFormat == 1, "4:2:0");
            require(format->bitDepthLuma == (c.profile == 2 ? 10u : 8u) && format->bitDepthChroma == format->bitDepthLuma, "bit depth");
        }
    }

    // Units of the wrong kind or too short to read are refused, not guessed at.
    require(!parseH264Sps(Unit {0x68, 1, 2, 3}), "PPS is not an SPS");
    require(!parseH264Sps(Unit {0x67, 1}), "truncated SPS");
    require(!parseHevcSps(Unit {0x40, 0x01, 1, 2, 3}), "VPS is not an SPS");
    require(!parseHevcSps(Unit {0x42, 0x01, 1, 2, 3}), "truncated HEVC SPS");
    require(!h264Cabac(Unit {0x67, 1, 2}), "SPS is not a PPS");
}

// Builds RBSP bit by bit, so that an SPS can use syntax the hardware encoder does
// not write.
class BitWriter {
public:
    void bits(std::uint64_t value, unsigned count)
    {
        while (count-- > 0) put((value >> count) & 1u);
    }
    void ue(std::uint32_t value)
    {
        const std::uint64_t coded = std::uint64_t(value) + 1;
        unsigned length = 0;
        while ((coded >> (length + 1)) != 0) ++length;
        bits(0, length);
        bits(coded, length + 1);
    }
    void se(std::int32_t value) { ue(value > 0 ? std::uint32_t(2 * value - 1) : std::uint32_t(-2 * value)); }
    // The unit: a NAL header, then the payload with its stop bit, padded and
    // with emulation prevention bytes.
    Unit unit(Unit header)
    {
        put(1);
        while (count_ % 8 != 0) put(0);
        const auto escaped = escapeEmulation(bytes_);
        header.insert(header.end(), escaped.begin(), escaped.end());
        return header;
    }
    Unit sps() { return unit({0x42, 0x01}); }
private:
    void put(unsigned bit)
    {
        if (count_ % 8 == 0) bytes_.push_back(0);
        bytes_.back() = std::uint8_t(bytes_.back() | (bit << (7 - count_ % 8)));
        ++count_;
    }
    std::vector<std::uint8_t> bytes_;
    std::size_t count_ {};
};

// An SPS with three sub-layers (two of which state a profile or a level), a
// conformance window, ordering info per sub-layer, PCM, short-term sets
// predicted from one another, long-term pictures, and a VUI with an extended
// sample aspect ratio, overscan and a colour description.
Unit syntheticSps(bool scalingList, bool vui, bool vuiHrd = false)
{
    BitWriter w;
    w.bits(0, 4);   // sps_video_parameter_set_id
    w.bits(2, 3);   // sps_max_sub_layers_minus1
    w.bits(1, 1);
    w.bits(0x22000000, 32); w.bits(0x60000000, 32); w.bits(63, 32);   // general profile (high tier), compatibility, constraints, level 2.1
    w.bits(1, 1); w.bits(1, 1);   // sub-layer 0: profile and level
    w.bits(0, 1); w.bits(1, 1);   // sub-layer 1: level only
    w.bits(0, 12);                // reserved_zero_2bits for the sub-layers that do not exist
    for (unsigned i = 0; i < 11; ++i) w.bits(0x5a, 8);   // sub-layer 0 profile (88 bits)
    w.bits(90, 8);                // sub-layer 0 level
    w.bits(60, 8);                // sub-layer 1 level
    w.ue(0);                      // sps_seq_parameter_set_id
    w.ue(1);                      // chroma_format_idc
    w.ue(1920);
    w.ue(1080);
    w.bits(1, 1);                 // conformance window
    for (unsigned i = 0; i < 4; ++i) w.ue(i);
    w.ue(2); w.ue(2);             // ten bits
    w.ue(4);                      // log2_max_pic_order_cnt_lsb_minus4: eight bits
    w.bits(1, 1);                 // sub_layer_ordering_info_present_flag
    for (unsigned i = 0; i < 3; ++i) { w.ue(4); w.ue(2); w.ue(0); }
    w.ue(0); w.ue(3); w.ue(0); w.ue(3);   // block sizes
    w.ue(2); w.ue(2);             // transform hierarchy depths
    w.bits(scalingList ? 1 : 0, 1);   // scaling_list_enabled_flag
    if (scalingList) w.bits(1, 1);    // sps_scaling_list_data_present_flag
    w.bits(1, 1); w.bits(1, 1);   // amp, sample adaptive offset
    w.bits(1, 1);                 // pcm
    w.bits(0x77, 8); w.ue(0); w.ue(2); w.bits(1, 1);
    w.ue(4);                      // num_short_term_ref_pic_sets
    // 0: negative -1 and -3, positive +2.
    w.ue(2); w.ue(1);
    w.ue(0); w.bits(1, 1); w.ue(1); w.bits(1, 1);
    w.ue(1); w.bits(1, 1);
    // 1: predicted from 0 with delta_rps +1: four flags (three pictures and the delta itself).
    w.bits(1, 1); w.bits(0, 1); w.ue(0);
    w.bits(0xf, 4);               // used_by_curr_pic_flag, four times
    // 2: predicted from 1 (-2, +1, +3) with delta_rps -1; one flag says "not used, use the delta".
    w.bits(1, 1); w.bits(1, 1); w.ue(0);
    w.bits(1, 1); w.bits(0, 1); w.bits(1, 1); w.bits(1, 1); w.bits(1, 1);
    // 3: predicted from 2 (-1, -3, +2): four flags again.
    w.bits(1, 1); w.bits(0, 1); w.ue(1);
    w.bits(1, 1); w.bits(1, 1); w.bits(1, 1); w.bits(1, 1);
    w.bits(1, 1);                 // long_term_ref_pics_present_flag
    w.ue(2);
    w.bits(0xa5, 8); w.bits(1, 1); w.bits(0x5a, 8); w.bits(0, 1);
    w.bits(1, 1); w.bits(1, 1);   // temporal MVP, strong intra smoothing
    w.bits(vui ? 1 : 0, 1);
    if (vui) {
        w.bits(1, 1); w.bits(255, 8); w.bits(16, 16); w.bits(11, 16);   // sample aspect ratio 16:11
        w.bits(1, 1); w.bits(1, 1);                                      // overscan
        w.bits(1, 1); w.bits(5, 3); w.bits(1, 1);                        // video signal type: unspecified format, full range
        w.bits(1, 1); w.bits(9, 8); w.bits(16, 8); w.bits(9, 8);         // BT.2020, PQ, BT.2020
        w.bits(1, 1); w.ue(2); w.ue(2);                                  // chroma sample location
        w.bits(0, 3);                                                    // neutral chroma, field_seq, frame_field_info
        w.bits(1, 1); for (unsigned i = 0; i < 4; ++i) w.ue(i + 1);      // default display window
        w.bits(1, 1); w.bits(1001, 32); w.bits(30000, 32);               // timing: 30000/1001
        w.bits(1, 1); w.ue(3);                                           // poc_proportional_to_timing_flag
        w.bits(vuiHrd ? 1 : 0, 1);                                       // vui_hrd_parameters_present_flag
    }
    return w.sps();
}

// ---- Levels.

// An H.264 SPS with the fields the level limits read.
struct H264SpsSpec {
    unsigned profile {66};
    unsigned constraints {0};
    unsigned level {10};
    unsigned widthMacroblocks {11};
    unsigned heightMacroblocks {9};
    unsigned refs {1};
    unsigned pocType {0};
    bool frameMbsOnly {true};
    bool scalingMatrix {false};
    bool vui {false};
    bool timing {false};
    unsigned timeScale {30};     // with 1 unit in tick: twice the frame rate when fixed
    bool hrd {false};
    unsigned hrdBitRate {64000}; // a multiple of 64
    unsigned hrdCpb {64000};     // a multiple of 16
    bool restriction {false};
    unsigned log2MvVertical {8};
    unsigned reorder {0};
    unsigned dpb {1};
};

Unit h264Sps(const H264SpsSpec& spec)
{
    BitWriter w;
    w.bits(spec.profile, 8);
    w.bits(spec.constraints, 8);
    w.bits(spec.level, 8);
    w.ue(0);   // seq_parameter_set_id
    switch (spec.profile) {
    case 100: case 110: case 122: case 244: case 44:
        w.ue(1); w.ue(0); w.ue(0); w.bits(0, 1);   // 4:2:0, eight bits, no bypass
        w.bits(spec.scalingMatrix ? 1 : 0, 1);
        if (spec.scalingMatrix) {
            // Eight lists: the first has two deltas ending in a zero scale, the second says "use the default",
            // the third has a delta and runs to its end, and five are absent.
            w.bits(1, 1); w.se(-3); w.se(-5);   // 16 values: 8 -> 5, then 5 -> 0 stops the list
            w.bits(1, 1); w.se(-8);             // next scale 0 straight away
            w.bits(1, 1); for (unsigned i = 0; i < 16; ++i) w.se(i == 0 ? 2 : 0);
            for (unsigned i = 3; i < 8; ++i) w.bits(0, 1);
        }
        break;
    default: break;
    }
    w.ue(0);   // log2_max_frame_num_minus4
    w.ue(spec.pocType);
    if (spec.pocType == 0) {
        w.ue(2);
    } else if (spec.pocType == 1) {
        w.bits(0, 1); w.se(-1); w.se(2); w.ue(2); w.se(1); w.se(-1);
    }
    w.ue(spec.refs);
    w.bits(0, 1);   // gaps_in_frame_num_value_allowed_flag
    w.ue(spec.widthMacroblocks - 1);
    w.ue((spec.frameMbsOnly ? spec.heightMacroblocks : spec.heightMacroblocks / 2) - 1);
    w.bits(spec.frameMbsOnly ? 1 : 0, 1);
    if (!spec.frameMbsOnly) w.bits(0, 1);
    w.bits(1, 1);   // direct_8x8_inference_flag
    w.bits(0, 1);   // frame_cropping_flag
    w.bits(spec.vui ? 1 : 0, 1);
    if (spec.vui) {
        w.bits(1, 1); w.bits(255, 8); w.bits(4, 16); w.bits(3, 16);   // aspect ratio 4:3
        w.bits(1, 1); w.bits(0, 1);                                   // overscan
        w.bits(1, 1); w.bits(5, 3); w.bits(0, 1); w.bits(1, 1); w.bits(1, 8); w.bits(1, 8); w.bits(1, 8);   // BT.709
        w.bits(1, 1); w.ue(1); w.ue(2);                               // chroma sample location
        w.bits(spec.timing ? 1 : 0, 1);
        if (spec.timing) { w.bits(1, 32); w.bits(spec.timeScale, 32); w.bits(1, 1); }
        w.bits(spec.hrd ? 1 : 0, 1);
        if (spec.hrd) {
            w.ue(0); w.bits(0, 4); w.bits(0, 4);   // one schedule, no scaling
            w.ue(spec.hrdBitRate / 64 - 1); w.ue(spec.hrdCpb / 16 - 1); w.bits(1, 1);
            w.bits(23, 5); w.bits(23, 5); w.bits(23, 5); w.bits(24, 5);
        }
        w.bits(0, 1);                                                 // vcl_hrd_parameters_present_flag
        if (spec.hrd) w.bits(0, 1);                                   // low_delay_hrd_flag
        w.bits(0, 1);                                                 // pic_struct_present_flag
        w.bits(spec.restriction ? 1 : 0, 1);
        if (spec.restriction) {
            w.bits(1, 1); w.ue(2); w.ue(1); w.ue(16); w.ue(spec.log2MvVertical); w.ue(spec.reorder); w.ue(spec.dpb);
        }
    }
    return w.unit({0x67});
}

// A Main-profile HEVC SPS of one temporal layer with the fields the level
// limits read, a PPS, and a VPS-shaped unit carrying a level.
struct HevcSpsSpec {
    unsigned level {63};
    bool highTier {false};
    unsigned width {176};
    unsigned height {144};
    unsigned dpb {5};
    unsigned reorder {0};
    bool hrd {false};
    bool timing {false};
    unsigned timeScale {15};   // with 1 unit in tick: the frame rate
};

Unit hevcSps(const HevcSpsSpec& spec)
{
    BitWriter w;
    w.bits(0, 4);   // sps_video_parameter_set_id
    w.bits(0, 3);   // sps_max_sub_layers_minus1
    w.bits(1, 1);
    w.bits(0x01000000u | (spec.highTier ? 0x20000000u : 0), 32);   // profile space, tier and Main
    w.bits(0x60000000, 32);                                        // compatibility, then constraint flags
    w.bits(spec.level, 32);                                        // the rest of the flags and the level
    w.ue(0); w.ue(1);                                              // sps_seq_parameter_set_id, 4:2:0
    w.ue(spec.width); w.ue(spec.height);
    w.bits(0, 1);                                                  // conformance window
    w.ue(0); w.ue(0);                                              // eight bits
    w.ue(4);                                                       // log2_max_pic_order_cnt_lsb_minus4
    w.bits(1, 1);                                                  // sub_layer_ordering_info_present_flag
    w.ue(spec.dpb - 1); w.ue(spec.reorder); w.ue(0);
    w.ue(0); w.ue(3); w.ue(0); w.ue(3);                            // block sizes
    w.ue(2); w.ue(2);                                              // transform hierarchy depths
    w.bits(0, 1);                                                  // scaling_list_enabled_flag
    w.bits(1, 1); w.bits(1, 1);                                    // amp, sample adaptive offset
    w.bits(0, 1);                                                  // pcm
    w.ue(0);                                                       // num_short_term_ref_pic_sets
    w.bits(0, 1);                                                  // long_term_ref_pics_present_flag
    w.bits(1, 1); w.bits(1, 1);                                    // temporal MVP, strong intra smoothing
    w.bits(1, 1);                                                  // vui_parameters_present_flag
    w.bits(0, 1); w.bits(0, 1);                                    // aspect ratio, overscan
    w.bits(0, 1); w.bits(0, 1);                                    // video signal type, chroma location
    w.bits(0, 3);
    w.bits(0, 1);                                                  // default display window
    w.bits(spec.timing || spec.hrd ? 1 : 0, 1);
    if (spec.timing || spec.hrd) {
        w.bits(1, 32); w.bits(spec.timeScale, 32); w.bits(0, 1);
        w.bits(spec.hrd ? 1 : 0, 1);
    }
    return w.sps();
}

Unit hevcPps(bool tiles)
{
    BitWriter w;
    w.ue(0); w.ue(0);
    w.bits(0, 1); w.bits(0, 1); w.bits(0, 3); w.bits(0, 2);
    w.ue(0); w.ue(0); w.se(0);
    w.bits(0, 2);
    w.bits(0, 1);   // cu_qp_delta_enabled_flag
    w.se(0); w.se(0);
    w.bits(0, 4);
    w.bits(tiles ? 1 : 0, 1);
    return w.unit({0x44, 0x01});
}

// A VPS with the level at its place in profile_tier_level, behind four bytes
// of fields, and a few fields after it.
Unit hevcVps(unsigned level)
{
    BitWriter w;
    w.bits(0, 4); w.bits(1, 1); w.bits(1, 1); w.bits(0, 6); w.bits(0, 3); w.bits(1, 1); w.bits(0xffff, 16);
    w.bits(0x01000000, 32); w.bits(0x60000000, 32); w.bits(level, 32);
    w.bits(1, 1); w.ue(4); w.ue(2); w.ue(0);
    w.bits(0, 6); w.ue(0); w.bits(0, 2);
    return w.unit({0x40, 0x01});
}

// Setting a level changes the level and nothing else.
void levelSetting()
{
    // H.264 on the checked-in streams.
    for (const char* name : {"h264-baseline", "h264-main", "h264-high"}) {
        const auto stream = read(std::string(name) + ".bin");
        for (const auto unit : splitAnnexB(stream)) {
            if (h264NalType(unit) != h264NalSps) continue;
            Unit sps(unit.begin(), unit.end());
            const auto level = h264Level(sps);
            require(level.has_value() && *level == sps[3], "the level is level_idc");
            Unit unchanged = sps;
            require(!setH264Level(unchanged, *level) && unchanged == sps, "a level already stated is left alone");
            require(setH264Level(sps, 52) && h264Level(sps) == std::optional<unsigned>(52), "a higher level is stated");
            require(setH264Level(sps, 10) && h264Level(sps) == std::optional<unsigned>(10), "and a lower one");
            Unit rest = sps, was(unit.begin(), unit.end());
            rest[3] = was[3];
            require(rest == was, "and nothing else changed");
        }
    }
    // Level 1b is written as level 1.1 with constraint_set3_flag by Baseline, Main and Extended, and as 9 by the rest.
    Unit oneB {0x67, 66, 0x10, 11, 0xaa};
    require(h264Level(oneB) == std::optional<unsigned>(9), "Baseline level 1.1 with constraint_set3_flag is level 1b");
    Unit high {0x67, 100, 0x10, 11, 0xaa};
    require(h264Level(high) == std::optional<unsigned>(11), "the flag means something else in High");
    require(setH264Level(oneB, 13) && oneB == (Unit {0x67, 66, 0x00, 13, 0xaa}), "leaving level 1b clears the flag");
    require(setH264Level(oneB, 9) && oneB == (Unit {0x67, 66, 0x10, 11, 0xaa}), "Baseline states 1b as 1.1 with the flag");
    require(setH264Level(oneB, 10) && oneB == (Unit {0x67, 66, 0x00, 10, 0xaa}), "level 1b falls to 1.0 and the flag clears");
    Unit highNine {0x67, 100, 0x00, 9, 0xaa};
    require(h264Level(highNine) == std::optional<unsigned>(9) && setH264Level(highNine, 10) && highNine[3] == 10, "High level 1b");
    require(setH264Level(highNine, 9) && highNine == (Unit {0x67, 100, 0x00, 9, 0xaa}), "High states 1b as level_idc 9");
    Unit notSps {0x68, 1, 2, 3};
    require(!h264Level(notSps) && !setH264Level(notSps, 30), "only an SPS has a level");

    // HEVC: the VPS and SPS of the checked-in streams state the same level, and a set one reads back.
    for (const char* name : {"hevc-main", "hevc-main10"}) {
        const auto stream = read(std::string(name) + ".bin");
        std::optional<unsigned> vpsLevel, spsLevel;
        for (const auto unit : splitAnnexB(stream)) {
            const unsigned type = hevcNalType(unit);
            if (type != hevcNalVps && type != hevcNalSps) continue;
            Unit nal(unit.begin(), unit.end());
            const auto level = hevcLevel(nal);
            require(level.has_value(), "the level can be read");
            (type == hevcNalVps ? vpsLevel : spsLevel) = level;
            if (type == hevcNalSps) require(parseHevcSps(nal)->levelIdc == *level, "the same level as the SPS reader finds");
            Unit unchanged = nal;
            require(!setHevcLevel(unchanged, *level) && unchanged == nal, "a level already stated is left alone");
            Unit raised = nal;
            require(setHevcLevel(raised, 156) && hevcLevel(raised) == std::optional<unsigned>(156), "a higher level is stated");
            require(setHevcLevel(raised, *level) && raised == nal, "and falling back restores the unit exactly");
            require(setHevcLevel(raised, 156), "once more");
            auto before = rbsp(nal, 2), after = rbsp(raised, 2);
            require(before.size() == after.size(), "the payload keeps its size");
            for (std::size_t i = 0; i < before.size(); ++i)
                require(before[i] == after[i] || i == *hevcLevelOffset(nal), "and only the level differs");
            // The unit still holds together as a unit of a stream.
            std::vector<std::uint8_t> annexB;
            appendAnnexB(annexB, raised.data(), raised.size());
            appendAnnexB(annexB, raised.data(), raised.size());
            const auto units = splitAnnexB(annexB);
            require(units.size() == 2 && Unit(units[0].begin(), units[0].end()) == raised, "with no start code inside it");
        }
        require(vpsLevel && spsLevel && *vpsLevel == *spsLevel, "the VPS and the SPS state one level");
    }
    // The profile_tier_level of a real SPS is full of zeros, so the level sits among emulation prevention bytes.
    Unit synthetic = syntheticSps(false, true);
    const auto before = parseHevcSpsFormat(synthetic);
    require(setHevcLevel(synthetic, 93) && hevcLevel(synthetic) == std::optional<unsigned>(93), "a synthetic SPS takes a level");
    const auto after = parseHevcSpsFormat(synthetic);
    require(before && after && after->primaries == before->primaries && after->transfer == before->transfer &&
            after->bitDepthLuma == before->bitDepthLuma && after->levelIdc == 93, "and still reads to its VUI");
    Unit slice {0x44, 0x01, 1, 2, 3, 4};
    require(!hevcLevel(slice) && !setHevcLevel(slice, 93), "only a VPS or SPS has a level");
}

void h264SpsLimits()
{
    for (const char* name : {"h264-baseline", "h264-main", "h264-high", "h264-high10"}) {
        const auto stream = read(std::string(name) + ".bin");
        bool found = false;
        for (const auto unit : splitAnnexB(stream)) {
            if (h264NalType(unit) != h264NalSps) continue;
            const auto limits = parseH264SpsLimits(unit);
            require(limits.has_value(), "the SPS of a checked-in stream is read to its VUI");
            require(limits->widthMacroblocks == 8 && limits->heightMacroblocks == 6 && limits->frameMbsOnly, "the picture is 128x96");
            require(limits->vuiPresent && limits->timingInfo && limits->bitstreamRestriction && !limits->hrd, "with timing and restrictions");
            require(limits->maxNumRefFrames <= limits->maxDecFrameBuffering, "the DPB holds the references");
            require(limits->levelIdc == h264Level(unit), "and the level the helper reads");
            found = true;
        }
        require(found, "an SPS");
    }
    H264SpsSpec spec;
    for (const unsigned profile : {66u, 77u, 100u, 110u}) {
        spec.profile = profile;
        spec.vui = true; spec.timing = true; spec.timeScale = 60; spec.hrd = true; spec.hrdBitRate = 128000; spec.hrdCpb = 350000;
        spec.restriction = true; spec.reorder = 1; spec.dpb = 3; spec.refs = 2; spec.pocType = profile == 77 ? 1 : 0;
        spec.scalingMatrix = profile >= 100;
        const auto limits = parseH264SpsLimits(h264Sps(spec));
        require(limits.has_value(), "a synthetic SPS is read");
        require(limits->profileIdc == profile && limits->levelIdc == 10 && limits->widthMacroblocks == 11 && limits->heightMacroblocks == 9,
                "profile, level and picture");
        require(limits->maxNumRefFrames == 2 && limits->pocType == spec.pocType, "references and order count type");
        require(limits->timingInfo && limits->timeScale == 60 && limits->numUnitsInTick == 1, "timing");
        require(limits->hrd && limits->hrdBitRate == 128000 && limits->hrdCpbSize == 350000, "the HRD is read to its rate and size");
        require(limits->bitstreamRestriction && limits->log2MaxMvLengthVertical == 8 && limits->maxNumReorderFrames == 1 &&
                limits->maxDecFrameBuffering == 3, "and the restrictions behind it");
    }
    spec = {};
    spec.frameMbsOnly = false; spec.heightMacroblocks = 8;
    const auto interlaced = parseH264SpsLimits(h264Sps(spec));
    require(interlaced && !interlaced->frameMbsOnly && interlaced->heightMacroblocks == 8, "field coding doubles the map units");
    const auto bare = parseH264SpsLimits(h264Sps({}));
    require(bare && !bare->vuiPresent && !bare->bitstreamRestriction && !bare->hrd, "no VUI states nothing");
    auto truncated = h264Sps([] { H264SpsSpec s; s.vui = true; s.restriction = true; return s; }());
    truncated.resize(truncated.size() - 3);
    require(!parseH264SpsLimits(truncated), "a truncated SPS is refused");
    require(!parseH264SpsLimits(Unit {0x68, 1, 2, 3, 4}), "a PPS is not an SPS");
}

// The settings of the Android case that failed: a QCIF recording at 15 frames
// per second and 64 kbit/s, variable bit rate with the peak the level allows,
// level 1.0 asked for.
VideoEncodeSettings qcifRecording(VideoEncodeProfile profile = VideoEncodeProfile::h264Baseline)
{
    VideoEncodeSettings settings;
    settings.codec = profile >= VideoEncodeProfile::hevcMain ? VideoCodec::hevc : VideoCodec::h264;
    settings.profile = profile;
    settings.level = settings.codec == VideoCodec::hevc ? 30 : 10;
    settings.width = 176;
    settings.height = 144;
    settings.frameRateNumerator = 15;
    settings.rateControl = VideoRateControl::variableBitrate;
    settings.bitrate = 64000;
    settings.peakBitrate = 64000;
    return settings;
}

std::optional<unsigned> choice(const Unit& sps, const levels::Policy& policy) { return levels::chooseH264Level(sps, policy); }

void h264LevelChoice()
{
    const auto policy = levels::makePolicy(qcifRecording(), false, true);
    require(policy.floor == 10 && policy.boundedBitrate == 64000 && !policy.reorders, "level 1.0 is asked for and the rate is bounded");

    // VideoToolbox states 1.1 for the picture and rate that fit 1.0 exactly (99 macroblocks at 15 frames per second).
    H264SpsSpec spec;
    spec.level = 11;
    require(choice(h264Sps(spec), policy) == std::optional<unsigned>(10), "VideoToolbox's 1.1 falls to the 1.0 that was asked for");
    // The same level is left alone, and a level below the floor rises to it.
    spec.level = 10;
    require(!choice(h264Sps(spec), policy), "the level asked for is left as it is");
    spec.level = 10;
    auto needsMore = qcifRecording();
    needsMore.bitrate = needsMore.peakBitrate = 100000;
    const auto raised = levels::makePolicy(needsMore, false, true);
    require(raised.floor == 11, "a stream of 100 kbit/s needs level 1.1");
    require(choice(h264Sps(spec), raised) == std::optional<unsigned>(11), "and a lower level rises to it");

    // Nothing bounds the rate: the level stays what VideoToolbox chose.
    spec.level = 11;
    auto noPeak = qcifRecording();
    noPeak.peakBitrate = 0;
    require(!choice(h264Sps(spec), levels::makePolicy(noPeak, false, true)), "a variable rate without a peak keeps VideoToolbox's level");
    require(!choice(h264Sps(spec), levels::makePolicy(qcifRecording(), false, false)), "and so does one whose peak is not enforced");
    auto quality = qcifRecording();
    quality.rateControl = VideoRateControl::constantQuality;
    require(!choice(h264Sps(spec), levels::makePolicy(quality, false, true)), "and constant quality");
    auto constant = qcifRecording();
    constant.rateControl = VideoRateControl::constantBitrate;
    constant.peakBitrate = 0;
    require(choice(h264Sps(spec), levels::makePolicy(constant, false, true)) == std::optional<unsigned>(10), "a constant rate is bounded by itself");

    // The picture, the frame rate and the rate each keep a stream out of a level.
    auto fast = qcifRecording();
    fast.frameRateNumerator = 30;
    const auto fastPolicy = levels::makePolicy(fast, false, true);
    require(fastPolicy.floor == 11, "30 frames per second of QCIF need level 1.1");
    require(choice(h264Sps(spec), fastPolicy) == std::nullopt, "and VideoToolbox's 1.1 stays");
    H264SpsSpec timed;
    timed.level = 12; timed.vui = true; timed.timing = true; timed.timeScale = 60;   // the stream says 30 frames per second
    require(choice(h264Sps(timed), policy) == std::optional<unsigned>(11), "a frame rate the SPS states beyond the settings counts");
    H264SpsSpec big;
    big.level = 21; big.widthMacroblocks = 22; big.heightMacroblocks = 18;
    auto cif = qcifRecording();
    cif.width = 352; cif.height = 288; cif.level = 20; cif.bitrate = cif.peakBitrate = 832000; cif.frameRateNumerator = 30;
    require(levels::makePolicy(cif, false, true).floor == 20, "CIF at 832 kbit/s asked for 2.0 states 2.0");
    require(choice(h264Sps(big), levels::makePolicy(cif, false, true)) == std::optional<unsigned>(20), "2.1 falls to 2.0");
    cif.level = 13;
    require(levels::makePolicy(cif, false, true).floor == 20, "level 1.3 asked for CIF at 832 kbit/s is raised to 2.0 by the rate");

    // The DPB: the references, and the frames waiting for output when B-frames reorder.
    H264SpsSpec refs;
    refs.level = 12; refs.refs = 4;
    require(choice(h264Sps(refs), policy) == std::optional<unsigned>(10), "four references fit the four frames of 1.0");
    refs.refs = 5;
    require(choice(h264Sps(refs), policy) == std::optional<unsigned>(11), "five do not, and 1.1 holds nine");
    const auto reordering = levels::makePolicy(qcifRecording(), true, true);
    refs.refs = 1;
    require(choice(h264Sps(refs), reordering) == std::optional<unsigned>(10), "one reference and three waiting frames fit 1.0");
    refs.refs = 2;
    require(choice(h264Sps(refs), reordering) == std::optional<unsigned>(11), "two references and three waiting frames need 1.1");
    refs.level = 11; refs.refs = 4;
    require(!choice(h264Sps(refs), reordering), "and VideoToolbox's 1.1 holds four and three (nine frames)");
    H264SpsSpec restricted;
    restricted.level = 12; restricted.vui = true; restricted.restriction = true; restricted.refs = 2; restricted.dpb = 4; restricted.reorder = 2;
    require(choice(h264Sps(restricted), reordering) == std::optional<unsigned>(10), "a stated DPB of four fits 1.0 whatever reorders");
    restricted.dpb = 5;
    require(choice(h264Sps(restricted), reordering) == std::optional<unsigned>(11), "five does not");
    restricted.dpb = 4; restricted.log2MvVertical = 9;
    require(choice(h264Sps(restricted), reordering) == std::optional<unsigned>(11), "a vertical motion vector range beyond 64 samples does not");

    // The HRD parameters the SPS states are held to the level as well.
    H264SpsSpec hrd;
    hrd.level = 12; hrd.vui = true; hrd.hrd = true; hrd.hrdBitRate = 64000; hrd.hrdCpb = 160000;
    require(choice(h264Sps(hrd), policy) == std::optional<unsigned>(10), "an HRD within 64 kbit/s and 175 kbit fits 1.0");
    hrd.hrdBitRate = 128000;
    require(choice(h264Sps(hrd), policy) == std::optional<unsigned>(11), "a higher rate does not");
    hrd.hrdBitRate = 64000; hrd.hrdCpb = 200000;
    require(choice(h264Sps(hrd), policy) == std::optional<unsigned>(11), "nor a larger buffer");
    auto buffered = qcifRecording();
    buffered.bufferBits = 200000;
    spec.level = 11;
    require(!choice(h264Sps(spec), levels::makePolicy(buffered, false, true)) ||
            *choice(h264Sps(spec), levels::makePolicy(buffered, false, true)) != 10, "a buffer the caller sized beyond 1.0's does not");

    // Interlaced coding is allowed from level 2.1.
    H264SpsSpec fields;
    fields.level = 21; fields.frameMbsOnly = false; fields.heightMacroblocks = 8;
    auto smallFields = qcifRecording();
    smallFields.height = 128;
    smallFields.level = 10;
    require(!choice(h264Sps(fields), levels::makePolicy(smallFields, false, true)), "an interlaced SPS stays at 2.1");

    // The bit rate factor follows the profile of the stream.
    auto highRate = qcifRecording(VideoEncodeProfile::h264High);
    highRate.bitrate = highRate.peakBitrate = 80000;
    H264SpsSpec highSps;
    highSps.profile = 100; highSps.level = 11;
    require(levels::makePolicy(highRate, false, true).floor == 10, "High allows 80 kbit/s at level 1.0");
    require(choice(h264Sps(highSps), levels::makePolicy(highRate, false, true)) == std::optional<unsigned>(10), "and 1.1 falls to it");
    H264SpsSpec mainSps;
    mainSps.profile = 77; mainSps.level = 11;
    require(choice(h264Sps(mainSps), levels::makePolicy(highRate, false, true)) != std::optional<unsigned>(10),
            "a Main stream does not get the High allowance");
    H264SpsSpec unknown;
    unknown.profile = 83; unknown.level = 11;
    require(!choice(h264Sps(unknown), policy), "a profile whose factor is not known is left alone");

    // Level 1b sits between 1.0 and 1.1.
    auto oneB = qcifRecording();
    oneB.level = 9;
    const auto oneBPolicy = levels::makePolicy(oneB, false, true);
    require(oneBPolicy.floor == 9, "level 1b asked for is the floor, above the 1.0 the stream needs");
    spec.level = 11; spec.profile = 66;
    auto baseline = h264Sps(spec);
    require(choice(baseline, oneBPolicy) == std::optional<unsigned>(9), "1.1 falls to 1b");
    std::vector<Unit> units {baseline};
    require(levels::restateLevel(VideoCodec::h264, units, oneBPolicy) == 9 && units[0][2] == 0x10 && units[0][3] == 11,
            "Baseline states 1b as 1.1 with constraint_set3_flag");
    // A stream that states 1b where only 1.0 was asked for falls to the 1.0 that fits.
    std::vector<Unit> flagged {units[0]};
    require(levels::restateLevel(VideoCodec::h264, flagged, policy) == 10, "1b falls to 1.0");
    require(flagged[0][2] == 0x00 && flagged[0][3] == 10, "and clears the flag");
}

void hevcLevelChoice()
{
    const auto policy = levels::makePolicy(qcifRecording(VideoEncodeProfile::hevcMain), false, true);
    require(policy.floor == 30 && policy.boundedBitrate == 64000, "level 1 is asked for");
    HevcSpsSpec spec;
    const auto pps = hevcPps(false);
    const auto vps = hevcVps(63);
    const auto choose = [&](const HevcSpsSpec& s, const levels::Policy& p, const Unit& picturePps = hevcPps(false)) {
        return levels::chooseHevcLevel(hevcSps(s), picturePps, p);
    };
    require(choose(spec, policy) == std::optional<unsigned>(30), "VideoToolbox's 2.1 falls to the level 1 that was asked for");
    spec.level = 30;
    require(!choose(spec, policy), "the level asked for is left alone");
    auto bigger = qcifRecording(VideoEncodeProfile::hevcMain);
    bigger.bitrate = bigger.peakBitrate = 400000;
    spec.level = 90;
    require(levels::makePolicy(bigger, false, true).floor == 60, "400 kbit/s needs level 2");
    require(choose(spec, levels::makePolicy(bigger, false, true)) == std::optional<unsigned>(60), "and 3 falls to it, not to 1");
    spec.level = 20;
    require(choose(spec, levels::makePolicy(bigger, false, true)) == std::optional<unsigned>(60), "a level below the floor rises");

    spec.level = 63;
    auto noPeak = qcifRecording(VideoEncodeProfile::hevcMain);
    noPeak.peakBitrate = 0;
    require(!choose(spec, levels::makePolicy(noPeak, false, true)), "an unbounded rate keeps VideoToolbox's level");
    spec.dpb = 9;
    require(choose(spec, policy) == std::optional<unsigned>(60), "nine pictures do not fit 176x144 at level 1, whose DPB holds eight");
    spec.dpb = 5;
    spec.width = 320; spec.height = 240;
    auto qvga = qcifRecording(VideoEncodeProfile::hevcMain);
    qvga.width = 320; qvga.height = 240;
    require(levels::makePolicy(qvga, false, true).floor == 30 || levels::makePolicy(qvga, false, true).floor == 60,
            "QVGA is above level 1's picture size");
    qvga.level = 30;
    const auto qvgaPolicy = levels::makePolicy(qvga, false, true);
    require(qvgaPolicy.floor == 60, "so level 2 is the floor");
    spec.level = 93;
    require(choose(spec, qvgaPolicy) == std::optional<unsigned>(60), "and 3.1 falls to 2");
    spec.width = 176; spec.height = 144;

    // What the walk cannot vouch for keeps VideoToolbox's level.
    spec.level = 63;
    HevcSpsSpec highTier = spec;
    highTier.highTier = true;
    require(!choose(highTier, policy), "the High tier");
    HevcSpsSpec withHrd = spec;
    withHrd.hrd = true;
    require(!choose(withHrd, policy), "HRD parameters");
    require(!choose(spec, policy, hevcPps(true)), "tiles");
    require(levels::chooseHevcLevel(hevcSps(spec), Unit {}, policy) == std::nullopt, "a missing PPS");
    HevcSpsSpec timed = spec;
    timed.timing = true;
    require(choose(timed, policy) == std::optional<unsigned>(30), "timing information within the limit is fine");
    timed.timeScale = 30;
    require(choose(timed, policy) == std::optional<unsigned>(60), "a frame rate the SPS states beyond level 1's sample rate counts");

    // The VPS and the SPS state one level, as the stream stated it before.
    std::vector<Unit> units {vps, hevcSps(spec), pps};
    require(levels::restateLevel(VideoCodec::hevc, units, policy) == 30, "the stream now states level 1");
    require(hevcLevel(units[0]) == std::optional<unsigned>(30) && hevcLevel(units[1]) == std::optional<unsigned>(30), "in the VPS and the SPS");
    require(units[2] == pps, "and the PPS is untouched");
    std::vector<Unit> unbounded {vps, hevcSps(spec), pps};
    require(levels::restateLevel(VideoCodec::hevc, unbounded, levels::makePolicy(noPeak, false, true)) == 63 &&
            hevcLevel(unbounded[0]) == std::optional<unsigned>(63), "units left alone keep their level");
}

// The units of the checked-in streams, with the level VideoToolbox might have
// chosen, and the policy that brings the level back to the stream's own.
void levelRoundTrip()
{
    struct Case { const char* name; VideoCodec codec; VideoEncodeProfile profile; };
    for (const Case c : {Case {"h264-baseline", VideoCodec::h264, VideoEncodeProfile::h264Baseline},
                         Case {"h264-main", VideoCodec::h264, VideoEncodeProfile::h264Main},
                         Case {"h264-high", VideoCodec::h264, VideoEncodeProfile::h264High},
                         Case {"hevc-main", VideoCodec::hevc, VideoEncodeProfile::hevcMain},
                         Case {"hevc-main10", VideoCodec::hevc, VideoEncodeProfile::hevcMain10}}) {
        const auto stream = read(std::string(c.name) + ".bin");
        std::vector<Unit> original;
        for (const auto unit : splitAnnexB(stream)) {
            const bool hevc = c.codec == VideoCodec::hevc;
            const unsigned type = hevc ? hevcNalType(unit) : h264NalType(unit);
            if (hevc ? (type == hevcNalVps || type == hevcNalSps || type == hevcNalPps) : (type == h264NalSps || type == h264NalPps))
                original.emplace_back(unit.begin(), unit.end());
        }
        require(original.size() >= 2, "parameter sets");
        VideoEncodeSettings settings;
        settings.codec = c.codec;
        settings.profile = c.profile;
        settings.width = 128;
        settings.height = 96;
        settings.frameRateNumerator = 25;
        settings.rateControl = VideoRateControl::variableBitrate;
        settings.bitrate = 40000;
        settings.peakBitrate = 40000;
        settings.level = c.codec == VideoCodec::hevc ? 30 : 10;
        const auto policy = levels::makePolicy(settings, c.profile != VideoEncodeProfile::h264Baseline, true);
        auto units = original;
        const unsigned before = levels::restateLevel(c.codec, units, policy);
        require(before == settings.level, "a stream already at the level asked for keeps it");
        require(units == original, "and is byte for byte what it was");
        // VideoToolbox's choice, three levels up.
        auto high = original;
        for (auto& unit : high) {
            if (c.codec == VideoCodec::hevc) (void)setHevcLevel(unit, 93);
            else if (h264NalType(unit) == h264NalSps) (void)setH264Level(unit, 31);
        }
        require(high != original, "the level was raised");
        const unsigned after = levels::restateLevel(c.codec, high, policy);
        std::printf("  %s: from a level three up, %u\n", c.name, after);
        require(after == settings.level, "and comes back to the level asked for");
        require(high == original, "byte for byte");
    }
}

void hevcSpsFormat()
{
    const auto unit = syntheticSps(false, true);
    const auto format = parseHevcSpsFormat(unit);
    require(format.has_value(), "the synthetic SPS is read to its VUI");
    require(format->chromaFormat == 1 && format->bitDepthLuma == 10 && format->bitDepthChroma == 10, "format and depth");
    require(format->vuiPresent && format->colorDescription, "a colour description");
    require(format->primaries == 9 && format->transfer == 16 && format->matrix == 9 && format->fullRange,
            "the VUI is found behind sub-layers, a window, PCM, predicted sets and long-term pictures");

    require(format->highTier && format->levelIdc == 63 && format->maxSubLayersMinus1 == 2 && format->subLayerLevels,
            "the tier, the level and the sub-layers");
    require(format->width == 1920 && format->height == 1080 && format->maxDecPicBuffering == 5 && format->maxNumReorder == 2,
            "the picture size and the DPB of the highest sub-layer");
    require(format->timingInfo && format->numUnitsInTick == 1001 && format->timeScale == 30000 && !format->hrd,
            "the timing information and no HRD");
    const auto withHrd = parseHevcSpsFormat(syntheticSps(false, true, true));
    require(withHrd && withHrd->hrd, "HRD parameters are noticed");

    const auto noVui = parseHevcSpsFormat(syntheticSps(false, false));
    require(noVui.has_value() && !noVui->vuiPresent && !noVui->colorDescription, "an SPS with no VUI states no colour");
    require(!parseHevcSpsFormat(syntheticSps(true, true)).has_value(), "scaling list data is refused rather than skipped");
    Unit truncated = unit;
    truncated.resize(truncated.size() - 6);
    require(!parseHevcSpsFormat(truncated).has_value(), "a truncated SPS is refused");
    require(!parseHevcSpsFormat(Unit {0x40, 0x01, 1, 2, 3, 4}).has_value(), "a VPS is not an SPS");
}
} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: uml_video_encoder_bitstream_tests FIXTURE_DIRECTORY\n");
        return 2;
    }
    fixtureDirectory = argv[1];
    try {
        splitting();
        emulationPrevention();
        bitReading();
        seiMessages();
        hevcSpsFormat();
        levelSetting();
        h264SpsLimits();
        h264LevelChoice();
        hevcLevelChoice();
        levelRoundTrip();
        fixtures();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("video encoder bitstream helpers ok");
    return 0;
}
