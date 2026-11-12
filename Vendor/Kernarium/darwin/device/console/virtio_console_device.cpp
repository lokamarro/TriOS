// SPDX-License-Identifier: GPL-2.0-or-later

#include "virtio_console_device.hpp"

#include "protocol.hpp"
#include "unique_descriptor.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

namespace uml::console {
namespace {

constexpr std::uintptr_t controlNotificationIdentifier = 1;

bool validPortNameCharacter(char character)
{
    return (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') || character == '.' ||
        character == '_' || character == '-';
}

} // namespace

class VirtioConsoleDevice::EventMultiplexer final {
public:
    explicit EventMultiplexer(ConsoleChannel& channel)
        : channel_(channel)
        , descriptor_(::kqueue())
    {
        if (descriptor_.get() < 0)
            throw std::runtime_error("creating Virtio serial event queue failed");
        struct kevent userEvent {};
        EV_SET(
            &userEvent,
            controlNotificationIdentifier,
            EVFILT_USER,
            EV_ADD | EV_CLEAR,
            0,
            0,
            nullptr);
        if (::kevent(descriptor_.get(), &userEvent, 1, nullptr, 0, nullptr) != 0)
            throw std::runtime_error("registering Virtio serial control event failed");
        updateChannelDescriptor();
    }

    [[nodiscard]] int descriptor() const noexcept
    {
        return descriptor_.get();
    }

    void notify()
    {
        struct kevent notification {};
        EV_SET(
            &notification,
            controlNotificationIdentifier,
            EVFILT_USER,
            0,
            NOTE_TRIGGER,
            0,
            nullptr);
        if (::kevent(
                descriptor_.get(), &notification, 1, nullptr, 0, nullptr) != 0) {
            throw std::runtime_error("notifying Virtio serial control queue failed");
        }
    }

    void drain()
    {
        constexpr timespec noWait {};
        std::array<struct kevent, 8> events {};
        for (;;) {
            const int count = ::kevent(
                descriptor_.get(),
                nullptr,
                0,
                events.data(),
                static_cast<int>(events.size()),
                &noWait);
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                throw std::runtime_error("draining Virtio serial events failed");
            }
            if (count < static_cast<int>(events.size()))
                break;
        }
        // A channel drain is nonblocking even when only the internal control
        // notification fired. It also lets us replace the listener registration
        // with the accepted client, or return to the listener after disconnect.
        channel_.drainEventSource();
        updateChannelDescriptor();
    }

    // A write can detach the channel's client too, when it finds the client
    // gone. Its descriptor is closed then, which drops its registration, so
    // the listener has to be watched again before anything else happens:
    // otherwise the next client never wakes the device, and once one is
    // accepted under the same descriptor number it is never registered.
    void followChannel()
    {
        updateChannelDescriptor();
    }

private:
    void updateChannelDescriptor()
    {
        const int next = channel_.eventDescriptor();
        if (next < 0)
            throw std::runtime_error("Virtio serial channel has no event descriptor");
        if (next != channelDescriptor_) {
            if (channelDescriptor_ >= 0) {
                remove(channelDescriptor_, EVFILT_READ);
                if (watchingForRoom_)
                    remove(channelDescriptor_, EVFILT_WRITE);
            }
            watchingForRoom_ = false;
            add(next, EVFILT_READ, EV_ADD | EV_CLEAR);
            channelDescriptor_ = next;
        }
        // Output that waits for the peer is sent once it makes room, which
        // only the descriptor's writability reports.
        const bool waiting = channel_.outputWaiting();
        if (waiting == watchingForRoom_)
            return;
        if (waiting)
            add(next, EVFILT_WRITE, EV_ADD);
        else
            remove(next, EVFILT_WRITE);
        watchingForRoom_ = waiting;
    }

    void add(int descriptor, std::int16_t filter, std::uint16_t flags)
    {
        struct kevent addition {};
        EV_SET(
            &addition,
            static_cast<std::uintptr_t>(descriptor),
            filter,
            flags,
            0,
            0,
            nullptr);
        if (::kevent(descriptor_.get(), &addition, 1, nullptr, 0, nullptr) != 0)
            throw std::runtime_error("registering Virtio serial channel event failed");
    }

    // A descriptor the channel closed has lost its registrations already.
    void remove(int descriptor, std::int16_t filter)
    {
        struct kevent removal {};
        EV_SET(
            &removal,
            static_cast<std::uintptr_t>(descriptor),
            filter,
            EV_DELETE,
            0,
            0,
            nullptr);
        if (::kevent(descriptor_.get(), &removal, 1, nullptr, 0, nullptr) != 0 &&
            errno != ENOENT && errno != EBADF) {
            throw std::runtime_error(
                "removing stale Virtio serial channel event failed");
        }
    }

    ConsoleChannel& channel_;
    uml::gpu::UniqueDescriptor descriptor_;
    int channelDescriptor_ {-1};
    bool watchingForRoom_ {};
};

VirtioConsoleDevice::VirtioConsoleDevice(
    std::unique_ptr<ConsoleChannel> channel)
    : channel_(std::move(channel))
{
    if (!channel_)
        throw std::invalid_argument("Virtio console requires a host channel");
}

VirtioConsoleDevice::VirtioConsoleDevice(
    std::unique_ptr<ConsoleChannel> channel,
    std::string portName)
    : channel_(std::move(channel))
    , portName_(std::move(portName))
{
    if (!channel_)
        throw std::invalid_argument("Virtio serial requires a host channel");
    validateNamedPort();
    events_ = std::make_unique<EventMultiplexer>(*channel_);
}

VirtioConsoleDevice::~VirtioConsoleDevice() = default;

const char* VirtioConsoleDevice::name() const noexcept
{
    return portName_.empty() ? "Virtio console" : "Virtio serial";
}

std::uint64_t VirtioConsoleDevice::features() const noexcept
{
    return portName_.empty() ? 0 : protocol::featureMultiport;
}

std::size_t VirtioConsoleDevice::queueCount() const noexcept
{
    return portName_.empty() ? protocol::queueCount :
                               protocol::namedSerialQueueCount;
}

std::vector<std::uint8_t> VirtioConsoleDevice::configuration() const
{
    return uml::virtio::toBytes(protocol::Configuration {
        .maximumPorts = portName_.empty() ? 0U : 1U,
    });
}

void VirtioConsoleDevice::writeConfiguration(
    std::uint32_t,
    std::uint32_t,
    std::span<const std::uint8_t> bytes)
{
    if (!bytes.empty())
        throw std::runtime_error("Virtio console configuration is read-only");
}

uml::virtio::VhostUserQueueResult VirtioConsoleDevice::processQueue(
    std::uint32_t index,
    std::uint64_t requestId,
    std::span<const std::uint8_t> request,
    std::size_t responseCapacity)
{
    (void)requestId;
    if (index == protocol::inputQueue) {
        if (!request.empty() || responseCapacity == 0) {
            throw std::runtime_error(
                "invalid Virtio console input descriptor");
        }
        auto response = channel_->read(responseCapacity);
        if (response.empty())
            return {};
        return {
            .action = uml::virtio::VhostUserQueueAction::complete,
            .response = std::move(response),
        };
    }
    if (index == protocol::outputQueue) {
        if (responseCapacity != 0) {
            throw std::runtime_error(
                "invalid Virtio console output descriptor");
        }
        // Output the channel's peer has not made room for keeps later output
        // queued, so none is dropped or reordered. The channel's descriptor
        // turning writable wakes the device, which offers it again.
        if (channel_->outputWaiting())
            return {};
        channel_->write(request);
        if (events_)
            events_->followChannel();
        return {
            .action = uml::virtio::VhostUserQueueAction::complete,
            .response = {},
        };
    }
    if (!portName_.empty() && index == protocol::controlInputQueue) {
        if (!request.empty() || responseCapacity == 0) {
            throw std::runtime_error(
                "invalid Virtio serial control input descriptor");
        }
        if (controlMessages_.empty())
            return {};
        if (controlMessages_.front().size() > responseCapacity) {
            throw std::runtime_error(
                "short Virtio serial control input descriptor");
        }
        auto response = std::move(controlMessages_.front());
        controlMessages_.pop_front();
        return {
            .action = uml::virtio::VhostUserQueueAction::complete,
            .response = std::move(response),
        };
    }
    if (!portName_.empty() && index == protocol::controlOutputQueue) {
        if (responseCapacity != 0) {
            throw std::runtime_error(
                "invalid Virtio serial control output descriptor");
        }
        processControl(request);
        return {
            .action = uml::virtio::VhostUserQueueAction::complete,
            .response = {},
        };
    }
    throw std::runtime_error("invalid Virtio console queue index");
}

void VirtioConsoleDevice::reset() noexcept
{
    channel_->reset();
    controlMessages_.clear();
    deviceReady_ = false;
    portReady_ = false;
}

int VirtioConsoleDevice::eventDescriptor() const noexcept
{
    return events_ ? events_->descriptor() : channel_->eventDescriptor();
}

void VirtioConsoleDevice::drainEventSource()
{
    if (events_)
        events_->drain();
    else
        channel_->drainEventSource();
}

void VirtioConsoleDevice::validateNamedPort() const
{
    if (portName_.empty() || portName_.size() > 255U ||
        !std::all_of(
            portName_.begin(), portName_.end(), validPortNameCharacter)) {
        throw std::invalid_argument("invalid Virtio serial port name");
    }
}

void VirtioConsoleDevice::enqueueControl(
    std::uint32_t portId,
    std::uint16_t event,
    std::uint16_t value,
    std::string_view payload)
{
    const protocol::Control control {
        .id = portId,
        .event = event,
        .value = value,
    };
    std::vector<std::uint8_t> message(sizeof(control) + payload.size());
    std::memcpy(message.data(), &control, sizeof(control));
    if (!payload.empty()) {
        std::memcpy(
            message.data() + sizeof(control), payload.data(), payload.size());
    }
    controlMessages_.push_back(std::move(message));
    events_->notify();
}

void VirtioConsoleDevice::processControl(
    std::span<const std::uint8_t> request)
{
    if (request.size() != sizeof(protocol::Control))
        throw std::runtime_error("invalid Virtio serial control message size");
    protocol::Control control {};
    std::memcpy(&control, request.data(), sizeof(control));
    const auto event = static_cast<protocol::ControlEvent>(control.event);
    switch (event) {
    case protocol::ControlEvent::deviceReady:
        if (control.id != protocol::badPortId || control.value > 1U) {
            throw std::runtime_error(
                "invalid Virtio serial device-ready message");
        }
        controlMessages_.clear();
        deviceReady_ = control.value != 0;
        portReady_ = false;
        if (deviceReady_) {
            enqueueControl(
                protocol::hostSerialPortId,
                static_cast<std::uint16_t>(protocol::ControlEvent::portAdd),
                1,
                {});
        }
        return;
    case protocol::ControlEvent::portReady:
        if (!deviceReady_ || control.id != protocol::hostSerialPortId ||
            control.value > 1U) {
            throw std::runtime_error(
                "invalid Virtio serial port-ready message");
        }
        if (control.value == 0 || portReady_)
            return;
        portReady_ = true;
        enqueueControl(
            protocol::hostSerialPortId,
            static_cast<std::uint16_t>(protocol::ControlEvent::portName),
            1,
            portName_);
        enqueueControl(
            protocol::hostSerialPortId,
            static_cast<std::uint16_t>(protocol::ControlEvent::portOpen),
            1,
            {});
        return;
    case protocol::ControlEvent::portOpen:
        if (!portReady_ || control.id != protocol::hostSerialPortId ||
            control.value > 1U) {
            throw std::runtime_error(
                "invalid Virtio serial port-open message");
        }
        return;
    case protocol::ControlEvent::portAdd:
    case protocol::ControlEvent::portRemove:
    case protocol::ControlEvent::consolePort:
    case protocol::ControlEvent::resize:
    case protocol::ControlEvent::portName:
        throw std::runtime_error("invalid driver-to-device Virtio serial event");
    }
    throw std::runtime_error("unknown Virtio serial control event");
}

} // namespace uml::console
