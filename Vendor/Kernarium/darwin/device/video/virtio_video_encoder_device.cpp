// SPDX-License-Identifier: GPL-2.0-or-later
#include "virtio_video_encoder_device.hpp"

#include "activity_log.hpp"
#include "encoder_capabilities.hpp"
#include "encoder_controls.hpp"
#include "guest_buffers.hpp"
#include "raw_pitch.hpp"
#include "unique_descriptor.hpp"
#include "vhost_user_protocol.hpp"

#include <sys/event.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace uml::video {
namespace {
using uml::platform::EncodedVideoFrame;
using uml::platform::EncodedVideoFrameKind;
using uml::platform::RawVideoLayout;
using uml::platform::VideoCodec;
using uml::platform::VideoEncodeCapability;
using uml::platform::VideoEncodeProfile;
using uml::platform::VideoEncodeStatus;
using uml::platform::VideoFrameType;
using uml::platform::VideoRateControl;
using uml::platform::WritableVideoFrame;
using uml::virtio::VhostUserQueueAction;
using uml::virtio::VhostUserQueueCompletion;
using uml::virtio::VhostUserQueueResult;
using uml::virtio::toBytes;

/* Streams one device serves at most, and the least of that and what the
 * hardware reported it can run at once. */
constexpr std::size_t maximumStreams = 8;
/* Coded frames the device takes from the platform before the guest has a
 * buffer for them. Beyond it the platform's frames wait where they are, its
 * in-flight count stays high, and the guest feels the pressure as raw
 * buffers that do not come back. */
constexpr std::size_t codedBacklogLimit = 8;
constexpr std::size_t heldFrameLimit = 128;
constexpr std::size_t maximumControls = 256;
constexpr std::uint32_t minimumCodedBuffer = 1u << 20;
constexpr std::uint32_t maximumCodedBuffer = 32u << 20;
constexpr std::uint32_t defaultWidth = 1280;
constexpr std::uint32_t defaultHeight = 720;
constexpr std::uint32_t minimumBuffers = 1;
constexpr std::uint32_t maximumBuffers = 32;

template<class T> T load(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < sizeof(T)) throw std::invalid_argument("short video command");
    return uml::virtio::fromBytes<T>(bytes);
}
std::vector<std::uint8_t> reply(std::uint32_t stream, std::uint32_t type = VIRTIO_VIDEO_RESP_OK_NODATA)
{
    return toBytes(virtio_video_cmd_hdr {type, stream});
}
std::uint64_t resourceKey(std::uint32_t queue, std::uint32_t id) { return (std::uint64_t(queue) << 32) | id; }
bool validQueue(std::uint32_t queue)
{
    return queue == VIRTIO_VIDEO_QUEUE_TYPE_INPUT || queue == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
}
std::uint32_t roundUp(std::uint32_t value, std::uint32_t shift) { return (value + (1u << shift) - 1) >> shift; }
std::uint32_t alignDown(std::uint32_t value, std::uint32_t alignment) { return value - value % alignment; }

const char* codecName(VideoCodec codec) noexcept
{
    switch (codec) {
    case VideoCodec::h264: return "h264";
    case VideoCodec::hevc: return "hevc";
    case VideoCodec::jpeg: return "jpeg";
    case VideoCodec::vp9: return "vp9";
    }
    return "unknown";
}
const char* profileName(VideoEncodeProfile profile) noexcept
{
    switch (profile) {
    case VideoEncodeProfile::h264Baseline: return "h264-baseline";
    case VideoEncodeProfile::h264ConstrainedBaseline: return "h264-constrained-baseline";
    case VideoEncodeProfile::h264Main: return "h264-main";
    case VideoEncodeProfile::h264High: return "h264-high";
    case VideoEncodeProfile::h264ConstrainedHigh: return "h264-constrained-high";
    case VideoEncodeProfile::h264High422: return "h264-high422";
    case VideoEncodeProfile::h264High444Predictive: return "h264-high444-predictive";
    case VideoEncodeProfile::hevcMain: return "hevc-main";
    case VideoEncodeProfile::hevcMain10: return "hevc-main10";
    case VideoEncodeProfile::hevcMainStill: return "hevc-main-still";
    case VideoEncodeProfile::jpegBaseline: return "jpeg-baseline";
    }
    return "unknown";
}
const char* layoutName(RawVideoLayout layout) noexcept
{
    switch (layout) {
    case RawVideoLayout::nv12: return "nv12";
    case RawVideoLayout::i420: return "i420";
    case RawVideoLayout::p010: return "p010";
    case RawVideoLayout::nv16: return "nv16";
    case RawVideoLayout::nv24: return "nv24";
    case RawVideoLayout::bgra: return "bgra";
    case RawVideoLayout::rgba: return "rgba";
    }
    return "unknown";
}
const char* rateControlName(VideoRateControl mode) noexcept
{
    switch (mode) {
    case VideoRateControl::variableBitrate: return "vbr";
    case VideoRateControl::constantBitrate: return "cbr";
    case VideoRateControl::constantQuality: return "cq";
    case VideoRateControl::constantQp: return "cqp";
    }
    return "unknown";
}
const char* statusName(VideoEncodeStatus status) noexcept
{
    switch (status) {
    case VideoEncodeStatus::ok: return "ok";
    case VideoEncodeStatus::unsupported: return "unsupported";
    case VideoEncodeStatus::invalid: return "invalid";
    case VideoEncodeStatus::exhausted: return "exhausted";
    case VideoEncodeStatus::failed: return "failed";
    }
    return "unknown";
}

/* Where each plane of a raw frame lies in the guest's buffer. A component is
 * one plane of the platform's frame (luma, interleaved chroma, one chroma
 * plane, or the packed pixels); the guest holds them in one buffer or one
 * buffer per component. */
struct Component {
    unsigned plane {};
    std::uint64_t offset {};
    std::uint32_t stride {};
    std::uint32_t rows {};
    std::uint32_t xShift {};
    std::uint32_t yShift {};
    std::uint32_t sampleBytes {};
};
struct RawLayout {
    unsigned planes {};
    std::array<virtio_video_plane_format, VIRTIO_VIDEO_MAX_PLANES> formats {};
    std::vector<Component> components;
};

/* The layout of a `width` x `height` frame. `requestedPlanes` is zero for the
 * layout the format has by default (a buffer per component for the planar 8-bit
 * formats, one buffer for the others), one for everything in one buffer, or
 * the component count. The host decides the strides, and they are the pitch a
 * guest allocator gives a buffer (raw_pitch.hpp): the guest reads them back
 * from the parameters it gets, and a plane's rows lie that far apart. */
std::optional<RawLayout> rawLayout(RawVideoLayout layout, std::uint32_t width, std::uint32_t height,
                                   unsigned requestedPlanes)
{
    std::vector<Component> components;
    const auto chromaHeight = roundUp(height, 1);
    unsigned naturalPlanes = 1;
    switch (layout) {
    case RawVideoLayout::nv12: {
        // The interleaved chroma rows are as long as the luma rows and share their pitch.
        const auto pitch = rawPitch(width);
        components = {{0, 0, pitch, height, 0, 0, 1}, {1, 0, pitch, chromaHeight, 1, 1, 2}};
        naturalPlanes = 2;
        break;
    }
    case RawVideoLayout::i420: {
        const auto lumaPitch = rawPitch(width, rawTriplanarLumaAlignment);
        components = {{0, 0, lumaPitch, height, 0, 0, 1}, {1, 0, lumaPitch / 2, chromaHeight, 1, 1, 1},
                      {2, 0, lumaPitch / 2, chromaHeight, 1, 1, 1}};
        naturalPlanes = 3;
        break;
    }
    case RawVideoLayout::p010: {
        const auto pitch = rawPitch(2 * width);
        components = {{0, 0, pitch, height, 0, 0, 2}, {1, 0, pitch, chromaHeight, 1, 1, 4}};
        naturalPlanes = 1;
        break;
    }
    case RawVideoLayout::nv16: {
        const auto pitch = rawPitch(width);
        components = {{0, 0, pitch, height, 0, 0, 1}, {1, 0, pitch, height, 1, 0, 2}};
        naturalPlanes = 2;
        break;
    }
    case RawVideoLayout::nv24:
        components = {{0, 0, rawPitch(width), height, 0, 0, 1}, {1, 0, rawPitch(2 * width), height, 0, 0, 2}};
        naturalPlanes = 2;
        break;
    case RawVideoLayout::bgra:
    case RawVideoLayout::rgba:
        components = {{0, 0, rawPitch(4 * width), height, 0, 0, 4}};
        naturalPlanes = 1;
        break;
    }
    if (components.empty()) return std::nullopt;
    /* A buffer per component is offered only for the formats whose planes
     * are separate buffers in the capability response (planesLayout). */
    const bool perComponent = requestedPlanes == 0 ? naturalPlanes == components.size()
        : requestedPlanes == components.size() && components.size() > 1 && naturalPlanes == components.size();
    if (requestedPlanes > 1 && !perComponent) return std::nullopt;
    if (requestedPlanes == 1 || (requestedPlanes == 0 && !perComponent)) {
        std::uint64_t offset = 0;
        for (auto& component : components) {
            component.plane = 0;
            component.offset = offset;
            offset += std::uint64_t(component.stride) * component.rows;
        }
        RawLayout result;
        result.planes = 1;
        result.formats[0] = {std::uint32_t(offset), components.front().stride};
        result.components = std::move(components);
        return result;
    }
    RawLayout result;
    result.planes = unsigned(components.size());
    for (std::size_t index = 0; index < components.size(); ++index) {
        components[index].plane = unsigned(index);
        result.formats[index] = {components[index].stride * components[index].rows, components[index].stride};
    }
    result.components = std::move(components);
    return result;
}

/* Removes the parameter-set NAL units (SPS and PPS, plus VPS for HEVC) of an
 * Annex B access unit. A guest that turns PREPEND_SPSPPS_TO_IDR off gets the
 * sets once, at the start, instead of before every key frame. */
void stripParameterSets(VideoCodec codec, std::vector<std::uint8_t>& bytes)
{
    const auto isParameterSet = [codec](std::uint8_t header) {
        if (codec == VideoCodec::hevc) {
            const auto type = (header >> 1) & 0x3f;
            return type >= 32 && type <= 34;
        }
        const auto type = header & 0x1f;
        return type == 7 || type == 8;
    };
    std::vector<std::size_t> starts;  // where each NAL unit's start code begins
    for (std::size_t index = 0; index + 3 < bytes.size() + 0; ++index) {
        if (bytes[index] == 0 && bytes[index + 1] == 0 && bytes[index + 2] == 1) {
            const auto begin = index > 0 && bytes[index - 1] == 0 ? index - 1 : index;
            if (starts.empty() || begin > starts.back()) starts.push_back(begin);
            index += 2;
        }
    }
    if (starts.empty()) return;
    std::vector<std::uint8_t> kept(bytes.begin(), bytes.begin() + std::ptrdiff_t(starts.front()));
    for (std::size_t unit = 0; unit < starts.size(); ++unit) {
        const auto begin = starts[unit];
        const auto end = unit + 1 < starts.size() ? starts[unit + 1] : bytes.size();
        const auto header = begin + (bytes[begin] == 0 && bytes[begin + 1] == 0 && bytes[begin + 2] == 0 ? 4 : 3);
        if (header < end && isParameterSet(bytes[header])) continue;
        kept.insert(kept.end(), bytes.begin() + std::ptrdiff_t(begin), bytes.begin() + std::ptrdiff_t(end));
    }
    bytes = std::move(kept);
}

std::string join(const std::vector<std::string>& names)
{
    std::string result;
    for (const auto& name : names) result += (result.empty() ? "" : ",") + name;
    return result.empty() ? "none" : result;
}
}

class VirtioVideoEncoderDevice::Implementation {
public:
    struct Resource {
        GuestPlanes planes;
        std::uint64_t capacity {};  // bytes of the first plane
        bool queued {};
    };
    struct HeldFrame {
        std::uint64_t request;
        std::uint32_t resource;
        std::uint64_t timestamp;
        std::array<std::uint32_t, VIRTIO_VIDEO_MAX_PLANES> dataSizes;
        bool forceKey;
    };
    struct Output {
        std::uint64_t request;
        std::uint32_t resource;
    };
    enum class CodedKind { frame, parameterSets, endOfStream, error };
    struct Coded {
        CodedKind kind;
        VideoFrameType type;
        std::uint64_t timestamp;
        std::vector<std::uint8_t> bytes;
    };
    struct Stream {
        VideoCodec codec {VideoCodec::h264};
        RawVideoLayout input {RawVideoLayout::nv12};
        std::uint32_t frameWidth {};
        std::uint32_t frameHeight {};
        virtio_video_crop crop {};
        RawLayout layout;
        StreamInfo info;
        ControlValues controls;
        std::unique_ptr<uml::platform::VideoEncoder> encoder;
        /* Whether the platform session has been configured and has not been
         * reset since. */
        bool streaming {};
        bool failed {};
        /* The guest's FORCE_KEY_FRAME applies to the next frame it queues;
         * recovery from a coded frame that did not fit applies to the next
         * frame the device submits. */
        bool forceKeyNext {};
        bool recoverKey {};
        bool parameterSetsDelivered {};
        std::unordered_map<std::uint64_t, Resource> resources;
        std::deque<HeldFrame> held;
        std::deque<Output> outputs;
        std::deque<Coded> coded;
        std::size_t inFlight {};
        std::optional<std::uint64_t> drainRequest;
        bool drainIssued {};
        std::uint64_t framesIn {};
        std::uint64_t framesOut {};
        std::uint64_t keyFrames {};
        std::uint64_t bytesOut {};
        std::uint64_t errors {};
    };

    ActivityLog log;
    std::vector<VideoEncodeCapability> capabilities;
    EncoderControls controls;
    EncoderFactory factory;
    GuestMapper mapper;
    std::shared_ptr<const uml::virtio::SharedMemoryRegions> regions;
    std::unordered_map<std::uint32_t, Stream> streams;
    std::deque<std::uint64_t> eventBuffers;
    std::deque<virtio_video_event> events;
    std::deque<VhostUserQueueCompletion> completions;
    uml::gpu::UniqueDescriptor queue;
    std::size_t streamLimit {maximumStreams};

    using EncoderFactory = VirtioVideoEncoderDevice::EncoderFactory;

    Implementation(std::vector<VideoEncodeCapability> caps, EncoderFactory make)
        : capabilities(std::move(caps)), controls(capabilities), factory(std::move(make)),
          queue(::kqueue())
    {
        if (queue.get() < 0) throw std::runtime_error("Virtio video encoder event queue failed");
        for (const auto& capability : capabilities) {
            if (capability.concurrentSessions != 0)
                streamLimit = std::min<std::size_t>(streamLimit, capability.concurrentSessions);
            std::vector<std::string> profiles, inputs, modes;
            for (const auto& profile : capability.profiles) {
                profiles.emplace_back(profileName(profile.profile));
                for (const auto layout : profile.inputs)
                    if (std::find(inputs.begin(), inputs.end(), layoutName(layout)) == inputs.end())
                        inputs.emplace_back(layoutName(layout));
            }
            for (const auto mode : capability.rateControls) modes.emplace_back(rateControlName(mode));
            log.encoderAdvertised(codecName(capability.codec), join(profiles), join(inputs),
                                  capability.maximumWidth, capability.maximumHeight, join(modes),
                                  capability.maximumBFrames, capability.maximumTemporalLayers,
                                  capability.concurrentSessions);
        }
    }
    ~Implementation() { closeAll("shutdown"); }

    [[nodiscard]] const VideoEncodeCapability* capabilityOf(VideoCodec codec) const noexcept
    {
        const auto found = std::find_if(capabilities.begin(), capabilities.end(),
                                        [codec](const auto& capability) { return capability.codec == codec; });
        return found == capabilities.end() ? nullptr : &*found;
    }
    [[nodiscard]] static bool accepts(const VideoEncodeCapability& capability, RawVideoLayout layout)
    {
        return std::any_of(capability.profiles.begin(), capability.profiles.end(), [layout](const auto& profile) {
            return std::find(profile.inputs.begin(), profile.inputs.end(), layout) != profile.inputs.end();
        });
    }

    void record(std::uint32_t id, const Stream& stream, const char* reason) noexcept
    {
        log.encoderClosed(id, reason, codecName(stream.codec), stream.framesIn, stream.framesOut,
                          stream.keyFrames, stream.bytesOut, stream.errors, stream.outputs.size());
    }
    void closeAll(const char* reason) noexcept
    {
        for (const auto& [id, stream] : streams) record(id, stream, reason);
    }

    void event(std::uint32_t stream, std::uint32_t type)
    {
        /* Called from the wake-up path too, where a throw would leave the
         * transport. A full queue already holds unread errors, so the new
         * one adds nothing the guest has not been told. */
        if (events.size() >= 64) return;
        events.push_back({type, stream});
        while (!events.empty() && !eventBuffers.empty()) {
            completions.push_back({eventBuffers.front(), toBytes(events.front())});
            eventBuffers.pop_front();
            events.pop_front();
        }
    }

    std::vector<std::uint8_t> queued(std::uint32_t stream, std::uint64_t timestamp, std::uint32_t flags,
                                     std::uint32_t size = 0,
                                     const std::array<std::uint32_t, VIRTIO_VIDEO_MAX_PLANES>* sizes = nullptr)
    {
        virtio_video_resource_queue_resp response {};
        response.hdr = {VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE, stream};
        response.timestamp = timestamp;
        response.flags = flags;
        if (sizes) std::copy(sizes->begin(), sizes->end(), response.data_sizes);
        else response.data_sizes[0] = size;
        return toBytes(response);
    }

    /* The size a coded buffer needs for a frame: generous enough for a key
     * frame at a high bitrate, bounded so one stream cannot ask for the
     * guest's memory. */
    static std::uint32_t codedBufferSize(const Stream& stream) noexcept
    {
        const std::uint64_t picture = std::uint64_t(stream.crop.width) * stream.crop.height * 3 / 2;
        return std::uint32_t(std::clamp<std::uint64_t>(picture, minimumCodedBuffer, maximumCodedBuffer));
    }

    [[nodiscard]] std::uint32_t codedFormat(const Stream& stream) const
    {
        return encodedWireFormat(stream.codec).value_or(0);
    }

    virtio_video_params inputParams(const Stream& stream) const
    {
        virtio_video_params p {};
        p.queue_type = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
        p.format = wireRawFormat(stream.input);
        p.frame_width = stream.frameWidth;
        p.frame_height = stream.frameHeight;
        p.min_buffers = minimumBuffers;
        p.max_buffers = maximumBuffers;
        p.crop = stream.crop;
        p.frame_rate = frameRate(stream);
        p.num_planes = stream.layout.planes;
        for (unsigned plane = 0; plane < stream.layout.planes; ++plane) p.plane_formats[plane] = stream.layout.formats[plane];
        return p;
    }
    virtio_video_params outputParams(const Stream& stream) const
    {
        virtio_video_params p {};
        p.queue_type = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
        p.format = codedFormat(stream);
        p.frame_width = stream.crop.width;
        p.frame_height = stream.crop.height;
        p.min_buffers = minimumBuffers;
        p.max_buffers = maximumBuffers;
        p.crop = {0, 0, stream.crop.width, stream.crop.height};
        p.frame_rate = frameRate(stream);
        p.num_planes = 1;
        p.plane_formats[0] = {codedBufferSize(stream), 0};
        return p;
    }
    static std::uint32_t frameRate(const Stream& stream) noexcept
    {
        return (stream.info.intervalDenominator + stream.info.intervalNumerator / 2) / stream.info.intervalNumerator;
    }

    /* Gives a stream the raw layout and size it has before its guest says. */
    void useRawFormat(Stream& stream, RawVideoLayout layout, std::uint32_t width, std::uint32_t height,
                      std::uint32_t planes, const virtio_video_crop* crop)
    {
        const auto* capability = capabilityOf(stream.codec);
        const auto alignment = std::max<std::uint32_t>(capability->sizeAlignment, 1);
        if (!width || !height || width > capability->maximumWidth || height > capability->maximumHeight)
            throw std::invalid_argument("invalid video frame size");
        virtio_video_crop wanted = crop ? *crop : virtio_video_crop {};
        if (wanted.width == 0 || wanted.height == 0) wanted = {0, 0, width, height};
        /* A crop is adjusted the way a V4L2 driver adjusts a selection: moved
         * inside the frame and rounded down to what the encoder can take. */
        wanted.left = alignDown(std::min(wanted.left, width - 1), alignment);
        wanted.top = alignDown(std::min(wanted.top, height - 1), alignment);
        wanted.width = alignDown(std::min(wanted.width, width - wanted.left), alignment);
        wanted.height = alignDown(std::min(wanted.height, height - wanted.top), alignment);
        if (wanted.width < capability->minimumWidth || wanted.height < capability->minimumHeight ||
            wanted.width > capability->maximumWidth || wanted.height > capability->maximumHeight)
            throw std::invalid_argument("invalid video crop");
        auto laid = rawLayout(layout, width, height, planes);
        if (!laid) throw std::invalid_argument("unsupported raw video plane layout");
        stream.input = layout;
        stream.frameWidth = width;
        stream.frameHeight = height;
        stream.crop = wanted;
        stream.layout = std::move(*laid);
    }

    /* Output of the stream's encoder wakes the device through the kqueue,
     * whose registration ends with the encoder's descriptor. */
    void watch(std::uint32_t id, Stream& stream)
    {
        struct kevent change {};
        EV_SET(&change, static_cast<std::uintptr_t>(stream.encoder->readinessDescriptor()), EVFILT_READ,
               EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, reinterpret_cast<void*>(static_cast<std::uintptr_t>(id)));
        if (::kevent(queue.get(), &change, 1, nullptr, 0, nullptr) < 0)
            throw std::runtime_error("watching the video encoder failed");
    }
    /* The stream's session is configured from its settled state at its first
     * frame, which is the first moment a V4L2 client can no longer change what
     * it was given. */
    bool ensureStreaming(std::uint32_t id, Stream& stream)
    {
        if (stream.streaming) return true;
        EncodeStream request;
        request.codec = stream.codec;
        request.input = stream.input;
        request.width = stream.crop.width;
        request.height = stream.crop.height;
        request.info = stream.info;
        if (const auto* capability = capabilityOf(stream.codec)) request.maximumBFrames = capability->maximumBFrames;
        const auto settings = encodeSettings(request, stream.controls);
        const auto status = stream.encoder->configure(settings);
        log.encoderStarted(id, codecName(settings.codec), profileName(settings.profile),
                           rateControlName(settings.rateControl), settings.bitrate, settings.width,
                           settings.height, settings.frameRateNumerator, settings.frameRateDenominator,
                           layoutName(settings.input), settings.bFrames, settings.temporalLayers,
                           statusName(status));
        if (status != VideoEncodeStatus::ok) return false;
        stream.streaming = true;
        stream.parameterSetsDelivered = false;
        return true;
    }

    bool copyFrame(const Stream& stream, const Resource& resource, WritableVideoFrame& frame) const
    {
        const auto& components = stream.layout.components;
        if (frame.planeCount() != components.size()) return false;
        for (std::size_t index = 0; index < components.size(); ++index) {
            const auto& component = components[index];
            const auto rows = roundUp(stream.crop.height, component.yShift);
            const auto rowBytes = roundUp(stream.crop.width, component.xShift) * component.sampleBytes;
            const auto destination = frame.plane(index);
            const auto stride = frame.stride(index);
            if (frame.rows(index) < rows || stride < rowBytes ||
                destination.size() < std::size_t(rows - 1) * stride + rowBytes ||
                component.plane >= resource.planes.size())
                return false;
            GuestPlaneReader reader(mapper, regions.get(), resource.planes[component.plane]);
            const std::uint64_t first = component.offset +
                std::uint64_t(stream.crop.top >> component.yShift) * component.stride +
                std::uint64_t(stream.crop.left >> component.xShift) * component.sampleBytes;
            for (std::uint32_t row = 0; row < rows; ++row)
                if (!reader.read(first + std::uint64_t(row) * component.stride,
                                 destination.subspan(std::size_t(row) * stride, rowBytes)))
                    return false;
        }
        return true;
    }

    void completeHeld(std::uint32_t id, const HeldFrame& held, std::uint32_t flags, Stream& stream)
    {
        const auto found = stream.resources.find(resourceKey(VIRTIO_VIDEO_QUEUE_TYPE_INPUT, held.resource));
        if (found != stream.resources.end()) found->second.queued = false;
        completions.push_back({held.request, queued(id, held.timestamp, flags, 0, &held.dataSizes)});
    }

    /* Everything a stream has queued but not encoded goes back as an error,
     * the raw frames waiting for the encoder included, so the client does not
     * wait for buffers that will not return. */
    void cancelHeld(std::uint32_t id, Stream& stream)
    {
        for (const auto& held : stream.held) completeHeld(id, held, VIRTIO_VIDEO_DEQUEUE_FLAG_ERR, stream);
        stream.held.clear();
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

    /* Ends the session: whatever the platform and the device still hold of
     * the coded sequence is dropped, and the next frame starts a new one at a
     * key frame. */
    void restart(std::uint32_t id, Stream& stream)
    {
        stream.encoder->reset();
        stream.coded.clear();
        stream.inFlight = 0;
        stream.streaming = false;
        stream.failed = false;
        stream.drainIssued = false;
        stream.recoverKey = false;
        if (stream.drainRequest) {
            completions.push_back({*stream.drainRequest, reply(id)});
            stream.drainRequest.reset();
        }
    }

    bool takeOutput(std::uint32_t id, Stream& stream)
    {
        if (!stream.encoder || stream.coded.size() >= codedBacklogLimit) return false;
        auto frames = stream.encoder->takeFrames();
        for (auto& frame : frames) {
            switch (frame.kind) {
            case EncodedVideoFrameKind::frame: {
                if (stream.inFlight > 0) --stream.inFlight;
                if (stream.codec != VideoCodec::jpeg && frame.type == VideoFrameType::key) {
                    /* Parameter sets repeat before every key frame only when
                     * the guest asked for that; otherwise they come once at the
                     * start, in the first frame unless a separate buffer
                     * carries them. */
                    const bool repeat = stream.controls.scalars.contains(UML_V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR)
                        ? stream.controls.scalars[UML_V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR] != 0 : true;
                    const bool joined = stream.controls.scalars.contains(UML_V4L2_CID_MPEG_VIDEO_HEADER_MODE)
                        ? stream.controls.scalars[UML_V4L2_CID_MPEG_VIDEO_HEADER_MODE] ==
                            UML_V4L2_MPEG_VIDEO_HEADER_MODE_JOINED_WITH_1ST_FRAME : true;
                    if (!repeat && !(joined && !stream.parameterSetsDelivered))
                        stripParameterSets(stream.codec, frame.bytes);
                    stream.parameterSetsDelivered = true;
                }
                stream.coded.push_back({CodedKind::frame, frame.type, frame.timestamp, std::move(frame.bytes)});
                break;
            }
            case EncodedVideoFrameKind::parameterSets:
                stream.parameterSetsDelivered = true;
                stream.coded.push_back({CodedKind::parameterSets, VideoFrameType::key, frame.timestamp,
                                        std::move(frame.bytes)});
                break;
            case EncodedVideoFrameKind::drained:
                if (!stream.drainIssued) break;
                stream.drainIssued = false;
                stream.coded.push_back({CodedKind::endOfStream, VideoFrameType::key, 0, {}});
                if (stream.drainRequest) {
                    completions.push_back({*stream.drainRequest, reply(id)});
                    stream.drainRequest.reset();
                }
                break;
            case EncodedVideoFrameKind::error:
                if (stream.inFlight > 0) --stream.inFlight;
                stream.coded.push_back({CodedKind::error, VideoFrameType::key, frame.timestamp, {}});
                break;
            }
        }
        return !frames.empty();
    }

    bool pumpCoded(std::uint32_t id, Stream& stream)
    {
        bool progress = false;
        while (!stream.outputs.empty() && !stream.coded.empty()) {
            const auto output = stream.outputs.front();
            stream.outputs.pop_front();
            auto item = std::move(stream.coded.front());
            stream.coded.pop_front();
            auto& resource = stream.resources.at(resourceKey(VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT, output.resource));
            resource.queued = false;
            progress = true;
            std::uint32_t flags = 0;
            std::uint32_t size = 0;
            switch (item.kind) {
            case CodedKind::endOfStream: flags = VIRTIO_VIDEO_DEQUEUE_FLAG_EOS; break;
            case CodedKind::error:
                flags = VIRTIO_VIDEO_DEQUEUE_FLAG_ERR;
                ++stream.errors;
                break;
            case CodedKind::frame:
            case CodedKind::parameterSets:
                if (item.bytes.size() > resource.capacity || resource.planes.empty() ||
                    !transferSegments(mapper, regions.get(), resource.planes[0], item.bytes, true)) {
                    /* The picture did not fit, or its buffer is gone. The
                     * stream goes on from a key frame, since the client has
                     * lost a frame others depend on. */
                    flags = VIRTIO_VIDEO_DEQUEUE_FLAG_ERR;
                    ++stream.errors;
                    stream.recoverKey = true;
                    break;
                }
                size = std::uint32_t(item.bytes.size());
                if (item.kind == CodedKind::parameterSets) {
                    flags = VIRTIO_VIDEO_DEQUEUE_FLAG_CODEC_CONFIG;
                } else {
                    flags = item.type == VideoFrameType::key ? VIRTIO_VIDEO_DEQUEUE_FLAG_KEY_FRAME
                        : item.type == VideoFrameType::predicted ? VIRTIO_VIDEO_DEQUEUE_FLAG_PFRAME
                                                                 : VIRTIO_VIDEO_DEQUEUE_FLAG_BFRAME;
                    ++stream.framesOut;
                    if (item.type == VideoFrameType::key) ++stream.keyFrames;
                    stream.bytesOut += size;
                }
                break;
            }
            completions.push_back({output.request, queued(id, item.timestamp, flags, size)});
        }
        return progress;
    }

    bool submitHeld(std::uint32_t id, Stream& stream)
    {
        bool progress = false;
        while (!stream.held.empty()) {
            const auto held = stream.held.front();
            const auto found = stream.resources.find(resourceKey(VIRTIO_VIDEO_QUEUE_TYPE_INPUT, held.resource));
            if (stream.failed || found == stream.resources.end()) {
                stream.held.pop_front();
                completeHeld(id, held, VIRTIO_VIDEO_DEQUEUE_FLAG_ERR, stream);
                ++stream.errors;
                progress = true;
                continue;
            }
            if (!ensureStreaming(id, stream)) {
                stream.failed = true;
                event(id, VIRTIO_VIDEO_EVENT_ERROR);
                continue;
            }
            auto frame = stream.encoder->acquireFrame();
            if (!frame) {
                if (stream.inFlight != 0) break;
                /* Nothing is in flight, so no output will free a frame: the
                 * encoder cannot take this stream's pictures at all. */
                stream.failed = true;
                event(id, VIRTIO_VIDEO_EVENT_ERROR);
                continue;
            }
            stream.held.pop_front();
            progress = true;
            if (!copyFrame(stream, found->second, *frame)) {
                completeHeld(id, held, VIRTIO_VIDEO_DEQUEUE_FLAG_ERR, stream);
                ++stream.errors;
                continue;
            }
            /* Both requests are consumed by this frame, whichever asked. */
            const bool recover = std::exchange(stream.recoverKey, false);
            stream.encoder->submit(std::move(frame), held.timestamp, held.forceKey || recover);
            ++stream.inFlight;
            ++stream.framesIn;
            completeHeld(id, held, 0, stream);
        }
        return progress;
    }

    void issueDrain(std::uint32_t id, Stream& stream)
    {
        if (!stream.drainRequest || stream.drainIssued || !stream.held.empty()) return;
        if (stream.streaming) {
            stream.drainIssued = true;
            stream.encoder->drain();
            return;
        }
        /* Nothing has been encoded since the stream last started, so there is
         * nothing to wait for. */
        stream.coded.push_back({CodedKind::endOfStream, VideoFrameType::key, 0, {}});
        completions.push_back({*stream.drainRequest, reply(id)});
        stream.drainRequest.reset();
    }

    /* Moves everything that can move: coded frames from the encoder to the
     * guest's buffers, and the raw frames waiting for the encoder into it.
     * Each pass consumes something, so this ends. */
    void serve(std::uint32_t id, Stream& stream)
    {
        bool progress;
        do {
            progress = takeOutput(id, stream);
            progress |= pumpCoded(id, stream);
            progress |= submitHeld(id, stream);
            issueDrain(id, stream);
        } while (progress);
    }

    VhostUserQueueResult done(std::vector<std::uint8_t> data) const
    {
        return {VhostUserQueueAction::complete, std::move(data)};
    }

    VhostUserQueueResult queryControls(std::uint32_t id, std::span<const std::uint8_t> request, std::size_t capacity)
    {
        const auto query = load<virtio_video_query_v4l2_controls>(request);
        const auto& descriptors = controls.descriptors();
        if (query.start_index > descriptors.size()) throw std::invalid_argument("invalid control index");
        const auto room = capacity > sizeof(virtio_video_query_v4l2_controls_resp)
            ? (capacity - sizeof(virtio_video_query_v4l2_controls_resp)) / sizeof(virtio_video_v4l2_control_desc) : 0;
        const auto count = std::min<std::size_t>(descriptors.size() - query.start_index, room);
        if (count == 0 && query.start_index < descriptors.size()) throw std::invalid_argument("control response is too small");
        virtio_video_query_v4l2_controls_resp response {};
        response.hdr = {VIRTIO_VIDEO_RESP_OK_QUERY_V4L2_CONTROLS, id};
        response.num_descs = std::uint32_t(count);
        response.next_index = query.start_index + count < descriptors.size() ? std::uint32_t(query.start_index + count) : 0;
        auto result = toBytes(response);
        for (std::size_t index = 0; index < count; ++index) {
            const auto bytes = toBytes(descriptors[query.start_index + index]);
            result.insert(result.end(), bytes.begin(), bytes.end());
        }
        return done(std::move(result));
    }

    VhostUserQueueResult setControls(std::uint32_t id, Stream& stream, std::span<const std::uint8_t> request)
    {
        const auto header = load<virtio_video_v4l2_controls>(request);
        const std::uint32_t count = header.num_controls;
        if (count == 0 || count > maximumControls ||
            request.size() < sizeof(header) + std::size_t(count) * sizeof(virtio_video_v4l2_control_value))
            throw std::invalid_argument("invalid control count");
        const auto fail = [&](std::uint32_t index, std::uint32_t type) {
            return done(toBytes(virtio_video_v4l2_controls_resp {{type, id}, index, count}));
        };
        const auto area = request.subspan(sizeof(header) + std::size_t(count) * sizeof(virtio_video_v4l2_control_value));
        ControlValues next = stream.controls;
        std::vector<std::uint32_t> changed;
        bool forceKey = false;
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto entry = load<virtio_video_v4l2_control_value>(
                request.subspan(sizeof(header) + std::size_t(index) * sizeof(virtio_video_v4l2_control_value)));
            const auto* descriptor = controls.find(entry.id);
            if (!descriptor) return fail(index, VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL);
            if (descriptor->flags & VIRTIO_VIDEO_V4L2_CTRL_F_READ_ONLY) return fail(index, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER);
            std::vector<std::uint8_t> payload;
            if (descriptor->elem_size == 0 && entry.size != 0) return fail(index, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER);
            if (descriptor->elem_size != 0) {
                if (entry.size != descriptor->elem_size || entry.value % 8 != 0 || entry.value > area.size() ||
                    area.size() - entry.value < entry.size)
                    return fail(index, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER);
                payload.assign(area.begin() + std::ptrdiff_t(entry.value),
                               area.begin() + std::ptrdiff_t(entry.value + entry.size));
            }
            const auto value = static_cast<std::int64_t>(entry.value);
            if (controls.check(entry.id, value, payload.size()) != ControlCheck::ok)
                return fail(index, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER);
            if (stream.streaming && !isDynamic(*descriptor)) return fail(index, VIRTIO_VIDEO_RESP_ERR_BUSY);
            if (descriptor->type == UML_V4L2_CTRL_TYPE_BUTTON) {
                forceKey = true;
            } else if (descriptor->elem_size != 0) {
                next.payloads[entry.id] = std::move(payload);
            } else {
                next.scalars[entry.id] = value;
                if (isDynamic(*descriptor)) changed.push_back(entry.id);
            }
        }
        /* A quantizer range that closes on itself is refused at the control
         * that closed it. */
        for (const auto& [minimum, maximum] : {std::pair {UML_V4L2_CID_MPEG_VIDEO_H264_MIN_QP, UML_V4L2_CID_MPEG_VIDEO_H264_MAX_QP},
                                               std::pair {UML_V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP, UML_V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP}}) {
            if (!next.scalars.contains(minimum) || !next.scalars.contains(maximum) ||
                next.scalars[minimum] <= next.scalars[maximum])
                continue;
            /* The last of the pair in the request is the one that closed it. */
            for (std::uint32_t index = count; index-- > 0;) {
                const auto entryId = load<virtio_video_v4l2_control_value>(
                    request.subspan(sizeof(header) + std::size_t(index) * sizeof(virtio_video_v4l2_control_value))).id;
                if (entryId == minimum || entryId == maximum) return fail(index, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER);
            }
        }
        if (stream.streaming && !changed.empty()) {
            EncodeStream running;
            running.codec = stream.codec;
            running.info = stream.info;
            const auto update = encodeUpdate(running, changed, next);
            if (stream.encoder->update(update) != VideoEncodeStatus::ok)
                return fail(0, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER);
        }
        stream.controls = std::move(next);
        if (forceKey) stream.forceKeyNext = true;
        return done(toBytes(virtio_video_v4l2_controls_resp {{VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS, id}, count, count}));
    }

    VhostUserQueueResult getControls(std::uint32_t id, const Stream& stream, std::span<const std::uint8_t> request,
                                     std::size_t capacity)
    {
        const auto header = load<virtio_video_v4l2_controls>(request);
        const std::uint32_t count = header.num_controls;
        if (count == 0 || count > maximumControls ||
            request.size() < sizeof(header) + std::size_t(count) * sizeof(virtio_video_v4l2_control_value))
            throw std::invalid_argument("invalid control count");
        std::vector<virtio_video_v4l2_control_value> values;
        std::vector<std::uint8_t> area;
        for (std::uint32_t index = 0; index < count; ++index) {
            auto entry = load<virtio_video_v4l2_control_value>(
                request.subspan(sizeof(header) + std::size_t(index) * sizeof(virtio_video_v4l2_control_value)));
            const auto* descriptor = controls.find(entry.id);
            if (!descriptor)
                return done(toBytes(virtio_video_v4l2_controls_resp {{VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL, id},
                                                                     index, count}));
            if (descriptor->elem_size != 0) {
                const auto stored = stream.controls.payloads.find(entry.id);
                std::vector<std::uint8_t> payload = stored == stream.controls.payloads.end()
                    ? std::vector<std::uint8_t>(descriptor->elem_size) : stored->second;
                payload.resize(descriptor->elem_size);
                entry.size = descriptor->elem_size;
                entry.value = area.size();
                area.insert(area.end(), payload.begin(), payload.end());
                area.resize((area.size() + 7) / 8 * 8);
            } else {
                const auto stored = stream.controls.scalars.find(entry.id);
                entry.size = 0;
                entry.value = static_cast<std::uint64_t>(stored == stream.controls.scalars.end() ? 0 : stored->second);
            }
            values.push_back(entry);
        }
        auto result = toBytes(virtio_video_v4l2_controls_resp {{VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS, id}, count, count});
        for (const auto& value : values) {
            const auto bytes = toBytes(value);
            result.insert(result.end(), bytes.begin(), bytes.end());
        }
        result.insert(result.end(), area.begin(), area.end());
        if (result.size() > capacity) throw std::invalid_argument("control response is too small");
        return done(std::move(result));
    }

    Stream makeStream(std::uint32_t coded)
    {
        const auto codec = encodedCodec(coded);
        const auto* capability = codec ? capabilityOf(*codec) : nullptr;
        if (!capability) throw std::invalid_argument("unsupported coded format");
        Stream stream;
        stream.codec = *codec;
        stream.encoder = factory();
        if (!stream.encoder) throw std::runtime_error("no video encoder");
        stream.controls = controls.defaults();
        stream.info = StreamInfo {};
        const auto layouts = advertisedRawLayouts(capabilities);
        const auto layout = std::find_if(layouts.begin(), layouts.end(),
                                         [&](const auto candidate) { return accepts(*capability, candidate); });
        if (layout == layouts.end()) throw std::invalid_argument("codec has no raw input format");
        useRawFormat(stream, *layout,
                     std::min(defaultWidth, capability->maximumWidth), std::min(defaultHeight, capability->maximumHeight),
                     0, nullptr);
        return stream;
    }

    VhostUserQueueResult command(std::uint64_t requestId, std::span<const std::uint8_t> request, std::size_t capacity)
    {
        const auto header = load<virtio_video_cmd_hdr>(request);
        const auto id = header.stream_id;
        if (header.type == VIRTIO_VIDEO_CMD_QUERY_CAPABILITY) {
            const auto q = load<virtio_video_query_capability>(request);
            return done(encoderCapabilityResponse(capabilities, q.queue_type));
        }
        if (header.type == VIRTIO_VIDEO_CMD_QUERY_V4L2_CONTROLS) return queryControls(id, request, capacity);
        if (header.type == VIRTIO_VIDEO_CMD_STREAM_CREATE) {
            const auto q = load<virtio_video_stream_create>(request);
            if (!id || streams.contains(id) || q.in_mem_type || q.out_mem_type)
                return done(reply(id, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER));
            if (streams.size() >= streamLimit) return done(reply(id, VIRTIO_VIDEO_RESP_ERR_OUT_OF_MEMORY));
            auto stream = makeStream(q.coded_format);
            watch(id, stream);
            log.encoderOpened(id, codecName(stream.codec));
            streams.emplace(id, std::move(stream));
            return done(reply(id));
        }
        if (header.type == VIRTIO_VIDEO_CMD_QUERY_CONTROL || header.type == VIRTIO_VIDEO_CMD_GET_CONTROL ||
            header.type == VIRTIO_VIDEO_CMD_SET_CONTROL)
            return done(reply(id, VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL));
        const auto found = streams.find(id);
        if (found == streams.end()) return done(reply(id, VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID));
        auto& stream = found->second;
        switch (header.type) {
        case VIRTIO_VIDEO_CMD_STREAM_DESTROY:
            cancelHeld(id, stream);
            cancelOutputs(id, stream);
            if (stream.drainRequest) completions.push_back({*stream.drainRequest, reply(id)});
            record(id, stream, "destroy");
            streams.erase(found);
            return done(reply(id));
        case VIRTIO_VIDEO_CMD_GET_PARAMS: {
            const auto q = load<virtio_video_get_params>(request);
            if (!validQueue(q.queue_type)) throw std::invalid_argument("invalid parameter queue");
            return done(toBytes(virtio_video_get_params_resp {{VIRTIO_VIDEO_RESP_OK_GET_PARAMS, id},
                q.queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT ? inputParams(stream) : outputParams(stream)}));
        }
        case VIRTIO_VIDEO_CMD_SET_PARAMS: return setParams(id, stream, load<virtio_video_set_params>(request).params);
        case VIRTIO_VIDEO_CMD_SET_STREAM_INFO: {
            const auto q = load<virtio_video_set_stream_info>(request).info;
            if (!validQueue(q.queue_type)) throw std::invalid_argument("invalid stream information queue");
            auto info = sanitized({q.frame_interval_numerator, q.frame_interval_denominator, q.colorspace,
                                   q.xfer_func, q.ycbcr_enc, q.quantization});
            /* A zero interval is no request, and the colorimetry of the coded
             * queue is whatever the raw queue's produces. */
            if (info.intervalNumerator == 0 || info.intervalDenominator == 0) {
                info.intervalNumerator = stream.info.intervalNumerator;
                info.intervalDenominator = stream.info.intervalDenominator;
            }
            if (q.queue_type == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) {
                info.colorspace = stream.info.colorspace;
                info.transfer = stream.info.transfer;
                info.matrix = stream.info.matrix;
                info.quantization = stream.info.quantization;
            }
            if (stream.streaming && (info.colorspace != stream.info.colorspace || info.transfer != stream.info.transfer ||
                                     info.matrix != stream.info.matrix || info.quantization != stream.info.quantization))
                return done(reply(id, VIRTIO_VIDEO_RESP_ERR_BUSY));
            if (stream.streaming && (info.intervalNumerator != stream.info.intervalNumerator ||
                                     info.intervalDenominator != stream.info.intervalDenominator)) {
                uml::platform::VideoEncodeUpdate update;
                update.frameRateNumerator = info.intervalDenominator;
                update.frameRateDenominator = info.intervalNumerator;
                if (stream.encoder->update(update) != VideoEncodeStatus::ok)
                    return done(reply(id, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER));
            }
            stream.info = info;
            return done(reply(id));
        }
        case VIRTIO_VIDEO_CMD_GET_STREAM_INFO: {
            const auto q = load<virtio_video_get_stream_info>(request);
            if (!validQueue(q.queue_type)) throw std::invalid_argument("invalid stream information queue");
            return done(toBytes(virtio_video_get_stream_info_resp {{VIRTIO_VIDEO_RESP_OK_STREAM_INFO, id},
                {q.queue_type, stream.info.intervalNumerator, stream.info.intervalDenominator,
                 stream.info.colorspace, stream.info.transfer, stream.info.matrix, stream.info.quantization, {}}}));
        }
        case VIRTIO_VIDEO_CMD_SET_V4L2_CONTROLS: return setControls(id, stream, request);
        case VIRTIO_VIDEO_CMD_GET_V4L2_CONTROLS: return getControls(id, stream, request, capacity);
        case VIRTIO_VIDEO_CMD_RESOURCE_ATTACH: {
            const auto q = load<virtio_video_resource_attach>(request);
            if (!validQueue(q.queue_type) || !q.resource_id) throw std::invalid_argument("invalid video resource");
            const auto key = resourceKey(q.queue_type, q.resource_id);
            const auto previous = stream.resources.find(key);
            if (previous != stream.resources.end() && previous->second.queued)
                throw std::invalid_argument("video resource is already queued");
            if (previous == stream.resources.end() && stream.resources.size() >= 128)
                throw std::invalid_argument("invalid video resource");
            const bool raw = q.queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
            Resource resource;
            resource.planes = parseScatterLists(request.subspan(sizeof(q)), raw ? stream.layout.planes : 1u,
                                                mapper, regions.get());
            for (const auto& segment : resource.planes[0]) resource.capacity += segment.length;
            if (raw) {
                /* A buffer smaller than the layout the host told the guest
                 * would be read past its end. */
                for (unsigned plane = 0; plane < stream.layout.planes; ++plane) {
                    std::uint64_t total = 0;
                    for (const auto& segment : resource.planes[plane]) total += segment.length;
                    if (total < stream.layout.formats[plane].plane_size)
                        throw std::invalid_argument("video resource is smaller than the raw frame");
                }
            }
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
                if (stream.held.size() >= heldFrameLimit) throw std::invalid_argument("video input queue is full");
                HeldFrame held {requestId, q.resource_id, q.timestamp, {}, std::exchange(stream.forceKeyNext, false)};
                std::copy(std::begin(q.data_sizes), std::end(q.data_sizes), held.dataSizes.begin());
                resource->second.queued = true;
                stream.held.push_back(held);
            } else {
                if (stream.outputs.size() >= maximumBuffers) throw std::invalid_argument("video capture queue is full");
                resource->second.queued = true;
                stream.outputs.push_back({requestId, q.resource_id});
            }
            serve(id, stream);
            return {VhostUserQueueAction::defer, {}};
        }
        case VIRTIO_VIDEO_CMD_STREAM_DRAIN:
            if (stream.drainRequest) throw std::invalid_argument("video drain is already pending");
            stream.drainRequest = requestId;
            serve(id, stream);
            return {VhostUserQueueAction::defer, {}};
        case VIRTIO_VIDEO_CMD_QUEUE_CLEAR:
        case VIRTIO_VIDEO_CMD_QUEUE_DETACH_RESOURCES: {
            const auto q = load<virtio_video_queue_clear>(request);
            if (!validQueue(q.queue_type)) throw std::invalid_argument("invalid clear queue");
            if (q.queue_type == VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT) cancelOutputs(id, stream);
            else cancelHeld(id, stream);
            restart(id, stream);
            if (header.type == VIRTIO_VIDEO_CMD_QUEUE_DETACH_RESOURCES)
                std::erase_if(stream.resources, [&q](const auto& r) { return (r.first >> 32) == q.queue_type; });
            return done(reply(id));
        }
        default: return done(reply(id, VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION));
        }
    }

    VhostUserQueueResult setParams(std::uint32_t id, Stream& stream, const virtio_video_params& p)
    {
        if (!validQueue(p.queue_type)) throw std::invalid_argument("invalid parameter queue");
        if (p.queue_type == VIRTIO_VIDEO_QUEUE_TYPE_INPUT) {
            const auto layout = platformRawLayout(p.format);
            const auto* capability = capabilityOf(stream.codec);
            if (!layout || !accepts(*capability, *layout)) throw std::invalid_argument("unsupported raw video format");
            Stream trial;
            trial.codec = stream.codec;
            useRawFormat(trial, *layout, p.frame_width, p.frame_height, p.num_planes, &p.crop);
            const bool same = trial.input == stream.input && trial.frameWidth == stream.frameWidth &&
                trial.frameHeight == stream.frameHeight && std::memcmp(&trial.crop, &stream.crop, sizeof(trial.crop)) == 0 &&
                trial.layout.planes == stream.layout.planes;
            if (same) return done(reply(id));
            if (stream.streaming) return done(reply(id, VIRTIO_VIDEO_RESP_ERR_BUSY));
            const bool buffers = std::any_of(stream.resources.begin(), stream.resources.end(),
                                             [](const auto& r) { return (r.first >> 32) == VIRTIO_VIDEO_QUEUE_TYPE_INPUT; });
            if (buffers) throw std::invalid_argument("raw format changed with allocated buffers");
            stream.input = trial.input;
            stream.frameWidth = trial.frameWidth;
            stream.frameHeight = trial.frameHeight;
            stream.crop = trial.crop;
            stream.layout = std::move(trial.layout);
            return done(reply(id));
        }
        const auto codec = encodedCodec(p.format);
        if (!codec || !capabilityOf(*codec)) throw std::invalid_argument("unsupported coded video format");
        if (*codec != stream.codec) {
            if (stream.streaming) return done(reply(id, VIRTIO_VIDEO_RESP_ERR_BUSY));
            if (!stream.resources.empty()) throw std::invalid_argument("coded format changed with allocated buffers");
            stream.codec = *codec;
            const auto* capability = capabilityOf(stream.codec);
            if (!accepts(*capability, stream.input) || stream.frameWidth > capability->maximumWidth ||
                stream.frameHeight > capability->maximumHeight) {
                const auto layouts = advertisedRawLayouts(capabilities);
                const auto layout = std::find_if(layouts.begin(), layouts.end(),
                                                 [&](const auto candidate) { return accepts(*capability, candidate); });
                if (layout == layouts.end()) throw std::invalid_argument("codec has no raw input format");
                useRawFormat(stream, *layout, std::min(stream.frameWidth, capability->maximumWidth),
                             std::min(stream.frameHeight, capability->maximumHeight), 0, nullptr);
            }
        }
        return done(reply(id));
    }

    void drainEvents()
    {
        std::array<struct kevent, 8> ready {};
        const timespec none {};
        for (;;) {
            int count;
            do {
                count = ::kevent(queue.get(), nullptr, 0, ready.data(), int(ready.size()), &none);
            } while (count < 0 && errno == EINTR);
            if (count < 0) throw std::runtime_error("draining the video encoder events failed");
            for (int index = 0; index < count; ++index) {
                const auto found = streams.find(static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(ready[std::size_t(index)].udata)));
                if (found != streams.end()) serve(found->first, found->second);
            }
            if (count < int(ready.size())) return;
        }
    }
};

VirtioVideoEncoderDevice::VirtioVideoEncoderDevice(std::vector<VideoEncodeCapability> capabilities,
                                                   EncoderFactory factory)
{
    if (!factory) throw std::invalid_argument("a video encoder factory is required");
    std::erase_if(capabilities, [](const auto& capability) {
        return !encodedWireFormat(capability.codec) || capability.profiles.empty();
    });
    if (capabilities.empty()) throw std::runtime_error("this host has no hardware video encoder");
    implementation_ = std::make_unique<Implementation>(std::move(capabilities), std::move(factory));
}
VirtioVideoEncoderDevice::~VirtioVideoEncoderDevice() = default;
const char* VirtioVideoEncoderDevice::name() const noexcept { return "Virtio Video encoder"; }
std::uint64_t VirtioVideoEncoderDevice::features() const noexcept
{
    const std::uint64_t base = (1u << VIRTIO_VIDEO_F_RESOURCE_GUEST_PAGES) |
        (1u << VIRTIO_VIDEO_F_RESOURCE_NON_CONTIG) | (1u << VIRTIO_VIDEO_F_V4L2_CONTROLS);
    return implementation_->regions ? base | (1u << VIRTIO_VIDEO_F_RESOURCE_SHM_REGION) : base;
}
std::size_t VirtioVideoEncoderDevice::queueCount() const noexcept { return 2; }
std::vector<std::uint8_t> VirtioVideoEncoderDevice::configuration() const
{
    /* The longest answers: a capability query, and a page of control
     * descriptors, which the device splits to fit the buffer the guest gave. */
    return toBytes(virtio_video_config {UML_VIRTIO_VIDEO_VERSION, 16384, 512});
}
void VirtioVideoEncoderDevice::writeConfiguration(std::uint32_t, std::uint32_t, std::span<const std::uint8_t>) {}
void VirtioVideoEncoderDevice::setGuestMapper(GuestMapper mapper) { implementation_->mapper = std::move(mapper); }
void VirtioVideoEncoderDevice::setSharedMemoryRegions(std::shared_ptr<const uml::virtio::SharedMemoryRegions> regions)
{
    implementation_->regions = std::move(regions);
}
VhostUserQueueResult VirtioVideoEncoderDevice::processQueue(std::uint32_t index, std::uint64_t id,
    std::span<const std::uint8_t> request, std::size_t capacity)
{
    auto& impl = *implementation_;
    if (index == 1) {
        if (!request.empty() || capacity < sizeof(virtio_video_event))
            return {VhostUserQueueAction::complete, {}};
        if (!impl.events.empty()) {
            auto result = toBytes(impl.events.front());
            impl.events.pop_front();
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
        auto result = impl.command(id, request, capacity);
        if (result.response.size() > capacity)
            return {VhostUserQueueAction::complete, reply(header.stream_id, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER)};
        return result;
    } catch (const std::invalid_argument&) {
        return {VhostUserQueueAction::complete, reply(header.stream_id, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER)};
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Virtio video encoder stream %u: %s\n", header.stream_id, error.what());
        const auto found = impl.streams.find(header.stream_id);
        if (found != impl.streams.end()) {
            impl.cancelHeld(header.stream_id, found->second);
            impl.cancelOutputs(header.stream_id, found->second);
            impl.event(header.stream_id, VIRTIO_VIDEO_EVENT_ERROR);
        }
        if (header.type == VIRTIO_VIDEO_CMD_RESOURCE_QUEUE)
            return {VhostUserQueueAction::complete, impl.queued(header.stream_id, 0, VIRTIO_VIDEO_DEQUEUE_FLAG_ERR)};
        return {VhostUserQueueAction::complete, reply(header.stream_id, VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION)};
    }
}
bool VirtioVideoEncoderDevice::takeQueueCompletion(VhostUserQueueCompletion& completion)
{
    auto& pending = implementation_->completions;
    if (pending.empty()) return false;
    completion = std::move(pending.front());
    pending.pop_front();
    return true;
}
void VirtioVideoEncoderDevice::reset() noexcept
{
    auto& impl = *implementation_;
    impl.closeAll("reset");
    impl.streams.clear();
    impl.events.clear();
    impl.eventBuffers.clear();
    impl.completions.clear();
}
int VirtioVideoEncoderDevice::eventDescriptor() const noexcept { return implementation_->queue.get(); }
void VirtioVideoEncoderDevice::drainEventSource() { implementation_->drainEvents(); }

} // namespace uml::video
