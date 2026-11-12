// SPDX-License-Identifier: GPL-2.0-or-later

#include "vhost_user_message.hpp"

#include "platform/descriptor.hpp"

#ifndef UML_GPU_STOP_WAIT_TESTING
#define UML_GPU_STOP_WAIT_TESTING 0
#endif
#if UML_GPU_STOP_WAIT_TESTING
#include "vhost_user_message_testing.hpp"
#include <chrono>
#include <condition_variable>
#include <mutex>
#endif

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

namespace uml::gpu {
#if UML_GPU_STOP_WAIT_TESTING
namespace testing {
namespace {

std::mutex stopWaitMutex;
std::condition_variable stopWaitChanged;
std::uint64_t stopWaitCount;

void notifyStopWait() noexcept
{
    {
        std::lock_guard lock(stopWaitMutex);
        ++stopWaitCount;
    }
    stopWaitChanged.notify_all();
}

} // namespace

void resetStopWaitObserver() noexcept
{
    std::lock_guard lock(stopWaitMutex);
    stopWaitCount = 0;
}

bool waitForStopWaitCount(
    std::uint64_t count,
    std::uint32_t timeoutMilliseconds) noexcept
{
    std::unique_lock lock(stopWaitMutex);
    return stopWaitChanged.wait_for(
        lock,
        std::chrono::milliseconds {timeoutMilliseconds},
        [count] { return stopWaitCount >= count; });
}

} // namespace testing
#endif
namespace {

#ifndef UML_GPU_STOP_POLL_TIMEOUT_MILLISECONDS
#define UML_GPU_STOP_POLL_TIMEOUT_MILLISECONDS 100
#endif
constexpr int stopPollTimeoutMilliseconds =
    UML_GPU_STOP_POLL_TIMEOUT_MILLISECONDS;
static_assert(stopPollTimeoutMilliseconds > 0);
// Receive more than the protocol accepts so malformed over-limit bundles are
// still delivered to user space and can be closed deterministically. Darwin
// can install a trailing descriptor before reporting MSG_CTRUNC when the
// control buffer ends exactly at the accepted limit.
constexpr std::size_t maximumControlDescriptors =
    maximumVhostUserDescriptors * 4U;

void adoptReceivedDescriptors(
    msghdr& header,
    ReceivedDescriptors& destination);

/* A recvmsg on a Unix socket that cannot install the descriptors its message
 * carries fails with one of these, and with none of them for a message that
 * carries no descriptors. Darwin answers EMSGSIZE when the process's file
 * table holds no free slot at all and EMFILE when a slot exists above the
 * ceiling but allocating it would cross that ceiling, which is the same
 * exhaustion seen from a table that grew before the ceiling was lowered.
 * ENFILE is the machine's table rather than this process's.
 */
[[nodiscard]] bool descriptorTableIsFull(int error) noexcept
{
    return error == EMSGSIZE || error == EMFILE || error == ENFILE;
}

/* Say what that means, because "Message too long" reads as a protocol fault
 * and sends the reader after a defect that is not there.
 */
[[nodiscard]] std::string describeDescriptorRefusal()
{
    return "vhost-user read failed: the host file table is full, so the "
        "descriptors this message carries could not be installed (" +
        describeDescriptorCeiling() + ")";
}

[[nodiscard]] bool waitForInput(
    int descriptor,
    const std::atomic_bool* stopRequested)
{
    for (;;) {
        if (stopRequested != nullptr &&
            stopRequested->load(std::memory_order_acquire)) {
            return false;
        }
        pollfd input {
            .fd = descriptor,
            .events = POLLIN,
            .revents = 0,
        };
#if UML_GPU_STOP_WAIT_TESTING
        // Signal only when no byte is immediately readable. A notification
        // therefore proves that every fragment currently queued on the stream
        // has been consumed and the next production-shaped poll will block.
        pollfd readinessProbe = input;
        if (::poll(&readinessProbe, 1, 0) == 0)
            testing::notifyStopWait();
#endif
        const int result = ::poll(
            &input,
            1,
            stopRequested == nullptr ? -1 : stopPollTimeoutMilliseconds);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error(
                "vhost-user poll failed: " +
                std::string(std::strerror(errno)));
        }
        if (result == 0)
            continue;
        if ((input.revents & POLLNVAL) != 0)
            throw std::runtime_error("invalid vhost-user descriptor");
        return true;
    }
}

bool receiveExact(
    int descriptor,
    void* destination,
    std::size_t length,
    ReceivedDescriptors& descriptors,
    const std::atomic_bool* stopRequested)
{
    auto* bytes = static_cast<std::uint8_t*>(destination);
    while (length != 0) {
        if (!waitForInput(descriptor, stopRequested))
            return false;
        alignas(cmsghdr) std::array<
            std::uint8_t,
            CMSG_SPACE(sizeof(int) * maximumControlDescriptors)> control {};
        iovec vector {
            .iov_base = bytes,
            .iov_len = length,
        };
        msghdr header {
            .msg_name = nullptr,
            .msg_namelen = 0,
            .msg_iov = &vector,
            .msg_iovlen = 1,
            .msg_control = control.data(),
            .msg_controllen = control.size(),
            .msg_flags = 0,
        };
        const auto count = ::recvmsg(descriptor, &header, MSG_DONTWAIT);
        if (count == 0)
            return false;
        if (count < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            if (descriptorTableIsFull(errno))
                throw std::runtime_error(describeDescriptorRefusal());
            throw std::runtime_error(
                "vhost-user read failed: " +
                std::string(std::strerror(errno)));
        }
        adoptReceivedDescriptors(header, descriptors);
        bytes += count;
        length -= static_cast<std::size_t>(count);
    }
    return true;
}

void adoptReceivedDescriptors(
    msghdr& header,
    ReceivedDescriptors& destination)
{
    bool malformed = (header.msg_flags & MSG_CTRUNC) != 0;
    int descriptorError = 0;
    for (cmsghdr* control = CMSG_FIRSTHDR(&header);
         control != nullptr;
         control = CMSG_NXTHDR(&header, control)) {
        if (control->cmsg_len < CMSG_LEN(0)) {
            malformed = true;
            continue;
        }
        if (control->cmsg_level != SOL_SOCKET ||
            control->cmsg_type != SCM_RIGHTS) {
            continue;
        }

        const std::size_t payloadSize =
            control->cmsg_len - CMSG_LEN(0);
        if (payloadSize % sizeof(int) != 0)
            malformed = true;
        const std::size_t descriptorCount = payloadSize / sizeof(int);
        const auto* descriptorBytes =
            reinterpret_cast<const std::uint8_t*>(CMSG_DATA(control));
        for (std::size_t index = 0; index < descriptorCount; ++index) {
            int rawDescriptor = -1;
            std::memcpy(
                &rawDescriptor,
                descriptorBytes + index * sizeof(rawDescriptor),
                sizeof(rawDescriptor));
            UniqueDescriptor owned(rawDescriptor);
            if (!uml::platform::configureDescriptor(
                    rawDescriptor, {.closeOnExec = true}) &&
                descriptorError == 0) {
                descriptorError = errno;
            }
            if (!destination.adopt(std::move(owned)))
                malformed = true;
        }
    }

    if (descriptorError != 0) {
        throw std::runtime_error(
            "configuring received vhost-user descriptor failed: " +
            std::string(std::strerror(descriptorError)));
    }
    if (malformed)
        throw std::runtime_error("truncated vhost-user descriptor list");
}

} // namespace

namespace {

/* The kernel's per-process descriptor cap, or zero when the host will not say.
 * A soft limit above it buys nothing and hides the number that decides.
 */
[[nodiscard]] std::uint64_t perProcessDescriptorCap() noexcept
{
    int cap = 0;
    std::size_t size = sizeof(cap);
    if (::sysctlbyname("kern.maxfilesperproc", &cap, &size, nullptr, 0) != 0)
        return 0;
    return cap > 0 ? static_cast<std::uint64_t>(cap) : 0;
}

} // namespace

DescriptorCeiling descriptorCeiling() noexcept
{
    rlimit limit {};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0)
        return {};
    return {
        .soft = static_cast<std::uint64_t>(limit.rlim_cur),
        .hard = static_cast<std::uint64_t>(limit.rlim_max),
        .perProcess = perProcessDescriptorCap(),
    };
}

std::string describeDescriptorCeiling()
{
    const auto ceiling = descriptorCeiling();
    const auto name = [](std::uint64_t value) {
        if (value == 0)
            return std::string("unknown");
        return value == static_cast<std::uint64_t>(RLIM_INFINITY)
            ? std::string("unlimited")
            : std::to_string(value);
    };
    return "open-descriptor ceiling " + std::to_string(ceiling.effective()) +
        " (soft " + name(ceiling.soft) + ", hard " + name(ceiling.hard) +
        ", per-process " + name(ceiling.perProcess) + ")";
}

std::uint64_t raiseDescriptorCeiling() noexcept
{
    rlimit limit {};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0)
        return 0;

    /* A process never gets more descriptors than kern.maxfilesperproc,
     * whatever its hard ceiling says, and a hard ceiling is usually unlimited.
     * Ask for the smaller of the two so the ceiling this reports is the number
     * that actually decides, not one no open() will ever reach.
     */
    rlim_t wanted = limit.rlim_max;
    const auto cap = perProcessDescriptorCap();
    if (cap != 0 && static_cast<rlim_t>(cap) < wanted)
        wanted = static_cast<rlim_t>(cap);
    if (limit.rlim_cur >= wanted)
        return static_cast<std::uint64_t>(limit.rlim_cur);

    rlimit raised = limit;
    raised.rlim_cur = wanted;
    if (::setrlimit(RLIMIT_NOFILE, &raised) == 0)
        return static_cast<std::uint64_t>(wanted);

    /* A host may refuse the whole of what it advertises while granting part of
     * it. Keep the largest accepted value; the loop halves the untried range
     * on every step, so it ends after at most one step per bit of the ceiling.
     */
    rlim_t accepted = limit.rlim_cur;
    rlim_t refused = wanted;
    while (refused - accepted > 1) {
        const rlim_t candidate = accepted + (refused - accepted) / 2;
        raised.rlim_cur = candidate;
        if (::setrlimit(RLIMIT_NOFILE, &raised) == 0)
            accepted = candidate;
        else
            refused = candidate;
    }
    return static_cast<std::uint64_t>(accepted);
}

bool receiveVhostUserMessage(
    int socket,
    VhostUserMessage& message,
    const std::atomic_bool* stopRequested)
{
    message = VhostUserMessage {};
    // Every stream fragment uses recvmsg rather than falling back to recv.
    // SCM_RIGHTS may accompany any byte in a fragmented frame, and the kernel
    // installs those descriptors as soon as that fragment is consumed.
    if (!receiveExact(
            socket,
            &message.header,
            sizeof(message.header),
            message.descriptors,
            stopRequested)) {
        return false;
    }
    if ((message.header.flags & protocol::vhostUserVersionMask) !=
        protocol::vhostUserVersion) {
        throw std::runtime_error("unsupported vhost-user protocol version");
    }
    if (message.header.size > maximumVhostUserMessagePayload)
        throw std::runtime_error("oversized vhost-user message");

    message.payload.resize(message.header.size);
    return receiveExact(
        socket,
        message.payload.data(),
        message.payload.size(),
        message.descriptors,
        stopRequested);
}

} // namespace uml::gpu
