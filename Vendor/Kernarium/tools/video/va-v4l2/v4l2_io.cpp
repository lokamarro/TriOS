// SPDX-License-Identifier: GPL-2.0-or-later
#include "v4l2_io.hpp"

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>

namespace uml::va {
namespace {

class SystemIo final : public V4l2Io {
public:
    int open(const char* path) override { return ::open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC); }
    int close(int fd) override { return ::close(fd); }
    int ioctl(int fd, unsigned long request, void* argument) override
    {
        int result;
        do { result = ::ioctl(fd, request, argument); } while (result < 0 && errno == EINTR);
        return result;
    }
    void* mmap(int fd, std::size_t length, std::int64_t offset) override
    {
        return ::mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off_t(offset));
    }
    int munmap(void* address, std::size_t length) override { return ::munmap(address, length); }
    int poll(pollfd* fds, unsigned long count, int timeoutMilliseconds) override
    {
        return ::poll(fds, nfds_t(count), timeoutMilliseconds);
    }
};

}

V4l2Io& systemIo() noexcept
{
    static SystemIo io;
    return io;
}

}
