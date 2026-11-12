// SPDX-License-Identifier: GPL-2.0-or-later

#include "channel.hpp"
#include "connected_socket_channel.hpp"
#include "in_process_service.hpp"
#include "protocol.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"
#include "unique_descriptor.hpp"
#include "unix_socket_channel.hpp"
#include "virtio_console_device.hpp"
#include "wait_until.hpp"
#include "wire_bytes.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <atomic>
#include <thread>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

namespace protocol = uml::console::protocol;

using uml::testing::load;
using uml::testing::require;
using uml::testing::requireThrows;
using uml::testing::waitUntil;

class TestChannel final : public uml::console::ConsoleChannel {
public:
    std::vector<std::uint8_t> read(std::size_t maximumBytes) override
    {
        const auto count = std::min(maximumBytes, input.size());
        std::vector<std::uint8_t> result;
        for (std::size_t index = 0; index < count; ++index) {
            result.push_back(input.front());
            input.pop_front();
        }
        return result;
    }

    void write(std::span<const std::uint8_t> bytes) noexcept override
    {
        output.insert(output.end(), bytes.begin(), bytes.end());
    }

    int eventDescriptor() const noexcept override { return descriptor; }
    void drainEventSource() override { ++drainCount; }
    void reset() noexcept override
    {
        ++resetCount;
        input.clear();
        output.clear();
    }

    std::deque<std::uint8_t> input;
    std::vector<std::uint8_t> output;
    int descriptor {73};
    unsigned drainCount {};
    unsigned resetCount {};
};

struct DeviceFixture final {
    DeviceFixture()
    {
        auto owned = std::make_unique<TestChannel>();
        channel = owned.get();
        device = std::make_unique<uml::console::VirtioConsoleDevice>(
            std::move(owned));
    }

    TestChannel* channel {};
    std::unique_ptr<uml::console::VirtioConsoleDevice> device;
};

struct NamedDeviceFixture final {
    NamedDeviceFixture()
    {
        std::array<int, 2> pipeDescriptors {-1, -1};
        require(::pipe(pipeDescriptors.data()) == 0);
        channelEvent = uml::gpu::UniqueDescriptor {pipeDescriptors[0]};
        channelEventWriter = uml::gpu::UniqueDescriptor {pipeDescriptors[1]};
        auto owned = std::make_unique<TestChannel>();
        channel = owned.get();
        channel->descriptor = channelEvent.get();
        device = std::make_unique<uml::console::VirtioConsoleDevice>(
            std::move(owned), std::string(protocol::hostSerialPortName));
    }

    TestChannel* channel {};
    uml::gpu::UniqueDescriptor channelEvent;
    uml::gpu::UniqueDescriptor channelEventWriter;
    std::unique_ptr<uml::console::VirtioConsoleDevice> device;
};

/* Socket paths must fit sun_path, so the directory is in the host temporary
 * directory. */
uml::testing::ScratchSubdirectory socketDirectory()
{
    return uml::testing::ScratchSubdirectory {
        "uml-console-test.XXXXXX", uml::testing::socket_scratch_parent()};
}

void waitReadable(int descriptor)
{
    pollfd readiness {
        .fd = descriptor,
        .events = POLLIN,
        .revents = 0,
    };
    require(::poll(&readiness, 1, 1000) == 1);
}

uml::gpu::UniqueDescriptor connectClient(
    const std::filesystem::path& path)
{
    uml::gpu::UniqueDescriptor client {::socket(AF_UNIX, SOCK_STREAM, 0)};
    require(client.get() >= 0);
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    require(path.string().size() < sizeof(address.sun_path));
    std::memcpy(
        address.sun_path, path.c_str(), path.string().size() + 1U);
    require(::connect(
                client.get(),
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == 0);
    return client;
}

std::vector<std::uint8_t> controlMessage(
    std::uint32_t id,
    protocol::ControlEvent event,
    std::uint16_t value)
{
    const protocol::Control control {
        .id = id,
        .event = static_cast<std::uint16_t>(event),
        .value = value,
    };
    return uml::testing::bytes(control);
}

void deviceImplementsTheTwoQueueConsoleContract()
{
    DeviceFixture fixture;
    require(fixture.device->features() == 0);
    require(fixture.device->queueCount() == protocol::queueCount);
    require(fixture.device->configuration().size() ==
            sizeof(protocol::Configuration));

    const auto empty = fixture.device->processQueue(
        protocol::inputQueue, 1, {}, 16);
    require(empty.action == uml::virtio::VhostUserQueueAction::retry);

    constexpr std::array<std::uint8_t, 5> input {'l', 'o', 'g', '\n', 'x'};
    fixture.channel->input.insert(
        fixture.channel->input.end(), input.begin(), input.end());
    const auto received = fixture.device->processQueue(
        protocol::inputQueue, 2, {}, 4);
    require(received.action == uml::virtio::VhostUserQueueAction::complete);
    require(received.response ==
            std::vector<std::uint8_t>(input.begin(), input.begin() + 4));

    constexpr std::array<std::uint8_t, 4> output {'o', 'k', '\r', '\n'};
    const auto sent = fixture.device->processQueue(
        protocol::outputQueue, 3, output, 0);
    require(sent.action == uml::virtio::VhostUserQueueAction::complete);
    require(sent.response.empty());
    require(fixture.channel->output ==
            std::vector<std::uint8_t>(output.begin(), output.end()));

    require(fixture.device->eventDescriptor() == fixture.channel->descriptor);
    fixture.device->drainEventSource();
    require(fixture.channel->drainCount == 1);
    fixture.device->reset();
    require(fixture.channel->resetCount == 1);

    requireThrows([&] {
        (void)fixture.device->processQueue(
            protocol::inputQueue, 4, output, 4);
    });
    requireThrows([&] {
        (void)fixture.device->processQueue(
            protocol::outputQueue, 5, output, 1);
    });
    requireThrows([&] {
        (void)fixture.device->processQueue(2, 6, {}, 1);
    });
    requireThrows([&] {
        fixture.device->writeConfiguration(0, 0, output);
    });
}

void deviceImplementsTheNamedSerialControlHandshake()
{
    NamedDeviceFixture fixture;
    require(fixture.device->features() == protocol::featureMultiport);
    require(fixture.device->queueCount() == protocol::namedSerialQueueCount);
    const auto configuration = uml::testing::loadExact<protocol::Configuration>(
        fixture.device->configuration());
    require(configuration.maximumPorts == 1);

    const auto empty = fixture.device->processQueue(
        protocol::controlInputQueue, 1, {}, 512);
    require(empty.action == uml::virtio::VhostUserQueueAction::retry);

    const auto ready = controlMessage(
        protocol::badPortId, protocol::ControlEvent::deviceReady, 1);
    const auto readyResult = fixture.device->processQueue(
        protocol::controlOutputQueue, 2, ready, 0);
    require(
        readyResult.action == uml::virtio::VhostUserQueueAction::complete);
    waitReadable(fixture.device->eventDescriptor());
    fixture.device->drainEventSource();

    requireThrows([&] {
        (void)fixture.device->processQueue(
            protocol::controlInputQueue, 3, {}, sizeof(protocol::Control) - 1U);
    });
    const auto add = fixture.device->processQueue(
        protocol::controlInputQueue, 4, {}, 512);
    require(add.action == uml::virtio::VhostUserQueueAction::complete);
    const auto addControl = load<protocol::Control>(add.response);
    require(addControl.id == protocol::hostSerialPortId);
    require(addControl.event ==
            static_cast<std::uint16_t>(protocol::ControlEvent::portAdd));
    require(addControl.value == 1);

    const auto portReady = controlMessage(
        protocol::hostSerialPortId, protocol::ControlEvent::portReady, 1);
    const auto portReadyResult = fixture.device->processQueue(
        protocol::controlOutputQueue, 5, portReady, 0);
    require(portReadyResult.action ==
            uml::virtio::VhostUserQueueAction::complete);
    waitReadable(fixture.device->eventDescriptor());
    fixture.device->drainEventSource();

    const auto name = fixture.device->processQueue(
        protocol::controlInputQueue, 6, {}, 512);
    require(name.action == uml::virtio::VhostUserQueueAction::complete);
    const auto nameControl = load<protocol::Control>(name.response);
    require(nameControl.id == protocol::hostSerialPortId);
    require(nameControl.event ==
            static_cast<std::uint16_t>(protocol::ControlEvent::portName));
    const std::string_view receivedName {
        reinterpret_cast<const char*>(
            name.response.data() + sizeof(protocol::Control)),
        name.response.size() - sizeof(protocol::Control)};
    require(receivedName == protocol::hostSerialPortName);

    const auto open = fixture.device->processQueue(
        protocol::controlInputQueue, 7, {}, 512);
    require(open.action == uml::virtio::VhostUserQueueAction::complete);
    const auto openControl = load<protocol::Control>(open.response);
    require(openControl.id == protocol::hostSerialPortId);
    require(openControl.event ==
            static_cast<std::uint16_t>(protocol::ControlEvent::portOpen));
    require(openControl.value == 1);

    const auto guestOpen = controlMessage(
        protocol::hostSerialPortId, protocol::ControlEvent::portOpen, 1);
    require(fixture.device
                ->processQueue(
                    protocol::controlOutputQueue, 7, guestOpen, 0)
                .action == uml::virtio::VhostUserQueueAction::complete);

    requireThrows([&] {
        const auto invalid = controlMessage(
            protocol::hostSerialPortId, protocol::ControlEvent::portName, 1);
        (void)fixture.device->processQueue(
            protocol::controlOutputQueue, 9, invalid, 0);
    });

    fixture.device->reset();
    require(fixture.channel->resetCount == 1);
    require(fixture.device
                ->processQueue(protocol::controlInputQueue, 10, {}, 512)
                .action == uml::virtio::VhostUserQueueAction::retry);

    auto invalidChannel = std::make_unique<TestChannel>();
    requireThrows([&] {
        uml::console::VirtioConsoleDevice invalid(
            std::move(invalidChannel), "invalid/port");
    });
}

void unixSocketChannelSupportsTailInputAndReconnect()
{
    const auto temporary = socketDirectory();
    const auto path = temporary.path() / "console.sock";
    {
        uml::console::UnixSocketChannel channel(path);
        struct stat status {};
        require(::lstat(path.c_str(), &status) == 0);
        require(S_ISSOCK(status.st_mode));
        require((status.st_mode & 0777) == 0600);

        constexpr std::array<std::uint8_t, 5> boot {'b', 'o', 'o', 't', '\n'};
        channel.write(boot);
        auto client = connectClient(path);
        waitReadable(channel.eventDescriptor());
        channel.drainEventSource();
        waitReadable(client.get());
        std::array<std::uint8_t, 16> received {};
        require(::recv(client.get(), received.data(), received.size(), 0) ==
                static_cast<ssize_t>(boot.size()));
        require(std::equal(boot.begin(), boot.end(), received.begin()));

        constexpr std::array<std::uint8_t, 7> command {
            'l', 'o', 'g', 'c', 'a', 't', '\n'};
        require(::send(client.get(), command.data(), command.size(), 0) ==
                static_cast<ssize_t>(command.size()));
        waitReadable(channel.eventDescriptor());
        channel.drainEventSource();
        require(channel.read(4) ==
                std::vector<std::uint8_t>(command.begin(), command.begin() + 4));
        require(channel.read(16) ==
                std::vector<std::uint8_t>(command.begin() + 4, command.end()));

        constexpr std::array<std::uint8_t, 3> reply {'o', 'k', '\n'};
        channel.write(reply);
        waitReadable(client.get());
        require(::recv(client.get(), received.data(), received.size(), 0) ==
                static_cast<ssize_t>(reply.size()));
        require(std::equal(reply.begin(), reply.end(), received.begin()));

        client.reset();
        waitReadable(channel.eventDescriptor());
        channel.drainEventSource();
        auto reconnected = connectClient(path);
        waitReadable(channel.eventDescriptor());
        channel.drainEventSource();
        channel.write(reply);
        waitReadable(reconnected.get());
        require(::recv(
                    reconnected.get(), received.data(), received.size(), 0) ==
                static_cast<ssize_t>(reply.size()));
    }
    struct stat removed {};
    require(::lstat(path.c_str(), &removed) != 0 && errno == ENOENT);
}

/*
 * A named serial port watches its listener while no client is attached, and
 * the client once one is. A guest write that finds the client gone detaches
 * it, and the port must go back to watching the listener then: the next
 * client has to wake the device, and so does that client's input once it is
 * accepted, usually under the departed client's descriptor number.
 */
void namedSerialPortFollowsAClientThatLeftDuringAWrite()
{
    const auto temporary = socketDirectory();
    const auto path = temporary.path() / "serial.sock";
    {
        uml::console::VirtioConsoleDevice device(
            std::make_unique<uml::console::UnixSocketChannel>(path),
            std::string(protocol::hostSerialPortName));
        auto first = connectClient(path);
        waitReadable(device.eventDescriptor());
        device.drainEventSource();
        first.reset();

        constexpr std::array<std::uint8_t, 4> output {'o', 'u', 't', '\n'};
        (void)device.processQueue(protocol::outputQueue, 1, output, 0);

        auto second = connectClient(path);
        waitReadable(device.eventDescriptor());
        device.drainEventSource();
        constexpr std::array<std::uint8_t, 3> input {'i', 'n', '\n'};
        require(::send(second.get(), input.data(), input.size(), 0) ==
                static_cast<ssize_t>(input.size()));
        waitReadable(device.eventDescriptor());
        device.drainEventSource();
        const auto received =
            device.processQueue(protocol::inputQueue, 2, {}, 64);
        require(received.response ==
                std::vector<std::uint8_t>(input.begin(), input.end()));
    }
    struct stat removed {};
    require(::lstat(path.c_str(), &removed) != 0 && errno == ENOENT);
}

/*
 * The harness accepts the artifact socket only when it first observes mode
 * 0600.  Creating the file under the process umask and narrowing it after
 * listen() exposed a wider mode for the bind-to-chmod window, which aborted
 * guest launches on a loaded host.
 */
void unixSocketChannelNeverExposesAWiderModeWhileListening()
{
    const auto temporary = socketDirectory();
    const auto path = temporary.path() / "console.sock";
    const mode_t previous = ::umask(022);
    ::umask(previous);

    std::atomic<bool> started {false};
    std::atomic<bool> stop {false};
    std::atomic<int> widerObservations {0};
    std::atomic<int> observations {0};
    std::thread watcher([&] {
        started.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_relaxed)) {
            struct stat status {};
            if (::lstat(path.c_str(), &status) == 0) {
                observations.fetch_add(1, std::memory_order_relaxed);
                if ((status.st_mode & 0777) != 0600) {
                    widerObservations.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    });
    while (!started.load(std::memory_order_acquire))
        std::this_thread::yield();
    {
        uml::console::UnixSocketChannel channel(path);
        struct stat status {};
        require(::lstat(path.c_str(), &status) == 0);
        require((status.st_mode & 0777) == 0600);
        // A loaded host can leave the watcher unscheduled for the whole
        // lifetime of the channel. Keep the socket until it has been seen.
        require(waitUntil([&] { return observations.load() != 0; },
                    std::chrono::seconds(10), std::chrono::seconds(0)),
            "the watcher never saw the socket");
        stop.store(true, std::memory_order_relaxed);
        watcher.join();
    }
    require(observations.load() > 0);
    require(widerObservations.load() == 0);
    const mode_t restored = ::umask(previous);
    ::umask(previous);
    require(restored == previous);
}

void unixSocketChannelRejectsAClientThatClosedBeforeAccept()
{
    const auto temporary = socketDirectory();
    const auto path = temporary.path() / "console.sock";
    uml::console::UnixSocketChannel channel(path);

    /*
     * Darwin rejects F_SETNOSIGPIPE with EINVAL on a socket whose peer already
     * closed, which a client that connects and exits immediately produces
     * routinely.  Treating that as a channel error escapes into the vhost-user
     * connection loop and takes the console device down for the rest of the
     * run, so the accept has to drop just that client.
     */
    connectClient(path).reset();
    waitReadable(channel.eventDescriptor());
    channel.drainEventSource();

    auto client = connectClient(path);
    waitReadable(channel.eventDescriptor());
    channel.drainEventSource();
    constexpr std::array<std::uint8_t, 3> reply {'o', 'k', '\n'};
    channel.write(reply);
    waitReadable(client.get());
    std::array<std::uint8_t, 16> received {};
    require(::recv(client.get(), received.data(), received.size(), 0) ==
            static_cast<ssize_t>(reply.size()));
    require(std::equal(reply.begin(), reply.end(), received.begin()));
}

void connectedSocketChannelIsNonblocking()
{
    std::array<int, 2> descriptors {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) == 0);
    uml::gpu::UniqueDescriptor peer {descriptors[1]};
    uml::console::ConnectedSocketChannel channel(descriptors[0]);

    const int flags = ::fcntl(channel.eventDescriptor(), F_GETFL);
    require(flags >= 0 && (flags & O_NONBLOCK) != 0);
    require(channel.read(16).empty());

    constexpr std::array<std::uint8_t, 5> input {'i', 'n', 'p', 'u', 't'};
    require(::send(peer.get(), input.data(), input.size(), 0) ==
            static_cast<ssize_t>(input.size()));
    require(channel.read(input.size()) ==
            std::vector<std::uint8_t>(input.begin(), input.end()));

    constexpr std::array<std::uint8_t, 6> output {'o', 'u', 't', 'p', 'u', 't'};
    channel.write(output);
    waitReadable(peer.get());
    std::array<std::uint8_t, 16> received {};
    require(::recv(peer.get(), received.data(), received.size(), 0) ==
            static_cast<ssize_t>(output.size()));
    require(std::equal(output.begin(), output.end(), received.begin()));
}

/* The device sizes a read by the guest's writable descriptors, which a guest
 * can make as large as its memory. The channel's own buffer stays bounded. */
void connectedSocketChannelBoundsItsReadBuffer()
{
    constexpr std::size_t bound = 64U * 1024U;
    std::array<int, 2> descriptors {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) == 0);
    uml::gpu::UniqueDescriptor peer {descriptors[1]};
    uml::console::ConnectedSocketChannel channel(descriptors[0]);

    const int buffer = 4 * static_cast<int>(bound);
    (void)::setsockopt(peer.get(), SOL_SOCKET, SO_SNDBUF, &buffer,
        sizeof(buffer));
    (void)::setsockopt(channel.eventDescriptor(), SOL_SOCKET, SO_RCVBUF,
        &buffer, sizeof(buffer));
    const int flags = ::fcntl(peer.get(), F_GETFL);
    require(flags >= 0 && ::fcntl(peer.get(), F_SETFL, flags | O_NONBLOCK) == 0);
    const std::vector<std::uint8_t> input(2U * bound, 'i');
    const ssize_t sent = ::send(peer.get(), input.data(), input.size(), 0);
    require(sent > 0);

    const auto received = channel.read(std::size_t {16} << 20U);
    require(!received.empty());
    require(received.size() <= std::min(bound, static_cast<std::size_t>(sent)));
    require(received.capacity() <= bound);
}

/*
 * A relay that has not read earlier output leaves the channel's descriptor
 * full. The channel keeps what it could not send, the device leaves later
 * guest output queued meanwhile, and the room the relay makes wakes the
 * device to send it: every byte arrives once and in order.
 */
void connectedSerialPortKeepsOutputTheRelayHasNoRoomFor()
{
    std::array<int, 2> descriptors {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) == 0);
    uml::gpu::UniqueDescriptor relay {descriptors[1]};
    uml::console::VirtioConsoleDevice device(
        std::make_unique<uml::console::ConnectedSocketChannel>(descriptors[0]),
        std::string(protocol::hostSerialPortName));

    std::vector<std::uint8_t> first(1U << 20U);
    for (std::size_t index = 0; index < first.size(); ++index)
        first[index] = static_cast<std::uint8_t>(index * 131U + 7U);
    const std::vector<std::uint8_t> second {'n', 'e', 'x', 't', '\n'};
    require(device.processQueue(protocol::outputQueue, 1, first, 0).action ==
            uml::virtio::VhostUserQueueAction::complete);
    require(device.processQueue(protocol::outputQueue, 2, second, 0).action ==
            uml::virtio::VhostUserQueueAction::retry);

    std::vector<std::uint8_t> received;
    const auto takeWhatTheRelayHas = [&] {
        for (;;) {
            std::array<std::uint8_t, 4096> buffer {};
            const auto count = ::recv(
                relay.get(), buffer.data(), buffer.size(), MSG_DONTWAIT);
            if (count <= 0)
                return;
            received.insert(
                received.end(), buffer.begin(), buffer.begin() + count);
        }
    };
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        require(std::chrono::steady_clock::now() < deadline);
        takeWhatTheRelayHas();
        if (received.size() >= first.size())
            break;
        // The room the relay just made is what has to wake the device.
        waitReadable(device.eventDescriptor());
        device.drainEventSource();
    }
    require(device.processQueue(protocol::outputQueue, 2, second, 0).action ==
            uml::virtio::VhostUserQueueAction::complete);
    waitReadable(relay.get());
    takeWhatTheRelayHas();
    std::vector<std::uint8_t> expected(first);
    expected.insert(expected.end(), second.begin(), second.end());
    require(received == expected);
}

void inProcessServicePublishesAndRemovesTheExternalSocket()
{
    const auto temporary = socketDirectory();
    const auto path = temporary.path() / "service.sock";
    const auto service = uml::console::makeInProcessService(path);
    struct stat status {};
    require(::lstat(path.c_str(), &status) == 0 && S_ISSOCK(status.st_mode));
    const auto connection = service->takeConnectionName();
    require(connection.starts_with("fd="));
    const int descriptor = std::stoi(connection.substr(3));
    require(descriptor >= 0);
    (void)::close(descriptor);
    service->stop();
    require(::lstat(path.c_str(), &status) != 0 && errno == ENOENT);

    const auto serialPath = temporary.path() / "serial.sock";
    const auto serialService = uml::console::makeInProcessService(
        serialPath, std::string(protocol::hostSerialPortName));
    require(
        ::lstat(serialPath.c_str(), &status) == 0 && S_ISSOCK(status.st_mode));
    const auto serialConnection = serialService->takeConnectionName();
    require(serialConnection.starts_with("fd="));
    (void)::close(std::stoi(serialConnection.substr(3)));
    serialService->stop();
    require(::lstat(serialPath.c_str(), &status) != 0 && errno == ENOENT);
}

void inProcessServiceServesAConnectedDescriptor()
{
    std::array<int, 2> descriptors {-1, -1};
    require(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors.data()) == 0);
    uml::gpu::UniqueDescriptor local {descriptors[0]};
    uml::gpu::UniqueDescriptor peer {descriptors[1]};
    // Only a named serial port watches the descriptor for room to resume.
    requireThrows([&] {
        (void)uml::console::makeInProcessService(local.get(), std::string {});
    });
    requireThrows([] {
        (void)uml::console::makeInProcessService(
            -1, std::string(protocol::hostSerialPortName));
    });
    const auto service = uml::console::makeInProcessService(
        local.get(), std::string(protocol::hostSerialPortName));
    // The service serves a duplicate; the caller keeps its own descriptor.
    require(::fcntl(local.get(), F_GETFD) >= 0);
    const auto connection = service->takeConnectionName();
    require(connection.starts_with("fd="));
    (void)::close(std::stoi(connection.substr(3)));
    service->stop();
    require(::fcntl(local.get(), F_GETFD) >= 0);
}

} // namespace

int main()
{
    deviceImplementsTheTwoQueueConsoleContract();
    deviceImplementsTheNamedSerialControlHandshake();
    unixSocketChannelSupportsTailInputAndReconnect();
    namedSerialPortFollowsAClientThatLeftDuringAWrite();
    unixSocketChannelNeverExposesAWiderModeWhileListening();
    unixSocketChannelRejectsAClientThatClosedBeforeAccept();
    connectedSocketChannelIsNonblocking();
    connectedSocketChannelBoundsItsReadBuffer();
    connectedSerialPortKeepsOutputTheRelayHasNoRoomFor();
    inProcessServicePublishesAndRemovesTheExternalSocket();
    inProcessServiceServesAConnectedDescriptor();
    return EXIT_SUCCESS;
}
