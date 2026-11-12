// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace uml::platform {

enum class VideoCodec { h264, hevc, jpeg, vp9 };
enum class VideoProfile {
    h264Baseline, h264ConstrainedBaseline, h264Main, h264High, h264ConstrainedHigh,
    h264High10, hevcMain, hevcMain10, jpegBaseline, vp9Profile0,
};

// maximumLevel is the highest level the hardware decodes, as the stream states
// it: H.264 level_idc (51 is level 5.1) and HEVC general_level_idc (186 is
// level 6.2); 0 for a codec with no levels.
struct VideoDecodeCapability {
    VideoCodec codec;
    VideoProfile profile;
    std::uint32_t maximumWidth;
    std::uint32_t maximumHeight;
    std::uint32_t maximumLevel;
    std::uint32_t bitDepth;
};

struct VideoFormat {
    std::uint32_t width {};
    std::uint32_t height {};
    std::uint32_t bitDepth {8};
    friend bool operator==(const VideoFormat&, const VideoFormat&) = default;
};

// How a decoded frame carries its samples. A planar frame is the decoder's
// native 4:2:0 output, 8-bit NV12 or 10-bit P010 by the stream's depth;
// planar8Bit asks for NV12 whatever the depth is, which for a 10-bit stream
// means the host narrows it, and is planarYuv for an 8-bit one. A packed
// frame has already left the YCbCr domain and is what a client that only draws
// through a 32-bit RGB surface needs.
enum class VideoPixelLayout { planarYuv, packedBgra, planar8Bit };

// How a planar frame's samples relate to RGB. It is read back from the decoded
// surface rather than guessed, so it is the description the hardware itself
// used: a frame converted by a caller and a frame the hardware packed then
// agree by construction, whatever the stream declared or left unstated.
enum class VideoColorMatrix { bt601, bt709, bt2020 };
struct VideoColorimetry {
    VideoColorMatrix matrix {VideoColorMatrix::bt709};
    bool fullRange {};
    friend bool operator==(const VideoColorimetry&, const VideoColorimetry&) = default;
};

// Timestamps retain the caller's units. Frames are returned in the order the
// stream shows them, including delayed drain output: H.264 and HEVC pictures
// by their picture order count (VideoToolbox returns them as decoded). H.264
// field pictures are the exception and leave as decoded. `layout` selects which
// buffers carry the samples and is the layout the frame actually has, not the
// one last requested; the depth is the frame's own.
struct DecodedVideoFrame {
    VideoFormat format;
    VideoPixelLayout layout {VideoPixelLayout::planarYuv};
    VideoColorimetry color;
    std::uint64_t timestamp {};
    // planarYuv: tightly packed NV12 (8-bit) or P010 (10-bit).
    std::vector<std::uint8_t> luma;
    std::vector<std::uint8_t> chroma;
    // packedBgra: tightly packed rows of 8-bit B, G, R, A samples, each row
    // `format.width * 4` bytes and alpha opaque.
    std::vector<std::uint8_t> pixels;
};

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;
    virtual void submit(std::span<const std::uint8_t> bytes,
                        std::uint64_t timestamp) = 0;
    // Reads a packet's headers so that `format()` describes the stream, and
    // decodes nothing. A caller that must report the picture size before its
    // client can choose an output layout inspects the packet, reports the
    // size, and submits the same packet once the layout is chosen, so the
    // first picture it decodes already carries that layout. Inspecting a
    // packet and later submitting it has the same effect as submitting it
    // alone, and a packet whose headers are incomplete leaves the format as
    // it was.
    virtual void inspect(std::span<const std::uint8_t> bytes) = 0;
    // Finishes decoding and makes every picture still waiting for its turn
    // available, in output order.
    virtual void drain() = 0;
    // A seek: drops every picture in flight, every reference and the state of
    // the stream's picture order, and keeps the parameter sets and the format
    // they described, because a client that seeks does not send them again.
    // The next picture must begin at a random access point.
    virtual void discard() = 0;
    virtual std::vector<DecodedVideoFrame> takeFrames() = 0;
    [[nodiscard]] virtual VideoFormat format() const noexcept = 0;
    // How many pictures of the stream may follow a picture in decoding order
    // and precede it in output order, from the parameter set the stream is
    // described by: what the stream states, or what its profile and level
    // allow. A client that must not stall the decoder keeps that many
    // pictures, and one more, in flight. Zero until a parameter set has been
    // read, and for a codec whose pictures leave as they decode.
    [[nodiscard]] virtual unsigned reorderDepth() const noexcept { return 0; }
    // Requests the layout of frames decoded from this point on. A decoder that
    // cannot produce the request keeps producing its native layout, and one
    // that must restart to honor it waits for a point in the stream where
    // restarting is lossless, so a request never interrupts decoding. Frames
    // already decoded keep the layout they were produced with, and every frame
    // reports the layout it actually carries.
    virtual void requestOutputLayout(VideoPixelLayout layout) = 0;
};

[[nodiscard]] std::vector<VideoDecodeCapability> videoDecodeCapabilities();
[[nodiscard]] std::unique_ptr<VideoDecoder> makeVideoDecoder(VideoCodec codec);

} // namespace uml::platform
