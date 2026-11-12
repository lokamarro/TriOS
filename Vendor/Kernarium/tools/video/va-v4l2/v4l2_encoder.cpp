// SPDX-License-Identifier: GPL-2.0-or-later
#include "v4l2_encoder.hpp"

#include <linux/videodev2.h>
#include <sys/mman.h>
#include <dirent.h>
#include <poll.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "v4l2_time.hpp"

namespace uml::va {
namespace {

constexpr auto kOutput = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
constexpr auto kCapture = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
constexpr std::size_t kMostNotes = 32;
constexpr std::size_t kLeastCodedBuffer = 1u << 20;

std::string reason(const char* what, int number = errno)
{
    return std::string(what) + ": " + std::strerror(number);
}

std::string hex(std::uint32_t value)
{
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

unsigned long long roundUp(unsigned long long value, unsigned long long unit) noexcept
{
    return unit ? (value + unit - 1) / unit * unit : value;
}

}

const EncodeCodec* EncoderCapabilities::codec(std::uint32_t format) const noexcept
{
    for (const auto& entry : codecs)
        if (entry.format == format) return &entry;
    return nullptr;
}

const ControlInfo* EncoderCapabilities::control(std::uint32_t id) const noexcept
{
    const auto found = controls.find(id);
    return found == controls.end() ? nullptr : &found->second;
}

bool EncoderCapabilities::offersItem(std::uint32_t id, unsigned item) const noexcept
{
    const ControlInfo* info = control(id);
    return info && item < 64 && ((info->menuMask >> item) & 1u);
}

bool EncoderCapabilities::offersRaw(std::uint32_t format) const noexcept
{
    return std::find(rawFormats.begin(), rawFormats.end(), format) != rawFormats.end();
}

bool probeEncoder(V4l2Io& io, int descriptor, EncoderCapabilities& capabilities, std::string& error)
{
    v4l2_capability capability {};
    if (io.ioctl(descriptor, VIDIOC_QUERYCAP, &capability) < 0) { error = reason("query the node"); return false; }
    const unsigned caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) ? capability.device_caps
                                                                            : capability.capabilities;
    const unsigned wanted = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
    if ((caps & wanted) != wanted) { error = "not a multi-planar memory-to-memory node"; return false; }

    capabilities.rawFormats.clear();
    capabilities.codecs.clear();
    capabilities.controls.clear();
    for (unsigned index = 0;; ++index) {
        v4l2_fmtdesc description {};
        description.type = kCapture;
        description.index = index;
        if (io.ioctl(descriptor, VIDIOC_ENUM_FMT, &description) < 0) break;
        if (!(description.flags & V4L2_FMT_FLAG_COMPRESSED)) continue;
        EncodeCodec codec;
        codec.format = description.pixelformat;
        v4l2_frmsizeenum sizes {};
        sizes.index = 0;
        sizes.pixel_format = codec.format;
        if (io.ioctl(descriptor, VIDIOC_ENUM_FRAMESIZES, &sizes) == 0) {
            if (sizes.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                codec.minWidth = codec.maxWidth = sizes.discrete.width;
                codec.minHeight = codec.maxHeight = sizes.discrete.height;
                codec.stepWidth = codec.stepHeight = 1;
            } else {
                codec.minWidth = sizes.stepwise.min_width;
                codec.minHeight = sizes.stepwise.min_height;
                codec.maxWidth = sizes.stepwise.max_width;
                codec.maxHeight = sizes.stepwise.max_height;
                codec.stepWidth = std::max(sizes.stepwise.step_width, 1u);
                codec.stepHeight = std::max(sizes.stepwise.step_height, 1u);
            }
        }
        capabilities.codecs.push_back(codec);
    }
    if (capabilities.codecs.empty()) { error = "no compressed format on the capture queue"; return false; }
    for (unsigned index = 0;; ++index) {
        v4l2_fmtdesc description {};
        description.type = kOutput;
        description.index = index;
        if (io.ioctl(descriptor, VIDIOC_ENUM_FMT, &description) < 0) break;
        if (description.flags & V4L2_FMT_FLAG_COMPRESSED) continue;
        capabilities.rawFormats.push_back(description.pixelformat);
    }

    std::uint32_t after = 0;
    for (;;) {
        v4l2_query_ext_ctrl query {};
        query.id = after | V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
        if (io.ioctl(descriptor, VIDIOC_QUERY_EXT_CTRL, &query) < 0) break;
        after = query.id;
        if (query.flags & V4L2_CTRL_FLAG_DISABLED) continue;
        ControlInfo info;
        info.id = query.id;
        info.type = query.type;
        info.minimum = query.minimum;
        info.maximum = query.maximum;
        info.step = std::max<std::int64_t>(query.step, 1);
        info.defaultValue = query.default_value;
        info.flags = query.flags;
        if (query.type == V4L2_CTRL_TYPE_MENU || query.type == V4L2_CTRL_TYPE_INTEGER_MENU) {
            bool answered = false;
            const std::int64_t last = std::min<std::int64_t>(query.maximum, 63);
            for (std::int64_t item = std::max<std::int64_t>(query.minimum, 0); item <= last; ++item) {
                v4l2_querymenu menu {};
                menu.id = query.id;
                menu.index = unsigned(item);
                if (io.ioctl(descriptor, VIDIOC_QUERYMENU, &menu) == 0) {
                    info.menuMask |= 1ull << item;
                    answered = true;
                }
            }
            // A node that cannot enumerate menus offers the whole range.
            if (!answered && errno == ENOTTY)
                for (std::int64_t item = std::max<std::int64_t>(query.minimum, 0); item <= last; ++item)
                    info.menuMask |= 1ull << item;
        }
        capabilities.controls[info.id] = info;
    }
    return true;
}

std::string findEncoderDevice(V4l2Io& io)
{
    if (const char* wanted = std::getenv("UML_VA_V4L2_ENCODER_DEVICE"); wanted && *wanted) return wanted;
    std::vector<std::string> nodes;
    if (DIR* directory = opendir("/dev")) {
        while (const dirent* entry = readdir(directory))
            if (std::strncmp(entry->d_name, "video", 5) == 0) nodes.push_back(std::string("/dev/") + entry->d_name);
        closedir(directory);
    }
    std::sort(nodes.begin(), nodes.end());
    for (const auto& node : nodes) {
        const int descriptor = io.open(node.c_str());
        if (descriptor < 0) continue;
        EncoderCapabilities found;
        std::string ignored;
        const bool encoder = probeEncoder(io, descriptor, found, ignored);
        io.close(descriptor);
        if (encoder) return node;
    }
    return {};
}

std::unique_ptr<V4l2Encoder> V4l2Encoder::open(Lease lease, std::shared_ptr<const EncoderCapabilities> capabilities,
                                               const Settings& settings, std::string& error)
{
    if (!lease) { error = "no Virtio video encoder node"; return nullptr; }
    if (!capabilities) { error = "the encoder's capabilities are unknown"; return nullptr; }
    std::unique_ptr<V4l2Encoder> encoder(new V4l2Encoder);
    encoder->lease_ = std::move(lease);
    encoder->io_ = &encoder->lease_.io();
    encoder->fd_ = encoder->lease_.fd();
    encoder->capabilities_ = std::move(capabilities);
    if (!encoder->negotiate(settings, error)) return nullptr;
    return encoder;
}

V4l2Encoder::~V4l2Encoder()
{
    if (fd_ < 0) return;
    bool clean = true;
    if (codedStreaming_) {
        auto type = unsigned(kCapture);
        if (io_->ioctl(fd_, VIDIOC_STREAMOFF, &type) < 0) clean = false;
    }
    if (rawStreaming_) {
        auto type = unsigned(kOutput);
        if (io_->ioctl(fd_, VIDIOC_STREAMOFF, &type) < 0) clean = false;
    }
    unmap(coded_);
    unmap(raw_);
    // Free both queues so that the next session on this descriptor starts from
    // the initialisation sequence.
    for (const unsigned type : {unsigned(kCapture), unsigned(kOutput)}) {
        v4l2_requestbuffers request {};
        request.type = type;
        request.memory = V4L2_MEMORY_MMAP;
        request.count = 0;
        if (io_->ioctl(fd_, VIDIOC_REQBUFS, &request) < 0 && errno != EINVAL) clean = false;
    }
    lease_.giveBack(clean);
}

bool V4l2Encoder::negotiate(const Settings& settings, std::string& error)
{
    if (!settings.width || !settings.height || (settings.width | settings.height) & 1u) {
        error = "the encoder needs an even frame size";
        return false;
    }
    const EncodeCodec* codec = capabilities_->codec(settings.codec);
    if (!codec) { error = "the encoder does not produce that codec"; return false; }
    if (settings.width < codec->minWidth || settings.height < codec->minHeight ||
        settings.width > codec->maxWidth || settings.height > codec->maxHeight) {
        error = "the frame size is outside what the encoder accepts";
        return false;
    }

    // A descriptor that served an earlier session may still hold its events.
    v4l2_event_subscription subscription {};
    subscription.type = V4L2_EVENT_ALL;
    io_->ioctl(fd_, VIDIOC_UNSUBSCRIBE_EVENT, &subscription);

    // The initialisation sequence of a stateful encoder: the coded format
    // first, then the raw format, which also sets the coded frame size.
    v4l2_format coded {};
    coded.type = kCapture;
    coded.fmt.pix_mp.width = settings.width;
    coded.fmt.pix_mp.height = settings.height;
    coded.fmt.pix_mp.pixelformat = settings.codec;
    coded.fmt.pix_mp.num_planes = 1;
    coded.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (io_->ioctl(fd_, VIDIOC_S_FMT, &coded) < 0) { error = reason("set the coded format"); return false; }
    if (coded.fmt.pix_mp.pixelformat != settings.codec) {
        error = "the encoder substituted the coded format";
        return false;
    }

    rawFormat_ = 0;
    for (const std::uint32_t candidate : {std::uint32_t(V4L2_PIX_FMT_NV12M), std::uint32_t(V4L2_PIX_FMT_NV12)})
        if (capabilities_->offersRaw(candidate)) { rawFormat_ = candidate; break; }
    if (!rawFormat_) { error = "the encoder takes neither NV12 nor NV12M"; return false; }
    v4l2_format raw {};
    raw.type = kOutput;
    raw.fmt.pix_mp.width = settings.width;
    raw.fmt.pix_mp.height = settings.height;
    raw.fmt.pix_mp.pixelformat = rawFormat_;
    raw.fmt.pix_mp.num_planes = rawFormat_ == V4L2_PIX_FMT_NV12M ? 2 : 1;
    raw.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (io_->ioctl(fd_, VIDIOC_S_FMT, &raw) < 0) { error = reason("set the raw format"); return false; }
    if (raw.fmt.pix_mp.pixelformat != rawFormat_) { error = "the encoder substituted the raw format"; return false; }
    if (raw.fmt.pix_mp.width != settings.width || raw.fmt.pix_mp.height != settings.height) {
        error = "the encoder adjusted the frame size";
        return false;
    }
    width_ = settings.width;
    height_ = settings.height;
    rawPlanes_ = raw.fmt.pix_mp.num_planes;
    if (rawPlanes_ != (rawFormat_ == V4L2_PIX_FMT_NV12M ? 2u : 1u)) { error = "unusable raw plane count"; return false; }
    for (unsigned plane = 0; plane < rawPlanes_; ++plane) {
        const auto& layout = raw.fmt.pix_mp.plane_fmt[plane];
        pitch_[plane] = layout.bytesperline ? layout.bytesperline : width_;
        planeSize_[plane] = layout.sizeimage;
    }
    // The device's sizes decide where each row goes, so they are checked
    // against what a copy of the frame will touch.
    const std::size_t lumaRows = height_;
    const std::size_t chromaRows = (std::size_t(height_) + 1) / 2;
    const std::size_t chromaBytes = roundUp(width_, 2);
    if (pitch_[0] < width_) { error = "raw rows shorter than the frame"; return false; }
    if (rawFormat_ == V4L2_PIX_FMT_NV12M) {
        if (pitch_[1] < chromaBytes) { error = "raw chroma rows shorter than the frame"; return false; }
        if (planeSize_[0] < pitch_[0] * lumaRows || planeSize_[1] < pitch_[1] * chromaRows) {
            error = "raw planes smaller than the frame";
            return false;
        }
    } else if (planeSize_[0] < pitch_[0] * (lumaRows + chromaRows)) {
        error = "raw plane smaller than the frame";
        return false;
    }

    v4l2_format reread {};
    reread.type = kCapture;
    if (io_->ioctl(fd_, VIDIOC_G_FMT, &reread) < 0) { error = reason("read the coded format"); return false; }
    codedSize_ = reread.fmt.pix_mp.plane_fmt[0].sizeimage;
    if (!codedSize_) codedSize_ = std::max<std::size_t>(std::size_t(width_) * height_ * 3 / 2, kLeastCodedBuffer);

    v4l2_streamparm parameters {};
    parameters.type = kOutput;
    parameters.parm.output.timeperframe.numerator = std::max(settings.frameRateDenominator, 1u);
    parameters.parm.output.timeperframe.denominator = std::max(settings.frameRateNumerator, 1u);
    if (io_->ioctl(fd_, VIDIOC_S_PARM, &parameters) < 0)
        notes_.push_back(reason("set the frame rate"));

    // Controls outlive a session on the descriptor, so one that an earlier
    // session set and this one does not mention would otherwise still hold the
    // earlier session's value.
    if (lease_.reused()) resetControls();
    for (const auto& control : settings.controls) applyControl(control, false);

    if (!allocate(kOutput, std::max(settings.rawBuffers, 2u), raw_, error)) return false;
    if (!allocate(kCapture, std::max(settings.codedBuffers, 2u), coded_, error)) return false;
    for (unsigned index = 0; index < coded_.size(); ++index)
        if (!queueCoded(index, error)) return false;
    auto type = unsigned(kOutput);
    if (io_->ioctl(fd_, VIDIOC_STREAMON, &type) < 0) { error = reason("start the raw queue"); return false; }
    rawStreaming_ = true;
    type = unsigned(kCapture);
    if (io_->ioctl(fd_, VIDIOC_STREAMON, &type) < 0) { error = reason("start the coded queue"); return false; }
    codedStreaming_ = true;
    return true;
}

bool V4l2Encoder::allocate(unsigned type, unsigned count, std::vector<Buffer>& buffers, std::string& error)
{
    v4l2_requestbuffers request {};
    request.type = type;
    request.memory = V4L2_MEMORY_MMAP;
    request.count = count;
    if (io_->ioctl(fd_, VIDIOC_REQBUFS, &request) < 0) { error = reason("request buffers"); return false; }
    if (!request.count || request.count > 32) { error = "unusable video buffer count"; return false; }
    buffers.clear();
    for (unsigned index = 0; index < request.count; ++index) {
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = type;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        buffer.length = VIDEO_MAX_PLANES;
        buffer.m.planes = planes;
        if (io_->ioctl(fd_, VIDIOC_QUERYBUF, &buffer) < 0) { error = reason("query buffer"); return false; }
        if (!buffer.length || buffer.length > 3) { error = "unusable video plane count"; return false; }
        Buffer mapped;
        mapped.count = buffer.length;
        buffers.push_back(mapped);
        for (unsigned plane = 0; plane < buffer.length; ++plane) {
            void* address = io_->mmap(fd_, planes[plane].length, planes[plane].m.mem_offset);
            if (address == MAP_FAILED) { error = reason("map video buffer"); return false; }
            buffers.back().planes[plane] = {static_cast<std::uint8_t*>(address), planes[plane].length};
        }
    }
    return true;
}

void V4l2Encoder::unmap(std::vector<Buffer>& buffers)
{
    for (auto& buffer : buffers)
        for (unsigned plane = 0; plane < buffer.count; ++plane)
            if (buffer.planes[plane].data) io_->munmap(buffer.planes[plane].data, buffer.planes[plane].length);
    buffers.clear();
}

bool V4l2Encoder::setControl(std::uint32_t id, std::int64_t value, int& number)
{
    v4l2_ext_control control {};
    control.id = id;
    const ControlInfo* info = capabilities_->control(id);
    if (info && info->type == V4L2_CTRL_TYPE_INTEGER64) control.value64 = value;
    else control.value = std::int32_t(value);
    v4l2_ext_controls controls {};
    controls.which = V4L2_CTRL_WHICH_CUR_VAL;
    controls.count = 1;
    controls.controls = &control;
    if (io_->ioctl(fd_, VIDIOC_S_EXT_CTRLS, &controls) < 0) {
        number = errno;
        return false;
    }
    return true;
}

void V4l2Encoder::resetControls()
{
    for (const auto& entry : capabilities_->controls) {
        const ControlInfo& info = entry.second;
        if (info.flags & (V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_DISABLED)) continue;
        if (info.type != V4L2_CTRL_TYPE_INTEGER && info.type != V4L2_CTRL_TYPE_BOOLEAN &&
            info.type != V4L2_CTRL_TYPE_MENU && info.type != V4L2_CTRL_TYPE_INTEGER_MENU)
            continue;
        int number = 0;
        setControl(info.id, info.defaultValue, number);
    }
}

bool V4l2Encoder::applyControl(const ControlValue& control, bool streaming)
{
    const auto note = [this](std::string text) {
        if (notes_.size() < kMostNotes) notes_.push_back(std::move(text));
    };
    const ControlInfo* info = capabilities_->control(control.id);
    if (!info) { note("control " + hex(control.id) + " is not offered"); return false; }
    if (info->flags & (V4L2_CTRL_FLAG_READ_ONLY | V4L2_CTRL_FLAG_DISABLED)) {
        note("control " + hex(control.id) + " is read-only");
        return false;
    }
    std::int64_t value = control.value;
    switch (info->type) {
    case V4L2_CTRL_TYPE_MENU:
    case V4L2_CTRL_TYPE_INTEGER_MENU:
        if (value < 0 || value > 63 || !((info->menuMask >> value) & 1u)) {
            note("control " + hex(control.id) + " has no item " + std::to_string(value));
            return false;
        }
        break;
    case V4L2_CTRL_TYPE_BOOLEAN:
        value = value ? 1 : 0;
        break;
    case V4L2_CTRL_TYPE_BUTTON:
        value = 1;
        break;
    default: {
        const std::int64_t asked = value;
        value = std::clamp(value, info->minimum, info->maximum);
        if (info->step > 1) value = info->minimum + (value - info->minimum) / info->step * info->step;
        if (value != asked)
            note("control " + hex(control.id) + " asked " + std::to_string(asked) + ", used " + std::to_string(value));
        break;
    }
    }
    int number = 0;
    if (!setControl(control.id, value, number)) {
        note("control " + hex(control.id) + (streaming ? " while streaming: " : ": ") + std::strerror(number));
        return false;
    }
    return true;
}

unsigned V4l2Encoder::update(const std::vector<ControlValue>& controls)
{
    unsigned applied = 0;
    for (const auto& control : controls)
        if (applyControl(control, true)) ++applied;
    return applied;
}

bool V4l2Encoder::setFrameRate(unsigned numerator, unsigned denominator, std::string& error)
{
    v4l2_streamparm parameters {};
    parameters.type = kOutput;
    parameters.parm.output.timeperframe.numerator = std::max(denominator, 1u);
    parameters.parm.output.timeperframe.denominator = std::max(numerator, 1u);
    if (io_->ioctl(fd_, VIDIOC_S_PARM, &parameters) < 0) { error = reason("set the frame rate"); return false; }
    return true;
}

bool V4l2Encoder::queueCoded(unsigned index, std::string& error)
{
    if (index >= coded_.size()) { error = "coded buffer index out of range"; return false; }
    auto& buffer = coded_[index];
    v4l2_plane planes[VIDEO_MAX_PLANES] {};
    v4l2_buffer request {};
    request.type = kCapture;
    request.memory = V4L2_MEMORY_MMAP;
    request.index = index;
    request.length = buffer.count;
    request.m.planes = planes;
    for (unsigned plane = 0; plane < buffer.count; ++plane) planes[plane].length = buffer.planes[plane].length;
    if (io_->ioctl(fd_, VIDIOC_QBUF, &request) < 0) { error = reason("queue a coded buffer"); return false; }
    buffer.queued = true;
    return true;
}

bool V4l2Encoder::reclaimRaw(std::string& error)
{
    for (;;) {
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = kOutput;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.length = VIDEO_MAX_PLANES;
        buffer.m.planes = planes;
        if (io_->ioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN) return true;
            error = reason("reclaim a raw buffer");
            return false;
        }
        if (buffer.index < raw_.size()) raw_[buffer.index].queued = false;
    }
}

bool V4l2Encoder::pump(std::string& error)
{
    for (;;) {
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = kCapture;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.length = VIDEO_MAX_PLANES;
        buffer.m.planes = planes;
        if (io_->ioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
            // EPIPE is how a drained stateful encoder says there is nothing more.
            if (errno == EAGAIN || errno == EPIPE) return true;
            error = reason("collect a coded buffer");
            return false;
        }
        if (buffer.index >= coded_.size()) { error = "encoder returned an unknown buffer"; return false; }
        auto& mapped = coded_[buffer.index];
        mapped.queued = false;
        const bool last = (buffer.flags & V4L2_BUF_FLAG_LAST) != 0;
        const std::size_t used = std::min<std::size_t>(planes[0].bytesused, mapped.planes[0].length);
        if (!(last && !used)) {
            EncodedFrame frame;
            frame.timestamp = toNanoseconds(buffer.timestamp);
            frame.key = (buffer.flags & V4L2_BUF_FLAG_KEYFRAME) != 0;
            frame.error = (buffer.flags & V4L2_BUF_FLAG_ERROR) != 0 || !used;
            if (used) frame.data.assign(mapped.planes[0].data, mapped.planes[0].data + used);
            ready_.push_back(std::move(frame));
        }
        // The buffer goes straight back, so the device never runs out of
        // somewhere to put the next frame.
        if (!queueCoded(buffer.index, error)) return false;
    }
}

void V4l2Encoder::copyFrame(const RawFrame& frame, Buffer& target) const
{
    const std::size_t lumaBytes = frame.width;
    const std::size_t chromaBytes = roundUp(frame.width, 2);
    const std::size_t chromaRows = (std::size_t(frame.height) + 1) / 2;
    std::uint8_t* luma = target.planes[0].data;
    std::uint8_t* chroma = rawFormat_ == V4L2_PIX_FMT_NV12M ? target.planes[1].data
                                                            : target.planes[0].data + pitch_[0] * height_;
    const std::size_t chromaPitch = rawFormat_ == V4L2_PIX_FMT_NV12M ? pitch_[1] : pitch_[0];
    for (unsigned row = 0; row < frame.height; ++row)
        std::memcpy(luma + row * pitch_[0], frame.luma + row * frame.lumaPitch, lumaBytes);
    for (std::size_t row = 0; row < chromaRows; ++row)
        std::memcpy(chroma + row * chromaPitch, frame.chroma + row * frame.chromaPitch, chromaBytes);
}

bool V4l2Encoder::submit(const RawFrame& frame, std::uint64_t timestamp, bool forceKey, std::string& error)
{
    if (!frame.luma || !frame.chroma || frame.width != width_ || frame.height != height_) {
        error = "the picture is not the size of the stream";
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + bound_;
    unsigned index = 0;
    for (;;) {
        if (!reclaimRaw(error) || !pump(error)) return false;
        const auto free = std::find_if(raw_.begin(), raw_.end(), [](const Buffer& b) { return !b.queued; });
        if (free != raw_.end()) { index = unsigned(free - raw_.begin()); break; }
        if (std::chrono::steady_clock::now() >= deadline) {
            error = "the encoder kept every raw buffer";
            return false;
        }
        // A device that has too many coded frames waiting defers the raw
        // buffers, so a coded buffer arriving is as good as a raw one coming
        // back.
        pollfd waiting {fd_, POLLIN | POLLOUT, 0};
        if (io_->poll(&waiting, 1, 50) < 0 && errno != EINTR) { error = reason("wait for a raw buffer"); return false; }
    }
    Buffer& target = raw_[index];
    copyFrame(frame, target);

    if (forceKey) {
        int number = 0;
        if (!capabilities_->offers(V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME)) {
            if (notes_.size() < kMostNotes) notes_.push_back("the encoder cannot force a key frame");
        } else if (!setControl(V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1, number)) {
            if (notes_.size() < kMostNotes) notes_.push_back(reason("force a key frame", number));
        }
    }

    v4l2_plane planes[VIDEO_MAX_PLANES] {};
    v4l2_buffer buffer {};
    buffer.type = kOutput;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    buffer.length = target.count;
    buffer.m.planes = planes;
    buffer.timestamp = fromNanoseconds(timestamp);
    for (unsigned plane = 0; plane < target.count; ++plane) planes[plane].length = target.planes[plane].length;
    if (rawFormat_ == V4L2_PIX_FMT_NV12M) {
        planes[0].bytesused = unsigned(pitch_[0] * height_);
        planes[1].bytesused = unsigned(pitch_[1] * ((std::size_t(height_) + 1) / 2));
    } else {
        planes[0].bytesused = unsigned(pitch_[0] * (height_ + (std::size_t(height_) + 1) / 2));
    }
    if (io_->ioctl(fd_, VIDIOC_QBUF, &buffer) < 0) { error = reason("queue a raw frame"); return false; }
    target.queued = true;
    ++submitted_;
    return true;
}

bool V4l2Encoder::collect(EncodedFrame& frame, std::chrono::milliseconds wait, std::string& error)
{
    const auto deadline = std::chrono::steady_clock::now() + wait;
    for (;;) {
        if (!pump(error)) return false;
        if (!ready_.empty()) {
            frame = std::move(ready_.front());
            ready_.pop_front();
            ++collected_;
            return true;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) return false;
        pollfd waiting {fd_, POLLIN, 0};
        if (io_->poll(&waiting, 1, int(std::min<std::int64_t>(left, 50))) < 0 && errno != EINTR) {
            error = reason("wait for a coded frame");
            return false;
        }
    }
}

}
