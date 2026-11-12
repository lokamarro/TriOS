// SPDX-License-Identifier: GPL-2.0-or-later
#include "virtio_video_device.hpp"
#include "raw_pitch.hpp"
#include "require.hpp"
#include "scratch_backing.hpp"
#include "scratch_directory.hpp"
#include "shared_memory_regions.hpp"
#include "video_capabilities.hpp"
#include "wire_bytes.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <string>
#include <memory>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

using namespace uml::platform;
using namespace uml::video;
using namespace uml::virtio;
namespace {
using uml::testing::require;
using uml::testing::append;
using uml::testing::load;
/* The 4 KiB guest page production passes. The cases place their buffers on
 * page boundaries and split the encoded input across two pages; the device
 * itself never sees a page size. */
constexpr std::size_t page = 4096;
/* The encoder commands are laid out for a kernel that compiles the same
 * header, so each structure keeps its size and every 64-bit member its
 * natural alignment, with no compiler padding to disagree about. */
static_assert(sizeof(virtio_video_query_v4l2_controls) == 16);
static_assert(sizeof(virtio_video_query_v4l2_controls_resp) == 16);
static_assert(sizeof(virtio_video_v4l2_control_desc) == 64);
static_assert(offsetof(virtio_video_v4l2_control_desc, menu_skip_mask) == 40);
static_assert(offsetof(virtio_video_v4l2_control_desc, coded_formats) == 56);
static_assert(sizeof(virtio_video_v4l2_control_value) == 16);
static_assert(sizeof(virtio_video_v4l2_controls) == 16);
static_assert(sizeof(virtio_video_v4l2_controls_resp) == 16);
static_assert(sizeof(virtio_video_stream_info) == 32);
static_assert(sizeof(virtio_video_set_stream_info) == 40);
static_assert(sizeof(virtio_video_get_stream_info) == 16);
static_assert(sizeof(virtio_video_get_stream_info_resp) == 40);
/* Neighbouring samples differ along both axes, so a plane copied with the
 * wrong stride cannot reproduce the picture: every destination row would hold
 * a window that starts at the wrong sample. A flat fill survives any stride
 * error, which is why the fixtures below never use one. */
std::uint8_t patternLuma(std::uint32_t row, std::uint32_t column) noexcept
{
    return std::uint8_t(0x20 + (row * 5u + column * 3u) % 0xc0u);
}
/* The same pattern addressed by byte offset in a 64x64 luma plane, where a
 * ten-bit sample repeats its value in both of its bytes. */
std::uint8_t patternLumaByte(std::size_t index, bool ten) noexcept
{
    const auto sample = index / (ten ? 2u : 1u);
    return patternLuma(std::uint32_t(sample / 64), std::uint32_t(sample % 64));
}

/* The interleaved chroma of a decoder that varies it, addressed by chroma row
 * and by sample within the row, so that a chroma row written at the wrong
 * pitch cannot match. */
std::uint8_t patternChroma(std::uint32_t row, std::uint32_t sample) noexcept
{
    return std::uint8_t(0x40 + (row * 7u + sample) % 0x80u);
}

/* The BT.601 limited-range conversion a device performs for a client that
 * asked for packed pixels, written independently of the device's own loop so
 * that a test sees placement and colour separately. */
void convertBt601Limited(std::uint8_t y, std::uint8_t cb, std::uint8_t cr, std::uint8_t* out)
{
    const double kr = 0.299, kb = 0.114, kg = 1.0 - kr - kb;
    const double chroma = 255.0 / 224.0;
    const double luma = (255.0 / 219.0) * (double(y) - 16.0);
    const double u = double(cb) - 128.0, v = double(cr) - 128.0;
    const auto byte = [](double value) { return std::uint8_t(std::clamp(value, 0.0, 255.0) + 0.5); };
    out[0] = byte(luma + chroma * 2.0 * (1.0 - kb) * u);
    out[1] = byte(luma - chroma * 2.0 * kb * (1.0 - kb) / kg * u
                       - chroma * 2.0 * kr * (1.0 - kr) / kg * v);
    out[2] = byte(luma + chroma * 2.0 * (1.0 - kr) * v);
}

/* `packs` distinguishes a decoder that produces packed frames itself, as the
 * Apple hardware does, from one that only ever produces its planar output and
 * leaves the device to convert. */
class Decoder final : public VideoDecoder {
public:
    explicit Decoder(bool ten, bool packs = false, bool defers = false,
                     std::uint32_t width = 64, std::uint32_t height = 64, bool variesChroma = false,
                     unsigned reorder = 0)
        : ten_(ten), packs_(packs), defers_(defers), width_(width), height_(height),
          variesChroma_(variesChroma), reorder_(reorder) {}
    /* Reads what the packet declares and decodes nothing, so a caller can
     * report the picture size before it has to choose an output layout. */
    void inspect(std::span<const std::uint8_t> packet) override
    {
        require(packet.size() == 5 && packet[0] == 1 && packet[4] == 5, "scatter input changed");
        format_ = {width_, height_, ten_ ? 10u : 8u};
    }
    void submit(std::span<const std::uint8_t> packet, std::uint64_t timestamp) override
    {
        require(packet.size() == 5 && packet[0] == 1 && packet[4] == 5, "scatter input changed");
        format_ = {width_, height_, ten_ ? 10u : 8u};
        DecodedVideoFrame frame;
        frame.format = format_;
        frame.timestamp = timestamp;
        /* A deferring decoder keeps its layout until a later picture, which is
         * what a real one does in the middle of a group of pictures. Before
         * any picture there is no group to be in the middle of, so a layout
         * asked for then takes effect on the first one. */
        if (defers_ && pictures_ && layout_ != adopted_) { ++deferred_; if (deferred_ > 1) adopted_ = layout_; }
        else adopted_ = layout_;
        ++pictures_;
        if (packs_ && adopted_ == VideoPixelLayout::packedBgra) {
            frame.layout = VideoPixelLayout::packedBgra;
            frame.format.bitDepth = 8;
            /* Every pixel differs from its neighbours in both directions, so
             * a row written at the wrong offset cannot go unnoticed. A flat
             * colour would survive any stride error. */
            frame.pixels.resize(std::size_t(width_) * height_ * 4);
            for (unsigned row = 0; row < height_; ++row)
                for (unsigned column = 0; column < width_; ++column) {
                    auto* out = frame.pixels.data() + (std::size_t(row) * width_ + column) * 4;
                    out[0] = std::uint8_t(row); out[1] = std::uint8_t(column);
                    out[2] = std::uint8_t(row + column); out[3] = 0xff;
                }
        } else {
            frame.color = {VideoColorMatrix::bt601, false};
            const std::size_t step = ten_ ? 2 : 1;
            frame.luma.resize(std::size_t(width_) * height_ * step);
            for (std::uint32_t row = 0; row < height_; ++row)
                for (std::uint32_t column = 0; column < width_; ++column) {
                    auto* out = frame.luma.data() + (std::size_t(row) * width_ + column) * step;
                    for (std::size_t byte = 0; byte < step; ++byte) out[byte] = patternLuma(row, column);
                }
            // Flat chroma keeps the converted colour a function of luma alone. A row of
            // interleaved chroma holds a pair for every two columns, an odd width's last
            // pair included.
            const std::size_t chromaSamples = 2 * std::size_t((width_ + 1) / 2);
            frame.chroma.resize(chromaSamples * ((height_ + 1) / 2) * step);
            for (std::size_t i = 0; i < frame.chroma.size(); ++i) {
                const auto sample = i / step;
                frame.chroma[i] = variesChroma_
                    ? patternChroma(std::uint32_t(sample / chromaSamples), std::uint32_t(sample % chromaSamples))
                    : (i % 2 ? 0x91 : 0x27);
            }
        }
        frames_.push_back(std::move(frame));
    }
    void drain() override {}
    void discard() override { frames_.clear(); }
    std::vector<DecodedVideoFrame> takeFrames() override { return std::exchange(frames_, {}); }
    VideoFormat format() const noexcept override { return format_; }
    unsigned reorderDepth() const noexcept override { return format_.width ? reorder_ : 0; }
    void requestOutputLayout(VideoPixelLayout layout) override { layout_ = layout; }
private:
    bool ten_;
    bool packs_ {};
    bool defers_ {};
    unsigned deferred_ {};
    unsigned pictures_ {};
    std::uint32_t width_ {64};
    std::uint32_t height_ {64};
    bool variesChroma_ {};
    unsigned reorder_ {};
    VideoPixelLayout layout_ {VideoPixelLayout::planarYuv};
    VideoPixelLayout adopted_ {VideoPixelLayout::planarYuv};
    VideoFormat format_;
    std::vector<DecodedVideoFrame> frames_;
};
/* A decoder whose pictures change size where the script says, and which, like
 * the hardware, knows the size of the latest picture it has read as soon as it
 * has read it: before anyone has taken the pictures before it. Every picture is
 * square. */
std::uint8_t scriptedLuma(unsigned picture, std::uint32_t row, std::uint32_t column) noexcept
{
    return std::uint8_t(0x20 + (row * 5u + column * 3u + picture * 7u) % 0xc0u);
}
struct ScriptedState {
    unsigned discards {};
    unsigned submitted {};
};
class ScriptedDecoder final : public VideoDecoder {
public:
    ScriptedDecoder(std::vector<std::uint32_t> sizes, std::shared_ptr<ScriptedState> state)
        : sizes_(std::move(sizes)), state_(std::move(state)) {}
    void inspect(std::span<const std::uint8_t>) override { format_ = {sizes_.front(), sizes_.front(), 8}; }
    void submit(std::span<const std::uint8_t>, std::uint64_t timestamp) override
    {
        const auto size = sizes_[std::min<std::size_t>(state_->submitted, sizes_.size() - 1)];
        const auto picture = ++state_->submitted;
        format_ = {size, size, 8};
        DecodedVideoFrame frame;
        frame.format = format_;
        frame.timestamp = timestamp;
        frame.color = {VideoColorMatrix::bt601, false};
        frame.luma.resize(std::size_t(size) * size);
        for (std::uint32_t row = 0; row < size; ++row)
            for (std::uint32_t column = 0; column < size; ++column)
                frame.luma[std::size_t(row) * size + column] = scriptedLuma(picture, row, column);
        frame.chroma.assign(std::size_t(size) * size / 2, 0x80);
        frames_.push_back(std::move(frame));
    }
    void drain() override {}
    void discard() override { ++state_->discards; frames_.clear(); }
    std::vector<DecodedVideoFrame> takeFrames() override { return std::exchange(frames_, {}); }
    VideoFormat format() const noexcept override { return format_; }
    void requestOutputLayout(VideoPixelLayout) override {}
private:
    std::vector<std::uint32_t> sizes_;
    std::shared_ptr<ScriptedState> state_;
    VideoFormat format_;
    std::vector<DecodedVideoFrame> frames_;
};
using Planes = std::vector<std::vector<virtio_video_resource_sg_entry>>;

/* A device for one case, with `memory` as the guest RAM behind its mapper.
 * Every command names stream `stream`; `next` is the request identifier the
 * next one takes, so a case that waits for a deferred command reads it first.
 * The default capability is 8-bit H.264 High. */
struct VideoFixture final {
    VideoFixture(std::uint32_t streamId, std::size_t memorySize, VirtioVideoDevice::DecoderFactory factory,
                 std::vector<VideoDecodeCapability> capabilities =
                     {{VideoCodec::h264, VideoProfile::h264High, 4096, 2304, 51, 8}})
        : device(std::move(capabilities), std::move(factory)), memory(memorySize, 0xcc), stream(streamId)
    {
        device.setGuestMapper([this](std::uint64_t offset, std::size_t size) {
            if (offset > memory.size() || size > memory.size() - offset) return std::span<std::uint8_t>{};
            return std::span(memory).subspan(offset, size);
        });
    }
    VideoFixture(const VideoFixture&) = delete;
    VideoFixture& operator=(const VideoFixture&) = delete;

    template<class T> VhostUserQueueResult command(const T& value)
    {
        std::vector<std::uint8_t> request;
        append(request, value);
        return device.processQueue(0, next++, request, 16384);
    }
    template<class T> void ok(const T& value, std::source_location location = std::source_location::current())
    {
        require(load<virtio_video_cmd_hdr>(command(value).response).type == VIRTIO_VIDEO_RESP_OK_NODATA,
                "command failed", location);
    }
    void createStream(std::source_location location = std::source_location::current())
    {
        virtio_video_stream_create create {};
        create.hdr = {VIRTIO_VIDEO_CMD_STREAM_CREATE, stream};
        create.coded_format = VIRTIO_VIDEO_FORMAT_H264;
        ok(create, location);
    }
    VhostUserQueueResult attach(unsigned queue, unsigned id, const Planes& planes)
    {
        std::vector<std::uint8_t> request;
        append(request, virtio_video_resource_attach{VIRTIO_VIDEO_CMD_RESOURCE_ATTACH, stream, queue, id});
        for (const auto& plane : planes) {
            append(request, std::uint32_t(plane.size())); append(request, std::uint32_t(0));
            for (const auto& segment : plane) append(request, segment);
        }
        return device.processQueue(0, next++, request, 16384);
    }
    /* The capture queue's format and frame size. */
    void setOutputFormat(std::uint32_t format, std::uint32_t width = 64, std::uint32_t height = 64,
                         std::source_location location = std::source_location::current())
    {
        virtio_video_set_params set {};
        set.hdr = {VIRTIO_VIDEO_CMD_SET_PARAMS, stream};
        set.params.queue_type = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
        set.params.format = format;
        set.params.frame_width = width; set.params.frame_height = height;
        ok(set, location);
    }
    /* The command that queues `resource` on a queue; `size` is the length of an encoded packet. */
    virtio_video_resource_queue queueCommand(std::uint32_t queueType, std::uint32_t resource,
                                             std::uint32_t size = 0, std::uint64_t timestamp = 0) const
    {
        virtio_video_resource_queue queue {};
        queue.cmd_type = VIRTIO_VIDEO_CMD_RESOURCE_QUEUE;
        queue.stream_id = stream;
        queue.queue_type = queueType;
        queue.resource_id = resource;
        queue.data_sizes[0] = size;
        queue.timestamp = timestamp;
        return queue;
    }
    /* The five encoded bytes every case feeds, split between the end of the first page and the
     * start of the third so the device has to gather two segments, attached as input `resource`. */
    void attachEncodedInput(unsigned resource = 1, std::source_location location = std::source_location::current())
    {
        memory[page - 2] = 1; memory[page - 1] = 2;
        memory[2 * page] = 3; memory[2 * page + 1] = 4; memory[2 * page + 2] = 5;
        require(load<virtio_video_cmd_hdr>(attach(VIRTIO_VIDEO_QUEUE_TYPE_INPUT, resource,
                    {{{page - 2, 2, {}}, {2 * page, 3, {}}}}).response).type == VIRTIO_VIDEO_RESP_OK_NODATA,
                "input attach failed", location);
    }

    VirtioVideoDevice device;
    std::vector<std::uint8_t> memory;
    std::uint32_t stream;
    std::uint64_t next {1};
};

void exercise(unsigned format)
{
    const bool ten = format == VIRTIO_VIDEO_FORMAT_P010;
    VideoFixture video(7, 8 * page, [ten](auto) { return std::make_unique<Decoder>(ten); },
        {{VideoCodec::h264, ten ? VideoProfile::h264High10 : VideoProfile::h264High, 4096, 2304, 51,
          ten ? 10u : 8u}});
    video.createStream();
    require(video.device.processQueue(1, 1000, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::defer, "event not deferred");
    video.setOutputFormat(format);
    const auto input = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    const auto output = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    video.attachEncodedInput();
    require(load<virtio_video_cmd_hdr>(video.attach(input, 9, {{{video.memory.size(), 1, {}}}}).response).type == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "invalid guest address accepted");
    Planes planes;
    const unsigned sizes[] = {ten ? 12288u : 4096u, format == VIRTIO_VIDEO_FORMAT_YUV420 ? 1024u : 2048u, 1024u};
    const unsigned count = ten ? 1 : format == VIRTIO_VIDEO_FORMAT_YUV420 ? 3 : 2;
    for (unsigned i = 0; i < count; ++i) {
        const auto base = (3 + i) * page;
        planes.push_back({{base, 97, {}}, {base + 97, sizes[i] - 97, {}}});
    }
    require(load<virtio_video_cmd_hdr>(video.attach(output, 2, planes).response).type == VIRTIO_VIDEO_RESP_OK_NODATA, "output attach failed");
    const auto capture = video.queueCommand(output, 2);
    const auto captureRequest = video.next;
    require(video.command(capture).action == VhostUserQueueAction::defer, "capture did not wait for input");
    require(load<virtio_video_cmd_hdr>(video.command(capture).response).type == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "duplicate queue accepted");
    const auto packet = video.queueCommand(input, 1, 5, 7654321);
    require(load<virtio_video_resource_queue_resp>(video.command(packet).response).flags == 0, "decode input failed");
    VhostUserQueueCompletion completion;
    require(video.device.takeQueueCompletion(completion) && completion.requestId == 1000, "missing source event");
    require(load<virtio_video_event>(completion.response).event_type == VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED, "incorrect source event");
    require(video.device.takeQueueCompletion(completion) && completion.requestId == captureRequest, "missing capture completion");
    const auto result = load<virtio_video_resource_queue_resp>(completion.response);
    require(result.flags == 0 && result.timestamp == packet.timestamp && result.data_sizes[0] == sizes[0], "incorrect frame completion");
    require(video.memory[3 * page] == patternLumaByte(0, ten) &&
            video.memory[3 * page + 4095] == patternLumaByte(4095, ten), "luma changed");
    const auto chroma = ten ? 3 * page + 8192 : 4 * page;
    require(video.memory[chroma] == 0x27, "chroma changed");
    require(video.memory[chroma + 1] == (format == VIRTIO_VIDEO_FORMAT_YUV420 ? 0x27 : 0x91), "chroma layout changed");
    if (count == 3) require(video.memory[5 * page] == 0x91, "third plane changed");
    video.ok(virtio_video_stream_drain{{VIRTIO_VIDEO_CMD_STREAM_DRAIN, 7}});
    require(video.command(capture).action == VhostUserQueueAction::defer, "EOS capture failed");
    require(video.device.takeQueueCompletion(completion), "missing EOS");
    require(load<virtio_video_resource_queue_resp>(completion.response).flags == (VIRTIO_VIDEO_DEQUEUE_FLAG_EOS), "EOS flag missing");
    require(video.command(capture).action == VhostUserQueueAction::defer, "post-EOS capture failed");
    video.ok(virtio_video_queue_clear{{VIRTIO_VIDEO_CMD_QUEUE_CLEAR, 7}, output, {}});
    require(video.device.takeQueueCompletion(completion), "clear lost outstanding capture");
    require(load<virtio_video_resource_queue_resp>(completion.response).flags == (VIRTIO_VIDEO_DEQUEUE_FLAG_ERR), "clear did not cancel capture");
    video.ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 7}});
    video.device.reset();
    require(!video.device.takeQueueCompletion(completion), "reset retained completions");
    for (unsigned i = 0; i < 256; ++i)
        require(video.device.processQueue(1, 2000 + i, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::defer, "event descriptor was completed without an event");
    require(video.device.processQueue(1, 3000, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::retry, "full event queue did not apply backpressure");
    video.device.reset();

}

std::vector<std::uint32_t> levelsOf(VirtioVideoDevice& device, std::uint32_t format)
{
    std::vector<std::uint8_t> request;
    append(request, virtio_video_query_control{{VIRTIO_VIDEO_CMD_QUERY_CONTROL, 0}, VIRTIO_VIDEO_CONTROL_LEVEL, {}});
    append(request, virtio_video_query_control_level{format, {}});
    const auto result = device.processQueue(0, 3, request, 256);
    const auto header = load<virtio_video_cmd_hdr>(result.response);
    if (header.type != VIRTIO_VIDEO_RESP_OK_QUERY_CONTROL) return {};
    const auto bytes = std::span(result.response).subspan(sizeof(header));
    const auto count = load<virtio_video_query_control_resp_level>(bytes).num;
    std::vector<std::uint32_t> levels;
    for (std::uint32_t i = 0; i < count; ++i)
        levels.push_back(load<std::uint32_t>(bytes.subspan(sizeof(virtio_video_query_control_resp_level) + 4 * i)));
    return levels;
}

/* A profile the host lists reaches the guest as its own wire profile, as
 * Constrained Baseline does, so the guest's profile menu names it. */
void h264ProfileControls()
{
    const std::vector<VideoProfile> profiles {VideoProfile::h264Baseline, VideoProfile::h264ConstrainedBaseline,
        VideoProfile::h264Main, VideoProfile::h264High, VideoProfile::h264ConstrainedHigh};
    std::vector<VideoDecodeCapability> capabilities;
    for (const auto profile : profiles) capabilities.push_back({VideoCodec::h264, profile, 4096, 2304, 52, 8});
    VirtioVideoDevice device(capabilities, [](auto) { return std::make_unique<Decoder>(false); });
    std::vector<std::uint8_t> request;
    append(request, virtio_video_query_control{{VIRTIO_VIDEO_CMD_QUERY_CONTROL, 0}, VIRTIO_VIDEO_CONTROL_PROFILE, {}});
    append(request, virtio_video_query_control_profile{VIRTIO_VIDEO_FORMAT_H264, {}});
    const auto result = device.processQueue(0, 3, request, 256);
    const auto header = load<virtio_video_cmd_hdr>(result.response);
    require(header.type == VIRTIO_VIDEO_RESP_OK_QUERY_CONTROL, "the profile query failed");
    const auto bytes = std::span(result.response).subspan(sizeof(header));
    const std::vector<std::uint32_t> wanted {VIRTIO_VIDEO_PROFILE_H264_BASELINE,
        VIRTIO_VIDEO_PROFILE_H264_CONSTRAINED_BASELINE, VIRTIO_VIDEO_PROFILE_H264_MAIN,
        VIRTIO_VIDEO_PROFILE_H264_HIGH, VIRTIO_VIDEO_PROFILE_H264_CONSTRAINED_HIGH};
    require(load<virtio_video_query_control_resp_profile>(bytes).num == wanted.size(), "wrong number of profiles");
    for (std::size_t i = 0; i < wanted.size(); ++i)
        require(load<std::uint32_t>(bytes.subspan(sizeof(virtio_video_query_control_resp_profile) + 4 * i)) == wanted[i],
                "an H.264 profile is missing from the wire list");
}

/* The levels a stream may carry are the ones the capability states. The
 * device neither claims a level the hardware refuses nor withholds one it
 * decodes, and the current level is the highest. */
void codecLevelControls()
{
    struct Case { VideoCodec codec; std::uint32_t maximum, highest, count; };
    for (const auto& c : {Case {VideoCodec::h264, 52, VIRTIO_VIDEO_LEVEL_H264_5_2, 16},
                          Case {VideoCodec::h264, 51, VIRTIO_VIDEO_LEVEL_H264_5_1, 15},
                          Case {VideoCodec::h264, 62, VIRTIO_VIDEO_LEVEL_H264_6_2, 19},
                          Case {VideoCodec::hevc, 186, VIRTIO_VIDEO_LEVEL_HEVC_6_2, 13},
                          Case {VideoCodec::hevc, 153, VIRTIO_VIDEO_LEVEL_HEVC_5_1, 9},
                          Case {VideoCodec::jpeg, 0, 0, 0}}) {
        const auto codec = c.codec;
        VirtioVideoDevice device({{codec, codec == VideoCodec::hevc ? VideoProfile::hevcMain :
            codec == VideoCodec::jpeg ? VideoProfile::jpegBaseline : VideoProfile::h264High,
            4096, 2304, c.maximum, 8}}, [](auto) { return std::make_unique<Decoder>(false); });
        virtio_video_stream_create create {};
        create.hdr = {VIRTIO_VIDEO_CMD_STREAM_CREATE, 1};
        create.coded_format = wireCodec(codec);
        std::vector<std::uint8_t> request;
        append(request, create);
        require(load<virtio_video_cmd_hdr>(device.processQueue(0, 1, request, 128).response).type ==
            VIRTIO_VIDEO_RESP_OK_NODATA, "control stream creation failed");
        request.clear();
        append(request, virtio_video_get_control{{VIRTIO_VIDEO_CMD_GET_CONTROL, 1}, VIRTIO_VIDEO_CONTROL_LEVEL, {}});
        const auto result = device.processQueue(0, 2, request, 128);
        const auto header = load<virtio_video_cmd_hdr>(result.response);
        const auto levels = levelsOf(device, wireCodec(codec));
        if (codec == VideoCodec::jpeg) {
            require(header.type == VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL, "JPEG exposed a coded level");
            require(levels.empty(), "JPEG listed coded levels");
        } else {
            require(header.type == VIRTIO_VIDEO_RESP_OK_GET_CONTROL, "level control failed");
            const auto value = load<virtio_video_control_val_level>(std::span(result.response).subspan(sizeof(header)));
            require(value.level == c.highest, "the level is not the capability's maximum");
            require(levels.size() == c.count && levels.back() == c.highest &&
                    std::is_sorted(levels.begin(), levels.end()),
                    "the level list is not every level up to the capability's maximum");
            require(levels.front() == (codec == VideoCodec::h264 ? VIRTIO_VIDEO_LEVEL_H264_1_0 :
                VIRTIO_VIDEO_LEVEL_HEVC_1), "the level list starts at the wrong level");
        }
    }
}

/* The encoder's commands share this header, but a decoder never offers
 * VIRTIO_VIDEO_F_V4L2_CONTROLS, and one that receives such a command anyway
 * answers an error and leaves its stream as it was. */
void decoderRejectsEncoderCommands()
{
    VideoFixture video(7, 4 * page, [](auto) { return std::make_unique<Decoder>(false); });
    require((video.device.features() & (1ull << VIRTIO_VIDEO_F_V4L2_CONTROLS)) == 0,
            "a decoder offered the encoder control feature");
    video.createStream();
    const auto rejected = [&video](const auto& value, const char* what) {
        require(load<virtio_video_cmd_hdr>(video.command(value).response).type >=
                VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION, what);
    };
    rejected(virtio_video_query_v4l2_controls{{VIRTIO_VIDEO_CMD_QUERY_V4L2_CONTROLS, 0}, 0, {}},
             "a decoder answered a control query");
    rejected(virtio_video_v4l2_controls{{VIRTIO_VIDEO_CMD_SET_V4L2_CONTROLS, video.stream}, 0, {}},
             "a decoder accepted control values");
    rejected(virtio_video_get_stream_info{{VIRTIO_VIDEO_CMD_GET_STREAM_INFO, video.stream},
                                          VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT, {}},
             "a decoder answered a stream information query");
    rejected(virtio_video_set_stream_info{{VIRTIO_VIDEO_CMD_SET_STREAM_INFO, video.stream},
                                          {VIRTIO_VIDEO_QUEUE_TYPE_INPUT, 1, 30, 0, 0, 0, 0, {}}},
             "a decoder accepted stream information");
    video.setOutputFormat(VIRTIO_VIDEO_FORMAT_NV12);
}

/* The activity log is how an observer outside the guest tells a real hardware
 * decode from a silent software fallback, so it has to stay silent unless it
 * is asked for and to name what each stream actually decoded when it is.
 */
void sharedMemoryCapture(); // exercised again below, with the log on
void packedCapture(bool decoderPacks, std::uint32_t width, std::uint32_t height);
void packedDefaultThroughStreamon();

void activityLog()
{
    const auto path = uml::testing::scratch_directory() /
        ("uml-video-activity-" + std::to_string(::getpid()) + ".log");
    std::filesystem::remove(path);
    ::unsetenv("UML_VIDEO_ACTIVITY_LOG");
    exercise(VIRTIO_VIDEO_FORMAT_NV12);
    require(!std::filesystem::exists(path), "the video activity log is not off by default");

    ::setenv("UML_VIDEO_ACTIVITY_LOG", path.c_str(), 1);
    exercise(VIRTIO_VIDEO_FORMAT_NV12);
    ::unsetenv("UML_VIDEO_ACTIVITY_LOG");
    std::ostringstream buffer;
    buffer << std::ifstream(path).rdbuf();
    const auto text = buffer.str();
    std::filesystem::remove(path);
    require(text.find("advertise codec=h264 profile=h264-high max=4096x2304 depth=8") != std::string::npos,
        "the video activity log omits the advertised profile");
    require(text.find("stream=7 open codec=h264 profile=h264-high") != std::string::npos,
        "the video activity log omits the stream a guest opened");
    require(text.find("stream=7 close=destroy packets=1 bytes=5 frames=1 output=64x64") != std::string::npos,
        "the video activity log omits what the stream decoded");
    /* The capture fields say where a client's capture memory lives and
     * whether it ever gives a filled buffer back, which is how a starved
     * client is told apart from a device that stops filling one. They are
     * appended, so the older prefix above still matches. */
    require(text.find("output=64x64 capture=guest-pages queued=3 distinct=1 "
                      "requeued=2 pending_at_close=0 raw=nv12 converted=0 coded=h264")
            != std::string::npos,
        "the video activity log omits the capture-queue facts");

    ::setenv("UML_VIDEO_ACTIVITY_LOG", path.c_str(), 1);
    sharedMemoryCapture();
    ::unsetenv("UML_VIDEO_ACTIVITY_LOG");
    std::ostringstream shared;
    shared << std::ifstream(path).rdbuf();
    const auto sharedText = shared.str();
    std::filesystem::remove(path);
    const auto closeLine = sharedText.find("stream=3 close=");
    require(closeLine != std::string::npos,
        "the video activity log omits a shared-memory stream");
    const auto line = sharedText.substr(closeLine, sharedText.find('\n', closeLine) - closeLine);
    require(line.find("capture=shm-region") != std::string::npos,
        "a shared-memory capture buffer is not named as one");
    require(line.find("queued=2 distinct=1 requeued=1") != std::string::npos,
        "the video activity log does not count a re-queued capture buffer");

    /* Which capture format a client negotiated, and how many of its frames the
     * device had to repack itself, separate a decoder that hands the client its
     * own layout from one whose every frame costs a per-pixel conversion. */
    for (const bool decoderPacks : {true, false}) {
        ::setenv("UML_VIDEO_ACTIVITY_LOG", path.c_str(), 1);
        packedCapture(decoderPacks, 64, 64);
        ::unsetenv("UML_VIDEO_ACTIVITY_LOG");
        std::ostringstream packed;
        packed << std::ifstream(path).rdbuf();
        const auto packedText = packed.str();
        std::filesystem::remove(path);
        require(packedText.find(decoderPacks ? "raw=bgra8888 converted=0"
                                             : "raw=bgra8888 converted=1") != std::string::npos,
            "the video activity log miscounts device-side packing");
    }

    /* Chromium's order, where the client never names a capture format and
     * simply starts the queue: the log has to show the packed format it was
     * given and no conversion at all, because the device held the encoded
     * packets until that silence became a choice. This is the number the
     * guest proof reads out of a real session. */
    ::setenv("UML_VIDEO_ACTIVITY_LOG", path.c_str(), 1);
    packedDefaultThroughStreamon();
    ::unsetenv("UML_VIDEO_ACTIVITY_LOG");
    std::ostringstream streamon;
    streamon << std::ifstream(path).rdbuf();
    const auto streamonText = streamon.str();
    std::filesystem::remove(path);
    require(streamonText.find("stream=8 close=destroy packets=2 bytes=10 frames=2 output=64x64")
            != std::string::npos,
        "the video activity log omits what a client that never chose a format decoded");
    require(streamonText.find("raw=bgra8888 converted=0") != std::string::npos,
        "a client that never chose a capture format still cost a device-side conversion");

    /* A guest driver creates its stream when the device node is opened, before
     * the client has chosen a codec, and switches the codec with SET_PARAMS. The
     * open line therefore names the codec the driver created with, and only the
     * close line can name the one that decoded. */
    std::vector<VideoDecodeCapability> both {
        {VideoCodec::h264, VideoProfile::h264High, 4096, 2304, 51, 8},
        {VideoCodec::vp9, VideoProfile::vp9Profile0, 4096, 2304, 0, 8}};
    ::setenv("UML_VIDEO_ACTIVITY_LOG", path.c_str(), 1);
    {
        VirtioVideoDevice device(both, [](auto) { return std::make_unique<Decoder>(false); });
        std::uint64_t next = 1;
        auto send = [&](const auto& value) {
            std::vector<std::uint8_t> request;
            append(request, value);
            auto result = device.processQueue(0, next++, request, 16384);
            require(load<virtio_video_cmd_hdr>(result.response).type == VIRTIO_VIDEO_RESP_OK_NODATA,
                    "a codec switch command failed");
        };
        virtio_video_stream_create create {};
        create.hdr = {VIRTIO_VIDEO_CMD_STREAM_CREATE, 9};
        create.coded_format = VIRTIO_VIDEO_FORMAT_H264;
        send(create);
        virtio_video_set_params set {};
        set.hdr = {VIRTIO_VIDEO_CMD_SET_PARAMS, 9};
        set.params.queue_type = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
        set.params.format = VIRTIO_VIDEO_FORMAT_VP9;
        set.params.frame_width = 1920; set.params.frame_height = 1200;
        send(set);
        send(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 9}});
    }
    ::unsetenv("UML_VIDEO_ACTIVITY_LOG");
    std::ostringstream switched;
    switched << std::ifstream(path).rdbuf();
    const auto switchedText = switched.str();
    std::filesystem::remove(path);
    require(switchedText.find("stream=9 open codec=h264") != std::string::npos,
        "the open line does not name the codec the stream was created with");
    require(switchedText.find("stream=9 close=destroy") != std::string::npos &&
            switchedText.find("coded=vp9") != std::string::npos,
        "the close line does not name the codec that decoded");
}

/* A capture buffer may be a host-visible blob: the guest names an offset in
 * the shared-memory region its GPU backend published instead of a
 * guest-physical address, and the decoder writes the frame into the host
 * memory behind that region. A range withdrawn while a buffer is queued must
 * fail that buffer rather than reach memory the guest no longer has.
 */
void sharedMemoryCapture()
{
    constexpr std::uint8_t regionId = 1;
    constexpr std::uint64_t regionOffset = 64 * 1024;
    constexpr std::size_t regionLength = 64 * 1024;
    const uml::testing::ScratchBacking backing {"uml-video-shm-XXXXXX", regionLength};
    const int descriptor = backing.descriptor();

    auto regions = std::make_shared<SharedMemoryRegions>();
    require(regions->publish(regionId, regionOffset, regionLength, descriptor, 0),
        "publishing a shared-memory range failed");
    require(!regions->publish(regionId, regionOffset + 4096, 4096, descriptor, 0),
        "an overlapping shared-memory range was published");
    require(regions->contains(regionId, regionOffset, 16) &&
        regions->contains(regionId, regionOffset + regionLength - 1, 1),
        "a published range does not contain its own memory");
    require(!regions->contains(regionId, regionOffset - 1, 1) &&
        !regions->contains(regionId, regionOffset + regionLength - 1, 2) &&
        !regions->contains(regionId + 1, regionOffset, 1),
        "an unpublished range resolved");

    VideoFixture video(3, 8 * page, [](auto) { return std::make_unique<Decoder>(false); });
    require((video.device.features() & (1 << VIRTIO_VIDEO_F_RESOURCE_SHM_REGION)) == 0,
        "a device with no shared-memory index offered the region feature");
    video.device.setSharedMemoryRegions(regions);
    require((video.device.features() & (1 << VIRTIO_VIDEO_F_RESOURCE_SHM_REGION)) != 0,
        "a device with a shared-memory index withheld the region feature");
    video.createStream();
    video.setOutputFormat(VIRTIO_VIDEO_FORMAT_NV12);
    const auto input = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    const auto output = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    video.memory[0] = 1; video.memory[1] = 2; video.memory[2] = 3; video.memory[3] = 4; video.memory[4] = 5;
    require(load<virtio_video_cmd_hdr>(video.attach(input, 1, {{{0, 5, {}}}}).response) .type ==
        VIRTIO_VIDEO_RESP_OK_NODATA, "input attach failed");

    const std::uint32_t shared = VIRTIO_VIDEO_RESOURCE_SG_FLAG_SHM_REGION |
        (std::uint32_t(regionId) << VIRTIO_VIDEO_RESOURCE_SG_SHM_ID_SHIFT);
    /* NV12 at 64x64: a 4096-byte luma plane followed by a 2048-byte chroma
     * plane, both inside the published range. */
    const Planes planes {
        {{regionOffset, 4096, shared}},
        {{regionOffset + 4096, 2048, shared}},
    };
    require(load<virtio_video_cmd_hdr>(video.attach(output, 2, planes).response).type ==
        VIRTIO_VIDEO_RESP_OK_NODATA, "shared-memory attach failed");
    require(load<virtio_video_cmd_hdr>(video.attach(output, 3, {
            {{regionOffset + regionLength, 4096, shared}},
            {{regionOffset, 2048, shared}},
        }).response).type == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
        "a range outside the published region was accepted");
    require(load<virtio_video_cmd_hdr>(video.attach(output, 4, {
            {{regionOffset, 4096, shared | 0x8000u}},
            {{regionOffset + 4096, 2048, shared}},
        }).response).type == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
        "an unknown scatter entry flag was accepted");
    /* Rebinding a buffer index to other memory reattaches its identifier. */
    require(load<virtio_video_cmd_hdr>(video.attach(output, 2, planes).response).type ==
        VIRTIO_VIDEO_RESP_OK_NODATA, "reattaching an idle resource failed");

    const auto capture = video.queueCommand(output, 2);
    const auto captureRequest = video.next;
    require(video.command(capture).action == VhostUserQueueAction::defer, "capture did not wait for input");
    require(load<virtio_video_cmd_hdr>(video.attach(output, 2, planes).response).type ==
        VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a queued resource was reattached");
    const auto packet = video.queueCommand(input, 1, 5, 12345);
    require(load<virtio_video_resource_queue_resp>(video.command(packet).response).flags == 0,
        "decode input failed");
    VhostUserQueueCompletion completion;
    bool delivered = false;
    while (video.device.takeQueueCompletion(completion)) {
        if (completion.requestId != captureRequest) continue;
        const auto result = load<virtio_video_resource_queue_resp>(completion.response);
        require(result.flags == 0 && result.timestamp == packet.timestamp,
            "shared-memory capture did not complete");
        delivered = true;
    }
    require(delivered, "shared-memory capture produced no completion");
    bool written = false;
    require(regions->resolve(regionId, regionOffset, 4096 + 2048,
        [&](std::span<std::uint8_t> frame) {
            written = frame[0] == patternLumaByte(0, false) &&
                frame[4095] == patternLumaByte(4095, false) && frame[4096] == 0x27 &&
                frame[4097] == 0x91;
        }), "the published range could not be resolved");
    require(written, "the decoded frame did not reach the shared-memory range");

    /* A blob revoked while its buffer is queued fails that buffer. */
    const auto revokedRequest = video.next;
    require(video.command(capture).action == VhostUserQueueAction::defer, "second capture failed");
    regions->revoke(regionId, regionOffset, regionLength);
    require(!regions->contains(regionId, regionOffset, 1), "revoking a range left it published");
    require(load<virtio_video_resource_queue_resp>(video.command(packet).response).flags == 0,
        "decode input after revocation failed");
    bool failed = false;
    while (video.device.takeQueueCompletion(completion)) {
        if (completion.requestId != revokedRequest) continue;
        failed = load<virtio_video_resource_queue_resp>(completion.response).flags ==
            VIRTIO_VIDEO_DEQUEUE_FLAG_ERR;
    }
    require(failed, "a capture buffer whose region was revoked did not fail");
    video.ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 3}});
    video.device.reset();
    regions->revokeAll();
}

/* A client that only draws through a 32-bit RGB surface negotiates packed
 * capture. The frames the decoder produces packed arrive unchanged; a frame it
 * produced planar before the negotiation is converted by the device, so the
 * client never sees the format it did not ask for. */
void packedCapture(bool decoderPacks, std::uint32_t width = 64,
                   std::uint32_t height = 64)
{
    // Rows lie at the pitch a guest allocator gives a buffer: whole pixels that fill 16 bytes.
    const std::size_t pitch = (std::size_t(width) * 4 + 15) / 16 * 16;
    const std::size_t plane = pitch * height;
    VideoFixture video(7, 3 * page + plane + page, [&](auto) {
        return std::make_unique<Decoder>(false, decoderPacks, false, width, height);
    });
    const auto input = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    const auto output = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;

    // The capture queue advertises the packed format for every stream.
    std::vector<std::uint8_t> query;
    append(query, virtio_video_query_capability{{VIRTIO_VIDEO_CMD_QUERY_CAPABILITY, 0}, output, {}});
    const auto capability = video.device.processQueue(0, video.next++, query, 65536).response;
    auto offset = sizeof(virtio_video_query_capability_resp);
    bool advertised = false;
    for (unsigned i = 0; i < load<virtio_video_query_capability_resp>(capability).num_descs; ++i) {
        const auto desc = load<virtio_video_format_desc>(
            std::span(capability).subspan(offset));
        if (desc.format == VIRTIO_VIDEO_FORMAT_BGRA8888) {
            require(desc.planes_layout == VIRTIO_VIDEO_PLANES_LAYOUT_SINGLE_BUFFER, "packed capture is not one buffer");
            require(desc.mask != 0, "packed capture reaches no codec");
            advertised = true;
        }
        offset += sizeof(virtio_video_format_desc);
        for (unsigned frame = 0; frame < desc.num_frames; ++frame) {
            const auto described = load<virtio_video_format_frame>(std::span(capability).subspan(offset));
            offset += sizeof(virtio_video_format_frame) +
                described.num_rates * sizeof(virtio_video_format_range);
        }
    }
    require(advertised, "packed capture format is not advertised");

    video.createStream();
    video.setOutputFormat(VIRTIO_VIDEO_FORMAT_BGRA8888, width, height);
    virtio_video_get_params get {};
    get.hdr = {VIRTIO_VIDEO_CMD_GET_PARAMS, 7};
    get.queue_type = output;
    const auto negotiated = load<virtio_video_get_params_resp>(video.command(get).response).params;
    require(negotiated.format == VIRTIO_VIDEO_FORMAT_BGRA8888, "packed capture was substituted");
    require(negotiated.num_planes == 1, "packed capture is not a single plane");
    require(negotiated.plane_formats[0].stride == pitch, "packed capture stride differs");
    require(negotiated.plane_formats[0].plane_size == plane, "packed capture plane size differs");

    video.attachEncodedInput();
    const std::uint64_t base = 3 * page;
    require(load<virtio_video_cmd_hdr>(video.attach(output, 2, {{{base, std::uint32_t(plane), {}}}}).response).type == VIRTIO_VIDEO_RESP_OK_NODATA, "packed output attach failed");
    const auto capture = video.queueCommand(output, 2);
    const auto captureRequest = video.next;
    require(video.command(capture).action == VhostUserQueueAction::defer, "capture did not wait for input");
    const auto packet = video.queueCommand(input, 1, 5, 7654321);
    require(load<virtio_video_resource_queue_resp>(video.command(packet).response).flags == 0, "decode input failed");
    VhostUserQueueCompletion completion;
    require(video.device.takeQueueCompletion(completion) && completion.requestId == captureRequest, "missing capture completion");
    const auto result = load<virtio_video_resource_queue_resp>(completion.response);
    require(result.flags == 0 && result.timestamp == packet.timestamp, "packed frame was not delivered");
    require(result.data_sizes[0] == plane, "packed frame size differs");
    /* BT.601 limited range, matching what the platform decoder infers from an
     * unspecified color attachment. */
    /* Every row, not a corner and not the first quarter: a destination row
     * written at the wrong offset holds a window of the picture that starts at
     * the wrong sample, which only a per-row comparison sees. */
    for (unsigned row = 0; row < height; ++row)
        for (unsigned column = 0; column < width; ++column) {
            const auto* got = video.memory.data() + base + std::size_t(row) * pitch + column * 4;
            std::uint8_t expected[4] = {std::uint8_t(row), std::uint8_t(column),
                                        std::uint8_t(row + column), 0xff};
            if (!decoderPacks) convertBt601Limited(patternLuma(row, column), 0x27, 0x91, expected);
            require(got[3] == 0xff, "packed alpha differs");
            // A rounding step apart is the most two correct conversions differ.
            const int slack = decoderPacks ? 0 : 1;
            for (unsigned channel = 0; channel < 3; ++channel)
                require(std::abs(int(got[channel]) - int(expected[channel])) <= slack,
                        "packed pixels differ");
        }
    require(video.memory[base + plane] == 0xcc, "packed frame ran past its plane");
    video.ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 7}});
    video.device.reset();
}

/* One plane of a raw capture as the guest lays it out: where it starts, how
 * far apart its rows are, and what it holds per row. */
struct CapturePlane {
    std::uint32_t pitch;
    std::uint32_t rows;
    std::uint32_t rowBytes;
};

/* Planar capture at a width that is not a multiple of the allocator's row
 * alignment, where every row after the first lands in the wrong place if the
 * device states or writes tight rows. The expected pitches are spelled out
 * here, not taken from the device. The first byte past each plane is a guard
 * the device must leave alone. */
void unalignedPlanar(unsigned format, std::uint32_t width, std::uint32_t height)
{
    const bool ten = format == VIRTIO_VIDEO_FORMAT_P010;
    const std::uint32_t cw = (width + 1) / 2, ch = (height + 1) / 2;
    std::vector<CapturePlane> expected;
    if (format == VIRTIO_VIDEO_FORMAT_P010) {
        const auto pitch = (2 * width + 15) / 16 * 16;
        expected = {{pitch, height + ch, 0}};
    } else if (format == VIRTIO_VIDEO_FORMAT_YUV420) {
        const auto luma = (width + 31) / 32 * 32;
        expected = {{luma, height, width}, {luma / 2, ch, cw}, {luma / 2, ch, cw}};
    } else {
        const auto pitch = (width + 15) / 16 * 16;
        expected = {{pitch, height, width}, {pitch, ch, 2 * cw}};
    }
    std::size_t total = 3 * page;
    std::vector<std::uint64_t> bases;
    for (const auto& plane : expected) {
        bases.push_back(total);
        total += (std::size_t(plane.pitch) * plane.rows + page) / page * page + page;
    }
    VideoFixture video(7, total, [&](auto) {
        return std::make_unique<Decoder>(ten, false, false, width, height, true);
    }, {{VideoCodec::h264, ten ? VideoProfile::h264High10 : VideoProfile::h264High, 4096, 2304, 51,
         ten ? 10u : 8u}});
    const auto input = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    const auto output = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    video.createStream();
    video.setOutputFormat(format, width, height);
    virtio_video_get_params get {};
    get.hdr = {VIRTIO_VIDEO_CMD_GET_PARAMS, 7};
    get.queue_type = output;
    const auto negotiated = load<virtio_video_get_params_resp>(video.command(get).response).params;
    require(negotiated.format == format && negotiated.num_planes == expected.size(),
            "planar capture was substituted");
    Planes planes;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        require(negotiated.plane_formats[i].stride == expected[i].pitch, "capture stride differs");
        require(negotiated.plane_formats[i].plane_size == expected[i].pitch * expected[i].rows,
                "capture plane size differs");
        planes.push_back({{bases[i], negotiated.plane_formats[i].plane_size, {}}});
    }

    video.attachEncodedInput();
    require(load<virtio_video_cmd_hdr>(video.attach(output, 2, planes).response).type == VIRTIO_VIDEO_RESP_OK_NODATA,
            "capture attach failed");
    const auto capture = video.queueCommand(output, 2);
    const auto captureRequest = video.next;
    require(video.command(capture).action == VhostUserQueueAction::defer, "capture did not wait for input");
    const auto packet = video.queueCommand(input, 1, 5, 7654321);
    require(load<virtio_video_resource_queue_resp>(video.command(packet).response).flags == 0, "decode input failed");
    VhostUserQueueCompletion completion;
    require(video.device.takeQueueCompletion(completion) && completion.requestId == captureRequest,
            "missing capture completion");
    require(load<virtio_video_resource_queue_resp>(completion.response).flags == 0, "frame was not delivered");

    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& plane = expected[i];
        for (std::uint32_t row = 0; row < plane.rows; ++row) {
            const auto* got = video.memory.data() + bases[i] + std::size_t(row) * plane.pitch;
            if (ten) {
                // One plane: the luma rows, then the interleaved chroma rows, at one pitch.
                for (std::uint32_t column = 0; column < (row < height ? width : 2 * cw); ++column)
                    for (std::size_t byte = 0; byte < 2; ++byte)
                        require(got[column * 2 + byte] ==
                                    (row < height ? patternLuma(row, column) : patternChroma(row - height, column)),
                                "P010 picture differs");
                continue;
            }
            for (std::uint32_t column = 0; column < plane.rowBytes; ++column) {
                std::uint8_t want;
                if (i == 0) want = patternLuma(row, column);
                else if (format == VIRTIO_VIDEO_FORMAT_YUV420)
                    want = patternChroma(row, 2 * column + (i == 2 ? 1 : 0));
                else want = patternChroma(row, column);
                require(got[column] == want, "planar picture differs");
            }
        }
        require(video.memory[bases[i] + std::size_t(plane.pitch) * plane.rows] == 0xcc,
                "a plane was written past its end");
    }
    video.ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 7}});
    video.device.reset();
}

/* The rule, as vectors that do not depend on the function that implements it. */
void rawPitchRule()
{
    require(rawPitch(340) == 352 && rawPitch(520) == 528 && rawPitch(352) == 352 && rawPitch(1) == 16,
            "8-bit semi-planar pitch differs");
    require(rawPitch(2 * 340) == 688 && rawPitch(2 * 520) == 1040 && rawPitch(2 * 99) == 208,
            "10-bit pitch differs");
    require(rawPitch(4 * 341) == 1376 && rawPitch(4 * 64) == 256, "packed pitch differs");
    require(rawPitch(340, rawTriplanarLumaAlignment) == 352 && rawPitch(99, rawTriplanarLumaAlignment) == 128,
            "triplanar luma pitch differs");
    const std::vector<std::uint8_t> tight(6, 7);
    require(padRows(tight, 3, 3) == tight, "a tight picture was copied");
    require(padRows(tight, 3, 4) == std::vector<std::uint8_t>({7, 7, 7, 0, 7, 7, 7, 0}), "rows were not padded");
    require(padRows(tight, 3, 2).empty() && padRows(tight, 4, 4).empty(), "a skewed picture was accepted");
}

/* A client that changes its capture format after frames have already been
 * delivered: it settles on planar, decodes, and only then asks for packed.
 * The decoder is in the middle of a group of pictures by then and cannot
 * change layout there, so every frame must still reach the client packed,
 * whether the decoder adopted the layout or the device converted for it. */
void lateNegotiation()
{
    VideoFixture video(5, 8 * page, [](auto) { return std::make_unique<Decoder>(false, true, true); });
    const auto input = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    const auto output = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    video.createStream();
    require(video.device.processQueue(1, 900, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::defer,
            "event not deferred");
    video.attachEncodedInput();

    /* Settle on planar first, so that pictures are delivered before the
     * client changes its mind. Until a format is settled the device holds the
     * encoded packets rather than decoding them into a layout nobody asked
     * for, which is what keeps the ordinary path free of conversion. */
    video.setOutputFormat(VIRTIO_VIDEO_FORMAT_NV12);

    // Decoding raises the source change.
    auto packet = video.queueCommand(input, 1, 5, 11);
    require(load<virtio_video_resource_queue_resp>(video.command(packet).response).flags == 0, "decode input failed");
    VhostUserQueueCompletion completion;
    require(video.device.takeQueueCompletion(completion) && completion.requestId == 900, "missing source event");
    require(load<virtio_video_event>(completion.response).event_type == VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED,
            "incorrect source event");

    // Only now does the client choose packed capture.
    video.setOutputFormat(VIRTIO_VIDEO_FORMAT_BGRA8888);
    const std::uint64_t base = 3 * page;
    require(load<virtio_video_cmd_hdr>(video.attach(output, 2, {{{base, 64 * 64 * 4, {}}}}).response).type
            == VIRTIO_VIDEO_RESP_OK_NODATA, "packed output attach failed");

    /* Three pictures: the one decoded before the request, one the decoder is
     * still deferring, and one it has adopted the layout for. The request
     * reaches the decoder in the middle of a group of pictures, so a deferring
     * decoder is still planar for the first two and the device converts them. */
    for (unsigned round = 0; round < 3; ++round) {
        std::memset(video.memory.data() + base, 0xcc, 64 * 64 * 4);
        const auto capture = video.queueCommand(output, 2);
        const auto captureRequest = video.next;
        require(video.command(capture).action == VhostUserQueueAction::defer, "capture was not deferred");
        if (round) {
            packet.timestamp = 11 + round;
            require(load<virtio_video_resource_queue_resp>(video.command(packet).response).flags == 0,
                    "later decode failed");
        }
        require(video.device.takeQueueCompletion(completion) && completion.requestId == captureRequest,
                "missing capture completion");
        const auto result = load<virtio_video_resource_queue_resp>(completion.response);
        require(result.flags == 0, "late negotiation failed a capture buffer");
        require(result.data_sizes[0] == 64 * 64 * 4, "packed frame size differs");
        for (unsigned row = 0; row < 64; ++row)
            for (unsigned column = 0; column < 64; ++column) {
                const auto* got = video.memory.data() + base + (std::size_t(row) * 64 + column) * 4;
                std::uint8_t expected[4] = {std::uint8_t(row), std::uint8_t(column),
                                            std::uint8_t(row + column), 0xff};
                if (round != 2) convertBt601Limited(patternLuma(row, column), 0x27, 0x91, expected);
                require(got[3] == 0xff, "a late-negotiated frame lost its alpha");
                for (unsigned channel = 0; channel < 3; ++channel)
                    require(std::abs(int(got[channel]) - int(expected[channel])) <= (round == 2 ? 0 : 1),
                            "a late-negotiated frame has the wrong pixels");
            }
    }
    video.ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 5}});
    video.device.reset();
}

/* A stateful client cannot name its capture format until the source change.
 * The device therefore reports the size from the packet's headers and holds
 * the packet, so the client's choice is known before anything is decoded and
 * the planar format it names is the layout its first picture already has.
 * Nothing is converted and nothing is round-tripped through another colour
 * space, so the samples the client reads are the decoder's own. */
void planarAfterSourceChange()
{
    // A decoder that CAN pack, so a premature request for packing shows up.
    VideoFixture video(6, 8 * page, [](auto) { return std::make_unique<Decoder>(false, true); });
    const auto input = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    const auto output = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    video.createStream();
    require(video.device.processQueue(1, 901, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::defer,
            "event not deferred");
    video.attachEncodedInput();

    /* The client's first act: queue a packet. Its headers say how big the
     * picture is, which is what raises the source change; the packet itself
     * waits, because the capture format is still the client's to choose. */
    const auto packet = video.queueCommand(input, 1, 5, 21);
    const auto packetRequest = video.next;
    require(video.command(packet).action == VhostUserQueueAction::defer,
            "the packet was decoded before the client chose a capture format");
    VhostUserQueueCompletion completion;
    require(video.device.takeQueueCompletion(completion) && completion.requestId == 901, "missing source event");
    require(load<virtio_video_event>(completion.response).event_type ==
            VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED, "incorrect source event");
    require(!video.device.takeQueueCompletion(completion),
            "an encoded buffer came back before the capture format settled");

    // Only now can it choose, and it chooses the planar format.
    video.setOutputFormat(VIRTIO_VIDEO_FORMAT_NV12);
    const std::uint64_t base = 3 * page;
    const std::size_t luma = 64 * 64, chroma = 64 * 32;
    require(load<virtio_video_cmd_hdr>(video.attach(output, 2,
                {{{base, std::uint32_t(luma), {}}}, {{base + luma, std::uint32_t(chroma), {}}}}).response).type
            == VIRTIO_VIDEO_RESP_OK_NODATA, "planar output attach failed");

    /* Naming the format releases what was held, so the picture is decoded
     * into the layout the client just named. */
    require(video.device.takeQueueCompletion(completion) && completion.requestId == packetRequest,
            "settling the capture format did not release the held packet");
    require(load<virtio_video_resource_queue_resp>(completion.response).flags == 0,
            "the released encoded buffer came back failed");

    const auto capture = video.queueCommand(output, 2);
    const auto captureRequest = video.next;
    require(video.command(capture).action == VhostUserQueueAction::defer, "capture was not deferred");
    require(video.device.takeQueueCompletion(completion) && completion.requestId == captureRequest,
            "missing capture completion");
    const auto result = load<virtio_video_resource_queue_resp>(completion.response);
    require(result.flags == 0,
            "the picture decoded before the client chose was failed instead of delivered");
    require(result.data_sizes[0] == luma && result.data_sizes[1] == chroma, "planar frame sizes differ");
    for (std::uint32_t row = 0; row < 64; ++row)
        for (std::uint32_t column = 0; column < 64; ++column)
            require(video.memory[base + row * 64 + column] == patternLuma(row, column),
                    "the planar frame has the wrong pixels");
    video.ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 6}});
    video.device.reset();
}

/* Chromium's order: a client that never names a format and simply starts the
 * capture queue with the packed one it was given. Starting the queue settles
 * the choice, and because nothing has been decoded yet the decoder takes the
 * packed layout for its very first picture - even this decoder, which like the
 * hardware can only change layout at a random access point. Every frame is
 * therefore packed by the decoder and none is converted by the device, which
 * the pixels prove: a converted frame cannot be exact through a BT.601 round
 * trip and a hardware-packed one is. */
void packedDefaultThroughStreamon()
{
    const std::size_t plane = 64 * 64 * 4;
    VideoFixture video(8, 3 * page + plane + page, [](auto) { return std::make_unique<Decoder>(false, true, true); });
    const auto input = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    const auto output = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    video.createStream();
    // The format the client is given without asking is the packed one.
    virtio_video_get_params get {};
    get.hdr = {VIRTIO_VIDEO_CMD_GET_PARAMS, 8};
    get.queue_type = output;
    require(load<virtio_video_get_params_resp>(video.command(get).response).params.format
            == VIRTIO_VIDEO_FORMAT_BGRA8888, "the default capture format is not the packed one");
    video.attachEncodedInput();
    const std::uint64_t base = 3 * page;
    require(load<virtio_video_cmd_hdr>(video.attach(output, 2, {{{base, std::uint32_t(plane), {}}}}).response).type
            == VIRTIO_VIDEO_RESP_OK_NODATA, "packed output attach failed");

    auto packet = video.queueCommand(input, 1, 5);
    VhostUserQueueCompletion completion;
    for (unsigned round = 0; round < 2; ++round) {
        std::uint64_t packetRequest = 0;
        if (round == 0) {
            /* Queue a packet before the capture queue starts. Its headers say
             * how big the picture is; the packet itself waits, because the
             * client's silence is not yet a choice. */
            packet.timestamp = 31;
            packetRequest = video.next;
            require(video.command(packet).action == VhostUserQueueAction::defer,
                    "the packet was decoded before the capture queue started");
        }
        std::memset(video.memory.data() + base, 0xcc, plane);
        const auto capture = video.queueCommand(output, 2);
        const auto captureRequest = video.next;
        require(video.command(capture).action == VhostUserQueueAction::defer, "capture was not deferred");
        if (round) {
            packet.timestamp = 32;
            require(load<virtio_video_resource_queue_resp>(video.command(packet).response).flags == 0,
                    "later decode failed");
        }
        if (round == 0) {
            require(video.device.takeQueueCompletion(completion) && completion.requestId == packetRequest,
                    "starting the capture queue did not release the held packet");
            require(load<virtio_video_resource_queue_resp>(completion.response).flags == 0,
                    "the released encoded buffer came back failed");
        }
        require(video.device.takeQueueCompletion(completion) && completion.requestId == captureRequest,
                "missing capture completion");
        const auto result = load<virtio_video_resource_queue_resp>(completion.response);
        require(result.flags == 0, "a picture was failed for a client that never chose a format");
        require(result.data_sizes[0] == plane, "packed frame size differs");
        for (unsigned row = 0; row < 64; ++row)
            for (unsigned column = 0; column < 64; ++column) {
                const auto* got = video.memory.data() + base + (std::size_t(row) * 64 + column) * 4;
                const std::uint8_t expected[4] = {std::uint8_t(row), std::uint8_t(column),
                                                  std::uint8_t(row + column), 0xff};
                require(got[3] == 0xff, "a packed frame lost its alpha");
                for (unsigned channel = 0; channel < 3; ++channel)
                    require(got[channel] == expected[channel],
                            "a picture was converted by the device instead of packed by the decoder");
            }
    }
    video.ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 8}});
    video.device.reset();
}

/* A client feeds packets at its own pace and takes frames at its display
 * rate, so a decoder that runs faster than the display accumulates pictures.
 * The device must answer that by keeping the encoded buffer instead of
 * failing the command: a failure here reaches the client as EIO on both
 * queues and ends playback. */
/* The wait for a capture format is bounded. A client that queues encoded data
 * and never says what it wants must not be waited on forever, so at the bound
 * the device stops waiting and decodes what it holds, in the planar layout
 * nobody overrode, rather than failing the stream. (The byte bound is the same
 * rule for a client whose packets are large rather than many; this decoder
 * only accepts a five-byte packet, so the packet bound is the one a test can
 * reach.) */
void unsettledHoldIsBounded()
{
    const std::size_t plane = 64 * 64 * 4;
    VideoFixture video(9, 3 * page + plane + page, [](auto) { return std::make_unique<Decoder>(false, true); });
    const auto input = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    const auto output = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    video.createStream();
    require(video.device.processQueue(1, 950, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::defer,
            "event not deferred");
    /* One encoded buffer more than the device will hold unanswered, each with
     * its own identifier because a held one stays queued. */
    const unsigned packets = 33;
    for (unsigned id = 1; id <= packets; ++id)
        video.attachEncodedInput(id);
    require(load<virtio_video_cmd_hdr>(video.attach(output, 100, {{{3 * page, std::uint32_t(plane), {}}}}).response).type
            == VIRTIO_VIDEO_RESP_OK_NODATA, "capture attach failed");

    // Never name a format, never start the capture queue: only queue input.
    std::deque<std::uint64_t> waiting;
    for (unsigned id = 1; id <= packets; ++id) {
        const auto packet = video.queueCommand(input, id, 5, 200 + id);
        const auto request = video.next;
        const auto result = video.command(packet);
        if (result.action == VhostUserQueueAction::defer) waiting.push_back(request);
        else {
            const auto answer = load<virtio_video_resource_queue_resp>(result.response);
            require(answer.hdr.type == VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE && answer.flags == 0,
                    "an encoded buffer failed instead of being decoded");
        }
    }
    VhostUserQueueCompletion completion;
    unsigned released = 0;
    while (video.device.takeQueueCompletion(completion)) {
        if (completion.requestId == 950) {
            require(load<virtio_video_event>(completion.response).event_type ==
                    VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED, "incorrect source event");
            continue;
        }
        require(!waiting.empty() && completion.requestId == waiting.front(),
                "an encoded buffer came back out of order");
        waiting.pop_front();
        ++released;
        require(load<virtio_video_resource_queue_resp>(completion.response).flags == 0,
                "a released encoded buffer came back failed");
    }
    require(released > 0, "the device waited for a capture format that was never going to come");

    /* The stream is still usable: the client can still take frames, and every
     * buffer it gave comes back. */
    while (!waiting.empty()) {
        const auto capture = video.queueCommand(output, 100);
        const auto captureRequest = video.next;
        require(video.command(capture).action == VhostUserQueueAction::defer, "capture was not deferred");
        bool frame = false;
        while (video.device.takeQueueCompletion(completion)) {
            if (completion.requestId == captureRequest) {
                frame = true;
                const auto result = load<virtio_video_resource_queue_resp>(completion.response);
                require(result.flags == 0, "a frame was failed after the bounded wait");
                require(result.data_sizes[0] == plane, "the frame has the wrong size");
            } else {
                require(!waiting.empty() && completion.requestId == waiting.front(),
                        "an encoded buffer came back out of order");
                waiting.pop_front();
            }
        }
        require(frame, "a returned capture buffer produced no frame");
    }
    video.ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 9}});
    video.device.reset();
}

void inputBackpressure()
{
    const std::size_t plane = 64 * 64 * 4;
    VideoFixture video(4, 3 * page + plane + page, [](auto) { return std::make_unique<Decoder>(false, true); });
    const auto input = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    const auto output = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    video.createStream();
    /* Choose the capture format up front, so the only thing that can hold an
     * encoded buffer here is the frame queue filling up. */
    video.setOutputFormat(VIRTIO_VIDEO_FORMAT_BGRA8888);
    // Twelve encoded buffers over the same five bytes, as a client's ring.
    const unsigned packets = 12;
    for (unsigned id = 1; id <= packets; ++id)
        video.attachEncodedInput(id);
    require(load<virtio_video_cmd_hdr>(video.attach(output, 100, {{{3 * page, std::uint32_t(plane), {}}}}).response).type
            == VIRTIO_VIDEO_RESP_OK_NODATA, "capture attach failed");

    std::deque<std::uint64_t> waiting;
    unsigned answered = 0;
    for (unsigned id = 1; id <= packets; ++id) {
        const auto packet = video.queueCommand(input, id, 5, 100 + id);
        const auto request = video.next;
        const auto result = video.command(packet);
        if (result.action == VhostUserQueueAction::defer) waiting.push_back(request);
        else {
            ++answered;
            const auto answer = load<virtio_video_resource_queue_resp>(result.response);
            require(answer.hdr.type == VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE && answer.flags == 0,
                    "an encoded buffer failed instead of waiting");
        }
    }
    require(answered > 0 && !waiting.empty(),
            "the device did not hold an encoded buffer against a full frame queue");
    /* Two decoded frames wait for a capture buffer and the third packet is
     * the first one held: more would be host memory no decode uses. */
    require(answered == 2, "the frame queue did not hold at two frames");

    /* Every capture buffer the client gives back releases one held packet, so
     * the stream keeps running instead of erroring out. */
    const auto held = waiting.size();
    for (std::size_t round = 0; round < held; ++round) {
        const auto capture = video.queueCommand(output, 100);
        const auto captureRequest = video.next;
        require(video.command(capture).action == VhostUserQueueAction::defer, "capture was not deferred");
        bool frame = false, released = false;
        VhostUserQueueCompletion completion;
        while (video.device.takeQueueCompletion(completion)) {
            if (completion.requestId == captureRequest) {
                frame = true;
                const auto result = load<virtio_video_resource_queue_resp>(completion.response);
                require(result.flags == 0, "a frame was failed under backpressure");
                require(result.data_sizes[0] == plane, "the frame has the wrong size");
            } else {
                require(!waiting.empty() && completion.requestId == waiting.front(),
                        "an encoded buffer came back out of order");
                waiting.pop_front();
                released = true;
                const auto result = load<virtio_video_resource_queue_resp>(completion.response);
                require(result.flags == 0 && result.data_sizes[0] == 5,
                        "a held encoded buffer came back failed");
            }
        }
        require(frame && released, "a returned capture buffer did not release a held packet");
    }
    require(waiting.empty(), "the device kept an encoded buffer with room to decode");
    video.ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 4}});
    video.device.reset();
}

/* The capture queue's minimum buffer count is the stream's reorder depth plus
 * one, and four at the least, from the announcement of the picture size on: a
 * client reads it as V4L2_CID_MIN_BUFFERS_FOR_CAPTURE to size its pipeline by
 * the stream instead of by the worst case any stream could have. Until the
 * size is announced the decoder has read no parameter set, and the answer is
 * the floor. */
void captureMinimumFollowsReorder(unsigned reorder)
{
    const auto input = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    VideoFixture video(12, 8 * page, [reorder](auto) {
        return std::make_unique<Decoder>(false, true, false, 64, 64, false, reorder);
    });
    video.createStream();
    require(video.device.processQueue(1, 920, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::defer,
            "event not deferred");
    video.attachEncodedInput();
    const auto minimum = [&video] {
        std::vector<std::uint8_t> request;
        append(request, virtio_video_get_params{{VIRTIO_VIDEO_CMD_GET_PARAMS, video.stream},
                                                VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT, {}});
        const auto params = load<virtio_video_get_params_resp>(
            video.device.processQueue(0, video.next++, request, 16384).response).params;
        require(params.max_buffers >= params.min_buffers, "the minimum exceeds the maximum buffer count");
        return params.min_buffers;
    };
    require(minimum() == 4, "the capture minimum before the size is known is not the floor");

    // The packet's headers announce the size, and with it the reorder depth.
    const auto packet = video.queueCommand(input, 1, 5, 31);
    require(video.command(packet).action == VhostUserQueueAction::defer, "the packet was decoded early");
    VhostUserQueueCompletion completion;
    require(video.device.takeQueueCompletion(completion) && completion.requestId == 920, "missing source event");
    const unsigned expected = std::max(4u, reorder + 1);
    require(minimum() == expected, "the announced capture minimum does not follow the reorder depth");

    // Choosing the capture format answers with the same minimum.
    video.setOutputFormat(VIRTIO_VIDEO_FORMAT_NV12);
    require(minimum() == expected, "choosing the capture format changed the minimum");
    video.ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 12}});
    video.device.reset();
}

/* The coded queue's buffer size follows the picture size the client states, so
 * a client that keeps several coded buffers queued does not hold the full packet
 * a device takes for a picture that is a fraction of it. A stream that has
 * stated no size keeps the full packet, and so does a JPEG one. */
void codedBufferFollowsPicture()
{
    constexpr std::uint32_t mebibyte = 1u << 20;
    VideoFixture video(13, 8 * page, [](auto) { return std::make_unique<Decoder>(false); },
                       {{VideoCodec::h264, VideoProfile::h264High, 8192, 4320, 62, 8},
                        {VideoCodec::jpeg, VideoProfile::jpegBaseline, 8192, 4320, 0, 8}});
    video.createStream();
    const auto codedSize = [&video] {
        std::vector<std::uint8_t> request;
        append(request, virtio_video_get_params{{VIRTIO_VIDEO_CMD_GET_PARAMS, video.stream},
                                                VIRTIO_VIDEO_QUEUE_TYPE_INPUT, {}});
        const auto params = load<virtio_video_get_params_resp>(
            video.device.processQueue(0, video.next++, request, 16384).response).params;
        require(params.num_planes == 1, "the coded queue is not one plane");
        return params.plane_formats[0].plane_size;
    };
    const auto state = [&video](std::uint32_t format, std::uint32_t width, std::uint32_t height) {
        virtio_video_set_params set {};
        set.hdr = {VIRTIO_VIDEO_CMD_SET_PARAMS, video.stream};
        set.params.queue_type = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
        set.params.format = format;
        set.params.frame_width = width; set.params.frame_height = height;
        video.ok(set);
    };
    require(codedSize() == 8 * mebibyte, "a stream with no stated size does not take the full packet");
    struct Case { std::uint32_t width, height, expected; };
    for (const auto& c : {Case {64, 64, mebibyte}, Case {1280, 720, mebibyte}, Case {720, 1280, mebibyte},
                          Case {1920, 1080, 2 * mebibyte}, Case {1920, 1088, 2 * mebibyte},
                          Case {1088, 1920, 2 * mebibyte}, Case {2560, 1440, 4 * mebibyte},
                          Case {3840, 2160, 4 * mebibyte}, Case {4096, 2304, 4 * mebibyte},
                          Case {5120, 2880, 8 * mebibyte}, Case {8192, 4320, 8 * mebibyte}}) {
        state(VIRTIO_VIDEO_FORMAT_H264, c.width, c.height);
        require(codedSize() == c.expected, "the coded buffer does not follow the stated picture size");
    }
    video.ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 13}});

    virtio_video_stream_create create {};
    create.hdr = {VIRTIO_VIDEO_CMD_STREAM_CREATE, video.stream};
    create.coded_format = VIRTIO_VIDEO_FORMAT_MJPEG;
    video.ok(create);
    state(VIRTIO_VIDEO_FORMAT_MJPEG, 640, 360);
    require(codedSize() == 8 * mebibyte, "a JPEG stream does not keep the full packet");
    video.ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 13}});
    video.device.reset();
}

/* How the client deals with a size change it has been told of. */
enum class Renegotiation { clearCapture, setNewSize, drain, clearCoded };

/* Where a picture's size changes, every picture of the old size is delivered
 * first, in the buffers set up for it; one more buffer then completes empty and
 * last; the client is told the new size; and nothing of the new size is written
 * into the buffers it still holds until it has set its queue up again. The
 * coded buffer that carried the change is not given back until then either, or
 * a client that drains as soon as its coded buffers are back would drain across
 * the change: whichever way the client deals with it releases that buffer. */
void resolutionChange(Renegotiation how)
{
    auto state = std::make_shared<ScriptedState>();
    // Pictures one and two are 64 x 64, three and four 32 x 32.
    VideoFixture video(9, 16 * page, [state](auto) {
        return std::make_unique<ScriptedDecoder>(std::vector<std::uint32_t> {64, 64, 32, 32}, state);
    });
    const auto input = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    const auto output = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    video.createStream();
    for (unsigned id = 0; id < 2; ++id)
        require(video.device.processQueue(1, 910 + id, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::defer,
                "event not deferred");
    video.setOutputFormat(VIRTIO_VIDEO_FORMAT_NV12);
    for (unsigned id = 1; id <= 4; ++id) video.attachEncodedInput(id);
    const std::uint64_t big = 3 * page, small = 6 * page;
    require(load<virtio_video_cmd_hdr>(video.attach(output, 2, {{{big, 4096, {}}}, {{big + page, 2048, {}}}}).response).type
            == VIRTIO_VIDEO_RESP_OK_NODATA, "capture attach failed");
    const auto size = [&video](std::uint32_t queue) {
        std::vector<std::uint8_t> request;
        append(request, virtio_video_get_params{{VIRTIO_VIDEO_CMD_GET_PARAMS, video.stream}, queue, {}});
        return load<virtio_video_get_params_resp>(video.device.processQueue(0, video.next++, request, 16384).response)
            .params.frame_width;
    };
    const auto flagsOf = [](const VhostUserQueueCompletion& completion) {
        return load<virtio_video_resource_queue_resp>(completion.response).flags;
    };
    VhostUserQueueCompletion completion;
    const auto captureRequestFor = [&video](std::uint32_t resource) {
        const auto capture = video.queueCommand(VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT, resource);
        const auto request = video.next;
        require(video.command(capture).action == VhostUserQueueAction::defer, "capture was not deferred");
        return request;
    };

    // Two pictures of the first size, each into a buffer of the client's.
    for (unsigned picture = 1; picture <= 2; ++picture) {
        const auto request = captureRequestFor(2);
        require(load<virtio_video_resource_queue_resp>(
                    video.command(video.queueCommand(input, picture, 5, 100 + picture)).response).flags == 0,
                "decode input failed");
        if (picture == 1) {
            require(video.device.takeQueueCompletion(completion) && completion.requestId == 910 &&
                    load<virtio_video_event>(completion.response).event_type == VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED,
                    "the first size was not announced");
        }
        require(video.device.takeQueueCompletion(completion) && completion.requestId == request,
                "missing capture completion");
        const auto result = load<virtio_video_resource_queue_resp>(completion.response);
        require(result.flags == 0 && result.timestamp == 100 + picture && result.data_sizes[0] == 4096 &&
                result.data_sizes[1] == 2048, "incorrect first-size picture");
        require(video.memory[big + 64 * 7 + 9] == scriptedLuma(picture, 7, 9), "first-size picture changed");
        require(!video.device.takeQueueCompletion(completion), "an extra completion");
    }
    require(size(output) == 64, "the capture size is the first one");

    /* The third packet is smaller. Nothing is announced and no buffer is
     * used for it while the client has none queued: it is the client's to
     * receive the old pictures first. The packet's own buffer waits too. */
    const auto changing = video.next;
    require(video.command(video.queueCommand(input, 3, 5, 103)).action == VhostUserQueueAction::defer,
            "the coded buffer that changes the size was given back at once");
    require(!video.device.takeQueueCompletion(completion), "a size change was announced ahead of the pictures before it");
    require(size(output) == 64, "the size changed before the old pictures were delivered");
    require(load<virtio_video_cmd_hdr>(video.command(video.queueCommand(input, 3, 5, 999)).response).type ==
            VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a coded buffer that is held was queued again");

    // The next buffer the client queues ends the old size.
    const auto last = captureRequestFor(2);
    require(video.device.takeQueueCompletion(completion) && completion.requestId == last, "missing last buffer");
    const auto marker = load<virtio_video_resource_queue_resp>(completion.response);
    require(marker.flags == VIRTIO_VIDEO_DEQUEUE_FLAG_EOS && marker.data_sizes[0] == 0, "the last buffer is not empty and last");
    require(video.device.takeQueueCompletion(completion) && completion.requestId == 911 &&
            load<virtio_video_event>(completion.response).event_type == VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED,
            "the new size was not announced after the last buffer");
    require(!video.device.takeQueueCompletion(completion), "an extra completion after the change");
    require(size(output) == 32, "the new size is not reported");

    /* A packet of the new size is no change and is given back at once; the
     * buffer of the change stays with the device until the client has dealt
     * with it. */
    require(load<virtio_video_resource_queue_resp>(
                video.command(video.queueCommand(input, 4, 5, 104)).response).flags == 0, "decode input failed");
    const auto changeCompletion = [&](const char* what) {
        require(video.device.takeQueueCompletion(completion) && completion.requestId == changing, what);
        const auto result = load<virtio_video_resource_queue_resp>(completion.response);
        require(result.flags == 0 && result.timestamp == 103 && result.data_sizes[0] == 5, what);
    };

    switch (how) {
    case Renegotiation::clearCapture: {
        // A buffer the client still holds, and a picture that would fit it, wait.
        const auto held = captureRequestFor(2);
        require(!video.device.takeQueueCompletion(completion), "a picture of the new size was written into an old buffer");
        video.ok(virtio_video_queue_clear{{VIRTIO_VIDEO_CMD_QUEUE_CLEAR, 9}, output, {}});
        require(video.device.takeQueueCompletion(completion) && completion.requestId == held && flagsOf(completion) ==
                VIRTIO_VIDEO_DEQUEUE_FLAG_ERR, "the old buffer was not returned");
        changeCompletion("clearing the capture queue did not give back the coded buffer of the change");
        break;
    }
    case Renegotiation::setNewSize:
        require(!video.device.takeQueueCompletion(completion), "the coded buffer of the change was given back early");
        video.setOutputFormat(VIRTIO_VIDEO_FORMAT_NV12, 32, 32);
        changeCompletion("setting the new size did not give back the coded buffer of the change");
        break;
    case Renegotiation::drain:
        require(!video.device.takeQueueCompletion(completion), "the coded buffer of the change was given back early");
        video.ok(virtio_video_stream_drain{{VIRTIO_VIDEO_CMD_STREAM_DRAIN, 9}});
        changeCompletion("a drain did not give back the coded buffer of the change");
        break;
    case Renegotiation::clearCoded:
        require(!video.device.takeQueueCompletion(completion), "the coded buffer of the change was given back early");
        video.ok(virtio_video_queue_clear{{VIRTIO_VIDEO_CMD_QUEUE_CLEAR, 9}, input, {}});
        changeCompletion("clearing the coded queue did not give back the coded buffer of the change");
        break;
    }
    require(!video.device.takeQueueCompletion(completion), "an extra completion after the renegotiation");
    if (how == Renegotiation::clearCapture || how == Renegotiation::setNewSize) {
        if (how == Renegotiation::setNewSize)
            video.ok(virtio_video_queue_clear{{VIRTIO_VIDEO_CMD_QUEUE_CLEAR, 9}, output, {}});
        require(load<virtio_video_cmd_hdr>(video.attach(output, 3, {{{small, 1024, {}}}, {{small + page, 512, {}}}}).response).type
                == VIRTIO_VIDEO_RESP_OK_NODATA, "new capture attach failed");
        for (unsigned picture = 3; picture <= 4; ++picture) {
            const auto request = captureRequestFor(3);
            require(video.device.takeQueueCompletion(completion) && completion.requestId == request, "missing capture completion");
            const auto result = load<virtio_video_resource_queue_resp>(completion.response);
            require(result.flags == 0 && result.timestamp == 100 + picture && result.data_sizes[0] == 1024 &&
                    result.data_sizes[1] == 512, "incorrect second-size picture");
            require(video.memory[small + 32 * 5 + 3] == scriptedLuma(picture, 5, 3), "second-size picture changed");
        }
    }
    video.ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 9}});
    video.device.reset();
}

/* A client that drains straight after queueing its last coded buffer, with a
 * size change inside what it queued, takes the first last buffer for the end
 * of its drain. So every picture of both sizes has to come before the marker
 * that ends the drain. When the client has a buffer queued for it, the change
 * ends the old size with a marker of its own before the drain is asked for,
 * as for any change. Once the drain is asked for, the change is announced with
 * no marker, whether or not a buffer is queued, and the drain's marker follows
 * the last picture of the new size. */
void changeInsideDrain(bool markerFirst)
{
    auto state = std::make_shared<ScriptedState>();
    // Pictures one and two are 64 x 64, three and four 32 x 32.
    VideoFixture video(12, 16 * page, [state](auto) {
        return std::make_unique<ScriptedDecoder>(std::vector<std::uint32_t> {64, 64, 32, 32}, state);
    });
    const auto input = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    const auto output = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    video.createStream();
    for (unsigned id = 0; id < 2; ++id)
        require(video.device.processQueue(1, 930 + id, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::defer,
                "event not deferred");
    video.setOutputFormat(VIRTIO_VIDEO_FORMAT_NV12);
    for (unsigned id = 1; id <= 4; ++id) video.attachEncodedInput(id);
    const auto attach = [&video](unsigned resource, std::uint64_t base, std::uint32_t luma, std::uint32_t chroma,
                                 std::uint64_t second) {
        require(load<virtio_video_cmd_hdr>(video.attach(output, resource, {{{base, luma, {}}}, {{second, chroma, {}}}}).response).type
                == VIRTIO_VIDEO_RESP_OK_NODATA, "capture attach failed");
    };
    attach(2, 3 * page, 4096, 2048, 4 * page);
    attach(3, 8 * page, 4096, 2048, 9 * page);
    attach(7, 13 * page, 4096, 2048, 14 * page);
    const auto captureRequestFor = [&video](std::uint32_t resource) {
        const auto request = video.next;
        require(video.command(video.queueCommand(output, resource)).action == VhostUserQueueAction::defer,
                "capture was not deferred");
        return request;
    };
    const auto flagsOf = [](const VhostUserQueueCompletion& completion) {
        return load<virtio_video_resource_queue_resp>(completion.response).flags;
    };
    VhostUserQueueCompletion completion;
    const auto firstOld = captureRequestFor(2), secondOld = captureRequestFor(3);
    for (unsigned picture = 1; picture <= 2; ++picture) {
        require(load<virtio_video_resource_queue_resp>(
                    video.command(video.queueCommand(input, picture, 5, 100 + picture)).response).flags == 0,
                "decode input failed");
        if (picture == 1)
            require(video.device.takeQueueCompletion(completion) && completion.requestId == 930, "the first size was not announced");
        require(video.device.takeQueueCompletion(completion) && completion.requestId == (picture == 1 ? firstOld : secondOld) &&
                flagsOf(completion) == 0 && load<virtio_video_resource_queue_resp>(completion.response).timestamp == 100 + picture,
                "incorrect first-size picture");
    }
    const auto spare = markerFirst ? captureRequestFor(7) : 0;
    const auto changing = video.next;
    require(video.command(video.queueCommand(input, 3, 5, 103)).action == VhostUserQueueAction::defer,
            "the coded buffer that changes the size was given back at once");
    if (markerFirst) {
        require(video.device.takeQueueCompletion(completion) && completion.requestId == spare &&
                flagsOf(completion) == VIRTIO_VIDEO_DEQUEUE_FLAG_EOS, "the old size did not end in a last buffer");
        require(video.device.takeQueueCompletion(completion) && completion.requestId == 931, "the new size was not announced");
    }
    require(load<virtio_video_resource_queue_resp>(
                video.command(video.queueCommand(input, 4, 5, 104)).response).flags == 0, "decode input failed");
    require(!video.device.takeQueueCompletion(completion), "a size change was announced ahead of the drain");

    video.ok(virtio_video_stream_drain{{VIRTIO_VIDEO_CMD_STREAM_DRAIN, 12}});
    bool released = false, announced = markerFirst;
    while (video.device.takeQueueCompletion(completion)) {
        if (completion.requestId == changing) released = true;
        else if (completion.requestId == 931 && !markerFirst) announced = true;
        else require(false, "a drain inside which the size changes completed a buffer before the pictures of the new size");
    }
    require(released, "the drain did not give back the coded buffer of the change");
    require(announced, "the new size was not announced without a buffer to end the old one");

    /* The client sets its capture queue up for the new size, which returns the
     * buffer of the old size it still holds; the pictures follow, then the
     * drain's marker. */
    std::uint64_t held = 0;
    if (!markerFirst) {
        held = captureRequestFor(7);
        require(!video.device.takeQueueCompletion(completion), "a buffer of the old size was used after the change");
    }
    video.ok(virtio_video_queue_clear{{VIRTIO_VIDEO_CMD_QUEUE_CLEAR, 12}, output, {}});
    if (!markerFirst)
        require(video.device.takeQueueCompletion(completion) && completion.requestId == held &&
                flagsOf(completion) == VIRTIO_VIDEO_DEQUEUE_FLAG_ERR, "the buffer of the old size was not returned");
    attach(4, 10 * page, 1024, 512, 10 * page + 1024);
    attach(5, 11 * page, 1024, 512, 11 * page + 1024);
    attach(6, 12 * page, 1024, 512, 12 * page + 1024);
    for (unsigned picture = 3; picture <= 4; ++picture) {
        const auto request = captureRequestFor(picture + 1);
        require(video.device.takeQueueCompletion(completion) && completion.requestId == request, "missing capture completion");
        const auto result = load<virtio_video_resource_queue_resp>(completion.response);
        require(result.flags == 0 && result.timestamp == 100 + picture && result.data_sizes[0] == 1024,
                "incorrect second-size picture");
    }
    const auto marker = captureRequestFor(6);
    require(video.device.takeQueueCompletion(completion) && completion.requestId == marker &&
            flagsOf(completion) == VIRTIO_VIDEO_DEQUEUE_FLAG_EOS, "the drain did not end in the last buffer");
    require(!video.device.takeQueueCompletion(completion), "an extra completion after the drain");
    video.ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 12}});
    video.device.reset();
}

/* A seek throws away what was decoded and nothing else: the decoder keeps the
 * stream's parameter sets, and the client is not told a size it already has. */
void seekKeepsTheFormat()
{
    auto state = std::make_shared<ScriptedState>();
    VideoFixture video(10, 8 * page, [state](auto) {
        return std::make_unique<ScriptedDecoder>(std::vector<std::uint32_t> {64}, state);
    });
    const auto input = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    const auto output = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    video.createStream();
    for (unsigned id = 0; id < 2; ++id)
        require(video.device.processQueue(1, 920 + id, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::defer,
                "event not deferred");
    video.setOutputFormat(VIRTIO_VIDEO_FORMAT_NV12);
    for (unsigned id = 1; id <= 2; ++id) video.attachEncodedInput(id);
    require(load<virtio_video_cmd_hdr>(video.attach(output, 2, {{{3 * page, 4096, {}}}, {{4 * page, 2048, {}}}}).response).type
            == VIRTIO_VIDEO_RESP_OK_NODATA, "capture attach failed");
    VhostUserQueueCompletion completion;
    const auto decodeOne = [&](std::uint32_t resource, std::uint64_t timestamp) {
        const auto capture = video.queueCommand(output, 2);
        const auto request = video.next;
        require(video.command(capture).action == VhostUserQueueAction::defer, "capture was not deferred");
        require(load<virtio_video_resource_queue_resp>(
                    video.command(video.queueCommand(input, resource, 5, timestamp)).response).flags == 0,
                "decode input failed");
        return request;
    };
    decodeOne(1, 201);
    require(video.device.takeQueueCompletion(completion) && completion.requestId == 920, "missing source event");
    require(video.device.takeQueueCompletion(completion), "missing capture completion");
    video.ok(virtio_video_queue_clear{{VIRTIO_VIDEO_CMD_QUEUE_CLEAR, 10}, input, {}});
    require(state->discards == 1, "a seek did not discard the decoder's state");
    require(!video.device.takeQueueCompletion(completion), "a seek completed something");
    const auto request = decodeOne(2, 202);
    require(video.device.takeQueueCompletion(completion) && completion.requestId == request,
            "the first picture after a seek was not delivered, or the format was announced again");
    const auto result = load<virtio_video_resource_queue_resp>(completion.response);
    require(result.flags == 0 && result.timestamp == 202, "incorrect picture after a seek");
    require(!video.device.takeQueueCompletion(completion), "an extra completion after a seek");
    video.ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 10}});
    video.device.reset();
}

/* A 10-bit stream is offered 8-bit planar capture, which Android asks for by
 * default, and still P010. A client that wants P010 from an 8-bit stream is
 * refused: the samples would be a lie. */
void tenBitStreamToNv12(unsigned format)
{
    VideoFixture video(11, 8 * page, [](auto) { return std::make_unique<Decoder>(true); },
        {{VideoCodec::h264, VideoProfile::h264High10, 4096, 2304, 51, 10}});
    const auto input = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    const auto output = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    video.createStream();
    require(video.device.processQueue(1, 930, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::defer,
            "event not deferred");
    video.setOutputFormat(format);
    video.attachEncodedInput();
    const bool nv12 = format == VIRTIO_VIDEO_FORMAT_NV12;
    Planes planes = nv12 ? Planes {{{3 * page, 4096, {}}}, {{4 * page, 2048, {}}}}
                         : Planes {{{3 * page, 12288, {}}}};
    require(load<virtio_video_cmd_hdr>(video.attach(output, 2, planes).response).type ==
            VIRTIO_VIDEO_RESP_OK_NODATA, "capture attach failed");
    const auto capture = video.queueCommand(output, 2);
    const auto request = video.next;
    require(video.command(capture).action == VhostUserQueueAction::defer, "capture was not deferred");
    require(load<virtio_video_resource_queue_resp>(
                video.command(video.queueCommand(input, 1, 5, 31)).response).flags == 0, "decode input failed");
    VhostUserQueueCompletion completion;
    require(video.device.takeQueueCompletion(completion), "missing source event");
    require(video.device.takeQueueCompletion(completion) && completion.requestId == request, "missing capture completion");
    const auto result = load<virtio_video_resource_queue_resp>(completion.response);
    require(result.flags == 0, "the picture was failed");
    if (nv12) {
        require(result.data_sizes[0] == 4096 && result.data_sizes[1] == 2048, "8-bit planes of a 10-bit stream");
        // The decoder's samples carry the pattern in both bytes; narrowing rounds.
        for (std::uint32_t row = 0; row < 64; row += 9)
            for (std::uint32_t column = 0; column < 64; column += 7) {
                const unsigned wide = patternLuma(row, column) * 257u;
                require(video.memory[3 * page + row * 64 + column] == std::min(255u, (wide + 0x80u) >> 8),
                        "a 10-bit sample was not narrowed to 8 bits");
            }
        require(video.memory[4 * page] == 0x91 && video.memory[4 * page + 1] == 0x91, "chroma was not narrowed");
    } else {
        require(result.data_sizes[0] == 12288, "10-bit samples of a 10-bit stream");
    }
    video.ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 11}});
    video.device.reset();

    // And the other way round.
    VideoFixture eight(12, 4 * page, [](auto) { return std::make_unique<Decoder>(false); });
    eight.createStream();
    virtio_video_set_params set {};
    set.hdr = {VIRTIO_VIDEO_CMD_SET_PARAMS, 12};
    set.params.queue_type = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    set.params.format = VIRTIO_VIDEO_FORMAT_P010;
    set.params.frame_width = set.params.frame_height = 64;
    require(load<virtio_video_cmd_hdr>(eight.command(set).response).type == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "P010 was offered for an 8-bit stream");
}

/* A format descriptor's `mask` names positions in the other queue's list, in
 * the order the same response carries them, and a guest that walks that list
 * offers its client exactly the formats the bits select. This checks both
 * directions against the lists actually sent, so a raw format added in the
 * middle of the list cannot silently stop being reachable. */
void capabilityMasks()
{
    std::vector<VideoDecodeCapability> caps {
        {VideoCodec::vp9, VideoProfile::vp9Profile0, 4096, 2304, 0, 8},
        {VideoCodec::hevc, VideoProfile::hevcMain, 8192, 4320, 0, 8},
        {VideoCodec::hevc, VideoProfile::hevcMain10, 8192, 4320, 0, 10},
    };
    VirtioVideoDevice device(caps, [](auto) { return std::make_unique<Decoder>(false); });
    std::uint64_t next = 1;
    auto formats = [&](std::uint32_t queue) {
        std::vector<std::uint8_t> request;
        append(request, virtio_video_query_capability{{VIRTIO_VIDEO_CMD_QUERY_CAPABILITY, 0}, queue, {}});
        const auto reply = device.processQueue(0, next++, request, 65536).response;
        std::vector<std::pair<std::uint32_t, std::uint64_t>> described;
        auto offset = sizeof(virtio_video_query_capability_resp);
        for (unsigned i = 0; i < load<virtio_video_query_capability_resp>(reply).num_descs; ++i) {
            const auto desc = load<virtio_video_format_desc>(std::span(reply).subspan(offset));
            described.emplace_back(desc.format, desc.mask);
            offset += sizeof(virtio_video_format_desc);
            for (unsigned frame = 0; frame < desc.num_frames; ++frame) {
                const auto range = load<virtio_video_format_frame>(std::span(reply).subspan(offset));
                offset += sizeof(virtio_video_format_frame) +
                    range.num_rates * sizeof(virtio_video_format_range);
            }
        }
        return described;
    };
    const auto coded = formats(VIRTIO_VIDEO_QUEUE_TYPE_INPUT);
    const auto raw = formats(VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT);
    require(coded.size() == 2 && raw.size() == 4, "capability lists changed shape");

    /* A client that enumerates the capture list and never negotiates receives
     * the first entry, so the head of that list and the format a fresh stream
     * reports have to be the same thing: the packed format every renderer can
     * draw. A client that picks by preference and a client that picks nothing
     * then agree, instead of one of them being handed planar data it did not
     * ask for. */
    require(raw.front().first == VIRTIO_VIDEO_FORMAT_BGRA8888,
            "the capture list does not offer the packed format first");
    virtio_video_stream_create create {};
    create.hdr = {VIRTIO_VIDEO_CMD_STREAM_CREATE, 11};
    create.coded_format = VIRTIO_VIDEO_FORMAT_VP9;
    {
        std::vector<std::uint8_t> request;
        append(request, create);
        require(load<virtio_video_cmd_hdr>(
                    device.processQueue(0, next++, request, 16384).response).type
                == VIRTIO_VIDEO_RESP_OK_NODATA, "stream create failed");
    }
    virtio_video_get_params get {};
    get.hdr = {VIRTIO_VIDEO_CMD_GET_PARAMS, 11};
    get.queue_type = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    {
        std::vector<std::uint8_t> request;
        append(request, get);
        const auto params = load<virtio_video_get_params_resp>(
            device.processQueue(0, next++, request, 16384).response).params;
        require(params.format == raw.front().first,
                "a fresh stream's capture format is not the first one advertised");
        require(params.num_planes == 1 && params.plane_formats[0].stride == 128 * 4,
                "the default capture format is not a single packed plane");
    }

    auto selected = [](const auto& list, std::uint64_t mask) {
        std::vector<std::uint32_t> chosen;
        for (std::size_t index = 0; index < list.size(); ++index)
            if (mask & (std::uint64_t(1) << index)) chosen.push_back(list[index].first);
        std::sort(chosen.begin(), chosen.end());
        return chosen;
    };
    auto sorted = [](std::vector<std::uint32_t> values) {
        std::sort(values.begin(), values.end());
        return values;
    };

    for (const auto& [format, mask] : coded) {
        // VP9 is 8-bit here, HEVC has both an 8-bit and a 10-bit capability.
        const auto expected = format == VIRTIO_VIDEO_FORMAT_VP9
            ? sorted({VIRTIO_VIDEO_FORMAT_NV12, VIRTIO_VIDEO_FORMAT_YUV420,
                      VIRTIO_VIDEO_FORMAT_BGRA8888})
            : sorted({VIRTIO_VIDEO_FORMAT_NV12, VIRTIO_VIDEO_FORMAT_YUV420,
                      VIRTIO_VIDEO_FORMAT_BGRA8888, VIRTIO_VIDEO_FORMAT_P010});
        require(selected(raw, mask) == expected, "a coded format names the wrong capture formats");
    }
    for (const auto& [format, mask] : raw) {
        const auto expected = format == VIRTIO_VIDEO_FORMAT_P010
            ? sorted({VIRTIO_VIDEO_FORMAT_HEVC})
            : sorted({VIRTIO_VIDEO_FORMAT_VP9, VIRTIO_VIDEO_FORMAT_HEVC});
        require(selected(coded, mask) == expected, "a capture format names the wrong codecs");
    }
    device.reset();

    /* A codec with only a 10-bit capability is reached by the 8-bit planar
     * formats too: its pictures are narrowed. */
    VirtioVideoDevice tenBitOnly({{VideoCodec::h264, VideoProfile::h264High10, 4096, 2304, 51, 10}},
                                 [](auto) { return std::make_unique<Decoder>(true); });
    std::vector<std::uint8_t> request;
    append(request, virtio_video_query_capability{{VIRTIO_VIDEO_CMD_QUERY_CAPABILITY, 0},
                                                  VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT, {}});
    const auto reply = tenBitOnly.processQueue(0, 1, request, 65536).response;
    unsigned seen = 0;
    auto offset = sizeof(virtio_video_query_capability_resp);
    for (unsigned i = 0; i < load<virtio_video_query_capability_resp>(reply).num_descs; ++i) {
        const auto desc = load<virtio_video_format_desc>(std::span(reply).subspan(offset));
        require(desc.mask == 1, "a raw format of a 10-bit only codec does not name it");
        ++seen;
        offset += sizeof(virtio_video_format_desc);
        const auto range = load<virtio_video_format_frame>(std::span(reply).subspan(offset));
        require(range.width.max == 4096, "a raw format of a 10-bit only codec has no size");
        offset += sizeof(virtio_video_format_frame) + range.num_rates * sizeof(virtio_video_format_range);
    }
    require(seen == 4, "all four raw formats are offered for a 10-bit codec");
}

/* Fixes a picture's sample depth when it decodes it, as AppleVideoDecoder
 * does: asked for 8-bit planes, it narrows 10-bit samples as it copies them.
 * The first packet carries only parameter sets, so it declares the size and
 * decodes nothing. */
class NarrowingDecoder final : public VideoDecoder {
public:
    void inspect(std::span<const std::uint8_t>) override { format_ = {128, 128, 10}; }
    void submit(std::span<const std::uint8_t>, std::uint64_t timestamp) override
    {
        format_ = {128, 128, 10};
        if (!packets_++) return;
        const bool narrow = layout_ == VideoPixelLayout::planar8Bit;
        DecodedVideoFrame frame;
        frame.format = {128, 128, narrow ? 8u : 10u};
        frame.timestamp = timestamp;
        frame.color = {VideoColorMatrix::bt601, false};
        frame.luma.assign(128 * 128 * (narrow ? 1 : 2), 0x40);
        frame.chroma.assign(128 * 64 * (narrow ? 1 : 2), 0x80);
        frames_.push_back(std::move(frame));
    }
    void drain() override {}
    void discard() override { frames_.clear(); }
    std::vector<DecodedVideoFrame> takeFrames() override { return std::exchange(frames_, {}); }
    VideoFormat format() const noexcept override { return format_; }
    void requestOutputLayout(VideoPixelLayout layout) override { layout_ = layout; }
private:
    unsigned packets_ {};
    VideoPixelLayout layout_ {VideoPixelLayout::planarYuv};
    VideoFormat format_;
    std::vector<DecodedVideoFrame> frames_;
};

/* v4l2_codec2 names an 8-bit capture format before the picture size is known
 * and makes its real choice, P010 for a 10-bit stream, after the source
 * change; the guest may queue the next packet on either side of that choice.
 * A picture decoded in between keeps its depth and reaches the P010 client.
 * Narrowed, it would end the stream in an empty LAST buffer, which a draining
 * client takes for the end of its drain. */
void provisionalCaptureFormat(bool pictureFirst)
{
    VideoFixture video(21, 32 * page, [](auto) { return std::make_unique<NarrowingDecoder>(); },
        {{VideoCodec::h264, VideoProfile::h264High10, 4096, 2304, 51, 10}});
    const auto input = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    const auto output = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    video.createStream();
    for (unsigned id = 0; id < 2; ++id)
        require(video.device.processQueue(1, 940 + id, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::defer,
                "event not deferred");
    video.setOutputFormat(VIRTIO_VIDEO_FORMAT_NV12, 64, 64);
    video.attachEncodedInput(1);
    video.attachEncodedInput(2);
    /* The packet that declares the size comes back only once the client has
     * set its capture queue up for that size: v4l2_codec2 drains only when
     * every packet is back, and a drain it starts before the source change
     * ends at once without the picture. */
    const auto declaring = video.next;
    require(video.command(video.queueCommand(input, 1, 5, 0)).action == VhostUserQueueAction::defer,
            "the packet that declared the size came back before the capture queue was set up for it");
    VhostUserQueueCompletion completion;
    require(video.device.takeQueueCompletion(completion) && completion.requestId == 940 &&
            load<virtio_video_event>(completion.response).event_type == VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED,
            "size not announced");
    const auto picture = [&] {
        require(load<virtio_video_resource_queue_resp>(video.command(video.queueCommand(input, 2, 5, 77)).response).flags == 0,
                "picture packet failed");
    };
    if (pictureFirst) picture();
    video.ok(virtio_video_queue_clear{{VIRTIO_VIDEO_CMD_QUEUE_CLEAR, 21}, output, {}});
    require(video.device.takeQueueCompletion(completion) && completion.requestId == declaring &&
            load<virtio_video_resource_queue_resp>(completion.response).flags == 0,
            "stopping the capture queue did not give back the packet that declared the size");
    video.setOutputFormat(VIRTIO_VIDEO_FORMAT_P010, 128, 128);
    require(load<virtio_video_cmd_hdr>(video.attach(output, 3, {{{4 * page, 2 * 128 * 128 + 4 * 64 * 64, {}}}}).response).type
            == VIRTIO_VIDEO_RESP_OK_NODATA, "capture attach failed");
    if (!pictureFirst) picture();
    const auto captureRequest = video.next;
    require(video.command(video.queueCommand(output, 3)).action == VhostUserQueueAction::defer, "capture was not deferred");
    video.ok(virtio_video_stream_drain{{VIRTIO_VIDEO_CMD_STREAM_DRAIN, 21}});
    require(video.device.takeQueueCompletion(completion) && completion.requestId == captureRequest,
            "missing capture completion");
    const auto result = load<virtio_video_resource_queue_resp>(completion.response);
    require(result.flags == 0 && result.timestamp == 77, "the picture did not reach the P010 client");
    require(!video.device.takeQueueCompletion(completion), "the size was announced again");
    video.ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 21}});
    video.device.reset();
}

/* v4l2_codec2 also queues buffers for its provisional format and starts the
 * capture queue before the size is known. A picture decoded after the size is
 * announced, while those buffers are still queued, waits for the buffers the
 * client sets up for the real size, even when the client drains before it
 * handles the source change. */
void provisionalCaptureBuffers(bool drainFirst)
{
    VideoFixture video(21, 32 * page, [](auto) { return std::make_unique<NarrowingDecoder>(); },
        {{VideoCodec::h264, VideoProfile::h264High10, 4096, 2304, 51, 10}});
    const auto input = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
    const auto output = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
    video.createStream();
    require(video.device.processQueue(1, 940, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::defer,
            "event not deferred");
    video.setOutputFormat(VIRTIO_VIDEO_FORMAT_NV12, 64, 64);
    video.attachEncodedInput(1);
    video.attachEncodedInput(2);
    for (unsigned i = 0; i < 2; ++i) {
        const std::uint64_t base = (20 + 2 * i) * page;
        require(load<virtio_video_cmd_hdr>(video.attach(output, 5 + i, {{{base, 64 * 64, {}}}, {{base + 64 * 64, 64 * 32, {}}}}).response).type
                == VIRTIO_VIDEO_RESP_OK_NODATA, "provisional capture attach failed");
        require(video.command(video.queueCommand(output, 5 + i)).action == VhostUserQueueAction::defer,
                "provisional capture was not deferred");
    }
    const auto declaring = video.next;
    require(video.command(video.queueCommand(input, 1, 5, 0)).action == VhostUserQueueAction::defer,
            "the packet that declared the size came back before the capture queue was set up for it");
    require(load<virtio_video_resource_queue_resp>(video.command(video.queueCommand(input, 2, 5, 77)).response).flags == 0,
            "picture packet failed");
    if (drainFirst) video.ok(virtio_video_stream_drain{{VIRTIO_VIDEO_CMD_STREAM_DRAIN, 21}});
    VhostUserQueueCompletion completion;
    bool announced = false, declared = false;
    const auto noPictureYet = [&] {
        while (video.device.takeQueueCompletion(completion)) {
            if (completion.requestId == 940) { announced = true; continue; }
            if (completion.requestId == declaring) { declared = true; continue; }
            require(load<virtio_video_resource_queue_resp>(completion.response).timestamp != 77,
                    "the picture went into a buffer set up for the provisional size");
        }
    };
    noPictureYet();
    require(announced, "size not announced");
    /* A drain gives the packet back, since a client that drains is not waiting
     * for it. */
    require(declared == drainFirst, "the packet that declared the size came back before the client stopped its capture queue");
    video.ok(virtio_video_queue_clear{{VIRTIO_VIDEO_CMD_QUEUE_CLEAR, 21}, output, {}});
    noPictureYet();
    require(declared, "stopping the capture queue did not give back the packet that declared the size");
    video.setOutputFormat(VIRTIO_VIDEO_FORMAT_P010, 128, 128);
    require(load<virtio_video_cmd_hdr>(video.attach(output, 3, {{{4 * page, 2 * 128 * 128 + 4 * 64 * 64, {}}}}).response).type
            == VIRTIO_VIDEO_RESP_OK_NODATA, "capture attach failed");
    const auto captureRequest = video.next;
    require(video.command(video.queueCommand(output, 3)).action == VhostUserQueueAction::defer, "capture was not deferred");
    if (!drainFirst) video.ok(virtio_video_stream_drain{{VIRTIO_VIDEO_CMD_STREAM_DRAIN, 21}});
    require(video.device.takeQueueCompletion(completion) && completion.requestId == captureRequest,
            "missing capture completion");
    const auto result = load<virtio_video_resource_queue_resp>(completion.response);
    require(result.flags == 0 && result.timestamp == 77, "the picture did not reach the buffers set up for its size");
    video.ok(virtio_video_stream_destroy{{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 21}});
    video.device.reset();
}
}
int main()
{
    try {
        h264ProfileControls();
        codecLevelControls();
        decoderRejectsEncoderCommands();
        capabilityMasks();
        sharedMemoryCapture();
        for (const bool decoderPacks : {true, false})
            packedCapture(decoderPacks);
        // The geometry a browser actually decodes, where a stride error shows.
        for (const bool decoderPacks : {true, false})
            packedCapture(decoderPacks, 1920, 1200);
        // Widths that are not a multiple of the allocator's row alignment.
        for (const bool decoderPacks : {true, false}) {
            packedCapture(decoderPacks, 341, 30);
            packedCapture(decoderPacks, 99, 77);
        }
        rawPitchRule();
        for (const auto format : {VIRTIO_VIDEO_FORMAT_NV12, VIRTIO_VIDEO_FORMAT_YUV420, VIRTIO_VIDEO_FORMAT_P010})
            for (const auto& [width, height] : {std::pair{340u, 180u}, std::pair{520u, 38u}, std::pair{99u, 77u},
                                                std::pair{17u, 5u}, std::pair{1000u, 12u}})
                unalignedPlanar(format, width, height);
        lateNegotiation();
        inputBackpressure();
        unsettledHoldIsBounded();
        for (const unsigned reorder : {0u, 2u, 3u, 4u, 9u, 16u}) captureMinimumFollowsReorder(reorder);
        codedBufferFollowsPicture();
        planarAfterSourceChange();
        packedDefaultThroughStreamon();
        for (const auto how : {Renegotiation::clearCapture, Renegotiation::setNewSize,
                               Renegotiation::drain, Renegotiation::clearCoded})
            resolutionChange(how);
        for (const bool markerFirst : {false, true}) changeInsideDrain(markerFirst);
        seekKeepsTheFormat();
        for (const auto format : {VIRTIO_VIDEO_FORMAT_NV12, VIRTIO_VIDEO_FORMAT_P010})
            tenBitStreamToNv12(format);
        provisionalCaptureFormat(false);
        provisionalCaptureFormat(true);
        provisionalCaptureBuffers(false);
        provisionalCaptureBuffers(true);
        for (const auto format : {VIRTIO_VIDEO_FORMAT_NV12, VIRTIO_VIDEO_FORMAT_YUV420, VIRTIO_VIDEO_FORMAT_P010})
            exercise(format);
        activityLog();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what()); return 1;
    }
}
