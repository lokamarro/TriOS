// SPDX-License-Identifier: GPL-2.0-or-later
#include "v4l2_decoder.hpp"

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "v4l2_time.hpp"

namespace uml::va {
namespace {

constexpr auto kOutput = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
constexpr auto kCapture = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
constexpr unsigned kOutputBuffers = 2;
constexpr unsigned kCaptureBuffers = 8;

int call(int fd, unsigned long request, void* argument) noexcept
{
    int result;
    do { result = ioctl(fd, request, argument); } while (result < 0 && errno == EINTR);
    return result;
}

std::string reason(const char* what)
{
    return std::string(what) + ": " + std::strerror(errno);
}

bool acceptsFormat(int fd, std::uint32_t codedFormat) noexcept
{
    v4l2_capability capability {};
    if (call(fd, VIDIOC_QUERYCAP, &capability) < 0) return false;
    const auto wanted = V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING;
    if ((capability.device_caps & wanted) != wanted) return false;
    for (unsigned index = 0;; ++index) {
        v4l2_fmtdesc description {};
        description.type = kOutput;
        description.index = index;
        if (call(fd, VIDIOC_ENUM_FMT, &description) < 0) return false;
        if (description.pixelformat == codedFormat) return true;
    }
}

}

RgbCapture rgbCaptureRequest(const char* request) noexcept
{
    if (!request || !*request) return RgbCapture::automatic;
    const std::string wanted = request;
    if (wanted == "off" || wanted == "0" || wanted == "nv12") return RgbCapture::off;
    return RgbCapture::automatic;
}

CaptureMemory captureMemoryRequest(const char* request) noexcept
{
    if (!request || !*request) return CaptureMemory::automatic;
    const std::string wanted = request;
    if (wanted == "device" || wanted == "mmap" || wanted == "off" || wanted == "0")
        return CaptureMemory::device;
    return CaptureMemory::automatic;
}

bool useRgbCapture(RgbCapture policy, bool deviceOffers) noexcept
{
    return deviceOffers && policy != RgbCapture::off;
}

bool decoderOffersCapture(int descriptor, std::uint32_t fourcc)
{
    if (descriptor < 0 || !fourcc) return false;
    for (unsigned index = 0;; ++index) {
        v4l2_fmtdesc description {};
        description.type = kCapture;
        description.index = index;
        if (call(descriptor, VIDIOC_ENUM_FMT, &description) < 0) return false;
        if (description.pixelformat == fourcc) return true;
    }
}

std::string findDecoderDevice(std::uint32_t codedFormat)
{
    if (const char* override_ = std::getenv("UML_VA_V4L2_DEVICE"); override_ && *override_)
        return override_;
    std::vector<std::string> candidates {"/dev/video-dec0"};
    if (DIR* directory = opendir("/dev")) {
        std::vector<std::string> nodes;
        while (const dirent* entry = readdir(directory))
            if (std::strncmp(entry->d_name, "video", 5) == 0)
                nodes.push_back(std::string("/dev/") + entry->d_name);
        closedir(directory);
        std::sort(nodes.begin(), nodes.end());
        candidates.insert(candidates.end(), nodes.begin(), nodes.end());
    }
    for (const auto& candidate : candidates) {
        const int fd = ::open(candidate.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        const bool good = acceptsFormat(fd, codedFormat);
        ::close(fd);
        if (good) return candidate;
    }
    return {};
}

bool capturedPlaneFits(std::uint32_t format, unsigned plane, unsigned width, unsigned height,
                       std::size_t pitch, std::size_t offset, std::size_t length) noexcept
{
    if (offset > length) return false;
    std::size_t rows = 0;
    std::size_t rowBytes = 0;
    if (format == V4L2_PIX_FMT_ABGR32) {
        if (plane == 0) {
            rows = height;
            rowBytes = std::size_t(width) * 4;
        }
    } else if (plane == 0) {
        rows = height;
        rowBytes = width;
    } else if (plane == 1) {
        rows = (std::size_t(height) + 1) / 2;
        rowBytes = (std::size_t(width) + 1) / 2 * 2;
    }
    if (!rows || !rowBytes) return true;
    const std::size_t room = length - offset;
    if (rowBytes > room) return false;
    // The last row starts (rows - 1) pitches in and reads rowBytes from there.
    return rows == 1 || pitch <= (room - rowBytes) / (rows - 1);
}

bool queryCodecLimits(int descriptor, std::uint32_t codedFormat, unsigned& minWidth,
                      unsigned& minHeight, unsigned& maxWidth, unsigned& maxHeight)
{
    if (descriptor < 0) return false;
    v4l2_frmsizeenum sizes {};
    sizes.index = 0;
    sizes.pixel_format = codedFormat;
    if (call(descriptor, VIDIOC_ENUM_FRAMESIZES, &sizes) != 0) return false;
    if (sizes.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
        minWidth = maxWidth = sizes.discrete.width;
        minHeight = maxHeight = sizes.discrete.height;
        return true;
    }
    minWidth = sizes.stepwise.min_width;
    minHeight = sizes.stepwise.min_height;
    maxWidth = sizes.stepwise.max_width;
    maxHeight = sizes.stepwise.max_height;
    return minWidth && minHeight && maxWidth && maxHeight;
}

std::unique_ptr<V4l2Decoder> V4l2Decoder::open(Lease lease, std::uint32_t codedFormat,
                                               unsigned width, unsigned height,
                                               std::uint32_t preferredCapture, std::string& error)
{
    if (!lease) { error = "no Virtio video decoder node"; return nullptr; }
    std::unique_ptr<V4l2Decoder> decoder(new V4l2Decoder);
    decoder->preferredCapture_ = preferredCapture;
    decoder->fd_ = lease.fd();
    decoder->lease_ = std::move(lease);
    if (!acceptsFormat(decoder->fd_, codedFormat)) { error = "decoder does not offer the codec"; return nullptr; }
    // A descriptor that served an earlier session may still hold its events.
    v4l2_event_subscription subscription {};
    subscription.type = V4L2_EVENT_ALL;
    call(decoder->fd_, VIDIOC_UNSUBSCRIBE_EVENT, &subscription);
    subscription.type = V4L2_EVENT_SOURCE_CHANGE;
    if (call(decoder->fd_, VIDIOC_SUBSCRIBE_EVENT, &subscription) < 0) { error = reason("subscribe source change"); return nullptr; }
    subscription.type = V4L2_EVENT_EOS;
    call(decoder->fd_, VIDIOC_SUBSCRIBE_EVENT, &subscription);
    if (!decoder->setupOutput(codedFormat, width, height, error)) return nullptr;
    // The decoded format is negotiated before the first packet: the device
    // keeps it across the source-change event, and changing it once a decode
    // session exists means asking the host to switch layout underneath it.
    if (preferredCapture) {
        v4l2_format wanted {};
        wanted.type = kCapture;
        wanted.fmt.pix_mp.width = width;
        wanted.fmt.pix_mp.height = height;
        wanted.fmt.pix_mp.pixelformat = preferredCapture;
        wanted.fmt.pix_mp.num_planes = 1;
        wanted.fmt.pix_mp.field = V4L2_FIELD_NONE;
        wanted.fmt.pix_mp.plane_fmt[0].bytesperline = width * 4;
        wanted.fmt.pix_mp.plane_fmt[0].sizeimage = width * 4 * height;
        if (call(decoder->fd_, VIDIOC_S_FMT, &wanted) < 0 ||
            wanted.fmt.pix_mp.pixelformat != preferredCapture)
            decoder->preferredCapture_ = 0;
    }
    return decoder;
}

V4l2Decoder::~V4l2Decoder()
{
    if (fd_ < 0) return;
    bool clean = true;
    if (captureStreaming_) {
        auto type = unsigned(kCapture);
        if (call(fd_, VIDIOC_STREAMOFF, &type) < 0) clean = false;
    }
    if (outputStreaming_) {
        auto type = unsigned(kOutput);
        if (call(fd_, VIDIOC_STREAMOFF, &type) < 0) clean = false;
    }
    unmap(capture_);
    unmap(output_);
    // Free both queues so that the next session on this descriptor starts from
    // the initialisation sequence. A queue that was never set up has nothing to
    // free, and that is not a failure.
    const auto freeQueue = [this, &clean](unsigned type, unsigned memory) {
        v4l2_requestbuffers request {};
        request.type = type;
        request.memory = memory;
        request.count = 0;
        if (call(fd_, VIDIOC_REQBUFS, &request) < 0 && errno != EINVAL) clean = false;
    };
    freeQueue(kCapture, captureImports_ ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP);
    freeQueue(kOutput, V4L2_MEMORY_MMAP);
    lease_.giveBack(clean);
}

bool V4l2Decoder::setupOutput(std::uint32_t codedFormat, unsigned width, unsigned height, std::string& error)
{
    v4l2_format format {};
    format.type = kOutput;
    format.fmt.pix_mp.width = width;
    format.fmt.pix_mp.height = height;
    format.fmt.pix_mp.pixelformat = codedFormat;
    format.fmt.pix_mp.num_planes = 1;
    format.fmt.pix_mp.plane_fmt[0].sizeimage = 4u * 1024u * 1024u;
    if (call(fd_, VIDIOC_S_FMT, &format) < 0) { error = reason("set coded format"); return false; }
    if (format.fmt.pix_mp.pixelformat != codedFormat) { error = "decoder substituted the coded format"; return false; }
    if (!allocate(kOutput, kOutputBuffers, output_, error)) return false;
    auto type = unsigned(kOutput);
    if (call(fd_, VIDIOC_STREAMON, &type) < 0) { error = reason("start the coded queue"); return false; }
    outputStreaming_ = true;
    width_ = width;
    height_ = height;
    return true;
}

bool V4l2Decoder::allocate(unsigned type, unsigned count, std::vector<Buffer>& buffers, std::string& error)
{
    v4l2_requestbuffers request {};
    request.type = type;
    request.memory = V4L2_MEMORY_MMAP;
    request.count = count;
    if (call(fd_, VIDIOC_REQBUFS, &request) < 0) { error = reason("request buffers"); return false; }
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
        if (call(fd_, VIDIOC_QUERYBUF, &buffer) < 0) { error = reason("query buffer"); return false; }
        if (!buffer.length || buffer.length > 3) { error = "unusable video plane count"; return false; }
        Buffer mapped;
        mapped.count = buffer.length;
        for (unsigned plane = 0; plane < buffer.length; ++plane) {
            void* address = mmap(nullptr, planes[plane].length, PROT_READ | PROT_WRITE, MAP_SHARED,
                                 fd_, off_t(planes[plane].m.mem_offset));
            if (address == MAP_FAILED) { error = reason("map video buffer"); return false; }
            mapped.planes[plane] = {static_cast<std::uint8_t*>(address), planes[plane].length};
        }
        buffers.push_back(mapped);
    }
    return true;
}

void V4l2Decoder::unmap(std::vector<Buffer>& buffers)
{
    for (auto& buffer : buffers)
        for (unsigned plane = 0; plane < buffer.count; ++plane)
            if (buffer.planes[plane].data) munmap(buffer.planes[plane].data, buffer.planes[plane].length);
    buffers.clear();
}

bool V4l2Decoder::pendingSourceChange(std::string& error)
{
    for (;;) {
        v4l2_event event {};
        if (call(fd_, VIDIOC_DQEVENT, &event) < 0) {
            if (errno == ENOENT || errno == EAGAIN || errno == EINVAL) return false;
            error = reason("dequeue decoder event");
            return false;
        }
        if (event.type == V4L2_EVENT_SOURCE_CHANGE &&
            (event.u.src_change.changes & V4L2_EVENT_SRC_CH_RESOLUTION))
            return true;
    }
}

bool V4l2Decoder::setupCapture(std::string& error)
{
    v4l2_format format {};
    format.type = kCapture;
    if (call(fd_, VIDIOC_G_FMT, &format) < 0) { error = reason("read the decoded format"); return false; }
    // A packed RGB capture format lets the host do the colour conversion the
    // caller would otherwise do on the guest processor, so it is asked for
    // first and the request falls back to NV12 when the device declines.
    captureFormat_ = 0;
    if (preferredCapture_ && format.fmt.pix_mp.pixelformat == preferredCapture_) {
        // Already negotiated before the stream started, and kept across the
        // source change: take the device's own plane layout as it stands.
        captureFormat_ = preferredCapture_;
    }
    if (!captureFormat_) {
        format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;
        if (call(fd_, VIDIOC_S_FMT, &format) < 0) { error = reason("set the decoded format"); return false; }
        if (format.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12M) {
            error = "decoder cannot produce NV12";
            return false;
        }
        captureFormat_ = V4L2_PIX_FMT_NV12M;
    }
    width_ = format.fmt.pix_mp.width;
    height_ = format.fmt.pix_mp.height;
    capturePlanes_ = format.fmt.pix_mp.num_planes;
    for (unsigned plane = 0; plane < 4; ++plane) {
        const unsigned reported = plane < capturePlanes_
            ? format.fmt.pix_mp.plane_fmt[plane].bytesperline : 0;
        capturePitch_[plane] = reported ? reported : width_;
        captureSize_[plane] = plane < capturePlanes_ ? format.fmt.pix_mp.plane_fmt[plane].sizeimage : 0;
    }
    layout_ = std::to_string(width_) + "x" + std::to_string(height_) + " planes " +
              std::to_string(capturePlanes_) + " pitch " +
              std::to_string(format.fmt.pix_mp.plane_fmt[0].bytesperline) + " size " +
              std::to_string(format.fmt.pix_mp.plane_fmt[0].sizeimage);
    captureImports_ = false;
    if (captureSource_) {
        std::string why;
        if (importCapture(why)) captureImports_ = true;
        else if (!why.empty()) importFailure_ = why;
    }
    if (!captureImports_) {
        if (!allocate(kCapture, kCaptureBuffers, capture_, error)) return false;
        for (unsigned index = 0; index < capture_.size(); ++index)
            if (!queueCapture(index, error)) return false;
    }
    auto type = unsigned(kCapture);
    if (call(fd_, VIDIOC_STREAMON, &type) < 0) { error = reason("start the decoded queue"); return false; }
    captureStreaming_ = true;
    return true;
}

bool V4l2Decoder::teardownCapture(std::string& error)
{
    if (!captureStreaming_) return true;
    auto type = unsigned(kCapture);
    if (call(fd_, VIDIOC_STREAMOFF, &type) < 0) { error = reason("stop the decoded queue"); return false; }
    captureStreaming_ = false;
    if (!captureImports_) unmap(capture_);
    capture_.clear();
    v4l2_requestbuffers request {};
    request.type = kCapture;
    request.memory = captureImports_ ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
    request.count = 0;
    call(fd_, VIDIOC_REQBUFS, &request);
    captureImports_ = false;
    return true;
}

bool V4l2Decoder::importCapture(std::string& error)
{
    // A queue that takes the caller's own buffers lets the device write each
    // picture where the client will read it, so nothing is copied in the guest.
    v4l2_requestbuffers request {};
    request.type = kCapture;
    request.memory = V4L2_MEMORY_DMABUF;
    request.count = kCaptureBuffers;
    if (call(fd_, VIDIOC_REQBUFS, &request) < 0) { error = reason("request imported buffers"); return false; }
    if (!request.count || request.count > 32) { error = "unusable imported buffer count"; return false; }
    capture_.assign(request.count, Buffer {});
    for (auto& buffer : capture_) buffer.count = capturePlanes_;
    for (unsigned index = 0; index < capture_.size(); ++index) {
        CaptureTarget target {};
        if (!captureSource_(index, index == 0, target) || !queueImported(index, target, error)) {
            // Hand the queue back whole rather than half-filled.
            v4l2_requestbuffers release {};
            release.type = kCapture;
            release.memory = V4L2_MEMORY_DMABUF;
            release.count = 0;
            call(fd_, VIDIOC_REQBUFS, &release);
            capture_.clear();
            if (error.empty()) error = "no buffer to lend the decoder";
            return false;
        }
    }
    return true;
}

bool V4l2Decoder::queueImported(unsigned index, const CaptureTarget& target, std::string& error)
{
    if (index >= capture_.size()) { error = "decoded buffer index out of range"; return false; }
    if (!target.planes || target.planes < capturePlanes_) { error = "lent buffer has too few planes"; return false; }
    v4l2_plane planes[VIDEO_MAX_PLANES] {};
    v4l2_buffer request {};
    request.type = kCapture;
    request.memory = V4L2_MEMORY_DMABUF;
    request.index = index;
    request.length = capturePlanes_;
    request.m.planes = planes;
    for (unsigned plane = 0; plane < capturePlanes_; ++plane) {
        planes[plane].m.fd = target.fd[plane];
        planes[plane].data_offset = unsigned(target.offset[plane]);
        planes[plane].length = unsigned(target.length[plane]);
    }
    if (call(fd_, VIDIOC_QBUF, &request) < 0) { error = reason("lend the decoder a buffer"); return false; }
    capture_[index].queued = true;
    return true;
}

bool V4l2Decoder::release(unsigned index, const CaptureTarget& target, std::string& error)
{
    if (index >= capture_.size()) { error = "decoded buffer index out of range"; return false; }
    capture_[index].queued = false;
    return queueImported(index, target, error);
}

bool V4l2Decoder::queueCapture(unsigned index, std::string& error)
{
    if (index >= capture_.size()) { error = "decoded buffer index out of range"; return false; }
    auto& buffer = capture_[index];
    if (buffer.queued) return true;
    v4l2_plane planes[VIDEO_MAX_PLANES] {};
    v4l2_buffer request {};
    request.type = kCapture;
    request.memory = V4L2_MEMORY_MMAP;
    request.index = index;
    request.length = buffer.count;
    request.m.planes = planes;
    for (unsigned plane = 0; plane < buffer.count; ++plane) planes[plane].length = buffer.planes[plane].length;
    if (call(fd_, VIDIOC_QBUF, &request) < 0) { error = reason("queue a decoded buffer"); return false; }
    buffer.queued = true;
    return true;
}

bool V4l2Decoder::reclaimOutput(std::string& error)
{
    for (;;) {
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = kOutput;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.length = VIDEO_MAX_PLANES;
        buffer.m.planes = planes;
        if (call(fd_, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN) return true;
            error = reason("reclaim a coded buffer");
            return false;
        }
        if (buffer.index < output_.size()) output_[buffer.index].queued = false;
    }
}

bool V4l2Decoder::submit(std::span<const std::uint8_t> packet, std::uint64_t timestamp, std::string& error)
{
    if (!reclaimOutput(error)) return false;
    auto free = std::find_if(output_.begin(), output_.end(), [](const Buffer& b) { return !b.queued; });
    if (free == output_.end()) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (free == output_.end() && std::chrono::steady_clock::now() < deadline) {
            pollfd waiting {fd_, POLLOUT, 0};
            if (poll(&waiting, 1, 50) < 0 && errno != EINTR) { error = reason("wait for a coded buffer"); return false; }
            if (!reclaimOutput(error)) return false;
            free = std::find_if(output_.begin(), output_.end(), [](const Buffer& b) { return !b.queued; });
        }
        if (free == output_.end()) { error = "the decoder kept every coded buffer"; return false; }
    }
    if (packet.size() > free->planes[0].length) { error = "compressed frame exceeds the coded buffer"; return false; }
    std::memcpy(free->planes[0].data, packet.data(), packet.size());
    const auto index = unsigned(free - output_.begin());
    v4l2_plane planes[VIDEO_MAX_PLANES] {};
    v4l2_buffer buffer {};
    buffer.type = kOutput;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    buffer.length = free->count;
    buffer.m.planes = planes;
    buffer.timestamp = fromNanoseconds(timestamp);
    for (unsigned plane = 0; plane < free->count; ++plane) planes[plane].length = free->planes[plane].length;
    planes[0].bytesused = unsigned(packet.size());
    if (call(fd_, VIDIOC_QBUF, &buffer) < 0) { error = reason("queue a compressed frame"); return false; }
    free->queued = true;
    if (!captureStreaming_) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        bool changed = pendingSourceChange(error);
        while (!changed && error.empty() && std::chrono::steady_clock::now() < deadline) {
            pollfd waiting {fd_, POLLPRI, 0};
            if (poll(&waiting, 1, 50) < 0 && errno != EINTR) { error = reason("wait for the decoded format"); return false; }
            changed = pendingSourceChange(error);
        }
        if (!error.empty()) return false;
        if (!changed) { error = "the decoder never reported a decoded format"; return false; }
        if (!setupCapture(error)) return false;
    }
    return true;
}

bool V4l2Decoder::collect(DecodedPicture& picture, std::chrono::milliseconds wait, std::string& error)
{
    if (!captureStreaming_) return false;
    const auto deadline = std::chrono::steady_clock::now() + wait;
    for (;;) {
        if (pendingSourceChange(error)) {
            // A mid-stream resolution change: give the queue back and build it
            // again at the new size. The picture that triggered it arrives next.
            if (!teardownCapture(error) || !setupCapture(error)) return false;
            continue;
        }
        if (!error.empty()) return false;
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = kCapture;
        buffer.memory = captureImports_ ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
        buffer.length = VIDEO_MAX_PLANES;
        buffer.m.planes = planes;
        if (call(fd_, VIDIOC_DQBUF, &buffer) == 0) {
            if (buffer.index >= capture_.size()) { error = "decoder returned an unknown buffer"; return false; }
            capture_[buffer.index].queued = false;
            const bool last = (buffer.flags & V4L2_BUF_FLAG_LAST) != 0;
            if (last && !planes[0].bytesused) {
                // An empty last buffer carries no picture: give it straight
                // back, lending another one when the queue imports.
                if (captureImports_) {
                    CaptureTarget target {};
                    if (captureSource_ && captureSource_(buffer.index, false, target))
                        queueImported(buffer.index, target, error);
                } else {
                    queueCapture(buffer.index, error);
                }
                continue;
            }
            picture = {};
            picture.timestamp = toNanoseconds(buffer.timestamp);
            picture.index = buffer.index;
            picture.width = width_;
            picture.height = height_;
            picture.planes = captureImports_ ? capturePlanes_ : capture_[buffer.index].count;
            picture.imported = captureImports_;
            picture.error = (buffer.flags & V4L2_BUF_FLAG_ERROR) != 0;
            for (unsigned plane = 0; plane < picture.planes; ++plane) {
                picture.pitch[plane] = capturePitch_[plane];
                const Plane& mapped = capture_[buffer.index].planes[plane];
                if (!mapped.data) continue;
                // The format's stride and height and the buffer's data offset
                // come from the device independently of the length it mapped,
                // so a picture they would read past the mapping fails instead.
                if (!capturedPlaneFits(captureFormat_, plane, width_, height_, capturePitch_[plane],
                                       planes[plane].data_offset, mapped.length)) {
                    picture.error = true;
                    continue;
                }
                picture.plane[plane] = mapped.data + planes[plane].data_offset;
            }
            return true;
        }
        // EPIPE is how a drained stateful decoder says there is nothing more.
        if (errno == EPIPE) return false;
        if (errno != EAGAIN) { error = reason("collect a decoded picture"); return false; }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        pollfd waiting {fd_, POLLIN | POLLPRI, 0};
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (poll(&waiting, 1, int(std::min<std::int64_t>(std::max<std::int64_t>(left, 0), 50))) < 0 && errno != EINTR) {
            error = reason("wait for a decoded picture");
            return false;
        }
    }
}

bool V4l2Decoder::release(unsigned index, std::string& error)
{
    if (captureImports_) {
        // A queue that imports needs a buffer with the descriptor, so a caller
        // that asks for a plain release gets the one it lent before.
        CaptureTarget target {};
        if (!captureSource_ || !captureSource_(index, false, target)) {
            error = "no buffer to lend the decoder";
            return false;
        }
        return queueImported(index, target, error);
    }
    return queueCapture(index, error);
}

bool V4l2Decoder::drain(std::string& error)
{
    if (!captureStreaming_) return true;
    v4l2_decoder_cmd command {};
    command.cmd = V4L2_DEC_CMD_STOP;
    if (call(fd_, VIDIOC_DECODER_CMD, &command) < 0) { error = reason("drain the decoder"); return false; }
    return true;
}

}
