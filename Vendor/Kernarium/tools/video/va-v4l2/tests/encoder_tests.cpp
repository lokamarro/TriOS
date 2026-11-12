// SPDX-License-Identifier: GPL-2.0-or-later
// The V4L2 encode session against an encoder node written in memory: the
// initialisation sequence, what reaches the device, the controls, the queue
// discipline and the teardown that hands the descriptor back.
#include <linux/videodev2.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../v4l2_encoder.hpp"
#include "fake_v4l2.hpp"
#include "picture.hpp"
#include "test_support.hpp"

namespace uml::va::test {
namespace {

struct Rig {
    FakeEncoderNode device;
    DescriptorPool pool;
    std::shared_ptr<EncoderCapabilities> capabilities;

    explicit Rig(const FakeEncoderNode::Options& options = {}) : device(options), pool(device)
    {
        pool.setTarget(Role::encoder, 1);
        pool.fill(Role::encoder, FakeEncoderNode::kPath);
        Lease lease = pool.acquire(Role::encoder, FakeEncoderNode::kPath, error);
        capabilities = std::make_shared<EncoderCapabilities>();
        capabilities->device = FakeEncoderNode::kPath;
        probed = probeEncoder(device, lease.fd(), *capabilities, error);
        lease.giveBack(true, false);
    }
    std::unique_ptr<V4l2Encoder> open(const V4l2Encoder::Settings& settings)
    {
        Lease lease = pool.acquire(Role::encoder, FakeEncoderNode::kPath, error);
        return V4l2Encoder::open(std::move(lease), capabilities, settings, error);
    }
    std::string error;
    bool probed {};
};

V4l2Encoder::Settings baseSettings(unsigned width, unsigned height)
{
    V4l2Encoder::Settings settings;
    settings.codec = V4L2_PIX_FMT_H264;
    settings.width = width;
    settings.height = height;
    return settings;
}

bool startsWith(const std::vector<std::uint8_t>& data, std::initializer_list<std::uint8_t> prefix)
{
    return data.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), data.begin());
}

// The type byte of each Annex B NAL unit in the fake's stream.
std::vector<std::uint8_t> nalTypes(const std::vector<std::uint8_t>& data)
{
    std::vector<std::uint8_t> types;
    for (std::size_t index = 0; index + 4 < data.size(); ++index)
        if (data[index] == 0 && data[index + 1] == 0 && data[index + 2] == 0 && data[index + 3] == 1)
            types.push_back(data[index + 4]);
    return types;
}

void testProbe()
{
    Rig rig;
    check(rig.probed, "an encoder node probes");
    const auto& caps = *rig.capabilities;
    check(caps.codecs.size() == 2 && caps.codec(V4L2_PIX_FMT_H264) && caps.codec(V4L2_PIX_FMT_HEVC),
          "the compressed capture formats are the codecs");
    const EncodeCodec* h264 = caps.codec(V4L2_PIX_FMT_H264);
    check(h264 && h264->minWidth == 16 && h264->maxWidth == 4096 && h264->stepWidth == 2,
          "the frame sizes come from the device");
    check(caps.codec(V4L2_PIX_FMT_HEVC)->maxWidth == 8192, "each codec has its own limits");
    check(caps.offersRaw(V4L2_PIX_FMT_NV12M) && caps.offersRaw(V4L2_PIX_FMT_NV12) && !caps.offersRaw(V4L2_PIX_FMT_YUV420),
          "the raw formats come from the output queue");
    check(caps.offers(V4L2_CID_MPEG_VIDEO_BITRATE) && !caps.offers(V4L2_CID_MPEG_VIDEO_B_FRAMES + 0x1000),
          "a published control is offered and another is not");
    const ControlInfo* bitrate = caps.control(V4L2_CID_MPEG_VIDEO_BITRATE);
    check(bitrate && bitrate->minimum == 1000 && bitrate->maximum == 240000000 && bitrate->defaultValue == 2000000,
          "a control carries its range and default");
    check(caps.offersItem(V4L2_CID_MPEG_VIDEO_H264_PROFILE, V4L2_MPEG_VIDEO_H264_PROFILE_MAIN) &&
              caps.offersItem(V4L2_CID_MPEG_VIDEO_H264_PROFILE, V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_HIGH) &&
              !caps.offersItem(V4L2_CID_MPEG_VIDEO_H264_PROFILE, V4L2_MPEG_VIDEO_H264_PROFILE_EXTENDED),
          "a menu offers exactly the items the device allows");
    check(caps.control(V4L2_CID_MIN_BUFFERS_FOR_OUTPUT)->flags & V4L2_CTRL_FLAG_READ_ONLY,
          "a read-only control is marked");

    Rig plain({.hevc = false});
    check(plain.probed && plain.capabilities->codecs.size() == 1 && !plain.capabilities->offers(V4L2_CID_MPEG_VIDEO_HEVC_PROFILE),
          "a node without HEVC offers none");
    Rig bare({.noControls = true});
    check(bare.probed && bare.capabilities->controls.empty(), "a node with no controls probes with none");

    // A node that is not a stateful encoder says so.
    FakeEncoderNode device;
    EncoderCapabilities nothing;
    std::string error;
    check(!probeEncoder(device, 12345, nothing, error) && !error.empty(), "a descriptor that is no node is refused");
}

void testSessionSetup()
{
    Rig rig;
    auto settings = baseSettings(640, 360);
    settings.frameRateNumerator = 30000;
    settings.frameRateDenominator = 1001;
    settings.controls = {
        {V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_CBR},
        {V4L2_CID_MPEG_VIDEO_BITRATE, 4000000},
        {V4L2_CID_MPEG_VIDEO_GOP_SIZE, 60},
        {V4L2_CID_MPEG_VIDEO_H264_PROFILE, V4L2_MPEG_VIDEO_H264_PROFILE_MAIN},
        {V4L2_CID_MPEG_VIDEO_H264_LEVEL, V4L2_MPEG_VIDEO_H264_LEVEL_3_1},
        {V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 1},
        {V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, 0},
    };
    auto encoder = rig.open(settings);
    check(encoder != nullptr, "a session opens");
    if (!encoder) return;
    check(rig.device.codedFormat == V4L2_PIX_FMT_H264, "the coded format is set");
    check(rig.device.outputFormat == V4L2_PIX_FMT_NV12M && rig.device.outputPlanes == 2,
          "the raw format is the two-plane one the device lists first");
    check(rig.device.outputWidth == 640 && rig.device.outputHeight == 360, "the raw frame size is the stream size");
    check(rig.device.frameRateNumerator == 30000 && rig.device.frameRateDenominator == 1001,
          "the fractional frame rate reaches the device");
    check(rig.device.streamOns == 2, "both queues are started");
    check(rig.device.control(V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE) == 0, "a boolean control reaches the device");
    check(rig.device.control(V4L2_CID_MPEG_VIDEO_BITRATE) == 4000000 &&
              rig.device.control(V4L2_CID_MPEG_VIDEO_GOP_SIZE) == 60 &&
              rig.device.control(V4L2_CID_MPEG_VIDEO_BITRATE_MODE) == V4L2_MPEG_VIDEO_BITRATE_MODE_CBR,
          "the controls reach the device");
    bool before = !rig.device.controlLog.empty();
    for (const auto& set : rig.device.controlLog) before = before && !set.streaming && set.result == 0;
    check(before && rig.device.controlLog.size() == settings.controls.size(),
          "every setup control is applied before streaming, in order");
    check(rig.device.controlLog.front().id == V4L2_CID_MPEG_VIDEO_BITRATE_MODE &&
              rig.device.controlLog.back().id == V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE,
          "controls are applied in the order given");
    check(encoder->notes().empty(), "a clean setup leaves no notes");

    encoder.reset();
    check(rig.device.streamOffs == 2 && rig.device.requestBuffersFree == 2,
          "the session stops both queues and frees their buffers");
    check(rig.pool.idle(Role::encoder) == 1 && rig.device.closes == 0,
          "the descriptor goes back to the pool, still open");
    auto again = rig.open(baseSettings(320, 240));
    check(again != nullptr && rig.device.opens == 1, "the next session reuses the descriptor");
    check(rig.device.outputWidth == 320, "and starts from the initialisation sequence");
    check(rig.device.control(V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE) == 1 &&
              rig.device.control(V4L2_CID_MPEG_VIDEO_BITRATE) == 2000000 &&
              rig.device.control(V4L2_CID_MPEG_VIDEO_GOP_SIZE) == 0,
          "a reused descriptor does not keep the controls of the session before");
}

void testEncodeAndCollect()
{
    Rig rig;
    auto settings = baseSettings(360, 202);
    settings.controls = {{V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 1}};
    auto encoder = rig.open(settings);
    check(encoder != nullptr, "an odd-pitch session opens");
    if (!encoder) return;
    check(rig.device.outputPitch[0] == 384, "the device lays rows out on its own pitch");

    Picture first(360, 202, 1), second(360, 202, 2), third(360, 202, 3);
    std::string error;
    check(encoder->submit(first.frame(), 1000, true, error), "the first frame is accepted");
    check(encoder->submit(second.frame(), 2000, false, error), "the second frame is accepted");
    check(encoder->submit(third.frame(), 3000, true, error), "a forced key frame is accepted");
    check(encoder->inFlight() == 3, "three pictures are in flight");

    EncodedFrame frame;
    check(encoder->collect(frame, std::chrono::milliseconds(200), error), "the first coded frame arrives");
    check(frame.timestamp == 1000 && frame.key && !frame.error, "it carries its picture's timestamp and is a key frame");
    check(startsWith(frame.data, {0, 0, 0, 1, 0x67}), "a key frame starts with the parameter sets");
    const auto types = nalTypes(frame.data);
    check(types.size() == 3 && types[0] == 0x67 && types[1] == 0x68 && types[2] == 0x65, "SPS, PPS, then the IDR slice");
    // The checksum the device computed over what it was given.
    check(rig.device.frames.size() >= 1 && rig.device.frames[0].checksum == first.checksum(),
          "the device received the picture's visible samples at the right rows");
    check(rig.device.frames[0].timestamp == 1000, "the device received the timestamp");

    check(encoder->collect(frame, std::chrono::milliseconds(200), error), "the second coded frame arrives");
    check(frame.timestamp == 2000 && !frame.key && nalTypes(frame.data) == std::vector<std::uint8_t> {0x41},
          "a following frame is a plain slice");
    check(rig.device.frames[1].checksum == second.checksum(), "the second picture arrived intact");
    check(encoder->collect(frame, std::chrono::milliseconds(200), error), "the third coded frame arrives");
    check(frame.timestamp == 3000 && frame.key, "a forced key frame is a key frame");
    bool forced = false;
    for (const auto& set : rig.device.controlLog)
        if (set.id == V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME && set.streaming && set.result == 0) forced = true;
    check(forced, "the key frame was forced through the dynamic control");
    check(encoder->inFlight() == 0, "nothing is left in flight");
    check(!encoder->collect(frame, std::chrono::milliseconds(5), error) && error.empty(),
          "a wait with nothing coming ends without an error");

    // A frame of another size is not this stream's.
    Picture wrong(320, 240, 4);
    check(!encoder->submit(wrong.frame(), 4000, false, error) && !error.empty(), "a picture of another size is refused");
}

void testSinglePlaneLayout()
{
    Rig rig({.nv12m = false});
    auto encoder = rig.open(baseSettings(96, 64));
    check(encoder != nullptr, "a device with only the one-plane format opens");
    if (!encoder) return;
    check(rig.device.outputFormat == V4L2_PIX_FMT_NV12 && rig.device.outputPlanes == 1,
          "the one-plane format is used when it is all there is");
    Picture picture(96, 64, 5);
    std::string error;
    check(encoder->submit(picture.frame(), 7000, false, error), "a frame is accepted");
    EncodedFrame frame;
    check(encoder->collect(frame, std::chrono::milliseconds(200), error), "its coded frame arrives");
    check(rig.device.frames.size() == 1 && rig.device.frames[0].checksum == picture.checksum(),
          "the chroma follows the luma in the one plane");
}

void testControls()
{
    Rig rig;
    auto settings = baseSettings(128, 96);
    settings.controls = {
        {V4L2_CID_MPEG_VIDEO_BITRATE, 500},                                             // below the range
        {V4L2_CID_MPEG_VIDEO_H264_PROFILE, V4L2_MPEG_VIDEO_H264_PROFILE_EXTENDED},     // not offered
        {V4L2_CID_MPEG_VIDEO_B_FRAMES, 0},
        {0x00990fff, 3},                                                                // unknown
        {V4L2_CID_MIN_BUFFERS_FOR_OUTPUT, 4},                                           // read-only
        {V4L2_CID_MPEG_VIDEO_H264_MAX_QP, 60},                                          // above the range
    };
    auto encoder = rig.open(settings);
    check(encoder != nullptr, "setup survives controls the device does not honour");
    if (!encoder) return;
    check(encoder->notes().size() == 5, "each control that was not honoured is noted");
    check(rig.device.control(V4L2_CID_MPEG_VIDEO_BITRATE) == 1000, "an out-of-range value is clamped to the range");
    check(rig.device.control(V4L2_CID_MPEG_VIDEO_H264_MAX_QP) == 51, "so is one above the maximum");
    check(rig.device.control(V4L2_CID_MPEG_VIDEO_H264_PROFILE) == 2, "a menu item the device lacks is not set");

    encoder->clearNotes();
    check(encoder->update({{V4L2_CID_MPEG_VIDEO_BITRATE, 3000000}, {V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, 6000000}}) == 2,
          "dynamic controls change while streaming");
    check(rig.device.control(V4L2_CID_MPEG_VIDEO_BITRATE) == 3000000, "and reach the device");
    check(encoder->update({{V4L2_CID_MPEG_VIDEO_GOP_SIZE, 30}, {V4L2_CID_MPEG_VIDEO_BITRATE, 2500000}}) == 1,
          "a control the device holds fixed while streaming is skipped and the others still apply");
    check(encoder->notes().size() == 1, "the refusal is noted");
    std::string error;
    check(encoder->setFrameRate(60, 1, error) && rig.device.frameRateNumerator == 60, "the frame rate changes while streaming");

    // A node that publishes no controls still encodes.
    Rig bare({.noControls = true});
    auto plain = bare.open(settings);
    check(plain != nullptr && plain->notes().size() == settings.controls.size(),
          "a node with no controls encodes and notes every control it skipped");
    if (plain) {
        Picture picture(128, 96, 1);
        check(plain->submit(picture.frame(), 1000, true, error), "a key frame can be asked for");
        check(!plain->notes().empty() && plain->notes().back().find("key frame") != std::string::npos,
              "a node that cannot force a key frame says so");
    }
}

void testBackpressure()
{
    // The device defers raw completion while it holds a coded frame that
    // nobody has collected, and has only two coded buffers to put them in.
    Rig rig({.codedLimit = 1});
    auto settings = baseSettings(64, 64);
    settings.rawBuffers = 3;
    settings.codedBuffers = 2;
    auto encoder = rig.open(settings);
    check(encoder != nullptr, "a small session opens");
    if (!encoder) return;
    encoder->setBound(std::chrono::milliseconds(1000));
    std::string error;
    bool allAccepted = true;
    for (unsigned index = 1; index <= 12; ++index) {
        Picture picture(64, 64, index);
        allAccepted = encoder->submit(picture.frame(), index * 1000ull, index == 1, error) && allAccepted;
    }
    check(allAccepted, "a client that submits twelve frames before collecting any is not stalled");
    EncodedFrame frame;
    bool ordered = true;
    for (unsigned index = 1; index <= 12; ++index) {
        const bool got = encoder->collect(frame, std::chrono::milliseconds(500), error);
        ordered = ordered && got && frame.timestamp == index * 1000ull;
    }
    check(ordered, "every frame comes back, in order, with its own timestamp");

    // When the device never gives a raw buffer back, a submission gives up.
    Rig stuck({.codedLimit = 0, .holdCoded = true});
    auto starved = stuck.open(baseSettings(64, 64));
    check(starved != nullptr, "a session on a device that holds everything opens");
    if (starved) {
        starved->setBound(std::chrono::milliseconds(80));
        bool refused = false;
        for (unsigned index = 1; index <= 8 && !refused; ++index) {
            Picture picture(64, 64, index);
            refused = !starved->submit(picture.frame(), index * 1000ull, false, error);
        }
        check(refused && error.find("raw buffer") != std::string::npos, "a stuck device ends in an error, not a hang");
    }
}

void testFailures()
{
    Rig rig;
    std::string error;
    check(rig.open(baseSettings(100, 63)) == nullptr, "an odd frame size is refused");
    check(rig.open(baseSettings(8, 8)) == nullptr, "a frame smaller than the device takes is refused");
    check(rig.open(baseSettings(8192, 8192)) == nullptr, "a frame larger than the device takes is refused");
    auto mpeg = baseSettings(64, 64);
    mpeg.codec = V4L2_PIX_FMT_MPEG4;
    check(rig.open(mpeg) == nullptr && rig.error.find("codec") != std::string::npos, "a codec the device lacks is refused");
    check(rig.pool.idle(Role::encoder) == 1, "a refused session gives its descriptor back");

    Rig noStream({.failStreamOn = true});
    check(noStream.open(baseSettings(64, 64)) == nullptr && !noStream.error.empty(), "a device that will not start is an error");
    check(noStream.pool.idle(Role::encoder) == 1, "and still gives its descriptor back");

    Rig noFormat({.failSetFormat = true});
    check(noFormat.open(baseSettings(64, 64)) == nullptr, "a device that refuses the format is an error");

    Rig noRaw({.nv12m = false, .nv12 = false});
    check(noRaw.open(baseSettings(64, 64)) == nullptr && noRaw.error.find("NV12") != std::string::npos,
          "a device that takes no NV12 is an error");

    Rig flaky({.errorFrame = 1});
    auto encoder = flaky.open(baseSettings(64, 64));
    if (encoder) {
        Picture a(64, 64, 1), b(64, 64, 2), c(64, 64, 3);
        encoder->submit(a.frame(), 1000, true, error);
        encoder->submit(b.frame(), 2000, false, error);
        encoder->submit(c.frame(), 3000, false, error);
        EncodedFrame frame;
        encoder->collect(frame, std::chrono::milliseconds(100), error);
        const bool firstGood = !frame.error;
        encoder->collect(frame, std::chrono::milliseconds(100), error);
        const bool secondBad = frame.error && frame.timestamp == 2000;
        encoder->collect(frame, std::chrono::milliseconds(100), error);
        check(firstGood && secondBad && !frame.error, "a frame the device failed carries the error and only that frame");
    }
}

}

void runEncoderTests()
{
    testProbe();
    testSessionSetup();
    testEncodeAndCollect();
    testSinglePlaneLayout();
    testControls();
    testBackpressure();
    testFailures();
}

}
