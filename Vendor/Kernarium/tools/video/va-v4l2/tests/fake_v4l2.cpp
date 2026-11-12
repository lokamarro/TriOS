// SPDX-License-Identifier: GPL-2.0-or-later
#include "fake_v4l2.hpp"

#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "../v4l2_time.hpp"

namespace uml::va::test {
namespace {

constexpr auto kOutput = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
constexpr auto kCapture = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

unsigned roundUp(unsigned value, unsigned unit) noexcept
{
    return unit ? (value + unit - 1) / unit * unit : value;
}

void hash(std::uint64_t& state, std::uint8_t byte) noexcept
{
    state = (state ^ byte) * 1099511628211ull;
}

void append(std::vector<std::uint8_t>& out, std::initializer_list<std::uint8_t> bytes)
{
    for (const std::uint8_t byte : bytes) out.push_back(byte);
}

// Big-endian, with every byte masked so that a field of small numbers never
// spells the start code that separates the stream's NAL units.
void appendBig(std::vector<std::uint8_t>& out, std::uint64_t value, unsigned bytes)
{
    for (unsigned index = bytes; index-- > 0;) out.push_back(std::uint8_t(value >> (8 * index)) ^ 0x5a);
}


// Bits for a parameter set, written the way the standards write them.
class BitWriter {
public:
    void u(std::uint64_t value, unsigned count)
    {
        for (unsigned index = count; index-- > 0;) bit(unsigned(value >> index) & 1u);
    }
    void ue(std::uint64_t value)
    {
        const std::uint64_t coded = value + 1;
        unsigned length = 0;
        while ((coded >> length) > 1) ++length;
        u(0, length);
        u(coded, length + 1);
    }
    // The payload with the stop bit, zero padding and emulation prevention.
    std::vector<std::uint8_t> finish()
    {
        bit(1);
        while (count_ % 8) bit(0);
        std::vector<std::uint8_t> out;
        int zeros = 0;
        for (const std::uint8_t byte : bytes_) {
            if (zeros >= 2 && byte <= 3) {
                out.push_back(3);
                zeros = 0;
            }
            out.push_back(byte);
            zeros = byte == 0 ? zeros + 1 : 0;
        }
        return out;
    }

private:
    void bit(unsigned value)
    {
        if (count_ % 8 == 0) bytes_.push_back(0);
        bytes_.back() |= std::uint8_t(value << (7 - count_ % 8));
        ++count_;
    }
    std::vector<std::uint8_t> bytes_;
    std::size_t count_ {};
};

unsigned alignUp(unsigned value, unsigned to) { return (value + to - 1) / to * to; }

constexpr std::uint8_t kH264LevelIdc[] {10, 9, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42, 50, 51, 52, 60, 61, 62};
constexpr std::uint8_t kHevcLevelIdc[] {30, 60, 63, 90, 93, 120, 123, 150, 153, 156, 180, 183, 186};

// An H.264 sequence parameter set for a picture of the given size, cropped from
// the macroblock-aligned size the way an encoder crops.
std::vector<std::uint8_t> h264Sps(std::int64_t profileItem, std::int64_t levelItem, unsigned width, unsigned height)
{
    const unsigned profile = profileItem == 0 || profileItem == 1 ? 66 : profileItem == 2 ? 77 : profileItem == 3 ? 88 : 100;
    const unsigned level = levelItem >= 0 && levelItem < std::int64_t(sizeof(kH264LevelIdc)) ? kH264LevelIdc[levelItem] : 40;
    const unsigned codedWidth = alignUp(width, 16);
    const unsigned codedHeight = alignUp(height, 16);
    BitWriter bits;
    bits.u(profile, 8);
    bits.u(profileItem == 1 ? 0x40 : 0, 8);
    bits.u(level, 8);
    bits.ue(0);
    if (profile == 100) {
        bits.ue(1);
        bits.ue(0);
        bits.ue(0);
        bits.u(0, 2);
    }
    bits.ue(0);
    bits.ue(2);
    bits.ue(1);
    bits.u(0, 1);
    bits.ue(codedWidth / 16 - 1);
    bits.ue(codedHeight / 16 - 1);
    bits.u(1, 1);
    bits.u(1, 1);
    const bool crop = codedWidth != width || codedHeight != height;
    bits.u(crop, 1);
    if (crop) {
        bits.ue(0);
        bits.ue((codedWidth - width) / 2);
        bits.ue(0);
        bits.ue((codedHeight - height) / 2);
    }
    bits.u(0, 1);
    return bits.finish();
}

// The start of an HEVC sequence parameter set: enough to say the profile, the
// level, the size and the cropping.
std::vector<std::uint8_t> hevcSps(std::int64_t profileItem, std::int64_t levelItem, std::int64_t tier, unsigned width,
                                  unsigned height)
{
    const unsigned profile = profileItem == 1 ? 3 : profileItem == 2 ? 2 : 1;
    const unsigned level = levelItem >= 0 && levelItem < std::int64_t(sizeof(kHevcLevelIdc)) ? kHevcLevelIdc[levelItem] : 120;
    const unsigned codedWidth = alignUp(width, 8);
    const unsigned codedHeight = alignUp(height, 8);
    BitWriter bits;
    bits.u(0, 4);
    bits.u(0, 3);
    bits.u(1, 1);
    bits.u(0, 2);
    bits.u(tier != 0, 1);
    bits.u(profile, 5);
    bits.u(std::uint64_t(1) << (31 - profile), 32);
    bits.u(0x9, 4);
    bits.u(0, 44);
    bits.u(level, 8);
    bits.ue(0);
    bits.ue(1);
    bits.ue(codedWidth);
    bits.ue(codedHeight);
    const bool crop = codedWidth != width || codedHeight != height;
    bits.u(crop, 1);
    if (crop) {
        bits.ue(0);
        bits.ue((codedWidth - width) / 2);
        bits.ue(0);
        bits.ue((codedHeight - height) / 2);
    }
    return bits.finish();
}

}

FakeEncoderNode::FakeEncoderNode() { buildTable(); }

FakeEncoderNode::FakeEncoderNode(const Options& options) : options_(options)
{
    buildTable();
}

void FakeEncoderNode::buildTable()
{
    const auto add = [this](std::uint32_t id, std::uint32_t type, std::int64_t minimum, std::int64_t maximum,
                            std::int64_t defaultValue, bool dynamic, std::uint64_t menuMask = 0,
                            std::uint32_t flags = 0, std::int64_t step = 1) {
        ControlInfo info;
        info.id = id;
        info.type = type;
        info.minimum = minimum;
        info.maximum = maximum;
        info.step = step;
        info.defaultValue = defaultValue;
        info.flags = flags;
        info.menuMask = menuMask;
        table_[id] = info;
        dynamic_[id] = dynamic;
    };
    if (options_.noControls) return;
    add(V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_CTRL_TYPE_MENU, 0, 2, 0, false, options_.bitrateModeMask);
    add(V4L2_CID_MPEG_VIDEO_BITRATE, V4L2_CTRL_TYPE_INTEGER, 1000, 240000000, 2000000, true);
    add(V4L2_CID_MPEG_VIDEO_BITRATE_PEAK, V4L2_CTRL_TYPE_INTEGER, 0, 240000000, 0, true);
    add(V4L2_CID_MPEG_VIDEO_CONSTANT_QUALITY, V4L2_CTRL_TYPE_INTEGER, 1, 100, 50, true);
    add(V4L2_CID_MPEG_VIDEO_FRAME_RC_ENABLE, V4L2_CTRL_TYPE_BOOLEAN, 0, 1, 1, false);
    add(V4L2_CID_MPEG_VIDEO_MB_RC_ENABLE, V4L2_CTRL_TYPE_BOOLEAN, 0, 1, 1, false);
    add(V4L2_CID_MPEG_VIDEO_GOP_SIZE, V4L2_CTRL_TYPE_INTEGER, 0, 2147483647, 0, false);
    add(V4L2_CID_MPEG_VIDEO_B_FRAMES, V4L2_CTRL_TYPE_INTEGER, 0, 2, 0, false);
    add(V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, V4L2_CTRL_TYPE_BUTTON, 0, 0, 0, true, 0, V4L2_CTRL_FLAG_WRITE_ONLY);
    add(V4L2_CID_MPEG_VIDEO_HEADER_MODE, V4L2_CTRL_TYPE_MENU, 0, 1, 1, false, 0x3);
    add(V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR, V4L2_CTRL_TYPE_BOOLEAN, 0, 1, 1, true);
    add(V4L2_CID_MPEG_VIDEO_VBV_SIZE, V4L2_CTRL_TYPE_INTEGER, 0, 2147483647, 0, false);
    add(V4L2_CID_MIN_BUFFERS_FOR_OUTPUT, V4L2_CTRL_TYPE_INTEGER, 2, 2, 2, false, 0, V4L2_CTRL_FLAG_READ_ONLY);
    // Baseline, constrained baseline, main, high and constrained high.
    add(V4L2_CID_MPEG_VIDEO_H264_PROFILE, V4L2_CTRL_TYPE_MENU, 0, 17, 2, false, options_.h264ProfileMask);
    add(V4L2_CID_MPEG_VIDEO_H264_LEVEL, V4L2_CTRL_TYPE_MENU, 0, 16, 11, false, (1ull << 17) - 1);
    add(V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE, V4L2_CTRL_TYPE_MENU, 0, 1, 1, false, 0x3);
    add(V4L2_CID_MPEG_VIDEO_H264_8X8_TRANSFORM, V4L2_CTRL_TYPE_BOOLEAN, 0, 1, 1, false);
    add(V4L2_CID_MPEG_VIDEO_H264_MIN_QP, V4L2_CTRL_TYPE_INTEGER, 0, 51, 0, false);
    add(V4L2_CID_MPEG_VIDEO_H264_MAX_QP, V4L2_CTRL_TYPE_INTEGER, 0, 51, 51, true);
    add(V4L2_CID_MPEG_VIDEO_H264_I_FRAME_QP, V4L2_CTRL_TYPE_INTEGER, 0, 51, 26, false);
    add(V4L2_CID_MPEG_VIDEO_H264_P_FRAME_QP, V4L2_CTRL_TYPE_INTEGER, 0, 51, 26, false);
    add(V4L2_CID_MPEG_VIDEO_H264_B_FRAME_QP, V4L2_CTRL_TYPE_INTEGER, 0, 51, 26, false);
    add(V4L2_CID_MPEG_VIDEO_H264_CPB_SIZE, V4L2_CTRL_TYPE_INTEGER, 0, 2147483647, 0, false);
    if (options_.hevc) {
        add(V4L2_CID_MPEG_VIDEO_HEVC_PROFILE, V4L2_CTRL_TYPE_MENU, 0, 2, 0, false, 0x5);
        add(V4L2_CID_MPEG_VIDEO_HEVC_LEVEL, V4L2_CTRL_TYPE_MENU, 0, 12, 5, false, (1ull << 13) - 1);
        add(V4L2_CID_MPEG_VIDEO_HEVC_TIER, V4L2_CTRL_TYPE_MENU, 0, 1, 0, false, 0x1);
        add(V4L2_CID_MPEG_VIDEO_HEVC_MIN_QP, V4L2_CTRL_TYPE_INTEGER, 0, 51, 0, false);
        add(V4L2_CID_MPEG_VIDEO_HEVC_MAX_QP, V4L2_CTRL_TYPE_INTEGER, 0, 51, 51, true);
        add(V4L2_CID_MPEG_VIDEO_HEVC_I_FRAME_QP, V4L2_CTRL_TYPE_INTEGER, 0, 51, 26, false);
        add(V4L2_CID_MPEG_VIDEO_HEVC_P_FRAME_QP, V4L2_CTRL_TYPE_INTEGER, 0, 51, 26, false);
        add(V4L2_CID_MPEG_VIDEO_HEVC_B_FRAME_QP, V4L2_CTRL_TYPE_INTEGER, 0, 51, 26, false);
    }
}

std::uint64_t FakeEncoderNode::checksum(const std::uint8_t* luma, std::size_t lumaPitch, const std::uint8_t* chroma,
                                        std::size_t chromaPitch, unsigned width, unsigned height)
{
    std::uint64_t state = 14695981039346656037ull;
    for (unsigned row = 0; row < height; ++row)
        for (unsigned column = 0; column < width; ++column) hash(state, luma[row * lumaPitch + column]);
    for (unsigned row = 0; row < (height + 1) / 2; ++row)
        for (unsigned column = 0; column < roundUp(width, 2); ++column) hash(state, chroma[row * chromaPitch + column]);
    return state;
}

FakeEncoderNode::Session* FakeEncoderNode::find(int fd)
{
    const auto found = sessions_.find(fd);
    return found == sessions_.end() ? nullptr : found->second.get();
}

std::int64_t FakeEncoderNode::control(std::uint32_t id) const
{
    // The session a test is looking at is the one that was configured last,
    // whichever descriptor of a pool it was given.
    const auto active = sessions_.find(focus_);
    if (active != sessions_.end()) {
        const auto found = active->second->values.find(id);
        if (found != active->second->values.end()) return found->second;
    }
    const auto found = table_.find(id);
    return found == table_.end() ? 0 : found->second.defaultValue;
}

unsigned FakeEncoderNode::codedWaiting() const
{
    unsigned count = 0;
    for (const auto& entry : sessions_) count += unsigned(entry.second->internal.size() + entry.second->codedDone.size());
    return count;
}

int FakeEncoderNode::open(const char* path)
{
    if (std::strcmp(path, kPath) != 0) { errno = ENOENT; return -1; }
    if (denyOpen) { errno = EACCES; return -1; }
    auto session = std::make_unique<Session>();
    session->fd = next_++;
    for (const auto& entry : table_) session->values[entry.first] = entry.second.defaultValue;
    const int fd = session->fd;
    sessions_[fd] = std::move(session);
    ++opens;
    return fd;
}

int FakeEncoderNode::close(int fd)
{
    if (!sessions_.erase(fd)) { errno = EBADF; return -1; }
    ++closes;
    return 0;
}

void FakeEncoderNode::stop(Session& session, unsigned type)
{
    if (type == kOutput) {
        session.rawOn = false;
        session.rawDone.clear();
        session.rawWaiting.clear();
        for (auto& buffer : session.raw) buffer.queued = false;
        session.internal.clear();
        session.started = false;
        session.sinceKey = 0;
    } else {
        session.codedOn = false;
        session.codedFree.clear();
        session.codedDone.clear();
        for (auto& buffer : session.coded) buffer.queued = false;
    }
}

int FakeEncoderNode::setFormat(Session& session, v4l2_format& format, bool apply)
{
    if (options_.failSetFormat) { errno = EINVAL; return -1; }
    auto& pix = format.fmt.pix_mp;
    if (format.type == kCapture) {
        if (pix.pixelformat != V4L2_PIX_FMT_H264 && !(options_.hevc && pix.pixelformat == V4L2_PIX_FMT_HEVC))
            pix.pixelformat = V4L2_PIX_FMT_H264;
        pix.num_planes = 1;
        pix.plane_fmt[0].bytesperline = 0;
        pix.plane_fmt[0].sizeimage = options_.codedSize
            ? options_.codedSize
            : std::max<std::uint32_t>(pix.width * pix.height * 3 / 2, 1u << 20);
        if (apply) session.capture = format;
        return 0;
    }
    const bool offeredM = options_.nv12m && pix.pixelformat == V4L2_PIX_FMT_NV12M;
    const bool offered = options_.nv12 && pix.pixelformat == V4L2_PIX_FMT_NV12;
    if (!offeredM && !offered) pix.pixelformat = options_.nv12m ? V4L2_PIX_FMT_NV12M : V4L2_PIX_FMT_NV12;
    const unsigned pitch = roundUp(pix.width, options_.pitchAlign);
    if (pix.pixelformat == V4L2_PIX_FMT_NV12M) {
        pix.num_planes = 2;
        pix.plane_fmt[0].bytesperline = pitch;
        pix.plane_fmt[0].sizeimage = pitch * pix.height;
        pix.plane_fmt[1].bytesperline = pitch;
        pix.plane_fmt[1].sizeimage = pitch * ((pix.height + 1) / 2);
    } else {
        pix.num_planes = 1;
        pix.plane_fmt[0].bytesperline = pitch;
        pix.plane_fmt[0].sizeimage = pitch * (pix.height + (pix.height + 1) / 2);
    }
    if (apply) {
        session.output = format;
        outputFormat = pix.pixelformat;
        outputPlanes = pix.num_planes;
        outputWidth = pix.width;
        outputHeight = pix.height;
        outputPitch[0] = pix.plane_fmt[0].bytesperline;
        outputPitch[1] = pix.plane_fmt[1].bytesperline;
        // Setting the raw format sets the coded frame size.
        session.capture.fmt.pix_mp.width = pix.width;
        session.capture.fmt.pix_mp.height = pix.height;
        if (session.capture.fmt.pix_mp.plane_fmt[0].sizeimage == 0 || !options_.codedSize)
            session.capture.fmt.pix_mp.plane_fmt[0].sizeimage = options_.codedSize
                ? options_.codedSize
                : std::max<std::uint32_t>(pix.width * pix.height * 3 / 2, 1u << 20);
    }
    return 0;
}

int FakeEncoderNode::queryExtControl(v4l2_query_ext_ctrl& query)
{
    const bool next = query.id & V4L2_CTRL_FLAG_NEXT_CTRL;
    const std::uint32_t id = query.id & ~(V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND);
    const ControlInfo* found = nullptr;
    if (next) {
        for (const auto& entry : table_)
            if (entry.first > id) { found = &entry.second; break; }
    } else {
        const auto exact = table_.find(id);
        if (exact != table_.end()) found = &exact->second;
    }
    if (!found) { errno = EINVAL; return -1; }
    query = {};
    query.id = found->id;
    query.type = found->type;
    query.minimum = found->minimum;
    query.maximum = found->maximum;
    query.step = found->step;
    query.default_value = found->defaultValue;
    query.flags = found->flags;
    query.elem_size = 4;
    query.elems = 1;
    return 0;
}

int FakeEncoderNode::setExtControls(Session& session, v4l2_ext_controls& controls)
{
    const bool streaming = session.rawOn || session.codedOn;
    for (unsigned index = 0; index < controls.count; ++index) {
        v4l2_ext_control& control = controls.controls[index];
        const std::int64_t value = control.value;
        int result = 0;
        const auto found = table_.find(control.id);
        if (found == table_.end()) {
            result = EINVAL;
        } else {
            const ControlInfo& info = found->second;
            if (info.flags & V4L2_CTRL_FLAG_READ_ONLY) result = EACCES;
            else if (streaming && !dynamic_[control.id]) result = EBUSY;
            else if (info.type == V4L2_CTRL_TYPE_MENU && (value < info.minimum || value > info.maximum ||
                                                           !((info.menuMask >> value) & 1u))) result = EINVAL;
            else if (info.type == V4L2_CTRL_TYPE_INTEGER &&
                     (value < info.minimum || value > info.maximum || (value - info.minimum) % info.step)) result = ERANGE;
            else if (info.type == V4L2_CTRL_TYPE_BOOLEAN && (value < 0 || value > 1)) result = ERANGE;
        }
        controlLog.push_back({control.id, value, streaming, result});
        if (result) {
            controls.error_idx = index;
            errno = result;
            return -1;
        }
        if (control.id == V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME) session.forceKey = true;
        else session.values[control.id] = value;
    }
    return 0;
}

int FakeEncoderNode::requestBuffers(Session& session, v4l2_requestbuffers& request)
{
    if (request.memory != V4L2_MEMORY_MMAP) { errno = EINVAL; return -1; }
    auto& buffers = request.type == kOutput ? session.raw : session.coded;
    if ((request.type == kOutput ? session.rawOn : session.codedOn)) { errno = EBUSY; return -1; }
    buffers.clear();
    if (request.count == 0) { ++requestBuffersFree; return 0; }
    const auto& format = request.type == kOutput ? session.output : session.capture;
    if (!format.fmt.pix_mp.pixelformat) { errno = EINVAL; return -1; }
    buffers.resize(request.count);
    for (auto& buffer : buffers) {
        buffer.planes.resize(format.fmt.pix_mp.num_planes);
        for (unsigned plane = 0; plane < format.fmt.pix_mp.num_planes; ++plane)
            buffer.planes[plane].assign(format.fmt.pix_mp.plane_fmt[plane].sizeimage, 0);
    }
    return 0;
}

int FakeEncoderNode::queryBuffer(Session& session, v4l2_buffer& buffer)
{
    auto& buffers = buffer.type == kOutput ? session.raw : session.coded;
    if (buffer.index >= buffers.size()) { errno = EINVAL; return -1; }
    const auto& source = buffers[buffer.index];
    buffer.length = unsigned(source.planes.size());
    for (unsigned plane = 0; plane < source.planes.size(); ++plane) {
        buffer.m.planes[plane].length = unsigned(source.planes[plane].size());
        buffer.m.planes[plane].m.mem_offset =
            ((buffer.type == kOutput ? 1u : 2u) << 28) | (buffer.index << 12) | plane;
    }
    return 0;
}

int FakeEncoderNode::queueBuffer(Session& session, v4l2_buffer& buffer)
{
    if (buffer.memory != V4L2_MEMORY_MMAP) { errno = EINVAL; return -1; }
    auto& buffers = buffer.type == kOutput ? session.raw : session.coded;
    if (buffer.index >= buffers.size() || buffers[buffer.index].queued) { errno = EINVAL; return -1; }
    Buffer& target = buffers[buffer.index];
    if (buffer.length != target.planes.size()) { errno = EINVAL; return -1; }
    target.queued = true;
    if (buffer.type == kOutput) {
        for (unsigned plane = 0; plane < target.planes.size(); ++plane)
            if (buffer.m.planes[plane].bytesused > target.planes[plane].size()) { target.queued = false; errno = EINVAL; return -1; }
        target.timestamp = toNanoseconds(buffer.timestamp);
        session.rawWaiting.push_back(buffer.index);
    } else {
        session.codedFree.push_back(buffer.index);
    }
    advance(session);
    return 0;
}

int FakeEncoderNode::dequeueBuffer(Session& session, v4l2_buffer& buffer)
{
    if (buffer.type == kOutput) {
        if (session.rawDone.empty()) { errno = EAGAIN; return -1; }
        const unsigned index = session.rawDone.front();
        session.rawDone.pop_front();
        session.raw[index].queued = false;
        buffer.index = index;
        buffer.flags = 0;
        buffer.m.planes[0].bytesused = 0;
        return 0;
    }
    if (session.codedDone.empty()) { errno = EAGAIN; return -1; }
    const unsigned index = session.codedDone.front();
    session.codedDone.pop_front();
    Buffer& source = session.coded[index];
    source.queued = false;
    buffer.index = index;
    buffer.flags = source.flags;
    buffer.timestamp = fromNanoseconds(source.timestamp);
    buffer.m.planes[0].bytesused = source.bytesused;
    buffer.m.planes[0].data_offset = 0;
    return 0;
}

void FakeEncoderNode::encodeOne(Session& session, unsigned index)
{
    Buffer& raw = session.raw[index];
    const auto& pix = session.output.fmt.pix_mp;
    const unsigned width = pix.width;
    const unsigned height = pix.height;
    const std::size_t pitch = pix.plane_fmt[0].bytesperline;
    const std::uint8_t* luma = raw.planes[0].data();
    const std::uint8_t* chroma = pix.pixelformat == V4L2_PIX_FMT_NV12M ? raw.planes[1].data() : luma + pitch * height;
    const std::size_t chromaPitch = pix.pixelformat == V4L2_PIX_FMT_NV12M ? pix.plane_fmt[1].bytesperline : pitch;
    const std::uint64_t sum = checksum(luma, pitch, chroma, chromaPitch, width, height);

    const std::int64_t gop = session.values[V4L2_CID_MPEG_VIDEO_GOP_SIZE];
    bool key = !session.started || (gop > 0 && std::int64_t(session.sinceKey) >= gop);
    if (session.forceKey && !options_.ignoreForceKey) key = true;
    session.forceKey = false;
    session.started = true;
    session.sinceKey = key ? 1 : session.sinceKey + 1;

    Coded coded;
    coded.timestamp = raw.timestamp;
    coded.key = key;
    coded.error = options_.errorFrame >= 0 && std::int64_t(session.ordinal) == options_.errorFrame;
    // An HEVC NAL unit header is two bytes: the type in bits 1..6 of the first,
    // and a one in the second (temporal id plus one).
    const bool hevc = session.capture.fmt.pix_mp.pixelformat == V4L2_PIX_FMT_HEVC;
    if (key && session.values[V4L2_CID_MPEG_VIDEO_PREPEND_SPSPPS_TO_IDR]) {
        if (hevc) {
            append(coded.data, {0, 0, 0, 1, 0x40, 0x01});
            append(coded.data, {0, 0, 0, 1, 0x42, 0x01});
            const auto sps = hevcSps(session.values[V4L2_CID_MPEG_VIDEO_HEVC_PROFILE],
                                     session.values[V4L2_CID_MPEG_VIDEO_HEVC_LEVEL],
                                     session.values[V4L2_CID_MPEG_VIDEO_HEVC_TIER], width, height);
            coded.data.insert(coded.data.end(), sps.begin(), sps.end());
            append(coded.data, {0, 0, 0, 1, 0x44, 0x01});
        } else {
            append(coded.data, {0, 0, 0, 1, 0x67});
            const auto sps = h264Sps(session.values[V4L2_CID_MPEG_VIDEO_H264_PROFILE],
                                     session.values[V4L2_CID_MPEG_VIDEO_H264_LEVEL], width, height);
            coded.data.insert(coded.data.end(), sps.begin(), sps.end());
            append(coded.data, {0, 0, 0, 1, 0x68, std::uint8_t(session.values[V4L2_CID_MPEG_VIDEO_H264_ENTROPY_MODE]),
                                std::uint8_t(session.values[V4L2_CID_MPEG_VIDEO_H264_8X8_TRANSFORM])});
        }
    }
    if (hevc)
        append(coded.data, {0, 0, 0, 1, std::uint8_t(key ? 0x26 : 0x02), 0x01});
    else
        append(coded.data, {0, 0, 0, 1, std::uint8_t(key ? 0x65 : 0x41)});
    appendBig(coded.data, sum, 8);
    appendBig(coded.data, raw.timestamp, 8);
    appendBig(coded.data, session.ordinal, 4);
    const std::uint64_t bitrate = std::uint64_t(session.values[V4L2_CID_MPEG_VIDEO_BITRATE]);
    const unsigned numerator = frameRateNumerator ? frameRateNumerator : 30;
    const unsigned denominator = frameRateDenominator ? frameRateDenominator : 1;
    const std::size_t target = std::min<std::uint64_t>(
        std::max<std::uint64_t>(bitrate / 8 * denominator / numerator, coded.data.size()),
        session.capture.fmt.pix_mp.plane_fmt[0].sizeimage + 1024);
    coded.data.resize(std::max(target, coded.data.size()), 0x55);

    frames.push_back({raw.timestamp, width, height, sum, key});
    ++session.ordinal;
    session.internal.push_back(std::move(coded));
}

bool FakeEncoderNode::moveCoded(Session& session)
{
    if (session.internal.empty() || session.codedFree.empty()) return false;
    Coded coded = std::move(session.internal.front());
    session.internal.pop_front();
    const unsigned index = session.codedFree.front();
    session.codedFree.pop_front();
    Buffer& target = session.coded[index];
    const std::size_t used = std::min(coded.data.size(), target.planes[0].size());
    std::memcpy(target.planes[0].data(), coded.data.data(), used);
    target.bytesused = std::uint32_t(used);
    target.timestamp = coded.timestamp;
    target.flags = (coded.key ? V4L2_BUF_FLAG_KEYFRAME : 0u) | (coded.error ? V4L2_BUF_FLAG_ERROR : 0u);
    session.codedDone.push_back(index);
    return true;
}

bool FakeEncoderNode::encodeWaiting(Session& session)
{
    if (!session.rawOn || !session.codedOn) return false;
    bool any = false;
    while (!session.rawWaiting.empty() && session.internal.size() < options_.codedLimit) {
        const unsigned index = session.rawWaiting.front();
        session.rawWaiting.pop_front();
        encodeOne(session, index);
        session.rawDone.push_back(index);
        any = true;
    }
    return any;
}

void FakeEncoderNode::advance(Session& session)
{
    if (!session.rawOn || !session.codedOn) return;
    for (bool again = true; again;) {
        again = encodeWaiting(session);
        while (!options_.holdCoded && moveCoded(session)) again = true;
    }
}

unsigned FakeEncoderNode::deliver(unsigned count)
{
    unsigned moved = 0;
    for (auto& entry : sessions_) {
        Session& session = *entry.second;
        while (moved < count) {
            // Frames the device was holding back go in as room appears.
            const bool encoded = encodeWaiting(session);
            if (moveCoded(session)) ++moved;
            else if (!encoded) break;
        }
        encodeWaiting(session);
    }
    return moved;
}

int FakeEncoderNode::ioctl(int fd, unsigned long request, void* argument)
{
    Session* session = find(fd);
    if (!session) { errno = EBADF; return -1; }
    switch (request) {
    case VIDIOC_QUERYCAP: {
        auto& capability = *static_cast<v4l2_capability*>(argument);
        capability = {};
        std::strcpy(reinterpret_cast<char*>(capability.driver), "uml-fake");
        std::strcpy(reinterpret_cast<char*>(capability.card), "stateful-encoder");
        capability.device_caps = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
        capability.capabilities = capability.device_caps | V4L2_CAP_DEVICE_CAPS;
        return 0;
    }
    case VIDIOC_ENUM_FMT: {
        auto& description = *static_cast<v4l2_fmtdesc*>(argument);
        std::vector<std::pair<std::uint32_t, bool>> formats;
        if (description.type == kCapture) {
            formats.push_back({V4L2_PIX_FMT_H264, true});
            if (options_.hevc) formats.push_back({V4L2_PIX_FMT_HEVC, true});
        } else if (description.type == kOutput) {
            if (options_.nv12m) formats.push_back({V4L2_PIX_FMT_NV12M, false});
            if (options_.nv12) formats.push_back({V4L2_PIX_FMT_NV12, false});
        } else {
            errno = EINVAL;
            return -1;
        }
        if (description.index >= formats.size()) { errno = EINVAL; return -1; }
        const unsigned index = description.index;
        const unsigned type = description.type;
        description = {};
        description.index = index;
        description.type = type;
        description.pixelformat = formats[index].first;
        description.flags = formats[index].second ? V4L2_FMT_FLAG_COMPRESSED : 0;
        return 0;
    }
    case VIDIOC_ENUM_FRAMESIZES: {
        auto& sizes = *static_cast<v4l2_frmsizeenum*>(argument);
        const bool good = sizes.pixel_format == V4L2_PIX_FMT_H264 ||
                          (options_.hevc && sizes.pixel_format == V4L2_PIX_FMT_HEVC);
        if (!good || sizes.index != 0) { errno = EINVAL; return -1; }
        const std::uint32_t format = sizes.pixel_format;
        sizes = {};
        sizes.pixel_format = format;
        sizes.type = V4L2_FRMSIZE_TYPE_STEPWISE;
        sizes.stepwise.min_width = sizes.stepwise.min_height = 16;
        sizes.stepwise.max_width = sizes.stepwise.max_height = format == V4L2_PIX_FMT_HEVC ? 8192 : 4096;
        sizes.stepwise.step_width = sizes.stepwise.step_height = 2;
        return 0;
    }
    case VIDIOC_QUERY_EXT_CTRL:
        return queryExtControl(*static_cast<v4l2_query_ext_ctrl*>(argument));
    case VIDIOC_QUERYMENU: {
        auto& menu = *static_cast<v4l2_querymenu*>(argument);
        const auto found = table_.find(menu.id);
        if (found == table_.end() || found->second.type != V4L2_CTRL_TYPE_MENU || menu.index > 63 ||
            !((found->second.menuMask >> menu.index) & 1u)) {
            errno = EINVAL;
            return -1;
        }
        return 0;
    }
    case VIDIOC_S_EXT_CTRLS:
        focus_ = fd;
        return setExtControls(*session, *static_cast<v4l2_ext_controls*>(argument));
    case VIDIOC_G_FMT: {
        auto& format = *static_cast<v4l2_format*>(argument);
        if (format.type != kOutput && format.type != kCapture) { errno = EINVAL; return -1; }
        const unsigned type = format.type;
        format = type == kOutput ? session->output : session->capture;
        format.type = type;
        return 0;
    }
    case VIDIOC_S_FMT: {
        auto& format = *static_cast<v4l2_format*>(argument);
        if (format.type != kOutput && format.type != kCapture) { errno = EINVAL; return -1; }
        if (session->rawOn || session->codedOn) { errno = EBUSY; return -1; }
        focus_ = fd;
        const int result = setFormat(*session, format, true);
        if (result == 0 && format.type == kCapture) codedFormat = format.fmt.pix_mp.pixelformat;
        return result;
    }
    case VIDIOC_S_PARM: {
        auto& parameters = *static_cast<v4l2_streamparm*>(argument);
        frameRateNumerator = parameters.parm.output.timeperframe.denominator;
        frameRateDenominator = parameters.parm.output.timeperframe.numerator;
        return 0;
    }
    case VIDIOC_UNSUBSCRIBE_EVENT:
    case VIDIOC_SUBSCRIBE_EVENT:
    case VIDIOC_ENCODER_CMD:
        return 0;
    case VIDIOC_REQBUFS:
        return requestBuffers(*session, *static_cast<v4l2_requestbuffers*>(argument));
    case VIDIOC_QUERYBUF:
        return queryBuffer(*session, *static_cast<v4l2_buffer*>(argument));
    case VIDIOC_QBUF:
        return queueBuffer(*session, *static_cast<v4l2_buffer*>(argument));
    case VIDIOC_DQBUF:
        return dequeueBuffer(*session, *static_cast<v4l2_buffer*>(argument));
    case VIDIOC_STREAMON: {
        if (options_.failStreamOn) { errno = EIO; return -1; }
        const unsigned type = *static_cast<unsigned*>(argument);
        if (type == kOutput) session->rawOn = true;
        else if (type == kCapture) session->codedOn = true;
        else { errno = EINVAL; return -1; }
        ++streamOns;
        advance(*session);
        return 0;
    }
    case VIDIOC_STREAMOFF: {
        const unsigned type = *static_cast<unsigned*>(argument);
        if (type != kOutput && type != kCapture) { errno = EINVAL; return -1; }
        ++streamOffs;
        stop(*session, type);
        return 0;
    }
    default:
        errno = ENOTTY;
        return -1;
    }
}

void* FakeEncoderNode::mmap(int fd, std::size_t length, std::int64_t offset)
{
    Session* session = find(fd);
    if (!session) return MAP_FAILED;
    const unsigned queue = unsigned(offset >> 28);
    const unsigned index = unsigned((offset >> 12) & 0xffff);
    const unsigned plane = unsigned(offset & 0xfff);
    auto& buffers = queue == 1 ? session->raw : session->coded;
    if (index >= buffers.size() || plane >= buffers[index].planes.size() ||
        buffers[index].planes[plane].size() != length) return MAP_FAILED;
    return buffers[index].planes[plane].data();
}

int FakeEncoderNode::munmap(void*, std::size_t) { return 0; }

int FakeEncoderNode::poll(pollfd* fds, unsigned long count, int timeoutMilliseconds)
{
    int ready = 0;
    for (unsigned long index = 0; index < count; ++index) {
        fds[index].revents = 0;
        Session* session = find(fds[index].fd);
        if (!session) continue;
        if ((fds[index].events & POLLIN) && !session->codedDone.empty()) fds[index].revents |= POLLIN;
        if ((fds[index].events & POLLOUT) && !session->rawDone.empty()) fds[index].revents |= POLLOUT;
        if (fds[index].revents) ++ready;
    }
    if (!ready && timeoutMilliseconds > 0) usleep(1000);
    return ready;
}

}
