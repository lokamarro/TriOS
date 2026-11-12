// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

struct pollfd;

namespace uml::va {

// The system calls a V4L2 session makes, behind one seam so that the encoder
// and the descriptor pool can be exercised against a device written in memory.
// Production code always runs on systemIo(), which is the plain system calls.
class V4l2Io {
public:
    virtual ~V4l2Io() = default;

    // Opens a device node read-write, non-blocking and close-on-exec.
    virtual int open(const char* path) = 0;
    virtual int close(int fd) = 0;
    // Retries an interrupted call.
    virtual int ioctl(int fd, unsigned long request, void* argument) = 0;
    // MAP_FAILED when the mapping cannot be made.
    virtual void* mmap(int fd, std::size_t length, std::int64_t offset) = 0;
    virtual int munmap(void* address, std::size_t length) = 0;
    virtual int poll(pollfd* fds, unsigned long count, int timeoutMilliseconds) = 0;
};

V4l2Io& systemIo() noexcept;

}
