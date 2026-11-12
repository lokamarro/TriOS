// SPDX-License-Identifier: GPL-2.0-or-later

#include "in_process_service.hpp"

#include "connected_socket_channel.hpp"
#include "unique_descriptor.hpp"
#include "unix_socket_channel.hpp"
#include "vhost_user_server.hpp"
#include "virtio_console_device.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <stdexcept>
#include <utility>

namespace uml::console {
namespace {

using ChannelFactory = std::function<std::unique_ptr<ConsoleChannel>()>;

/* The channel is created on the service thread, with the device around it. */
std::unique_ptr<uml::gpu::InProcessVhostUserService> startService(
    ChannelFactory makeChannel,
    std::string portName)
{
    return std::make_unique<uml::gpu::InProcessVhostUserService>(
        "Virtio console", "uml-console",
        [makeChannel = std::move(makeChannel),
         portName = std::move(portName)]()
            -> std::unique_ptr<uml::virtio::VhostUserDevice> {
            auto channel = makeChannel();
            if (portName.empty())
                return std::make_unique<VirtioConsoleDevice>(std::move(channel));
            return std::make_unique<VirtioConsoleDevice>(
                std::move(channel), portName);
        });
}

} // namespace

std::unique_ptr<uml::gpu::InProcessVhostUserService> makeInProcessService(
    std::filesystem::path socketPath,
    std::string portName)
{
    return startService(
        [socketPath = std::move(socketPath)]()
            -> std::unique_ptr<ConsoleChannel> {
            return std::make_unique<UnixSocketChannel>(socketPath);
        },
        std::move(portName));
}

std::unique_ptr<uml::gpu::InProcessVhostUserService> makeInProcessService(
    int connectedDescriptor,
    std::string portName)
{
    /* Output the relay has no room for waits in the channel, and only a
     * named serial port watches the descriptor for the room that resumes
     * it. */
    if (portName.empty()) {
        throw std::invalid_argument(
            "a connected Virtio console needs a named serial port");
    }
    /* The caller keeps its descriptor; the service owns a duplicate from
     * here on, even when it fails to start. */
    auto connection = std::make_shared<uml::gpu::UniqueDescriptor>(
        ::fcntl(connectedDescriptor, F_DUPFD_CLOEXEC, 0));
    if (connection->get() < 0) {
        throw std::runtime_error(
            "duplicating the connected Virtio console descriptor failed: " +
            std::string(std::strerror(errno)));
    }
    return startService(
        [connection]() -> std::unique_ptr<ConsoleChannel> {
            return std::make_unique<ConnectedSocketChannel>(
                connection->release());
        },
        std::move(portName));
}

} // namespace uml::console
