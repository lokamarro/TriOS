// SPDX-License-Identifier: GPL-2.0-or-later
//
// Allocates the capture buffers of the Virtio video decoder with
// V4L2_MEMORY_MMAP on an otherwise idle guest and requires the allocation to
// finish within a bound. With a Virtio-GPU device the buffers are host-visible
// blobs, and the driver learns where one lives only from the host's answer to
// a command it queued; a command that is queued and never sent leaves
// VIDIOC_REQBUFS asleep until some other Virtio-GPU traffic happens to flush
// the queue, which on an idle guest is never.
//
// Usage: v4l2_mmap_capture_probe [--require-blob] [DEVICE [BOUND_SECONDS]]
//
// --require-blob also fails when the buffers are not host-visible blobs, so a
// run that was meant to cover the blob path cannot pass by taking another one.
//
// The call that stalls sleeps in the kernel where no signal reaches it, so a
// child process makes the calls and this one only waits for its progress.
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr auto output = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
constexpr auto capture = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
constexpr unsigned bufferCount = 4;

// What the child tells its parent, one line at a time. A write is used rather
// than stdio so that nothing is left in a buffer when the process ends.
int reportFd = -1;

void say(const std::string& line)
{
    const std::string text = line + "\n";
    ssize_t ignored = write(reportFd, text.data(), text.size());
    (void)ignored;
}

[[noreturn]] void fail(const std::string& detail)
{
    say("FAIL " + detail);
    _exit(1);
}

double secondsSince(std::chrono::steady_clock::time_point from)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count();
}

int control(int fd, unsigned long request, void* value)
{
    int result;
    do { result = ioctl(fd, request, value); } while (result < 0 && errno == EINTR);
    return result;
}

// True when a mapping of the process carries VM_IO, which a window onto host
// memory does and a mapping of guest pages does not.
bool mappingIsIo(const void* address)
{
    std::ifstream smaps("/proc/self/smaps");
    std::string line;
    const auto target = reinterpret_cast<std::uintptr_t>(address);
    bool inside = false;
    while (std::getline(smaps, line)) {
        std::uintptr_t low = 0, high = 0;
        if (std::sscanf(line.c_str(), "%lx-%lx ", &low, &high) == 2) {
            inside = target >= low && target < high;
        } else if (inside && line.rfind("VmFlags:", 0) == 0) {
            return line.find(" io") != std::string::npos;
        }
    }
    return false;
}

// The child: set the decoder up, allocate, map, write to and release the
// capture buffers, naming each step before it starts.
[[noreturn]] void allocate(const char* device, bool requireBlob)
{
    say("PHASE open");
    const int fd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) fail(std::string("open ") + device + ": " + std::strerror(errno));
    v4l2_capability cap {};
    if (control(fd, VIDIOC_QUERYCAP, &cap) < 0) fail("QUERYCAP failed");
    if ((cap.device_caps & (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING)) !=
        (V4L2_CAP_VIDEO_M2M_MPLANE | V4L2_CAP_STREAMING))
        fail("device is not a streaming mem2mem decoder");

    // Any coded format the device offers: the capture queue needs a decoder
    // session, not a stream.
    say("PHASE formats");
    v4l2_fmtdesc coded {};
    coded.type = output;
    if (control(fd, VIDIOC_ENUM_FMT, &coded) < 0) fail("the decoder offers no coded format");
    v4l2_format input {};
    input.type = output;
    input.fmt.pix_mp.width = 128;
    input.fmt.pix_mp.height = 96;
    input.fmt.pix_mp.pixelformat = coded.pixelformat;
    input.fmt.pix_mp.num_planes = 1;
    input.fmt.pix_mp.plane_fmt[0].sizeimage = 1 << 16;
    if (control(fd, VIDIOC_S_FMT, &input) < 0) fail("S_FMT coded failed");
    v4l2_format format {};
    format.type = capture;
    if (control(fd, VIDIOC_G_FMT, &format) < 0) fail("G_FMT capture failed");
    if (control(fd, VIDIOC_S_FMT, &format) < 0) fail("S_FMT capture failed");

    say("PHASE REQBUFS capture MMAP");
    const auto start = std::chrono::steady_clock::now();
    v4l2_requestbuffers request {};
    request.type = capture;
    request.memory = V4L2_MEMORY_MMAP;
    request.count = bufferCount;
    if (control(fd, VIDIOC_REQBUFS, &request) < 0)
        fail(std::string("REQBUFS capture MMAP: ") + std::strerror(errno));
    if (request.count < 1) fail("REQBUFS capture MMAP granted no buffer");
    const double allocationSeconds = secondsSince(start);

    // The mapping of a blob waits for the same host answer, and a client
    // always maps what it allocates.
    unsigned mapped = 0, io = 0;
    const auto mapStart = std::chrono::steady_clock::now();
    for (unsigned index = 0; index < request.count; ++index) {
        v4l2_plane planes[VIDEO_MAX_PLANES] {};
        v4l2_buffer buffer {};
        buffer.type = capture;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        buffer.m.planes = planes;
        buffer.length = format.fmt.pix_mp.num_planes;
        if (control(fd, VIDIOC_QUERYBUF, &buffer) < 0) fail("QUERYBUF capture failed");
        for (unsigned plane = 0; plane < buffer.length; ++plane) {
            say("PHASE mmap capture buffer " + std::to_string(index));
            const std::size_t length = planes[plane].length;
            void* address = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, planes[plane].m.mem_offset);
            if (address == MAP_FAILED) fail(std::string("mmap capture buffer: ") + std::strerror(errno));
            auto* bytes = static_cast<volatile std::uint8_t*>(address);
            bytes[0] = std::uint8_t(0xa5 + index);
            bytes[length - 1] = std::uint8_t(0x5a + index);
            if (bytes[0] != std::uint8_t(0xa5 + index) || bytes[length - 1] != std::uint8_t(0x5a + index))
                fail("a capture buffer does not keep what was written to it");
            ++mapped;
            if (mappingIsIo(address)) ++io;
            munmap(address, length);
        }
    }
    const double mapSeconds = secondsSince(mapStart);
    say("PHASE release");
    request.count = 0;
    if (control(fd, VIDIOC_REQBUFS, &request) < 0) fail("REQBUFS release failed");

    if (requireBlob && io != mapped)
        fail("only " + std::to_string(io) + " of " + std::to_string(mapped) + " capture planes are host-visible blobs");
    char detail[160];
    std::snprintf(detail, sizeof detail, "allocation %.3f s, %u planes mapped in %.3f s, %u of them host blobs",
                  allocationSeconds, mapped, mapSeconds, io);
    say(std::string("PASS ") + detail);
    _exit(0);
}

[[noreturn]] void finish(bool pass, const std::string& detail)
{
    const std::string line = std::string("UML_V4L2_MMAP_CAPTURE: ") + (pass ? "PASS " : "FAIL ") + detail + "\n";
    ssize_t ignored = write(STDOUT_FILENO, line.data(), line.size());
    (void)ignored;
    _exit(pass ? 0 : 1);
}

} // namespace

int main(int argc, char** argv)
{
    bool requireBlob = false;
    std::vector<const char*> positional;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--require-blob")) requireBlob = true;
        else positional.push_back(argv[i]);
    }
    const char* device = positional.size() > 0 ? positional[0] : "/dev/video0";
    const double bound = positional.size() > 1 ? std::atof(positional[1]) : 5.0;

    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC) < 0) finish(false, std::string("pipe: ") + std::strerror(errno));
    const pid_t child = fork();
    if (child < 0) finish(false, std::string("fork: ") + std::strerror(errno));
    if (child == 0) {
        reportFd = pipes[1];
        close(pipes[0]);
        allocate(device, requireBlob);
    }
    close(pipes[1]);

    // Every step has the same bound, counted from the moment the child names
    // it. A step that outlasts it is the stall; the child is left behind,
    // asleep in the kernel, for the guest to deal with.
    std::string pending, phase = "start", result;
    auto phaseStart = std::chrono::steady_clock::now();
    for (;;) {
        const double left = bound - secondsSince(phaseStart);
        if (left <= 0)
            finish(false, "\"" + phase + "\" still blocked after " + std::to_string(secondsSince(phaseStart)) +
                              " s (bound " + std::to_string(bound) + " s)");
        pollfd watched {pipes[0], POLLIN, 0};
        const int ready = poll(&watched, 1, int(left * 1000) + 1);
        if (ready < 0 && errno != EINTR) finish(false, std::string("poll: ") + std::strerror(errno));
        if (ready <= 0) continue;
        char chunk[256];
        const ssize_t count = read(pipes[0], chunk, sizeof chunk);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        pending.append(chunk, std::size_t(count));
        for (std::size_t newline; (newline = pending.find('\n')) != std::string::npos;) {
            const std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (line.rfind("PHASE ", 0) == 0) {
                phase = line.substr(6);
                phaseStart = std::chrono::steady_clock::now();
            } else {
                result = line;
            }
        }
    }
    int status = 0;
    waitpid(child, &status, 0);
    if (result.rfind("PASS ", 0) == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0)
        finish(true, result.substr(5));
    finish(false, result.empty() ? "the child ended without a result" : result.substr(result.find(' ') + 1));
}
