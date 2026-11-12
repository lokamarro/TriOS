// SPDX-License-Identifier: GPL-2.0-or-later
/* Test support: a vhost-user backend that raises a config change every few
 * milliseconds. Each change is a request on the vhost-user backend channel
 * that waits for the guest's acknowledgement, so a guest that leaves the
 * channel unread, as a suspended one does, holds the next request back until
 * it resumes.
 *
 * The device has no behavior of its own. Bound to an ID no guest driver
 * binds, it makes the guest acknowledge each change and do nothing else with
 * it. Bound to the ID of a driver that reads its configuration when it
 * changes, it also makes the guest read the configuration over the main
 * socket from a worker, which crosses the change request on the backend
 * channel; the server counts the reads it answers. The shape of the device
 * follows what that driver needs to bind and to read: its queue count, the
 * size of its configuration, and the feature that makes it read.
 *
 * It serves no virtqueue: the guest RAM the emulator names in SET_MEM_TABLE
 * is an identity token that only the emulator process resolves
 * (darwin/runtime/bridge.cpp), so a backend in another process cannot read a
 * request in any case. A queue the driver fills at probe, as the console's
 * input queue, is kicked, and the kick finds no request.
 *
 *   uml-vhost-user-churn-server --fd N [--period-ms 50] [--queues 1]
 *       [--config-bytes 4] [--features 0]
 *
 * It prints the number of changes raised so far when it receives SIGUSR1, and
 * exits when the guest closes the connection with a summary line of the totals.
 */

#include "unique_descriptor.hpp"
#include "vhost_user_device.hpp"
#include "vhost_user_server.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <sys/event.h>
#include <unistd.h>

namespace {

std::atomic_bool stopRequested {};
std::atomic<std::uint64_t> changesRaised {};
std::atomic<std::uint64_t> configReadsAnswered {};

void requestStop(int) noexcept
{
    stopRequested.store(true, std::memory_order_release);
}

/* Writes "vhost-user churn mark: <changes raised so far>" with nothing but
 * write(2), so that it is safe in a signal handler. */
void printMark(int) noexcept
{
    static constexpr char prefix[] = "vhost-user churn mark: ";
    char text[sizeof(prefix) + 24];
    std::size_t length = 0;
    for (const char character : prefix) {
        if (character != '\0')
            text[length++] = character;
    }
    char digits[24];
    std::size_t count = 0;
    std::uint64_t value = changesRaised.load(std::memory_order_relaxed);
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count != 0)
        text[length++] = digits[--count];
    text[length++] = '\n';
    (void)::write(STDOUT_FILENO, text, length);
}

/* A device with zero-filled configuration, whose queues the backend never
 * serves. */
class ChurningDevice final : public uml::virtio::VhostUserDevice {
public:
    ChurningDevice(
        std::uint64_t periodMilliseconds,
        std::size_t queues,
        std::size_t configurationBytes,
        std::uint64_t features)
        : features_(features)
        , queues_(queues)
        , configurationBytes_(configurationBytes)
        , timer_(::kqueue())
    {
        if (timer_.get() < 0)
            throw std::runtime_error(
                "kqueue failed: " + std::string(std::strerror(errno)));
        struct kevent timer {};
        EV_SET(&timer, 1, EVFILT_TIMER, EV_ADD | EV_ENABLE, 0,
            static_cast<std::intptr_t>(periodMilliseconds), nullptr);
        if (::kevent(timer_.get(), &timer, 1, nullptr, 0, nullptr) != 0)
            throw std::runtime_error(
                "arming the timer failed: " +
                std::string(std::strerror(errno)));
    }

    [[nodiscard]] const char* name() const noexcept override
    {
        return "Churning vhost-user device";
    }

    [[nodiscard]] std::uint64_t features() const noexcept override
    {
        return features_;
    }

    [[nodiscard]] std::size_t queueCount() const noexcept override
    {
        return queues_;
    }

    [[nodiscard]] std::vector<std::uint8_t> configuration() const override
    {
        return std::vector<std::uint8_t>(configurationBytes_);
    }

    void configurationRead(std::uint32_t, std::uint32_t) noexcept override
    {
        configReadsAnswered.fetch_add(1, std::memory_order_relaxed);
    }

    void writeConfiguration(
        std::uint32_t,
        std::uint32_t,
        std::span<const std::uint8_t>) override
    {
    }

    [[nodiscard]] uml::virtio::VhostUserQueueResult processQueue(
        std::uint32_t,
        std::uint64_t,
        std::span<const std::uint8_t>,
        std::size_t) override
    {
        return {.action = uml::virtio::VhostUserQueueAction::complete};
    }

    void reset() noexcept override
    {
        pending_ = false;
    }

    [[nodiscard]] int eventDescriptor() const noexcept override
    {
        return timer_.get();
    }

    void drainEventSource() override
    {
        std::array<struct kevent, 4> events {};
        const timespec immediately {};
        int count;
        do {
            count = ::kevent(timer_.get(), nullptr, 0, events.data(),
                static_cast<int>(events.size()), &immediately);
        } while (count < 0 && errno == EINTR);
        if (count < 0)
            throw std::runtime_error("draining the timer failed");
        if (count > 0 && !pending_) {
            pending_ = true;
            changesRaised.fetch_add(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] bool configChangePending() const noexcept override
    {
        return pending_;
    }

    void clearConfigChange() noexcept override
    {
        pending_ = false;
    }

private:
    std::uint64_t features_;
    std::size_t queues_;
    std::size_t configurationBytes_;
    uml::gpu::UniqueDescriptor timer_;
    bool pending_ {};
};

[[noreturn]] void usage()
{
    std::fputs(
        "usage: uml-vhost-user-churn-server --fd N [--period-ms MS] "
        "[--queues N] [--config-bytes N] [--features MASK]\n",
        stderr);
    std::exit(2);
}

std::uint64_t number(const char* text)
{
    char* end = nullptr;
    const auto value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0')
        usage();
    return value;
}

} // namespace

int main(int argc, char** argv)
{
    int descriptor = -1;
    std::uint64_t periodMilliseconds = 50;
    std::uint64_t queues = 1;
    std::uint64_t configurationBytes = sizeof(std::uint32_t);
    std::uint64_t features = 0;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option {argv[index]};
        if (index + 1 >= argc)
            usage();
        const char* value = argv[++index];
        if (option == "--fd")
            descriptor = static_cast<int>(number(value));
        else if (option == "--period-ms")
            periodMilliseconds = number(value);
        else if (option == "--queues")
            queues = number(value);
        else if (option == "--config-bytes")
            configurationBytes = number(value);
        else if (option == "--features")
            features = number(value);
        else
            usage();
    }
    if (descriptor < 0 || periodMilliseconds == 0 || queues == 0 ||
        queues > 16 || configurationBytes > 256) {
        usage();
    }

    struct sigaction action {};
    action.sa_handler = requestStop;
    sigemptyset(&action.sa_mask);
    (void)::sigaction(SIGTERM, &action, nullptr);
    (void)::sigaction(SIGINT, &action, nullptr);
    action.sa_handler = printMark;
    (void)::sigaction(SIGUSR1, &action, nullptr);

    try {
        uml::gpu::VhostUserServer server {
            std::make_unique<ChurningDevice>(
                periodMilliseconds,
                static_cast<std::size_t>(queues),
                static_cast<std::size_t>(configurationBytes),
                features)};
        server.runConnected(descriptor, stopRequested);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "uml-vhost-user-churn-server: %s\n",
            error.what());
        return 1;
    }
    std::printf(
        "vhost-user churn: %llu config changes raised, %llu config reads "
        "answered\n",
        static_cast<unsigned long long>(
            changesRaised.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(
            configReadsAnswered.load(std::memory_order_relaxed)));
    return 0;
}
