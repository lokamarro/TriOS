// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The platform's hardware video encoder. The Apple backend implements it over
// VideoToolbox; nothing here names VideoToolbox, V4L2 or the Virtio wire, and
// the Virtio video encoder device maps these types to and from the wire.

#include "video_decoder.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace uml::platform {

// Profiles an encoder can produce. Only profiles a guest API can name are
// listed (the HWENC U6 decision): HEVC range extensions, ProRes, HEVC with
// alpha and depth/disparity HEVC are deliberately absent.
enum class VideoEncodeProfile {
    h264Baseline, h264ConstrainedBaseline, h264Main, h264High,
    h264ConstrainedHigh, h264High422, h264High444Predictive,
    hevcMain, hevcMain10, hevcMainStill,
    jpegBaseline,
};

// Raw sample layouts an encoder accepts. Planes are tightly described by
// `RawVideoFrame`; 10-bit layouts carry samples in the high bits of 16.
enum class RawVideoLayout {
    nv12,  // 8-bit 4:2:0, Y plane then interleaved CbCr
    i420,  // 8-bit 4:2:0, Y, Cb, Cr planes
    p010,  // 10-bit 4:2:0, Y plane then interleaved CbCr
    nv16,  // 8-bit 4:2:2, Y plane then interleaved CbCr
    nv24,  // 8-bit 4:4:4, Y plane then interleaved CbCr
    bgra,  // 8-bit packed B, G, R, A; the encoder converts to YCbCr
    rgba,  // 8-bit packed R, G, B, A in memory order (DRM ABGR8888); likewise converted
};

enum class VideoRateControl {
    variableBitrate,  // target bitrate, optional peak
    constantBitrate,  // target bitrate held per window
    constantQuality,  // quality 1..100, no bitrate target
    constantQp,       // fixed per-frame-type QP
};

enum class VideoColorPrimaries { unspecified, bt709, bt601Pal, bt601Ntsc, bt2020, p3D65 };
enum class VideoTransferFunction { unspecified, bt709, srgb, pq, hlg, linear };
enum class VideoMatrixCoefficients { unspecified, bt601, bt709, bt2020 };

struct VideoEncodeColor {
    VideoColorPrimaries primaries {VideoColorPrimaries::unspecified};
    VideoTransferFunction transfer {VideoTransferFunction::unspecified};
    VideoMatrixCoefficients matrix {VideoMatrixCoefficients::unspecified};
    bool fullRange {};
    friend bool operator==(const VideoEncodeColor&, const VideoEncodeColor&) = default;
};

// SMPTE ST 2086 mastering display and CTA-861.3 content light level, in the
// units of the HEVC SEI messages that carry them.
struct VideoHdrStaticMetadata {
    std::array<std::uint16_t, 6> displayPrimaries {};  // G, B, R (x, y), 0.00002 units
    std::uint16_t whitePointX {};
    std::uint16_t whitePointY {};
    std::uint32_t maximumLuminance {};  // 0.0001 cd/m^2
    std::uint32_t minimumLuminance {};  // 0.0001 cd/m^2
    std::uint16_t maximumContentLightLevel {};
    std::uint16_t maximumFrameAverageLightLevel {};
    friend bool operator==(const VideoHdrStaticMetadata&, const VideoHdrStaticMetadata&) = default;
};

struct VideoEncodeProfileCapability {
    VideoEncodeProfile profile;
    // Layouts that encoded at this profile during discovery.
    std::vector<RawVideoLayout> inputs;
    // Highest level this host can be asked for, as level_idc (H.264, e.g. 52)
    // or general_level_idc (HEVC, e.g. 186). Zero for JPEG.
    std::uint32_t maximumLevel {};
    // True when a stream states the level it was asked for, or the lowest one
    // its picture size, frame rate and bit rate need if that is higher. False
    // when the encoder chooses and a requested level is only a ceiling.
    bool levelSelectable {};
};

// The smallest picture side, in samples, that a capability ever lists. VideoToolbox
// accepts smaller pictures, but a guest's quality check rejects what it makes of
// them: Android's CTS finds a PSNR under 20 dB at 16, 32 and 48 samples and none
// at 64. Discovery therefore probes from this side up and never reports less.
inline constexpr std::uint32_t kVideoEncodeMinimumSide = 64;

// One codec as this host's hardware encodes it. Every field is the result of
// discovery on this device: a profile, layout, size or mode is listed only
// after an encode with it succeeded.
struct VideoEncodeCapability {
    VideoCodec codec;
    std::vector<VideoEncodeProfileCapability> profiles;
    std::uint32_t minimumWidth {};
    std::uint32_t minimumHeight {};
    std::uint32_t maximumWidth {};
    std::uint32_t maximumHeight {};
    std::uint32_t sizeAlignment {2};  // visible sizes are multiples of this
    std::vector<VideoRateControl> rateControls;
    std::uint64_t maximumBitrate {};  // bits per second
    std::uint32_t minimumQp {};
    std::uint32_t maximumQp {};
    // The B-frame run the encoder codes when it may reorder; zero: output order
    // is input order. The run is fixed (VideoToolbox cannot shorten it), so a
    // request below it codes no B-frames and a request at or above it codes it.
    std::uint32_t maximumBFrames {};
    std::uint32_t maximumTemporalLayers {1};
    bool entropyModeSelectable {};    // H.264 CAVLC or CABAC
    bool hdrStaticMetadata {};
    std::uint32_t concurrentSessions {};
    // Frames per second one session sustained at 1280x720 and 1920x1080
    // during discovery; zero when not measured.
    std::uint32_t measuredFramesPerSecond720p {};
    std::uint32_t measuredFramesPerSecond1080p {};
};

// Everything fixed for the life of a session. Changing any of it reconfigures
// the session, which restarts the stream at a key frame.
struct VideoEncodeSettings {
    VideoCodec codec {VideoCodec::h264};
    VideoEncodeProfile profile {VideoEncodeProfile::h264High};
    // As maximumLevel. The stream states at least this, and the lowest level
    // at or above it that the stream provably fits; zero: as low as it fits.
    std::uint32_t level {};
    std::uint32_t width {};
    std::uint32_t height {};
    RawVideoLayout input {RawVideoLayout::nv12};
    VideoEncodeColor color;
    std::optional<VideoHdrStaticMetadata> hdr;
    std::uint32_t frameRateNumerator {30};
    std::uint32_t frameRateDenominator {1};
    VideoRateControl rateControl {VideoRateControl::variableBitrate};
    std::uint64_t bitrate {};      // target, bits per second
    std::uint64_t peakBitrate {};  // zero: no peak limit
    std::uint32_t quality {};      // constantQuality: 1..100
    std::array<std::uint32_t, 3> frameQp {};  // constantQp: I, P, B
    std::optional<std::uint32_t> minimumQp;
    std::optional<std::uint32_t> maximumQp;
    std::uint32_t keyFrameInterval {};  // frames; zero: first frame and requests only
    std::uint32_t bFrames {};           // the longest B-frame run accepted; zero: no reordering
    std::uint32_t temporalLayers {1};
    std::uint32_t baseLayerBitratePercent {};  // temporalLayers > 1; zero: encoder default
    bool cabac {true};                         // H.264 Main and above
    std::uint64_t bufferBits {};               // VBV/CPB size; zero: encoder default
    bool realTime {};
    // Parameter sets: emitted as their own frame before the first key frame
    // (separate) or joined to it; repeated before every key frame or not.
    bool separateParameterSets {};
    bool parameterSetsWithEveryKeyFrame {true};
    std::uint32_t jpegQuality {};  // JPEG: 1..100
};

// Values that may change while a session encodes. An unset field keeps its
// current value.
struct VideoEncodeUpdate {
    std::optional<std::uint64_t> bitrate;
    std::optional<std::uint64_t> peakBitrate;
    std::optional<std::uint32_t> quality;
    std::optional<std::uint32_t> maximumQp;
    std::optional<std::uint32_t> frameRateNumerator;
    std::optional<std::uint32_t> frameRateDenominator;
    std::optional<std::uint32_t> jpegQuality;
};

enum class VideoEncodeStatus { ok, unsupported, invalid, exhausted, failed };

// A frame from the session's own pool, written by the caller and then
// submitted. Its memory is what the hardware reads, so filling it is the only
// copy a raw frame takes on the host.
class WritableVideoFrame {
public:
    virtual ~WritableVideoFrame() = default;
    [[nodiscard]] virtual std::size_t planeCount() const noexcept = 0;
    [[nodiscard]] virtual std::span<std::uint8_t> plane(std::size_t index) noexcept = 0;
    [[nodiscard]] virtual std::uint32_t stride(std::size_t index) const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t rows(std::size_t index) const noexcept = 0;
};

enum class EncodedVideoFrameKind {
    frame,          // a coded picture
    parameterSets,  // headers only (separateParameterSets)
    drained,        // every frame submitted before drain() has been returned
    error,          // the frame with this timestamp was not encoded
};

enum class VideoFrameType { key, predicted, bidirectional };

struct EncodedVideoFrame {
    EncodedVideoFrameKind kind {EncodedVideoFrameKind::frame};
    VideoFrameType type {VideoFrameType::key};
    std::uint64_t timestamp {};  // the submitted frame's own timestamp
    std::uint32_t temporalLayer {};
    // Annex B for H.264 and HEVC (start codes, parameter sets placed as the
    // settings ask); a complete JFIF image for JPEG.
    std::vector<std::uint8_t> bytes;
};

// One encode session. Methods are called from one thread; output becomes
// available asynchronously and is announced on readinessDescriptor().
// Frames are returned in decode order.
class VideoEncoder {
public:
    virtual ~VideoEncoder() = default;
    [[nodiscard]] virtual VideoEncodeStatus configure(const VideoEncodeSettings& settings) = 0;
    [[nodiscard]] virtual VideoEncodeStatus update(const VideoEncodeUpdate& update) = 0;
    // Null while the configured number of frames is already in flight; the
    // caller retries once output has been taken.
    [[nodiscard]] virtual std::unique_ptr<WritableVideoFrame> acquireFrame() = 0;
    virtual void submit(std::unique_ptr<WritableVideoFrame> frame, std::uint64_t timestamp,
                        bool forceKeyFrame) = 0;
    // Completes asynchronously with an EncodedVideoFrameKind::drained entry.
    virtual void drain() = 0;
    // Discards frames in flight and output not yet taken; the next frame
    // starts a new coded sequence at a key frame.
    virtual void reset() = 0;
    [[nodiscard]] virtual std::vector<EncodedVideoFrame> takeFrames() = 0;
    // Readable while takeFrames() has something to return. Owned by the
    // encoder and valid for its lifetime.
    [[nodiscard]] virtual int readinessDescriptor() const noexcept = 0;
};

// Starts discovery on a background thread; idempotent. Composition calls it
// when it creates the encoder service so that the first capability query
// rarely waits.
void prepareVideoEncodeCapabilities();
// Discovery's result, waiting for it when it is still running. Empty when the
// host has no usable hardware encoder.
[[nodiscard]] std::vector<VideoEncodeCapability> videoEncodeCapabilities();
[[nodiscard]] std::unique_ptr<VideoEncoder> makeVideoEncoder();

} // namespace uml::platform
