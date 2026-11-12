// SPDX-License-Identifier: GPL-2.0-or-later
// Qualifies the guest V4L2 stateful decoder (the Virtio video decoder) from
// inside a guest, with and without the GPU. It checks:
//
// - every advertised profile's pixels, and the raw layouts, including packed
//   BGRA capture and 8-bit capture of a 10-bit stream;
// - the rows of every raw format at the padded pitch the device states
//   (TRY_FMT and S_FMT against the rule at widths that are and are not whole
//   pitches, and the 120 x 90 window streams read row by row);
// - imported dma-buf capture: the queue's own buffers handed back and guest
//   memory a client brings, with each buffer's planes in separate dma-bufs or
//   in one and their lengths stated as QUERYBUF gave them, as the whole
//   dma-buf, or not at all. With the GPU the capture buffers are host-visible
//   blobs and an imported dma-buf takes another path in the driver;
// - timestamps, source-change and EOS events, the display order of H.264 and
//   HEVC B-pictures and a seek in both (over open-group streams for HEVC);
// - the H.264 and HEVC profile and level menus: Constrained High beside High,
//   levels from 1.0 up to 5.2 and from 1 up to 6.2, the current level the
//   highest;
// - flat portrait and square H.264 and HEVC pictures (2304 x 4096,
//   4096 x 4096, 4320 x 8192), decoded to frames of their own size inside a
//   frame range that is a rectangle;
// - a stream that changes its picture size twice, decoded the way Android's
//   V4L2 codec handles it, including a drain asked for while the last change
//   is still on its way: every picture of a size returns, then the LAST
//   buffer, before the source change is visible. A client that queues the
//   whole stream and drains straight away, with both changes inside its
//   drain, gets every picture of all three sizes before the one LAST buffer
//   that ends the drain;
// - every profile started in v4l2_codec2's order, with a provisional 64 x 64
//   capture format (and queued buffers for it) before the size is known, the
//   parameter sets and the picture as two coded buffers, and the capture
//   format chosen only after the source change: the buffer that declared the
//   size is held until then, nothing lands in the provisional buffers, and
//   the picture arrives whole before the LAST buffer;
// - the coded queue's buffer size, read back for every coded format as a
//   client sees it (S_FMT and G_FMT, then the plane length REQBUFS
//   allocates): the tier of each picture size stated, and for a size beyond
//   the format's range, as a client that states none passes it, the tier of
//   the largest picture it takes;
// - large buffers created with the guest's free memory in blocks of 32 KiB,
//   none larger, the state a guest that has run for days is in: a 124 MiB
//   coded buffer, the most the device attaches, is created, exported and
//   imported again, and the 4096 x 4096 and 4320 x 8192 flat pictures are
//   decoded into mapped buffers. The scatter lists of these buffers are
//   hundreds of kilobytes, so a driver that asks the kernel for a list in one
//   block fails them with `page allocation failure: order:4`.
//
// The test, `guest-v4l2-decode` or `guest-v4l2-decode-gpu`, also fails on a
// kernel fault or a failed shutdown.
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void call(int fd, unsigned long request, void* value, const char* name)
{
    int result;
    do { result = ioctl(fd, request, value); } while (result < 0 && errno == EINTR);
    if (result < 0) throw std::runtime_error(std::string(name) + ": " + std::strerror(errno));
}
struct Device {
    int fd;
    explicit Device(const char* path) : fd(open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC))
    { if (fd < 0) throw std::runtime_error(std::string("open decoder: ") + std::strerror(errno)); }
    ~Device() { close(fd); }
};
struct Mapping {
    /* `data` is the plane's first byte and `length` its size. A plane that
     * starts inside a dma-buf, after the planes before it, need not start on a
     * page, so the mapping begins on the page below it. */
    void* data;
    std::size_t length;
    Mapping(int fd, std::size_t size, off_t offset)
    {
        const off_t page = sysconf(_SC_PAGESIZE);
        const off_t aligned = offset - offset % page;
        skew = std::size_t(offset - aligned);
        base = mmap(nullptr, size + skew, PROT_READ | PROT_WRITE, MAP_SHARED, fd, aligned);
        require(base != MAP_FAILED, "map video buffer failed");
        data = static_cast<std::uint8_t*>(base) + skew;
        length = size;
    }
    Mapping(Mapping&& other) noexcept : data(other.data), length(other.length), base(other.base), skew(other.skew)
    { other.base = MAP_FAILED; }
    ~Mapping() { if (base != MAP_FAILED) munmap(base, length + skew); }
private:
    void* base;
    std::size_t skew;
};
std::vector<std::uint8_t> read(const std::string& path)
{
    std::ifstream input(path, std::ios::binary);
    require(bool(input), "video fixture missing");
    return {std::istreambuf_iterator<char>(input), {}};
}
constexpr auto output = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
constexpr auto capture = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
using Buffers = std::vector<std::vector<Mapping>>;
struct Prime {
    int fd {-1};
    std::size_t length {};
    /* Where the plane starts in the dma-buf, which is not zero for the second
     * plane of a buffer whose planes share one. */
    std::size_t offset {};
    Prime() = default;
    Prime(int descriptor, std::size_t size, std::size_t start = 0) : fd(descriptor), length(size), offset(start) {}
    Prime(Prime&& other) noexcept : fd(other.fd), length(other.length), offset(other.offset) { other.fd = -1; }
    Prime& operator=(Prime&&) = delete;
    Prime(const Prime&) = delete;
    ~Prime() { if (fd >= 0) close(fd); }
};
using Primes = std::vector<std::vector<Prime>>;
Buffers allocate(int fd, v4l2_buf_type type, unsigned count)
{
    v4l2_requestbuffers request {};
    request.type = type; request.memory = V4L2_MEMORY_MMAP; request.count = count;
    call(fd, VIDIOC_REQBUFS, &request, "REQBUFS");
    require(request.count >= count && request.count <= 32, "unexpected video buffer count");
    Buffers result;
    for (unsigned i = 0; i < request.count; ++i) {
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = type; buffer.memory = V4L2_MEMORY_MMAP; buffer.index = i;
        buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
        call(fd, VIDIOC_QUERYBUF, &buffer, "QUERYBUF");
        require(buffer.length > 0 && buffer.length <= VIDEO_MAX_PLANES, "invalid plane count");
        result.emplace_back();
        for (unsigned j = 0; j < buffer.length; ++j)
            result.back().emplace_back(fd, planes[j].length, planes[j].m.mem_offset);
    }
    return result;
}
/* Exports the queue's own buffers as dma-bufs and hands the queue back
 * empty. vb2 orphans buffers an exported dma-buf still holds, so the
 * descriptors keep their memory alive after the mapped allocation is gone
 * and the same queue can import them again. */
Primes exportAndRelease(int fd, v4l2_buf_type type, const Buffers& maps)
{
    Primes result;
    for (unsigned i = 0; i < maps.size(); ++i) {
        result.emplace_back();
        for (unsigned j = 0; j < maps[i].size(); ++j) {
            v4l2_exportbuffer request {};
            request.type = type; request.index = i; request.plane = j;
            request.flags = O_RDWR | O_CLOEXEC;
            call(fd, VIDIOC_EXPBUF, &request, "EXPBUF");
            require(request.fd >= 0, "EXPBUF returned no descriptor");
            result.back().emplace_back(request.fd, maps[i][j].length);
        }
    }
    return result;
}
void release(int fd, v4l2_buf_type type)
{
    v4l2_requestbuffers request {};
    request.type = type; request.memory = V4L2_MEMORY_MMAP; request.count = 0;
    call(fd, VIDIOC_REQBUFS, &request, "REQBUFS release");
}
/* Free guest memory by the size of its blocks: element N counts the free
 * blocks of 2^N pages, summed over the zones, as /proc/buddyinfo lists them. */
std::vector<unsigned long> freeBlocks()
{
    std::vector<unsigned long> blocks;
    std::ifstream info("/proc/buddyinfo");
    require(bool(info), "/proc/buddyinfo is unreadable");
    for (std::string line; std::getline(info, line);) {
        const auto zone = line.find("zone");
        if (zone == std::string::npos) continue;
        std::istringstream fields(line.substr(zone));
        std::string word, name;
        fields >> word >> name;
        unsigned long count;
        for (std::size_t order = 0; fields >> count; ++order) {
            if (blocks.size() <= order) blocks.resize(order + 1);
            blocks[order] += count;
        }
    }
    return blocks;
}
unsigned long freePages()
{
    std::ifstream info("/proc/meminfo");
    for (std::string line; std::getline(info, line);) {
        unsigned long kilobytes;
        if (std::sscanf(line.c_str(), "MemFree: %lu kB", &kilobytes) == 1)
            return kilobytes * 1024 / static_cast<unsigned long>(sysconf(_SC_PAGESIZE));
    }
    throw std::runtime_error("/proc/meminfo has no MemFree");
}
/* Guest memory whose free part is in blocks of 32 KiB, none larger, with
 * plenty of those. The kernel allocates the ring of a pipe in one block, as
 * large as its slots need and not otherwise used, and keeps it until the pipe
 * closes, so pipes of a chosen number of slots take blocks of a chosen order
 * out of memory and hold them where the kernel does not move them. This
 * takes order-3 blocks (a ring of 512 slots) until memory is nearly full,
 * lets go of every other one, and then takes away whatever larger block is
 * left. What the kernel allocates next has to be built from 32 KiB blocks,
 * as it is in a guest that has run for days: and a request for more than
 * 32 KiB in one piece fails, though half of memory is free. Blocks of 32 KiB
 * must stay, because a mapped buffer asks for them before it asks for
 * smaller ones, and the kernel answers a failed request of that order by
 * killing a process. The blocks go back when this object ends. */
class Fragmented {
public:
    explicit Fragmented(unsigned long reservePages)
    {
        rlimit limit {1 << 20, 1 << 20};
        require(setrlimit(RLIMIT_NOFILE, &limit) == 0, "raising the descriptor limit failed");
        while (freePages() > reservePages && blocksAtLeast(3) > 8192)
            for (unsigned i = 0; i < 128; ++i) require(claim(512), "claiming a block failed");
        const std::size_t claimed = pipes_.size();
        for (std::size_t i = 0; i < claimed; i += 2) {
            close(pipes_[i].read); close(pipes_[i].write);
            pipes_[i] = {-1, -1};
        }
        /* Blocks of 64 KiB and more are taken until there are none; a ring of
         * 1024 slots is one of 64 KiB. */
        for (unsigned long guard = 0; blocksAtLeast(4) != 0; ++guard) {
            require(guard < (1ul << 20), "larger blocks could not be taken away");
            require(claim(1024), "taking away a larger block failed");
        }
        std::printf("fragmented memory: %zu blocks taken, %lu pages free, %lu free blocks of 32 KiB\n", claimed,
                    freePages(), freeBlocks().at(3));
    }
    Fragmented(const Fragmented&) = delete;
    Fragmented& operator=(const Fragmented&) = delete;
    ~Fragmented()
    {
        for (const auto& pipe : pipes_) if (pipe.read >= 0) { close(pipe.read); close(pipe.write); }
    }
    /* Pages that lie in free blocks of 2^order pages or more. */
    static unsigned long blocksAtLeast(unsigned order)
    {
        const auto blocks = freeBlocks();
        unsigned long pages = 0;
        for (std::size_t i = order; i < blocks.size(); ++i) pages += blocks[i] << i;
        return pages;
    }
private:
    struct Pipe { int read; int write; };
    std::vector<Pipe> pipes_;
    /* A pipe whose ring of `slots` slots takes one block. */
    bool claim(unsigned slots)
    {
        int ends[2];
        require(pipe2(ends, O_CLOEXEC) == 0, "pipe failed");
        if (fcntl(ends[1], F_SETPIPE_SZ, int(slots * sysconf(_SC_PAGESIZE))) < 0) {
            close(ends[0]); close(ends[1]);
            return false;
        }
        pipes_.push_back({ends[0], ends[1]});
        return true;
    }
};
unsigned import(int fd, v4l2_buf_type type, unsigned count)
{
    v4l2_requestbuffers request {};
    request.type = type; request.memory = V4L2_MEMORY_DMABUF; request.count = count;
    call(fd, VIDIOC_REQBUFS, &request, "REQBUFS dmabuf");
    require(request.count >= count && request.count <= 32, "unexpected imported buffer count");
    return request.count;
}
void queue(int fd, v4l2_buf_type type, unsigned index, const std::vector<Mapping>& maps, unsigned bytes = 0)
{
    v4l2_plane planes[VIDEO_MAX_PLANES] {};
    v4l2_buffer buffer {};
    buffer.type = type; buffer.memory = V4L2_MEMORY_MMAP; buffer.index = index;
    buffer.length = maps.size(); buffer.m.planes = planes;
    buffer.timestamp.tv_sec = 7; buffer.timestamp.tv_usec = 654321;
    for (unsigned i = 0; i < maps.size(); ++i) planes[i].length = maps[i].length;
    planes[0].bytesused = bytes;
    call(fd, VIDIOC_QBUF, &buffer, "QBUF");
}
/* How an imported capture buffer states its planes' lengths. A client may
 * pass what QUERYBUF gave it, the dma-buf's whole size, or nothing at all, in
 * which case the queue takes the dma-buf's size. */
enum class Declared { negotiated, whole, none };
void queueImported(int fd, v4l2_buf_type type, unsigned index, const std::vector<Prime>& primes,
                   const std::vector<unsigned>& sizes = {}, Declared declared = Declared::whole, unsigned bytes = 0)
{
    v4l2_plane planes[VIDEO_MAX_PLANES] {};
    v4l2_buffer buffer {};
    buffer.type = type; buffer.memory = V4L2_MEMORY_DMABUF; buffer.index = index;
    buffer.length = primes.size(); buffer.m.planes = planes;
    buffer.timestamp.tv_sec = 7; buffer.timestamp.tv_usec = 654321;
    for (unsigned i = 0; i < primes.size(); ++i) {
        planes[i].m.fd = primes[i].fd;
        planes[i].length = declared == Declared::none ? 0
            : declared == Declared::negotiated ? sizes.at(i) : primes[i].length;
    }
    planes[0].bytesused = bytes;
    call(fd, VIDIOC_QBUF, &buffer, "QBUF dmabuf");
}
/* The rows of a raw picture in the buffer the device describes. The host
 * pads each row to the pitch a guest allocator gives a buffer of that format
 * (16 bytes for NV12, P010 and packed 32-bit, 32 for the luma of planar
 * YUV 4:2:0), so a picture whose width is not a multiple of that has rows
 * longer than its samples and a client steps by bytesperline, not by width.
 * A plane is stored as bands: `rows` rows each holding `bytes` of picture
 * before the padding. Plane heights are the picture's: chroma rows are
 * rounded up. */
struct Band { std::size_t bytes; std::size_t rows; };
struct PlaneLayout {
    std::size_t pitch;
    std::vector<Band> bands;
    std::size_t size() const
    {
        return pitch * std::accumulate(bands.begin(), bands.end(), std::size_t {},
                                       [](std::size_t sum, const Band& band) { return sum + band.rows; });
    }
};
std::size_t alignUp(std::size_t value, std::size_t alignment) { return (value + alignment - 1) / alignment * alignment; }
/* Empty for a format the rule does not cover. */
std::vector<PlaneLayout> rawLayout(unsigned raw, std::size_t width, std::size_t height)
{
    const std::size_t halfWidth = (width + 1) / 2, halfHeight = (height + 1) / 2;
    switch (raw) {
    case V4L2_PIX_FMT_NV12M: case V4L2_PIX_FMT_NV21M: {
        const std::size_t pitch = alignUp(width, 16);
        return {{pitch, {{width, height}}}, {pitch, {{2 * halfWidth, halfHeight}}}};
    }
    case V4L2_PIX_FMT_YUV420M: case V4L2_PIX_FMT_YVU420M: {
        const std::size_t pitch = alignUp(width, 32);
        return {{pitch, {{width, height}}}, {pitch / 2, {{halfWidth, halfHeight}}}, {pitch / 2, {{halfWidth, halfHeight}}}};
    }
    case V4L2_PIX_FMT_P010: {
        const std::size_t pitch = alignUp(2 * width, 16);
        return {{pitch, {{2 * width, height}, {4 * halfWidth, halfHeight}}}};
    }
    case V4L2_PIX_FMT_ABGR32: case V4L2_PIX_FMT_ARGB32: case V4L2_PIX_FMT_RGBA32: case V4L2_PIX_FMT_RGB32:
        return {{alignUp(4 * width, 16), {{4 * width, height}}}};
    default:
        return {};
    }
}
/* A format's planes are what the rule says. TRY_FMT has to say so as well as
 * S_FMT: a client that sizes its buffers from TRY_FMT, as Android's does,
 * would otherwise allocate rows of the picture's width and be refused. */
void requireLayout(const v4l2_pix_format_mplane& format, const std::vector<PlaneLayout>& layout, const char* call)
{
    const std::string name = call;
    require(format.num_planes == layout.size(), (name + " reports another plane count than the device lays out").c_str());
    for (unsigned i = 0; i < layout.size(); ++i) {
        require(format.plane_fmt[i].bytesperline == layout[i].pitch,
                (name + " does not report the padded pitch of a plane").c_str());
        require(format.plane_fmt[i].sizeimage == layout[i].size(),
                (name + " does not report the padded size of a plane").c_str());
    }
}

/* The picture a dequeued buffer holds, with the padding between rows taken
 * out: its planes one after another, each as tight rows. */
std::vector<std::uint8_t> tightPicture(const std::vector<PlaneLayout>& layout, const v4l2_plane* planes,
                                       const std::vector<Mapping>& maps)
{
    require(maps.size() == layout.size(), "the buffer has another plane count than its format");
    std::vector<std::uint8_t> picture;
    for (unsigned i = 0; i < layout.size(); ++i) {
        require(planes[i].data_offset <= planes[i].bytesused && planes[i].bytesused <= maps[i].length,
                "invalid decoded plane extent");
        const auto* base = static_cast<const std::uint8_t*>(maps[i].data);
        std::size_t at = planes[i].data_offset;
        for (const auto& band : layout[i].bands)
            for (std::size_t row = 0; row < band.rows; ++row, at += layout[i].pitch) {
                require(at + band.bytes <= planes[i].bytesused, "a decoded plane is shorter than its rows");
                picture.insert(picture.end(), base + at, base + at + band.bytes);
            }
    }
    return picture;
}
/* Where an imported capture buffer's memory comes from. By default it is the
 * capture queue's own buffers, exported and handed back, which on a host with
 * a GPU are host-visible blobs. `guestPages` instead takes guest memory a
 * client could equally have got from a dma-buf heap or another driver: the
 * coded queue's buffers, which are guest pages on every host. `shared` puts a
 * buffer's planes in one dma-buf, one after another, the way a client that
 * allocates one buffer per picture does. */
struct Import {
    bool guestPages {};
    bool shared {};
    Declared declared {Declared::whole};
};
/* Exports `count` buffers of guest pages, one dma-buf per plane or one for all
 * of a buffer's planes. The descriptors outlive the device that allocated
 * them: vb2 keeps a buffer an exported dma-buf still holds. */
Primes exportGuestPages(const char* device, unsigned coded, const std::vector<unsigned>& sizes,
                        bool shared, unsigned count)
{
    Device owner(device);
    const int fd = owner.fd;
    v4l2_format input {};
    input.type = output;
    input.fmt.pix_mp.width = 128; input.fmt.pix_mp.height = 96;
    input.fmt.pix_mp.pixelformat = coded;
    input.fmt.pix_mp.num_planes = 1;
    input.fmt.pix_mp.plane_fmt[0].sizeimage = 4096;
    call(fd, VIDIOC_S_FMT, &input, "S_FMT coded for export");
    std::vector<unsigned> chunks;
    if (shared) chunks.push_back(std::accumulate(sizes.begin(), sizes.end(), 0u));
    else chunks = sizes;
    /* One group of buffers per distinct size. A coded buffer is as large as
     * the request says, so each dma-buf is exactly what its planes need. */
    Primes result(count);
    for (const unsigned size : chunks) {
        v4l2_create_buffers create {};
        create.count = count; create.memory = V4L2_MEMORY_MMAP;
        create.format = input;
        create.format.fmt.pix_mp.plane_fmt[0].sizeimage = size;
        call(fd, VIDIOC_CREATE_BUFS, &create, "CREATE_BUFS guest pages");
        require(create.count == count, "unexpected guest page buffer count");
        for (unsigned i = 0; i < count; ++i) {
            v4l2_exportbuffer request {};
            request.type = output; request.index = create.index + i; request.plane = 0;
            request.flags = O_RDWR | O_CLOEXEC;
            call(fd, VIDIOC_EXPBUF, &request, "EXPBUF guest pages");
            require(request.fd >= 0, "EXPBUF returned no descriptor");
            const off_t extent = lseek(request.fd, 0, SEEK_END);
            require(extent >= off_t(size), "exported guest page buffer is too small");
            result[i].emplace_back(request.fd, std::size_t(extent));
        }
    }
    if (shared) {
        /* The planes follow one another in the one dma-buf. */
        std::size_t offset = 0;
        for (auto& planes : result) {
            const int whole = planes[0].fd;
            const std::size_t extent = planes[0].length;
            offset = 0;
            planes[0].offset = 0;
            for (unsigned i = 1; i < sizes.size(); ++i) {
                offset += sizes[i - 1];
                planes.emplace_back(dup(whole), extent, offset);
                require(planes.back().fd >= 0, "dup of a dma-buf failed");
            }
        }
    }
    release(fd, output);
    return result;
}
/* `memory` selects how the capture queue holds its frames: its own mapped
 * buffers, or dma-buf descriptors it imports. The imported case is the one a
 * GPU client uses, so it is qualified against the same fixtures. */
/* The picture a stream shows, which is smaller than the 128 x 96 it is coded
 * as when the stream states a window onto it. */
struct Picture { unsigned width {128}; unsigned height {96}; };
void decode(const char* device, const std::string& directory, const char* name, unsigned coded, unsigned raw,
            unsigned memory = V4L2_MEMORY_MMAP, Import origin = {}, Picture picture = {})
{
    const std::string memoryName = memory != V4L2_MEMORY_DMABUF ? "mmap"
        : !origin.guestPages ? "dmabuf"
        : std::string(origin.shared ? "dmabuf-guest-shared-" : "dmabuf-guest-") +
            (origin.declared == Declared::negotiated ? "negotiated" : origin.declared == Declared::whole ? "whole" : "none");
    const auto packet = read(directory + "/" + name + ".bin");
    const auto reference = read(directory + "/" + name + ".raw");
    Device owner(device);
    const int fd = owner.fd;
    v4l2_capability cap {};
    call(fd, VIDIOC_QUERYCAP, &cap, "QUERYCAP");
    require((cap.device_caps & (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING)) ==
            (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING), "device is not a streaming mem2mem decoder");
    v4l2_event_subscription subscription {};
    subscription.type = V4L2_EVENT_SOURCE_CHANGE;
    call(fd, VIDIOC_SUBSCRIBE_EVENT, &subscription, "SUBSCRIBE_EVENT");
    v4l2_format input {};
    input.type = output;
    input.fmt.pix_mp.width = 128; input.fmt.pix_mp.height = 96;
    input.fmt.pix_mp.pixelformat = coded;
    input.fmt.pix_mp.num_planes = 1;
    input.fmt.pix_mp.plane_fmt[0].sizeimage = packet.size();
    /* A client asks what it would get by naming only the format and the
     * picture, the way Chromium's V4L2 decoder builds a candidate: the plane
     * layout is what it expects the call to fill in. */
    v4l2_format codedCandidate {};
    codedCandidate.type = output;
    codedCandidate.fmt.pix_mp.width = 128; codedCandidate.fmt.pix_mp.height = 96;
    codedCandidate.fmt.pix_mp.pixelformat = coded;
    call(fd, VIDIOC_TRY_FMT, &codedCandidate, "TRY_FMT coded");
    require(codedCandidate.fmt.pix_mp.pixelformat == coded, "TRY_FMT substituted the coded format");
    require(codedCandidate.fmt.pix_mp.num_planes == 1, "TRY_FMT coded is not a single buffer");
    require(codedCandidate.fmt.pix_mp.plane_fmt[0].sizeimage >= packet.size(),
            "TRY_FMT coded buffer is shorter than the packet");
    call(fd, VIDIOC_S_FMT, &input, "S_FMT coded");
    require(input.fmt.pix_mp.pixelformat == coded, "coded format was substituted");
    auto inputs = allocate(fd, output, 2);
    require(inputs[0][0].length >= packet.size(), "coded buffer too short");
    std::memcpy(inputs[0][0].data, packet.data(), packet.size());
    auto type = output;
    call(fd, VIDIOC_STREAMON, &type, "STREAMON output");
    queue(fd, output, 0, inputs[0], packet.size());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool source = false;
    while (!source && std::chrono::steady_clock::now() < deadline) {
        pollfd p {fd, POLLPRI, 0};
        if (poll(&p, 1, 100) < 0 && errno != EINTR) throw std::runtime_error("poll source change failed");
        v4l2_event event {};
        if (ioctl(fd, VIDIOC_DQEVENT, &event) == 0)
            source = event.type == V4L2_EVENT_SOURCE_CHANGE && (event.u.src_change.changes & V4L2_EVENT_SRC_CH_RESOLUTION);
        else require(errno == EAGAIN, "DQEVENT failed");
    }
    require(source, "decoder source-change timeout");
    v4l2_format format {};
    format.type = capture;
    call(fd, VIDIOC_G_FMT, &format, "G_FMT capture");
    require(format.fmt.pix_mp.width == picture.width && format.fmt.pix_mp.height == picture.height, "decoded dimensions differ");
    format.fmt.pix_mp.pixelformat = raw;
    /* A client filters its candidates with TRY_FMT and drops the ones it
     * fails, so the call has to answer before the format is set. */
    v4l2_format candidate {};
    candidate.type = capture;
    candidate.fmt.pix_mp.width = format.fmt.pix_mp.width;
    candidate.fmt.pix_mp.height = format.fmt.pix_mp.height;
    candidate.fmt.pix_mp.pixelformat = raw;
    call(fd, VIDIOC_TRY_FMT, &candidate, "TRY_FMT capture");
    require(candidate.fmt.pix_mp.pixelformat == raw, "TRY_FMT substituted the raw format");
    call(fd, VIDIOC_S_FMT, &format, "S_FMT capture");
    require(format.fmt.pix_mp.pixelformat == raw, "raw format was substituted");
    /* TRY_FMT has to predict what S_FMT produces, planes included: a client
     * that compares the two, as Chromium's V4L2 decoder does, rejects a
     * capture format whose plane count or stride appears only once it is set. */
    require(candidate.fmt.pix_mp.num_planes == format.fmt.pix_mp.num_planes,
            "TRY_FMT and S_FMT disagree on the plane count");
    for (unsigned i = 0; i < format.fmt.pix_mp.num_planes; ++i) {
        require(candidate.fmt.pix_mp.plane_fmt[i].bytesperline ==
                    format.fmt.pix_mp.plane_fmt[i].bytesperline,
                "TRY_FMT and S_FMT disagree on a plane stride");
        require(candidate.fmt.pix_mp.plane_fmt[i].sizeimage ==
                    format.fmt.pix_mp.plane_fmt[i].sizeimage,
                "TRY_FMT and S_FMT disagree on a plane size");
    }
    /* The stride reported for the capture format is the one its rows are
     * written with: the pitch the device pads them to, which TRY_FMT reports
     * as S_FMT does. A consumer that steps a mapping by the picture's width
     * instead reads every row from the wrong sample. */
    const auto& negotiated = format.fmt.pix_mp;
    const auto layout = rawLayout(raw, negotiated.width, negotiated.height);
    require(!layout.empty(), "no layout rule for the raw format");
    requireLayout(negotiated, layout, "S_FMT");
    requireLayout(candidate.fmt.pix_mp, layout, "TRY_FMT");
    Buffers outputs;
    Primes primes;
    std::vector<unsigned> sizes;
    for (unsigned i = 0; i < format.fmt.pix_mp.num_planes; ++i)
        sizes.push_back(format.fmt.pix_mp.plane_fmt[i].sizeimage);
    if (origin.guestPages) {
        require(memory == V4L2_MEMORY_DMABUF, "guest page buffers are imported");
        /* The queue says how many buffers it keeps; the descriptors are then
         * exported for exactly those. */
        primes = exportGuestPages(device, coded, sizes, origin.shared, import(fd, capture, 4));
        for (auto& planes : primes) {
            outputs.emplace_back();
            for (unsigned i = 0; i < planes.size(); ++i)
                outputs.back().emplace_back(planes[i].fd, sizes.at(i), planes[i].offset);
        }
    } else {
        outputs = allocate(fd, capture, 4);
        if (memory == V4L2_MEMORY_DMABUF) {
            primes = exportAndRelease(fd, capture, outputs);
            outputs.clear();
            release(fd, capture);
            require(import(fd, capture, primes.size()) == primes.size(), "imported buffer count differs");
            for (auto& planes : primes) {
                outputs.emplace_back();
                for (auto& plane : planes)
                    outputs.back().emplace_back(plane.fd, plane.length, 0);
            }
        }
    }
    for (unsigned i = 0; i < outputs.size(); ++i) {
        if (memory == V4L2_MEMORY_DMABUF) queueImported(fd, capture, i, primes[i], sizes, origin.declared);
        else queue(fd, capture, i, outputs[i]);
    }
    type = capture;
    call(fd, VIDIOC_STREAMON, &type, "STREAMON capture");
    v4l2_decoder_cmd drain {};
    drain.cmd = V4L2_DEC_CMD_STOP;
    call(fd, VIDIOC_DECODER_CMD, &drain, "DECODER_CMD drain");
    bool frame = false, eos = false;
    while (!eos && std::chrono::steady_clock::now() < deadline) {
        pollfd p {fd, POLLIN, 0};
        if (poll(&p, 1, 100) < 0 && errno != EINTR) throw std::runtime_error("poll capture failed");
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = capture; buffer.memory = memory;
        buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
        if (ioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
            require(errno == EAGAIN, "DQBUF capture failed"); continue;
        }
        require(!(buffer.flags & V4L2_BUF_FLAG_ERROR), "capture buffer has decoder error");
        if (buffer.flags & V4L2_BUF_FLAG_LAST) { eos = true; if (!planes[0].bytesused) continue; }
        require(!frame, "decoder returned duplicate frame");
        frame = true;
        require(buffer.timestamp.tv_sec == 7 && buffer.timestamp.tv_usec == 654321, "frame timestamp differs");
        require(buffer.index < outputs.size(), "capture index outside allocation");
        require(buffer.length == layout.size(), "the decoded buffer has another plane count than its format");
        auto actual = tightPicture(layout, planes, outputs[buffer.index]);
        const std::size_t width = picture.width, height = picture.height;
        const std::size_t halfWidth = (width + 1) / 2, halfHeight = (height + 1) / 2;
        if (raw == V4L2_PIX_FMT_ABGR32) {
            /* The host decodes straight to packed B, G, R, A. The reference is
             * the planar fixture taken through the limited-range BT.601 matrix
             * a frame this small carries when the stream states nothing, and
             * only a pixel whose chroma neighborhood is constant is compared:
             * hardware interpolates the subsampled field, so a pixel beside a
             * chroma edge has no single defined answer. */
            require(actual.size() == width * height * 4u, "packed frame size differs");
            const unsigned step = reference.size() == width * height * 3u ? 2u : 1u;
            const auto sample = [&](std::size_t index) { return int(reference[step * index + step - 1]); };
            const auto chroma = [&](long cellRow, long cellColumn) {
                cellRow = cellRow < 0 ? 0 : cellRow > long(halfHeight) - 1 ? long(halfHeight) - 1 : cellRow;
                cellColumn = cellColumn < 0 ? 0 : cellColumn > long(halfWidth) - 1 ? long(halfWidth) - 1 : cellColumn;
                const auto pair = std::size_t(width * height + cellRow * 2 * halfWidth + cellColumn * 2);
                return std::pair<int, int>{sample(pair), sample(pair + 1)};
            };
            unsigned maximum = 0, checked = 0;
            for (long row = 0; row < long(height); ++row)
                for (long column = 0; column < long(width); ++column) {
                    const auto* got = actual.data() + (row * width + column) * 4;
                    require(got[3] == 0xff, "packed frame is not opaque");
                    const auto cell = chroma(row / 2, column / 2);
                    bool flat = true;
                    for (long dy = -1; dy <= 1; ++dy)
                        for (long dx = -1; dx <= 1; ++dx)
                            flat = flat && chroma(row / 2 + dy, column / 2 + dx) == cell;
                    if (!flat) continue;
                    ++checked;
                    const double luma = 1.164 * (sample(std::size_t(row * width + column)) - 16);
                    const double u = cell.first - 128, v = cell.second - 128;
                    const double wanted[3] = {luma + 2.017 * u, luma - 0.392 * u - 0.813 * v,
                                              luma + 1.596 * v};
                    for (unsigned channel = 0; channel < 3; ++channel) {
                        const double clamped = wanted[channel] < 0 ? 0 : wanted[channel] > 255 ? 255 : wanted[channel];
                        const int difference = int(got[channel]) - int(clamped + 0.5);
                        maximum = std::max(maximum, unsigned(difference < 0 ? -difference : difference));
                    }
                }
            require(checked >= width * height / 8, "too little of the packed frame could be verified");
            require(maximum <= 8, "V4L2 packed pixels differ from the reference");
            std::printf("PASS %s raw=%08x memory=%s max-error=%u pixels=%u\n", name, raw,
                memoryName.c_str(), maximum, checked);
            std::fflush(stdout);
            continue;
        }
        /* A 10-bit stream captured as 8-bit planes is the 10-bit picture
         * rounded to 8 bits, so its reference is half the size. */
        const unsigned step = raw == V4L2_PIX_FMT_P010 ? 2 : 1;
        const unsigned referenceStep = reference.size() == width * height * 3u ? 2 : 1;
        require(actual.size() == reference.size() / referenceStep * step, "decoded plane sizes differ");
        if (raw == V4L2_PIX_FMT_YUV420M) {
            const auto planar = actual;
            for (std::size_t i = 0; i < halfWidth * halfHeight; ++i) {
                actual[width * height + 2 * i] = planar[width * height + i];
                actual[width * height + 2 * i + 1] = planar[width * height + halfWidth * halfHeight + i];
            }
        }
        unsigned maximum = 0;
        for (std::size_t i = 0; i < actual.size(); i += step) {
            const std::size_t at = i / step * referenceStep;
            const int got = step == 1 ? actual[i] : (actual[i] | (actual[i + 1] << 8)) >> 6;
            int wanted = referenceStep == 1 ? reference[at] : (reference[at] | (reference[at + 1] << 8)) >> 6;
            if (referenceStep > step) wanted = (wanted + 2) >> 2;
            maximum = std::max(maximum, unsigned(std::abs(got - wanted)));
        }
        require(maximum <= (coded == V4L2_PIX_FMT_MJPEG ? 4u : 2u), "V4L2 pixels differ from reference");
        std::printf("PASS %s raw=%08x memory=%s max-error=%u\n", name, raw,
            memoryName.c_str(), maximum);
        std::fflush(stdout);
    }
    require(frame && eos, "capture frame/EOS timeout");
    type = output; call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF output");
    type = capture; call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF capture");
}
}
/* A picture that is not landscape, or not small, decoded to a frame of its own
 * size. The hardware limits a dimension and not the area, so the guest's range
 * is a rectangle and every picture within it decodes; a host that capped the
 * height at the landscape 2304 refused these. The stream is a flat gray picture,
 * so the frame is checked by its size, its stride and its mean luma. */
void decodeShape(const char* device, const std::string& directory, const char* name, unsigned coded,
                 unsigned width, unsigned height)
{
    const auto packet = read(directory + "/" + name + ".bin");
    Device owner(device);
    const int fd = owner.fd;
    v4l2_event_subscription subscription {};
    subscription.type = V4L2_EVENT_SOURCE_CHANGE;
    call(fd, VIDIOC_SUBSCRIBE_EVENT, &subscription, "SUBSCRIBE_EVENT");
    v4l2_format input {};
    input.type = output;
    input.fmt.pix_mp.width = 128; input.fmt.pix_mp.height = 96;
    input.fmt.pix_mp.pixelformat = coded;
    input.fmt.pix_mp.num_planes = 1;
    input.fmt.pix_mp.plane_fmt[0].sizeimage = packet.size();
    call(fd, VIDIOC_S_FMT, &input, "S_FMT coded");
    auto inputs = allocate(fd, output, 2);
    require(inputs[0][0].length >= packet.size(), "coded buffer too short");
    std::memcpy(inputs[0][0].data, packet.data(), packet.size());
    auto type = output;
    call(fd, VIDIOC_STREAMON, &type, "STREAMON output");
    queue(fd, output, 0, inputs[0], packet.size());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    bool source = false;
    while (!source && std::chrono::steady_clock::now() < deadline) {
        pollfd p {fd, POLLPRI, 0};
        if (poll(&p, 1, 100) < 0 && errno != EINTR) throw std::runtime_error("poll source change failed");
        v4l2_event event {};
        if (ioctl(fd, VIDIOC_DQEVENT, &event) == 0)
            source = event.type == V4L2_EVENT_SOURCE_CHANGE && (event.u.src_change.changes & V4L2_EVENT_SRC_CH_RESOLUTION);
        else require(errno == EAGAIN, "DQEVENT failed");
    }
    require(source, "decoder source-change timeout");
    v4l2_format format {};
    format.type = capture;
    call(fd, VIDIOC_G_FMT, &format, "G_FMT capture");
    require(format.fmt.pix_mp.width == width && format.fmt.pix_mp.height == height,
            "the decoded picture is not the size of the stream");
    format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;
    call(fd, VIDIOC_S_FMT, &format, "S_FMT capture");
    const auto stride = format.fmt.pix_mp.plane_fmt[0].bytesperline;
    require(format.fmt.pix_mp.width == width && format.fmt.pix_mp.height == height && stride >= width &&
            format.fmt.pix_mp.plane_fmt[0].sizeimage >= std::size_t(stride) * height,
            "the capture format does not hold the picture");
    auto outputs = allocate(fd, capture, 4);
    for (unsigned i = 0; i < outputs.size(); ++i) queue(fd, capture, i, outputs[i]);
    type = capture;
    call(fd, VIDIOC_STREAMON, &type, "STREAMON capture");
    v4l2_decoder_cmd drain {};
    drain.cmd = V4L2_DEC_CMD_STOP;
    call(fd, VIDIOC_DECODER_CMD, &drain, "DECODER_CMD drain");
    bool frame = false, eos = false;
    while (!eos && std::chrono::steady_clock::now() < deadline) {
        pollfd p {fd, POLLIN, 0};
        if (poll(&p, 1, 100) < 0 && errno != EINTR) throw std::runtime_error("poll capture failed");
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = capture; buffer.memory = V4L2_MEMORY_MMAP;
        buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
        if (ioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
            require(errno == EAGAIN, "DQBUF capture failed"); continue;
        }
        require(!(buffer.flags & V4L2_BUF_FLAG_ERROR), "capture buffer has decoder error");
        if (buffer.flags & V4L2_BUF_FLAG_LAST) { eos = true; if (!planes[0].bytesused) continue; }
        require(!frame, "decoder returned duplicate frame");
        frame = true;
        require(buffer.index < outputs.size() && planes[0].bytesused >= std::size_t(stride) * height,
                "the decoded plane is shorter than the picture");
        const auto* luma = static_cast<const std::uint8_t*>(outputs[buffer.index][0].data) + planes[0].data_offset;
        std::uint64_t sum = 0;
        for (unsigned row = 0; row < height; ++row)
            for (unsigned column = 0; column < width; ++column) sum += luma[std::size_t(row) * stride + column];
        const double mean = double(sum) / (double(width) * height);
        require(mean > 122.0 && mean < 130.0, "the decoded picture is not the flat gray the stream codes");
        std::printf("PASS shape %s %ux%u mean-luma=%.1f\n", name, width, height, mean);
        std::fflush(stdout);
    }
    require(frame && eos, "capture frame/EOS timeout");
    type = output; call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF output");
    type = capture; call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF capture");
}
/* TRY_FMT answers with the layout S_FMT will set for a picture of any size,
 * including one whose rows the device pads: here every raw format the
 * decoder advertises, at sizes whose rows are and are not whole pitches. */
void tryRawLayouts(const char* device, unsigned coded)
{
    Device owner(device);
    const int fd = owner.fd;
    v4l2_format input {};
    input.type = output;
    input.fmt.pix_mp.width = 128; input.fmt.pix_mp.height = 96;
    input.fmt.pix_mp.pixelformat = coded;
    input.fmt.pix_mp.num_planes = 1;
    input.fmt.pix_mp.plane_fmt[0].sizeimage = 4096;
    call(fd, VIDIOC_S_FMT, &input, "S_FMT coded for TRY_FMT");
    static const unsigned sizes[][2] = {{16, 16}, {98, 76}, {120, 90}, {340, 180}, {342, 120}, {346, 100}, {1366, 768}};
    unsigned checked = 0;
    for (unsigned index = 0;; ++index) {
        v4l2_fmtdesc fmt {};
        fmt.type = capture; fmt.index = index;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) < 0) {
            require(errno == EINVAL, "ENUM_FMT capture failed");
            break;
        }
        if (rawLayout(fmt.pixelformat, 16, 16).empty()) continue;
        for (const auto& size : sizes) {
            v4l2_format candidate {};
            candidate.type = capture;
            candidate.fmt.pix_mp.width = size[0]; candidate.fmt.pix_mp.height = size[1];
            candidate.fmt.pix_mp.pixelformat = fmt.pixelformat;
            call(fd, VIDIOC_TRY_FMT, &candidate, "TRY_FMT capture size");
            const auto& answer = candidate.fmt.pix_mp;
            require(answer.pixelformat == fmt.pixelformat, "TRY_FMT substituted an advertised raw format");
            requireLayout(answer, rawLayout(fmt.pixelformat, answer.width, answer.height), "TRY_FMT");
            ++checked;
        }
    }
    require(checked > 0, "the decoder advertises no raw format the layout rule covers");
    std::printf("PASS try-fmt layouts %u\n", checked);
    std::fflush(stdout);
}

/* A buffer's scatter list is as long as the buffer has pieces, and in a guest
 * that has run for a while the pieces are small: Codec2 asks for a 64 MiB
 * bitstream buffer and gets one of sixteen thousand pages. The driver has to
 * attach that buffer without asking the kernel for one block as large as its
 * list, which the first version did (a 256 KiB order-6 allocation, and QBUF
 * failed with "page allocation failure"). Here the guest's free memory is in
 * pieces of 32 KiB or less, a 124 MiB coded buffer, the most the device
 * attaches, is created from them (the list of its mapped buffer), exported
 * and imported again by another open (the list of an imported dma-buf), and a
 * picture whose planes are tens of megabytes is decoded into mapped buffers,
 * whose lists are several pages long. */
void fragmentedAttach(const char* device, const std::string& directory, unsigned coded, const char* shape,
                      unsigned width, unsigned height)
{
    Fragmented fragments(8192);
    {
        constexpr unsigned size = 124u << 20;
        const auto primes = exportGuestPages(device, coded, {size}, false, 1);
        /* The run proves something only if nothing is left to build the list
         * from: no block of 64 KiB or more, which is where a list of this
         * size would have come from. */
        const auto blocks = freeBlocks();
        unsigned long large = 0;
        for (std::size_t order = 4; order < blocks.size(); ++order) large += blocks[order];
        std::printf("free blocks of 64 KiB or more: %lu\n", large);
        require(large <= 4, "the guest's memory was not fragmented");
        Device importer(device);
        const int fd = importer.fd;
        v4l2_format input {};
        input.type = output;
        input.fmt.pix_mp.width = 128; input.fmt.pix_mp.height = 96;
        input.fmt.pix_mp.pixelformat = coded;
        input.fmt.pix_mp.num_planes = 1;
        input.fmt.pix_mp.plane_fmt[0].sizeimage = 4096;
        call(fd, VIDIOC_S_FMT, &input, "S_FMT coded for import");
        import(fd, output, 1);
        queueImported(fd, output, 0, primes[0], {}, Declared::whole, 16);
        v4l2_requestbuffers none {};
        none.type = output; none.memory = V4L2_MEMORY_DMABUF; none.count = 0;
        call(fd, VIDIOC_REQBUFS, &none, "REQBUFS dmabuf release");
        std::printf("PASS fragmented attach %s %u MiB\n", shape, size >> 20);
        std::fflush(stdout);
    }
    decodeShape(device, directory, shape, coded, width, height);
}

/* The imported buffers a client that is not the driver's own exporter queues:
 * guest memory, with its planes in separate dma-bufs or in one, and their
 * lengths stated as QUERYBUF gave them, as the whole dma-buf, or not at all.
 * A plane after the first in a shared dma-buf starts past its own length, so
 * only the dma-buf's size can bound it. */
void importGuestPages(const char* device, const std::string& directory, const char* name,
                      unsigned coded, unsigned raw)
{
    for (const bool shared : {false, true})
        for (const auto declared : {Declared::negotiated, Declared::whole, Declared::none})
            decode(device, directory, name, coded, raw, V4L2_MEMORY_DMABUF, Import{true, shared, declared});
}
/* A client that renders what it decodes keeps its pictures in a pool and hands
 * the same descriptors back to the capture queue as they are released, so each
 * buffer is queued once per pass around that pool. This drives exactly that: a
 * ring of imported descriptors, every one queued again as soon as it is
 * dequeued, for many more pictures than the ring holds. A defect that appears
 * only on a buffer's second use stops the run after one pass, which is what a
 * client that decodes a handful of pictures and then gives up looks like from
 * the outside. */
void ringCycle(const char* device, const std::string& directory, const char* name,
               unsigned coded, unsigned raw, unsigned ring, unsigned pictures)
{
    const auto packet = read(directory + "/" + name + ".bin");
    Device owner(device);
    const int fd = owner.fd;
    const auto output = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    const auto capture = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    v4l2_event_subscription subscription {};
    subscription.type = V4L2_EVENT_SOURCE_CHANGE;
    call(fd, VIDIOC_SUBSCRIBE_EVENT, &subscription, "SUBSCRIBE_EVENT");

    v4l2_format input {};
    input.type = output;
    input.fmt.pix_mp.width = 128; input.fmt.pix_mp.height = 96;
    input.fmt.pix_mp.pixelformat = coded;
    input.fmt.pix_mp.num_planes = 1;
    input.fmt.pix_mp.plane_fmt[0].sizeimage = packet.size();
    call(fd, VIDIOC_S_FMT, &input, "S_FMT coded");
    auto inputs = allocate(fd, output, 4);
    require(inputs[0][0].length >= packet.size(), "coded buffer too short");
    auto type = output;
    call(fd, VIDIOC_STREAMON, &type, "STREAMON output");

    const auto feed = [&](unsigned index, unsigned sequence) {
        std::memcpy(inputs[index][0].data, packet.data(), packet.size());
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = output; buffer.memory = V4L2_MEMORY_MMAP; buffer.index = index;
        buffer.length = inputs[index].size(); buffer.m.planes = planes;
        buffer.timestamp.tv_sec = 7; buffer.timestamp.tv_usec = sequence;
        planes[0].length = inputs[index][0].length;
        planes[0].bytesused = packet.size();
        call(fd, VIDIOC_QBUF, &buffer, "QBUF coded");
    };

    unsigned sequence = 0;
    feed(0, ++sequence);
    unsigned free_inputs = unsigned(inputs.size()) - 1;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool source = false;
    while (!source && std::chrono::steady_clock::now() < deadline) {
        pollfd p {fd, POLLPRI, 0};
        if (poll(&p, 1, 100) < 0 && errno != EINTR) throw std::runtime_error("poll source change failed");
        v4l2_event event {};
        if (ioctl(fd, VIDIOC_DQEVENT, &event) == 0)
            source = event.type == V4L2_EVENT_SOURCE_CHANGE;
        else require(errno == EAGAIN, "DQEVENT failed");
    }
    require(source, "decoder source-change timeout");

    v4l2_format format {};
    format.type = capture;
    call(fd, VIDIOC_G_FMT, &format, "G_FMT capture");
    format.fmt.pix_mp.pixelformat = raw;
    call(fd, VIDIOC_S_FMT, &format, "S_FMT capture");
    /* The descriptors stand in for a client's own: exported once, then the
     * queue owns none of the memory it writes into. */
    auto maps = allocate(fd, capture, ring);
    auto primes = exportAndRelease(fd, capture, maps);
    maps.clear();
    release(fd, capture);
    const unsigned slots = import(fd, capture, unsigned(primes.size()));
    require(slots == primes.size(), "imported buffer count differs");
    for (unsigned i = 0; i < primes.size(); ++i) queueImported(fd, capture, i, primes[i]);
    type = capture;
    call(fd, VIDIOC_STREAMON, &type, "STREAMON capture");

    std::vector<unsigned> uses(primes.size(), 0);
    unsigned produced = 0, reused = 0;
    const auto started = std::chrono::steady_clock::now();
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
    while (produced < pictures && std::chrono::steady_clock::now() < deadline) {
        while (free_inputs && sequence < pictures + slots) {
            const auto index = unsigned(sequence % inputs.size());
            feed(index, ++sequence);
            --free_inputs;
        }
        pollfd p {fd, POLLIN | POLLOUT, 0};
        if (poll(&p, 1, 200) < 0 && errno != EINTR) throw std::runtime_error("poll ring failed");
        for (;;) {
            v4l2_plane planes[VIDEO_MAX_PLANES] {};
            v4l2_buffer buffer {};
            buffer.type = output; buffer.memory = V4L2_MEMORY_MMAP;
            buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
            if (ioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
                require(errno == EAGAIN, "DQBUF coded failed");
                break;
            }
            ++free_inputs;
        }
        for (;;) {
            v4l2_plane planes[VIDEO_MAX_PLANES] {};
            v4l2_buffer buffer {};
            buffer.type = capture; buffer.memory = V4L2_MEMORY_DMABUF;
            buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
            if (ioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
                require(errno == EAGAIN, "DQBUF capture failed");
                break;
            }
            require(buffer.index < primes.size(), "capture index outside the ring");
            if (buffer.flags & V4L2_BUF_FLAG_ERROR) {
                std::printf("RING-FAIL %s ring=%u picture=%u slot=%u uses=%u error-flagged\n",
                            name, ring, produced, buffer.index, uses[buffer.index]);
                std::fflush(stdout);
                require(false, "capture buffer came back error-flagged");
            }
            require(planes[0].bytesused > 0, "capture buffer came back empty");
            ++produced;
            if (uses[buffer.index]) ++reused;
            ++uses[buffer.index];
            /* The same descriptor, on the same slot, exactly as a pool hands
             * a released picture back. */
            errno = 0;
            v4l2_plane again[VIDEO_MAX_PLANES] {};
            v4l2_buffer requeue {};
            requeue.type = capture; requeue.memory = V4L2_MEMORY_DMABUF;
            requeue.index = buffer.index;
            requeue.length = primes[buffer.index].size(); requeue.m.planes = again;
            for (unsigned i = 0; i < primes[buffer.index].size(); ++i) {
                again[i].m.fd = primes[buffer.index][i].fd;
                again[i].length = primes[buffer.index][i].length;
            }
            if (ioctl(fd, VIDIOC_QBUF, &requeue) < 0) {
                std::printf("RING-FAIL %s ring=%u picture=%u slot=%u uses=%u requeue-errno=%d (%s)\n",
                            name, ring, produced, buffer.index, uses[buffer.index],
                            errno, std::strerror(errno));
                std::fflush(stdout);
                require(false, "re-queueing an imported capture buffer failed");
            }
        }
    }
    require(produced >= pictures, "the capture ring stopped producing pictures");
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    std::printf("PASS ring %s raw=%08x ring=%u pictures=%u reused=%u seconds=%.3f rate=%.1f\n",
                name, raw, ring, produced, reused, elapsed,
                elapsed > 0 ? produced / elapsed : 0.0);
    std::fflush(stdout);
}

/* The access units of an Annex B stream in which every unit opens with an
 * access unit delimiter. */
std::vector<std::vector<std::uint8_t>> delimitedUnits(const std::vector<std::uint8_t>& stream)
{
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i + 4 < stream.size(); ++i)
        if (stream[i] == 0 && stream[i + 1] == 0 && stream[i + 2] == 0 && stream[i + 3] == 1 &&
            (stream[i + 4] & 0x1f) == 9)
            starts.push_back(i);
    require(!starts.empty() && starts.front() == 0, "the stream does not open with an access unit delimiter");
    std::vector<std::vector<std::uint8_t>> units;
    for (std::size_t i = 0; i < starts.size(); ++i) {
        const auto end = i + 1 < starts.size() ? starts[i + 1] : stream.size();
        units.emplace_back(stream.begin() + std::ptrdiff_t(starts[i]), stream.begin() + std::ptrdiff_t(end));
    }
    return units;
}
/* A stream that changes its picture size twice, decoded the way a stateful
 * client does (Android's V4L2 codec among them): the capture queue is set up
 * at the first source change and again at each later one, and a client that
 * handles a change drops every picture it has not dequeued by then. So the
 * device has to return every picture of the old size, then a buffer marked
 * V4L2_BUF_FLAG_LAST, before the source change is visible, and the client
 * must see no picture of the new size in a buffer set up for the old one.
 * Every picture is flat gray, 8 levels brighter than the one before it, so the
 * brightness names the picture and a lost, repeated or reordered one shows.
 *
 * The eager client ends the stream the way such a client does: it asks for its
 * drain as soon as every coded buffer is back and takes the first LAST buffer
 * it sees as the end of that drain. The device must therefore not let a change
 * be pending then, which it does by keeping the coded buffer that carries the
 * change until the client has handled it, as a hardware decoder does. The
 * straight client queues every access unit and drains at once, so the changes
 * are inside its drain, and it takes the first LAST buffer for the end of it:
 * the device must not end an old size with one there, but announce the change
 * and carry on into the buffers the client then sets up. */
enum class Drain { afterPictures, eager, straight };
void resolutionChange(const char* device, const std::string& directory, unsigned coded,
                      unsigned memory = V4L2_MEMORY_MMAP, Import origin = {}, Drain drain = Drain::afterPictures)
{
    const bool eager = drain == Drain::eager;
    struct Sequence { unsigned width, height, pictures; };
    const Sequence sequences[] = {{128, 96, 8}, {96, 64, 8}, {160, 112, 8}};
    constexpr unsigned count = 3;
    constexpr unsigned serial = 424242;
    const auto units = delimitedUnits(read(directory + "/h264-resolution-change.bin"));
    unsigned total = 0;
    std::size_t largest = 0;
    for (const auto& sequence : sequences) total += sequence.pictures;
    for (const auto& unit : units) largest = std::max(largest, unit.size());
    require(units.size() == total, "the resolution-change stream has the wrong number of pictures");

    Device owner(device);
    const int fd = owner.fd;
    v4l2_event_subscription subscription {};
    subscription.type = V4L2_EVENT_SOURCE_CHANGE;
    call(fd, VIDIOC_SUBSCRIBE_EVENT, &subscription, "SUBSCRIBE_EVENT");
    v4l2_format input {};
    input.type = output;
    input.fmt.pix_mp.width = sequences[0].width; input.fmt.pix_mp.height = sequences[0].height;
    input.fmt.pix_mp.pixelformat = coded;
    input.fmt.pix_mp.num_planes = 1;
    input.fmt.pix_mp.plane_fmt[0].sizeimage = unsigned(largest);
    call(fd, VIDIOC_S_FMT, &input, "S_FMT coded");
    /* The straight client holds a buffer for every access unit, as one that
     * reads its whole stream ahead does. */
    auto inputs = allocate(fd, output, drain == Drain::straight ? total : 4);
    for (const auto& buffer : inputs) require(buffer[0].length >= largest, "coded buffer too short");
    std::vector<bool> free(inputs.size(), true);
    auto type = output;
    call(fd, VIDIOC_STREAMON, &type, "STREAMON output");

    Buffers frames;
    Primes primes;
    std::vector<unsigned> sizes;
    bool capturing = false, last = false, draining = false, drained = false;
    unsigned sequence = 0, received = 0, fed = 0, changes = 0, drainedAfter = 0, drainedChanges = 0;
    const auto firstOf = [&](unsigned index) {
        unsigned first = 0;
        for (unsigned i = 0; i < index; ++i) first += sequences[i].pictures;
        return first;
    };
    const auto set_up = [&](const Sequence& want) {
        v4l2_format format {};
        format.type = capture;
        call(fd, VIDIOC_G_FMT, &format, "G_FMT capture");
        require(format.fmt.pix_mp.width == want.width && format.fmt.pix_mp.height == want.height,
                "the capture format does not name the new picture size");
        if (capturing) {
            type = capture;
            call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF capture");
            frames.clear();
            primes.clear();
            release(fd, capture);
        }
        format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;
        call(fd, VIDIOC_S_FMT, &format, "S_FMT capture");
        require(format.fmt.pix_mp.width == want.width && format.fmt.pix_mp.height == want.height,
                "S_FMT changed the picture size");
        require(format.fmt.pix_mp.num_planes == 2, "the raw format is not two planes");
        sizes.clear();
        for (unsigned i = 0; i < format.fmt.pix_mp.num_planes; ++i)
            sizes.push_back(format.fmt.pix_mp.plane_fmt[i].sizeimage);
        if (memory != V4L2_MEMORY_DMABUF) {
            frames = allocate(fd, capture, 6);
            for (unsigned i = 0; i < frames.size(); ++i) queue(fd, capture, i, frames[i]);
        } else {
            /* A client that draws what it decodes brings new dma-bufs of the
             * new size. */
            if (origin.guestPages) {
                primes = exportGuestPages(device, coded, sizes, origin.shared, import(fd, capture, 4));
            } else {
                auto maps = allocate(fd, capture, 6);
                primes = exportAndRelease(fd, capture, maps);
                maps.clear();
                release(fd, capture);
                require(import(fd, capture, unsigned(primes.size())) == primes.size(), "imported buffer count differs");
            }
            frames.clear();
            for (auto& planes : primes) {
                frames.emplace_back();
                for (unsigned i = 0; i < planes.size(); ++i)
                    frames.back().emplace_back(planes[i].fd, origin.guestPages ? sizes.at(i) : planes[i].length,
                                               planes[i].offset);
            }
            for (unsigned i = 0; i < primes.size(); ++i)
                queueImported(fd, capture, i, primes[i], sizes, origin.declared);
        }
        type = capture;
        call(fd, VIDIOC_STREAMON, &type, "STREAMON capture");
        capturing = true;
        last = false;
    };
    const auto check = [&](const v4l2_buffer& buffer, const v4l2_plane* planes) {
        const auto& now = sequences[sequence];
        const unsigned id = unsigned(buffer.timestamp.tv_sec);
        require(buffer.timestamp.tv_usec == serial, "a picture lost its timestamp");
        require(id == received, "a picture was lost, repeated or returned out of order");
        require(id >= firstOf(sequence) && id < firstOf(sequence) + now.pictures,
                "a picture of another size came back in a buffer set up for this one");
        require(buffer.index < frames.size() && buffer.length == 2, "capture buffer is not two planes");
        const std::size_t luma = std::size_t(now.width) * now.height;
        require(planes[0].bytesused == luma && planes[1].bytesused == luma / 2,
                "a picture does not fill exactly the size the sequence has");
        const auto level = [](const void* data, std::size_t length, int wanted) {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            return std::all_of(bytes, bytes + length, [wanted](std::uint8_t v) { return std::abs(int(v) - wanted) <= 2; });
        };
        require(level(frames[buffer.index][0].data, luma, 16 + 8 * int(id)), "a picture is not the one its timestamp names");
        require(level(frames[buffer.index][1].data, luma / 2, 128), "a picture has the wrong chroma");
    };
    /* Dequeues what is ready; the LAST buffer ends it, and anything after it
     * is refused with EPIPE. */
    const auto collect = [&]() {
        while (capturing && !last) {
            v4l2_plane planes[VIDEO_MAX_PLANES] {};
            v4l2_buffer buffer {};
            buffer.type = capture; buffer.memory = memory;
            buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
            if (ioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
                require(errno == EAGAIN, "DQBUF capture failed");
                return;
            }
            require(!(buffer.flags & V4L2_BUF_FLAG_ERROR), "capture buffer has decoder error");
            if (planes[0].bytesused) {
                check(buffer, planes);
                ++received;
            }
            if (buffer.flags & V4L2_BUF_FLAG_LAST) {
                last = true;
                /* The LAST buffer ends a drain. A client that asked for one
                 * takes the first it sees as the end, so every picture has to
                 * be back by then. */
                if (draining) {
                    if (received != total) {
                        char text[160];
                        std::snprintf(text, sizeof text,
                                      "the LAST buffer of a drain came after %u of %u pictures, with %u size changes handled",
                                      received, total, changes);
                        throw std::runtime_error(text);
                    }
                    drained = true;
                }
                return;
            }
            if (memory == V4L2_MEMORY_DMABUF) queueImported(fd, capture, buffer.index, primes[buffer.index], sizes, origin.declared);
            else queue(fd, capture, buffer.index, frames[buffer.index]);
        }
    };
    const auto refused = [&]() {
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = capture; buffer.memory = memory;
        buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
        require(ioctl(fd, VIDIOC_DQBUF, &buffer) < 0 && errno == EPIPE,
                "a capture buffer came after the LAST one");
    };

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    std::chrono::steady_clock::time_point announced_at {};
    while (std::chrono::steady_clock::now() < deadline) {
        while (fed < units.size()) {
            const auto slot = std::find(free.begin(), free.end(), true);
            if (slot == free.end()) break;
            const auto index = unsigned(slot - free.begin());
            std::memcpy(inputs[index][0].data, units[fed].data(), units[fed].size());
            v4l2_plane planes[VIDEO_MAX_PLANES] {};
            v4l2_buffer buffer {};
            buffer.type = output; buffer.memory = V4L2_MEMORY_MMAP; buffer.index = index;
            buffer.length = inputs[index].size(); buffer.m.planes = planes;
            buffer.timestamp.tv_sec = fed; buffer.timestamp.tv_usec = serial;
            planes[0].length = inputs[index][0].length;
            planes[0].bytesused = unsigned(units[fed].size());
            call(fd, VIDIOC_QBUF, &buffer, "QBUF coded");
            *slot = false;
            ++fed;
        }
        pollfd p {fd, POLLIN | POLLOUT | POLLPRI, 0};
        if (poll(&p, 1, 100) < 0 && errno != EINTR) throw std::runtime_error("poll resolution change failed");
        for (;;) {
            v4l2_plane planes[VIDEO_MAX_PLANES] {};
            v4l2_buffer buffer {};
            buffer.type = output; buffer.memory = V4L2_MEMORY_MMAP;
            buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
            if (ioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
                require(errno == EAGAIN, "DQBUF coded failed");
                break;
            }
            require(!(buffer.flags & V4L2_BUF_FLAG_ERROR), "a coded buffer came back with an error");
            free.at(buffer.index) = true;
        }
        /* A client ends its stream with a drain as soon as every coded buffer
         * is back, without waiting for the pictures: it does not know a size
         * change is still on its way. Hardware holds back the coded buffer
         * that carries the change until the client has handled it, so for such
         * a client the drain never starts first. The patient client below
         * waits for the pictures instead. */
        if (!draining && capturing && fed == units.size() && (drain != Drain::afterPictures || received == total) &&
            (drain == Drain::straight || std::all_of(free.begin(), free.end(), [](bool v) { return v; }))) {
            v4l2_decoder_cmd stop {};
            stop.cmd = V4L2_DEC_CMD_STOP;
            call(fd, VIDIOC_DECODER_CMD, &stop, "DECODER_CMD drain");
            draining = true;
            drainedAfter = received;
            drainedChanges = changes;
        }
        /* Pictures first, then the event, which is what a client does: it
         * answers an event that was there when it woke, with the pictures that
         * were ready at the same moment. */
        bool event = p.revents & POLLPRI;
        /* The eager client is slow to look at events: it asks for its drain
         * while a size change is still on its way, which is the order a
         * client's own threads can produce. A device that holds the coded
         * buffer carrying the change back has not let it ask yet. */
        if (event && eager && changes > 0 && !draining) {
            const auto now = std::chrono::steady_clock::now();
            if (announced_at == std::chrono::steady_clock::time_point {}) announced_at = now;
            event = now - announced_at >= std::chrono::seconds(1);
            if (!event) usleep(20000);
        }
        if (event) announced_at = {};
        collect();
        if (event) {
            for (;;) {
                v4l2_event announced {};
                if (ioctl(fd, VIDIOC_DQEVENT, &announced) < 0) {
                    /* A non-blocking descriptor with no event pending is ENOENT. */
                    require(errno == ENOENT || errno == EAGAIN, "DQEVENT failed");
                    break;
                }
                if (announced.type != V4L2_EVENT_SOURCE_CHANGE ||
                    !(announced.u.src_change.changes & V4L2_EVENT_SRC_CH_RESOLUTION))
                    continue;
                if (changes++ == 0) {
                    set_up(sequences[0]);
                    continue;
                }
                require(sequence + 1 < count, "more size changes than the stream has");
                require(received == firstOf(sequence) + sequences[sequence].pictures,
                        "the source change came before every picture of the old size");
                /* The drain a client has asked for ends the old size by itself,
                 * at the end of the new one, so a change inside it has no
                 * LAST buffer of its own. */
                if (!draining) {
                    require(last, "the source change came before the LAST buffer");
                    refused();
                }
                set_up(sequences[++sequence]);
            }
        }
        if (drained) break;
    }
    require(drained, "the drain timed out");
    require(received == total, "the drain ended before every picture was returned");
    require(changes == count, "the stream's size changes were not all announced");
    refused();
    type = output; call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF output");
    type = capture; call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF capture");
    std::printf("PASS resolution-change sequences=%u pictures=%u memory=%s drain=%s drained-after=%u pictures, %u changes\n",
        count, received, memory != V4L2_MEMORY_DMABUF ? "mmap" : !origin.guestPages ? "dmabuf" : "dmabuf-guest",
        eager ? "eager" : drain == Drain::straight ? "straight" : "after-pictures", drainedAfter, drainedChanges);
    std::fflush(stdout);
}

/* The access units of an Annex B HEVC or H.264 stream: a new one starts at a
 * parameter set, an access unit delimiter or a prefix SEI after a picture's
 * slices, or at a slice that begins a picture. */
std::vector<std::vector<std::uint8_t>> accessUnits(const std::vector<std::uint8_t>& stream, bool avc = false)
{
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i + 3 < stream.size(); ++i)
        if (stream[i] == 0 && stream[i + 1] == 0 && stream[i + 2] == 1)
            starts.push_back(i > 0 && stream[i - 1] == 0 ? i - 1 : i);
    std::vector<std::vector<std::uint8_t>> units;
    bool picture = false;
    std::size_t begin = 0;
    for (const auto start : starts) {
        const auto nal = start + (stream[start] == 0 && stream[start + 1] == 0 && stream[start + 2] == 0 ? 4 : 3);
        if (nal + 2 >= stream.size()) break;
        const unsigned type = avc ? stream[nal] & 0x1f : (stream[nal] >> 1) & 0x3f;
        const bool slice = avc ? type == 1 || type == 5 : type < 32;
        // first_mb_in_slice (H.264) or first_slice_segment_in_pic_flag (HEVC)
        const bool first = slice && (stream[nal + (avc ? 1 : 2)] & 0x80);
        const bool boundary = avc ? type >= 6 && type <= 9 : (type >= 32 && type <= 35) || type == 39;
        if (picture && (first || boundary)) {
            units.emplace_back(stream.begin() + begin, stream.begin() + start);
            begin = start;
            picture = false;
        }
        if (slice) picture = true;
    }
    units.emplace_back(stream.begin() + begin, stream.end());
    return units;
}
/* The access unit without its parameter sets, as a player seeking to a random
 * access point sends it: it has them already. */
std::vector<std::uint8_t> withoutParameterSets(const std::vector<std::uint8_t>& unit, bool avc = false)
{
    std::vector<std::uint8_t> result;
    std::size_t begin = 0;
    const auto flush = [&](std::size_t end, bool keep) {
        if (keep) result.insert(result.end(), unit.begin() + begin, unit.begin() + end);
    };
    bool keep = true;
    for (std::size_t i = 0; i + 3 < unit.size(); ++i) {
        if (!(unit[i] == 0 && unit[i + 1] == 0 && unit[i + 2] == 1)) continue;
        const auto start = i > 0 && unit[i - 1] == 0 && i - 1 >= begin ? i - 1 : i;
        flush(start, keep);
        begin = start;
        const unsigned type = avc ? unit[i + 3] & 0x1f : (unit[i + 3] >> 1) & 0x3f;
        keep = avc ? type != 7 && type != 8 : type < 32 || type > 34;
    }
    flush(unit.size(), keep);
    return result;
}
/* A stateful decode of a stream of several access units, one per coded
 * buffer, collecting each picture's timestamp and brightness in the order the
 * decoder returns them. */
class OrderRun {
public:
    struct Picture { unsigned stamp; int luma; };
    OrderRun(const char* device, std::size_t largest, unsigned codec) : owner_(device), fd_(owner_.fd)
    {
        v4l2_event_subscription subscription {};
        subscription.type = V4L2_EVENT_SOURCE_CHANGE;
        call(fd_, VIDIOC_SUBSCRIBE_EVENT, &subscription, "SUBSCRIBE_EVENT");
        v4l2_format input {};
        input.type = output;
        input.fmt.pix_mp.width = 64; input.fmt.pix_mp.height = 64;
        input.fmt.pix_mp.pixelformat = codec;
        input.fmt.pix_mp.num_planes = 1;
        input.fmt.pix_mp.plane_fmt[0].sizeimage = largest;
        call(fd_, VIDIOC_S_FMT, &input, "S_FMT coded");
        inputs_ = allocate(fd_, output, 4);
        for (unsigned i = 0; i < inputs_.size(); ++i) {
            require(inputs_[i][0].length >= largest, "coded buffer too short");
            free_.push_back(i);
        }
        auto type = output;
        call(fd_, VIDIOC_STREAMON, &type, "STREAMON output");
    }
    void feed(const std::vector<std::uint8_t>& unit, unsigned stamp)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (free_.empty() && std::chrono::steady_clock::now() < deadline) pump();
        require(!free_.empty(), "no coded buffer came back");
        const auto index = free_.back();
        free_.pop_back();
        std::memcpy(inputs_[index][0].data, unit.data(), unit.size());
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = output; buffer.memory = V4L2_MEMORY_MMAP; buffer.index = index;
        buffer.length = 1; buffer.m.planes = planes;
        buffer.timestamp.tv_sec = 7; buffer.timestamp.tv_usec = stamp;
        planes[0].length = inputs_[index][0].length;
        planes[0].bytesused = unit.size();
        call(fd_, VIDIOC_QBUF, &buffer, "QBUF coded");
        if (!captureOn_) startCapture();
    }
    // A seek: the coded queue stops and starts again, and the capture queue runs on.
    void seek()
    {
        auto type = output;
        call(fd_, VIDIOC_STREAMOFF, &type, "STREAMOFF output");
        call(fd_, VIDIOC_STREAMON, &type, "STREAMON output");
        free_.clear();
        for (unsigned i = 0; i < inputs_.size(); ++i) free_.push_back(i);
    }
    // Drains the decoder and returns every picture it showed.
    const std::vector<Picture>& finish()
    {
        v4l2_decoder_cmd drain {};
        drain.cmd = V4L2_DEC_CMD_STOP;
        call(fd_, VIDIOC_DECODER_CMD, &drain, "DECODER_CMD drain");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!last_ && std::chrono::steady_clock::now() < deadline) pump();
        require(last_, "the decoder did not end the stream");
        return shown_;
    }

private:
    void startCapture()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        bool source = false;
        while (!source && std::chrono::steady_clock::now() < deadline) {
            pollfd p {fd_, POLLPRI, 0};
            if (poll(&p, 1, 100) < 0 && errno != EINTR) throw std::runtime_error("poll source change failed");
            v4l2_event event {};
            if (ioctl(fd_, VIDIOC_DQEVENT, &event) == 0) source = event.type == V4L2_EVENT_SOURCE_CHANGE;
            else require(errno == EAGAIN, "DQEVENT failed");
        }
        require(source, "decoder source-change timeout");
        v4l2_format format {};
        format.type = capture;
        call(fd_, VIDIOC_G_FMT, &format, "G_FMT capture");
        require(format.fmt.pix_mp.width == 64 && format.fmt.pix_mp.height == 64, "decoded dimensions differ");
        format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;
        call(fd_, VIDIOC_S_FMT, &format, "S_FMT capture");
        outputs_ = allocate(fd_, capture, 8);
        for (unsigned i = 0; i < outputs_.size(); ++i) queue(fd_, capture, i, outputs_[i]);
        auto type = capture;
        call(fd_, VIDIOC_STREAMON, &type, "STREAMON capture");
        captureOn_ = true;
    }
    void pump()
    {
        pollfd p {fd_, POLLIN | POLLOUT, 0};
        if (poll(&p, 1, 100) < 0 && errno != EINTR) throw std::runtime_error("poll failed");
        for (;;) {
            v4l2_plane planes[VIDEO_MAX_PLANES] {};
            v4l2_buffer buffer {};
            buffer.type = output; buffer.memory = V4L2_MEMORY_MMAP;
            buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
            if (ioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) { require(errno == EAGAIN, "DQBUF coded failed"); break; }
            free_.push_back(buffer.index);
        }
        if (!captureOn_) return;
        for (;;) {
            v4l2_plane planes[VIDEO_MAX_PLANES] {};
            v4l2_buffer buffer {};
            buffer.type = capture; buffer.memory = V4L2_MEMORY_MMAP;
            buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
            if (ioctl(fd_, VIDIOC_DQBUF, &buffer) < 0) {
                // The queue refuses once its last buffer has been taken.
                require(errno == EAGAIN || (last_ && errno == EPIPE), "DQBUF capture failed");
                break;
            }
            require(!(buffer.flags & V4L2_BUF_FLAG_ERROR), "capture buffer has decoder error");
            require(buffer.index < outputs_.size(), "capture index outside allocation");
            if (buffer.flags & V4L2_BUF_FLAG_LAST) {
                last_ = true;
                if (!planes[0].bytesused) continue;
            }
            if (planes[0].bytesused)
                shown_.push_back({unsigned(buffer.timestamp.tv_usec),
                                  static_cast<const std::uint8_t*>(outputs_[buffer.index][0].data)[planes[0].data_offset]});
            if (!last_) queue(fd_, capture, buffer.index, outputs_[buffer.index]);
        }
    }
    Device owner_;
    int fd_;
    Buffers inputs_, outputs_;
    std::vector<unsigned> free_;
    std::vector<Picture> shown_;
    bool captureOn_ {}, last_ {};
};
/* The same for H.264, whose pictures also come back as decoded: B-pictures and
 * reference B-pictures in groups the stream's own limits describe, the same
 * stream with the limits left to the level, a stream that cannot reorder, and a
 * seek to an IDR picture whose parameter sets the decoder already has. */
void h264DisplayOrder(const char* device, const std::string& directory)
{
    static const unsigned closedDisplay[] = {0, 4, 2, 1, 3, 8, 6, 5, 7, 11, 9, 10, 12, 16, 14, 13, 15, 19, 17, 18};
    for (const char* name : {"h264-bframes.bin", "h264-no-restriction.bin"}) {
        const auto units = accessUnits(read(directory + "/" + name), true);
        require(units.size() == 20, "the H.264 B-picture fixture is not twenty access units");
        std::size_t largest = 0;
        for (const auto& unit : units) largest = std::max(largest, unit.size());
        OrderRun run(device, largest, V4L2_PIX_FMT_H264);
        for (unsigned i = 0; i < units.size(); ++i) run.feed(units[i], i + 1);
        const auto& shown = run.finish();
        require(shown.size() == 20, "the decoder did not return every H.264 picture");
        for (unsigned i = 0; i < shown.size(); ++i) {
            require(shown[i].stamp >= 1 && shown[i].stamp <= 20 && closedDisplay[shown[i].stamp - 1] == i,
                    "H.264 pictures are not returned in display order");
            require(std::abs(shown[i].luma - int(16 + 10 * i)) <= 3, "an H.264 picture is not the one its timestamp names");
        }
        std::printf("PASS h264 display order: %s, 20 pictures\n", name);
        std::fflush(stdout);
    }
    {
        const auto units = accessUnits(read(directory + "/h264-poc2.bin"), true);
        std::size_t largest = 0;
        for (const auto& unit : units) largest = std::max(largest, unit.size());
        OrderRun run(device, largest, V4L2_PIX_FMT_H264);
        for (unsigned i = 0; i < units.size(); ++i) run.feed(units[i], i + 1);
        const auto& shown = run.finish();
        require(shown.size() == units.size(), "the decoder did not return every picture of a stream without reordering");
        for (unsigned i = 0; i < shown.size(); ++i)
            require(shown[i].stamp == i + 1 && std::abs(shown[i].luma - int(16 + 10 * i)) <= 3,
                    "a stream without reordering is shown as decoded");
        std::puts("PASS h264 order count type 2: pictures as decoded");
        std::fflush(stdout);
    }
    {
        const auto units = accessUnits(read(directory + "/h264-bframes.bin"), true);
        std::size_t largest = 0;
        for (const auto& unit : units) largest = std::max(largest, unit.size());
        OrderRun run(device, largest, V4L2_PIX_FMT_H264);
        for (unsigned i = 0; i < 7; ++i) run.feed(units[i], i + 1);
        run.seek();
        for (unsigned i = 12; i < units.size(); ++i) run.feed(i == 12 ? withoutParameterSets(units[i], true) : units[i], i + 1);
        const auto& shown = run.finish();
        std::vector<OrderRun::Picture> after;
        for (const auto& picture : shown) if (picture.stamp >= 13) after.push_back(picture);
        require(after.size() == 8, "the pictures after an H.264 seek are not those of the IDR picture");
        for (unsigned i = 0; i < after.size(); ++i)
            require(std::abs(after[i].luma - int(16 + 10 * (12 + i))) <= 3,
                    "H.264 pictures after a seek are not in display order");
        std::puts("PASS h264 seek: 8 pictures after a seek");
        std::fflush(stdout);
    }
}

/* Pictures with B-pictures leave in the order they are shown, whatever order
 * they were decoded in. The pictures of these streams are flat gray that rises
 * by 10 levels from one display position to the next, so the brightness of a
 * returned picture names its display position and its timestamp the order it
 * was decoded in. After a seek to a random access point whose leading pictures
 * need pictures the decoder never had, those are not shown. */
void hevcDisplayOrder(const char* device, const std::string& directory)
{
    static const unsigned closedDisplay[] = {0, 4, 2, 1, 3, 8, 6, 5, 7, 11, 10, 9, 12, 16, 14, 13, 15, 19, 18, 17};
    const auto closed = accessUnits(read(directory + "/hevc-bframes.bin"));
    require(closed.size() == 20, "the B-picture fixture is not twenty access units");
    std::size_t largest = 0;
    for (const auto& unit : closed) largest = std::max(largest, unit.size());
    {
        OrderRun run(device, largest, V4L2_PIX_FMT_HEVC);
        for (unsigned i = 0; i < closed.size(); ++i) run.feed(closed[i], i + 1);
        const auto& shown = run.finish();
        require(shown.size() == 20, "the decoder did not return every picture");
        for (unsigned i = 0; i < shown.size(); ++i) {
            require(shown[i].stamp >= 1 && shown[i].stamp <= 20 && closedDisplay[shown[i].stamp - 1] == i,
                    "pictures are not returned in display order");
            require(std::abs(shown[i].luma - int(16 + 10 * i)) <= 3, "a picture is not the one its timestamp names");
        }
        std::puts("PASS hevc display order: 20 pictures with B-pictures");
        std::fflush(stdout);
    }
    const auto open = accessUnits(read(directory + "/hevc-open-gop.bin"));
    require(open.size() == 24, "the open-group fixture is not twenty-four access units");
    largest = 0;
    for (const auto& unit : open) largest = std::max(largest, unit.size());
    {
        /* The first nine access units, a seek, then the second CRA picture and
         * what follows, without the parameter sets the decoder has already. The
         * three pictures that lead the CRA are not shown. */
        OrderRun run(device, largest, V4L2_PIX_FMT_HEVC);
        for (unsigned i = 0; i < 9; ++i) run.feed(open[i], i + 1);
        run.seek();
        for (unsigned i = 13; i < open.size(); ++i) run.feed(withoutParameterSets(open[i]), i + 1);
        const auto& shown = run.finish();
        std::vector<OrderRun::Picture> after;
        for (const auto& picture : shown) if (picture.stamp >= 14) after.push_back(picture);
        require(after.size() == 8, "the pictures after a seek are not those of the random access point");
        for (unsigned i = 0; i < after.size(); ++i)
            require(std::abs(after[i].luma - int(16 + 10 * (16 + i))) <= 3,
                    "pictures after a seek are not in display order");
        std::puts("PASS hevc seek: 8 pictures after a seek, leading pictures skipped");
        std::fflush(stdout);
    }
}

/* An access unit as the parameter sets that open it and the picture after
 * them, which a client may queue as two coded buffers. */
std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>
splitAtPicture(const std::vector<std::uint8_t>& unit, bool avc)
{
    for (std::size_t i = 0; i + 3 < unit.size(); ++i) {
        if (!(unit[i] == 0 && unit[i + 1] == 0 && unit[i + 2] == 1)) continue;
        const unsigned type = avc ? unit[i + 3] & 0x1f : (unit[i + 3] >> 1) & 0x3f;
        if (avc ? type != 1 && type != 5 : type >= 32) continue;
        const auto start = std::ptrdiff_t(i > 0 && unit[i - 1] == 0 ? i - 1 : i);
        return {{unit.begin(), unit.begin() + start}, {unit.begin() + start, unit.end()}};
    }
    throw std::runtime_error("the access unit has no picture");
}
/* Starts a stream before the picture size is known, as Android's V4L2 codec
 * (v4l2_codec2) does. It sets a provisional capture format, 64 x 64 NV12, and
 * with `buffers` queues buffers for it and starts the capture queue. It queues
 * the parameter sets and the first picture as two coded buffers, and only when
 * it dequeues the source change does it stop the capture queue and choose the
 * format and buffers for the size the stream has, P010 for a 10-bit one. Its
 * input thread may run ahead of its event thread, so the pause before the event
 * is dequeued here is the interleaving it produces.
 *
 * The device must keep the coded buffer that declared the size until the
 * client has set its capture queue up for it, because a client drains once
 * every coded buffer is back and a drain it starts with no change pending ends
 * at once, without the picture. No picture may go into the provisional buffers,
 * which are too small for it, and the picture must keep the stream's depth for
 * a client that chooses P010 after it was decoded. The picture then arrives
 * whole, ahead of the LAST buffer. */
void provisionalCapture(const char* device, const std::string& directory, const char* name, unsigned coded,
                        unsigned raw, bool buffers)
{
    constexpr unsigned guessed = 64;
    const auto unit = read(directory + "/" + name + ".bin");
    const auto reference = read(directory + "/" + name + ".raw");
    const auto [sets, picture] = splitAtPicture(unit, coded == V4L2_PIX_FMT_H264);
    require(!sets.empty(), "the access unit has no parameter sets before its picture");
    Device owner(device);
    const int fd = owner.fd;
    v4l2_event_subscription subscription {};
    subscription.type = V4L2_EVENT_SOURCE_CHANGE;
    call(fd, VIDIOC_SUBSCRIBE_EVENT, &subscription, "SUBSCRIBE_EVENT");
    v4l2_format input {};
    input.type = output;
    input.fmt.pix_mp.width = guessed; input.fmt.pix_mp.height = guessed;
    input.fmt.pix_mp.pixelformat = coded;
    input.fmt.pix_mp.num_planes = 1;
    input.fmt.pix_mp.plane_fmt[0].sizeimage = unit.size();
    call(fd, VIDIOC_S_FMT, &input, "S_FMT coded");
    auto inputs = allocate(fd, output, 2);
    require(inputs[0][0].length >= unit.size() && inputs[1][0].length >= unit.size(), "coded buffer too short");
    v4l2_format format {};
    format.type = capture;
    format.fmt.pix_mp.width = guessed; format.fmt.pix_mp.height = guessed;
    format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12M;
    call(fd, VIDIOC_S_FMT, &format, "S_FMT provisional capture");
    require(format.fmt.pix_mp.num_planes == 2, "the provisional format is not two planes");
    std::vector<unsigned> sizes;
    for (unsigned i = 0; i < format.fmt.pix_mp.num_planes; ++i)
        sizes.push_back(format.fmt.pix_mp.plane_fmt[i].sizeimage);
    const unsigned count = import(fd, capture, 4);
    Primes provisional;
    if (buffers) {
        provisional = exportGuestPages(device, coded, sizes, false, count);
        for (unsigned i = 0; i < provisional.size(); ++i)
            queueImported(fd, capture, i, provisional[i], sizes, Declared::negotiated);
        auto type = capture;
        call(fd, VIDIOC_STREAMON, &type, "STREAMON provisional capture");
    }
    auto type = output;
    call(fd, VIDIOC_STREAMON, &type, "STREAMON output");
    const auto submit = [&](unsigned index, const std::vector<std::uint8_t>& packet, long stamp) {
        std::memcpy(inputs[index][0].data, packet.data(), packet.size());
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = output; buffer.memory = V4L2_MEMORY_MMAP; buffer.index = index;
        buffer.length = 1; buffer.m.planes = planes;
        buffer.timestamp.tv_sec = stamp; buffer.timestamp.tv_usec = 424242;
        planes[0].length = inputs[index][0].length;
        planes[0].bytesused = unsigned(packet.size());
        call(fd, VIDIOC_QBUF, &buffer, "QBUF coded");
    };
    const auto dequeueCoded = [&]() {
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = output; buffer.memory = V4L2_MEMORY_MMAP;
        buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
        if (ioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
            require(errno == EAGAIN, "DQBUF coded failed");
            return false;
        }
        require(!(buffer.flags & V4L2_BUF_FLAG_ERROR), "a coded buffer came back with an error");
        return true;
    };
    submit(0, sets, 1);
    submit(1, picture, 2);
    usleep(50000);
    unsigned back = 0;
    while (dequeueCoded()) ++back;
    require(back <= 1, "both coded buffers were back before the source change was handled");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    bool source = false;
    while (!source && std::chrono::steady_clock::now() < deadline) {
        pollfd p {fd, POLLPRI, 0};
        if (poll(&p, 1, 100) < 0 && errno != EINTR) throw std::runtime_error("poll source change failed");
        v4l2_event event {};
        if (ioctl(fd, VIDIOC_DQEVENT, &event) == 0)
            source = event.type == V4L2_EVENT_SOURCE_CHANGE && (event.u.src_change.changes & V4L2_EVENT_SRC_CH_RESOLUTION);
        else require(errno == ENOENT || errno == EAGAIN, "DQEVENT failed");
    }
    require(source, "decoder source-change timeout");
    call(fd, VIDIOC_G_FMT, &format, "G_FMT capture");
    require(format.fmt.pix_mp.width == 128 && format.fmt.pix_mp.height == 96, "decoded dimensions differ");
    if (buffers) {
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = capture; buffer.memory = V4L2_MEMORY_DMABUF;
        buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
        require(ioctl(fd, VIDIOC_DQBUF, &buffer) < 0 && errno == EAGAIN,
                "a picture came back in a buffer set up for the provisional size");
        type = capture;
        call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF provisional capture");
    }
    provisional.clear();
    release(fd, capture);
    format.fmt.pix_mp.pixelformat = raw;
    call(fd, VIDIOC_S_FMT, &format, "S_FMT capture");
    require(format.fmt.pix_mp.pixelformat == raw && format.fmt.pix_mp.width == 128 && format.fmt.pix_mp.height == 96,
            "S_FMT changed the capture format");
    const auto layout = rawLayout(raw, format.fmt.pix_mp.width, format.fmt.pix_mp.height);
    requireLayout(format.fmt.pix_mp, layout, "S_FMT");
    sizes.clear();
    for (unsigned i = 0; i < format.fmt.pix_mp.num_planes; ++i)
        sizes.push_back(format.fmt.pix_mp.plane_fmt[i].sizeimage);
    const auto primes = exportGuestPages(device, coded, sizes, false, import(fd, capture, 4));
    Buffers frames;
    for (const auto& planes : primes) {
        frames.emplace_back();
        for (unsigned i = 0; i < planes.size(); ++i) frames.back().emplace_back(planes[i].fd, sizes.at(i), planes[i].offset);
    }
    for (unsigned i = 0; i < primes.size(); ++i) queueImported(fd, capture, i, primes[i], sizes, Declared::negotiated);
    type = capture;
    call(fd, VIDIOC_STREAMON, &type, "STREAMON capture");
    v4l2_decoder_cmd drain {};
    drain.cmd = V4L2_DEC_CMD_STOP;
    call(fd, VIDIOC_DECODER_CMD, &drain, "DECODER_CMD drain");
    unsigned pictures = 0, maximum = 0;
    bool last = false;
    while (!last && std::chrono::steady_clock::now() < deadline) {
        pollfd p {fd, POLLIN, 0};
        if (poll(&p, 1, 100) < 0 && errno != EINTR) throw std::runtime_error("poll capture failed");
        while (dequeueCoded()) ++back;
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = capture; buffer.memory = V4L2_MEMORY_DMABUF;
        buffer.length = VIDEO_MAX_PLANES; buffer.m.planes = planes;
        if (ioctl(fd, VIDIOC_DQBUF, &buffer) < 0) {
            require(errno == EAGAIN, "DQBUF capture failed");
            continue;
        }
        require(!(buffer.flags & V4L2_BUF_FLAG_ERROR), "capture buffer has decoder error");
        last = buffer.flags & V4L2_BUF_FLAG_LAST;
        if (!planes[0].bytesused) continue;
        require(!pictures++, "decoder returned duplicate frame");
        require(buffer.timestamp.tv_sec == 2 && buffer.timestamp.tv_usec == 424242, "frame timestamp differs");
        require(buffer.index < frames.size(), "capture index outside allocation");
        require(buffer.length == layout.size(), "the decoded buffer has another plane count than its format");
        const auto actual = tightPicture(layout, planes, frames[buffer.index]);
        require(actual.size() == reference.size(), "the picture is not the size of its reference");
        const unsigned step = raw == V4L2_PIX_FMT_P010 ? 2 : 1;
        for (std::size_t i = 0; i < actual.size(); i += step) {
            const int got = step == 1 ? actual[i] : (actual[i] | (actual[i + 1] << 8)) >> 6;
            const int wanted = step == 1 ? reference[i] : (reference[i] | (reference[i + 1] << 8)) >> 6;
            maximum = std::max(maximum, unsigned(std::abs(got - wanted)));
        }
        require(maximum <= 2, "the picture is not the one the stream codes");
    }
    require(last, "capture EOS timeout");
    require(pictures == 1, "the LAST buffer came without the picture");
    for (const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(30);
         back < 2 && std::chrono::steady_clock::now() < limit; usleep(10000))
        while (dequeueCoded()) ++back;
    require(back == 2, "a coded buffer was never given back");
    type = output; call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF output");
    type = capture; call(fd, VIDIOC_STREAMOFF, &type, "STREAMOFF capture");
    std::printf("PASS provisional-capture %s raw=%08x buffers=%s max-error=%u\n", name, raw,
        buffers ? "queued" : "none", maximum);
    std::fflush(stdout);
}

/* The values a read-only menu lists, in order. */
std::vector<int> menuItems(int fd, unsigned id)
{
    v4l2_queryctrl control {};
    control.id = id;
    call(fd, VIDIOC_QUERYCTRL, &control, "QUERYCTRL menu");
    require(control.type == V4L2_CTRL_TYPE_MENU && control.minimum >= 0 && control.maximum < 32, "invalid menu bounds");
    require((control.flags & V4L2_CTRL_FLAG_READ_ONLY) != 0, "a decoder menu can be written");
    std::vector<int> items;
    for (int value = control.minimum; value <= control.maximum; ++value) {
        v4l2_querymenu menu {};
        menu.id = id; menu.index = unsigned(value);
        if (ioctl(fd, VIDIOC_QUERYMENU, &menu) < 0) {
            require(errno == EINVAL, "QUERYMENU failed");
            continue;
        }
        items.push_back(value);
    }
    require(!items.empty(), "a menu lists nothing");
    return items;
}

/* The profile and level menus a client reads to decide what the decoder takes.
 * Constrained Baseline and Constrained High are subsets of Baseline and High
 * that the host names as profiles of their own, so a stream that states them is
 * not refused for lacking an entry. The levels are everything from the lowest to
 * the highest the host decodes, with no gap but the 1b the wire lacks; the host
 * decodes H.264 up to level 5.2 (the largest frame its hardware opens, which
 * the CTS encoder profile and level tests send) and HEVC up to 6.2. */
void profileAndLevelMenus(int fd, bool avc)
{
    const auto has = [](const std::vector<int>& items, int value) {
        return std::find(items.begin(), items.end(), value) != items.end();
    };
    const auto profiles = menuItems(fd, avc ? V4L2_CID_MPEG_VIDEO_H264_PROFILE : V4L2_CID_MPEG_VIDEO_HEVC_PROFILE);
    const auto levels = menuItems(fd, avc ? V4L2_CID_MPEG_VIDEO_H264_LEVEL : V4L2_CID_MPEG_VIDEO_HEVC_LEVEL);
    if (avc) {
        require(has(profiles, V4L2_MPEG_VIDEO_H264_PROFILE_HIGH), "the H.264 profile menu lacks High");
        require(has(profiles, V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_HIGH),
                "the H.264 profile menu lacks Constrained High, a subset of the High it lists");
        require(has(profiles, V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE) ==
                has(profiles, V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE),
                "the H.264 profile menu lists Baseline without Constrained Baseline or the reverse");
        require(levels.front() == V4L2_MPEG_VIDEO_H264_LEVEL_1_0, "the H.264 level menu does not start at level 1.0");
        require(levels.back() >= V4L2_MPEG_VIDEO_H264_LEVEL_5_2,
                "the H.264 level menu stops below level 5.2, which the hardware decodes");
        for (int level = levels.front(); level <= levels.back(); ++level)
            require(level == V4L2_MPEG_VIDEO_H264_LEVEL_1B || has(levels, level), "the H.264 level menu has a gap");
        std::printf("H.264 profiles %zu, levels up to menu index %d\n", profiles.size(), levels.back());
    } else {
        require(has(profiles, V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN), "the HEVC profile menu lacks Main");
        require(levels.front() == V4L2_MPEG_VIDEO_HEVC_LEVEL_1, "the HEVC level menu does not start at level 1");
        require(levels.back() == V4L2_MPEG_VIDEO_HEVC_LEVEL_6_2, "the HEVC level menu does not reach level 6.2");
        for (int level = levels.front(); level <= levels.back(); ++level)
            require(has(levels, level), "the HEVC level menu has a gap");
        std::printf("HEVC profiles %zu, levels up to menu index %d\n", profiles.size(), levels.back());
    }
    std::fflush(stdout);
}

/* The size of the coded queue's buffers follows the picture the client states
 * with its coded format: 1 MiB up to 1280 x 720, 2 MiB up to 1920 x 1088, 4 MiB
 * up to 4096 x 2304 and 8 MiB, the largest packet the device takes, above that,
 * and every JPEG stream keeps the 8 MiB. A picture the format does not take is
 * stated as the nearest one it does, so a client that states none (the Android
 * V4L2 HAL passes ui::Size(), -1 x -1, which reaches the driver as 0xFFFFFFFF
 * in both fields) is sized for the largest picture of the format: it must
 * allocate buffers of the size S_FMT answers or QBUF refuses them. The queue
 * allocates the size S_FMT answers, which is the one the client reads. */
void codedBufferSizes(const char* device, unsigned coded)
{
    constexpr unsigned mebibyte = 1u << 20;
    const auto tier = [coded](unsigned width, unsigned height) {
        if (coded == V4L2_PIX_FMT_MJPEG) return 8 * mebibyte;
        const std::uint64_t area = std::uint64_t(width) * height;
        if (area <= 1280u * 720u) return mebibyte;
        if (area <= 1920u * 1088u) return 2 * mebibyte;
        if (area <= 4096u * 2304u) return 4 * mebibyte;
        return 8 * mebibyte;
    };
    Device decoder(device);
    v4l2_frmsizeenum range {};
    range.index = 0; range.pixel_format = coded;
    call(decoder.fd, VIDIOC_ENUM_FRAMESIZES, &range, "ENUM_FRAMESIZES");
    require(range.type == V4L2_FRMSIZE_TYPE_STEPWISE, "frame sizes are not reported stepwise");
    const auto state = [&decoder, coded](unsigned width, unsigned height) {
        v4l2_format format {};
        format.type = output;
        format.fmt.pix_mp.width = width; format.fmt.pix_mp.height = height;
        format.fmt.pix_mp.pixelformat = coded;
        format.fmt.pix_mp.num_planes = 1;
        call(decoder.fd, VIDIOC_S_FMT, &format, "S_FMT coded size");
        require(format.fmt.pix_mp.num_planes == 1, "the coded queue is not one plane");
        return format;
    };
    const auto queued = [&decoder](unsigned expected, const char* message) {
        v4l2_format current {};
        current.type = output;
        call(decoder.fd, VIDIOC_G_FMT, &current, "G_FMT coded size");
        require(current.fmt.pix_mp.plane_fmt[0].sizeimage == expected, message);
        v4l2_requestbuffers request {};
        request.type = output; request.memory = V4L2_MEMORY_DMABUF; request.count = 1;
        call(decoder.fd, VIDIOC_REQBUFS, &request, "REQBUFS coded size");
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = output; buffer.memory = V4L2_MEMORY_DMABUF; buffer.index = 0;
        buffer.m.planes = planes; buffer.length = 1;
        call(decoder.fd, VIDIOC_QUERYBUF, &buffer, "QUERYBUF coded size");
        request.count = 0;
        call(decoder.fd, VIDIOC_REQBUFS, &request, "REQBUFS release coded size");
        require(planes[0].length == expected, "the coded queue allocates another size than G_FMT answers");
    };
    struct Case { unsigned width, height; };
    // One stream states several sizes in turn, the largest first and last.
    for (const Case size : {Case {3840, 2160}, Case {64, 64}, Case {1280, 720}, Case {720, 1280}, Case {1920, 1080},
                            Case {1088, 1920}, Case {2560, 1440}, Case {3840, 2160}}) {
        const auto format = state(size.width, size.height);
        require(format.fmt.pix_mp.width == size.width && format.fmt.pix_mp.height == size.height,
                "S_FMT changed a picture size inside the format's range");
        require(format.fmt.pix_mp.plane_fmt[0].sizeimage == tier(size.width, size.height),
                "the coded buffer does not follow the stated picture size");
        queued(tier(size.width, size.height), "G_FMT disagrees with S_FMT about the coded buffer size");
    }
    // No size stated, and sizes beyond the format's range: the largest picture it takes.
    for (const Case size : {Case {0xFFFFFFFFu, 0xFFFFFFFFu}, Case {16384, 16384}}) {
        const auto format = state(size.width, size.height);
        const unsigned width = format.fmt.pix_mp.width, height = format.fmt.pix_mp.height;
        require(width == range.stepwise.max_width && height == range.stepwise.max_height,
                "a picture beyond the format's range is not stated as the largest the format takes");
        require(format.fmt.pix_mp.plane_fmt[0].sizeimage == tier(width, height),
                "the coded buffer does not follow the clamped picture size");
        queued(tier(width, height), "G_FMT disagrees with S_FMT about the coded buffer size");
    }
    std::printf("coded buffer sizes of %.4s follow the stated picture\n", reinterpret_cast<const char*>(&coded));
    std::fflush(stdout);
}

int main(int argc, char** argv)
{
    try {
        require(argc >= 2, "usage: v4l2_decode_probe FIXTURE_DIRECTORY [DEVICE]");
        const char* device = argc > 2 ? argv[2] : "/dev/video0";
        std::printf("V4L2 guest page size: %ld\n", sysconf(_SC_PAGESIZE));
        struct Profile { unsigned value; const char* name; unsigned raw; };
        const Profile h264[] = {
            {V4L2_MPEG_VIDEO_H264_PROFILE_BASELINE, "h264-baseline", V4L2_PIX_FMT_NV12M},
            {V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_BASELINE, "h264-constrained-baseline", V4L2_PIX_FMT_NV12M},
            {V4L2_MPEG_VIDEO_H264_PROFILE_MAIN, "h264-main", V4L2_PIX_FMT_NV12M},
            {V4L2_MPEG_VIDEO_H264_PROFILE_HIGH, "h264-high", V4L2_PIX_FMT_NV12M},
            {V4L2_MPEG_VIDEO_H264_PROFILE_CONSTRAINED_HIGH, "h264-constrained-high", V4L2_PIX_FMT_NV12M},
            {V4L2_MPEG_VIDEO_H264_PROFILE_HIGH_10, "h264-high10", V4L2_PIX_FMT_P010},
        };
        const Profile hevc[] = {
            {V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN, "hevc-main", V4L2_PIX_FMT_NV12M},
            {V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN_10, "hevc-main10", V4L2_PIX_FMT_P010},
        };
        Device capabilities(device);
        /* The discovery a V4L2 client performs before it builds a decoder.
         * Each of these answers gated Chromium's decoder entirely: a
         * continuous frame-size range left it on its built-in maximum coded
         * size, an unimplemented TRY_FMT made it discard every capture
         * candidate, and a reversed format list hid the planar formats a
         * coded format names. */
        for (unsigned index = 0;; ++index) {
            v4l2_fmtdesc coded {};
            coded.type = output; coded.index = index;
            if (ioctl(capabilities.fd, VIDIOC_ENUM_FMT, &coded) < 0) {
                require(errno == EINVAL, "ENUM_FMT failed"); break;
            }
            v4l2_frmsizeenum sizes {};
            sizes.index = 0; sizes.pixel_format = coded.pixelformat;
            call(capabilities.fd, VIDIOC_ENUM_FRAMESIZES, &sizes, "ENUM_FRAMESIZES");
            require(sizes.type == V4L2_FRMSIZE_TYPE_STEPWISE,
                    "frame sizes are not reported stepwise");
            require(sizes.stepwise.step_width >= 1 && sizes.stepwise.step_height >= 1,
                    "a stepwise frame size has a zero step");
            require(sizes.stepwise.max_width >= 1920 && sizes.stepwise.max_height >= 1200,
                    "the advertised maximum coded size is too small");
            /* The range is a rectangle, so it names a portrait picture as it names a landscape one. */
            if (coded.pixelformat == V4L2_PIX_FMT_H264)
                require(sizes.stepwise.max_width >= 4096 && sizes.stepwise.max_height >= 4096,
                        "the advertised H.264 range stops short of 4096 x 4096");
            if (coded.pixelformat == V4L2_PIX_FMT_HEVC)
                require(sizes.stepwise.max_width >= 8192 && sizes.stepwise.max_height >= 8192,
                        "the advertised HEVC range stops short of 8192 x 8192");
            /* A capture list in wire order names the planar format the device
             * actually decodes to; a reversed one names only the tail. */
            bool planar = false;
            for (unsigned raw = 0;; ++raw) {
                v4l2_fmtdesc fmt {};
                fmt.type = capture; fmt.index = raw;
                if (ioctl(capabilities.fd, VIDIOC_ENUM_FMT, &fmt) < 0) {
                    require(errno == EINVAL, "ENUM_FMT capture failed"); break;
                }
                planar = planar || fmt.pixelformat == V4L2_PIX_FMT_NV12M;
            }
            require(planar, "the capture list does not name the planar format");
            codedBufferSizes(device, coded.pixelformat);
        }
        /* The pattern a rendering client produces: a small ring of imported
         * descriptors carried through many more pictures than it holds, so
         * every buffer is used several times. It runs before the rest because
         * a ring that stops after one pass is the failure a client sees. */
        for (unsigned index = 0;; ++index) {
            v4l2_fmtdesc fmt {};
            fmt.type = output; fmt.index = index;
            if (ioctl(capabilities.fd, VIDIOC_ENUM_FMT, &fmt) < 0) {
                require(errno == EINVAL, "ENUM_FMT failed"); break;
            }
            if (fmt.pixelformat != V4L2_PIX_FMT_VP9) continue;
            ringCycle(device, argv[1], "vp9-profile0", fmt.pixelformat,
                      V4L2_PIX_FMT_NV12M, 8, 40);
            ringCycle(device, argv[1], "vp9-profile0", fmt.pixelformat,
                      V4L2_PIX_FMT_ABGR32, 8, 40);
            break;
        }
        /* A stream whose picture size changes twice, decoded as a stateful
         * client does it. */
        for (unsigned index = 0;; ++index) {
            v4l2_fmtdesc fmt {};
            fmt.type = output; fmt.index = index;
            if (ioctl(capabilities.fd, VIDIOC_ENUM_FMT, &fmt) < 0) {
                require(errno == EINVAL, "ENUM_FMT failed"); break;
            }
            if (fmt.pixelformat != V4L2_PIX_FMT_H264) continue;
            resolutionChange(device, argv[1], fmt.pixelformat);
            resolutionChange(device, argv[1], fmt.pixelformat, V4L2_MEMORY_DMABUF);
            resolutionChange(device, argv[1], fmt.pixelformat, V4L2_MEMORY_DMABUF,
                             Import{true, false, Declared::negotiated});
            resolutionChange(device, argv[1], fmt.pixelformat, V4L2_MEMORY_MMAP, {}, Drain::eager);
            resolutionChange(device, argv[1], fmt.pixelformat, V4L2_MEMORY_MMAP, {}, Drain::straight);
            resolutionChange(device, argv[1], fmt.pixelformat, V4L2_MEMORY_DMABUF,
                             Import{true, false, Declared::negotiated}, Drain::straight);
            break;
        }
        unsigned tested = 0;
        for (unsigned index = 0;; ++index) {
            v4l2_fmtdesc fmt {};
            fmt.type = output; fmt.index = index;
            if (ioctl(capabilities.fd, VIDIOC_ENUM_FMT, &fmt) < 0) {
                require(errno == EINVAL, "ENUM_FMT failed"); break;
            }
            if (fmt.pixelformat == V4L2_PIX_FMT_MJPEG) {
                decode(device, argv[1], "jpeg", fmt.pixelformat, V4L2_PIX_FMT_NV12M);
                decode(device, argv[1], "jpeg", fmt.pixelformat, V4L2_PIX_FMT_NV12M, V4L2_MEMORY_DMABUF);
                decode(device, argv[1], "jpeg", fmt.pixelformat, V4L2_PIX_FMT_ABGR32);
                decode(device, argv[1], "jpeg", fmt.pixelformat, V4L2_PIX_FMT_ABGR32, V4L2_MEMORY_DMABUF);
                ++tested; continue;
            }
            // VP9 states its profile in the stream itself, so the driver
            // exposes no profile menu to walk: the advertised format is the
            // whole capability.
            if (fmt.pixelformat == V4L2_PIX_FMT_VP9) {
                decode(device, argv[1], "vp9-profile0", fmt.pixelformat, V4L2_PIX_FMT_NV12M);
                decode(device, argv[1], "vp9-profile0", fmt.pixelformat, V4L2_PIX_FMT_NV12M,
                       V4L2_MEMORY_DMABUF);
                decode(device, argv[1], "vp9-profile0", fmt.pixelformat, V4L2_PIX_FMT_ABGR32);
                decode(device, argv[1], "vp9-profile0", fmt.pixelformat, V4L2_PIX_FMT_ABGR32,
                       V4L2_MEMORY_DMABUF);
                ++tested; continue;
            }
            require(fmt.pixelformat == V4L2_PIX_FMT_H264 || fmt.pixelformat == V4L2_PIX_FMT_HEVC, "advertised codec lacks a fixture");
            const bool avc = fmt.pixelformat == V4L2_PIX_FMT_H264;
            const unsigned id = avc ? V4L2_CID_MPEG_VIDEO_H264_PROFILE : V4L2_CID_MPEG_VIDEO_HEVC_PROFILE;
            v4l2_queryctrl control {}; control.id = id;
            call(capabilities.fd, VIDIOC_QUERYCTRL, &control, "QUERYCTRL profiles");
            require(control.minimum >= 0 && control.maximum <= 32, "invalid profile menu bounds");
            profileAndLevelMenus(capabilities.fd, avc);
            if (avc) {
                tryRawLayouts(device, fmt.pixelformat);
                /* A window onto the coded picture leaves a width that is not a whole pitch. */
                decode(device, argv[1], "h264-window", fmt.pixelformat, V4L2_PIX_FMT_NV12M, V4L2_MEMORY_MMAP, {}, Picture{120, 90});
                decode(device, argv[1], "h264-window", fmt.pixelformat, V4L2_PIX_FMT_ABGR32, V4L2_MEMORY_MMAP, {}, Picture{120, 90});
                decode(device, argv[1], "h264-window", fmt.pixelformat, V4L2_PIX_FMT_NV12M, V4L2_MEMORY_DMABUF,
                       Import{true, false, Declared::negotiated}, Picture{120, 90});
                decodeShape(device, argv[1], "h264-portrait-2304x4096", fmt.pixelformat, 2304, 4096);
                decodeShape(device, argv[1], "h264-square-4096x4096", fmt.pixelformat, 4096, 4096);
                fragmentedAttach(device, argv[1], fmt.pixelformat, "h264-square-4096x4096", 4096, 4096);
            } else {
                tryRawLayouts(device, fmt.pixelformat);
                decode(device, argv[1], "hevc-window", fmt.pixelformat, V4L2_PIX_FMT_NV12M, V4L2_MEMORY_MMAP, {}, Picture{120, 90});
                decode(device, argv[1], "hevc-window", fmt.pixelformat, V4L2_PIX_FMT_ABGR32, V4L2_MEMORY_MMAP, {}, Picture{120, 90});
                decode(device, argv[1], "hevc-window", fmt.pixelformat, V4L2_PIX_FMT_NV12M, V4L2_MEMORY_DMABUF,
                       Import{true, false, Declared::negotiated}, Picture{120, 90});
                decodeShape(device, argv[1], "hevc-portrait-4320x8192", fmt.pixelformat, 4320, 8192);
                fragmentedAttach(device, argv[1], fmt.pixelformat, "hevc-portrait-4320x8192", 4320, 8192);
            }
            bool imported = false;
            for (int value = control.minimum; value <= control.maximum; ++value) {
                v4l2_querymenu menu {}; menu.id = id; menu.index = value;
                if (ioctl(capabilities.fd, VIDIOC_QUERYMENU, &menu) < 0) {
                    require(errno == EINVAL, "QUERYMENU profile failed"); continue;
                }
                const auto* first = avc ? std::begin(h264) : std::begin(hevc);
                const auto* last = avc ? std::end(h264) : std::end(hevc);
                const auto* profile = std::find_if(first, last, [value](const auto& p) { return p.value == unsigned(value); });
                require(profile != last, "advertised profile lacks a fixture");
                decode(device, argv[1], profile->name, fmt.pixelformat, profile->raw);
                /* Packed capture is what a client that only draws through a
                 * 32-bit RGB surface negotiates, and it is offered whatever
                 * the stream's bit depth is. */
                decode(device, argv[1], profile->name, fmt.pixelformat, V4L2_PIX_FMT_ABGR32);
                if (!imported) {
                    decode(device, argv[1], profile->name, fmt.pixelformat, profile->raw,
                           V4L2_MEMORY_DMABUF);
                    decode(device, argv[1], profile->name, fmt.pixelformat, V4L2_PIX_FMT_ABGR32,
                           V4L2_MEMORY_DMABUF);
                    importGuestPages(device, argv[1], profile->name, fmt.pixelformat, profile->raw);
                    decode(device, argv[1], profile->name, fmt.pixelformat, V4L2_PIX_FMT_ABGR32,
                           V4L2_MEMORY_DMABUF, Import{true, false, Declared::negotiated});
                    imported = true;
                }
                if (profile->raw == V4L2_PIX_FMT_P010)
                    decode(device, argv[1], profile->name, fmt.pixelformat, profile->raw,
                           V4L2_MEMORY_DMABUF, Import{true, false, Declared::negotiated});
                for (const bool buffers : {false, true})
                    provisionalCapture(device, argv[1], profile->name, fmt.pixelformat, profile->raw, buffers);
                ++tested;
                if (avc && profile->value == V4L2_MPEG_VIDEO_H264_PROFILE_HIGH) {
                    decode(device, argv[1], profile->name, fmt.pixelformat, V4L2_PIX_FMT_YUV420M);
                    /* Three planes in one dma-buf, the last two starting off a page boundary. */
                    decode(device, argv[1], profile->name, fmt.pixelformat, V4L2_PIX_FMT_YUV420M,
                           V4L2_MEMORY_DMABUF, Import{true, true, Declared::negotiated});
                }
                // A 10-bit stream is also captured as the 8-bit planes Android asks for.
                if (profile->raw == V4L2_PIX_FMT_P010)
                    decode(device, argv[1], profile->name, fmt.pixelformat, V4L2_PIX_FMT_NV12M);
            }
            if (avc) h264DisplayOrder(device, argv[1]);
            else hevcDisplayOrder(device, argv[1]);
        }
        require(tested > 0, "decoder advertised no supported profiles");
        std::puts("UML_V4L2_DECODE: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what()); return 1;
    }
}
