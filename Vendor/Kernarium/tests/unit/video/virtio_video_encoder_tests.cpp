// SPDX-License-Identifier: GPL-2.0-or-later
// The Virtio video encoder device driven the way the guest driver drives it,
// over its two queues, in front of a synthetic platform encoder. The platform
// encoder itself has its own test; here every case is about what the device
// does with the guest's commands, buffers and controls: how it answers, what
// it hands the encoder, and what it writes back.

#include "encoder_capabilities.hpp"
#include "encoder_controls.hpp"
#include "encoder_test_capabilities.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"
#include "shared_memory_regions.hpp"
#include "synthetic_encoder.hpp"
#include "virtio_video_encoder_device.hpp"
#include "wire_bytes.hpp"

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace uml::platform;
using namespace uml::video;
using namespace uml::virtio;
using uml::testing::SyntheticEncoder;
using uml::testing::append;
using uml::testing::encoderHostCapabilities;
using uml::testing::load;
using uml::testing::require;

namespace {

constexpr std::size_t page = 4096;
constexpr auto input = VIRTIO_VIDEO_QUEUE_TYPE_INPUT;
constexpr auto output = VIRTIO_VIDEO_QUEUE_TYPE_OUTPUT;
using Planes = std::vector<std::vector<virtio_video_resource_sg_entry>>;

/* The 64x64 NV12 frames most cases feed. Raw resource n keeps its luma and
 * chroma on pages of its own; coded resource k is split across the end of one
 * page and the start of the next, so the device has to scatter its output. */
constexpr std::uint32_t frameSize = 64;
constexpr std::size_t lumaBytes = std::size_t(frameSize) * frameSize;
constexpr std::size_t chromaBytes = lumaBytes / 2;
std::size_t lumaBase(unsigned n) { return (8 + 2 * n) * page; }
std::size_t chromaBase(unsigned n) { return (9 + 2 * n) * page; }
constexpr unsigned codedFirst = 20;      // resource ids of the coded buffers
constexpr std::size_t codedHead = 96;    // bytes of a coded buffer on its first page
constexpr std::size_t codedBytes = page; // capacity of a coded buffer
std::size_t codedBase(unsigned k) { return (30 + 3 * k) * page; }

std::uint8_t lumaPattern(unsigned seed, std::size_t row, std::size_t column)
{
    return std::uint8_t(0x20 + (row * 5 + column * 3 + seed * 11) % 0xc0);
}
std::uint8_t chromaPattern(unsigned seed, std::size_t row, std::size_t byte)
{
    return std::uint8_t(0x30 + (row * 7 + byte * 13 + seed) % 0x90);
}

/* A device for one case, with `memory` as the guest RAM behind its mapper.
 * `setup` runs on every encoder the device creates, before the device sees it,
 * and `encoders` lists them in creation order for the case to steer. */
struct Fixture final {
    /* With `platform`, the device encodes on the real platform encoder and
     * `setup` and `encoders` are unused. */
    explicit Fixture(std::function<void(SyntheticEncoder&)> configure = {},
                     std::vector<VideoEncodeCapability> capabilities = encoderHostCapabilities(),
                     std::size_t memorySize = 128 * page,
                     std::function<std::unique_ptr<VideoEncoder>()> platform = {})
        : setup(std::move(configure)),
          device(std::move(capabilities), [this, platform = std::move(platform)]() -> std::unique_ptr<VideoEncoder> {
              if (platform) return platform();
              auto encoder = std::make_unique<SyntheticEncoder>();
              if (setup) setup(*encoder);
              encoders.push_back(encoder.get());
              return encoder;
          }),
          memory(memorySize, 0xcc)
    {
        device.setGuestMapper([this](std::uint64_t offset, std::size_t size) {
            if (offset > memory.size() || size > memory.size() - offset) return std::span<std::uint8_t> {};
            return std::span(memory).subspan(offset, size);
        });
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    std::function<void(SyntheticEncoder&)> setup;
    std::vector<SyntheticEncoder*> encoders;
    VirtioVideoEncoderDevice device;
    std::vector<std::uint8_t> memory;
    std::uint64_t next {1};

    SyntheticEncoder& encoder(std::size_t index = 0) { return *encoders.at(index); }

    template<class T> VhostUserQueueResult send(const T& value, std::size_t capacity = 16384)
    {
        std::vector<std::uint8_t> request;
        append(request, value);
        return device.processQueue(0, next++, request, capacity);
    }
    static std::uint32_t type(const VhostUserQueueResult& result) { return load<virtio_video_cmd_hdr>(result.response).type; }
    template<class T> void ok(const T& value, std::source_location where = std::source_location::current())
    {
        const auto result = send(value);
        require(result.action == VhostUserQueueAction::complete && type(result) == VIRTIO_VIDEO_RESP_OK_NODATA,
                "command failed", where);
    }
    template<class T> void refused(const T& value, std::uint32_t expected, const char* what,
                                   std::source_location where = std::source_location::current())
    {
        const auto result = send(value);
        require(result.action == VhostUserQueueAction::complete && type(result) == expected, what, where);
    }

    void create(std::uint32_t stream, std::uint32_t coded = VIRTIO_VIDEO_FORMAT_H264,
                std::source_location where = std::source_location::current())
    {
        virtio_video_stream_create command {};
        command.hdr = {VIRTIO_VIDEO_CMD_STREAM_CREATE, stream};
        command.coded_format = coded;
        ok(command, where);
    }
    void destroy(std::uint32_t stream) { ok(virtio_video_stream_destroy {{VIRTIO_VIDEO_CMD_STREAM_DESTROY, stream}}); }

    VhostUserQueueResult setInput(std::uint32_t stream, std::uint32_t format, std::uint32_t width,
                                  std::uint32_t height, std::uint32_t planes = 0, virtio_video_crop crop = {})
    {
        virtio_video_set_params set {};
        set.hdr = {VIRTIO_VIDEO_CMD_SET_PARAMS, stream};
        set.params.queue_type = input;
        set.params.format = format;
        set.params.frame_width = width;
        set.params.frame_height = height;
        set.params.num_planes = planes;
        set.params.crop = crop;
        return send(set);
    }
    VhostUserQueueResult setOutput(std::uint32_t stream, std::uint32_t format)
    {
        virtio_video_set_params set {};
        set.hdr = {VIRTIO_VIDEO_CMD_SET_PARAMS, stream};
        set.params.queue_type = output;
        set.params.format = format;
        return send(set);
    }
    virtio_video_params params(std::uint32_t stream, std::uint32_t queue,
                               std::source_location where = std::source_location::current())
    {
        const auto result = send(virtio_video_get_params {{VIRTIO_VIDEO_CMD_GET_PARAMS, stream}, queue, {}});
        require(type(result) == VIRTIO_VIDEO_RESP_OK_GET_PARAMS, "GET_PARAMS failed", where);
        return load<virtio_video_get_params_resp>(result.response).params;
    }

    VhostUserQueueResult attach(std::uint32_t stream, std::uint32_t queue, std::uint32_t id, const Planes& planes)
    {
        std::vector<std::uint8_t> request;
        append(request, virtio_video_resource_attach {VIRTIO_VIDEO_CMD_RESOURCE_ATTACH, stream, queue, id});
        for (const auto& plane : planes) {
            append(request, std::uint32_t(plane.size()));
            append(request, std::uint32_t(0));
            for (const auto& segment : plane) append(request, segment);
        }
        return device.processQueue(0, next++, request, 16384);
    }
    /* Raw resource `n`: luma and chroma planes in their own buffers, filled
     * with a pattern that differs along both axes and between resources. */
    void attachRaw(std::uint32_t stream, unsigned n, std::source_location where = std::source_location::current())
    {
        for (std::size_t row = 0; row < frameSize; ++row)
            for (std::size_t column = 0; column < frameSize; ++column)
                memory[lumaBase(n) + row * frameSize + column] = lumaPattern(n, row, column);
        for (std::size_t row = 0; row < frameSize / 2; ++row)
            for (std::size_t byte = 0; byte < frameSize; ++byte)
                memory[chromaBase(n) + row * frameSize + byte] = chromaPattern(n, row, byte);
        require(type(attach(stream, input, n, {{{lumaBase(n), std::uint32_t(lumaBytes), 0}},
                                              {{chromaBase(n), std::uint32_t(chromaBytes), 0}}})) ==
                VIRTIO_VIDEO_RESP_OK_NODATA, "raw attach failed", where);
    }
    /* Coded resource `k`: one plane of two segments. */
    VhostUserQueueResult attachCoded(std::uint32_t stream, unsigned k, std::uint32_t head = codedHead)
    {
        return attach(stream, output, codedFirst + k,
                      {{{codedBase(k) + page - head, head, 0}, {codedBase(k) + page, std::uint32_t(codedBytes - head), 0}}});
    }
    void attachCodedOk(std::uint32_t stream, unsigned k, std::source_location where = std::source_location::current())
    {
        require(type(attachCoded(stream, k)) == VIRTIO_VIDEO_RESP_OK_NODATA, "coded attach failed", where);
    }
    std::vector<std::uint8_t> codedOutput(unsigned k, std::size_t size) const
    {
        std::vector<std::uint8_t> bytes;
        const auto first = std::min(size, codedHead);
        bytes.insert(bytes.end(), memory.begin() + std::ptrdiff_t(codedBase(k) + page - codedHead),
                     memory.begin() + std::ptrdiff_t(codedBase(k) + page - codedHead + first));
        if (size > first)
            bytes.insert(bytes.end(), memory.begin() + std::ptrdiff_t(codedBase(k) + page),
                         memory.begin() + std::ptrdiff_t(codedBase(k) + page + (size - first)));
        return bytes;
    }

    virtio_video_resource_queue queueCommand(std::uint32_t stream, std::uint32_t queue, std::uint32_t id,
                                             std::uint64_t timestamp = 0) const
    {
        virtio_video_resource_queue command {};
        command.cmd_type = VIRTIO_VIDEO_CMD_RESOURCE_QUEUE;
        command.stream_id = stream;
        command.queue_type = queue;
        command.resource_id = id;
        command.timestamp = timestamp;
        if (queue == input) {
            command.data_sizes[0] = std::uint32_t(lumaBytes);
            command.data_sizes[1] = std::uint32_t(chromaBytes);
        }
        return command;
    }
    /* Queues a resource and returns the request identifier a completion will
     * carry; the command itself must have been deferred. */
    std::uint64_t queue(std::uint32_t stream, std::uint32_t queueType, std::uint32_t id, std::uint64_t timestamp = 0,
                        std::source_location where = std::source_location::current())
    {
        const auto request = next;
        require(send(queueCommand(stream, queueType, id, timestamp)).action == VhostUserQueueAction::defer,
                "a queued buffer was not deferred", where);
        return request;
    }
    std::uint64_t queueRaw(std::uint32_t stream, unsigned n, std::uint64_t timestamp,
                           std::source_location where = std::source_location::current())
    {
        return queue(stream, input, n, timestamp, where);
    }
    std::uint64_t queueCoded(std::uint32_t stream, unsigned k, std::source_location where = std::source_location::current())
    {
        return queue(stream, output, codedFirst + k, 0, where);
    }

    struct Completion {
        std::uint64_t request {};
        std::vector<std::uint8_t> response;
        virtio_video_resource_queue_resp buffer() const { return load<virtio_video_resource_queue_resp>(response); }
        std::uint32_t responseType() const { return load<virtio_video_cmd_hdr>(response).type; }
    };
    std::vector<Completion> completions()
    {
        std::vector<Completion> result;
        VhostUserQueueCompletion completion;
        while (device.takeQueueCompletion(completion)) result.push_back({completion.requestId, completion.response});
        return result;
    }
    /* The completion of `request`, which must be the only one in `done`. */
    static const Completion& completionOf(const std::vector<Completion>& done, std::uint64_t request,
                                          std::source_location where = std::source_location::current())
    {
        const auto matches = std::count_if(done.begin(), done.end(), [request](const auto& c) { return c.request == request; });
        require(matches == 1, "a request did not complete exactly once", where);
        return *std::find_if(done.begin(), done.end(), [request](const auto& c) { return c.request == request; });
    }
    static bool completed(const std::vector<Completion>& done, std::uint64_t request)
    {
        return std::any_of(done.begin(), done.end(), [request](const auto& c) { return c.request == request; });
    }

    /* The device's wake-up, as the transport uses it: the descriptor must
     * become readable within a bound, and draining it must leave it quiet. */
    bool readable(int milliseconds)
    {
        pollfd descriptor {device.eventDescriptor(), POLLIN, 0};
        return ::poll(&descriptor, 1, milliseconds) == 1 && (descriptor.revents & POLLIN);
    }
    void wake(std::source_location where = std::source_location::current())
    {
        require(readable(2000), "the encoder's output did not wake the device", where);
        device.drainEventSource();
        require(!readable(0), "draining the wake-up left the descriptor readable", where);
    }

    /* The planes the encoder was handed for raw resource `n`, against the
     * guest's own bytes. The pool pads every row, so a copy at the wrong
     * stride, or one that writes into the padding, fails here. */
    void requireFrame(const SyntheticEncoder::Submitted& frame, std::size_t luma, std::size_t chroma,
                      std::uint32_t guestStride, std::uint32_t left, std::uint32_t top, std::uint32_t width,
                      std::uint32_t height, const char* what, std::source_location where = std::source_location::current())
    {
        require(frame.planes.size() == 2, what, where);
        const auto rowsOf = [&](std::size_t plane, std::size_t rows, std::size_t rowBytes, std::size_t source) {
            const auto stride = frame.strides[plane];
            require(stride == rowBytes + SyntheticEncoder::paddingBytes && frame.rows[plane] == rows, what, where);
            for (std::size_t row = 0; row < rows; ++row) {
                require(std::memcmp(&frame.planes[plane][row * stride], &memory[source + row * guestStride], rowBytes) == 0,
                        what, where);
                for (std::size_t pad = rowBytes; pad < stride; ++pad)
                    require(frame.planes[plane][row * stride + pad] == SyntheticEncoder::paddingByte, what, where);
            }
        };
        rowsOf(0, height, width, luma + std::size_t(top) * guestStride + left);
        rowsOf(1, (height + 1) / 2, 2 * ((width + 1) / 2), chroma + std::size_t(top / 2) * guestStride + (left / 2) * 2);
    }
};

using Unit = SyntheticEncoder;
std::vector<std::uint8_t> sansParameterSets(std::vector<std::uint8_t> bytes, VideoCodec codec)
{
    bytes.erase(bytes.begin(), bytes.begin() + std::ptrdiff_t(Unit::parameterSets(codec).size()));
    return bytes;
}

/* Everything a stream needs to encode 64x64 NV12: raw resources 1..3, coded
 * resources 0..3, and the format set. */
void prepare(Fixture& f, std::uint32_t stream = 1, std::uint32_t coded = VIRTIO_VIDEO_FORMAT_H264,
             std::source_location where = std::source_location::current())
{
    f.create(stream, coded, where);
    require(Fixture::type(f.setInput(stream, VIRTIO_VIDEO_FORMAT_NV12, frameSize, frameSize)) ==
            VIRTIO_VIDEO_RESP_OK_NODATA, "raw format refused", where);
    for (unsigned n = 1; n <= 3; ++n) f.attachRaw(stream, n, where);
    for (unsigned k = 0; k < 4; ++k) f.attachCodedOk(stream, k, where);
}

virtio_video_v4l2_controls_resp controlsResponse(const VhostUserQueueResult& result)
{
    return load<virtio_video_v4l2_controls_resp>(result.response);
}

/* A SET_V4L2_CONTROLS request: scalars by value, compound controls by
 * payload, laid out as the wire says (values, then an 8-aligned payload area). */
struct ControlSet {
    struct Entry {
        std::uint32_t id;
        std::uint64_t value;
        std::vector<std::uint8_t> payload;
    };
    std::vector<Entry> entries;
    ControlSet& scalar(std::uint32_t id, std::int64_t value)
    {
        entries.push_back({id, static_cast<std::uint64_t>(value), {}});
        return *this;
    }
    ControlSet& compound(std::uint32_t id, std::vector<std::uint8_t> payload)
    {
        entries.push_back({id, 0, std::move(payload)});
        return *this;
    }
    std::vector<std::uint8_t> bytes(std::uint32_t command, std::uint32_t stream) const
    {
        std::vector<std::uint8_t> request;
        append(request, virtio_video_v4l2_controls {{command, stream}, std::uint32_t(entries.size()), {}});
        std::vector<std::uint8_t> area;
        for (const auto& entry : entries) {
            virtio_video_v4l2_control_value value {entry.id, 0, entry.value};
            if (!entry.payload.empty()) {
                value.size = std::uint32_t(entry.payload.size());
                value.value = area.size();
                area.insert(area.end(), entry.payload.begin(), entry.payload.end());
                area.resize((area.size() + 7) / 8 * 8);
            }
            append(request, value);
        }
        request.insert(request.end(), area.begin(), area.end());
        return request;
    }
};
VhostUserQueueResult setControls(Fixture& f, std::uint32_t stream, const ControlSet& set, std::size_t capacity = 16384)
{
    return f.device.processQueue(0, f.next++, set.bytes(VIRTIO_VIDEO_CMD_SET_V4L2_CONTROLS, stream), capacity);
}
/* GET_V4L2_CONTROLS: the scalars by id, and the compound payloads by id. */
struct Fetched {
    std::uint32_t type {};
    std::uint32_t errorIndex {};
    std::map<std::uint32_t, std::uint64_t> scalars;
    std::map<std::uint32_t, std::vector<std::uint8_t>> payloads;
};
Fetched getControls(Fixture& f, std::uint32_t stream, const std::vector<std::uint32_t>& ids, std::size_t capacity = 16384)
{
    ControlSet request;
    for (const auto id : ids) request.scalar(id, 0);
    const auto result = f.device.processQueue(0, f.next++, request.bytes(VIRTIO_VIDEO_CMD_GET_V4L2_CONTROLS, stream), capacity);
    Fetched fetched;
    fetched.type = Fixture::type(result);
    if (fetched.type != VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS) {
        if (result.response.size() >= sizeof(virtio_video_v4l2_controls_resp))
            fetched.errorIndex = controlsResponse(result).error_index;
        return fetched;
    }
    const auto header = controlsResponse(result);
    require(header.error_index == ids.size() && header.num_controls == ids.size(), "GET reported a failure it did not return");
    const std::size_t area = sizeof(header) + ids.size() * sizeof(virtio_video_v4l2_control_value);
    for (std::size_t index = 0; index < ids.size(); ++index) {
        const auto value = load<virtio_video_v4l2_control_value>(result.response, sizeof(header) + index * sizeof(virtio_video_v4l2_control_value));
        require(value.id == ids[index], "GET answered controls out of order");
        if (value.size == 0) {
            fetched.scalars[value.id] = value.value;
        } else {
            require(value.value % 8 == 0 && area + value.value + value.size <= result.response.size(), "payload outside the area");
            fetched.payloads[value.id].assign(result.response.begin() + std::ptrdiff_t(area + value.value),
                                              result.response.begin() + std::ptrdiff_t(area + value.value + value.size));
        }
    }
    return fetched;
}

void identity()
{
    Fixture f;
    require(std::string(f.device.name()).find("encoder") != std::string::npos, "the device name does not say encoder");
    require(f.device.queueCount() == 2, "an encoder has a command queue and an event queue");
    const auto features = f.device.features();
    require((features & (1ull << VIRTIO_VIDEO_F_RESOURCE_GUEST_PAGES)) && (features & (1ull << VIRTIO_VIDEO_F_RESOURCE_NON_CONTIG)) &&
            (features & (1ull << VIRTIO_VIDEO_F_V4L2_CONTROLS)), "an encoder always offers guest pages and the control commands");
    require(!(features & (1ull << VIRTIO_VIDEO_F_VENDOR)) && !(features & (1ull << VIRTIO_VIDEO_F_RESOURCE_SHM_REGION)),
            "shared-memory buffers were offered without a region index");
    f.device.setSharedMemoryRegions(std::make_shared<SharedMemoryRegions>());
    require(f.device.features() & (1ull << VIRTIO_VIDEO_F_RESOURCE_SHM_REGION), "shared-memory buffers were not offered with an index");
    const auto config = load<virtio_video_config>(f.device.configuration());
    require(config.version == UML_VIRTIO_VIDEO_VERSION && config.max_caps_length >= 4096 && config.max_resp_length >= 512,
            "the configuration is smaller than the capability response");
    require(f.device.eventDescriptor() >= 0, "the wake-up descriptor is missing");
    require(encoderDeviceId == 30, "the encoder is device 30");

    uml::testing::requireThrows<std::runtime_error>([] {
        VirtioVideoEncoderDevice device({}, [] { return std::unique_ptr<VideoEncoder> {}; });
    }, "a host without an encoder started the device");
    auto vp9 = encoderHostCapabilities().front();
    vp9.codec = VideoCodec::vp9;
    uml::testing::requireThrows<std::runtime_error>([&vp9] {
        VirtioVideoEncoderDevice device({vp9}, [] { return std::unique_ptr<VideoEncoder> {}; });
    }, "a host with only a codec the wire cannot name started the device");
    uml::testing::requireThrows<std::invalid_argument>([] {
        VirtioVideoEncoderDevice device(encoderHostCapabilities(), {});
    }, "the device started without an encoder factory");
    // A codec without a profile is dropped, not advertised.
    auto none = encoderHostCapabilities();
    none[0].profiles.clear();
    VirtioVideoEncoderDevice partial(none, [] { return std::unique_ptr<VideoEncoder> {}; });
    (void)partial;
}

void queries()
{
    Fixture f;
    const auto capabilities = encoderHostCapabilities();
    for (const auto queue : {input, output}) {
        const auto result = f.send(virtio_video_query_capability {{VIRTIO_VIDEO_CMD_QUERY_CAPABILITY, 0}, queue, {}});
        require(Fixture::type(result) == VIRTIO_VIDEO_RESP_OK_QUERY_CAPABILITY &&
                result.response == encoderCapabilityResponse(capabilities, queue), "capability response");
    }
    f.refused(virtio_video_query_capability {{VIRTIO_VIDEO_CMD_QUERY_CAPABILITY, 0}, 0, {}},
              VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a capability query for no queue was answered");

    const EncoderControls controls(capabilities);
    const auto& all = controls.descriptors();
    const auto asBytes = [](const virtio_video_v4l2_control_desc& desc) {
        std::vector<std::uint8_t> bytes;
        append(bytes, desc);
        return bytes;
    };
    // One response carries the whole table when the buffer is big enough.
    auto result = f.send(virtio_video_query_v4l2_controls {{VIRTIO_VIDEO_CMD_QUERY_V4L2_CONTROLS, 0}, 0, {}});
    require(Fixture::type(result) == VIRTIO_VIDEO_RESP_OK_QUERY_V4L2_CONTROLS, "control query type");
    auto header = load<virtio_video_query_v4l2_controls_resp>(result.response);
    require(header.num_descs == all.size() && header.next_index == 0, "the whole table in one response");
    require(result.response.size() == sizeof(header) + all.size() * sizeof(virtio_video_v4l2_control_desc), "response length");
    for (std::size_t index = 0; index < all.size(); ++index)
        require(std::memcmp(result.response.data() + sizeof(header) + index * sizeof(all[index]), asBytes(all[index]).data(),
                            sizeof(all[index])) == 0, "a descriptor changed on the wire");
    // A small buffer pages the table; the pages concatenate to the whole.
    std::size_t start = 0, pages = 0;
    while (true) {
        result = f.send(virtio_video_query_v4l2_controls {{VIRTIO_VIDEO_CMD_QUERY_V4L2_CONTROLS, 0}, std::uint32_t(start), {}},
                        sizeof(header) + 5 * sizeof(virtio_video_v4l2_control_desc) + 17);
        require(Fixture::type(result) == VIRTIO_VIDEO_RESP_OK_QUERY_V4L2_CONTROLS, "paged control query");
        header = load<virtio_video_query_v4l2_controls_resp>(result.response);
        require(header.num_descs == std::min<std::size_t>(5, all.size() - start), "a page holds as many as fit");
        for (std::uint32_t index = 0; index < header.num_descs; ++index)
            require(std::memcmp(result.response.data() + sizeof(header) + index * sizeof(virtio_video_v4l2_control_desc),
                                asBytes(all[start + index]).data(), sizeof(virtio_video_v4l2_control_desc)) == 0,
                    "a paged descriptor changed");
        ++pages;
        start += header.num_descs;
        if (header.next_index == 0) break;
        require(header.next_index == start, "the next page starts where this one ended");
    }
    require(start == all.size() && pages == (all.size() + 4) / 5, "the pages did not cover the table");
    // Past the end is an empty, final page; beyond that and too small a buffer are refused.
    result = f.send(virtio_video_query_v4l2_controls {{VIRTIO_VIDEO_CMD_QUERY_V4L2_CONTROLS, 0}, std::uint32_t(all.size()), {}});
    header = load<virtio_video_query_v4l2_controls_resp>(result.response);
    require(header.num_descs == 0 && header.next_index == 0, "the page after the last is empty");
    f.refused(virtio_video_query_v4l2_controls {{VIRTIO_VIDEO_CMD_QUERY_V4L2_CONTROLS, 0}, std::uint32_t(all.size() + 1), {}},
              VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a query beyond the table was answered");
    require(Fixture::type(f.send(virtio_video_query_v4l2_controls {{VIRTIO_VIDEO_CMD_QUERY_V4L2_CONTROLS, 0}, 0, {}},
                                 sizeof(header) + 10)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "a buffer that cannot hold one descriptor was answered");
    // The legacy per-control commands belong to the decoder.
    f.refused(virtio_video_get_control {{VIRTIO_VIDEO_CMD_GET_CONTROL, 1}, VIRTIO_VIDEO_CONTROL_LEVEL, {}},
              VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL, "the encoder answered a decoder control");
    // Nothing yet created an encoder.
    require(f.encoders.empty(), "a query created an encoder");
}

void streams()
{
    Fixture f;
    f.create(5);
    require(f.encoders.size() == 1 && f.encoder().configures == 0, "creating a stream configured the encoder before a frame");
    virtio_video_stream_create again {};
    again.hdr = {VIRTIO_VIDEO_CMD_STREAM_CREATE, 5};
    again.coded_format = VIRTIO_VIDEO_FORMAT_H264;
    f.refused(again, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a duplicate stream was created");
    again.hdr.stream_id = 0;
    f.refused(again, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "stream 0 was created");
    again.hdr.stream_id = 6;
    again.coded_format = VIRTIO_VIDEO_FORMAT_VP9;
    f.refused(again, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a VP9 encoder stream was created");
    again.coded_format = VIRTIO_VIDEO_FORMAT_NV12;
    f.refused(again, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a raw format was taken for a coded one");
    again.coded_format = VIRTIO_VIDEO_FORMAT_H264;
    again.in_mem_type = 1;
    f.refused(again, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a memory type the device lacks was accepted");
    require(f.encoders.size() == 1, "a refused stream created an encoder");

    f.refused(virtio_video_get_params {{VIRTIO_VIDEO_CMD_GET_PARAMS, 99}, input, {}},
              VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID, "an unknown stream answered");
    f.refused(virtio_video_stream_drain {{VIRTIO_VIDEO_CMD_STREAM_DRAIN, 99}}, VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID,
              "an unknown stream drained");
    f.refused(virtio_video_stream_destroy {{VIRTIO_VIDEO_CMD_STREAM_DESTROY, 99}}, VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID,
              "an unknown stream was destroyed");
    f.refused(virtio_video_cmd_hdr {0x1ff, 5}, VIRTIO_VIDEO_RESP_ERR_INVALID_OPERATION, "an unknown command was accepted");
    f.refused(virtio_video_cmd_hdr {0x1ff, 77}, VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID, "an unknown command on an unknown stream");
    // A truncated command is dropped with an empty answer, as the decoder's is.
    const auto truncated = f.device.processQueue(0, f.next++, std::vector<std::uint8_t>(4, 0), 16384);
    require(truncated.action == VhostUserQueueAction::complete && truncated.response.empty(), "a truncated command was answered");
    require(f.device.processQueue(0, f.next++, std::vector<std::uint8_t>(4, 0), 4).response.empty(), "a tiny response buffer");

    f.destroy(5);
    f.refused(virtio_video_get_params {{VIRTIO_VIDEO_CMD_GET_PARAMS, 5}, input, {}}, VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID,
              "a destroyed stream answered");
    f.create(5);  // the identifier is free again, with a fresh encoder
    require(f.encoders.size() == 2, "re-creating a stream did not make a new encoder");

    // The host's own session limit bounds the streams.
    auto limited = encoderHostCapabilities();
    for (auto& capability : limited) capability.concurrentSessions = 2;
    Fixture capped({}, limited);
    capped.create(1);
    capped.create(2);
    virtio_video_stream_create third {};
    third.hdr = {VIRTIO_VIDEO_CMD_STREAM_CREATE, 3};
    third.coded_format = VIRTIO_VIDEO_FORMAT_HEVC;
    capped.refused(third, VIRTIO_VIDEO_RESP_ERR_OUT_OF_MEMORY, "streams beyond the host's session limit were created");
    capped.destroy(1);
    capped.ok(third);
    // No limit reported means the device's own cap of eight.
    auto unlimited = encoderHostCapabilities();
    for (auto& capability : unlimited) capability.concurrentSessions = 0;
    Fixture many({}, unlimited);
    for (std::uint32_t stream = 1; stream <= 8; ++stream) many.create(stream);
    third.hdr.stream_id = 9;
    many.refused(third, VIRTIO_VIDEO_RESP_ERR_OUT_OF_MEMORY, "a ninth stream was created");
}

void parameters()
{
    Fixture f;
    f.create(1);
    // Defaults: the first raw format the codec takes, a full-size frame, one coded plane.
    auto raw = f.params(1, input);
    require(raw.format == VIRTIO_VIDEO_FORMAT_NV12 && raw.frame_width == 1280 && raw.frame_height == 720, "default raw format");
    require(raw.crop.left == 0 && raw.crop.top == 0 && raw.crop.width == 1280 && raw.crop.height == 720, "default crop");
    require(raw.num_planes == 2 && raw.plane_formats[0].plane_size == 1280u * 720 && raw.plane_formats[0].stride == 1280 &&
            raw.plane_formats[1].plane_size == 1280u * 360 && raw.plane_formats[1].stride == 1280, "default NV12 planes");
    require(raw.frame_rate == 30, "default frame rate");
    auto coded = f.params(1, output);
    require(coded.format == VIRTIO_VIDEO_FORMAT_H264 && coded.frame_width == 1280 && coded.frame_height == 720 &&
            coded.num_planes == 1 && coded.plane_formats[0].plane_size >= 1u << 20, "default coded format");
    f.refused(virtio_video_get_params {{VIRTIO_VIDEO_CMD_GET_PARAMS, 1}, 0, {}}, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
              "parameters of no queue were returned");

    // I420, as a buffer per plane and as one buffer.
    require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_YUV420, 64, 64)) == VIRTIO_VIDEO_RESP_OK_NODATA, "I420");
    raw = f.params(1, input);
    require(raw.format == VIRTIO_VIDEO_FORMAT_YUV420 && raw.num_planes == 3 && raw.plane_formats[0].plane_size == 4096 &&
            raw.plane_formats[1].plane_size == 1024 && raw.plane_formats[1].stride == 32 &&
            raw.plane_formats[2].plane_size == 1024, "I420 planes");
    require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_YUV420, 64, 64, 1)) == VIRTIO_VIDEO_RESP_OK_NODATA, "I420 in one buffer");
    raw = f.params(1, input);
    require(raw.num_planes == 1 && raw.plane_formats[0].plane_size == 6144 && raw.plane_formats[0].stride == 64,
            "I420 in one buffer is one plane of the whole frame");
    require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_YUV420, 64, 64, 2)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "a plane count between one and the planes of the format was accepted");
    // Odd sizes keep their luma rows and round the chroma up; the crop is rounded down to the alignment.
    require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_NV12, 99, 77, 0)) == VIRTIO_VIDEO_RESP_OK_NODATA, "odd size");
    raw = f.params(1, input);
    require(raw.frame_width == 99 && raw.frame_height == 77 && raw.crop.width == 98 && raw.crop.height == 76 &&
            raw.plane_formats[0].plane_size == 112u * 77 && raw.plane_formats[0].stride == 112 &&
            raw.plane_formats[1].plane_size == 112u * 39 && raw.plane_formats[1].stride == 112, "odd NV12 planes");
    require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_NV12, 100, 80, 0)) == VIRTIO_VIDEO_RESP_OK_NODATA, "even size");
    raw = f.params(1, input);
    require(raw.plane_formats[1].plane_size == 112u * 40 && raw.plane_formats[1].stride == 112, "NV12 chroma plane");
    // Rows lie at the pitch a guest allocator gives a buffer: 16 bytes for NV12 and packed pixels, 32 for I420 luma.
    require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_YUV420, 99, 77, 0)) == VIRTIO_VIDEO_RESP_OK_NODATA, "odd I420");
    raw = f.params(1, input);
    require(raw.num_planes == 3 && raw.plane_formats[0].plane_size == 128u * 77 && raw.plane_formats[0].stride == 128 &&
            raw.plane_formats[1].plane_size == 64u * 39 && raw.plane_formats[1].stride == 64 &&
            raw.plane_formats[2].plane_size == 64u * 39 && raw.plane_formats[2].stride == 64, "odd I420 planes");
    require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_YUV420, 99, 77, 1)) == VIRTIO_VIDEO_RESP_OK_NODATA, "odd I420 in one buffer");
    raw = f.params(1, input);
    require(raw.num_planes == 1 && raw.plane_formats[0].plane_size == 128u * 77 + 2 * 64u * 39 && raw.plane_formats[0].stride == 128,
            "odd I420 in one buffer");
    require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_BGRA8888, 341, 64, 0)) == VIRTIO_VIDEO_RESP_OK_NODATA, "odd packed");
    raw = f.params(1, input);
    require(raw.num_planes == 1 && raw.plane_formats[0].plane_size == 1376u * 64 && raw.plane_formats[0].stride == 1376,
            "odd packed plane");
    // BGRA and RGBA are one plane of four bytes a pixel, whichever way the plane count is asked for.
    for (const auto format : {VIRTIO_VIDEO_FORMAT_BGRA8888, VIRTIO_VIDEO_FORMAT_RGBA8888})
        for (const unsigned planes : {0u, 1u}) {
            require(Fixture::type(f.setInput(1, format, 64, 64, planes)) == VIRTIO_VIDEO_RESP_OK_NODATA, "packed format");
            raw = f.params(1, input);
            require(raw.format == format && raw.num_planes == 1 && raw.plane_formats[0].plane_size == 64u * 64 * 4 &&
                    raw.plane_formats[0].stride == 256, "packed plane");
        }
    require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_RGBA8888, 64, 64, 2)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "a packed format in two planes was accepted");

    // What the host cannot encode, or size, is refused.
    require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_P010, 64, 64)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "H.264 accepted ten-bit input");
    require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_NV16, 64, 64)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "NV16");
    require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_H264, 64, 64)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "a coded format on the raw queue");
    require(Fixture::type(f.setInput(1, 0, 64, 64)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "no format");
    for (const auto [w, h] : {std::pair {0u, 64u}, {64u, 0u}, {32u, 64u}, {64u, 32u}, {4098u, 64u}, {64u, 2306u}})
        require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_NV12, w, h)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
                "an unsupported frame size was accepted");
    require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_NV12, 4096, 2304)) == VIRTIO_VIDEO_RESP_OK_NODATA,
            "the largest frame was refused");
    // A refused request leaves the stream as it was.
    raw = f.params(1, input);
    require(raw.frame_width == 4096 && raw.format == VIRTIO_VIDEO_FORMAT_NV12, "a refused request changed the stream");

    // The crop is moved inside the frame and rounded down to the encoder's alignment; the coded size follows it.
    require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_NV12, 256, 256, 0, {7, 3, 129, 131})) == VIRTIO_VIDEO_RESP_OK_NODATA,
            "crop");
    raw = f.params(1, input);
    require(raw.crop.left == 6 && raw.crop.top == 2 && raw.crop.width == 128 && raw.crop.height == 130, "crop adjusted");
    coded = f.params(1, output);
    require(coded.frame_width == 128 && coded.frame_height == 130 && coded.crop.width == 128 && coded.crop.height == 130,
            "the coded size is the crop");
    require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_NV12, 256, 256, 0, {250, 250, 100, 100})) ==
            VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a crop smaller than the encoder's minimum was accepted");

    // The coded format can change until buffers exist, within what the input fits.
    require(Fixture::type(f.setOutput(1, VIRTIO_VIDEO_FORMAT_HEVC)) == VIRTIO_VIDEO_RESP_OK_NODATA, "HEVC");
    require(f.params(1, output).format == VIRTIO_VIDEO_FORMAT_HEVC, "coded format");
    require(Fixture::type(f.setOutput(1, VIRTIO_VIDEO_FORMAT_VP9)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "VP9");
    require(Fixture::type(f.setOutput(1, VIRTIO_VIDEO_FORMAT_NV12)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "NV12 as coded");
    require(Fixture::type(f.setOutput(1, VIRTIO_VIDEO_FORMAT_MJPEG)) == VIRTIO_VIDEO_RESP_OK_NODATA, "JPEG");
    // JPEG takes NV12, BGRA and RGBA but not I420.
    require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_YUV420, 64, 64)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "JPEG accepted I420");

    // Buffers pin the formats.
    Fixture g;
    prepare(g);
    require(Fixture::type(g.setInput(1, VIRTIO_VIDEO_FORMAT_NV12, frameSize, frameSize)) == VIRTIO_VIDEO_RESP_OK_NODATA,
            "setting the same raw format again must be a no-op");
    require(Fixture::type(g.setInput(1, VIRTIO_VIDEO_FORMAT_BGRA8888, frameSize, frameSize)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "the raw format changed under allocated buffers");
    require(Fixture::type(g.setOutput(1, VIRTIO_VIDEO_FORMAT_HEVC)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "the coded format changed under allocated buffers");
    require(Fixture::type(g.setOutput(1, VIRTIO_VIDEO_FORMAT_H264)) == VIRTIO_VIDEO_RESP_OK_NODATA, "same coded format");
}

void streamInformation()
{
    Fixture f;
    f.create(1);
    const auto info = [&f](std::uint32_t queue) {
        const auto result = f.send(virtio_video_get_stream_info {{VIRTIO_VIDEO_CMD_GET_STREAM_INFO, 1}, queue, {}});
        require(Fixture::type(result) == VIRTIO_VIDEO_RESP_OK_STREAM_INFO, "GET_STREAM_INFO");
        return load<virtio_video_get_stream_info_resp>(result.response).info;
    };
    auto current = info(input);
    require(current.queue_type == input && current.frame_interval_numerator == 1 && current.frame_interval_denominator == 30 &&
            current.colorspace == 0 && current.xfer_func == 0 && current.ycbcr_enc == 0 && current.quantization == 0,
            "default stream information");
    f.refused(virtio_video_get_stream_info {{VIRTIO_VIDEO_CMD_GET_STREAM_INFO, 1}, 0, {}}, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
              "stream information of no queue");

    virtio_video_set_stream_info set {};
    set.hdr = {VIRTIO_VIDEO_CMD_SET_STREAM_INFO, 1};
    set.info = {input, 1001, 30000, UML_V4L2_COLORSPACE_REC709, UML_V4L2_XFER_FUNC_709, UML_V4L2_YCBCR_ENC_709,
                UML_V4L2_QUANTIZATION_LIM_RANGE, {}};
    f.ok(set);
    for (const auto queue : {input, output}) {
        current = info(queue);
        require(current.queue_type == queue && current.frame_interval_numerator == 1001 && current.frame_interval_denominator == 30000 &&
                current.colorspace == UML_V4L2_COLORSPACE_REC709 && current.xfer_func == UML_V4L2_XFER_FUNC_709 &&
                current.ycbcr_enc == UML_V4L2_YCBCR_ENC_709 && current.quantization == UML_V4L2_QUANTIZATION_LIM_RANGE,
                "the coded queue reports the colorimetry it writes");
    }
    require(f.params(1, input).frame_rate == 30, "the frame rate is the rounded interval");
    // The coded queue cannot change the colorimetry; it can set the interval.
    set.info = {output, 1, 25, UML_V4L2_COLORSPACE_SRGB, UML_V4L2_XFER_FUNC_SRGB, UML_V4L2_YCBCR_ENC_601,
                UML_V4L2_QUANTIZATION_FULL_RANGE, {}};
    f.ok(set);
    current = info(input);
    require(current.frame_interval_numerator == 1 && current.frame_interval_denominator == 25 &&
            current.colorspace == UML_V4L2_COLORSPACE_REC709, "the coded side changed the raw colorimetry");
    // A zero interval is no request; values the encoder cannot describe become the default.
    set.info = {input, 0, 0, 99, 99, 99, 99, {}};
    f.ok(set);
    current = info(input);
    require(current.frame_interval_numerator == 1 && current.frame_interval_denominator == 25 && current.colorspace == 0 &&
            current.xfer_func == 0 && current.ycbcr_enc == 0 && current.quantization == 0, "unsupported colorimetry");
    // RAW states no standard: the colorspace is kept, and the fields beside it keep their own values.
    set.info = {input, 1, 25, UML_V4L2_COLORSPACE_RAW, UML_V4L2_XFER_FUNC_NONE, UML_V4L2_YCBCR_ENC_DEFAULT,
                UML_V4L2_QUANTIZATION_FULL_RANGE, {}};
    f.ok(set);
    for (const auto queue : {input, output}) {
        current = info(queue);
        require(current.colorspace == UML_V4L2_COLORSPACE_RAW && current.xfer_func == UML_V4L2_XFER_FUNC_NONE &&
                current.ycbcr_enc == UML_V4L2_YCBCR_ENC_DEFAULT && current.quantization == UML_V4L2_QUANTIZATION_FULL_RANGE,
                "a RAW colorspace was not kept");
    }
    // Hybrid log-gamma is the kernel line's own V4L2 value, and the wire carries it like any other transfer function.
    set.info = {input, 1, 25, UML_V4L2_COLORSPACE_BT2020, UML_V4L2_XFER_FUNC_HLG, UML_V4L2_YCBCR_ENC_BT2020,
                UML_V4L2_QUANTIZATION_LIM_RANGE, {}};
    f.ok(set);
    for (const auto queue : {input, output}) {
        current = info(queue);
        require(current.colorspace == UML_V4L2_COLORSPACE_BT2020 && current.xfer_func == UML_V4L2_XFER_FUNC_HLG &&
                current.ycbcr_enc == UML_V4L2_YCBCR_ENC_BT2020 && current.quantization == UML_V4L2_QUANTIZATION_LIM_RANGE,
                "hybrid log-gamma was not kept");
    }
    set.info.queue_type = 0;
    f.refused(set, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "stream information for no queue was accepted");
    set.hdr.stream_id = 9;
    set.info.queue_type = input;
    f.refused(set, VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID, "stream information for an unknown stream was accepted");
}

void controlCommands()
{
    Fixture f;
    f.create(1);
    const auto refusedAt = [&f](const ControlSet& set, std::uint32_t expected, std::uint32_t index, const char* what,
                                std::source_location where = std::source_location::current()) {
        const auto result = setControls(f, 1, set);
        require(Fixture::type(result) == expected, what, where);
        if (result.response.size() >= sizeof(virtio_video_v4l2_controls_resp))
            require(controlsResponse(result).error_index == index && controlsResponse(result).num_controls == set.entries.size(),
                    "the failing control was not named", where);
    };
    const auto value = [&f](std::uint32_t id) { return getControls(f, 1, {id}).scalars.at(id); };

    require(value(UML_V4L2_CID_MPEG_VIDEO_BITRATE) == 2'000'000 &&
            value(UML_V4L2_CID_MPEG_VIDEO_H264_PROFILE) == UML_V4L2_MPEG_VIDEO_H264_PROFILE_HIGH, "defaults read back");
    // Each way a value can be wrong names the control that was.
    refusedAt(ControlSet().scalar(0x12345678, 1), VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL, 0, "unknown control");
    refusedAt(ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_BITRATE, 3'000'000).scalar(0x12345678, 1),
              VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL, 1, "unknown control after a good one");
    refusedAt(ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_BITRATE, 19'999), VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, 0,
              "below the bit rate floor");
    refusedAt(ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_BITRATE, 999), VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, 0, "below range");
    // The floor itself is taken: 20 kbit/s is the least that holds the quality bar at the smallest picture.
    require(Fixture::type(setControls(f, 1, ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_BITRATE, 20'000))) ==
                    VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS &&
            value(UML_V4L2_CID_MPEG_VIDEO_BITRATE) == 20'000, "the bit rate floor was refused");
    require(Fixture::type(setControls(f, 1, ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_BITRATE, 2'000'000))) ==
                    VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS &&
            value(UML_V4L2_CID_MPEG_VIDEO_BITRATE) == 2'000'000, "the default was not restored");
    refusedAt(ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_H264_PROFILE, UML_V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE),
              VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, 0, "a menu item the host lacks");
    refusedAt(ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_MB_RC_ENABLE, 2), VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, 0, "boolean");
    refusedAt(ControlSet().scalar(UML_V4L2_CID_MIN_BUFFERS_FOR_OUTPUT, 3), VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, 0,
              "a read-only control was set");
    {
        auto request = ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_BITRATE, 3'000'000).bytes(VIRTIO_VIDEO_CMD_SET_V4L2_CONTROLS, 1);
        const std::uint32_t size = 4;
        std::memcpy(request.data() + sizeof(virtio_video_v4l2_controls) + offsetof(virtio_video_v4l2_control_value, size), &size, sizeof(size));
        const auto result = f.device.processQueue(0, f.next++, request, 16384);
        require(Fixture::type(result) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER && controlsResponse(result).error_index == 0,
                "a scalar carrying a payload size was accepted");
    }
    refusedAt(ControlSet().compound(UML_V4L2_CID_COLORIMETRY_HDR10_CLL_INFO, {1, 2, 3}), VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, 0,
              "a compound payload of the wrong size");
    // A set applies every value or none.
    refusedAt(ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_BITRATE, 3'000'000).scalar(UML_V4L2_CID_MPEG_VIDEO_MB_RC_ENABLE, 5),
              VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, 1, "mixed set");
    require(value(UML_V4L2_CID_MPEG_VIDEO_BITRATE) == 2'000'000, "a set that failed half way applied its first value");
    // The quantizer range may not close on itself; the control that closes it is named.
    refusedAt(ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_H264_MIN_QP, 30).scalar(UML_V4L2_CID_MPEG_VIDEO_H264_MAX_QP, 20),
              VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, 1, "an empty quantizer range");
    require(value(UML_V4L2_CID_MPEG_VIDEO_H264_MIN_QP) == 0 && value(UML_V4L2_CID_MPEG_VIDEO_H264_MAX_QP) == 51,
            "the refused quantizer range was kept");
    // Malformed requests.
    for (const std::uint32_t count : {0u, 257u}) {
        auto request = ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_BITRATE, 3'000'000).bytes(VIRTIO_VIDEO_CMD_SET_V4L2_CONTROLS, 1);
        std::memcpy(request.data() + offsetof(virtio_video_v4l2_controls, num_controls), &count, sizeof(count));
        require(Fixture::type(f.device.processQueue(0, f.next++, request, 16384)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
                "a bad control count was accepted");
    }
    auto shortRequest = ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_BITRATE, 3'000'000).bytes(VIRTIO_VIDEO_CMD_SET_V4L2_CONTROLS, 1);
    const std::uint32_t three = 3;
    std::memcpy(shortRequest.data() + offsetof(virtio_video_v4l2_controls, num_controls), &three, sizeof(three));
    require(Fixture::type(f.device.processQueue(0, f.next++, shortRequest, 16384)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "a count larger than the request was accepted");
    // A payload offset outside the area, or one that is not aligned.
    {
        ControlSet set;
        set.compound(UML_V4L2_CID_COLORIMETRY_HDR10_CLL_INFO, {1, 2, 3, 4});
        auto request = set.bytes(VIRTIO_VIDEO_CMD_SET_V4L2_CONTROLS, 1);
        for (const std::uint64_t offset : {std::uint64_t(8), std::uint64_t(4)}) {
            std::memcpy(request.data() + sizeof(virtio_video_v4l2_controls) + offsetof(virtio_video_v4l2_control_value, value),
                        &offset, sizeof(offset));
            require(Fixture::type(f.device.processQueue(0, f.next++, request, 16384)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
                    "a payload outside the area, or unaligned, was accepted");
        }
    }

    // Valid sets are read back, scalars and compound payloads alike.
    {
        const auto result = setControls(f, 1, ControlSet()
            .scalar(UML_V4L2_CID_MPEG_VIDEO_BITRATE, 4'000'000)
            .scalar(UML_V4L2_CID_MPEG_VIDEO_H264_PROFILE, UML_V4L2_MPEG_VIDEO_H264_PROFILE_MAIN)
            .scalar(UML_V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1));
        require(Fixture::type(result) == VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS && controlsResponse(result).error_index == 3 &&
                controlsResponse(result).num_controls == 3, "a valid set was refused");
    }
    require(value(UML_V4L2_CID_MPEG_VIDEO_BITRATE) == 4'000'000 &&
            value(UML_V4L2_CID_MPEG_VIDEO_H264_PROFILE) == UML_V4L2_MPEG_VIDEO_H264_PROFILE_MAIN, "values read back");
    require(getControls(f, 1, {UML_V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME}).scalars.at(UML_V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME) == 0,
            "a button keeps a value");
    const std::vector<std::uint8_t> light {1, 2, 3, 4};
    require(Fixture::type(setControls(f, 1, ControlSet().compound(UML_V4L2_CID_COLORIMETRY_HDR10_CLL_INFO, light))) ==
            VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS, "compound set");
    auto fetched = getControls(f, 1, {UML_V4L2_CID_COLORIMETRY_HDR10_CLL_INFO, UML_V4L2_CID_MPEG_VIDEO_GOP_SIZE,
                                      UML_V4L2_CID_COLORIMETRY_HDR10_MASTERING_DISPLAY});
    require(fetched.type == VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS && fetched.payloads.at(UML_V4L2_CID_COLORIMETRY_HDR10_CLL_INFO) == light &&
            fetched.payloads.at(UML_V4L2_CID_COLORIMETRY_HDR10_MASTERING_DISPLAY) == std::vector<std::uint8_t>(24) &&
            fetched.scalars.at(UML_V4L2_CID_MPEG_VIDEO_GOP_SIZE) == 0, "compound values read back, unset ones zero");
    fetched = getControls(f, 1, {UML_V4L2_CID_MPEG_VIDEO_BITRATE, 0x12345678});
    require(fetched.type == VIRTIO_VIDEO_RESP_ERR_UNSUPPORTED_CONTROL && fetched.errorIndex == 1, "reading an unknown control");
    require(getControls(f, 1, {UML_V4L2_CID_MPEG_VIDEO_BITRATE}, 20).type == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "a response that does not fit was written");
    f.refused(virtio_video_cmd_hdr {VIRTIO_VIDEO_CMD_GET_V4L2_CONTROLS, 9}, VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID,
              "controls of an unknown stream");
    // Streams keep their own values.
    f.create(2);
    require(getControls(f, 2, {UML_V4L2_CID_MPEG_VIDEO_BITRATE}).scalars.at(UML_V4L2_CID_MPEG_VIDEO_BITRATE) == 2'000'000,
            "a new stream inherited another's controls");
}

/* The planes of a raw frame reach the encoder exactly, the coded frame reaches
 * the guest in its scattered buffer, and the input completes at the host copy. */
void encodeRoundTrip()
{
    // Coded frames are padded past the first segment of the guest's buffer.
    constexpr std::size_t codedFrame = 200;
    Fixture f([](SyntheticEncoder& encoder) { encoder.codedSize = codedFrame; encoder.bFrameEvery = 2; });
    const auto padded = [](std::vector<std::uint8_t> bytes) {
        bytes.resize(codedFrame, 0x5a);
        return bytes;
    };
    prepare(f);
    require(f.device.processQueue(1, 900, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::defer,
            "an event buffer did not wait");

    // A frame with no coded buffer waiting: the input completes, the coded frame waits in the device.
    const auto first = f.queueRaw(1, 1, 1000);
    auto done = f.completions();
    require(done.size() == 1, "the raw frame did not complete at the host copy");
    require(f.encoders.size() == 1 && f.encoder().configures == 1, "the first frame configures the encoder once");
    const auto& settings = f.encoder().settings;
    require(settings.codec == VideoCodec::h264 && settings.profile == VideoEncodeProfile::h264High && settings.level == 10 &&
            settings.width == frameSize && settings.height == frameSize && settings.input == RawVideoLayout::nv12 &&
            settings.rateControl == VideoRateControl::variableBitrate && settings.bitrate == 2'000'000 &&
            settings.frameRateNumerator == 30 && settings.frameRateDenominator == 1, "the encoder was not configured from the stream");
    const auto& completed = Fixture::completionOf(done, first);
    require(completed.responseType() == VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE && completed.buffer().flags == 0 &&
            completed.buffer().timestamp == 1000 && completed.buffer().data_sizes[0] == lumaBytes &&
            completed.buffer().data_sizes[1] == chromaBytes, "raw frame completion");
    require(f.encoder().submitted.size() == 1 && f.encoder().submitted[0].timestamp == 1000 && !f.encoder().submitted[0].forceKey,
            "submission");
    f.requireFrame(f.encoder().submitted[0], lumaBase(1), chromaBase(1), frameSize, 0, 0, frameSize, frameSize, "raw frame 1");

    // The coded buffer then takes it.
    auto coded = f.queueCoded(1, 0);
    done = f.completions();
    const auto& key = Fixture::completionOf(done, coded);
    const auto expected = padded(Unit::unit(VideoCodec::h264, VideoFrameType::key, 0, f.memory[lumaBase(1)]));
    require(key.responseType() == VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE && key.buffer().flags == VIRTIO_VIDEO_DEQUEUE_FLAG_KEY_FRAME &&
            key.buffer().timestamp == 1000 && key.buffer().data_sizes[0] == expected.size(), "coded completion");
    require(f.codedOutput(0, expected.size()) == expected, "the coded bytes did not cross the scattered buffer");
    require(expected.size() > codedHead, "the coded frame must span both segments to test the scatter");
    require(f.codedOutput(0, codedBytes)[codedFrame] == 0xcc, "the device wrote past the coded frame");

    // A coded buffer waiting first, then a frame: the next one is a predicted frame.
    coded = f.queueCoded(1, 1);
    require(f.completions().empty(), "a coded buffer completed without a frame");
    const auto second = f.queueRaw(1, 2, 2033);
    done = f.completions();
    require(done.size() == 2 && done[0].request == second && done[1].request == coded, "input completes before its coded frame");
    require(done[1].buffer().flags == VIRTIO_VIDEO_DEQUEUE_FLAG_PFRAME && done[1].buffer().timestamp == 2033,
            "a later frame is a predicted frame carrying its own timestamp");
    require(f.codedOutput(1, done[1].buffer().data_sizes[0]) ==
            padded(Unit::unit(VideoCodec::h264, VideoFrameType::predicted, 1, f.memory[lumaBase(2)])), "second coded frame");
    f.requireFrame(f.encoder().submitted[1], lumaBase(2), chromaBase(2), frameSize, 0, 0, frameSize, frameSize, "raw frame 2");
    require(f.encoder().configures == 1, "the encoder was configured again mid stream");

    // Output the platform produces later reaches a waiting buffer through the wake-up.
    f.encoder().hold = true;
    coded = f.queueCoded(1, 2);
    const auto third = f.queueRaw(1, 3, 3066);
    done = f.completions();
    require(done.size() == 1 && done[0].request == third, "only the raw frame completes while output is held back");
    f.device.drainEventSource();  // flush wake-ups for output already taken
    require(!f.readable(0), "the descriptor is readable with nothing to take");
    f.encoder().release();
    f.wake();
    done = f.completions();
    require(done.size() == 1 && done[0].request == coded && done[0].buffer().timestamp == 3066 &&
            done[0].buffer().flags == VIRTIO_VIDEO_DEQUEUE_FLAG_BFRAME, "late output did not reach the waiting buffer");
    // A resource is free to be queued again once it completed.
    coded = f.queueCoded(1, 0);
    require(f.completions().empty(), "re-queued coded buffer completed without output");

    // The event queue held its buffer all along: nothing went wrong.
    require(!Fixture::completed(f.completions(), 900), "an event completed without an event");
}

void crop()
{
    Fixture f;
    f.create(1);
    // A 128x128 frame in the guest, of which a 64x64 window at (8, 4) is encoded.
    constexpr std::uint32_t guest = 128;
    const std::size_t luma = 40 * page, chroma = 50 * page;
    for (std::size_t row = 0; row < guest; ++row)
        for (std::size_t column = 0; column < guest; ++column) f.memory[luma + row * guest + column] = lumaPattern(9, row, column);
    for (std::size_t row = 0; row < guest / 2; ++row)
        for (std::size_t byte = 0; byte < guest; ++byte) f.memory[chroma + row * guest + byte] = chromaPattern(9, row, byte);
    require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_NV12, guest, guest, 0, {8, 4, 64, 66})) == VIRTIO_VIDEO_RESP_OK_NODATA,
            "crop");
    // The buffers must hold the whole frame, not only the window.
    require(Fixture::type(f.attach(1, input, 1, {{{luma, 4096, 0}}, {{chroma, 2048, 0}}})) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "a raw buffer smaller than the frame was accepted");
    require(Fixture::type(f.attach(1, input, 1, {{{luma, guest * guest, 0}}})) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "one plane was attached to a two-plane layout");
    require(Fixture::type(f.attach(1, input, 1, {{{luma, guest * guest, 0}}, {{chroma, guest * guest / 2, 0}}})) ==
            VIRTIO_VIDEO_RESP_OK_NODATA, "full-frame raw buffers refused");
    f.attachCodedOk(1, 0);
    f.queue(1, input, 1, 77);
    require(f.encoder().settings.width == 64 && f.encoder().settings.height == 66, "the encoder was not given the crop size");
    f.requireFrame(f.encoder().submitted.at(0), luma, chroma, guest, 8, 4, 64, 66, "cropped frame");
}

/* A frame whose width is not a multiple of the allocator's row alignment is
 * read at the pitch the device stated: every row after the first of a copy at
 * the tight width would hold a window of the picture that starts at the wrong
 * sample. The guest bytes are written at the pitch spelled out here. */
void unalignedFrame()
{
    struct Case { std::uint32_t width, height, pitch; };
    for (const auto [width, height, pitch] : {Case {99, 77, 112}, Case {340, 68, 352}, Case {520, 64, 528}}) {
        Fixture f;
        f.create(1);
        const std::size_t luma = 40 * page, chroma = 60 * page;
        const std::uint32_t cw = (width + 1) / 2, ch = (height + 1) / 2;
        for (std::size_t row = 0; row < height; ++row)
            for (std::size_t column = 0; column < pitch; ++column)
                f.memory[luma + row * pitch + column] = column < width ? lumaPattern(5, row, column) : 0xee;
        for (std::size_t row = 0; row < ch; ++row)
            for (std::size_t byte = 0; byte < pitch; ++byte)
                f.memory[chroma + row * pitch + byte] = byte < 2 * cw ? chromaPattern(5, row, byte) : 0xee;
        require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_NV12, width, height, 0)) == VIRTIO_VIDEO_RESP_OK_NODATA,
                "unaligned frame size");
        const auto raw = f.params(1, input);
        require(raw.plane_formats[0].stride == pitch && raw.plane_formats[0].plane_size == pitch * height &&
                raw.plane_formats[1].stride == pitch && raw.plane_formats[1].plane_size == pitch * ch,
                "unaligned frame layout");
        require(Fixture::type(f.attach(1, input, 1, {{{luma, raw.plane_formats[0].plane_size, 0}},
                                                      {{chroma, raw.plane_formats[1].plane_size, 0}}})) ==
                VIRTIO_VIDEO_RESP_OK_NODATA, "unaligned frame buffers refused");
        f.attachCodedOk(1, 0);
        f.queue(1, input, 1, 77);
        const auto cropWidth = width & ~1u, cropHeight = height & ~1u;
        require(f.encoder().settings.width == cropWidth && f.encoder().settings.height == cropHeight,
                "the encoder was not given the aligned size");
        f.requireFrame(f.encoder().submitted.at(0), luma, chroma, pitch, 0, 0, cropWidth, cropHeight, "unaligned frame");
    }
}

void otherLayouts()
{
    // Planar I420 in three buffers and in one, and packed BGRA, copy plane by plane.
    for (const unsigned planes : {0u, 1u}) {
        Fixture f;
        f.create(1);
        require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_YUV420, 64, 64, planes)) == VIRTIO_VIDEO_RESP_OK_NODATA, "I420");
        const std::size_t y = 40 * page, u = 42 * page, v = 44 * page, whole = 46 * page;
        for (std::size_t i = 0; i < 4096; ++i) f.memory[y + i] = lumaPattern(1, i / 64, i % 64);
        for (std::size_t i = 0; i < 1024; ++i) { f.memory[u + i] = chromaPattern(2, i / 32, i % 32); f.memory[v + i] = chromaPattern(3, i / 32, i % 32); }
        if (planes == 0) {
            require(Fixture::type(f.attach(1, input, 1, {{{y, 4096, 0}}, {{u, 1024, 0}}, {{v, 1024, 0}}})) == VIRTIO_VIDEO_RESP_OK_NODATA, "attach");
        } else {
            std::memcpy(&f.memory[whole], &f.memory[y], 4096);
            std::memcpy(&f.memory[whole + 4096], &f.memory[u], 1024);
            std::memcpy(&f.memory[whole + 5120], &f.memory[v], 1024);
            // One buffer of the whole frame, in two segments that split a plane.
            require(Fixture::type(f.attach(1, input, 1, {{{whole, 4100, 0}, {whole + 4100, 2044, 0}}})) == VIRTIO_VIDEO_RESP_OK_NODATA, "attach");
        }
        f.attachCodedOk(1, 0);
        f.queue(1, input, 1, 5);
        const auto& frame = f.encoder().submitted.at(0);
        require(frame.planes.size() == 3, "I420 has three planes");
        for (std::size_t plane = 0; plane < 3; ++plane) {
            const auto rowBytes = plane == 0 ? 64u : 32u, rows = plane == 0 ? 64u : 32u;
            const auto source = plane == 0 ? y : plane == 1 ? u : v;
            for (std::size_t row = 0; row < rows; ++row)
                require(std::memcmp(&frame.planes[plane][row * frame.strides[plane]], &f.memory[source + row * rowBytes], rowBytes) == 0,
                        "an I420 plane changed");
        }
    }
    // The two packed formats reach the encoder as their own layout, byte for byte.
    for (const auto format : {VIRTIO_VIDEO_FORMAT_BGRA8888, VIRTIO_VIDEO_FORMAT_RGBA8888}) {
        Fixture f;
        f.create(1);
        require(Fixture::type(f.setInput(1, format, 64, 64)) == VIRTIO_VIDEO_RESP_OK_NODATA, "packed format");
        const std::size_t base = 40 * page;
        for (std::size_t i = 0; i < 64 * 64 * 4; ++i) f.memory[base + i] = std::uint8_t(i * 7 + i / 256);
        require(Fixture::type(f.attach(1, input, 1, {{{base, 64 * 64 * 4, 0}}})) == VIRTIO_VIDEO_RESP_OK_NODATA, "attach");
        f.attachCodedOk(1, 0);
        f.queue(1, input, 1, 5);
        const auto& frame = f.encoder().submitted.at(0);
        require(f.encoder().settings.input == (format == VIRTIO_VIDEO_FORMAT_BGRA8888 ? RawVideoLayout::bgra : RawVideoLayout::rgba) &&
                frame.planes.size() == 1, "a packed format is one plane");
        for (std::size_t row = 0; row < 64; ++row)
            require(std::memcmp(&frame.planes[0][row * frame.strides[0]], &f.memory[base + row * 256], 256) == 0, "packed rows");
    }
    // Ten-bit input is one buffer holding both planes, and cannot be split in two.
    {
        Fixture f;
        f.create(1, VIRTIO_VIDEO_FORMAT_HEVC);
        require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_P010, 64, 64, 2)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
                "P010 in two buffers was accepted");
        require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_P010, 64, 64)) == VIRTIO_VIDEO_RESP_OK_NODATA, "P010");
        const auto p010 = f.params(1, input);
        require(p010.num_planes == 1 && p010.plane_formats[0].plane_size == 64u * 64 * 2 + 32u * 128 && p010.plane_formats[0].stride == 128,
                "P010 is one buffer of the luma and the interleaved chroma");
        const std::size_t base = 40 * page;
        const std::size_t chromaStart = 64 * 64 * 2;
        for (std::size_t i = 0; i < p010.plane_formats[0].plane_size; ++i) f.memory[base + i] = std::uint8_t(i * 5 + i / 128);
        require(Fixture::type(f.attach(1, input, 1, {{{base, p010.plane_formats[0].plane_size, 0}}})) == VIRTIO_VIDEO_RESP_OK_NODATA, "attach");
        f.attachCodedOk(1, 0);
        f.queue(1, input, 1, 5);
        const auto& frame = f.encoder().submitted.at(0);
        require(f.encoder().settings.input == RawVideoLayout::p010 && frame.planes.size() == 2, "P010 reaches the encoder as two planes");
        for (std::size_t row = 0; row < 64; ++row)
            require(std::memcmp(&frame.planes[0][row * frame.strides[0]], &f.memory[base + row * 128], 128) == 0, "P010 luma");
        for (std::size_t row = 0; row < 32; ++row)
            require(std::memcmp(&frame.planes[1][row * frame.strides[1]], &f.memory[base + chromaStart + row * 128], 128) == 0, "P010 chroma");
    }
}

void backpressure()
{
    // The platform takes two frames at once; the third waits in the device until output frees one.
    Fixture f([](SyntheticEncoder& encoder) { encoder.pool = 2; encoder.hold = true; });
    prepare(f);
    const auto r1 = f.queueRaw(1, 1, 100), r2 = f.queueRaw(1, 2, 200), r3 = f.queueRaw(1, 3, 300);
    auto done = f.completions();
    require(f.encoder().submitted.size() == 2 && Fixture::completed(done, r1) && Fixture::completed(done, r2) &&
            !Fixture::completed(done, r3), "the device submitted past the platform's pool");
    require(f.encoder().inFlight == 2, "frames in flight");
    const auto c0 = f.queueCoded(1, 0);
    require(f.completions().empty(), "a coded buffer completed with output held back");
    f.encoder().release(1);
    f.wake();
    done = f.completions();
    require(Fixture::completed(done, c0) && Fixture::completed(done, r3) && done.size() == 2,
            "taking output did not free the waiting frame");
    require(Fixture::completionOf(done, c0).buffer().timestamp == 100 && f.encoder().submitted.size() == 3 &&
            f.encoder().submitted[2].timestamp == 300, "frames out of order");
    f.requireFrame(f.encoder().submitted[2], lumaBase(3), chromaBase(3), frameSize, 0, 0, frameSize, frameSize, "frame 3");

    // The raw buffers are theirs again after completion, and a coded buffer holds its place until used.
    const auto again = f.queueRaw(1, 1, 400);
    require(!Fixture::completed(f.completions(), again), "a frame was submitted into a full pool");

    // A platform that stops providing frames with none in flight cannot encode this stream at all.
    Fixture h([](SyntheticEncoder& encoder) { encoder.pool = 1; });
    prepare(h);
    const auto h0 = h.queueCoded(1, 0);
    h.queueRaw(1, 1, 1);
    require(Fixture::completionOf(h.completions(), h0).buffer().timestamp == 1, "first frame");
    require(h.encoder().inFlight == 0, "the first frame was not taken");
    require(h.device.processQueue(1, 900, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::defer, "event buffer");
    h.encoder().pool = 0;
    const auto h1 = h.queueRaw(1, 2, 2);
    done = h.completions();
    require(Fixture::completionOf(done, h1).buffer().flags == VIRTIO_VIDEO_DEQUEUE_FLAG_ERR &&
            load<virtio_video_event>(Fixture::completionOf(done, 900).response).event_type == VIRTIO_VIDEO_EVENT_ERROR,
            "a platform with no frames and none in flight must fail the stream");

    // Coded frames wait in the device only up to a bound; the rest stay in the platform until buffers come.
    Fixture b([](SyntheticEncoder& encoder) { encoder.pool = 100; });
    prepare(b);
    for (unsigned i = 0; i < 12; ++i) b.queueRaw(1, 1 + i % 3, 1000 + i);
    require(b.encoder().submitted.size() == 12 && b.encoder().readyCount() == 4,
            "the device took coded frames without limit");
    for (unsigned i = 0; i < 12; ++i) {
        const auto request = b.queueCoded(1, i % 4);
        const auto result = Fixture::completionOf(b.completions(), request).buffer();
        require(result.timestamp == 1000 + i && result.flags != VIRTIO_VIDEO_DEQUEUE_FLAG_ERR, "frames out of order");
    }
    require(b.encoder().readyCount() == 0, "frames were left in the platform");

    // With room in the pool every frame goes straight through, whatever the coded side is doing.
    Fixture g([](SyntheticEncoder& encoder) { encoder.pool = 100; });
    prepare(g);
    for (unsigned k = 0; k < 4; ++k) g.queueRaw(1, 1 + k % 3, 1000 + k);
    require(g.encoder().submitted.size() == 4, "frames were not submitted");
}

void drain()
{
    {
        // Nothing encoded: the drain completes at once and the next coded buffer is the end marker.
        Fixture f;
        prepare(f);
        const auto request = f.next;
        require(f.send(virtio_video_stream_drain {{VIRTIO_VIDEO_CMD_STREAM_DRAIN, 1}}).action == VhostUserQueueAction::defer,
                "drain was not deferred");
        auto done = f.completions();
        require(done.size() == 1 && Fixture::completionOf(done, request).responseType() == VIRTIO_VIDEO_RESP_OK_NODATA,
                "an idle drain did not complete");
        require(f.encoder().drains == 0 && f.encoder().configures == 0, "an idle drain created a session");
        const auto coded = f.queueCoded(1, 0);
        done = f.completions();
        const auto marker = Fixture::completionOf(done, coded).buffer();
        require(marker.flags == VIRTIO_VIDEO_DEQUEUE_FLAG_EOS && marker.data_sizes[0] == 0, "the end marker");
    }
    {
        // Frames in the platform: the drain waits for the platform, and the marker follows the last frame.
        Fixture f([](SyntheticEncoder& encoder) { encoder.hold = true; });
        prepare(f);
        f.queueRaw(1, 1, 10);
        f.completions();
        const auto drainRequest = f.next;
        require(f.send(virtio_video_stream_drain {{VIRTIO_VIDEO_CMD_STREAM_DRAIN, 1}}).action == VhostUserQueueAction::defer,
                "drain was not deferred");
        require(f.encoder().drains == 1 && f.completions().empty(), "the drain completed before the platform finished");
        f.refused(virtio_video_stream_drain {{VIRTIO_VIDEO_CMD_STREAM_DRAIN, 1}}, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
                  "a second drain was accepted while one was pending");
        const auto c0 = f.queueCoded(1, 0), c1 = f.queueCoded(1, 1);
        f.encoder().release();
        f.wake();
        const auto done = f.completions();
        require(done.size() == 3 && Fixture::completionOf(done, drainRequest).responseType() == VIRTIO_VIDEO_RESP_OK_NODATA,
                "the drain did not finish with the platform");
        require(Fixture::completionOf(done, c0).buffer().flags == VIRTIO_VIDEO_DEQUEUE_FLAG_KEY_FRAME &&
                Fixture::completionOf(done, c1).buffer().flags == VIRTIO_VIDEO_DEQUEUE_FLAG_EOS,
                "the last frame comes before the end marker");
        // Encoding resumes on the same session.
        f.encoder().hold = false;
        const auto c2 = f.queueCoded(1, 2);
        f.queueRaw(1, 2, 20);
        const auto after = f.completions();
        require(f.encoder().configures == 1 && f.encoder().resets == 0, "resuming after a drain restarted the session");
        require(Fixture::completionOf(after, c2).buffer().timestamp == 20, "frame after the drain");
    }
    {
        // A drain waits for frames still held back by the platform's pool.
        Fixture f([](SyntheticEncoder& encoder) { encoder.pool = 1; encoder.hold = true; });
        prepare(f);
        f.queueRaw(1, 1, 10);
        const auto held = f.queueRaw(1, 2, 20);
        require(!Fixture::completed(f.completions(), held), "a frame completed that the pool could not take");
        const auto drainRequest = f.next;
        f.send(virtio_video_stream_drain {{VIRTIO_VIDEO_CMD_STREAM_DRAIN, 1}});
        require(f.encoder().drains == 0, "the drain went to the platform with a frame still waiting in the device");
        const auto c0 = f.queueCoded(1, 0);
        f.encoder().release(1);
        f.wake();
        require(f.encoder().drains == 1 && f.encoder().submitted.size() == 2, "the drain did not follow the waiting frame");
        f.encoder().release();
        f.wake();
        const auto c1 = f.queueCoded(1, 1), c2 = f.queueCoded(1, 2);
        const auto done = f.completions();
        require(Fixture::completed(done, drainRequest) && Fixture::completed(done, c0) && Fixture::completed(done, c1) &&
                Fixture::completionOf(done, c2).buffer().flags == VIRTIO_VIDEO_DEQUEUE_FLAG_EOS, "drain with a waiting frame");
    }
}

void clearing()
{
    Fixture f([](SyntheticEncoder& encoder) { encoder.hold = true; });
    prepare(f);
    f.queueRaw(1, 1, 10);
    f.completions();
    const auto c0 = f.queueCoded(1, 0), c1 = f.queueCoded(1, 1);
    // Clearing the coded queue returns its buffers with an error and ends the coded sequence.
    f.ok(virtio_video_queue_clear {{VIRTIO_VIDEO_CMD_QUEUE_CLEAR, 1}, output, {}});
    auto done = f.completions();
    require(done.size() == 2 && Fixture::completionOf(done, c0).buffer().flags == VIRTIO_VIDEO_DEQUEUE_FLAG_ERR &&
            Fixture::completionOf(done, c1).buffer().flags == VIRTIO_VIDEO_DEQUEUE_FLAG_ERR, "clear did not return the coded buffers");
    require(f.encoder().resets == 1, "clear did not reset the platform session");
    // Output the platform produced for the old sequence is gone.
    f.encoder().release();
    if (f.readable(0)) f.device.drainEventSource();
    // The same resources can be queued again; the next frame starts a new sequence at a key frame, with a new configure.
    f.encoder().hold = false;
    const auto c2 = f.queueCoded(1, 0);
    f.queueRaw(1, 2, 30);
    done = f.completions();
    require(f.encoder().configures == 2, "the next frame after a clear must configure again");
    const auto frame = Fixture::completionOf(done, c2).buffer();
    require(frame.flags == VIRTIO_VIDEO_DEQUEUE_FLAG_KEY_FRAME && frame.timestamp == 30, "a new sequence starts at a key frame");

    // Clearing the raw queue returns frames the device still holds.
    Fixture g([](SyntheticEncoder& encoder) { encoder.pool = 1; encoder.hold = true; });
    prepare(g);
    const auto r1 = g.queueRaw(1, 1, 1), r2 = g.queueRaw(1, 2, 2);
    done = g.completions();
    require(Fixture::completed(done, r1) && !Fixture::completed(done, r2), "pool of one");
    g.ok(virtio_video_queue_clear {{VIRTIO_VIDEO_CMD_QUEUE_CLEAR, 1}, input, {}});
    done = g.completions();
    require(done.size() == 1 && Fixture::completionOf(done, r2).buffer().flags == VIRTIO_VIDEO_DEQUEUE_FLAG_ERR,
            "clear did not return the waiting raw frame");
    require(g.encoder().resets == 1, "clearing the raw queue must end the coded sequence too");
    // Detaching releases the guest memory; a detached resource is no longer there to queue.
    g.ok(virtio_video_queue_detach_resources {VIRTIO_VIDEO_CMD_QUEUE_DETACH_RESOURCES, 1, input, {}});
    g.refused(g.queueCommand(1, input, 1, 5), VIRTIO_VIDEO_RESP_ERR_INVALID_RESOURCE_ID, "a detached resource was queued");
    g.attachRaw(1, 1);
    g.queueRaw(1, 1, 9);
    // Queue commands on unknown resources and queued twice.
    g.refused(g.queueCommand(1, output, 99, 0), VIRTIO_VIDEO_RESP_ERR_INVALID_RESOURCE_ID, "an unknown coded resource was queued");
    const auto pending = g.queueCoded(1, 3);
    (void)pending;
    g.refused(g.queueCommand(1, output, codedFirst + 3, 0), VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a resource was queued twice");
    require(Fixture::type(g.attachCoded(1, 3)) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a queued resource was re-attached");
    g.refused(virtio_video_queue_clear {{VIRTIO_VIDEO_CMD_QUEUE_CLEAR, 1}, 0, {}}, VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
              "clear of no queue");
    // The response buffer for a queue command must hold the answer.
    const auto small = g.send(g.queueCommand(1, output, codedFirst + 2, 0), sizeof(virtio_video_resource_queue_resp) - 1);
    require(Fixture::type(small) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "a queue command with no room for its answer");
}

void failures()
{
    {
        // A coded frame that does not fit its buffer is an error, and the stream recovers at a key frame.
        Fixture f;
        f.create(1);
        require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_NV12, frameSize, frameSize)) == VIRTIO_VIDEO_RESP_OK_NODATA, "format");
        for (unsigned n = 1; n <= 3; ++n) f.attachRaw(1, n);
        require(Fixture::type(f.attach(1, output, codedFirst, {{{codedBase(0), 16, 0}}})) == VIRTIO_VIDEO_RESP_OK_NODATA, "small buffer");
        f.attachCodedOk(1, 1);
        const auto small = f.queueCoded(1, 0);
        f.queueRaw(1, 1, 10);
        auto done = f.completions();
        const auto bad = Fixture::completionOf(done, small).buffer();
        require(bad.flags == VIRTIO_VIDEO_DEQUEUE_FLAG_ERR && bad.data_sizes[0] == 0, "an oversize frame was written or not reported");
        require(f.memory[codedBase(0)] == 0xcc, "an oversize frame was partly written");
        const auto good = f.queueCoded(1, 1);
        require(Fixture::type(setControls(f, 1, ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1))) ==
                VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS, "force key");
        f.queueRaw(1, 2, 20);
        done = f.completions();
        require(f.encoder().submitted.size() == 2 && f.encoder().submitted[1].forceKey && !f.encoder().submitted[0].forceKey,
                "the frame after an oversize one must be a key frame");
        require(Fixture::completionOf(done, good).buffer().flags == VIRTIO_VIDEO_DEQUEUE_FLAG_KEY_FRAME, "recovery key frame");
        // The recovery is spent by that frame, even when the guest asked for a key frame on the same one.
        f.attachCodedOk(1, 2);
        f.queueCoded(1, 2);
        f.queueRaw(1, 3, 30);
        require(!f.encoder().submitted[2].forceKey, "a recovery key frame was requested twice");
    }
    {
        // A frame the platform failed comes back as an error with its own timestamp.
        Fixture f;
        prepare(f);
        f.encoder().failNext = false;
        f.queueRaw(1, 1, 10);
        f.encoder().failNext = true;
        f.queueRaw(1, 2, 20);
        f.queueRaw(1, 3, 30);
        const auto c0 = f.queueCoded(1, 0), c1 = f.queueCoded(1, 1), c2 = f.queueCoded(1, 2);
        const auto done = f.completions();
        require(Fixture::completionOf(done, c0).buffer().timestamp == 10 && Fixture::completionOf(done, c0).buffer().flags != VIRTIO_VIDEO_DEQUEUE_FLAG_ERR,
                "frame before the failure");
        require(Fixture::completionOf(done, c1).buffer().flags == VIRTIO_VIDEO_DEQUEUE_FLAG_ERR &&
                Fixture::completionOf(done, c1).buffer().timestamp == 20, "the failed frame");
        require(Fixture::completionOf(done, c2).buffer().timestamp == 30 && Fixture::completionOf(done, c2).buffer().flags != VIRTIO_VIDEO_DEQUEUE_FLAG_ERR,
                "frame after the failure");
        require(f.encoder().inFlight == 0, "an error left a frame in flight");
    }
    {
        // A failed frame frees its place: a platform that then offers no frames at all, with none in flight, fails the stream.
        Fixture f([](SyntheticEncoder& encoder) { encoder.pool = 1; });
        prepare(f);
        f.encoder().failNext = true;
        const auto c0 = f.queueCoded(1, 0);
        f.queueRaw(1, 1, 10);
        require(Fixture::completionOf(f.completions(), c0).buffer().flags == VIRTIO_VIDEO_DEQUEUE_FLAG_ERR, "the failed frame");
        f.encoder().pool = 0;
        const auto r2 = f.queueRaw(1, 2, 20);
        require(Fixture::completionOf(f.completions(), r2).buffer().flags == VIRTIO_VIDEO_DEQUEUE_FLAG_ERR,
                "an error entry left the device counting a frame in flight");
    }
    {
        // A session the platform refuses fails the stream: the frame, the event and later frames.
        Fixture f([](SyntheticEncoder& encoder) { encoder.configureStatus = VideoEncodeStatus::unsupported; });
        prepare(f);
        require(f.device.processQueue(1, 900, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::defer, "event buffer");
        require(f.device.processQueue(1, 902, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::defer, "event buffer");
        const auto r1 = f.queueRaw(1, 1, 10);
        auto done = f.completions();
        require(Fixture::completionOf(done, r1).buffer().flags == VIRTIO_VIDEO_DEQUEUE_FLAG_ERR, "the frame of a failed session");
        const auto event = load<virtio_video_event>(Fixture::completionOf(done, 900).response);
        require(event.event_type == VIRTIO_VIDEO_EVENT_ERROR && event.stream_id == 1 && !Fixture::completed(done, 902),
                "the failed session was not reported to the first waiting buffer");
        const auto r2 = f.queueRaw(1, 2, 20);
        done = f.completions();
        require(Fixture::completionOf(done, r2).buffer().flags == VIRTIO_VIDEO_DEQUEUE_FLAG_ERR && f.encoder().configures == 1 &&
                done.size() == 1, "a failed stream retried by itself, or reported twice");
        // The guest recovers by clearing, which lets the next frame try again.
        f.encoder().configureStatus = VideoEncodeStatus::ok;
        f.ok(virtio_video_queue_clear {{VIRTIO_VIDEO_CMD_QUEUE_CLEAR, 1}, input, {}});
        const auto c0 = f.queueCoded(1, 0);
        f.queueRaw(1, 3, 30);
        done = f.completions();
        require(f.encoder().configures == 2 && Fixture::completionOf(done, c0).buffer().timestamp == 30, "recovery after a failed session");
        // The next failure goes to the next waiting buffer, and one with no buffer waiting is kept for the next one.
        f.encoder().configureStatus = VideoEncodeStatus::exhausted;
        f.ok(virtio_video_queue_clear {{VIRTIO_VIDEO_CMD_QUEUE_CLEAR, 1}, input, {}});
        f.queueRaw(1, 1, 40);
        done = f.completions();
        require(Fixture::completed(done, 902), "the second failure did not reach the second buffer");
        f.ok(virtio_video_queue_clear {{VIRTIO_VIDEO_CMD_QUEUE_CLEAR, 1}, input, {}});
        f.queueRaw(1, 1, 50);
        f.completions();
        const auto late = f.device.processQueue(1, 903, {}, sizeof(virtio_video_event));
        require(late.action == VhostUserQueueAction::complete && load<virtio_video_event>(late.response).event_type == VIRTIO_VIDEO_EVENT_ERROR,
                "an event with no waiting buffer was lost");
    }
    {
        // Events: a buffer too small is dropped, and the queue pushes back when full.
        Fixture f;
        require(f.device.processQueue(1, 1, {}, 4).response.empty(), "an event buffer too small");
        for (unsigned i = 0; i < 256; ++i)
            require(f.device.processQueue(1, 2000 + i, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::defer, "event buffer");
        require(f.device.processQueue(1, 3000, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::retry,
                "a full event queue did not push back");
    }
    {
        // Guest memory the device cannot map is refused at attach, and a bad resource id.
        Fixture f;
        f.create(1);
        require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_NV12, 64, 64)) == VIRTIO_VIDEO_RESP_OK_NODATA, "format");
        require(Fixture::type(f.attach(1, input, 1, {{{f.memory.size(), 4096, 0}}, {{0, 2048, 0}}})) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
                "an unmapped raw buffer was attached");
        require(Fixture::type(f.attach(1, output, 0, {{{0, 4096, 0}}})) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "resource 0");
        require(Fixture::type(f.attach(1, 0, 1, {{{0, 4096, 0}}})) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "no queue");
        require(Fixture::type(f.attach(1, output, 1, {{{0, 4096, 0}}, {{0, 4096, 0}}})) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
                "a coded buffer of two planes");
        require(Fixture::type(f.attach(7, output, 1, {{{0, 4096, 0}}})) == VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID, "unknown stream");
    }
}

void parameterSets()
{
    const auto run = [](VideoCodec codec, std::uint32_t format, const char* name) {
        Fixture f([](SyntheticEncoder& encoder) { encoder.gop = 2; });
        prepare(f, 1, format);
        const auto sets = Unit::parameterSets(codec);
        const auto sample = [&f](unsigned n) { return f.memory[lumaBase(n)]; };
        // By default every key frame carries the parameter sets.
        for (unsigned i = 0; i < 3; ++i) {
            const auto request = f.queueCoded(1, i);
            f.queueRaw(1, 1 + i, 10 + i);
            const auto done = f.completions();
            const auto buffer = Fixture::completionOf(done, request).buffer();
            const auto type = i % 2 == 0 ? VideoFrameType::key : VideoFrameType::predicted;
            require(f.codedOutput(i, buffer.data_sizes[0]) == Unit::unit(codec, type, i, sample(1 + i)), name);
        }
        // Without the prepend control only the first key frame does.
        Fixture g([](SyntheticEncoder& encoder) { encoder.gop = 2; });
        prepare(g, 1, format);
        require(Fixture::type(setControls(g, 1, ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 0))) ==
                VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS, "prepend off");
        for (unsigned i = 0; i < 3; ++i) {
            const auto request = g.queueCoded(1, i);
            g.queueRaw(1, 1 + i, 10 + i);
            const auto done = g.completions();
            const auto buffer = Fixture::completionOf(done, request).buffer();
            auto expected = Unit::unit(codec, i % 2 == 0 ? VideoFrameType::key : VideoFrameType::predicted, i, g.memory[lumaBase(1 + i)]);
            if (i == 2) expected = sansParameterSets(expected, codec);
            require(g.codedOutput(i, buffer.data_sizes[0]) == expected, name);
            require(i != 2 || buffer.data_sizes[0] + sets.size() == Unit::unit(codec, VideoFrameType::key, i, g.memory[lumaBase(1 + i)]).size(),
                    "the second key frame kept its parameter sets");
        }
        // The control may change while streaming.
        Fixture h([](SyntheticEncoder& encoder) { encoder.gop = 1; });
        prepare(h, 1, format);
        auto request = h.queueCoded(1, 0);
        h.queueRaw(1, 1, 1);
        require(h.codedOutput(0, Fixture::completionOf(h.completions(), request).buffer().data_sizes[0]).size() ==
                Unit::unit(codec, VideoFrameType::key, 0, h.memory[lumaBase(1)]).size(), name);
        require(Fixture::type(setControls(h, 1, ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 0))) ==
                VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS, "prepend off while streaming");
        request = h.queueCoded(1, 1);
        h.queueRaw(1, 2, 2);
        require(Fixture::completionOf(h.completions(), request).buffer().data_sizes[0] + sets.size() ==
                Unit::unit(codec, VideoFrameType::key, 1, h.memory[lumaBase(2)]).size(), "prepend off took effect");
    };
    run(VideoCodec::h264, VIRTIO_VIDEO_FORMAT_H264, "H.264 parameter sets");
    run(VideoCodec::hevc, VIRTIO_VIDEO_FORMAT_HEVC, "HEVC parameter sets");

    // Separate headers arrive in a buffer of their own, flagged as codec configuration.
    Fixture f([](SyntheticEncoder& encoder) { encoder.separateHeaders = true; });
    prepare(f);
    require(Fixture::type(setControls(f, 1, ControlSet()
                .scalar(UML_V4L2_CID_MPEG_VIDEO_HEADER_MODE, UML_V4L2_MPEG_VIDEO_HEADER_MODE_SEPARATE)
                .scalar(UML_V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, 0))) == VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS, "separate headers");
    const auto c0 = f.queueCoded(1, 0), c1 = f.queueCoded(1, 1);
    f.queueRaw(1, 1, 50);
    const auto done = f.completions();
    require(f.encoder().settings.separateParameterSets, "the encoder was not asked for separate headers");
    const auto headers = Fixture::completionOf(done, c0).buffer();
    require(headers.flags == VIRTIO_VIDEO_DEQUEUE_FLAG_CODEC_CONFIG && f.codedOutput(0, headers.data_sizes[0]) == Unit::parameterSets(VideoCodec::h264),
            "the parameter sets buffer");
    const auto keyFrame = Fixture::completionOf(done, c1).buffer();
    require(keyFrame.flags == VIRTIO_VIDEO_DEQUEUE_FLAG_KEY_FRAME && keyFrame.timestamp == 50 &&
            f.codedOutput(1, keyFrame.data_sizes[0]) ==
                sansParameterSets(Unit::unit(VideoCodec::h264, VideoFrameType::key, 0, f.memory[lumaBase(1)]), VideoCodec::h264),
            "with separate headers the key frame carries none");
}

void dynamicControls()
{
    Fixture f;
    prepare(f);
    f.queueCoded(1, 0);
    f.queueRaw(1, 1, 1);
    f.completions();
    require(f.encoder().configures == 1, "streaming");
    const auto busy = [&f](const ControlSet& set, const char* what, std::source_location where = std::source_location::current()) {
        const auto result = setControls(f, 1, set);
        require(Fixture::type(result) == VIRTIO_VIDEO_RESP_ERR_BUSY, what, where);
    };
    busy(ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_H264_PROFILE, UML_V4L2_MPEG_VIDEO_H264_PROFILE_MAIN), "profile changed while streaming");
    busy(ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_BITRATE_MODE, UML_V4L2_MPEG_VIDEO_BITRATE_MODE_CBR), "bitrate mode changed while streaming");
    busy(ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_GOP_SIZE, 10), "GOP size changed while streaming");
    busy(ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_BITRATE, 3'000'000).scalar(UML_V4L2_CID_MPEG_VIDEO_GOP_SIZE, 10),
         "a set mixing dynamic and static controls applied");
    require(getControls(f, 1, {UML_V4L2_CID_MPEG_VIDEO_BITRATE}).scalars.at(UML_V4L2_CID_MPEG_VIDEO_BITRATE) == 2'000'000,
            "a refused set changed a dynamic control");
    require(f.encoder().updates.empty(), "a refused set reached the encoder");

    // Dynamic controls reach the running encoder.
    require(Fixture::type(setControls(f, 1, ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_BITRATE, 3'000'000)
                                                  .scalar(UML_V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, 4'000'000)
                                                  .scalar(UML_V4L2_CID_MPEG_VIDEO_H264_MAX_QP, 44))) == VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS,
            "dynamic controls refused");
    require(f.encoder().updates.size() == 1 && f.encoder().updates[0].bitrate == 3'000'000u &&
            f.encoder().updates[0].peakBitrate == 4'000'000u && f.encoder().updates[0].maximumQp == 44u &&
            !f.encoder().updates[0].quality, "the update");
    // The encoder may refuse; the value then does not change.
    f.encoder().updateStatus = VideoEncodeStatus::invalid;
    require(Fixture::type(setControls(f, 1, ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_BITRATE, 5'000'000))) ==
            VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "an update the encoder refused was accepted");
    require(getControls(f, 1, {UML_V4L2_CID_MPEG_VIDEO_BITRATE}).scalars.at(UML_V4L2_CID_MPEG_VIDEO_BITRATE) == 3'000'000,
            "a refused update changed the value");
    f.encoder().updateStatus = VideoEncodeStatus::ok;

    // Force-key applies to the next frame the guest queues, and only to it.
    require(Fixture::type(setControls(f, 1, ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1))) ==
            VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS, "force key");
    f.queueRaw(1, 2, 2);
    f.queueRaw(1, 3, 3);
    require(f.encoder().submitted.size() == 3 && !f.encoder().submitted[0].forceKey && f.encoder().submitted[1].forceKey &&
            !f.encoder().submitted[2].forceKey, "force key frame applied to the wrong frame");

    // The frame interval changes the running frame rate; the format and colorimetry do not change.
    virtio_video_set_stream_info set {};
    set.hdr = {VIRTIO_VIDEO_CMD_SET_STREAM_INFO, 1};
    set.info = {input, 1, 60, 0, 0, 0, 0, {}};
    const auto updatesBefore = f.encoder().updates.size();
    f.ok(set);
    require(f.encoder().updates.size() == updatesBefore + 1 && f.encoder().updates.back().frameRateNumerator == 60u &&
            f.encoder().updates.back().frameRateDenominator == 1u && !f.encoder().updates.back().bitrate,
            "the frame rate did not reach the encoder");
    set.info.colorspace = UML_V4L2_COLORSPACE_REC709;
    f.refused(set, VIRTIO_VIDEO_RESP_ERR_BUSY, "colorimetry changed while streaming");
    require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_NV12, 128, 128)) == VIRTIO_VIDEO_RESP_ERR_BUSY,
            "the raw format changed while streaming");
    require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_NV12, frameSize, frameSize)) == VIRTIO_VIDEO_RESP_OK_NODATA,
            "the unchanged format was refused while streaming");
    require(Fixture::type(f.setOutput(1, VIRTIO_VIDEO_FORMAT_HEVC)) == VIRTIO_VIDEO_RESP_ERR_BUSY, "the codec changed while streaming");

    // After the session ends, the static controls are free again and take effect.
    f.ok(virtio_video_queue_clear {{VIRTIO_VIDEO_CMD_QUEUE_CLEAR, 1}, input, {}});
    require(Fixture::type(setControls(f, 1, ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_H264_PROFILE, UML_V4L2_MPEG_VIDEO_H264_PROFILE_MAIN))) ==
            VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS, "profile after the session ended");
    f.queueRaw(1, 1, 4);
    require(f.encoder().configures == 2 && f.encoder().settings.profile == VideoEncodeProfile::h264Main &&
            f.encoder().settings.bitrate == 3'000'000 && f.encoder().settings.frameRateNumerator == 60,
            "the new session did not take the stream's current settings");
}

/* B-frames are coded as the host's whole run or not at all, so a request below the run is none and the stream
 * stays a live one. The run is a codec's own: H.264 three, HEVC two. */
void bFrames()
{
    const auto settingsFor = [](std::uint32_t coded, std::int64_t asked) {
        Fixture f;
        prepare(f, 1, coded);
        if (asked >= 0)
            require(Fixture::type(setControls(f, 1, ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_B_FRAMES, asked))) ==
                    VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS, "B-frames refused inside the published range");
        f.queueCoded(1, 0);
        f.queueRaw(1, 1, 1);
        const auto& settings = f.encoder().settings;
        return std::pair {settings.bFrames, settings.realTime};
    };
    using Result = std::pair<std::uint32_t, bool>;
    require(settingsFor(VIRTIO_VIDEO_FORMAT_H264, -1) == Result {0, true}, "the default is no B-frames");
    require(settingsFor(VIRTIO_VIDEO_FORMAT_H264, 0) == Result {0, true}, "no B-frames");
    require(settingsFor(VIRTIO_VIDEO_FORMAT_H264, 1) == Result {0, true}, "one B-frame is below the run of three");
    require(settingsFor(VIRTIO_VIDEO_FORMAT_H264, 2) == Result {0, true}, "two B-frames are below the run of three");
    require(settingsFor(VIRTIO_VIDEO_FORMAT_H264, 3) == Result {3, false}, "the whole run was not coded");
    require(settingsFor(VIRTIO_VIDEO_FORMAT_HEVC, 1) == Result {0, true}, "one B-frame is below HEVC's run of two");
    require(settingsFor(VIRTIO_VIDEO_FORMAT_HEVC, 2) == Result {2, false}, "HEVC's whole run was not coded");
    require(settingsFor(VIRTIO_VIDEO_FORMAT_HEVC, 3) == Result {2, false}, "a longer run than HEVC has was not clamped to it");

    // Beyond the longest run is outside the control's range.
    Fixture f;
    prepare(f);
    require(Fixture::type(setControls(f, 1, ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_B_FRAMES, 4))) ==
            VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER, "B-frames beyond the longest run were accepted");
    // A host whose HEVC does not reorder refuses the control on an HEVC stream and keeps it for H.264.
    auto capabilities = encoderHostCapabilities();
    capabilities[1].maximumBFrames = 0;
    Fixture g({}, capabilities);
    prepare(g, 1, VIRTIO_VIDEO_FORMAT_HEVC);
    g.queueCoded(1, 0);
    g.queueRaw(1, 1, 1);
    require(g.encoder().settings.bFrames == 0 && g.encoder().settings.realTime, "an HEVC encoder that codes no B-frames was asked for them");
}

/* A client that states a transfer function or a range but no standard sends RAW (the V4L2 core would reset those
 * fields beside a DEFAULT colorspace), and the stream states exactly that and no more. */
void rawColorspace()
{
    Fixture f;
    prepare(f);
    virtio_video_set_stream_info set {};
    set.hdr = {VIRTIO_VIDEO_CMD_SET_STREAM_INFO, 1};
    set.info = {input, 1, 30, UML_V4L2_COLORSPACE_RAW, UML_V4L2_XFER_FUNC_SMPTE2084, UML_V4L2_YCBCR_ENC_DEFAULT,
                UML_V4L2_QUANTIZATION_FULL_RANGE, {}};
    f.ok(set);
    f.queueCoded(1, 0);
    f.queueRaw(1, 1, 1);
    const auto& color = f.encoder().settings.color;
    require(color == (VideoEncodeColor {VideoColorPrimaries::unspecified, VideoTransferFunction::pq,
                                        VideoMatrixCoefficients::unspecified, true}),
            "a RAW colorspace did not state only the transfer function and the range");
    // Streaming, the colorimetry is fixed: RAW is not a way around that.
    set.info.xfer_func = UML_V4L2_XFER_FUNC_NONE;
    f.refused(set, VIRTIO_VIDEO_RESP_ERR_BUSY, "the transfer function changed while streaming");

    // The device does not discard what sits beside a DEFAULT colorspace (the V4L2 core resets it before it gets here),
    // and a DEFAULT colorspace derives nothing from a standard.
    Fixture g;
    prepare(g);
    set.info = {input, 1, 30, UML_V4L2_COLORSPACE_DEFAULT, UML_V4L2_XFER_FUNC_NONE, UML_V4L2_YCBCR_ENC_DEFAULT,
                UML_V4L2_QUANTIZATION_DEFAULT, {}};
    g.ok(set);
    g.queueCoded(1, 0);
    g.queueRaw(1, 1, 1);
    require(g.encoder().settings.color.primaries == VideoColorPrimaries::unspecified &&
            g.encoder().settings.color.transfer == VideoTransferFunction::linear,
            "a default colorspace with a transfer function stated something else");
}

void jpeg()
{
    Fixture f;
    prepare(f, 1, VIRTIO_VIDEO_FORMAT_MJPEG);
    require(Fixture::type(setControls(f, 1, ControlSet().scalar(UML_V4L2_CID_JPEG_COMPRESSION_QUALITY, 60))) ==
            VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS, "JPEG quality");
    const auto c0 = f.queueCoded(1, 0), c1 = f.queueCoded(1, 1);
    f.queueRaw(1, 1, 5);
    f.queueRaw(1, 2, 6);
    const auto done = f.completions();
    require(f.encoder().settings.codec == VideoCodec::jpeg && f.encoder().settings.jpegQuality == 60 &&
            f.encoder().settings.profile == VideoEncodeProfile::jpegBaseline, "JPEG settings");
    const auto first = Fixture::completionOf(done, c0).buffer(), second = Fixture::completionOf(done, c1).buffer();
    require(first.flags == VIRTIO_VIDEO_DEQUEUE_FLAG_KEY_FRAME && second.flags == VIRTIO_VIDEO_DEQUEUE_FLAG_KEY_FRAME,
            "every JPEG image is complete");
    const auto image = f.codedOutput(0, first.data_sizes[0]);
    require(image.size() > 4 && image[0] == 0xff && image[1] == 0xd8 && image[image.size() - 2] == 0xff && image.back() == 0xd9,
            "JPEG image bytes");
    require(f.encoder().updates.empty(), "JPEG quality before streaming is a setting, not an update");
    require(Fixture::type(setControls(f, 1, ControlSet().scalar(UML_V4L2_CID_JPEG_COMPRESSION_QUALITY, 70))) ==
            VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS && f.encoder().updates.size() == 1 && f.encoder().updates[0].jpegQuality == 70u,
            "JPEG quality is dynamic");
}

void severalStreams()
{
    Fixture f([](SyntheticEncoder& encoder) { encoder.hold = true; });
    prepare(f, 1);
    prepare(f, 2);
    require(f.encoders.size() == 2, "one encoder per stream");
    const auto a = f.queueCoded(1, 0), b = f.queueCoded(2, 0);
    f.queueRaw(1, 1, 111);
    f.queueRaw(2, 1, 222);
    f.completions();
    require(f.encoders[0]->submitted.size() == 1 && f.encoders[1]->submitted.size() == 1, "each stream has its own encoder");
    // Only the second stream's encoder produces; only its buffer completes.
    f.encoders[1]->release();
    f.wake();
    auto done = f.completions();
    require(done.size() == 1 && done[0].request == b && done[0].buffer().timestamp == 222, "output reached the wrong stream");
    f.encoders[0]->release();
    f.wake();
    done = f.completions();
    require(done.size() == 1 && done[0].request == a && done[0].buffer().timestamp == 111, "the first stream's output");
    // Destroying one stream leaves the other working.
    f.destroy(1);
    f.encoders[1]->hold = false;
    const auto c = f.queueCoded(2, 1);
    f.queueRaw(2, 2, 333);
    done = f.completions();
    require(Fixture::completionOf(done, c).buffer().timestamp == 333, "the surviving stream stopped");
}

void resetAndDestroy()
{
    Fixture f([](SyntheticEncoder& encoder) { encoder.hold = true; });
    prepare(f);
    require(f.device.processQueue(1, 900, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::defer, "event buffer");
    f.queueRaw(1, 1, 5);
    f.completions();
    const auto c0 = f.queueCoded(1, 0);
    f.queueRaw(1, 2, 6);
    f.completions();
    // Destroying a stream returns everything it still holds and drops its encoder's wake-up.
    f.destroy(1);
    auto done = f.completions();
    require(Fixture::completed(done, c0) && Fixture::completionOf(done, c0).buffer().flags == VIRTIO_VIDEO_DEQUEUE_FLAG_ERR,
            "destroy did not return the waiting coded buffer");
    prepare(f, 2);
    f.queueRaw(2, 1, 5);
    f.completions();
    f.device.reset();
    require(f.completions().empty(), "reset kept completions");
    f.refused(virtio_video_get_params {{VIRTIO_VIDEO_CMD_GET_PARAMS, 2}, input, {}}, VIRTIO_VIDEO_RESP_ERR_INVALID_STREAM_ID,
              "a stream survived a device reset");
    // The event buffer was dropped with the reset: a new one waits again.
    for (unsigned i = 0; i < 256; ++i)
        require(f.device.processQueue(1, 5000 + i, {}, sizeof(virtio_video_event)).action == VhostUserQueueAction::defer,
                "event buffers survived the reset");
    // The device works again after a reset.
    prepare(f, 3);
    const auto c = f.queueCoded(3, 0);
    f.encoders.back()->hold = false;
    f.queueRaw(3, 1, 7);
    done = f.completions();
    require(Fixture::completionOf(done, c).buffer().timestamp == 7, "the device did not work after a reset");
}

std::string readLog(const std::filesystem::path& path)
{
    std::ostringstream buffer;
    buffer << std::ifstream(path).rdbuf();
    return buffer.str();
}

void activityLog()
{
    const auto path = uml::testing::scratch_directory() / ("uml-video-encoder-activity-" + std::to_string(::getpid()) + ".log");
    std::filesystem::remove(path);
    ::unsetenv("UML_VIDEO_ACTIVITY_LOG");
    {
        Fixture f;
        prepare(f);
        f.queueCoded(1, 0);
        f.queueRaw(1, 1, 1);
        f.destroy(1);
    }
    require(!std::filesystem::exists(path), "the activity log is not off by default");

    ::setenv("UML_VIDEO_ACTIVITY_LOG", path.c_str(), 1);
    {
        Fixture f([](SyntheticEncoder& encoder) { encoder.gop = 2; });
        f.create(7);
        require(Fixture::type(f.setInput(7, VIRTIO_VIDEO_FORMAT_NV12, frameSize, frameSize)) == VIRTIO_VIDEO_RESP_OK_NODATA, "format");
        for (unsigned n = 1; n <= 3; ++n) f.attachRaw(7, n);
        for (unsigned k = 0; k < 3; ++k) f.attachCodedOk(7, k);
        virtio_video_set_stream_info rate {};
        rate.hdr = {VIRTIO_VIDEO_CMD_SET_STREAM_INFO, 7};
        rate.info = {input, 1001, 30000, 0, 0, 0, 0, {}};
        f.ok(rate);
        std::size_t bytes = 0;
        for (unsigned i = 0; i < 3; ++i) {
            const auto coded = f.queueCoded(7, i);
            f.queueRaw(7, 1 + i, 10 + i);
            bytes += Fixture::completionOf(f.completions(), coded).buffer().data_sizes[0];
        }
        f.destroy(7);
        // A stream left open is closed, with the reason, by a reset and by the device going away.
        f.create(8);
        f.device.reset();
        f.create(9);
        require(std::filesystem::exists(path), "the activity log was not written");
        const auto text = readLog(path);
        require(text.find("encoder advertise codec=h264 profiles=h264-constrained-baseline,h264-main,h264-high inputs=nv12,i420,bgra,rgba "
                          "max=4096x2304 rc=vbr,cbr,cq,cqp bframes=3 layers=2 sessions=4") != std::string::npos, "advertised H.264");
        require(text.find("encoder advertise codec=hevc profiles=hevc-main,hevc-main10 inputs=nv12,i420,bgra,rgba,p010 max=8192x4320 "
                          "rc=vbr,cbr,cq bframes=2 layers=1 sessions=4") != std::string::npos, "advertised HEVC");
        require(text.find("encoder advertise codec=jpeg profiles=jpeg-baseline inputs=nv12,bgra,rgba max=16384x16384 rc=cq") != std::string::npos,
                "advertised JPEG");
        require(text.find("encoder stream=7 open codec=h264") != std::string::npos, "stream open");
        require(text.find("encoder stream=7 start codec=h264 profile=h264-high rc=vbr bitrate=2000000 size=64x64 fps=30000/1001 "
                          "input=nv12 bframes=0 layers=1 status=ok") != std::string::npos, "stream start");
        const auto close = text.find("encoder stream=7 close=destroy coded=h264");
        require(close != std::string::npos, "stream close");
        const auto line = text.substr(close, text.find('\n', close) - close);
        require(line.find("frames_in=3 frames_out=3 keyframes=2 bytes=" + std::to_string(bytes) + " errors=0 pending_outputs_at_close=0") !=
                std::string::npos, "stream totals: " + line);
    }
    // The stream left open closes with the device.
    const auto text = readLog(path);
    ::unsetenv("UML_VIDEO_ACTIVITY_LOG");
    std::filesystem::remove(path);
    require(text.find("encoder stream=8 close=reset coded=h264 frames_in=0") != std::string::npos,
            "a stream was not closed in the log by a reset");
    require(text.find("encoder stream=9 close=shutdown coded=h264 frames_in=0") != std::string::npos,
            "an open stream was not closed in the log at shutdown");
}

void sharedMemoryBuffers()
{
    constexpr std::uint8_t region = 1;
    const std::uint32_t flags = VIRTIO_VIDEO_RESOURCE_SG_FLAG_SHM_REGION | (std::uint32_t(region) << VIRTIO_VIDEO_RESOURCE_SG_SHM_ID_SHIFT);
    {
        // Without the feature a shared-memory entry is refused, whatever it names.
        Fixture f;
        f.create(1);
        require(Fixture::type(f.attach(1, output, 1, {{{0, 4096, flags}}})) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
                "a shared-memory buffer was attached without the feature");
    }
    // With an index, a buffer in a region nobody published is refused, and a published one is read and written in place.
    Fixture f;
    auto regions = std::make_shared<SharedMemoryRegions>();
    f.device.setSharedMemoryRegions(regions);
    f.create(1);
    require(Fixture::type(f.setInput(1, VIRTIO_VIDEO_FORMAT_NV12, frameSize, frameSize)) == VIRTIO_VIDEO_RESP_OK_NODATA, "format");
    require(Fixture::type(f.attach(1, input, 1, {{{0, 4096, flags}}, {{4096, 2048, flags}}})) == VIRTIO_VIDEO_RESP_ERR_INVALID_PARAMETER,
            "a buffer in an unpublished region was attached");

    auto path = uml::testing::scratch_template("uml-encoder-shm-XXXXXX");
    const int descriptor = ::mkstemp(path.data());
    require(descriptor >= 0, "scratch file");
    ::unlink(path.c_str());
    require(::ftruncate(descriptor, 16 * page) == 0, "scratch file size");
    std::vector<std::uint8_t> luma(lumaBytes), chroma(chromaBytes);
    for (std::size_t row = 0; row < frameSize; ++row)
        for (std::size_t column = 0; column < frameSize; ++column) luma[row * frameSize + column] = lumaPattern(4, row, column);
    for (std::size_t row = 0; row < frameSize / 2; ++row)
        for (std::size_t byte = 0; byte < frameSize; ++byte) chroma[row * frameSize + byte] = chromaPattern(4, row, byte);
    require(::pwrite(descriptor, luma.data(), luma.size(), 0) == ssize_t(luma.size()) &&
            ::pwrite(descriptor, chroma.data(), chroma.size(), 4096) == ssize_t(chroma.size()), "scratch file contents");
    require(regions->publish(region, 0, 16 * page, descriptor, 0), "publishing the region");

    require(Fixture::type(f.attach(1, input, 1, {{{0, std::uint32_t(lumaBytes), flags}}, {{4096, std::uint32_t(chromaBytes), flags}}})) ==
            VIRTIO_VIDEO_RESP_OK_NODATA, "raw buffers in the region");
    // The coded buffer is in the region too, across a page boundary.
    require(Fixture::type(f.attach(1, output, 2, {{{8192 + 4000, 96, flags}, {12288, 4000, flags}}})) == VIRTIO_VIDEO_RESP_OK_NODATA,
            "a coded buffer in the region");
    const auto coded = f.queue(1, output, 2);
    f.queue(1, input, 1, 99);
    const auto done = f.completions();

    const auto& frame = f.encoder().submitted.at(0);
    require(frame.planes.size() == 2, "planes");
    for (std::size_t row = 0; row < frameSize; ++row)
        require(std::memcmp(&frame.planes[0][row * frame.strides[0]], &luma[row * frameSize], frameSize) == 0, "luma read from the region");
    for (std::size_t row = 0; row < frameSize / 2; ++row)
        require(std::memcmp(&frame.planes[1][row * frame.strides[1]], &chroma[row * frameSize], frameSize) == 0,
                "chroma read from the region");
    const auto result = Fixture::completionOf(done, coded).buffer();
    const auto expected = Unit::unit(VideoCodec::h264, VideoFrameType::key, 0, luma[0]);
    require(result.flags == VIRTIO_VIDEO_DEQUEUE_FLAG_KEY_FRAME && result.data_sizes[0] == expected.size(), "coded completion");
    std::vector<std::uint8_t> written(expected.size());
    require(::pread(descriptor, written.data(), written.size(), 8192 + 4000) == ssize_t(written.size()), "reading the region back");
    require(written == expected, "the coded frame was not written into the region");
    ::close(descriptor);
}

/* ---- The device on the host's own hardware encoder.
 *
 * Everything above steers a synthetic encoder. These cases run a stream the
 * way a guest driver would on the real platform encoder, with the capability
 * list the platform discovered, and read the coded bytes back. What this host
 * can encode is what discovery says, so a case whose feature the host lacks
 * is skipped, and the capability list is checked to agree. */

std::vector<std::vector<std::uint8_t>> annexBUnits(const std::vector<std::uint8_t>& bytes)
{
    std::vector<std::vector<std::uint8_t>> units;
    std::size_t start = bytes.size();
    for (std::size_t index = 0; index + 2 < bytes.size(); ++index) {
        if (bytes[index] != 0 || bytes[index + 1] != 0 || bytes[index + 2] != 1) continue;
        if (start < bytes.size()) {
            auto end = index;
            while (end > start && bytes[end - 1] == 0) --end;
            units.emplace_back(bytes.begin() + std::ptrdiff_t(start), bytes.begin() + std::ptrdiff_t(end));
        }
        start = index + 3;
        index += 2;
    }
    if (start < bytes.size()) units.emplace_back(bytes.begin() + std::ptrdiff_t(start), bytes.end());
    return units;
}

/* The unit with its emulation prevention bytes removed. */
std::vector<std::uint8_t> unescaped(const std::vector<std::uint8_t>& unit)
{
    std::vector<std::uint8_t> out;
    unsigned zeros = 0;
    for (const auto byte : unit) {
        if (zeros >= 2 && byte == 3) {
            zeros = 0;
            continue;
        }
        zeros = byte == 0 ? zeros + 1 : 0;
        out.push_back(byte);
    }
    return out;
}

constexpr unsigned hevcSps = 33, hevcPrefixSei = 39, h264Sps = 7, h264Idr = 5;
unsigned hevcType(const std::vector<std::uint8_t>& unit) { return (unit.at(0) >> 1) & 0x3f; }
unsigned h264Type(const std::vector<std::uint8_t>& unit) { return unit.at(0) & 0x1f; }

/* The SEI messages of every prefix SEI unit, by payload type. */
std::map<unsigned, std::vector<std::uint8_t>> hevcSeiPayloads(const std::vector<std::uint8_t>& frame)
{
    std::map<unsigned, std::vector<std::uint8_t>> messages;
    for (const auto& unit : annexBUnits(frame)) {
        if (hevcType(unit) != hevcPrefixSei) continue;
        const auto rbsp = unescaped(unit);
        std::size_t at = 2;
        while (at + 2 < rbsp.size()) {
            unsigned type = 0, size = 0;
            while (at < rbsp.size() && rbsp[at] == 0xff) type += 255, ++at;
            type += rbsp.at(at++);
            while (at < rbsp.size() && rbsp[at] == 0xff) size += 255, ++at;
            size += rbsp.at(at++);
            require(at + size <= rbsp.size(), "an SEI message overruns its unit");
            messages[type].assign(rbsp.begin() + std::ptrdiff_t(at), rbsp.begin() + std::ptrdiff_t(at + size));
            at += size;
        }
    }
    return messages;
}

/* general_profile_idc of an HEVC stream's SPS: the low five bits of the byte after the unit header and the VPS id byte. */
unsigned hevcProfileIdc(const std::vector<std::uint8_t>& frame)
{
    for (const auto& unit : annexBUnits(frame))
        if (hevcType(unit) == hevcSps) return unescaped(unit).at(3) & 0x1f;
    require(false, "the key frame has no SPS");
    return 0;
}

/* The level an SPS states: level_idc of H.264, and general_level_idc of HEVC, which follows the profile, its
 * compatibility flags and its six bytes of constraint flags. */
unsigned h264LevelIdc(const std::vector<std::uint8_t>& frame)
{
    for (const auto& unit : annexBUnits(frame))
        if (h264Type(unit) == h264Sps) return unescaped(unit).at(3);
    require(false, "the key frame has no SPS");
    return 0;
}
unsigned hevcLevelIdc(const std::vector<std::uint8_t>& frame)
{
    for (const auto& unit : annexBUnits(frame))
        if (hevcType(unit) == hevcSps) return unescaped(unit).at(14);
    require(false, "the key frame has no SPS");
    return 0;
}

/* A picture that is not flat, in the layout's own bytes. */
void fillRaw(std::uint8_t* out, RawVideoLayout layout, unsigned width, unsigned height, unsigned index)
{
    const auto luma = [&](unsigned x, unsigned y) {
        return std::uint8_t(40 + ((x + 2 * index) % 64) + (((x / 16 + y / 16) & 1) * 90) + ((x * 7 + y * 13 + index) % 17));
    };
    const auto chroma = [&](unsigned x, unsigned y, unsigned plane) {
        return std::uint8_t(plane == 0 ? 100 + (x + index) % 40 : 120 + y % 40);
    };
    switch (layout) {
    case RawVideoLayout::nv12:
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x) out[y * width + x] = luma(x, y);
        out += std::size_t(width) * height;
        for (unsigned y = 0; y < height / 2; ++y)
            for (unsigned x = 0; x < width / 2; ++x) {
                out[y * width + 2 * x] = chroma(x, y, 0);
                out[y * width + 2 * x + 1] = chroma(x, y, 1);
            }
        break;
    case RawVideoLayout::p010: {
        const auto put = [](std::uint8_t* at, std::uint8_t value) {
            const auto sample = std::uint16_t(std::uint16_t(value) << 8);  // ten bits in the high bits of sixteen
            at[0] = std::uint8_t(sample & 0xff);
            at[1] = std::uint8_t(sample >> 8);
        };
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x) put(out + 2 * (std::size_t(y) * width + x), luma(x, y));
        out += 2 * std::size_t(width) * height;
        for (unsigned y = 0; y < height / 2; ++y)
            for (unsigned x = 0; x < width / 2; ++x) {
                put(out + 2 * (std::size_t(y) * width + 2 * x), chroma(x, y, 0));
                put(out + 2 * (std::size_t(y) * width + 2 * x + 1), chroma(x, y, 1));
            }
        break;
    }
    case RawVideoLayout::bgra:
    case RawVideoLayout::rgba:
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x) {
                const auto at = out + 4 * (std::size_t(y) * width + x);
                at[0] = luma(x, y); at[1] = chroma(x, y, 0); at[2] = chroma(x, y, 1); at[3] = 0xff;
            }
        break;
    default:
        require(false, "a layout this case does not draw");
    }
}

struct CodedFrame {
    std::uint32_t flags {};
    std::uint64_t timestamp {};
    std::vector<std::uint8_t> bytes;
};
struct PlatformStream {
    std::vector<CodedFrame> frames;  // in completion order, the end marker excluded
    bool endMarker {};
};

constexpr unsigned realRawSlot = 64, realCodedSlot = 16;  // pages
std::size_t realRawBase(unsigned n) { return (16 + realRawSlot * (n - 1)) * page; }
std::size_t realCodedBase(unsigned k) { return (16 + realRawSlot * 16 + realCodedSlot * k) * page; }
constexpr std::size_t realMemory = 16 + realRawSlot * 16 + realCodedSlot * 20;  // pages
std::uint64_t realTimestamp(unsigned n) { return 1000 + 33 * std::uint64_t(n); }

/* Configures stream 1 as `codedFormat` from `layout` through `configure`,
 * queues `count` pictures and enough coded buffers for them and the end
 * marker, drains, and collects what the device completes, bounded in time. */
PlatformStream encodeOnPlatform(Fixture& f, std::uint32_t codedFormat, RawVideoLayout layout, unsigned width, unsigned height,
                                unsigned count, const std::function<void(Fixture&)>& configure = {})
{
    f.create(1, codedFormat);
    if (configure) configure(f);
    require(Fixture::type(f.setInput(1, wireRawFormat(layout), width, height, 1)) == VIRTIO_VIDEO_RESP_OK_NODATA, "the host's own format was refused");
    const auto raw = f.params(1, input);
    require(raw.num_planes == 1, "the raw buffer is not one buffer");
    const std::uint32_t rawSize = raw.plane_formats[0].plane_size;
    require(rawSize <= realRawSlot * page, "the picture does not fit its slot");
    for (unsigned n = 1; n <= count; ++n) {
        fillRaw(&f.memory[realRawBase(n)], layout, width, height, n);
        require(Fixture::type(f.attach(1, input, n, {{{realRawBase(n), rawSize, 0}}})) == VIRTIO_VIDEO_RESP_OK_NODATA, "raw attach");
    }
    std::map<std::uint64_t, unsigned> codedRequests;
    for (unsigned k = 0; k < count + 1; ++k) {
        require(Fixture::type(f.attach(1, output, codedFirst + k, {{{realCodedBase(k), realCodedSlot * page, 0}}})) == VIRTIO_VIDEO_RESP_OK_NODATA,
                "coded attach");
        codedRequests[f.next] = k;
        require(f.send(f.queueCommand(1, output, codedFirst + k)).action == VhostUserQueueAction::defer, "a coded buffer was not deferred");
    }
    std::set<std::uint64_t> pending;
    for (unsigned n = 1; n <= count; ++n) {
        auto command = f.queueCommand(1, input, n, realTimestamp(n));
        command.data_sizes[0] = rawSize;
        command.data_sizes[1] = 0;
        pending.insert(f.next);
        require(f.send(command).action == VhostUserQueueAction::defer, "a raw buffer was not deferred");
    }
    const auto drainRequest = f.next;
    pending.insert(drainRequest);
    require(f.send(virtio_video_stream_drain {{VIRTIO_VIDEO_CMD_STREAM_DRAIN, 1}}).action == VhostUserQueueAction::defer, "drain was not deferred");

    PlatformStream result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (true) {
        for (const auto& completion : f.completions()) {
            if (const auto coded = codedRequests.find(completion.request); coded != codedRequests.end()) {
                require(completion.responseType() == VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE, "a coded buffer failed");
                const auto buffer = completion.buffer();
                require(!(buffer.flags & VIRTIO_VIDEO_DEQUEUE_FLAG_ERR), "the platform failed a frame");
                require(buffer.data_sizes[0] <= realCodedSlot * page, "a coded frame overran its buffer");
                if (buffer.flags & VIRTIO_VIDEO_DEQUEUE_FLAG_EOS) {
                    require(buffer.data_sizes[0] == 0 && !result.endMarker, "the end marker");
                    result.endMarker = true;
                } else {
                    const auto from = f.memory.begin() + std::ptrdiff_t(realCodedBase(coded->second));
                    result.frames.push_back({buffer.flags, buffer.timestamp, {from, from + buffer.data_sizes[0]}});
                }
                codedRequests.erase(coded);
            } else {
                require(pending.erase(completion.request) == 1, "a completion nobody asked for");
                require(completion.responseType() == (completion.request == drainRequest ? VIRTIO_VIDEO_RESP_OK_NODATA
                                                                                          : VIRTIO_VIDEO_RESP_OK_RESOURCE_QUEUE),
                        "a raw buffer or the drain failed");
                require(completion.request == drainRequest || !(completion.buffer().flags & VIRTIO_VIDEO_DEQUEUE_FLAG_ERR),
                        "the platform encoder refused the stream the host's own capability lists");
            }
        }
        if (pending.empty() && codedRequests.empty()) break;
        require(std::chrono::steady_clock::now() < deadline, "the platform encoder did not finish the stream in a minute");
        if (f.readable(200)) f.device.drainEventSource();
    }
    return result;
}

/* What every stream the platform encodes has to look like to the guest. */
void requireStream(const PlatformStream& stream, unsigned count, const char* what)
{
    require(stream.frames.size() == count && stream.endMarker, what);
    for (unsigned index = 0; index < count; ++index) {
        const auto& frame = stream.frames[index];
        require(!frame.bytes.empty(), what);
        require(frame.timestamp == realTimestamp(index + 1), what);
        require(index == 0 ? frame.flags == VIRTIO_VIDEO_DEQUEUE_FLAG_KEY_FRAME : frame.flags == VIRTIO_VIDEO_DEQUEUE_FLAG_PFRAME, what);
    }
}

void platformEncoder()
{
    const auto capabilities = videoEncodeCapabilities();
    if (capabilities.empty()) {
        std::puts("  no hardware video encoder on this host; skipped");
        return;
    }
    const auto lists = [&](VideoCodec codec, VideoEncodeProfile profile, RawVideoLayout layout) {
        for (const auto& capability : capabilities)
            if (capability.codec == codec)
                for (const auto& entry : capability.profiles)
                    if (entry.profile == profile && std::find(entry.inputs.begin(), entry.inputs.end(), layout) != entry.inputs.end())
                        return true;
        return false;
    };
    const auto make = [&] {
        return std::make_unique<Fixture>(std::function<void(SyntheticEncoder&)> {}, capabilities, realMemory * page,
                                         [] { return makeVideoEncoder(); });
    };
    constexpr unsigned count = 6, width = 128, height = 96;

    // The device lists each raw format on the wire when a profile of the host encoded from it.
    {
        const auto f = make();
        const auto response = f->send(virtio_video_query_capability {{VIRTIO_VIDEO_CMD_QUERY_CAPABILITY, 0}, input, {}});
        const auto header = load<virtio_video_query_capability_resp>(response.response);
        require(header.hdr.type == VIRTIO_VIDEO_RESP_OK_QUERY_CAPABILITY, "capability query");
        std::set<std::uint32_t> formats;
        std::size_t offset = sizeof(header);
        for (std::uint32_t index = 0; index < header.num_descs; ++index) {
            const auto desc = load<virtio_video_format_desc>(response.response, offset);
            formats.insert(desc.format);
            offset += sizeof(desc) + sizeof(virtio_video_format_frame) + sizeof(virtio_video_format_range);
        }
        for (const auto layout : {RawVideoLayout::nv12, RawVideoLayout::p010, RawVideoLayout::bgra, RawVideoLayout::rgba}) {
            bool any = false;
            for (const auto& capability : capabilities)
                for (const auto& entry : capability.profiles)
                    any = any || std::find(entry.inputs.begin(), entry.inputs.end(), layout) != entry.inputs.end();
            require(formats.count(wireRawFormat(layout)) == (any ? 1u : 0u), "the wire does not list the host's raw formats");
        }
    }

    // NV12 into H.264 and into plain HEVC: the profile the guest gets is the one it did not change.
    if (lists(VideoCodec::h264, VideoEncodeProfile::h264High, RawVideoLayout::nv12)) {
        const auto f = make();
        const auto stream = encodeOnPlatform(*f, VIRTIO_VIDEO_FORMAT_H264, RawVideoLayout::nv12, width, height, count);
        requireStream(stream, count, "H.264 from NV12");
        bool sps = false, idr = false;
        for (const auto& unit : annexBUnits(stream.frames[0].bytes)) sps = sps || h264Type(unit) == h264Sps, idr = idr || h264Type(unit) == h264Idr;
        require(sps && idr, "the first H.264 frame is not a key frame with its parameter sets");
    } else {
        std::puts("  no H.264 from NV12 on this host; skipped");
    }
    if (lists(VideoCodec::hevc, VideoEncodeProfile::hevcMain, RawVideoLayout::nv12)) {
        const auto f = make();
        const auto stream = encodeOnPlatform(*f, VIRTIO_VIDEO_FORMAT_HEVC, RawVideoLayout::nv12, width, height, count);
        requireStream(stream, count, "HEVC from NV12");
        require(hevcProfileIdc(stream.frames[0].bytes) == 1, "an HEVC stream the guest did not configure is not Main");
        require(hevcSeiPayloads(stream.frames[0].bytes).count(137) == 0, "HDR metadata nobody asked for");
    }

    // Levels: a stream states the level it asked for, or the lowest it fits, so the default is not the host's top
    // and a level asked for is the level stated.
    const auto capabilityOf = [&](VideoCodec codec) -> const VideoEncodeCapability* {
        for (const auto& capability : capabilities)
            if (capability.codec == codec) return &capability;
        return nullptr;
    };
    if (lists(VideoCodec::h264, VideoEncodeProfile::h264High, RawVideoLayout::nv12) && capabilityOf(VideoCodec::h264)->profiles.front().maximumLevel >= 51) {
        const auto defaults = encodeOnPlatform(*make(), VIRTIO_VIDEO_FORMAT_H264, RawVideoLayout::nv12, width, height, 3);
        const auto stated = h264LevelIdc(defaults.frames[0].bytes);
        require(stated >= 10 && stated < 40, "an H.264 stream that named no level did not state the lowest one it fits");
        const auto asked = encodeOnPlatform(*make(), VIRTIO_VIDEO_FORMAT_H264, RawVideoLayout::nv12, width, height, 3, [](Fixture& g) {
            require(Fixture::type(setControls(g, 1, ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_H264_LEVEL, UML_V4L2_MPEG_VIDEO_H264_LEVEL_5_1))) ==
                    VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS, "level 5.1 refused");
        });
        require(h264LevelIdc(asked.frames[0].bytes) == 51, "an H.264 stream did not state the level it asked for");
    }
    if (lists(VideoCodec::hevc, VideoEncodeProfile::hevcMain, RawVideoLayout::nv12) && capabilityOf(VideoCodec::hevc)->profiles.front().levelSelectable &&
        capabilityOf(VideoCodec::hevc)->profiles.front().maximumLevel >= 153) {
        const auto defaults = encodeOnPlatform(*make(), VIRTIO_VIDEO_FORMAT_HEVC, RawVideoLayout::nv12, width, height, 3);
        const auto stated = hevcLevelIdc(defaults.frames[0].bytes);
        require(stated >= 30 && stated < 120, "an HEVC stream that named no level did not state the lowest one it fits");
        const auto asked = encodeOnPlatform(*make(), VIRTIO_VIDEO_FORMAT_HEVC, RawVideoLayout::nv12, width, height, 3, [](Fixture& g) {
            require(Fixture::type(setControls(g, 1, ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_HEVC_LEVEL, UML_V4L2_MPEG_VIDEO_HEVC_LEVEL_5_1))) ==
                    VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS, "HEVC level 5.1 refused");
        });
        require(hevcLevelIdc(asked.frames[0].bytes) == 153, "an HEVC stream did not state the level it asked for");
    }

    // B-frames: the platform codes its whole run or none. A request below the run keeps the stream in order, and one
    // at the run reorders it; the frames still come back in decode order with their own timestamps.
    for (const auto [codec, coded, profile, name] : {std::tuple {VideoCodec::h264, VIRTIO_VIDEO_FORMAT_H264, VideoEncodeProfile::h264High, "H.264"},
                                                     std::tuple {VideoCodec::hevc, VIRTIO_VIDEO_FORMAT_HEVC, VideoEncodeProfile::hevcMain, "HEVC"}}) {
        const auto* capability = capabilityOf(codec);
        if (!capability || capability->maximumBFrames < 2 || !lists(codec, profile, RawVideoLayout::nv12)) {
            std::printf("  no %s B-frames on this host; skipped\n", name);
            continue;
        }
        const unsigned frames = 14, run = capability->maximumBFrames;
        const auto coded_with = [&](unsigned asked) {
            return encodeOnPlatform(*make(), coded, RawVideoLayout::nv12, width, height, frames, [asked](Fixture& g) {
                require(Fixture::type(setControls(g, 1, ControlSet().scalar(UML_V4L2_CID_MPEG_VIDEO_B_FRAMES, asked))) ==
                        VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS, "the host's own B-frame run was refused");
            });
        };
        const auto order = [](const PlatformStream& stream) {
            bool ascending = true, bidirectional = false;
            for (std::size_t index = 0; index < stream.frames.size(); ++index) {
                bidirectional = bidirectional || (stream.frames[index].flags & VIRTIO_VIDEO_DEQUEUE_FLAG_BFRAME);
                if (index > 0 && stream.frames[index].timestamp < stream.frames[index - 1].timestamp) ascending = false;
            }
            return std::pair {ascending, bidirectional};
        };
        const auto below = coded_with(run - 1);
        require(below.frames.size() == frames && below.endMarker, name);
        require(order(below) == std::pair {true, false}, "a request below the host's B-frame run coded B-frames");
        const auto whole = coded_with(run);
        require(whole.frames.size() == frames && whole.endMarker, name);
        require(order(whole) == std::pair {false, true}, "a request for the host's whole B-frame run did not reorder");
        std::set<std::uint64_t> timestamps;
        for (const auto& frame : whole.frames) timestamps.insert(frame.timestamp);
        require(timestamps.size() == frames && *timestamps.begin() == realTimestamp(1) && *timestamps.rbegin() == realTimestamp(frames),
                "a reordered stream lost or invented a timestamp");
    }

    // Android's surfaces: packed pixels, converted by the platform.
    for (const auto layout : {RawVideoLayout::bgra, RawVideoLayout::rgba}) {
        const char* name = layout == RawVideoLayout::bgra ? "BGRA" : "RGBA";
        const bool h264 = lists(VideoCodec::h264, VideoEncodeProfile::h264High, layout);
        if (!h264 && !lists(VideoCodec::hevc, VideoEncodeProfile::hevcMain, layout)) {
            std::printf("  no %s input on this host; skipped\n", name);
            continue;
        }
        const auto f = make();
        const auto stream = encodeOnPlatform(*f, h264 ? VIRTIO_VIDEO_FORMAT_H264 : VIRTIO_VIDEO_FORMAT_HEVC, layout, width, height, count);
        requireStream(stream, count, name);
    }

    // HEVC Main 10 from P010 with HDR10: the profile, the 10-bit layout and both SEI messages reach the coded stream.
    if (lists(VideoCodec::hevc, VideoEncodeProfile::hevcMain10, RawVideoLayout::p010)) {
        // Primaries are green, blue, red; the control carries all x and then all y, and the SEI interleaves them.
        const std::array<std::uint16_t, 8> mastering {8500, 39850, 6550, 2300, 35400, 14600, 15635, 16450};
        const std::uint32_t maximumLuminance = 10'000'000, minimumLuminance = 50;
        std::vector<std::uint8_t> display, light, expectedMastering, expectedLight;
        const auto le16 = [](std::vector<std::uint8_t>& out, unsigned value) { out.push_back(std::uint8_t(value)); out.push_back(std::uint8_t(value >> 8)); };
        const auto be16 = [](std::vector<std::uint8_t>& out, unsigned value) { out.push_back(std::uint8_t(value >> 8)); out.push_back(std::uint8_t(value)); };
        const auto le32 = [&](std::vector<std::uint8_t>& out, std::uint32_t value) { le16(out, value & 0xffff); le16(out, value >> 16); };
        const auto be32 = [&](std::vector<std::uint8_t>& out, std::uint32_t value) { be16(out, value >> 16); be16(out, value & 0xffff); };
        for (const auto x : {0, 2, 4}) le16(display, mastering[x]);
        for (const auto y : {1, 3, 5}) le16(display, mastering[y]);
        le16(display, mastering[6]); le16(display, mastering[7]);
        le32(display, maximumLuminance); le32(display, minimumLuminance);
        le16(light, 1000); le16(light, 400);
        for (const auto value : mastering) be16(expectedMastering, value);
        be32(expectedMastering, maximumLuminance); be32(expectedMastering, minimumLuminance);
        be16(expectedLight, 1000); be16(expectedLight, 400);

        const auto f = make();
        const auto stream = encodeOnPlatform(*f, VIRTIO_VIDEO_FORMAT_HEVC, RawVideoLayout::p010, width, height, count, [&](Fixture& g) {
            require(Fixture::type(setControls(g, 1, ControlSet()
                        .scalar(UML_V4L2_CID_MPEG_VIDEO_HEVC_PROFILE, UML_V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10)
                        .compound(UML_V4L2_CID_COLORIMETRY_HDR10_MASTERING_DISPLAY, display)
                        .compound(UML_V4L2_CID_COLORIMETRY_HDR10_CLL_INFO, light))) == VIRTIO_VIDEO_RESP_OK_V4L2_CONTROLS,
                    "Main 10 and the HDR10 controls were refused on a host that lists them");
            virtio_video_set_stream_info info {};
            info.hdr = {VIRTIO_VIDEO_CMD_SET_STREAM_INFO, 1};
            info.info = {input, 1, 30, UML_V4L2_COLORSPACE_BT2020, UML_V4L2_XFER_FUNC_SMPTE2084, UML_V4L2_YCBCR_ENC_BT2020, 0, {}};
            g.ok(info);
        });
        requireStream(stream, count, "HEVC Main 10 from P010");
        require(hevcProfileIdc(stream.frames[0].bytes) == 2, "the stream is not HEVC Main 10");
        const auto key = hevcSeiPayloads(stream.frames[0].bytes);
        require(key.count(137) == 1 && key.at(137) == expectedMastering, "the mastering display SEI is not the guest's control, byte for byte");
        require(key.count(144) == 1 && key.at(144) == expectedLight, "the content light level SEI is not the guest's control, byte for byte");
        for (unsigned index = 1; index < count; ++index) {
            const auto other = hevcSeiPayloads(stream.frames[index].bytes);
            require(other.count(137) == 0 && other.count(144) == 0, "a frame that is not a key frame states HDR metadata");
        }
    } else {
        std::puts("  no HEVC Main 10 from P010 on this host; skipped");
    }
}

} // namespace

int main()
{
    try {
        identity();
        queries();
        streams();
        parameters();
        streamInformation();
        controlCommands();
        encodeRoundTrip();
        crop();
        unalignedFrame();
        otherLayouts();
        backpressure();
        drain();
        clearing();
        failures();
        parameterSets();
        dynamicControls();
        bFrames();
        rawColorspace();
        jpeg();
        severalStreams();
        resetAndDestroy();
        activityLog();
        sharedMemoryBuffers();
        platformEncoder();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
