// SPDX-License-Identifier: GPL-2.0-or-later
// The encoder device's view of the platform: how the platform's capabilities
// become wire capability responses and V4L2 control descriptors, how a stream's
// control values become encoder settings, and which values it refuses. These
// are pure functions of their inputs, so none of it needs a device or an
// encoder.

#include "encoder_capabilities.hpp"
#include "encoder_controls.hpp"
#include "encoder_test_capabilities.hpp"
#include "require.hpp"
#include "wire_bytes.hpp"

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <tuple>

using namespace uml::platform;
using namespace uml::video;
using uml::testing::encoderHostCapabilities;
using uml::testing::load;
using uml::testing::require;

namespace {

constexpr auto input = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
constexpr auto output = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;

constexpr std::uint64_t codedBit(std::uint32_t format)
{
    return std::uint64_t(1) << (format - VIRTIO_VIDEO_FORMAT_CODED_MIN);
}
constexpr std::uint64_t menuBit(std::uint32_t index) { return std::uint64_t(1) << index; }

struct ParsedFormat {
    virtio_video_format_desc desc;
    virtio_video_format_frame frame;
    virtio_video_format_range rate;
};

/* The descriptors of a capability response, with every length cross-checked
 * so a response that overruns its own counts fails here. */
std::vector<ParsedFormat> parse(const std::vector<std::uint8_t>& response)
{
    const auto header = load<virtio_video_query_capability_resp>(response);
    require(header.hdr.type == VIRTIO_VIDEO_RESP_OK_QUERY_CAPABILITY, "capability response type");
    std::size_t offset = sizeof(header);
    std::vector<ParsedFormat> formats;
    for (std::uint32_t index = 0; index < header.num_descs; ++index) {
        ParsedFormat parsed {};
        parsed.desc = load<virtio_video_format_desc>(response, offset);
        offset += sizeof(parsed.desc);
        require(parsed.desc.num_frames == 1, "one frame range per format");
        parsed.frame = load<virtio_video_format_frame>(response, offset);
        offset += sizeof(parsed.frame);
        require(parsed.frame.num_rates == 1, "one rate range per frame range");
        parsed.rate = load<virtio_video_format_range>(response, offset);
        offset += sizeof(parsed.rate);
        formats.push_back(parsed);
    }
    require(offset == response.size(), "the capability response has trailing bytes");
    return formats;
}

const virtio_video_v4l2_control_desc& control(const EncoderControls& controls, std::uint32_t id)
{
    const auto* found = controls.find(id);
    require(found != nullptr, "a control the host should publish is missing");
    return *found;
}
bool published(const EncoderControls& controls, std::uint32_t id) { return controls.find(id) != nullptr; }

void wireNames()
{
    for (const auto layout : {RawVideoLayout::nv12, RawVideoLayout::i420, RawVideoLayout::p010,
                              RawVideoLayout::nv16, RawVideoLayout::nv24, RawVideoLayout::bgra, RawVideoLayout::rgba}) {
        const auto format = wireRawFormat(layout);
        require(format >= VIRTIO_VIDEO_FORMAT_RAW_MIN && format <= VIRTIO_VIDEO_FORMAT_RAW_MAX, "raw wire format range");
        require(platformRawLayout(format) == layout, "a raw format does not round-trip");
    }
    require(!platformRawLayout(VIRTIO_VIDEO_FORMAT_ARGB8888) && !platformRawLayout(VIRTIO_VIDEO_FORMAT_H264) &&
            !platformRawLayout(0), "the device named a raw format it cannot encode from");
    for (const auto codec : {VideoCodec::h264, VideoCodec::hevc, VideoCodec::jpeg}) {
        const auto format = encodedWireFormat(codec);
        require(format && *format >= VIRTIO_VIDEO_FORMAT_CODED_MIN, "coded wire format");
        require(encodedCodec(*format) == codec, "a coded format does not round-trip");
    }
    require(!encodedWireFormat(VideoCodec::vp9), "VP9 has no encoder here and must have no wire name");
    require(!encodedCodec(VIRTIO_VIDEO_FORMAT_VP9) && !encodedCodec(VIRTIO_VIDEO_FORMAT_NV12) &&
            !encodedCodec(VIRTIO_VIDEO_FORMAT_VP8), "the device decodes a format it cannot encode");
    require(encodedWireFormat(VideoCodec::jpeg) == VIRTIO_VIDEO_FORMAT_MJPEG, "JPEG is the motion JPEG format");
}

void capabilityResponses()
{
    const auto capabilities = encoderHostCapabilities();
    // The order is fixed: NV12, I420, P010, BGRA, RGBA, and the codecs as the host lists them.
    const auto raw = advertisedRawLayouts(capabilities);
    require((raw == std::vector<RawVideoLayout> {RawVideoLayout::nv12, RawVideoLayout::i420, RawVideoLayout::p010,
                                                  RawVideoLayout::bgra, RawVideoLayout::rgba}),
            "the raw formats are not the ones an encoder accepted, in the fixed order");
    require((advertisedCodedFormats(capabilities) ==
             std::vector<std::uint32_t> {VIRTIO_VIDEO_FORMAT_H264, VIRTIO_VIDEO_FORMAT_HEVC, VIRTIO_VIDEO_FORMAT_MJPEG}),
            "the coded formats are not the host's codecs in order");

    const auto inputs = parse(encoderCapabilityResponse(capabilities, input));
    require(inputs.size() == 5, "the input queue lists one entry per raw format");
    std::vector<std::uint32_t> inputFormats;
    for (const auto& entry : inputs) inputFormats.push_back(entry.desc.format);
    require((inputFormats == std::vector<std::uint32_t> {VIRTIO_VIDEO_FORMAT_NV12, VIRTIO_VIDEO_FORMAT_YUV420,
                                                          VIRTIO_VIDEO_FORMAT_P010, VIRTIO_VIDEO_FORMAT_BGRA8888,
                                                          VIRTIO_VIDEO_FORMAT_RGBA8888}),
            "raw format order changed");
    // A mask names, by index into the other queue's list, the formats it works with.
    const std::uint64_t h264 = 1, hevc = 2, jpeg = 4;
    require(inputs[0].desc.mask == (h264 | hevc | jpeg), "NV12 feeds every codec");
    require(inputs[1].desc.mask == (h264 | hevc), "I420 feeds H.264 and HEVC but not JPEG");
    require(inputs[2].desc.mask == hevc, "P010 feeds only HEVC Main10");
    require(inputs[3].desc.mask == (h264 | hevc | jpeg), "BGRA feeds every codec");
    require(inputs[4].desc.mask == (h264 | hevc | jpeg), "RGBA feeds every codec");
    // The planar 8-bit formats may be one buffer or one per plane; the others are one buffer.
    const auto both = VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER | VIRTIO_VIDEO_PLANES_LAYOUT_PER_PLANE;
    require(inputs[0].desc.planes_layout == both && inputs[1].desc.planes_layout == both,
            "planar formats must offer both buffer layouts");
    require(inputs[2].desc.planes_layout == VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER &&
            inputs[3].desc.planes_layout == VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER &&
            inputs[4].desc.planes_layout == VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER,
            "P010, BGRA and RGBA are single-buffer formats");
    // The size range is the union over the codecs that take the format.
    require(inputs[0].frame.width.min == 16 && inputs[0].frame.width.max == 16384 &&
            inputs[0].frame.height.max == 16384, "NV12 size range is the widest of its codecs");
    require(inputs[1].frame.width.min == 64 && inputs[1].frame.width.max == 8192 &&
            inputs[1].frame.height.max == 4320, "I420 size range is H.264's and HEVC's");
    require(inputs[2].frame.width.max == 8192 && inputs[2].frame.width.min == 64, "P010 size range is HEVC's");
    require(inputs[0].frame.width.step == 2 && inputs[0].frame.height.step == 2, "size step is the alignment");
    require(inputs[0].rate.min == 1 && inputs[0].rate.max == maximumAdvertisedFrameRate && inputs[0].rate.step == 1,
            "frame rate range");

    const auto outputs = parse(encoderCapabilityResponse(capabilities, output));
    require(outputs.size() == 3, "the output queue lists one entry per codec");
    require(outputs[0].desc.format == VIRTIO_VIDEO_FORMAT_H264 && outputs[1].desc.format == VIRTIO_VIDEO_FORMAT_HEVC &&
            outputs[2].desc.format == VIRTIO_VIDEO_FORMAT_MJPEG, "coded format order");
    // Output masks index the raw list: NV12 = 1, I420 = 2, P010 = 4, BGRA = 8, RGBA = 16.
    require(outputs[0].desc.mask == (1 | 2 | 8 | 16), "H.264 takes NV12, I420, BGRA and RGBA");
    require(outputs[1].desc.mask == (1 | 2 | 4 | 8 | 16), "HEVC takes everything");
    require(outputs[2].desc.mask == (1 | 8 | 16), "JPEG takes NV12, BGRA and RGBA");
    require(outputs[0].desc.planes_layout == VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER, "coded buffers are single-plane");
    require(outputs[0].frame.width.max == 4096 && outputs[0].frame.height.max == 2304, "H.264 size range");
    require(outputs[2].frame.width.min == 16, "JPEG size range");

    bool threw = false;
    try { (void)encoderCapabilityResponse(capabilities, 0); } catch (const std::invalid_argument&) { threw = true; }
    require(threw, "a capability query for no queue was answered");

    // A host with only JPEG advertises only what it has, and the masks follow the lists.
    std::vector<VideoEncodeCapability> jpegOnly {capabilities[2]};
    const auto jpegInputs = parse(encoderCapabilityResponse(jpegOnly, input));
    require(jpegInputs.size() == 3 && jpegInputs[0].desc.mask == 1 && jpegInputs[1].desc.format == VIRTIO_VIDEO_FORMAT_BGRA8888 &&
            jpegInputs[2].desc.format == VIRTIO_VIDEO_FORMAT_RGBA8888, "a JPEG-only host lists NV12, BGRA and RGBA");
    // A layout is listed because a profile encoded from it: a host that did not encode RGBA does not list it.
    auto noRgba = capabilities;
    for (auto& capability : noRgba)
        for (auto& profile : capability.profiles)
            profile.inputs.erase(std::remove(profile.inputs.begin(), profile.inputs.end(), RawVideoLayout::rgba), profile.inputs.end());
    const auto withoutRgba = parse(encoderCapabilityResponse(noRgba, input));
    require(withoutRgba.size() == 4 && withoutRgba.back().desc.format == VIRTIO_VIDEO_FORMAT_BGRA8888,
            "RGBA was listed on a host that did not encode it");
    // VP9 is the decoder's; an encoder capability for it is not advertised.
    VideoEncodeCapability vp9 = capabilities[0];
    vp9.codec = VideoCodec::vp9;
    std::vector<VideoEncodeCapability> withVp9 {capabilities[0], vp9};
    require(advertisedCodedFormats(withVp9).size() == 1, "VP9 was advertised by the encoder");
}

void controlTable()
{
    const auto capabilities = encoderHostCapabilities();
    const EncoderControls controls(capabilities);
    std::set<std::uint32_t> ids;
    for (const auto& descriptor : controls.descriptors()) {
        require(ids.insert(descriptor.id).second, "a control is published twice");
        require(descriptor.coded_formats != 0, "a control that applies to no codec");
        require((descriptor.coded_formats & ~(codedBit(VIRTIO_VIDEO_FORMAT_H264) | codedBit(VIRTIO_VIDEO_FORMAT_HEVC) |
                                              codedBit(VIRTIO_VIDEO_FORMAT_MJPEG))) == 0, "a control names a codec the host lacks");
        const auto minimum = std::int64_t(descriptor.minimum), maximum = std::int64_t(descriptor.maximum);
        require(minimum <= maximum, "a control with an empty range");
        require(std::int64_t(descriptor.default_value) >= minimum && std::int64_t(descriptor.default_value) <= maximum,
                "a default outside its range");
        if (descriptor.type == UML_V4L2_CTRL_TYPE_MENU) {
            require(descriptor.maximum < 64, "a menu beyond the skip mask");
            require(((descriptor.menu_skip_mask >> descriptor.default_value) & 1) == 0, "a default the menu skips");
            require(descriptor.menu_skip_mask >> (descriptor.maximum + 1) == 0, "a skip bit beyond the menu");
            require((descriptor.menu_skip_mask & (menuBit(std::uint32_t(descriptor.maximum) + 1) - 1)) !=
                    (menuBit(std::uint32_t(descriptor.maximum) + 1) - 1), "a menu with every item skipped");
        }
        if (descriptor.type == UML_V4L2_CTRL_TYPE_HDR10_CLL_INFO || descriptor.type == UML_V4L2_CTRL_TYPE_HDR10_MASTERING_DISPLAY)
            require(descriptor.elem_size != 0, "a compound control without a payload size");
        else
            require(descriptor.elem_size == 0, "a scalar with a payload size");
    }

    const auto h264Only = codedBit(VIRTIO_VIDEO_FORMAT_H264);
    const auto hevcOnly = codedBit(VIRTIO_VIDEO_FORMAT_HEVC);
    const auto video = h264Only | hevcOnly;

    // Rate control: a mode both codecs have, each with the limit of the stricter.
    const auto& mode = control(controls, UML_V4L2_CID_MPEG_VIDEO_BITRATE_MODE);
    require(mode.type == UML_V4L2_CTRL_TYPE_MENU && mode.maximum == UML_V4L2_MPEG_VIDEO_BITRATE_MODE_CQ &&
            mode.menu_skip_mask == 0 && mode.default_value == UML_V4L2_MPEG_VIDEO_BITRATE_MODE_VBR &&
            mode.coded_formats == video, "bitrate mode menu");
    const auto& bitrate = control(controls, UML_V4L2_CID_MPEG_VIDEO_BITRATE);
    // The floor is 20 kbit/s: the least that holds Android's quality bar at the smallest picture.
    require(bitrate.minimum == 20'000 && bitrate.maximum == 80'000'000 && bitrate.default_value == 2'000'000 &&
            (bitrate.flags & VIRTIO_VIDEO_V4L2_CTRL_F_DYNAMIC), "bitrate stops at HEVC's lower ceiling and is dynamic");
    require(published(controls, UML_V4L2_CID_MPEG_VIDEO_BITRATE_PEAK), "peak bitrate with VBR");
    const auto& quality = control(controls, UML_V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY);
    require(quality.minimum == 1 && quality.maximum == 100 && (quality.flags & VIRTIO_VIDEO_V4L2_CTRL_F_DYNAMIC),
            "constant quality");
    // HEVC has no quantizer-only mode, so the shared switch for it is not offered.
    require(!published(controls, UML_V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE), "frame RC switch offered without CQP in both codecs");
    require(published(controls, UML_V4L2_CID_MPEG_VIDEO_MB_RC_ENABLE) && published(controls, UML_V4L2_CID_MPEG_VIDEO_VBV_SIZE) &&
            published(controls, UML_V4L2_CID_MPEG_VIDEO_GOP_SIZE), "ordinary video controls");
    // B-frames: the longest run of the codecs that have one, for those codecs; a request below a run codes none.
    const auto& bFrames = control(controls, UML_V4L2_CID_MPEG_VIDEO_B_FRAMES);
    require(bFrames.minimum == 0 && bFrames.maximum == 3 && bFrames.step == 1 && bFrames.default_value == 0 &&
            bFrames.coded_formats == video, "B-frame range is up to the longest run");
    const auto& key = control(controls, UML_V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME);
    require(key.type == UML_V4L2_CTRL_TYPE_BUTTON && (key.flags & VIRTIO_VIDEO_V4L2_CTRL_F_DYNAMIC), "force key frame");
    const auto& header = control(controls, UML_V4L2_CID_MPEG_VIDEO_HEADER_MODE);
    require(header.maximum == UML_V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME && header.menu_skip_mask == 0 &&
            header.default_value == UML_V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME, "header mode");
    const auto& prepend = control(controls, UML_V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR);
    require(prepend.type == UML_V4L2_CTRL_TYPE_BOOLEAN && prepend.default_value == 1 &&
            (prepend.flags & VIRTIO_VIDEO_V4L2_CTRL_F_DYNAMIC), "prepend parameter sets");

    // H.264: the profile menu skips what the host did not encode with, and the level stops at the host's.
    const auto& profile = control(controls, UML_V4L2_CID_MPEG_VIDEO_H264_PROFILE);
    const std::uint64_t publishedProfiles = menuBit(UML_V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE) |
        menuBit(UML_V4L2_MPEG_VIDEO_H264_PROFILE_MAIN) | menuBit(UML_V4L2_MPEG_VIDEO_H264_PROFILE_HIGH);
    require(profile.maximum == UML_V4L2_MPEG_VIDEO_H264_PROFILE_HIGH &&
            profile.menu_skip_mask == (menuBit(UML_V4L2_MPEG_VIDEO_H264_PROFILE_HIGH + 1) - 1 - publishedProfiles) &&
            profile.default_value == UML_V4L2_MPEG_VIDEO_H264_PROFILE_HIGH && profile.coded_formats == h264Only,
            "H.264 profile menu");
    const auto& level = control(controls, UML_V4L2_CID_MPEG_VIDEO_H264_LEVEL);
    require(level.maximum == UML_V4L2_MPEG_VIDEO_H264_LEVEL_5_2 && level.menu_skip_mask == 0,
            "H.264 level menu stops at the host's level 5.2");
    // A stream states at least the level it asks for, so the default is the lowest level: as low as it fits.
    require(level.default_value == UML_V4L2_MPEG_VIDEO_H264_LEVEL_1_0, "H.264 level defaults to the lowest");
    require(control(controls, UML_V4L2_CID_MPEG_VIDEO_H264_MIN_QP).default_value == 0 &&
            control(controls, UML_V4L2_CID_MPEG_VIDEO_H264_MAX_QP).default_value == 51 &&
            (control(controls, UML_V4L2_CID_MPEG_VIDEO_H264_MAX_QP).flags & VIRTIO_VIDEO_V4L2_CTRL_F_DYNAMIC) &&
            !(control(controls, UML_V4L2_CID_MPEG_VIDEO_H264_MIN_QP).flags & VIRTIO_VIDEO_V4L2_CTRL_F_DYNAMIC),
            "H.264 quantizer bounds: only the maximum is dynamic");
    for (const auto id : {UML_V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP, UML_V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP,
                          UML_V4L2_CID_MPEG_VIDEO_H264_B_FRAME_QP})
        require(control(controls, id).default_value == 26 && control(controls, id).maximum == 51, "H.264 frame quantizers");
    require(published(controls, UML_V4L2_CID_MPEG_VIDEO_H264_8X8_TRANSFORM) &&
            published(controls, UML_V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE) &&
            published(controls, UML_V4L2_CID_MPEG_VIDEO_H264_CPB_SIZE), "H.264 tools");
    require(control(controls, UML_V4L2_CID_MPEG_VIDEO_H264_HIERARCHICAL_CODING_LAYER).maximum == 2 &&
            published(controls, UML_V4L2_CID_MPEG_VIDEO_H264_HIERARCHICAL_CODING) &&
            published(controls, UML_V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L0_BR) &&
            published(controls, UML_V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L1_BR), "H.264 temporal layers");
    // HEVC: no layers (one layer in the capability), but profile, tier and HDR metadata.
    const auto& hevcProfile = control(controls, UML_V4L2_CID_MPEG_VIDEO_HEVC_PROFILE);
    require(hevcProfile.maximum == UML_V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10 &&
            hevcProfile.menu_skip_mask == menuBit(UML_V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_STILL_PICTURE) &&
            hevcProfile.default_value == UML_V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN && hevcProfile.coded_formats == hevcOnly,
            "HEVC profile menu");
    require(control(controls, UML_V4L2_CID_MPEG_VIDEO_HEVC_LEVEL).maximum == UML_V4L2_MPEG_VIDEO_HEVC_LEVEL_6_2 &&
            control(controls, UML_V4L2_CID_MPEG_VIDEO_HEVC_LEVEL).default_value == UML_V4L2_MPEG_VIDEO_HEVC_LEVEL_1,
            "HEVC level menu runs to the host's level and defaults to the lowest");
    const auto& tier = control(controls, UML_V4L2_CID_MPEG_VIDEO_HEVC_TIER);
    require(tier.maximum == UML_V4L2_MPEG_VIDEO_HEVC_TIER_HIGH && tier.menu_skip_mask == menuBit(UML_V4L2_MPEG_VIDEO_HEVC_TIER_HIGH) &&
            tier.default_value == UML_V4L2_MPEG_VIDEO_HEVC_TIER_MAIN, "HEVC tier is a menu with one usable item");
    require(!published(controls, UML_V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP), "HEVC frame quantizers offered without CQP");
    require(!published(controls, UML_V4L2_CID_MPEG_VIDEO_HEVC_HIER_CODING_LAYER), "HEVC layers offered with one layer");
    const auto& light = control(controls, UML_V4L2_CID_COLORIMETRY_HDR10_CLL_INFO);
    const auto& display = control(controls, UML_V4L2_CID_COLORIMETRY_HDR10_MASTERING_DISPLAY);
    require(light.elem_size == 4 && display.elem_size == 24 && light.coded_formats == hevcOnly, "HDR10 compound controls");
    // JPEG and the buffer-count control.
    const auto& jpegQuality = control(controls, UML_V4L2_CID_JPEG_COMPRESSION_QUALITY);
    require(jpegQuality.minimum == 1 && jpegQuality.maximum == 100 && jpegQuality.default_value == 85 &&
            jpegQuality.coded_formats == codedBit(VIRTIO_VIDEO_FORMAT_MJPEG), "JPEG quality");
    const auto& buffers = control(controls, UML_V4L2_CID_MIN_BUFFERS_FOR_OUTPUT);
    require(buffers.minimum == 2 && buffers.maximum == 2 && (buffers.flags & VIRTIO_VIDEO_V4L2_CTRL_F_READ_ONLY),
            "the minimum raw buffer count is fixed and read-only");

    // A host with a single codec publishes only that codec's controls.
    const EncoderControls hevcHost(std::vector<VideoEncodeCapability> {capabilities[1]});
    require(!published(hevcHost, UML_V4L2_CID_MPEG_VIDEO_H264_PROFILE) && !published(hevcHost, UML_V4L2_CID_JPEG_COMPRESSION_QUALITY) &&
            published(hevcHost, UML_V4L2_CID_MPEG_VIDEO_HEVC_PROFILE), "controls of codecs the host lacks");
    require(control(hevcHost, UML_V4L2_CID_MPEG_VIDEO_BITRATE).maximum == 80'000'000, "bitrate ceiling of a lone codec");
    const EncoderControls h264Host(std::vector<VideoEncodeCapability> {capabilities[0]});
    require(published(h264Host, UML_V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE) &&
            control(h264Host, UML_V4L2_CID_MPEG_VIDEO_BITRATE).maximum == 100'000'000, "a lone H.264 offers its own limits");
    const EncoderControls jpegHost(std::vector<VideoEncodeCapability> {capabilities[2]});
    require(jpegHost.descriptors().size() == 2 && published(jpegHost, UML_V4L2_CID_JPEG_COMPRESSION_QUALITY) &&
            published(jpegHost, UML_V4L2_CID_MIN_BUFFERS_FOR_OUTPUT), "a JPEG-only host publishes two controls");
    require(EncoderControls(std::span<const VideoEncodeCapability> {}).descriptors().empty(),
            "a host without codecs publishes controls");

    // A capability without CBR or VBR offers no bitrate to set; one without B-frames no B-frame control.
    auto cqOnly = capabilities[0];
    cqOnly.rateControls = {VideoRateControl::constantQuality};
    cqOnly.maximumBFrames = 0;
    cqOnly.maximumTemporalLayers = 1;
    cqOnly.entropyModeSelectable = false;
    const EncoderControls cq(std::vector<VideoEncodeCapability> {cqOnly});
    const auto& cqMode = control(cq, UML_V4L2_CID_MPEG_VIDEO_BITRATE_MODE);
    require(cqMode.default_value == UML_V4L2_MPEG_VIDEO_BITRATE_MODE_CQ &&
            cqMode.menu_skip_mask == (menuBit(UML_V4L2_MPEG_VIDEO_BITRATE_MODE_VBR) | menuBit(UML_V4L2_MPEG_VIDEO_BITRATE_MODE_CBR)),
            "a quality-only host defaults to quality and skips the bitrate modes");
    require(!published(cq, UML_V4L2_CID_MPEG_VIDEO_BITRATE) && !published(cq, UML_V4L2_CID_MPEG_VIDEO_BITRATE_PEAK) &&
            !published(cq, UML_V4L2_CID_MPEG_VIDEO_B_FRAMES) && !published(cq, UML_V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE) &&
            !published(cq, UML_V4L2_CID_MPEG_VIDEO_H264_HIERARCHICAL_CODING_LAYER) &&
            published(cq, UML_V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY), "controls the host cannot honour were offered");

    // A B-frame run belongs to the codecs that have one. A host whose H.264 alone reorders offers the control for
    // H.264 alone, to the length of its run; one where neither does offers none.
    auto h264Reorders = capabilities;
    h264Reorders[1].maximumBFrames = 0;
    const EncoderControls h264RunHost(h264Reorders);
    const auto& h264Run = control(h264RunHost, UML_V4L2_CID_MPEG_VIDEO_B_FRAMES);
    require(h264Run.maximum == 3 && h264Run.coded_formats == codedBit(VIRTIO_VIDEO_FORMAT_H264),
            "the B-frame control reached a codec that codes none");
    auto hevcReorders = capabilities;
    hevcReorders[0].maximumBFrames = 0;
    const EncoderControls hevcRunHost(hevcReorders);
    const auto& hevcRun = control(hevcRunHost, UML_V4L2_CID_MPEG_VIDEO_B_FRAMES);
    require(hevcRun.maximum == 2 && hevcRun.coded_formats == codedBit(VIRTIO_VIDEO_FORMAT_HEVC),
            "a host whose HEVC alone reorders offers its run");
    auto neither = capabilities;
    neither[0].maximumBFrames = 0;
    neither[1].maximumBFrames = 0;
    require(!published(EncoderControls(neither), UML_V4L2_CID_MPEG_VIDEO_B_FRAMES), "B-frames offered where no codec reorders");

    // Baseline-only H.264 has no 8x8 transform.
    auto baseline = capabilities[0];
    baseline.profiles.resize(1);
    require(!published(EncoderControls(std::vector<VideoEncodeCapability> {baseline}), UML_V4L2_CID_MPEG_VIDEO_H264_8X8_TRANSFORM),
            "8x8 transform without a High profile");
    // Without a measured level the menu runs to the table's usual top.
    auto unleveled = capabilities[0];
    for (auto& entry : unleveled.profiles) entry.maximumLevel = 0;
    require(control(EncoderControls(std::vector<VideoEncodeCapability> {unleveled}), UML_V4L2_CID_MPEG_VIDEO_H264_LEVEL).default_value ==
            UML_V4L2_MPEG_VIDEO_H264_LEVEL_1_0, "default level");
    require(control(EncoderControls(std::vector<VideoEncodeCapability> {unleveled}), UML_V4L2_CID_MPEG_VIDEO_H264_LEVEL).maximum ==
            UML_V4L2_MPEG_VIDEO_H264_LEVEL_5_2, "top level");
    // A selectable HDR host without Main10 has no HDR controls.
    auto sdr = capabilities[1];
    sdr.profiles.resize(1);
    require(!published(EncoderControls(std::vector<VideoEncodeCapability> {sdr}), UML_V4L2_CID_COLORIMETRY_HDR10_CLL_INFO),
            "HDR metadata without Main10");
}

void valueChecks()
{
    const EncoderControls controls(encoderHostCapabilities());
    const auto check = [&controls](std::uint32_t id, std::int64_t value, std::size_t payload = 0) {
        return controls.check(id, value, payload);
    };
    require(check(0x12345678, 0) == ControlCheck::unsupported, "an unknown control was accepted");
    // Integer: range and step.
    require(check(UML_V4L2_CID_MPEG_VIDEO_BITRATE, 20'000) == ControlCheck::ok &&
            check(UML_V4L2_CID_MPEG_VIDEO_BITRATE, 80'000'000) == ControlCheck::ok &&
            check(UML_V4L2_CID_MPEG_VIDEO_BITRATE, 19'999) == ControlCheck::invalid &&
            check(UML_V4L2_CID_MPEG_VIDEO_BITRATE, 999) == ControlCheck::invalid &&
            check(UML_V4L2_CID_MPEG_VIDEO_BITRATE, 80'000'001) == ControlCheck::invalid &&
            check(UML_V4L2_CID_MPEG_VIDEO_BITRATE, -1) == ControlCheck::invalid, "integer range");
    require(check(UML_V4L2_CID_MPEG_VIDEO_B_FRAMES, 2) == ControlCheck::ok && check(UML_V4L2_CID_MPEG_VIDEO_B_FRAMES, 3) == ControlCheck::ok &&
            check(UML_V4L2_CID_MPEG_VIDEO_B_FRAMES, 4) == ControlCheck::invalid, "B-frame ceiling");
    // Boolean.
    require(check(UML_V4L2_CID_MPEG_VIDEO_MB_RC_ENABLE, 0) == ControlCheck::ok && check(UML_V4L2_CID_MPEG_VIDEO_MB_RC_ENABLE, 1) == ControlCheck::ok &&
            check(UML_V4L2_CID_MPEG_VIDEO_MB_RC_ENABLE, 2) == ControlCheck::invalid, "boolean values");
    // Menu: range, and the skipped items.
    require(check(UML_V4L2_CID_MPEG_VIDEO_H264_PROFILE, UML_V4L2_MPEG_VIDEO_H264_PROFILE_HIGH) == ControlCheck::ok &&
            check(UML_V4L2_CID_MPEG_VIDEO_H264_PROFILE, UML_V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE) == ControlCheck::invalid &&
            check(UML_V4L2_CID_MPEG_VIDEO_H264_PROFILE, UML_V4L2_MPEG_VIDEO_H264_PROFILE_HIGH + 1) == ControlCheck::invalid &&
            check(UML_V4L2_CID_MPEG_VIDEO_H264_PROFILE, -1) == ControlCheck::invalid, "menu items outside the published set");
    require(check(UML_V4L2_CID_MPEG_VIDEO_HEVC_TIER, UML_V4L2_MPEG_VIDEO_HEVC_TIER_HIGH) == ControlCheck::invalid,
            "the HEVC high tier is not encodable");
    // A button takes any value, a compound control only its own size.
    require(check(UML_V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1) == ControlCheck::ok, "button");
    require(check(UML_V4L2_CID_COLORIMETRY_HDR10_CLL_INFO, 0, 4) == ControlCheck::ok &&
            check(UML_V4L2_CID_COLORIMETRY_HDR10_CLL_INFO, 0, 3) == ControlCheck::invalid &&
            check(UML_V4L2_CID_COLORIMETRY_HDR10_MASTERING_DISPLAY, 0, 24) == ControlCheck::ok &&
            check(UML_V4L2_CID_COLORIMETRY_HDR10_MASTERING_DISPLAY, 0, 0) == ControlCheck::invalid, "compound payload size");

    const auto defaults = controls.defaults();
    require(defaults.scalars.at(UML_V4L2_CID_MPEG_VIDEO_BITRATE) == 2'000'000 &&
            defaults.scalars.at(UML_V4L2_CID_MPEG_VIDEO_H264_PROFILE) == UML_V4L2_MPEG_VIDEO_H264_PROFILE_HIGH &&
            defaults.scalars.at(UML_V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR) == 1, "defaults");
    require(!defaults.scalars.contains(UML_V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME) &&
            !defaults.scalars.contains(UML_V4L2_CID_COLORIMETRY_HDR10_CLL_INFO) && defaults.payloads.empty(),
            "a button or a compound control has no scalar default");
    for (const auto& descriptor : controls.descriptors())
        if (descriptor.elem_size == 0 && descriptor.type != UML_V4L2_CTRL_TYPE_BUTTON)
            require(defaults.scalars.contains(descriptor.id) &&
                    controls.check(descriptor.id, defaults.scalars.at(descriptor.id), 0) == ControlCheck::ok,
                    "a default the control itself refuses");
    require(isDynamic(control(controls, UML_V4L2_CID_MPEG_VIDEO_BITRATE)) && !isDynamic(control(controls, UML_V4L2_CID_MPEG_VIDEO_H264_PROFILE)),
            "dynamic flag");
}

void levels()
{
    require(levelIdc(VideoCodec::h264, UML_V4L2_MPEG_VIDEO_H264_LEVEL_4_1) == 41 &&
            levelIdc(VideoCodec::h264, UML_V4L2_MPEG_VIDEO_H264_LEVEL_1B) == 9 &&
            levelIdc(VideoCodec::h264, UML_V4L2_MPEG_VIDEO_H264_LEVEL_6_2) == 62, "H.264 level_idc");
    require(levelIdc(VideoCodec::hevc, UML_V4L2_MPEG_VIDEO_HEVC_LEVEL_4_1) == 123 &&
            levelIdc(VideoCodec::hevc, UML_V4L2_MPEG_VIDEO_HEVC_LEVEL_6_2) == 186 &&
            levelIdc(VideoCodec::hevc, UML_V4L2_MPEG_VIDEO_HEVC_LEVEL_1) == 30, "HEVC general_level_idc");
    require(levelIdc(VideoCodec::h264, 255) == 0 && levelIdc(VideoCodec::jpeg, 0) == 0 && levelIdc(VideoCodec::h264, -1) == 0,
            "an unknown level has no idc");
}

void streamInfo()
{
    StreamInfo info;
    info.colorspace = UML_V4L2_COLORSPACE_REC709;
    info.transfer = UML_V4L2_XFER_FUNC_709;
    info.matrix = UML_V4L2_YCBCR_ENC_709;
    info.quantization = UML_V4L2_QUANTIZATION_LIM_RANGE;
    require(sanitized(info) == info, "supported colorimetry changed");
    // RAW is the client's way to state no standard (the V4L2 core resets the other fields when the colorspace is
    // DEFAULT), so it passes with every other field.
    for (const auto transfer : {UML_V4L2_XFER_FUNC_DEFAULT, UML_V4L2_XFER_FUNC_NONE, UML_V4L2_XFER_FUNC_SMPTE2084}) {
        StreamInfo raw;
        raw.colorspace = UML_V4L2_COLORSPACE_RAW;
        raw.transfer = transfer;
        raw.matrix = UML_V4L2_YCBCR_ENC_BT2020;
        raw.quantization = UML_V4L2_QUANTIZATION_FULL_RANGE;
        require(sanitized(raw) == raw, "a RAW colorspace was dropped or changed its neighbours");
    }
    info.colorspace = 99;
    info.transfer = 99;
    info.matrix = 99;
    info.quantization = 99;
    const auto cleaned = sanitized(info);
    require(cleaned.colorspace == 0 && cleaned.transfer == 0 && cleaned.matrix == 0 && cleaned.quantization == 0,
            "unsupported colorimetry was kept");
    require(cleaned.intervalNumerator == info.intervalNumerator && cleaned.intervalDenominator == info.intervalDenominator,
            "the frame interval changed");
}

EncodeStream stream(VideoCodec codec, RawVideoLayout layout = RawVideoLayout::nv12)
{
    EncodeStream result;
    result.codec = codec;
    result.input = layout;
    result.width = 1280;
    result.height = 720;
    // The host's B-frame runs: three in H.264, two in HEVC, none in JPEG.
    result.maximumBFrames = codec == VideoCodec::h264 ? 3 : codec == VideoCodec::hevc ? 2 : 0;
    return result;
}

void settingsFromControls()
{
    const EncoderControls controls(encoderHostCapabilities());
    const auto defaults = controls.defaults();

    // Every default maps to the settings of a plain live stream.
    const auto plain = encodeSettings(stream(VideoCodec::h264), defaults);
    require(plain.codec == VideoCodec::h264 && plain.profile == VideoEncodeProfile::h264High && plain.level == 10 &&
            plain.width == 1280 && plain.height == 720 && plain.input == RawVideoLayout::nv12, "default stream shape");
    require(plain.rateControl == VideoRateControl::variableBitrate && plain.bitrate == 2'000'000 && plain.peakBitrate == 0 &&
            plain.quality == 50 && plain.frameRateNumerator == 30 && plain.frameRateDenominator == 1,
            "default rate control and frame rate");
    require(!plain.minimumQp && !plain.maximumQp, "quantizer bounds left at the published limits are no request");
    require(plain.keyFrameInterval == 0 && plain.bFrames == 0 && plain.temporalLayers == 1 && plain.cabac &&
            plain.bufferBits == 0 && plain.realTime, "default structure");
    require(!plain.separateParameterSets && plain.parameterSetsWithEveryKeyFrame && !plain.hdr, "default headers");

    // Values set by the guest.
    auto values = defaults;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_H264_PROFILE] = UML_V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_H264_LEVEL] = UML_V4L2_MPEG_VIDEO_H264_LEVEL_4_1;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_BITRATE_MODE] = UML_V4L2_MPEG_VIDEO_BITRATE_MODE_CBR;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_BITRATE] = 5'000'000;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_BITRATE_PEAK] = 6'000'000;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_GOP_SIZE] = 60;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_B_FRAMES] = 3;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_H264_MIN_QP] = 10;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_H264_MAX_QP] = 40;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE] = UML_V4L2_MPEG_VIDEO_H264_ENTROPY_MODE_CAVLC;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_H264_CPB_SIZE] = 100;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_HEADER_MODE] = UML_V4L2_MPEG_VIDEO_HEADER_MODE_SEPARATE;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_H264_HIERARCHICAL_CODING] = 1;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_H264_HIERARCHICAL_CODING_LAYER] = 2;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L0_BR] = 600;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_H264_HIER_CODING_L1_BR] = 400;
    auto request = stream(VideoCodec::h264, RawVideoLayout::i420);
    request.info.intervalNumerator = 1001;
    request.info.intervalDenominator = 60000;
    const auto set = encodeSettings(request, values);
    require(set.profile == VideoEncodeProfile::h264Main && set.level == 41 && set.input == RawVideoLayout::i420,
            "profile, level and input");
    require(set.rateControl == VideoRateControl::constantBitrate && set.bitrate == 5'000'000 && set.peakBitrate == 6'000'000,
            "rate control");
    require(set.frameRateNumerator == 60000 && set.frameRateDenominator == 1001, "a frame interval is a frame rate inverted");
    require(set.keyFrameInterval == 60 && set.bFrames == 3 && !set.realTime, "B-frames end the real-time behaviour");
    // The platform codes B-frames as a whole run or not at all, so the device asks for either: a request below the
    // run is none (and the stream stays a live one), one at the run is the run, and a longer one the codec's own run.
    for (const auto [codec, asked, coded] : {std::tuple {VideoCodec::h264, 0, 0}, std::tuple {VideoCodec::h264, 1, 0},
                                             std::tuple {VideoCodec::h264, 2, 0}, std::tuple {VideoCodec::h264, 3, 3},
                                             std::tuple {VideoCodec::hevc, 1, 0}, std::tuple {VideoCodec::hevc, 2, 2},
                                             std::tuple {VideoCodec::hevc, 3, 2}}) {
        auto frames = defaults;
        frames.scalars[UML_V4L2_CID_MPEG_VIDEO_B_FRAMES] = asked;
        const auto effective = encodeSettings(stream(codec), frames);
        require(effective.bFrames == std::uint32_t(coded) && effective.realTime == (coded == 0),
                "a B-frame request is not the run or none");
    }
    auto none = stream(VideoCodec::h264);
    none.maximumBFrames = 0;
    auto reorder = defaults;
    reorder.scalars[UML_V4L2_CID_MPEG_VIDEO_B_FRAMES] = 3;
    require(encodeSettings(none, reorder).bFrames == 0, "B-frames asked of a host that codes none");
    require(set.minimumQp == 10u && set.maximumQp == 40u && !set.cabac, "quantizer bounds and entropy mode");
    require(set.bufferBits == 100u * 8192, "the CPB size is in kilobytes");
    require(set.separateParameterSets && set.parameterSetsWithEveryKeyFrame, "separate headers");
    require(set.temporalLayers == 2 && set.baseLayerBitratePercent == 60, "the base layer's share of the bitrate");

    // The switch that turns frame-level rate control off selects fixed quantizers.
    values = defaults;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE] = 0;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP] = 20;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP] = 24;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_H264_B_FRAME_QP] = 28;
    const auto fixed = encodeSettings(stream(VideoCodec::h264), values);
    require(fixed.rateControl == VideoRateControl::constantQp && fixed.frameQp == (std::array<std::uint32_t, 3> {20, 24, 28}),
            "constant quantizer");
    values = defaults;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_BITRATE_MODE] = UML_V4L2_MPEG_VIDEO_BITRATE_MODE_CQ;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY] = 70;
    const auto quality = encodeSettings(stream(VideoCodec::h264), values);
    require(quality.rateControl == VideoRateControl::constantQuality && quality.quality == 70 && !quality.realTime,
            "constant quality is not a live stream");

    // HEVC reads its own controls, not H.264's.
    values = defaults;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_H264_PROFILE] = UML_V4L2_MPEG_VIDEO_H264_PROFILE_MAIN;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_HEVC_PROFILE] = UML_V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_HEVC_LEVEL] = UML_V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP] = 45;
    const auto hevc = encodeSettings(stream(VideoCodec::hevc, RawVideoLayout::p010), values);
    require(hevc.profile == VideoEncodeProfile::hevcMain10 && hevc.level == 153 && hevc.input == RawVideoLayout::p010 &&
            hevc.maximumQp == 45u && !hevc.minimumQp, "HEVC settings");
    require(!hevc.hdr, "HEVC without HDR metadata asked for HDR");
    std::vector<std::uint8_t> light(4), display(24);
    light[0] = 0xe8; light[1] = 0x03; light[2] = 0x90; light[3] = 0x01;                  // 1000, 400
    for (std::size_t i = 0; i < 6; ++i) { display[2 * i] = std::uint8_t(i + 1); display[2 * i + 1] = 0; }
    display[12] = 0x10; display[14] = 0x20;                                              // white point
    display[16] = 0x40; display[17] = 0x42; display[18] = 0x0f;                          // 1,000,000
    display[20] = 0x05;                                                                  // 5
    values.payloads[UML_V4L2_CID_COLORIMETRY_HDR10_CLL_INFO] = light;
    values.payloads[UML_V4L2_CID_COLORIMETRY_HDR10_MASTERING_DISPLAY] = display;
    const auto hdr = encodeSettings(stream(VideoCodec::hevc, RawVideoLayout::p010), values);
    require(hdr.hdr.has_value(), "HDR metadata was dropped");
    require(hdr.hdr->maximumContentLightLevel == 1000 && hdr.hdr->maximumFrameAverageLightLevel == 400, "content light level");
    // x coordinates are the first three values and y the next three, interleaved per primary.
    require(hdr.hdr->displayPrimaries == (std::array<std::uint16_t, 6> {1, 4, 2, 5, 3, 6}), "mastering display primaries");
    require(hdr.hdr->whitePointX == 0x10 && hdr.hdr->whitePointY == 0x20 && hdr.hdr->maximumLuminance == 1'000'000 &&
            hdr.hdr->minimumLuminance == 5, "mastering display white point and luminance");
    // H.264 never carries HDR metadata.
    require(!encodeSettings(stream(VideoCodec::h264), values).hdr, "H.264 stream got HDR metadata");

    // JPEG uses its own quality and ignores the video controls.
    values = defaults;
    values.scalars[UML_V4L2_CID_JPEG_COMPRESSION_QUALITY] = 60;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_BITRATE] = 9'000'000;
    const auto jpeg = encodeSettings(stream(VideoCodec::jpeg), values);
    require(jpeg.profile == VideoEncodeProfile::jpegBaseline && jpeg.jpegQuality == 60 &&
            jpeg.rateControl == VideoRateControl::constantQuality, "JPEG settings");

    // Colorimetry: V4L2's defaults follow the colorspace.
    EncodeStream colored = stream(VideoCodec::h264);
    colored.info.colorspace = UML_V4L2_COLORSPACE_REC709;
    auto color = encodeSettings(colored, defaults).color;
    require(color.primaries == VideoColorPrimaries::bt709 && color.transfer == VideoTransferFunction::bt709 &&
            color.matrix == VideoMatrixCoefficients::bt709 && !color.fullRange, "Rec.709 defaults");
    colored.info.colorspace = UML_V4L2_COLORSPACE_SRGB;
    color = encodeSettings(colored, defaults).color;
    require(color.primaries == VideoColorPrimaries::bt709 && color.transfer == VideoTransferFunction::srgb &&
            color.matrix == VideoMatrixCoefficients::bt601 && color.fullRange, "sRGB defaults to full range");
    colored.info.colorspace = UML_V4L2_COLORSPACE_BT2020;
    colored.info.transfer = UML_V4L2_XFER_FUNC_SMPTE2084;
    colored.info.quantization = UML_V4L2_QUANTIZATION_LIM_RANGE;
    color = encodeSettings(colored, defaults).color;
    require(color.primaries == VideoColorPrimaries::bt2020 && color.transfer == VideoTransferFunction::pq &&
            color.matrix == VideoMatrixCoefficients::bt2020 && !color.fullRange, "BT.2020 PQ");
    colored.info = StreamInfo {};
    color = encodeSettings(colored, defaults).color;
    require(color == VideoEncodeColor {}, "unspecified colorimetry must stay unspecified");

    // RAW states no standard, and then nothing is derived from one: DEFAULT is unspecified (or limited range), so a
    // client with a transfer function or a range alone states exactly that.
    const auto rawColor = [&](std::uint32_t transfer, std::uint32_t matrix, std::uint32_t quantization) {
        EncodeStream raw = stream(VideoCodec::hevc);
        raw.info.colorspace = UML_V4L2_COLORSPACE_RAW;
        raw.info.transfer = transfer;
        raw.info.matrix = matrix;
        raw.info.quantization = quantization;
        return encodeSettings(raw, defaults).color;
    };
    require(rawColor(UML_V4L2_XFER_FUNC_DEFAULT, UML_V4L2_YCBCR_ENC_DEFAULT, UML_V4L2_QUANTIZATION_DEFAULT) == VideoEncodeColor {},
            "RAW with every field at its default states a colour");
    color = rawColor(UML_V4L2_XFER_FUNC_DEFAULT, UML_V4L2_YCBCR_ENC_709, UML_V4L2_QUANTIZATION_DEFAULT);
    require(color.primaries == VideoColorPrimaries::unspecified && color.transfer == VideoTransferFunction::unspecified &&
            color.matrix == VideoMatrixCoefficients::bt709 && !color.fullRange, "RAW: a default transfer function is unspecified");
    color = rawColor(UML_V4L2_XFER_FUNC_NONE, UML_V4L2_YCBCR_ENC_DEFAULT, UML_V4L2_QUANTIZATION_DEFAULT);
    require(color.primaries == VideoColorPrimaries::unspecified && color.transfer == VideoTransferFunction::linear &&
            color.matrix == VideoMatrixCoefficients::unspecified && !color.fullRange, "RAW: no transfer function is linear");
    color = rawColor(UML_V4L2_XFER_FUNC_SMPTE2084, UML_V4L2_YCBCR_ENC_DEFAULT, UML_V4L2_QUANTIZATION_DEFAULT);
    require(color.primaries == VideoColorPrimaries::unspecified && color.transfer == VideoTransferFunction::pq &&
            color.matrix == VideoMatrixCoefficients::unspecified && !color.fullRange, "RAW: PQ without a standard or a matrix");
    color = rawColor(UML_V4L2_XFER_FUNC_709, UML_V4L2_YCBCR_ENC_DEFAULT, UML_V4L2_QUANTIZATION_DEFAULT);
    require(color.transfer == VideoTransferFunction::bt709 && color.primaries == VideoColorPrimaries::unspecified &&
            color.matrix == VideoMatrixCoefficients::unspecified, "RAW: an explicit transfer function is kept");
    color = rawColor(UML_V4L2_XFER_FUNC_SRGB, UML_V4L2_YCBCR_ENC_DEFAULT, UML_V4L2_QUANTIZATION_DEFAULT);
    require(color.transfer == VideoTransferFunction::srgb && color.primaries == VideoColorPrimaries::unspecified &&
            color.matrix == VideoMatrixCoefficients::unspecified && !color.fullRange,
            "RAW: sRGB is a transfer function here, not a standard that implies its matrix and range");
    color = rawColor(UML_V4L2_XFER_FUNC_DEFAULT, UML_V4L2_YCBCR_ENC_BT2020, UML_V4L2_QUANTIZATION_DEFAULT);
    require(color.matrix == VideoMatrixCoefficients::bt2020 && color.primaries == VideoColorPrimaries::unspecified &&
            color.transfer == VideoTransferFunction::unspecified, "RAW: an explicit matrix is kept");
    color = rawColor(UML_V4L2_XFER_FUNC_DEFAULT, UML_V4L2_YCBCR_ENC_601, UML_V4L2_QUANTIZATION_DEFAULT);
    require(color.matrix == VideoMatrixCoefficients::bt601, "RAW: an explicit 601 matrix is kept");
    color = rawColor(UML_V4L2_XFER_FUNC_DEFAULT, UML_V4L2_YCBCR_ENC_DEFAULT, UML_V4L2_QUANTIZATION_FULL_RANGE);
    require(color.fullRange && color.primaries == VideoColorPrimaries::unspecified && color.transfer == VideoTransferFunction::unspecified &&
            color.matrix == VideoMatrixCoefficients::unspecified, "RAW: full range alone");
    color = rawColor(UML_V4L2_XFER_FUNC_DEFAULT, UML_V4L2_YCBCR_ENC_DEFAULT, UML_V4L2_QUANTIZATION_LIM_RANGE);
    require(!color.fullRange, "RAW: limited range is limited");
    // Every aspect at once, as an HDR10 client states it with no standard of its own.
    color = rawColor(UML_V4L2_XFER_FUNC_SMPTE2084, UML_V4L2_YCBCR_ENC_BT2020, UML_V4L2_QUANTIZATION_LIM_RANGE);
    require(color == (VideoEncodeColor {VideoColorPrimaries::unspecified, VideoTransferFunction::pq,
                                        VideoMatrixCoefficients::bt2020, false}), "RAW with PQ and BT.2020");
    // Hybrid log-gamma is a transfer function of its own, with or without a standard beside it.
    color = rawColor(UML_V4L2_XFER_FUNC_HLG, UML_V4L2_YCBCR_ENC_BT2020, UML_V4L2_QUANTIZATION_LIM_RANGE);
    require(color == (VideoEncodeColor {VideoColorPrimaries::unspecified, VideoTransferFunction::hlg,
                                        VideoMatrixCoefficients::bt2020, false}), "RAW with HLG and BT.2020");
    colored.info = StreamInfo {};
    colored.info.colorspace = UML_V4L2_COLORSPACE_BT2020;
    colored.info.transfer = UML_V4L2_XFER_FUNC_HLG;
    colored.info.matrix = UML_V4L2_YCBCR_ENC_BT2020;
    require(encodeSettings(colored, defaults).color == (VideoEncodeColor {VideoColorPrimaries::bt2020, VideoTransferFunction::hlg,
                                                                          VideoMatrixCoefficients::bt2020, false}),
            "BT.2020 with HLG states the BT.709 transfer function its standard implies");
    // The standards still derive their own defaults (the cases above, and sRGB's full range).
    colored.info = StreamInfo {};
    colored.info.colorspace = UML_V4L2_COLORSPACE_BT2020;
    require(encodeSettings(colored, defaults).color == (VideoEncodeColor {VideoColorPrimaries::bt2020, VideoTransferFunction::bt709,
                                                                          VideoMatrixCoefficients::bt2020, false}),
            "BT.2020 without a transfer function or a matrix lost its defaults");
}

void updates()
{
    const EncoderControls controls(encoderHostCapabilities());
    auto values = controls.defaults();
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_BITRATE] = 3'000'000;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_BITRATE_PEAK] = 4'000'000;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY] = 80;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_H264_MAX_QP] = 44;
    values.scalars[UML_V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP] = 46;
    values.scalars[UML_V4L2_CID_JPEG_COMPRESSION_QUALITY] = 55;
    const std::vector<std::uint32_t> all {UML_V4L2_CID_MPEG_VIDEO_BITRATE, UML_V4L2_CID_MPEG_VIDEO_BITRATE_PEAK,
                                          UML_V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY, UML_V4L2_CID_MPEG_VIDEO_H264_MAX_QP,
                                          UML_V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP, UML_V4L2_CID_JPEG_COMPRESSION_QUALITY,
                                          UML_V4L2_CID_MPEG_VIDEO_GOP_SIZE};
    const auto h264 = encodeUpdate(stream(VideoCodec::h264), all, values);
    require(h264.bitrate == 3'000'000u && h264.peakBitrate == 4'000'000u && h264.quality == 80u && h264.maximumQp == 44u &&
            h264.jpegQuality == 55u && !h264.frameRateNumerator, "H.264 update");
    const auto hevc = encodeUpdate(stream(VideoCodec::hevc), all, values);
    require(hevc.maximumQp == 46u, "HEVC update reads its own maximum quantizer");
    const std::vector<std::uint32_t> only {UML_V4L2_CID_MPEG_VIDEO_BITRATE};
    const auto narrow = encodeUpdate(stream(VideoCodec::h264), only, values);
    require(narrow.bitrate == 3'000'000u && !narrow.peakBitrate && !narrow.quality && !narrow.maximumQp && !narrow.jpegQuality,
            "an update names only the controls that changed");
    require(!encodeUpdate(stream(VideoCodec::h264), {}, values).bitrate, "an empty update changed something");
}

/* The platform's own discovery on this host, through the device's mapping.
 * Every other case here feeds the mapping a capability list written by hand;
 * this one proves the lists the device publishes are what the hardware did.
 * Which layouts and profiles exist depends on the host, so a host without a
 * feature has the feature absent from the wire, not the case failing. */
void realHost()
{
    const auto capabilities = videoEncodeCapabilities();
    if (capabilities.empty()) {
        std::puts("  no hardware video encoder on this host; skipped");
        return;
    }
    const auto hasLayout = [&](VideoCodec codec, RawVideoLayout layout) {
        for (const auto& capability : capabilities)
            if (capability.codec == codec)
                for (const auto& profile : capability.profiles)
                    if (std::find(profile.inputs.begin(), profile.inputs.end(), layout) != profile.inputs.end()) return true;
        return false;
    };
    const auto codecs = {VideoCodec::h264, VideoCodec::hevc, VideoCodec::jpeg};
    const auto raws = {RawVideoLayout::nv12, RawVideoLayout::i420, RawVideoLayout::p010, RawVideoLayout::bgra,
                       RawVideoLayout::rgba, RawVideoLayout::nv16, RawVideoLayout::nv24};

    // The input queue lists a raw format exactly when some profile of some codec encoded from it, and its mask
    // names exactly the codecs that did.
    const auto coded = advertisedCodedFormats(capabilities);
    const auto inputs = parse(encoderCapabilityResponse(capabilities, input));
    std::vector<std::uint32_t> expectedFormats;
    for (const auto layout : raws) {
        bool any = false;
        for (const auto codec : codecs) any = any || hasLayout(codec, layout);
        if (any) expectedFormats.push_back(wireRawFormat(layout));
    }
    std::vector<std::uint32_t> listed;
    for (const auto& entry : inputs) listed.push_back(entry.desc.format);
    require(listed == expectedFormats, "the input queue is not the layouts the hardware encoded from");
    for (const auto& entry : inputs) {
        const auto layout = platformRawLayout(entry.desc.format);
        require(layout.has_value(), "a listed raw format is not an encoder input");
        std::uint64_t mask = 0;
        for (std::size_t index = 0; index < coded.size(); ++index)
            if (hasLayout(*encodedCodec(coded[index]), *layout)) mask |= std::uint64_t(1) << index;
        require(entry.desc.mask == mask, "a raw format's mask is not the codecs that encoded from it");
        const bool single = *layout == RawVideoLayout::p010 || *layout == RawVideoLayout::bgra || *layout == RawVideoLayout::rgba;
        require(single ? entry.desc.planes_layout == VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER : entry.desc.planes_layout != 0,
                "a packed or ten-bit format is not a single buffer");
    }

    // HEVC Main 10, and with it P010 input and the HDR10 controls, is on the wire exactly when the hardware has it.
    const EncoderControls controls(capabilities);
    bool hevc = false, main10Profile = false, hdr = false;
    for (const auto& capability : capabilities) {
        if (capability.codec != VideoCodec::hevc) continue;
        hevc = true;
        for (const auto& profile : capability.profiles) main10Profile = main10Profile || profile.profile == VideoEncodeProfile::hevcMain10;
        hdr = main10Profile && capability.hdrStaticMetadata;
    }
    const bool main10 = main10Profile && hasLayout(VideoCodec::hevc, RawVideoLayout::p010);
    require(!hasLayout(VideoCodec::h264, RawVideoLayout::p010) && !hasLayout(VideoCodec::jpeg, RawVideoLayout::p010),
            "10-bit input is encoded by something other than HEVC");
    require(std::count(listed.begin(), listed.end(), VIRTIO_VIDEO_FORMAT_P010) == (hasLayout(VideoCodec::hevc, RawVideoLayout::p010) ? 1 : 0),
            "P010 is listed on a host that did not encode from it, or missing on one that did");
    require(published(controls, UML_V4L2_CID_MPEG_VIDEO_HEVC_PROFILE) == hevc, "the HEVC profile control does not follow the HEVC capability");
    if (hevc) {
        const auto& profile = control(controls, UML_V4L2_CID_MPEG_VIDEO_HEVC_PROFILE);
        const bool offered = profile.maximum >= UML_V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10 &&
            ((profile.menu_skip_mask >> UML_V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10) & 1) == 0;
        require(offered == main10Profile, "Main 10 is offered without hardware for it, or hidden with it");
    }
    require(published(controls, UML_V4L2_CID_COLORIMETRY_HDR10_CLL_INFO) == hdr &&
            published(controls, UML_V4L2_CID_COLORIMETRY_HDR10_MASTERING_DISPLAY) == hdr,
            "the HDR10 controls do not follow the hardware's HDR metadata");
    // B-frames follow the platform's run: the control runs to the longest run, for the codecs that have one.
    std::uint32_t longestRun = 0, runFormats = 0;
    for (const auto& capability : capabilities) {
        if (capability.codec == VideoCodec::jpeg) continue;
        longestRun = std::max(longestRun, capability.maximumBFrames);
        if (capability.maximumBFrames) runFormats |= capability.codec == VideoCodec::h264 ? 1u : 2u;
    }
    require(published(controls, UML_V4L2_CID_MPEG_VIDEO_B_FRAMES) == (longestRun > 0),
            "the B-frame control does not follow the platform's run");
    if (longestRun > 0) {
        const auto& bFrames = control(controls, UML_V4L2_CID_MPEG_VIDEO_B_FRAMES);
        require(bFrames.maximum == longestRun && bFrames.coded_formats ==
                    (((runFormats & 1) ? codedBit(VIRTIO_VIDEO_FORMAT_H264) : 0) | ((runFormats & 2) ? codedBit(VIRTIO_VIDEO_FORMAT_HEVC) : 0)),
                "the B-frame control is not the platform's run for the codecs that have one");
    }
    // The default level is the lowest, because a stream states at least the level it asks for.
    if (published(controls, UML_V4L2_CID_MPEG_VIDEO_H264_LEVEL))
        require(control(controls, UML_V4L2_CID_MPEG_VIDEO_H264_LEVEL).default_value == UML_V4L2_MPEG_VIDEO_H264_LEVEL_1_0,
                "the default H.264 level is not the lowest");
    if (published(controls, UML_V4L2_CID_MPEG_VIDEO_HEVC_LEVEL))
        require(control(controls, UML_V4L2_CID_MPEG_VIDEO_HEVC_LEVEL).default_value == UML_V4L2_MPEG_VIDEO_HEVC_LEVEL_1,
                "the default HEVC level is not the lowest");
    std::printf("  this host lists %zu raw formats; HEVC Main 10 %s, P010 %s, HDR10 metadata %s\n", listed.size(), main10Profile ? "yes" : "no",
                hasLayout(VideoCodec::hevc, RawVideoLayout::p010) ? "yes" : "no", hdr ? "yes" : "no");
    if (main10) {
        // And a stream that asks for it gets it: Main 10, the 10-bit layout, and the metadata in the encoder's settings.
        auto values = controls.defaults();
        values.scalars[UML_V4L2_CID_MPEG_VIDEO_HEVC_PROFILE] = UML_V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10;
        std::vector<std::uint8_t> light(4), display(24);
        light[0] = 0xe8; light[1] = 0x03; light[2] = 0x90; light[3] = 0x01;
        for (std::size_t i = 0; i < 6; ++i) display[2 * i] = std::uint8_t(i + 1);
        display[12] = 0x10; display[14] = 0x20; display[16] = 0x40; display[17] = 0x42; display[18] = 0x0f; display[20] = 0x05;
        if (hdr) {
            require(controls.check(UML_V4L2_CID_COLORIMETRY_HDR10_CLL_INFO, 0, light.size()) == ControlCheck::ok &&
                    controls.check(UML_V4L2_CID_COLORIMETRY_HDR10_MASTERING_DISPLAY, 0, display.size()) == ControlCheck::ok,
                    "the host's HDR10 payload sizes are refused");
            values.payloads[UML_V4L2_CID_COLORIMETRY_HDR10_CLL_INFO] = light;
            values.payloads[UML_V4L2_CID_COLORIMETRY_HDR10_MASTERING_DISPLAY] = display;
        }
        const auto settings = encodeSettings(stream(VideoCodec::hevc, RawVideoLayout::p010), values);
        require(settings.profile == VideoEncodeProfile::hevcMain10 && settings.input == RawVideoLayout::p010 &&
                settings.hdr.has_value() == hdr, "a Main 10 stream is not the platform's Main 10 session");
        if (hdr)
            require(settings.hdr->maximumContentLightLevel == 1000 && settings.hdr->maximumFrameAverageLightLevel == 400 &&
                    settings.hdr->maximumLuminance == 1'000'000, "the HDR metadata is not the guest's");
    } else {
        std::puts("  no HEVC Main 10 from 10-bit input on this host; it is absent from the wire");
    }
}

} // namespace

int main()
{
    try {
        wireNames();
        capabilityResponses();
        controlTable();
        valueChecks();
        levels();
        streamInfo();
        settingsFromControls();
        updates();
        realHost();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
