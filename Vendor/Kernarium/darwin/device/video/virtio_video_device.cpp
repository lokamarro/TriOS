// SPDX-License-Identifier: GPL-2.0-or-later
#include "virtio_video_device.hpp"
#include "activity_log.hpp"
#include "guest_buffers.hpp"
#include "video_capabilities.hpp"
#include "vhost_user_protocol.hpp"
#include "raw_pitch.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace uml::video {
namespace {
using uml::virtio::VhostUserQueueAction;
using uml::virtio::VhostUserQueueCompletion;
using uml::virtio::VhostUserQueueResult;
constexpr std::size_t maximumPacket = 8 * 1024 * 1024;

using uml::virtio::toBytes;
template<class T> T load(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < sizeof(T)) throw std::invalid_argument("short video command");
    return uml::virtio::fromBytes<T>(bytes);
}
/* A frame that was decoded before the client negotiated a packed capture
 * format still has to be delivered in it, and a decoder that will not emit
 * packed frames at all must still answer the format it advertised. The host
 * converts such a frame itself; a frame the decoder produced packed is copied
 * unchanged. The matrix and range come from the frame, which reports the
 * description the hardware applied to it, so a converted frame and a packed
 * one are the same picture and a stream does not change appearance when the
 * client negotiates part way through. An empty result rejects the frame
 * rather than delivering wrong pixels. */
std::vector<std::uint8_t> packBgra(const uml::platform::DecodedVideoFrame& frame)
{
    const auto width = std::size_t(frame.format.width);
    const auto height = std::size_t(frame.format.height);
    const std::size_t step = frame.format.bitDepth == 10 ? 2 : 1;
    const auto chromaWidth = (width + 1) / 2;
    if (!width || !height || frame.luma.size() < width * height * step ||
        frame.chroma.size() < chromaWidth * 2 * ((height + 1) / 2) * step)
        return {};
    // ITU-R luma coefficients for the matrix the frame states.
    double kr = 0.2126, kb = 0.0722;
    if (frame.color.matrix == uml::platform::VideoColorMatrix::bt601) { kr = 0.299; kb = 0.114; }
    else if (frame.color.matrix == uml::platform::VideoColorMatrix::bt2020) { kr = 0.2627; kb = 0.0593; }
    const double kg = 1.0 - kr - kb;
    // Studio swing scales 16..235 and 16..240 onto the full output range.
    const double lumaGain = frame.color.fullRange ? 1.0 : 255.0 / 219.0;
    const double lumaFloor = frame.color.fullRange ? 0.0 : 16.0;
    const double chromaGain = frame.color.fullRange ? 1.0 : 255.0 / 224.0;
    const double vToR = chromaGain * 2.0 * (1.0 - kr);
    const double uToB = chromaGain * 2.0 * (1.0 - kb);
    const double uToG = chromaGain * 2.0 * kb * (1.0 - kb) / kg;
    const double vToG = chromaGain * 2.0 * kr * (1.0 - kr) / kg;
    // A 10-bit sample keeps its data in the high bits of a little-endian pair.
    const auto sample = [step](const std::vector<std::uint8_t>& plane, std::size_t index) {
        return int(plane[step * index + step - 1]);
    };
    const auto byte = [](double value) {
        return std::uint8_t(std::clamp(value, 0.0, 255.0) + 0.5);
    };
    std::vector<std::uint8_t> pixels(width * height * 4);
    for (std::size_t row = 0; row < height; ++row) {
        const auto pairs = (row / 2) * chromaWidth * 2;
        for (std::size_t column = 0; column < width; ++column) {
            const double luma = lumaGain * (sample(frame.luma, row * width + column) - lumaFloor);
            const double u = sample(frame.chroma, pairs + (column / 2) * 2) - 128;
            const double v = sample(frame.chroma, pairs + (column / 2) * 2 + 1) - 128;
            auto* out = pixels.data() + (row * width + column) * 4;
            out[0] = byte(luma + uToB * u);
            out[1] = byte(luma - uToG * u - vToG * v);
            out[2] = byte(luma + vToR * v);
            out[3] = 0xff;
        }
    }
    return pixels;
}

/* A planar frame of 10-bit samples, which sit in the high bits of a
 * little-endian pair, as the 8-bit NV12 a client that negotiated it expects.
 * The decoder narrows the pictures it decodes once it has been asked to, so
 * only a picture decoded before the client chose reaches this. The rounding is
 * the decoder's. */
void narrowTo8(uml::platform::DecodedVideoFrame& frame)
{
    for (auto* plane : {&frame.luma, &frame.chroma}) {
        std::vector<std::uint8_t> narrow(plane->size() / 2);
        for (std::size_t i = 0; i < narrow.size(); ++i) {
            const unsigned value = unsigned((*plane)[2 * i]) | (unsigned((*plane)[2 * i + 1]) << 8);
            narrow[i] = std::uint8_t(std::min(255u, (value + 0x80u) >> 8));
        }
        *plane = std::move(narrow);
    }
    frame.format.bitDepth = 8;
}

std::vector<std::uint8_t> reply(std::uint32_t stream, std::uint32_t type = VIRTIO_VIDEO_RESP_OK_NODATA)
{
    return toBytes(virtio_video_cmd_hdr{type, stream});
}
/* The capture format a stream has before its client negotiates one. It is the
 * head of the raw list `capabilityResponse()` publishes: packed BGRA for an
 * 8-bit stream, which the host produces in hardware and a GL client can draw
 * without converting, and P010 for a 10-bit one, which packed BGRA cannot
 * carry. */
std::uint32_t defaultCapture(unsigned bitDepth) noexcept
{
    return bitDepth == 10 ? VIRTIO_VIDEO_FORMAT_P010 : VIRTIO_VIDEO_FORMAT_BGRA8888;
}
/* What the decoder is asked to produce for a capture format: packed pixels, the
 * stream's own depth for P010, and 8-bit planes for NV12 and YUV420, which a
 * 10-bit stream reaches by being narrowed. */
uml::platform::VideoPixelLayout captureLayout(std::uint32_t format) noexcept
{
    if (format == VIRTIO_VIDEO_FORMAT_BGRA8888) return uml::platform::VideoPixelLayout::packedBgra;
    return format == VIRTIO_VIDEO_FORMAT_P010 ? uml::platform::VideoPixelLayout::planarYuv
                                              : uml::platform::VideoPixelLayout::planar8Bit;
}
/* What the decoder is asked to produce for a capture format named while the
 * picture size is `known` or not. A format named before the size is announced
 * is provisional: a stateful client sets its queue up with one and chooses
 * again after the source change, and a picture may be decoded in between. The
 * stream's own depth is the one layout that picture can still be delivered
 * from whatever the client chooses, so until the size is known 8-bit planes
 * are reached by narrowing on delivery, which a choice made afterwards moves
 * into the decoder. */
uml::platform::VideoPixelLayout decodeLayout(std::uint32_t format, bool known) noexcept
{
    const auto layout = captureLayout(format);
    return layout == uml::platform::VideoPixelLayout::planar8Bit && !known
               ? uml::platform::VideoPixelLayout::planarYuv : layout;
}
bool validQueue(std::uint32_t queue)
{
    return queue == VIRTIO_VIDEO_QUEUE_TYPE_INPUT || queue == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
}
std::uint64_t resourceKey(std::uint32_t queue, std::uint32_t id)
{
    return (std::uint64_t(queue) << 32) | id;
}
const char* codecName(uml::platform::VideoCodec codec) noexcept
{
    switch (codec) {
    case uml::platform::VideoCodec::h264: return "h264";
    case uml::platform::VideoCodec::hevc: return "hevc";
    case uml::platform::VideoCodec::jpeg: return "jpeg";
    case uml::platform::VideoCodec::vp9: return "vp9";
    }
    return "unknown";
}
const char* profileName(uml::platform::VideoProfile profile) noexcept
{
    switch (profile) {
    case uml::platform::VideoProfile::h264Baseline: return "h264-baseline";
    case uml::platform::VideoProfile::h264ConstrainedBaseline: return "h264-constrained-baseline";
    case uml::platform::VideoProfile::h264Main: return "h264-main";
    case uml::platform::VideoProfile::h264High: return "h264-high";
    case uml::platform::VideoProfile::h264ConstrainedHigh: return "h264-constrained-high";
    case uml::platform::VideoProfile::h264High10: return "h264-high10";
    case uml::platform::VideoProfile::hevcMain: return "hevc-main";
    case uml::platform::VideoProfile::hevcMain10: return "hevc-main10";
    case uml::platform::VideoProfile::jpegBaseline: return "jpeg-baseline";
    case uml::platform::VideoProfile::vp9Profile0: return "vp9-profile0";
    }
    return "unknown";
}

constexpr std::uint32_t captureMaximumBuffers = 32;
/* How few capture buffers a client may run a stream with. A client that keeps
 * every buffer it has queued until the picture is displayed must hold one more
 * than the pictures the stream reorders, because the decoder releases a picture
 * only when that many have followed it, so the capture queue reports the
 * stream's own reorder depth plus one, and four at the least. This is what a
 * client reads as V4L2_CID_MIN_BUFFERS_FOR_CAPTURE once the picture size is
 * announced, and it can size its pipeline by it instead of by the worst case
 * any stream could have (16 pictures). */
std::uint32_t captureMinimumBuffers(unsigned reorderDepth) noexcept
{
    return std::min(captureMaximumBuffers, std::max(4u, reorderDepth + 1));
}

/* The coded buffer a client must give the decoder for a stream of this picture
 * size, which is the size its input queue states once the client has stated one.
 * A compressed picture is far smaller than its raw samples, and a client that
 * keeps several coded buffers queued holds them for the stream's lifetime, so a
 * buffer of the largest packet the device takes (8 MiB) is memory no 1080p
 * picture uses: 1 MiB to 720p, 2 MiB to 1080p, 4 MiB to 4096 x 2304 and the
 * full packet above that, which is the order a ChromeOS stateful decoder asks
 * for. A JPEG picture compresses far less than a video picture and keeps
 * the full packet. A packet the guest buffer cannot hold is the client's to
 * refuse; the device takes any packet up to `maximumPacket` that fits. */
std::uint32_t codedBufferSize(std::uint32_t format, std::uint32_t width, std::uint32_t height) noexcept
{
    if (format == VIRTIO_VIDEO_FORMAT_MJPEG) return maximumPacket;
    const std::uint64_t area = std::uint64_t(width) * height;
    if (area <= 1280u * 720u) return 1u << 20;
    if (area <= 1920u * 1088u) return 2u << 20;
    if (area <= 4096u * 2304u) return 4u << 20;
    return maximumPacket;
}

virtio_video_params params(std::uint32_t queue, std::uint32_t format, std::uint32_t width, std::uint32_t height,
                           unsigned reorderDepth = 0)
{
    virtio_video_params p {};
    p.queue_type = queue;
    p.format = format;
    p.frame_width = width;
    p.frame_height = height;
    p.min_buffers = queue == VIRTIO_VIDEO_QUEUE_TYPE_INPUT ? 1 : captureMinimumBuffers(reorderDepth);
    p.max_buffers = captureMaximumBuffers;
    p.crop.width = width;
    p.crop.height = height;
    p.frame_rate = 30;
    if (queue == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
        p.num_planes = 1;
        p.plane_formats[0].plane_size = maximumPacket;
    } else {
        const auto ch = (height + 1) / 2;
        /* Rows lie at the pitch the guest allocator gives a buffer (raw_pitch.hpp),
         * and the planes keep their tight heights. */
        if (format == VIRTIO_VIDEO_FORMAT_BGRA8888) {
            // One packed plane of 8-bit B, G, R, A samples, one row per line.
            const auto pitch = rawPitch(4 * width);
            p.num_planes = 1;
            p.plane_formats[0] = {pitch * height, pitch};
        } else if (format == VIRTIO_VIDEO_FORMAT_P010) {
            // The interleaved chroma rows follow the luma rows in the one plane.
            const auto pitch = rawPitch(2 * width);
            p.num_planes = 1;
            p.plane_formats[0] = {pitch * (height + ch), pitch};
        } else if (format == VIRTIO_VIDEO_FORMAT_YUV420) {
            const auto lumaPitch = rawPitch(width, rawTriplanarLumaAlignment);
            const auto chromaPitch = lumaPitch / 2;
            p.num_planes = 3;
            p.plane_formats[0] = {lumaPitch * height, lumaPitch};
            p.plane_formats[1] = {chromaPitch * ch, chromaPitch};
            p.plane_formats[2] = {chromaPitch * ch, chromaPitch};
        } else {
            // The interleaved chroma rows are as long as the luma rows.
            const auto pitch = rawPitch(width);
            p.num_planes = 2;
            p.plane_formats[0] = {pitch * height, pitch};
            p.plane_formats[1] = {pitch * ch, pitch};
        }
    }
    return p;
}
/* The coded queue's parameters for a picture size the client has stated: the
 * coded buffer follows the size. A stream that has stated none keeps the full
 * packet, which fits whatever the stream turns out to carry. */
virtio_video_params inputParams(std::uint32_t format, std::uint32_t width, std::uint32_t height)
{
    auto p = params(VIRTIO_VIDEO_QUEUE_TYPE_INPUT, format, width, height);
    p.plane_formats[0].plane_size = codedBufferSize(format, width, height);
    return p;
}
}

class VirtioVideoDevice::Implementation {
public:
    using Segment = GuestSegment;
    struct Resource {
        std::vector<std::vector<Segment>> planes;
        bool queued {};
        bool filled {}; // a frame has been written into it at least once
    };
    struct Output { std::uint64_t request; std::uint32_t resource; };
    /* An encoded packet the device has taken from the guest but not given to
     * the decoder, because the frames already decoded have nowhere to go. */
    struct Held {
        std::uint64_t request;
        std::uint32_t resource;
        std::uint64_t timestamp;
        std::uint32_t size;
        std::vector<std::uint8_t> packet;
    };
    /* A coded buffer whose packet changed the picture size. The decoder has
     * consumed it, but it is not given back until the client has set its
     * capture queue up for the new size, which is when a hardware decoder
     * returns it too. A client that queues its last coded buffer and then
     * drains therefore cannot drain across a size change it has not handled:
     * its drain waits for every coded buffer, and the end marker of the old
     * size is not mistaken for the end of the drain. */
    struct SizeChange {
        std::uint64_t request;
        std::uint32_t resource;
        std::vector<std::uint8_t> response;
    };
    struct Stream {
        std::uint32_t coded {};
        std::uint32_t profile {};
        virtio_video_params input {};
        virtio_video_params output {};
        std::deque<Held> held;
        std::uint64_t heldBytes {};
        /* Whether the client has said what it wants from the capture queue,
         * by naming a format or by starting the queue with the one it was
         * given. Until it has, the decoder is left planar, because a picture
         * decoded packed cannot answer a planar format. */
        bool captureSettled {};
        /* Whether encoded packets are waiting for that answer. A stateful
         * client cannot name a capture format before the device reports the
         * picture size, so the device reads the size out of the packets'
         * headers and decodes none of them until the client has chosen. The
         * first picture it then decodes already carries the chosen layout,
         * which is what keeps the host from converting every frame. */
        bool captureDeferred {true};
        /* The picture size the client was told, which pictures are compared
         * with: a picture of another size is a resolution change, whatever
         * capture parameters the client has set since. */
        bool sourceKnown {};
        std::uint32_t announcedWidth {};
        std::uint32_t announcedHeight {};
        /* A resolution change has been announced and the client has not yet
         * set its capture queue up again: no picture of the new size is
         * delivered into the buffers it still holds. */
        bool renegotiating {};
        /* The size the latest packet given to the decoder declared, which the
         * next packet is compared with to find the one that changes it. */
        std::uint32_t submittedWidth {};
        std::uint32_t submittedHeight {};
        std::deque<SizeChange> sizeChanges;
        bool draining {};
        bool eosSent {};
        std::uint64_t packets {};
        std::uint64_t packetBytes {};
        std::uint64_t delivered {};
        std::uint64_t converted {};
        std::uint64_t captureQueued {};
        std::uint64_t captureRequeued {};
        std::unordered_set<std::uint32_t> captureBuffers;
        bool captureShared {};
        bool captureGuestPages {};
        std::unique_ptr<uml::platform::VideoDecoder> decoder;
        std::unordered_map<std::uint64_t, Resource> resources;
        std::deque<Output> outputs;
        std::deque<uml::platform::DecodedVideoFrame> frames;
    };

    ActivityLog log;
    std::vector<uml::platform::VideoDecodeCapability> capabilities;
    DecoderFactory factory;
    GuestMapper mapper;
    std::shared_ptr<const uml::virtio::SharedMemoryRegions> regions;
    std::unordered_map<std::uint32_t, Stream> streams;
    std::deque<std::uint64_t> eventBuffers;
    std::deque<virtio_video_event> events;
    std::deque<VhostUserQueueCompletion> completions;

    ~Implementation() { closeAll("shutdown"); }

    static const char* rawName(std::uint32_t format) noexcept
    {
        switch (format) {
        case VIRTIO_VIDEO_FORMAT_NV12: return "nv12";
        case VIRTIO_VIDEO_FORMAT_YUV420: return "yuv420";
        case VIRTIO_VIDEO_FORMAT_BGRA8888: return "bgra8888";
        case VIRTIO_VIDEO_FORMAT_P010: return "p010";
        }
        return "unknown";
    }

    void record(std::uint32_t id, const Stream& stream, const char* reason) noexcept
    {
        const char* capture = stream.captureShared
            ? (stream.captureGuestPages ? "mixed" : "shm-region")
            : (stream.captureGuestPages ? "guest-pages" : "none");
        /* A guest driver creates its stream before the client has chosen a
         * codec and changes it with SET_PARAMS, so the codec on the open line
         * is not always the one that decoded. */
        const auto cap = std::find_if(capabilities.begin(), capabilities.end(),
            [&stream](const auto& c) { return wireCodec(c.codec) == stream.coded; });
        log.closed(id, reason, stream.packets, stream.packetBytes, stream.delivered,
                   stream.output.frame_width, stream.output.frame_height, capture,
                   stream.captureQueued, stream.captureBuffers.size(),
                   stream.captureRequeued, stream.outputs.size(),
                   rawName(stream.output.format), stream.converted,
                   cap == capabilities.end() ? "unknown" : codecName(cap->codec));
    }

    void closeAll(const char* reason) noexcept
    {
        for (const auto& [id, stream] : streams) record(id, stream, reason);
    }

    void event(std::uint32_t stream, std::uint32_t type)
    {
        if (events.size() >= 64) throw std::runtime_error("video event queue is full");
        events.push_back({type, stream});
        while (!events.empty() && !eventBuffers.empty()) {
            completions.push_back({eventBuffers.front(), toBytes(events.front())});
            eventBuffers.pop_front();
            events.pop_front();
        }
    }

    std::vector<std::uint8_t> queued(std::uint32_t stream, std::uint64_t timestamp,
                                    std::uint32_t flags, const virtio_video_params* p = nullptr)
    {
        virtio_video_resource_queue_resp r {};
        r.hdr = {VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE, stream};
        r.timestamp = timestamp;
        r.flags = flags;
        if (p) for (unsigned i = 0; i < p->num_planes; ++i) r.data_sizes[i] = p->plane_formats[i].plane_size;
        return toBytes(r);
    }

    void cancelOutputs(std::uint32_t id, Stream& stream)
    {
        for (const auto& output : stream.outputs) {
            completions.push_back({output.request, queued(id, 0, VIRTIO_VIDEO_DEQUEUE_FLAG_ERR)});
            const auto found = stream.resources.find(resourceKey(VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT, output.resource));
            if (found != stream.resources.end()) found->second.queued = false;
        }
        stream.outputs.clear();
    }

    bool transfer(const std::vector<Segment>& segments, std::span<std::uint8_t> data, bool write)
    {
        return transferSegments(mapper, regions.get(), segments, data, write);
    }

    /* How many decoded frames the device keeps before it stops taking encoded
     * packets. A decoder in hardware cannot decode a picture without a free
     * capture buffer, and its client feels that as its input buffers not
     * coming back; this device decodes whenever it is asked, so it has to
     * withhold the input buffer itself. Without that the frames pile up
     * against a client that consumes them at its display rate, and the pile
     * used to end in an error the client reports as EIO on both queues.
     *
     * Two are enough, and more are host memory with no use: each is a copy of
     * a decoded picture (11.9 MiB at 3840x2160) that waits for a capture
     * buffer, and a decoder needs one frame being copied into a buffer while
     * the next one waits. The bound holds back input, never a frame: the
     * packets withheld behind it wait in a queue (heldPacketLimit) deeper than
     * any a client sets up, and a burst the decoder releases at once, at a
     * sequence boundary or a drain, is delivered whole. The pictures the
     * stream's reorder depth keeps in the decoder are not counted here. */
    static constexpr std::size_t framesInFlight = 2;
    /* What a stream may hold while it waits for the client to choose a
     * capture format. This is not backpressure and no client needs it to be
     * generous: a stateful client chooses as soon as the source change tells
     * it the size, which is within the first few packets. Reaching either
     * bound means the client is never going to choose, so the wait ends and
     * the packets are decoded rather than the stream stalling. */
    static constexpr std::size_t unsettledPacketLimit = 32;
    static constexpr std::uint64_t unsettledByteLimit = 16 * 1024 * 1024;
    // What it may hold once it is running, which is backpressure and is
    // bounded only by what the guest can have attached.
    static constexpr std::size_t heldPacketLimit = 128;

    /* Tells the client the picture size, which it needs before it can set its
     * capture queue up. The raw format is the client's choice when it has made
     * one, and otherwise follows the stream: P010 can only carry 10-bit
     * samples, and a stream that has none cannot be offered it. */
    void announce(std::uint32_t id, Stream& stream, std::uint32_t width, std::uint32_t height, unsigned depth)
    {
        auto raw = stream.output.format;
        if (raw == VIRTIO_VIDEO_FORMAT_BGRA8888) { /* keep the negotiated packed format */ }
        else if (raw == VIRTIO_VIDEO_FORMAT_P010) { if (depth != 10) raw = VIRTIO_VIDEO_FORMAT_NV12; }
        else if (depth == 10 && !stream.captureSettled) raw = VIRTIO_VIDEO_FORMAT_P010;
        stream.output = params(VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT, raw, width, height, stream.decoder->reorderDepth());
        stream.announcedWidth = width;
        stream.announcedHeight = height;
        stream.sourceKnown = true;
        event(id, VIRTIO_VIDEO_EVENT_DECODER_RESOLUTION_CHANGED);
    }

    /* The first announcement of a stream comes from the decoder's reading of the
     * stream's headers, before any picture exists. After that a size change is
     * announced where the pictures of the new size begin, so that every picture
     * of the old size is delivered first. */
    void refreshFormat(std::uint32_t id, Stream& stream)
    {
        if (stream.sourceKnown) return;
        const auto format = stream.decoder->format();
        if (!format.width) return;
        /* A capture queue set up before the size was known is for the size the
         * client guessed. When that is not the announced one, no picture goes
         * into it: the client sets its queue up again, as for a size change. */
        if (setUpForAnotherSize(stream, format)) stream.renegotiating = true;
        announce(id, stream, format.width, format.height, format.bitDepth);
    }

    /* Whether the client settled its capture queue before the size was known,
     * for a size other than the stream's. v4l2_codec2 does: it starts with a
     * 64x64 queue and chooses the real one after the source change. */
    static bool setUpForAnotherSize(const Stream& stream, const uml::platform::VideoFormat& format)
    {
        return stream.captureSettled &&
               (stream.output.frame_width != format.width || stream.output.frame_height != format.height);
    }

    /* Whether the packet just given to the decoder declares a picture size the
     * client has not set its capture queue up for: another size than the one
     * before it, or for the first packet, a size other than the one the client
     * guessed. */
    static bool carriesSizeChange(Stream& stream)
    {
        const auto format = stream.decoder->format();
        if (!format.width) return false;
        const bool changed = stream.submittedWidth
            ? format.width != stream.submittedWidth || format.height != stream.submittedHeight
            : !stream.sourceKnown && setUpForAnotherSize(stream, format);
        stream.submittedWidth = format.width;
        stream.submittedHeight = format.height;
        return changed;
    }

    /* Keeps the completion of a coded buffer until the client has dealt with
     * the size change it carries. The buffer stays queued meanwhile, so a
     * client cannot queue it again. */
    void holdSizeChange(Stream& stream, std::uint64_t request, std::uint32_t resource,
                        std::vector<std::uint8_t> response)
    {
        const auto found = stream.resources.find(resourceKey(VIRTIO_VIDEO_QUEUE_TYPE_INPUT, resource));
        if (found != stream.resources.end()) found->second.queued = true;
        stream.sizeChanges.push_back({request, resource, std::move(response)});
    }

    /* Gives back the coded buffers that carried a size change: the client has
     * set its capture queue up for the new size, stopped the queue, drained,
     * or cleared the coded queue, and none of those leaves it waiting for the
     * buffer. */
    void releaseSizeChanges(Stream& stream)
    {
        for (auto& change : stream.sizeChanges) {
            const auto found = stream.resources.find(resourceKey(VIRTIO_VIDEO_QUEUE_TYPE_INPUT, change.resource));
            if (found != stream.resources.end()) found->second.queued = false;
            completions.push_back({change.request, std::move(change.response)});
        }
        stream.sizeChanges.clear();
    }

    void collect(std::uint32_t id, Stream& stream)
    {
        refreshFormat(id, stream);
        for (auto& frame : stream.decoder->takeFrames()) stream.frames.push_back(std::move(frame));
        drive(id, stream);
    }

    /* Deliver what the client has room for, and let in as much of the input it
     * offered as the frames already decoded leave room for. Each pass consumes
     * one held packet, so this ends. */
    void drive(std::uint32_t id, Stream& stream)
    {
        for (;;) {
            pump(id, stream);
            if (stream.held.empty() || stream.frames.size() >= framesInFlight) return;
            auto held = std::move(stream.held.front());
            stream.held.pop_front();
            stream.heldBytes -= held.size;
            const auto found = stream.resources.find(
                resourceKey(VIRTIO_VIDEO_QUEUE_TYPE_INPUT, held.resource));
            if (found != stream.resources.end()) found->second.queued = false;
            stream.decoder->submit(held.packet, held.timestamp);
            ++stream.packets;
            stream.packetBytes += held.size;
            auto result = load<virtio_video_resource_queue_resp>(queued(id, held.timestamp, 0));
            result.data_sizes[0] = held.size;
            if (carriesSizeChange(stream)) holdSizeChange(stream, held.request, held.resource, toBytes(result));
            else completions.push_back({held.request, toBytes(result)});
            refreshFormat(id, stream);
            for (auto& frame : stream.decoder->takeFrames()) stream.frames.push_back(std::move(frame));
        }
    }

    /* A queue that stops returns every buffer it holds, the encoded ones
     * included, or the client waits for input buffers that never come back. */
    void cancelHeld(std::uint32_t id, Stream& stream)
    {
        for (const auto& held : stream.held) {
            completions.push_back({held.request, queued(id, held.timestamp, VIRTIO_VIDEO_DEQUEUE_FLAG_ERR)});
            const auto found = stream.resources.find(
                resourceKey(VIRTIO_VIDEO_QUEUE_TYPE_INPUT, held.resource));
            if (found != stream.resources.end()) found->second.queued = false;
        }
        stream.held.clear();
        stream.heldBytes = 0;
        releaseSizeChanges(stream);
    }

    /* Whether delivering this picture needs the client to set its capture queue
     * up again: it is not the size it was told, or it is 8-bit samples for a
     * client that negotiated P010. */
    static bool changesFormat(const Stream& stream, const uml::platform::DecodedVideoFrame& frame)
    {
        if (frame.format.width != stream.announcedWidth || frame.format.height != stream.announcedHeight)
            return true;
        return stream.output.format == VIRTIO_VIDEO_FORMAT_P010 &&
               frame.layout == uml::platform::VideoPixelLayout::planarYuv && frame.format.bitDepth != 10;
    }

    void pump(std::uint32_t id, Stream& stream)
    {
        for (;;) {
            // Nothing of the new size reaches buffers set up for the old one.
            if (stream.renegotiating) return;
            if (!stream.frames.empty() && stream.sourceKnown && changesFormat(stream, stream.frames.front())) {
                /* Every picture of the old size has been delivered, in order.
                 * One more buffer completes empty and last, which is where a
                 * V4L2 client sees the old stream end, and then the client is
                 * told the new size. A client that is draining has asked for
                 * the end of everything it queued and takes the first last
                 * buffer for it, so a change inside its drain is announced
                 * with no marker of its own: the drain's marker follows the
                 * last picture of the new size. */
                if (!stream.draining) {
                    if (stream.outputs.empty()) return;
                    const auto last = stream.outputs.front();
                    stream.outputs.pop_front();
                    stream.resources.at(resourceKey(VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT, last.resource)).queued = false;
                    completions.push_back({last.request, queued(id, 0, VIRTIO_VIDEO_DEQUEUE_FLAG_EOS)});
                }
                const auto& next = stream.frames.front();
                announce(id, stream, next.format.width, next.format.height, next.format.bitDepth);
                stream.renegotiating = true;
                return;
            }
            if (stream.outputs.empty()) return;
            if (stream.frames.empty() &&
                (!stream.draining || stream.eosSent || !stream.held.empty())) return;
            const auto output = stream.outputs.front();
            stream.outputs.pop_front();
            auto& resource = stream.resources.at(resourceKey(VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT, output.resource));
            resource.queued = false;
            if (stream.frames.empty()) {
                completions.push_back({output.request, queued(id, 0, VIRTIO_VIDEO_DEQUEUE_FLAG_EOS)});
                stream.eosSent = true;
                continue;
            }
            auto frame = std::move(stream.frames.front());
            stream.frames.pop_front();
            ++stream.delivered;
            if (stream.output.format != VIRTIO_VIDEO_FORMAT_BGRA8888 && stream.output.format != VIRTIO_VIDEO_FORMAT_P010 &&
                frame.layout == uml::platform::VideoPixelLayout::planarYuv && frame.format.bitDepth == 10)
                narrowTo8(frame);
            std::vector<std::vector<std::uint8_t>> planes;
            /* The decoder's rows are tight; the guest's lie at the pitch the
             * negotiated plane formats state. */
            const auto& formats = stream.output.plane_formats;
            const std::size_t width = frame.format.width;
            const std::size_t chromaWidth = (width + 1) / 2;
            if (stream.output.format == VIRTIO_VIDEO_FORMAT_BGRA8888) {
                const bool packed = frame.layout == uml::platform::VideoPixelLayout::packedBgra;
                if (!packed) ++stream.converted;
                planes.push_back(padRows(packed ? std::move(frame.pixels) : packBgra(frame), 4 * width,
                                         formats[0].stride));
            } else if (frame.layout == uml::platform::VideoPixelLayout::packedBgra) {
                planes.emplace_back(); // a packed frame cannot answer a planar format
            } else if (stream.output.format == VIRTIO_VIDEO_FORMAT_P010) {
                auto luma = padRows(std::move(frame.luma), 2 * width, formats[0].stride);
                const auto chroma = padRows(std::move(frame.chroma), 4 * chromaWidth, formats[0].stride);
                if (chroma.empty()) luma.clear();
                else luma.insert(luma.end(), chroma.begin(), chroma.end());
                planes.push_back(std::move(luma));
            } else if (stream.output.format == VIRTIO_VIDEO_FORMAT_YUV420) {
                planes.push_back(padRows(std::move(frame.luma), width, formats[0].stride));
                std::vector<std::uint8_t> u(frame.chroma.size() / 2);
                std::vector<std::uint8_t> v(frame.chroma.size() / 2);
                for (std::size_t i = 0; i < u.size(); ++i) {
                    u[i] = frame.chroma[2 * i];
                    v[i] = frame.chroma[2 * i + 1];
                }
                planes.push_back(padRows(std::move(u), chromaWidth, formats[1].stride));
                planes.push_back(padRows(std::move(v), chromaWidth, formats[2].stride));
            } else {
                planes.push_back(padRows(std::move(frame.luma), width, formats[0].stride));
                planes.push_back(padRows(std::move(frame.chroma), 2 * chromaWidth, formats[1].stride));
            }
            bool good = resource.planes.size() == planes.size() && !planes.empty();
            for (std::size_t i = 0; good && i < planes.size(); ++i)
                good = !planes[i].empty() && transfer(resource.planes[i], planes[i], true);
            if (good) resource.filled = true;
            completions.push_back({output.request, queued(id, frame.timestamp,
                good ? 0 : VIRTIO_VIDEO_DEQUEUE_FLAG_ERR, good ? &stream.output : nullptr)});
        }
    }

    VhostUserQueueResult command(std::uint64_t requestId, std::span<const std::uint8_t> request)
    {
        const auto header = load<virtio_video_cmd_hdr>(request);
        const auto id = header.stream_id;
        const auto done = [](std::vector<std::uint8_t> data) {
            return VhostUserQueueResult{VhostUserQueueAction::complete, std::move(data)};
        };
        if (header.type == VIRTIO_VIDEO_CMD_QUERY_CAPABILITY) {
            const auto q = load<virtio_video_query_capability>(request);
            return done(capabilityResponse(capabilities, q.queue_type));
        }
        if (header.type == VIRTIO_VIDEO_CMD_QUERY_CONTROL) {
            const auto q = load<virtio_video_query_control>(request);
            const auto format = load<virtio_video_query_control_profile>(request.subspan(sizeof(q))).format;
            std::vector<std::uint32_t> values;
            if (q.control == VIRTIO_VIDEO_CONTROL_PROFILE) values = profileValues(capabilities, format);
            else if (q.control == VIRTIO_VIDEO_CONTROL_LEVEL) values = levelValues(capabilities, format);
            else return done(reply(id, VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL));
            auto result = reply(id, VIRTIO_VIDEO_RESP_OK_QUERY_CONTROL);
            const auto count = toBytes(virtio_video_query_control_resp_profile{std::uint32_t(values.size()), {}});
            result.insert(result.end(), count.begin(), count.end());
            for (auto value : values) {
                const auto data = toBytes(value);
                result.insert(result.end(), data.begin(), data.end());
            }
            return done(std::move(result));
        }
        if (header.type == VIRTIO_VIDEO_CMD_STREAM_CREATE) {
            const auto q = load<virtio_video_stream_create>(request);
            if (!id || streams.contains(id) || streams.size() >= 16 || q.in_mem_type || q.out_mem_type)
                return done(reply(id, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER));
            const auto cap = std::find_if(capabilities.begin(), capabilities.end(),
                [&q](const auto& c) { return wireCodec(c.codec) == q.coded_format; });
            if (cap == capabilities.end()) return done(reply(id, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER));
            Stream stream;
            stream.coded = q.coded_format;
            stream.profile = wireProfile(cap->profile);
            stream.decoder = factory(cap->codec);
            stream.input = params(VIRTIO_VIDEO_QUEUE_TYPE_INPUT, q.coded_format, 128, 128);
            stream.output = params(VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT,
                                   defaultCapture(cap->bitDepth), 128, 128);
            streams.emplace(id, std::move(stream));
            log.opened(id, codecName(cap->codec), profileName(cap->profile));
            return done(reply(id));
        }
        const auto found = streams.find(id);
        if (found == streams.end()) return done(reply(id, VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID));
        auto& stream = found->second;
        switch (header.type) {
        case VIRTIO_VIDEO_CMD_STREAM_DESTROY:
            cancelOutputs(id, stream);
            cancelHeld(id, stream);
            record(id, stream, "destroy");
            streams.erase(found);
            return done(reply(id));
        case VIRTIO_VIDEO_CMD_GET_PARAMS: {
            const auto q = load<virtio_video_get_params>(request);
            if (!validQueue(q.queue_type)) throw std::invalid_argument("invalid parameter queue");
            return done(toBytes(virtio_video_get_params_resp{{VIRTIO_VIDEO_RESP_OK_GET_PARAMS, id},
                q.queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT ? stream.input : stream.output}));
        }
        case VIRTIO_VIDEO_CMD_SET_PARAMS: {
            const auto p = load<virtio_video_set_params>(request).params;
            const auto coded = p.queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT ? p.format : stream.coded;
            const auto cap = std::find_if(capabilities.begin(), capabilities.end(),
                [coded](const auto& c) { return wireCodec(c.codec) == coded; });
            if (cap == capabilities.end() || !validQueue(p.queue_type) || !p.frame_width || !p.frame_height ||
                p.frame_width > cap->maximumWidth || p.frame_height > cap->maximumHeight)
                throw std::invalid_argument("invalid video parameters");
            if (p.queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
                if (p.format != stream.coded) {
                    if (!stream.resources.empty() || stream.sourceKnown)
                        throw std::invalid_argument("coded format changed with allocated buffers");
                    stream.decoder = factory(cap->codec);
                    stream.coded = p.format;
                    stream.profile = wireProfile(cap->profile);
                    stream.captureSettled = false;
                    stream.captureDeferred = true;
                    stream.output = params(VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT,
                        defaultCapture(cap->bitDepth), p.frame_width, p.frame_height);
                }
                stream.input = inputParams(p.format, p.frame_width, p.frame_height);
            } else {
                const bool packed = p.format == VIRTIO_VIDEO_FORMAT_BGRA8888;
                if (!packed && p.format != VIRTIO_VIDEO_FORMAT_NV12 && p.format != VIRTIO_VIDEO_FORMAT_YUV420 &&
                    p.format != VIRTIO_VIDEO_FORMAT_P010) throw std::invalid_argument("unsupported raw video format");
                /* Only P010 has to match the stream's depth: it carries 10-bit
                 * samples and nothing else. A packed format and the 8-bit
                 * planar ones are reached from either depth, a 10-bit stream
                 * being narrowed on its way. */
                if (p.format == VIRTIO_VIDEO_FORMAT_P010 &&
                    (std::none_of(capabilities.begin(), capabilities.end(), [&stream](const auto& c) {
                         return wireCodec(c.codec) == stream.coded && c.bitDepth == 10u;
                     }) || (stream.sourceKnown && stream.decoder->format().bitDepth != 10)))
                    throw std::invalid_argument("raw video bit depth is incompatible with the stream");
                stream.decoder->requestOutputLayout(decodeLayout(p.format, stream.sourceKnown));
                stream.captureSettled = true;
                stream.captureDeferred = false;
                stream.output = params(p.queue_type, p.format, p.frame_width, p.frame_height,
                                       stream.decoder->reorderDepth());
                /* A client that sets the new size is done renegotiating. */
                if (p.frame_width == stream.announcedWidth && p.frame_height == stream.announcedHeight) {
                    stream.renegotiating = false;
                    releaseSizeChanges(stream);
                }
                drive(id, stream);
            }
            return done(reply(id));
        }
        case VIRTIO_VIDEO_CMD_RESOURCE_ATTACH: {
            const auto q = load<virtio_video_resource_attach>(request);
            if (!validQueue(q.queue_type) || !q.resource_id)
                throw std::invalid_argument("invalid video resource");
            const auto key = resourceKey(q.queue_type, q.resource_id);
            /* A guest rebinds a buffer index to different memory by attaching
             * its resource identifier again, so an identifier the guest is
             * not using answers with its new memory rather than an error. */
            const auto previous = stream.resources.find(key);
            if (previous != stream.resources.end() && previous->second.queued)
                throw std::invalid_argument("video resource is already queued");
            if (previous == stream.resources.end() && stream.resources.size() >= 128)
                throw std::invalid_argument("invalid video resource");
            const auto& p = q.queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT ? stream.input : stream.output;
            Resource resource;
            resource.planes = parseScatterLists(request.subspan(sizeof(q)), p.num_planes, mapper,
                                                regions.get());
            if (previous != stream.resources.end()) previous->second = std::move(resource);
            else stream.resources.emplace(key, std::move(resource));
            return done(reply(id));
        }
        case VIRTIO_VIDEO_CMD_RESOURCE_QUEUE: {
            const auto q = load<virtio_video_resource_queue>(request);
            const auto resource = stream.resources.find(resourceKey(q.queue_type, q.resource_id));
            if (resource == stream.resources.end()) return done(reply(id, VIRTIO_VIDEO_RESP_ERR_INVALID_RESOURCE_ID));
            if (resource->second.queued) throw std::invalid_argument("video resource is already queued");
            if (q.queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
                if (!q.data_sizes[0] || q.data_sizes[0] > maximumPacket)
                    throw std::invalid_argument("invalid encoded packet size");
                std::vector<std::uint8_t> packet(q.data_sizes[0]);
                if (!transfer(resource->second.planes[0], packet, false)) throw std::invalid_argument("short encoded video resource");
                stream.draining = stream.eosSent = false;
                /* A client that has queued this much encoded data without
                 * saying what it wants from the capture queue is not going to
                 * say. Stop waiting and decode what is held, in the planar
                 * layout it has not overridden, which is the one a client that
                 * chooses later can still be served from. */
                if (stream.captureDeferred && (stream.held.size() >= unsettledPacketLimit ||
                                               stream.heldBytes >= unsettledByteLimit)) {
                    stream.captureDeferred = false;
                    drive(id, stream);
                }
                /* Order is the client's, so a packet waits behind one that is
                 * already waiting even if room appeared meanwhile. */
                if (stream.captureDeferred || !stream.held.empty() ||
                    stream.frames.size() >= framesInFlight) {
                    if (stream.held.size() >= heldPacketLimit)
                        throw std::invalid_argument("video input queue is full");
                    resource->second.queued = true;
                    stream.heldBytes += q.data_sizes[0];
                    stream.held.push_back({requestId, q.resource_id, q.timestamp,
                                           q.data_sizes[0], std::move(packet)});
                    /* Reading the packet's headers reports the picture size
                     * without decoding it, which is the answer the client is
                     * waiting for before it can choose its capture format. */
                    if (stream.captureDeferred) {
                        stream.decoder->inspect(stream.held.back().packet);
                        refreshFormat(id, stream);
                    }
                    return {VhostUserQueueAction::defer, {}};
                }
                stream.decoder->submit(packet, q.timestamp);
                ++stream.packets;
                stream.packetBytes += q.data_sizes[0];
                const bool changesSize = carriesSizeChange(stream);
                collect(id, stream);
                auto result = load<virtio_video_resource_queue_resp>(queued(id, q.timestamp, 0));
                result.data_sizes[0] = q.data_sizes[0];
                if (changesSize) {
                    holdSizeChange(stream, requestId, q.resource_id, toBytes(result));
                    return {VhostUserQueueAction::defer, {}};
                }
                return done(toBytes(result));
            }
            if (stream.outputs.size() >= 32) throw std::invalid_argument("video capture queue is full");
            /* Starting the capture queue settles the format as surely as
             * naming one: a client that keeps what it was given has chosen the
             * format it was given. Nothing has been decoded yet, so the layout
             * takes effect on the very first picture. */
            if (!stream.captureSettled) {
                stream.captureSettled = true;
                stream.captureDeferred = false;
                stream.decoder->requestOutputLayout(decodeLayout(stream.output.format, stream.sourceKnown));
            }
            ++stream.captureQueued;
            stream.captureBuffers.insert(q.resource_id);
            if (resource->second.filled) ++stream.captureRequeued;
            for (const auto& plane : resource->second.planes)
                for (const auto& segment : plane)
                    (segment.shared ? stream.captureShared : stream.captureGuestPages) = true;
            resource->second.queued = true;
            stream.outputs.push_back({requestId, q.resource_id});
            drive(id, stream);
            return {VhostUserQueueAction::defer, {}};
        }
        case VIRTIO_VIDEO_CMD_STREAM_DRAIN:
            /* Nothing further is coming, so nothing is still waiting for a
             * capture format: decode what is held before draining, or the end
             * of stream would be reported ahead of the pictures. */
            stream.captureDeferred = false;
            releaseSizeChanges(stream);
            drive(id, stream);
            stream.decoder->drain();
            stream.draining = true;
            collect(id, stream);
            return done(reply(id));
        case VIRTIO_VIDEO_CMD_QUEUE_CLEAR:
        case VIRTIO_VIDEO_CMD_QUEUE_DETACH_RESOURCES: {
            const auto q = load<virtio_video_queue_clear>(request);
            if (!validQueue(q.queue_type)) throw std::invalid_argument("invalid clear queue");
            if (q.queue_type == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) {
                cancelOutputs(id, stream);
                stream.renegotiating = false;
                releaseSizeChanges(stream);
            } else {
                /* A seek. The decoder forgets what it decoded and keeps the
                 * stream's parameter sets, which the client does not send
                 * again, and the format it settled on: the picture size is
                 * announced again only if it is not the one the client has. */
                cancelHeld(id, stream);
                stream.decoder->discard();
                stream.frames.clear();
                stream.draining = stream.eosSent = false;
            }
            if (header.type == VIRTIO_VIDEO_CMD_QUEUE_DETACH_RESOURCES)
                std::erase_if(stream.resources, [&q](const auto& r) { return (r.first >> 32) == q.queue_type; });
            return done(reply(id));
        }
        case VIRTIO_VIDEO_CMD_GET_CONTROL: {
            const auto q = load<virtio_video_get_control>(request);
            auto result = reply(id, VIRTIO_VIDEO_RESP_OK_GET_CONTROL);
            std::uint32_t value;
            if (q.control == VIRTIO_VIDEO_CONTROL_PROFILE) value = stream.profile;
            else if (q.control == VIRTIO_VIDEO_CONTROL_LEVEL) {
                // The stream's level is not tracked: it is the highest the format decodes.
                const auto levels = levelValues(capabilities, stream.coded);
                if (levels.empty()) return done(reply(id, VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL));
                value = levels.back();
            } else return done(reply(id, VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL));
            const auto data = toBytes(virtio_video_control_val_profile{value, {}});
            result.insert(result.end(), data.begin(), data.end());
            return done(std::move(result));
        }
        default: return done(reply(id, VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION));
        }
    }
};

VirtioVideoDevice::VirtioVideoDevice(std::vector<uml::platform::VideoDecodeCapability> caps, DecoderFactory factory)
    : implementation_(std::make_unique<Implementation>())
{
    if (caps.empty() || !factory) throw std::invalid_argument("video capabilities and decoder factory are required");
    std::erase_if(caps, [](const auto& capability) { return !wireNames(capability.codec); });
    if (caps.empty()) throw std::invalid_argument("no video capability this protocol can name");
    implementation_->capabilities = std::move(caps);
    implementation_->factory = std::move(factory);
    for (const auto& capability : implementation_->capabilities)
        implementation_->log.advertised(codecName(capability.codec), profileName(capability.profile),
            capability.maximumWidth, capability.maximumHeight, capability.bitDepth);
}
VirtioVideoDevice::~VirtioVideoDevice() = default;
const char* VirtioVideoDevice::name() const noexcept { return "Virtio Video decoder"; }
std::uint64_t VirtioVideoDevice::features() const noexcept
{
    const std::uint64_t base = (1 << VIRTIO_VIDEO_F_RESOURCE_GUEST_PAGES) |
        (1 << VIRTIO_VIDEO_F_RESOURCE_NON_CONTIG);
    /* Only a guest whose host also serves a shared-memory region can name
     * buffers inside one. */
    return implementation_->regions ? base | (1 << VIRTIO_VIDEO_F_RESOURCE_SHM_REGION) : base;
}
std::size_t VirtioVideoDevice::queueCount() const noexcept { return 2; }
std::vector<std::uint8_t> VirtioVideoDevice::configuration() const
{
    return toBytes(virtio_video_config{UML_VIRTIO_VIDEO_VERSION, 4096, 512});
}
void VirtioVideoDevice::writeConfiguration(std::uint32_t, std::uint32_t, std::span<const std::uint8_t>) {}
void VirtioVideoDevice::setGuestMapper(GuestMapper mapper) { implementation_->mapper = std::move(mapper); }
void VirtioVideoDevice::setSharedMemoryRegions(
    std::shared_ptr<const uml::virtio::SharedMemoryRegions> regions)
{
    implementation_->regions = std::move(regions);
}
VhostUserQueueResult VirtioVideoDevice::processQueue(std::uint32_t index, std::uint64_t id,
    std::span<const std::uint8_t> request, std::size_t capacity)
{
    auto& impl = *implementation_;
    if (index == 1) {
        if (!request.empty() || capacity < sizeof(virtio_video_event))
            return {VhostUserQueueAction::complete, {}};
        if (!impl.events.empty()) {
            auto result = toBytes(impl.events.front()); impl.events.pop_front();
            return {VhostUserQueueAction::complete, std::move(result)};
        }
        if (impl.eventBuffers.size() >= uml::gpu::protocol::maximumVirtqueueSize)
            return {VhostUserQueueAction::retry, {}};
        impl.eventBuffers.push_back(id);
        return {VhostUserQueueAction::defer, {}};
    }
    if (index != 0 || request.size() < sizeof(virtio_video_cmd_hdr) || capacity < sizeof(virtio_video_cmd_hdr))
        return {VhostUserQueueAction::complete, {}};
    const auto header = load<virtio_video_cmd_hdr>(request);
    try {
        if (header.type == VIRTIO_VIDEO_CMD_RESOURCE_QUEUE && capacity < sizeof(virtio_video_resource_queue_resp))
            return {VhostUserQueueAction::complete, reply(header.stream_id, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER)};
        auto result = impl.command(id, request);
        if (result.response.size() > capacity)
            return {VhostUserQueueAction::complete, reply(header.stream_id, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER)};
        return result;
    } catch (const std::invalid_argument&) {
        return {VhostUserQueueAction::complete, reply(header.stream_id, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER)};
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Virtio video stream %u: %s\n", header.stream_id, error.what());
        const auto found = impl.streams.find(header.stream_id);
        if (found != impl.streams.end()) {
            impl.cancelOutputs(header.stream_id, found->second);
            impl.cancelHeld(header.stream_id, found->second);
            impl.event(header.stream_id, VIRTIO_VIDEO_EVENT_ERROR);
        }
        if (header.type == VIRTIO_VIDEO_CMD_RESOURCE_QUEUE)
            return {VhostUserQueueAction::complete, impl.queued(header.stream_id, 0, VIRTIO_VIDEO_DEQUEUE_FLAG_ERR)};
        return {VhostUserQueueAction::complete, reply(header.stream_id, VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION)};
    }
}
bool VirtioVideoDevice::takeQueueCompletion(VhostUserQueueCompletion& completion)
{
    auto& pending = implementation_->completions;
    if (pending.empty()) return false;
    completion = std::move(pending.front()); pending.pop_front(); return true;
}
void VirtioVideoDevice::reset() noexcept
{
    auto& impl = *implementation_;
    impl.closeAll("reset");
    impl.streams.clear(); impl.events.clear(); impl.eventBuffers.clear(); impl.completions.clear();
}
} // namespace uml::video
