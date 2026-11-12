// SPDX-License-Identifier: GPL-2.0-or-later

#include "KernariumConsoleInput.hpp"
#include "require.hpp"
#include "wait_until.hpp"
#include "watchdog.hpp"

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

using uml::testing::require;
using uml::testing::Watchdog;

struct Console final {
    Console()
    {
        int descriptors[2] {-1, -1};
        require(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0,
            "socketpair");
        guest = descriptors[0];
        host = descriptors[1];
        const int enabled = 1;
        require(::setsockopt(host, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
            sizeof(enabled)) == 0, "SO_NOSIGPIPE");
    }

    ~Console()
    {
        if (guest >= 0)
            ::close(guest);
    }

    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    int guest {-1};
    int host {-1};
};

std::vector<std::uint8_t> pattern(std::size_t size)
{
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t index = 0; index < size; ++index)
        bytes[index] = static_cast<std::uint8_t>(index * 131U + 7U);
    return bytes;
}

/* Waits until the guest end holds queued input that nothing reads, which is
 * what a writer blocks behind. */
void waitForQueuedInput(int guest)
{
    require(uml::testing::waitUntil([guest] {
        int queued = 0;
        require(::ioctl(guest, FIONREAD, &queued) == 0, "FIONREAD");
        return queued > 0;
    }, 10s), "the writer never reached the socket");
}

/* Input larger than the socket holds arrives whole and in order once the
 * guest reads, however long it takes to start. */
void aWriteWaitsForTheGuestToRead()
{
    Console console;
    kernarium::console::HostEnd input(console.host);
    const auto bytes = pattern(1U << 20U);
    int result = -1;
    std::thread writer([&] { result = input.send(bytes.data(), bytes.size()); });
    waitForQueuedInput(console.guest);
    std::vector<std::uint8_t> received;
    {
        Watchdog watchdog("reading written console input", 30s);
        std::uint8_t buffer[4096];
        while (received.size() < bytes.size()) {
            const ssize_t count = ::recv(console.guest, buffer, sizeof(buffer), 0);
            require(count > 0, "receiving console input");
            received.insert(received.end(), buffer, buffer + count);
        }
        writer.join();
    }
    require(result == 0, "the write did not complete");
    require(received == bytes, "console input arrived damaged");
}

/*
 * The runtime retires its console input when the guest's run ends. A write
 * the application's terminal made into a guest that stopped reading must not
 * hold that up: retiring ends the write, and neither side waits for the
 * guest again.
 */
void retiringEndsAWriteToAGuestThatStoppedReading()
{
    Console console;
    kernarium::console::HostEnd input(console.host);
    kernarium::console::InputRegistry registry;
    registry.publish(input);
    const auto bytes = pattern(1U << 20U);
    std::atomic<int> result {-1};
    std::thread writer([&] {
        result.store(registry.send(bytes.data(), bytes.size()),
            std::memory_order_release);
    });
    waitForQueuedInput(console.guest);
    std::this_thread::sleep_for(20ms);
    require(result.load(std::memory_order_acquire) == -1,
        "a write larger than the socket finished with nothing reading it");
    {
        Watchdog watchdog("retiring the console input", 10s);
        registry.retire();
        writer.join();
    }
    require(result.load(std::memory_order_acquire) == ECANCELED,
        "the retired write did not end as cancelled");
    require(registry.send(bytes.data(), 1U) == ENOTCONN,
        "a retired registry still accepted input");
    require(input.send(bytes.data(), 1U) == ECANCELED,
        "a stopped input still accepted input");
}

/* The relay closes the input when the guest end goes away; a write after that
 * reports the closed console rather than touching a reused descriptor. */
void aClosedInputRefusesWrites()
{
    Console console;
    kernarium::console::HostEnd input(console.host);
    const std::uint8_t byte = 1U;
    require(input.send(&byte, 1U) == 0, "writing to an open console");
    input.close();
    require(input.send(&byte, 1U) == ENOTCONN,
        "a closed input still accepted input");
    require(input.send(&byte, 0U) == 0, "an empty write failed");
    require(input.send(nullptr, 1U) == EINVAL, "a null write was accepted");
}

} // namespace

int main()
{
    aWriteWaitsForTheGuestToRead();
    retiringEndsAWriteToAGuestThatStoppedReading();
    aClosedInputRefusesWrites();
    std::puts("PASS: console input waits for the guest and ends when retired");
    return EXIT_SUCCESS;
}
