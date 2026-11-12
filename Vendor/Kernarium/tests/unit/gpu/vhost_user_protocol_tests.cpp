// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu_test_support.hpp"
#include "gpu_vhost_user_server.hpp"
#include "wait_until.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <mutex>
#include <optional>
#include <poll.h>
#include <thread>

namespace uml::gpu::testing {
namespace {

template<typename Operation>
void expectRejected(VhostUserServer& server, Operation operation)
{
    withServerConnection(server, [&](int socket) {
        operation(socket);
        waitForConnectionClose(socket);
    });
}

class DeferredQueueDevice final : public FixtureDevice {
public:
    DeferredQueueDevice() : FixtureDevice("deferred queue fixture", 4, true) {}

    [[nodiscard]] uml::virtio::VhostUserQueueResult processQueue(
        std::uint32_t index,
        std::uint64_t requestId,
        std::span<const std::uint8_t> request,
        std::size_t responseCapacity) override
    {
        require(index == 3);
        require(std::ranges::equal(
            request, std::array<std::uint8_t, 4> {1, 2, 3, 4}));
        require(responseCapacity == 4);
        requestId_.store(requestId, std::memory_order_release);
        return {.action = uml::virtio::VhostUserQueueAction::defer};
    }
    void reset() noexcept override
    {
        requestId_.store(0, std::memory_order_release);
        std::lock_guard lock(completionMutex_);
        completion_.reset();
    }
    [[nodiscard]] bool takeQueueCompletion(
        uml::virtio::VhostUserQueueCompletion& completion) override
    {
        std::lock_guard lock(completionMutex_);
        if (!completion_)
            return false;
        completion = std::move(*completion_);
        completion_.reset();
        return true;
    }

    void complete()
    {
        const auto requestId = requestId_.load(std::memory_order_acquire);
        require(requestId != 0);
        {
            std::lock_guard lock(completionMutex_);
            completion_ = uml::virtio::VhostUserQueueCompletion {
                .requestId = requestId,
                .response = {9, 8, 7, 6},
            };
        }
        signalReadiness();
    }

private:
    std::atomic<std::uint64_t> requestId_ {};
    std::mutex completionMutex_;
    std::optional<uml::virtio::VhostUserQueueCompletion> completion_;
};

class MultiDeferredQueueDevice final : public FixtureDevice {
public:
    MultiDeferredQueueDevice()
        : FixtureDevice("multi-deferred queue fixture", 1, true)
    {
    }

    [[nodiscard]] uml::virtio::VhostUserQueueResult processQueue(
        std::uint32_t index,
        std::uint64_t requestId,
        std::span<const std::uint8_t> request,
        std::size_t responseCapacity) override
    {
        require(index == 0);
        require(request.size() == 1);
        require(responseCapacity == 1);
        std::lock_guard lock(mutex_);
        requestIds_.push_back(requestId);
        requestValues_.push_back(request.front());
        return {.action = uml::virtio::VhostUserQueueAction::defer};
    }
    void reset() noexcept override
    {
        std::lock_guard lock(mutex_);
        requestIds_.clear();
        requestValues_.clear();
        completions_.clear();
    }
    [[nodiscard]] bool takeQueueCompletion(
        uml::virtio::VhostUserQueueCompletion& completion) override
    {
        std::lock_guard lock(mutex_);
        if (completions_.empty())
            return false;
        completion = std::move(completions_.front());
        completions_.pop_front();
        return true;
    }

    void complete(std::span<const std::size_t> order)
    {
        {
            std::lock_guard lock(mutex_);
            require(requestIds_.size() == 3);
            require(requestValues_.size() == 3);
            require(std::ranges::equal(
                requestValues_, std::array<std::uint8_t, 3> {1, 2, 3}));
            for (const auto index : order) {
                require(index < requestIds_.size());
                completions_.push_back(
                    uml::virtio::VhostUserQueueCompletion {
                        .requestId = requestIds_[index],
                        .response = {
                            static_cast<std::uint8_t>(0xa0U + index)},
                    });
            }
        }
        signalReadiness();
    }

private:
    std::mutex mutex_;
    std::vector<std::uint64_t> requestIds_;
    std::vector<std::uint8_t> requestValues_;
    std::deque<uml::virtio::VhostUserQueueCompletion> completions_;
};

class CompleteThenRetryDevice final : public FixtureDevice {
public:
    CompleteThenRetryDevice() : FixtureDevice("complete-then-retry fixture", 1)
    {
    }

    [[nodiscard]] uml::virtio::VhostUserQueueResult processQueue(
        std::uint32_t index,
        std::uint64_t,
        std::span<const std::uint8_t> request,
        std::size_t responseCapacity) override
    {
        require(index == 0);
        require(request.empty());
        require(responseCapacity == 4);
        const auto attempt = attempts_.fetch_add(1, std::memory_order_relaxed);
        if (attempt == 0) {
            return {
                .action = uml::virtio::VhostUserQueueAction::complete,
                .response = {9, 8, 7, 6},
            };
        }
        return {.action = uml::virtio::VhostUserQueueAction::retry};
    }
    void reset() noexcept override
    {
        attempts_.store(0, std::memory_order_relaxed);
    }
    [[nodiscard]] unsigned attempts() const noexcept
    {
        return attempts_.load(std::memory_order_relaxed);
    }

private:
    std::atomic_uint attempts_ {};
};

class ConfigChangeDevice final : public FixtureDevice {
public:
    ConfigChangeDevice() : FixtureDevice("config change fixture", 1, true) {}

    [[nodiscard]] std::vector<std::uint8_t> configuration() const override
    {
        protocol::GpuConfig config {
            .eventsRead = 0,
            .eventsClear = 0,
            .scanoutCount = 1,
            .capsetCount = 0,
        };
        const auto generation =
            generation_.load(std::memory_order_acquire);
        if (generation !=
            clearedGeneration_.load(std::memory_order_acquire)) {
            config.eventsRead = protocol::gpuEventDisplay;
        }
        const auto* first = reinterpret_cast<const std::uint8_t*>(&config);
        return {first, first + sizeof(config)};
    }
    void configurationRead(
        std::uint32_t offset,
        std::uint32_t size) noexcept override
    {
        constexpr std::uint32_t eventsReadOffset =
            offsetof(protocol::GpuConfig, eventsRead);
        constexpr std::uint32_t eventsReadEnd =
            eventsReadOffset + sizeof(protocol::GpuConfig::eventsRead);
        const auto readEnd =
            static_cast<std::uint64_t>(offset) + size;
        if (offset <= eventsReadOffset && readEnd >= eventsReadEnd) {
            reportedGeneration_.store(
                generation_.load(std::memory_order_acquire),
                std::memory_order_release);
        }
    }
    void writeConfiguration(
        std::uint32_t offset,
        std::uint32_t flags,
        std::span<const std::uint8_t> bytes) override
    {
        require(offset == offsetof(protocol::GpuConfig, eventsClear));
        require(flags == protocol::configFlagWritable);
        require(bytes.size() == sizeof(std::uint32_t));
        std::uint32_t events {};
        std::memcpy(&events, bytes.data(), sizeof(events));
        if ((events & protocol::gpuEventDisplay) == 0)
            return;
        clearedGeneration_.store(
            reportedGeneration_.load(std::memory_order_acquire),
            std::memory_order_release);
        if (generation_.load(std::memory_order_acquire) !=
            clearedGeneration_.load(std::memory_order_acquire)) {
            pending_.store(true, std::memory_order_release);
        }
    }
    [[nodiscard]] uml::virtio::VhostUserQueueResult processQueue(
        std::uint32_t,
        std::uint64_t,
        std::span<const std::uint8_t>,
        std::size_t) override
    {
        return {};
    }
    void reset() noexcept override
    {
        pending_.store(false, std::memory_order_release);
        generation_.store(0, std::memory_order_release);
        reportedGeneration_.store(0, std::memory_order_release);
        clearedGeneration_.store(0, std::memory_order_release);
    }
    void drainEventSource() override
    {
        FixtureDevice::drainEventSource();
        pending_.store(true, std::memory_order_release);
    }
    [[nodiscard]] bool configChangePending() const noexcept override
    {
        return pending_.load(std::memory_order_acquire);
    }
    void clearConfigChange() noexcept override
    {
        pending_.store(false, std::memory_order_release);
    }

    void trigger()
    {
        generation_.fetch_add(1, std::memory_order_acq_rel);
        signalReadiness();
    }

    [[nodiscard]] bool pending() const noexcept
    {
        return pending_.load(std::memory_order_acquire);
    }

private:
    std::atomic_bool pending_ {};
    std::atomic_uint64_t generation_ {};
    mutable std::atomic_uint64_t reportedGeneration_ {};
    std::atomic_uint64_t clearedGeneration_ {};
};

void configChangeWaitsForTheBackendChannelAndRequiresAcknowledgement()
{
    auto device = std::make_unique<ConfigChangeDevice>();
    auto* source = device.get();
    VhostUserServer server {std::move(device)};
    withServerConnection(server, [&](int socket) {
        constexpr std::uint64_t features =
            protocol::protocolFeatureReplyAck |
            protocol::protocolFeatureSlaveRequest |
            protocol::protocolFeatureConfig;
        negotiateProtocolFeatures(socket, features);

        /* Publish before SET_SLAVE_REQ_FD to prove the transport retains the
         * notification until it has a channel on which to deliver it. */
        source->trigger();
        const auto backend = attachBackendChannel(socket);
        const int channel = backend.descriptors[0].get();

        const auto notification = readBackendRequest(
            channel, protocol::VhostSlaveRequest::configChange);
        require(source->pending());

        const auto waitUntilDelivered = [&](
            const std::source_location location =
                std::source_location::current()) {
            awaitCondition([&] { return !source->pending(); }, location);
        };
        const auto readGpuConfig = [&] {
            return readConfig(socket, 0, sizeof(protocol::GpuConfig))
                .as<protocol::GpuConfig>();
        };
        const auto clearDisplayEvent = [&] {
            constexpr std::uint32_t events = protocol::gpuEventDisplay;
            const protocol::ConfigHeader header {
                .offset = offsetof(protocol::GpuConfig, eventsClear),
                .size = sizeof(events),
                .flags = protocol::configFlagWritable,
            };
            sendRequestAndReadAcknowledgement(
                socket,
                protocol::VhostRequest::setConfig,
                configPayload(header, bytesOf(events)));
        };
        const auto readScanoutCount = [&] {
            return readConfig(
                       socket,
                       offsetof(protocol::GpuConfig, scanoutCount),
                       sizeof(protocol::GpuConfig::scanoutCount))
                .as<std::uint32_t>();
        };

        replyToBackend(channel, notification);
        waitUntilDelivered();
        require(readGpuConfig().eventsRead == protocol::gpuEventDisplay);

        /* A second resize reaches the IRQ path while the first config work is
         * still represented by its old events_read snapshot. Its interrupt
         * may coalesce, so clearing that snapshot must enqueue another
         * backend notification after SET_CONFIG has been acknowledged. */
        source->trigger();
        const auto coalesced = readBackendRequest(
            channel, protocol::VhostSlaveRequest::configChange);
        replyToBackend(channel, coalesced);
        waitUntilDelivered();
        require(readScanoutCount() == 1);
        clearDisplayEvent();
        const auto rearmed = readBackendRequest(
            channel, protocol::VhostSlaveRequest::configChange);
        replyToBackend(channel, rearmed);
        waitUntilDelivered();

        require(readGpuConfig().eventsRead == protocol::gpuEventDisplay);
        clearDisplayEvent();
        pollfd unexpected {.fd = channel, .events = POLLIN};
        require(::poll(&unexpected, 1, 100) == 0);
    });
}

/* A UML front-end reads a GET_CONFIG reply from the main socket without
 * draining the backend channel, and the guest CPU that drains it can be the
 * one waiting. A config change still waiting for its acknowledgement must
 * therefore not stop the server from answering; a request that is not a
 * query waits for the acknowledgement and keeps its order. */
void configChangeWaitStillAnswersConfigReads()
{
    auto device = std::make_unique<ConfigChangeDevice>();
    auto* source = device.get();
    VhostUserServer server {std::move(device)};
    withServerConnection(server, [&](int socket) {
        constexpr std::uint64_t features =
            protocol::protocolFeatureReplyAck |
            protocol::protocolFeatureSlaveRequest |
            protocol::protocolFeatureConfig;
        const auto backend = openBackendChannel(socket, features);

        source->trigger();
        const auto notification = readBackendRequest(
            backend.descriptors[0].get(),
            protocol::VhostSlaveRequest::configChange);

        // The acknowledgement is withheld: the server is now waiting.
        const auto config = readConfig(socket, 0, sizeof(protocol::GpuConfig))
                                .as<protocol::GpuConfig>();
        require(config.eventsRead == protocol::gpuEventDisplay);

        // GET_VRING_BASE changes queue state, so it waits for the reply.
        const protocol::VringState ring {.index = 0, .value = 0};
        sendRequest(
            socket,
            protocol::VhostRequest::getVringBase,
            bytesOf(ring),
            {},
            false);
        pollfd early {.fd = socket, .events = POLLIN};
        require(::poll(&early, 1, 200) == 0);
        require(source->pending());

        replyToBackend(backend.descriptors[0].get(), notification);
        const auto base = readValueReply<protocol::VringState>(
            socket, protocol::VhostRequest::getVringBase);
        require(base.index == 0 && base.value == 0);
        awaitCondition([&] { return !source->pending(); });
    });
}

/* Holds what the process writes to stderr, where the server reports a failed
 * connection, until finish(). A failed requirement would vanish into it, so
 * none is checked until it has finished. */
class StandardErrorCapture final {
public:
    StandardErrorCapture()
    {
        require(file_ != nullptr);
        std::fflush(stderr);
        saved_ = UniqueDescriptor {::dup(STDERR_FILENO)};
        require(saved_.get() >= 0);
        require(::dup2(::fileno(file_), STDERR_FILENO) == STDERR_FILENO);
    }
    StandardErrorCapture(const StandardErrorCapture&) = delete;
    StandardErrorCapture& operator=(const StandardErrorCapture&) = delete;
    ~StandardErrorCapture()
    {
        restore();
        if (file_ != nullptr)
            std::fclose(file_);
    }

    /* Restores stderr, copies what was captured to it, and returns that. */
    [[nodiscard]] std::string finish()
    {
        restore();
        std::string text;
        std::rewind(file_);
        std::array<char, 512> buffer {};
        std::size_t count = 0;
        while ((count = std::fread(buffer.data(), 1, buffer.size(), file_)) !=
               0) {
            text.append(buffer.data(), count);
        }
        std::fputs(text.c_str(), stderr);
        return text;
    }

private:
    void restore() noexcept
    {
        if (saved_.get() < 0)
            return;
        std::fflush(stderr);
        (void)::dup2(saved_.get(), STDERR_FILENO);
        saved_.reset();
    }

    std::FILE* file_ {std::tmpfile()};
    UniqueDescriptor saved_;
};

/* A guest that powers off with a configuration change unanswered closes the
 * backend channel, possibly before the main socket, and one that has already
 * closed it fails the server's next request on it with EPIPE. Either way the
 * front-end has let the device go, so the server ends the connection as it
 * does at end of file on the main socket, without reporting a failure. */
void closedBackendChannelEndsTheConnectionQuietly()
{
    for (const bool closedBeforeTheChange : {false, true}) {
        auto device = std::make_unique<ConfigChangeDevice>();
        auto* source = device.get();
        VhostUserServer server {std::move(device)};
        std::optional<StandardErrorCapture> capture;
        bool notified = false;
        bool connectionClosed = false;
        withServerConnection(server, [&](int socket) {
            constexpr std::uint64_t features =
                protocol::protocolFeatureReplyAck |
                protocol::protocolFeatureSlaveRequest |
                protocol::protocolFeatureConfig;
            auto backend = openBackendChannel(socket, features);
            backend.descriptors[1].reset();

            capture.emplace();
            if (closedBeforeTheChange) {
                backend.descriptors[0].reset();
                source->trigger();
                notified = true;
            } else {
                source->trigger();
                pollfd ready {
                    .fd = backend.descriptors[0].get(),
                    .events = POLLIN,
                };
                protocol::VhostHeader notification {};
                notified = ::poll(&ready, 1, 2000) == 1 &&
                    ::recv(
                        backend.descriptors[0].get(),
                        &notification,
                        sizeof(notification),
                        MSG_WAITALL) == sizeof(notification) &&
                    notification.request == static_cast<std::uint32_t>(
                        protocol::VhostSlaveRequest::configChange);
                // Unanswered, as the guest leaves it.
                backend.descriptors[0].reset();
            }
            // The main socket stays open: the backend channel alone ends it.
            pollfd closed {.fd = socket, .events = POLLIN};
            std::uint8_t byte {};
            connectionClosed = ::poll(&closed, 1, 2000) == 1 &&
                ::read(socket, &byte, sizeof(byte)) == 0;
        });
        const auto reported = capture->finish();
        require(notified);
        require(connectionClosed);
        require(
            reported.find("connection failed") == std::string::npos,
            "a front-end that closed the connection was reported as a "
            "failure");
    }
}

class SharedMemoryDevice final : public FixtureDevice {
public:
    explicit SharedMemoryDevice(std::size_t actionCount = 1)
        : FixtureDevice("shared-memory fixture", 1), actionCount_(actionCount)
    {
    }

    [[nodiscard]] uml::virtio::VhostUserQueueResult processQueue(
        std::uint32_t,
        std::uint64_t,
        std::span<const std::uint8_t>,
        std::size_t) override
    {
        return {.action = uml::virtio::VhostUserQueueAction::retry};
    }
    void prepareReset() noexcept override
    {
        if (actionIssued_)
            return;
        actionIssued_ = true;
        try {
            actions_.reserve(actionCount_);
            for (std::size_t index = 0; index < actionCount_; ++index) {
                FILE* file = std::tmpfile();
                if (file == nullptr)
                    return;
                const int descriptor = ::dup(::fileno(file));
                const bool sized = descriptor >= 0 &&
                    ::ftruncate(descriptor, 0x8000) == 0;
                std::fclose(file);
                if (!sized) {
                    if (descriptor >= 0)
                        ::close(descriptor);
                    return;
                }
                actions_.push_back(
                    uml::virtio::VhostUserSharedMemoryAction {
                        .type = uml::virtio::VhostUserSharedMemoryAction::Type::map,
                        .actionId = 77 + index,
                        .regionId = protocol::hostVisibleSharedMemoryRegionId,
                        .descriptorOffset = 0x4000,
                        .regionOffset = 0x8000 + index * 0x4000,
                        .length = 0x4000,
                        .writable = true,
                        .descriptor = UniqueDescriptor {descriptor},
                    });
            }
        } catch (...) {
            actions_.clear();
        }
    }
    [[nodiscard]] std::optional<uml::virtio::VhostUserSharedMemoryRegion>
        sharedMemoryRegion() const noexcept override
    {
        return uml::virtio::VhostUserSharedMemoryRegion {
            .id = protocol::hostVisibleSharedMemoryRegionId,
            .size = protocol::hostVisibleSharedMemoryRegionSize,
        };
    }
    [[nodiscard]] bool takeSharedMemoryAction(
        uml::virtio::VhostUserSharedMemoryAction& action) override
    {
        if (actions_.empty())
            return false;
        action = std::move(actions_.front());
        actions_.erase(actions_.begin());
        return true;
    }
    void completeSharedMemoryAction(
        std::uint64_t actionId,
        bool success) noexcept override
    {
        completedAction_ = actionId;
        ++completedActionCount_;
        actionSucceeded_ = actionSucceeded_ && success;
    }
    [[nodiscard]] std::uint64_t completedAction() const noexcept
    {
        return completedAction_;
    }
    [[nodiscard]] bool actionSucceeded() const noexcept
    {
        return actionSucceeded_;
    }
    [[nodiscard]] std::size_t completedActionCount() const noexcept
    {
        return completedActionCount_;
    }

private:
    std::size_t actionCount_;
    std::vector<uml::virtio::VhostUserSharedMemoryAction> actions_;
    bool actionIssued_ {};
    std::uint64_t completedAction_ {};
    std::size_t completedActionCount_ {};
    bool actionSucceeded_ {true};
};

void sharedMemoryRequiresBackendAcknowledgements()
{
    auto device = std::make_unique<SharedMemoryDevice>();
    VhostUserServer server {std::move(device)};
    expectRejected(server, [](int socket) {
        sendDeviceFeatures(socket);
        constexpr std::uint64_t incompleteFeatures =
            protocol::protocolFeatureSharedMemoryMap |
            protocol::protocolFeatureSlaveRequest;
        sendRequest(
            socket,
            protocol::VhostRequest::setProtocolFeatures,
            bytesOf(incompleteFeatures));
    });
}

void sharedMemoryConfigurationRequiresNegotiation()
{
    auto device = std::make_unique<SharedMemoryDevice>();
    VhostUserServer server {std::move(device)};
    expectRejected(server, [](int socket) {
        sendRequest(
            socket,
            protocol::VhostRequest::getSharedMemoryConfig,
            {},
            {},
            false);
    });
}

void standardizedSharedMemoryNegotiationBatchesBackendRequestsBeforeReply()
{
    auto device = std::make_unique<SharedMemoryDevice>(2);
    auto* observed = device.get();
    VhostUserServer server {std::move(device)};
    withServerConnection(server, [&](int socket) {
        sendRequest(
            socket,
            protocol::VhostRequest::getProtocolFeatures,
            {},
            {},
            false);
        const auto offered = readValueReply<std::uint64_t>(
            socket, protocol::VhostRequest::getProtocolFeatures);
        require((offered & protocol::protocolFeatureSharedMemoryMap) != 0);

        constexpr std::uint64_t protocolFeatures =
            protocol::protocolFeatureReplyAck |
            protocol::protocolFeatureSlaveRequest |
            protocol::protocolFeatureSharedMemoryMap;
        const auto backend = openBackendChannel(socket, protocolFeatures);
        const int backendClient = backend.descriptors[0].get();

        sendRequest(
            socket,
            protocol::VhostRequest::getSharedMemoryConfig,
            {},
            {},
            false);
        const auto config = readValueReply<protocol::SharedMemoryConfig>(
            socket, protocol::VhostRequest::getSharedMemoryConfig);
        require(config.regionCount == 1);
        require(config.regionSizes[0] == 0);
        require(config.regionSizes[protocol::hostVisibleSharedMemoryRegionId] ==
            protocol::hostVisibleSharedMemoryRegionSize);

        sendRequest(
            socket, protocol::VhostRequest::resetOwner, {});
        for (std::size_t index = 0; index < 2; ++index) {
            pollfd prematureReply {.fd = socket, .events = POLLIN};
            require(::poll(&prematureReply, 1, 50) == 0);

            VhostUserMessage backendMessage;
            require(receiveVhostUserMessage(backendClient, backendMessage));
            require(backendMessage.header.request == static_cast<std::uint32_t>(
                protocol::VhostSlaveRequest::sharedMemoryMap));
            require(backendMessage.header.flags ==
                (protocol::vhostUserVersion | protocol::vhostUserNeedReply));
            require(backendMessage.descriptors.size() == 1);
            require(backendMessage.payload.size() ==
                sizeof(protocol::SharedMemoryMap));
            protocol::SharedMemoryMap map {};
            std::memcpy(&map, backendMessage.payload.data(), sizeof(map));
            require(map.regionId == protocol::hostVisibleSharedMemoryRegionId);
            require(map.descriptorOffset == 0x4000);
            require(map.regionOffset == 0x8000 + index * 0x4000);
            require(map.length == 0x4000);
            require(map.flags == protocol::sharedMemoryMapReadWrite);

            replyToBackend(backendClient, backendMessage.header);
        }
        readAcknowledgement(socket, protocol::VhostRequest::resetOwner);
        (void)getFeatures(socket);
        require(observed->completedAction() == 78);
        require(observed->completedActionCount() == 2);
        require(observed->actionSucceeded());
    });
}

void rejectedSharedMemoryActionClosesBackendChannel()
{
    auto device = std::make_unique<SharedMemoryDevice>();
    auto* observed = device.get();
    VhostUserServer server {std::move(device)};
    withServerConnection(server, [&](int socket) {
        constexpr std::uint64_t protocolFeatures =
            protocol::protocolFeatureReplyAck |
            protocol::protocolFeatureSlaveRequest |
            protocol::protocolFeatureSharedMemoryMap;
        auto backend = openBackendChannel(socket, protocolFeatures);
        backend.descriptors[1].reset();
        const int backendClient = backend.descriptors[0].get();

        sendRequest(socket, protocol::VhostRequest::resetOwner, {});
        VhostUserMessage backendMessage;
        require(receiveVhostUserMessage(backendClient, backendMessage));
        require(backendMessage.descriptors.size() == 1);

        replyToBackend(backendClient, backendMessage.header, 1);
        waitForConnectionClose(backendClient);
        require(observed->completedAction() == 77);
        require(!observed->actionSucceeded());
    });
}

void deferredCompletionSupportsFourQueues()
{
    auto device = std::make_unique<DeferredQueueDevice>();
    auto* completionSource = device.get();
    VhostUserServer server {std::move(device)};
    const SplitRing ring;
    const auto memory = ring.memory();

    constexpr std::uint32_t queueIndex = 3;
    constexpr std::size_t requestOffset = 0x300;
    constexpr std::size_t responseOffset = 0x400;
    const auto descriptors =
        requestChain(requestOffset, 4, responseOffset, 4);
    const std::array<std::uint8_t, 4> request {1, 2, 3, 4};
    std::memcpy(memory.data() + requestOffset, request.data(), request.size());
    std::memset(memory.data() + responseOffset, 0xa5, 4);
    constexpr std::array<std::uint16_t, 1> heads {0};
    ring.publish(descriptors, heads);

    withServerConnection(server, [&](int socket) {
        sendRequest(socket, protocol::VhostRequest::getQueueNum, {}, {}, false);
        require(readValueReply<std::uint64_t>(
                    socket, protocol::VhostRequest::getQueueNum) == 4);
        negotiateProtocolFeatures(socket, protocol::protocolFeatureReplyAck);
        configureMemory(socket, ring.descriptor());
        const auto channels = startQueue(socket, ring, queueIndex);
        (void)getFeatures(socket);

        require(ring.usedIndex() == 0);
        completionSource->complete();
        awaitCall(channels.call.readEnd.get());
        require(ring.usedIndex() == 1);
        require(std::ranges::equal(
            memory.subspan(responseOffset, 4),
            std::array<std::uint8_t, 4> {9, 8, 7, 6}));
    });
}

void multipleDeferredCompletionsRetainTheirDescriptorChains()
{
    auto device = std::make_unique<MultiDeferredQueueDevice>();
    auto* completionSource = device.get();
    VhostUserServer server {std::move(device)};
    const SplitRing ring;
    const auto memory = ring.memory();

    constexpr std::array<std::size_t, 3> requestOffsets {
        0x300, 0x310, 0x320};
    constexpr std::array<std::size_t, 3> responseOffsets {
        0x400, 0x410, 0x420};
    std::array<protocol::VirtqueueDescriptor, 6> descriptors {};
    std::array<std::uint16_t, 3> heads {};
    for (std::size_t index = 0; index < requestOffsets.size(); ++index) {
        const auto requestDescriptor = static_cast<std::uint16_t>(index * 2U);
        heads[index] = requestDescriptor;
        const auto chain = requestChain(
            requestOffsets[index], 1, responseOffsets[index], 1,
            requestDescriptor);
        descriptors[requestDescriptor] = chain[0];
        descriptors[requestDescriptor + 1U] = chain[1];
        memory[requestOffsets[index]] = static_cast<std::uint8_t>(index + 1U);
        memory[responseOffsets[index]] = 0x5a;
    }
    ring.publish(descriptors, heads);

    withServerConnection(server, [&](int socket) {
        negotiateProtocolFeatures(socket, protocol::protocolFeatureReplyAck);
        configureMemory(socket, ring.descriptor());
        const auto channels = startQueue(socket, ring);

        require(ring.usedIndex() == 0);
        constexpr std::array<std::size_t, 3> completionOrder {2, 0, 1};
        completionSource->complete(completionOrder);
        awaitCall(channels.call.readEnd.get());
        require(ring.usedIndex() == 3);

        for (std::size_t used = 0; used < completionOrder.size(); ++used) {
            const auto element = ring.usedElement(used);
            require(element.id == heads[completionOrder[used]]);
            require(element.length == 1);
        }
        for (std::size_t index = 0; index < responseOffsets.size(); ++index) {
            require(memory[responseOffsets[index]] ==
                static_cast<std::uint8_t>(0xa0U + index));
        }
    });
}

void completedPrefixNotifiesBeforeRetry()
{
    auto device = std::make_unique<CompleteThenRetryDevice>();
    auto* observed = device.get();
    VhostUserServer server {std::move(device)};
    const SplitRing ring;
    const auto memory = ring.memory();

    constexpr std::size_t firstResponseOffset = 0x300;
    constexpr std::size_t secondResponseOffset = 0x400;
    const std::array descriptors {
        protocol::VirtqueueDescriptor {
            .address = SplitRing::guestBase + firstResponseOffset,
            .length = 4,
            .flags = protocol::descriptorWrite,
            .next = 0,
        },
        protocol::VirtqueueDescriptor {
            .address = SplitRing::guestBase + secondResponseOffset,
            .length = 4,
            .flags = protocol::descriptorWrite,
            .next = 0,
        },
    };
    constexpr std::array<std::uint16_t, 2> heads {0, 1};
    ring.publish(descriptors, heads);
    constexpr std::uint8_t responseSentinel = 0xa5;
    std::memset(memory.data() + firstResponseOffset, responseSentinel, 4);
    std::memset(memory.data() + secondResponseOffset, responseSentinel, 4);

    withServerConnection(server, [&](int socket) {
        negotiateProtocolFeatures(socket, protocol::protocolFeatureReplyAck);
        configureMemory(socket, ring.descriptor());
        const auto channels = startQueue(socket, ring);

        awaitCall(channels.call.readEnd.get());
        require(ring.usedIndex() == 1);
        require(observed->attempts() == 2);
        require(std::ranges::equal(
            memory.subspan(firstResponseOffset, 4),
            std::array<std::uint8_t, 4> {9, 8, 7, 6}));
        require(std::ranges::all_of(
            memory.subspan(secondResponseOffset, 4),
            [](std::uint8_t byte) { return byte == responseSentinel; }));
    });
}

/* Holds events for its one queue, like virtio-input's event queue, and
 * delivers each into a buffer the front end only lets it fill. */
class EventSourceDevice final : public FixtureDevice {
public:
    EventSourceDevice() : FixtureDevice("event source fixture", 1, true) {}

    void raise()
    {
        pending_.fetch_add(1, std::memory_order_relaxed);
        signalReadiness();
    }
    [[nodiscard]] uml::virtio::VhostUserQueueResult processQueue(
        std::uint32_t index,
        std::uint64_t,
        std::span<const std::uint8_t> request,
        std::size_t responseCapacity) override
    {
        require(index == 0);
        require(request.empty());
        require(responseCapacity == event.size());
        if (pending_.load(std::memory_order_relaxed) == 0)
            return {.action = uml::virtio::VhostUserQueueAction::retry};
        pending_.fetch_sub(1, std::memory_order_relaxed);
        delivered_.fetch_add(1, std::memory_order_relaxed);
        return {
            .action = uml::virtio::VhostUserQueueAction::complete,
            .response = {event.begin(), event.end()},
        };
    }
    void drainEventSource() override
    {
        FixtureDevice::drainEventSource();
        drained_.fetch_add(1, std::memory_order_release);
    }
    [[nodiscard]] unsigned delivered() const noexcept
    {
        return delivered_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] unsigned drained() const noexcept
    {
        return drained_.load(std::memory_order_acquire);
    }

    static constexpr std::array<std::uint8_t, 8> event {1, 0, 116, 0, 1, 0, 0, 0};

private:
    std::atomic_uint pending_ {};
    std::atomic_uint delivered_ {};
    std::atomic_uint drained_ {};
};

/* A suspended guest disables its rings. An event raised meanwhile must wait
 * in the device rather than come back as an empty used buffer, which a
 * virtio-input front end would read as the event that buffer held before;
 * once the ring is enabled again the event is delivered, once. */
void aDisabledRingKeepsTheBuffersTheDeviceFills()
{
    auto device = std::make_unique<EventSourceDevice>();
    auto* source = device.get();
    VhostUserServer server {std::move(device)};
    const SplitRing ring;
    const auto memory = ring.memory();

    constexpr std::size_t eventOffset = 0x300;
    constexpr std::uint8_t staleByte = 0x5a;
    const std::array descriptors {
        protocol::VirtqueueDescriptor {
            .address = SplitRing::guestBase + eventOffset,
            .length = EventSourceDevice::event.size(),
            .flags = protocol::descriptorWrite,
            .next = 0,
        },
    };
    constexpr std::array<std::uint16_t, 1> heads {0};
    ring.publish(descriptors, heads);
    std::memset(memory.data() + eventOffset, staleByte,
        EventSourceDevice::event.size());

    withServerConnection(server, [&](int socket) {
        negotiateProtocolFeatures(socket, protocol::protocolFeatureReplyAck);
        configureMemory(socket, ring.descriptor());
        const auto channels = startQueue(socket, ring, 0, false);

        source->raise();
        awaitCondition([&] { return source->drained() != 0; });
        require(source->drained() == 1);
        pollfd call {.fd = channels.call.readEnd.get(), .events = POLLIN};
        require(::poll(&call, 1, 200) == 0);
        require(ring.usedIndex() == 0);
        require(source->delivered() == 0);
        require(std::ranges::all_of(
            memory.subspan(eventOffset, EventSourceDevice::event.size()),
            [](std::uint8_t byte) { return byte == staleByte; }));

        const protocol::VringState enabled {.index = 0, .value = 1};
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setVringEnable, bytesOf(enabled));
        awaitCall(channels.call.readEnd.get());
        require(ring.usedIndex() == 1);
        require(ring.usedElement(0).length == EventSourceDevice::event.size());
        require(std::ranges::equal(
            memory.subspan(eventOffset, EventSourceDevice::event.size()),
            EventSourceDevice::event));
        require(source->delivered() == 1);
    });
}

/* A device whose requests are one byte long and answered with that byte plus
 * 0xa0, or with that byte plus 0xd0 when the request is discarded. It records
 * the order in which it saw them, can be told to block inside a request, and
 * has an event source to raise. */
class OrderedRequestDevice final : public FixtureDevice {
public:
    OrderedRequestDevice(
        bool keepsRequestsOnDisabledRing,
        bool signalsReadiness,
        bool answersKicksBeforeRunning = false)
        : FixtureDevice("ordered request fixture", 1, signalsReadiness)
        , keeps_(keepsRequestsOnDisabledRing)
        , answersFirst_(answersKicksBeforeRunning)
    {
    }

    [[nodiscard]] bool keepsRequestsOnDisabledRing() const noexcept override
    {
        return keeps_;
    }
    [[nodiscard]] bool answersKicksBeforeRunning() const noexcept override
    {
        return answersFirst_;
    }
    [[nodiscard]] std::vector<std::uint8_t> discardedResponse(
        std::uint32_t index,
        std::span<const std::uint8_t> request) const override
    {
        require(index == 0);
        require(request.size() == 1);
        return {static_cast<std::uint8_t>(0xd0U + request.front())};
    }
    [[nodiscard]] uml::virtio::VhostUserQueueResult processQueue(
        std::uint32_t index,
        std::uint64_t,
        std::span<const std::uint8_t> request,
        std::size_t responseCapacity) override
    {
        require(index == 0);
        require(request.size() == 1);
        require(responseCapacity == 1);
        std::unique_lock lock(mutex_);
        calls_.push_back(request.front());
        changed_.notify_all();
        if (blockNext_) {
            blockNext_ = false;
            blocked_ = true;
            changed_.notify_all();
            changed_.wait(lock, [this] { return !blocked_; });
        }
        return {
            .action = uml::virtio::VhostUserQueueAction::complete,
            .response = {static_cast<std::uint8_t>(0xa0U + request.front())},
        };
    }
    void drainEventSource() override
    {
        FixtureDevice::drainEventSource();
        drained_.fetch_add(1, std::memory_order_release);
    }

    void raise() const { signalReadiness(); }
    [[nodiscard]] unsigned drained() const noexcept
    {
        return drained_.load(std::memory_order_acquire);
    }
    void blockNextRequest()
    {
        std::lock_guard lock(mutex_);
        blockNext_ = true;
    }
    /* Returns once a request is blocked inside the device. */
    void awaitBlocked()
    {
        std::unique_lock lock(mutex_);
        require(changed_.wait_for(
            lock, std::chrono::seconds {2}, [this] { return blocked_; }));
    }
    void unblock()
    {
        {
            std::lock_guard lock(mutex_);
            blocked_ = false;
        }
        changed_.notify_all();
    }
    [[nodiscard]] std::vector<std::uint8_t> calls() const
    {
        std::lock_guard lock(mutex_);
        return calls_;
    }

private:
    bool keeps_;
    bool answersFirst_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::uint8_t> calls_;
    bool blockNext_ {};
    bool blocked_ {};
    std::atomic_uint drained_ {};
};

/* A ring with `count` requests queued, request i carrying the byte i + 1. */
class OrderedRequestRing {
public:
    static constexpr std::size_t requestOffset = 0x300;
    static constexpr std::size_t responseOffset = 0x400;

    explicit OrderedRequestRing(std::uint16_t count)
        : count_(count)
    {
        for (std::uint16_t request = 0; request < count; ++request)
            append(request);
    }

    /* Queues the request carrying byte `value`, after those already queued. */
    void append(std::uint16_t value)
    {
        const std::uint16_t index = published_;
        const std::uint16_t first = static_cast<std::uint16_t>(2U * index);
        const auto descriptors = requestChain(
            requestOffset + index, 1, responseOffset + index, 1, first);
        std::memcpy(
            ring_.memory().data() + SplitRing::descriptorOffset +
                first * sizeof(protocol::VirtqueueDescriptor),
            descriptors.data(),
            descriptors.size() * sizeof(protocol::VirtqueueDescriptor));
        ring_.memory()[requestOffset + index] =
            static_cast<std::uint8_t>(value + 1U);
        ring_.memory()[responseOffset + index] = 0;
        const std::array<std::uint16_t, 1> head {first};
        ring_.writeHeads(head, index);
        ++published_;
        ring_.setAvailableIndex(published_);
    }

    [[nodiscard]] const SplitRing& ring() const noexcept { return ring_; }
    [[nodiscard]] std::uint16_t count() const noexcept { return count_; }
    /* Requires the used ring to hold every request in order, each answered
     * with the byte the device derives from it. */
    void requireAnswered(std::uint16_t answered) const
    {
        require(ring_.usedIndex() == answered);
        for (std::uint16_t request = 0; request < answered; ++request) {
            require(ring_.usedElement(request).id == 2U * request);
            require(ring_.usedElement(request).length == 1);
            require(ring_.memory()[responseOffset + request] ==
                0xa0U + request + 1U);
        }
    }

    /* Requires the used ring to hold `discarded` requests in order, each
     * completed with the byte the device derives for a request it did not
     * run. */
    void requireDiscarded(std::uint16_t discarded) const
    {
        require(ring_.usedIndex() == discarded);
        for (std::uint16_t request = 0; request < discarded; ++request) {
            require(ring_.usedElement(request).id == 2U * request);
            require(ring_.usedElement(request).length == 1);
            require(ring_.memory()[responseOffset + request] ==
                0xd0U + request + 1U);
        }
    }

private:
    SplitRing ring_;
    std::uint16_t count_;
    std::uint16_t published_ {};
};

[[nodiscard]] bool nothingRuns(
    const OrderedRequestRing& requests,
    const OrderedRequestDevice& device,
    int callRead)
{
    pollfd call {.fd = callRead, .events = POLLIN};
    return ::poll(&call, 1, 200) == 0 && requests.ring().usedIndex() == 0 &&
        device.calls().empty();
}

/* Starts queue 0 of `requests` for in-band kicks, with a call pipe. */
struct InbandQueue {
    SocketPair backend;
    Pipe call;
};

/* Starts queue 0 of `requests` with in-band kicks and a call pipe. */
[[nodiscard]] InbandQueue startInbandQueue(
    int socket,
    const OrderedRequestRing& requests)
{
    InbandQueue started {
        .backend = openBackendChannel(
            socket,
            protocol::protocolFeatureInbandNotifications |
                protocol::protocolFeatureSlaveRequest |
                protocol::protocolFeatureReplyAck),
        .call = makePipe(),
    };
    configureMemory(socket, requests.ring().descriptor());
    configureQueue(socket, requests.ring(), 0);
    constexpr std::uint64_t queue = 0;
    const std::array callDescriptors {started.call.writeEnd.get()};
    sendRequestAndReadAcknowledgement(
        socket,
        protocol::VhostRequest::setVringCall,
        bytesOf(queue),
        callDescriptors);
    return started;
}

[[nodiscard]] bool replyWaiting(int socket, int timeoutMilliseconds)
{
    pollfd reply {.fd = socket, .events = POLLIN};
    return ::poll(&reply, 1, timeoutMilliseconds) == 1;
}

/* An in-band kick to a device that answers kicks first is answered while the
 * device still runs the queue. The guest is told not to kick meanwhile, and a
 * request it queues then runs without a kick, after which kicks are wanted
 * again. */
void answeredKickRunsAfterItsReply()
{
    auto device = std::make_unique<OrderedRequestDevice>(false, false, true);
    auto* recorder = device.get();
    VhostUserServer server {std::move(device)};
    OrderedRequestRing requests {1};

    withServerConnection(server, [&](int socket) {
        const auto queue = startInbandQueue(socket, requests);
        recorder->blockNextRequest();
        const protocol::VringState kick {.index = 0, .value = 0};
        sendRequest(socket, protocol::VhostRequest::vringKick, bytesOf(kick));
        recorder->awaitBlocked();
        require(replyWaiting(socket, 2000));
        readAcknowledgement(socket, protocol::VhostRequest::vringKick);
        require(requests.ring().usedFlags() ==
            protocol::usedSuppressNotification);

        requests.append(1);
        recorder->unblock();
        awaitCondition([&] { return requests.ring().usedIndex() == 2; });
        requests.requireAnswered(2);
        require(std::ranges::equal(
            recorder->calls(), std::array<std::uint8_t, 2> {1, 2}));
        awaitCondition([&] { return requests.ring().usedFlags() == 0; });
        (void)getFeatures(socket);
    });
}

/* Any other device answers an in-band kick only once the queue has run, and
 * never tells the guest not to kick. */
void unansweredKickWaitsForTheQueue()
{
    auto device = std::make_unique<OrderedRequestDevice>(false, false);
    auto* recorder = device.get();
    VhostUserServer server {std::move(device)};
    const OrderedRequestRing requests {1};

    withServerConnection(server, [&](int socket) {
        const auto queue = startInbandQueue(socket, requests);
        recorder->blockNextRequest();
        const protocol::VringState kick {.index = 0, .value = 0};
        sendRequest(socket, protocol::VhostRequest::vringKick, bytesOf(kick));
        recorder->awaitBlocked();
        require(!replyWaiting(socket, 200));
        require(requests.ring().usedFlags() == 0);
        recorder->unblock();
        readAcknowledgement(socket, protocol::VhostRequest::vringKick);
        requests.requireAnswered(1);
    });
}

/* An in-band kick that asks for no acknowledgement runs the queue and gets no
 * reply, so the next reply on the socket is the next request's. */
void kickWithoutAcknowledgementRunsTheQueue()
{
    for (const bool answersFirst : {false, true}) {
        auto device =
            std::make_unique<OrderedRequestDevice>(false, false, answersFirst);
        auto* recorder = device.get();
        VhostUserServer server {std::move(device)};
        const OrderedRequestRing requests {1};

        withServerConnection(server, [&](int socket) {
            const auto queue = startInbandQueue(socket, requests);
            const protocol::VringState kick {.index = 0, .value = 0};
            sendRequest(
                socket,
                protocol::VhostRequest::vringKick,
                bytesOf(kick),
                {},
                false);
            awaitCondition([&] { return requests.ring().usedIndex() == 1; });
            requests.requireAnswered(1);
            require(std::ranges::equal(
                recorder->calls(), std::array<std::uint8_t, 1> {1}));
            require(!replyWaiting(socket, 200));
            (void)getFeatures(socket);
        });
    }
}

/* A closed gate keeps every request in its ring, and opening it runs them in
 * the order they were queued, each once. The server is idle in poll when the
 * gate opens, with no kick to follow, so the open itself must wake it. */
void heldRequestsRunInOrderAfterRelease()
{
    auto device = std::make_unique<OrderedRequestDevice>(false, false);
    const auto* recorder = device.get();
    const auto gate = std::make_shared<RequestGate>();
    VhostUserServer server {std::move(device)};
    server.setRequestGate(gate);
    const OrderedRequestRing requests {3};
    require(gate->hold(std::chrono::milliseconds {100}));

    withServerConnection(server, [&](int socket) {
        negotiateProtocolFeatures(socket, protocol::protocolFeatureReplyAck);
        configureMemory(socket, requests.ring().descriptor());
        const auto channels = startQueue(socket, requests.ring());
        writeKick(channels.kick);
        require(nothingRuns(requests, *recorder, channels.call.readEnd.get()));

        gate->release();
        awaitCall(channels.call.readEnd.get());
        requests.requireAnswered(3);
        require(std::ranges::equal(
            recorder->calls(), std::array<std::uint8_t, 3> {1, 2, 3}));
    });
}

/* The guest's suspend disables its rings. A request held meanwhile is not
 * answered empty by a pass over the disabled ring, and enabling the ring
 * again does not run it while the gate is closed: it runs, in order, when the
 * gate opens. */
void heldRequestsSurviveADisabledRing(bool deviceKeepsRequests)
{
    auto device = std::make_unique<OrderedRequestDevice>(
        deviceKeepsRequests, true);
    auto* recorder = device.get();
    const auto gate = std::make_shared<RequestGate>();
    VhostUserServer server {std::move(device)};
    server.setRequestGate(gate);
    const OrderedRequestRing requests {3};
    require(gate->hold(std::chrono::milliseconds {100}));

    withServerConnection(server, [&](int socket) {
        negotiateProtocolFeatures(socket, protocol::protocolFeatureReplyAck);
        configureMemory(socket, requests.ring().descriptor());
        const auto channels = startQueue(socket, requests.ring());
        writeKick(channels.kick);
        require(nothingRuns(requests, *recorder, channels.call.readEnd.get()));

        const auto passOverTheRings = [&] {
            const auto before = recorder->drained();
            recorder->raise();
            awaitCondition([&] { return recorder->drained() != before; });
            require(recorder->drained() == before + 1U);
        };
        const protocol::VringState disabled {.index = 0, .value = 0};
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setVringEnable, bytesOf(disabled));
        passOverTheRings();
        require(nothingRuns(requests, *recorder, channels.call.readEnd.get()));

        const protocol::VringState enabled {.index = 0, .value = 1};
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setVringEnable, bytesOf(enabled));
        passOverTheRings();
        require(nothingRuns(requests, *recorder, channels.call.readEnd.get()));

        gate->release();
        awaitCall(channels.call.readEnd.get());
        requests.requireAnswered(3);
        require(std::ranges::equal(
            recorder->calls(), std::array<std::uint8_t, 3> {1, 2, 3}));
    });
}

/* The gate waits out the request the device is already running and runs
 * nothing after it. */
void holdWaitsForTheRunningRequestAndStopsTheNext()
{
    auto device = std::make_unique<OrderedRequestDevice>(false, false);
    auto* recorder = device.get();
    const auto gate = std::make_shared<RequestGate>();
    VhostUserServer server {std::move(device)};
    server.setRequestGate(gate);
    OrderedRequestRing requests {0};
    recorder->blockNextRequest();

    withServerConnection(server, [&](int socket) {
        negotiateProtocolFeatures(socket, protocol::protocolFeatureReplyAck);
        configureMemory(socket, requests.ring().descriptor());
        const auto channels = startQueue(socket, requests.ring());
        requests.append(0);
        writeKick(channels.kick);
        recorder->awaitBlocked();

        /* Closed, but a request is inside the device: not yet quiet. */
        require(!gate->hold(std::chrono::milliseconds {50}));
        require(gate->held());
        recorder->unblock();
        require(gate->hold(std::chrono::seconds {2}));
        awaitCall(channels.call.readEnd.get());
        requests.requireAnswered(1);

        requests.append(1);
        writeKick(channels.kick);
        pollfd call {.fd = channels.call.readEnd.get(), .events = POLLIN};
        require(::poll(&call, 1, 200) == 0);
        require(requests.ring().usedIndex() == 1);
        require(recorder->calls().size() == 1);

        gate->release();
        awaitCall(channels.call.readEnd.get());
        requests.requireAnswered(2);
        require(std::ranges::equal(
            recorder->calls(), std::array<std::uint8_t, 2> {1, 2}));
    });
}

/* Without a gate, a request on a disabled ring is completed empty at once,
 * which a console's output relies on: its writer waits for the completion
 * with interrupts off. A device whose requests are asynchronous asks to have
 * them kept instead, and they run in order once the ring is enabled. */
void aDisabledRingKeepsRequestsOnlyForADeviceThatAsksForThem()
{
    for (const bool keeps : {false, true}) {
        auto device = std::make_unique<OrderedRequestDevice>(keeps, true);
        auto* recorder = device.get();
        VhostUserServer server {std::move(device)};
        const OrderedRequestRing requests {2};

        withServerConnection(server, [&](int socket) {
            negotiateProtocolFeatures(
                socket, protocol::protocolFeatureReplyAck);
            configureMemory(socket, requests.ring().descriptor());
            const auto channels =
                startQueue(socket, requests.ring(), 0, false);

            const auto before = recorder->drained();
            recorder->raise();
            awaitCondition([&] { return recorder->drained() != before; });
            require(recorder->drained() == before + 1U);
            require(recorder->calls().empty());

            if (!keeps) {
                awaitCall(channels.call.readEnd.get());
                require(requests.ring().usedIndex() == 2);
                require(requests.ring().usedElement(0).length == 0);
                require(requests.ring().usedElement(1).length == 0);
                return;
            }
            pollfd call {.fd = channels.call.readEnd.get(), .events = POLLIN};
            require(::poll(&call, 1, 200) == 0);
            require(requests.ring().usedIndex() == 0);

            const protocol::VringState enabled {.index = 0, .value = 1};
            sendRequestAndReadAcknowledgement(
                socket,
                protocol::VhostRequest::setVringEnable,
                bytesOf(enabled));
            awaitCall(channels.call.readEnd.get());
            requests.requireAnswered(2);
            require(std::ranges::equal(
                recorder->calls(), std::array<std::uint8_t, 2> {1, 2}));
        });
    }
}

/* Only a closed gate discards, and opening it ends the discarding. */
void aGateDiscardsOnlyWhileItIsClosed()
{
    RequestGate gate;
    require(gate.state() == RequestGate::State::open);
    require(!gate.discard());
    require(!gate.held() && !gate.discarding());

    require(gate.hold(std::chrono::milliseconds {10}));
    require(gate.state() == RequestGate::State::held);
    require(gate.discard());
    require(gate.held() && gate.discarding());
    /* Closing it again, and discarding again, change nothing. */
    require(gate.hold(std::chrono::milliseconds {10}));
    require(gate.discard());
    require(gate.state() == RequestGate::State::discarding);
    require(!gate.enter());

    gate.returnToHold();
    require(gate.state() == RequestGate::State::held);
    gate.returnToHold();
    require(gate.state() == RequestGate::State::held);
    require(!gate.enter());

    require(gate.discard());
    gate.release();
    require(gate.state() == RequestGate::State::open);
    require(!gate.discard());
    gate.returnToHold();
    require(gate.state() == RequestGate::State::open);
    require(static_cast<bool>(gate.enter()));
}

/* A guest that ends behind a closed gate cannot stop until its GPU requests
 * complete, so a discarding gate completes each one, those already waiting and
 * those that arrive, with the device's answer for a request that did not run,
 * and never calls the device. The next guest finds a plain closed gate: its
 * requests wait and run, in order, when the gate opens. */
void aDiscardingGateCompletesRequestsWithoutRunningThem()
{
    auto device = std::make_unique<OrderedRequestDevice>(true, false);
    const auto* recorder = device.get();
    const auto gate = std::make_shared<RequestGate>();
    VhostUserServer server {std::move(device)};
    server.setRequestGate(gate);
    OrderedRequestRing requests {2};
    require(gate->hold(std::chrono::milliseconds {100}));

    withServerConnection(server, [&](int socket) {
        negotiateProtocolFeatures(socket, protocol::protocolFeatureReplyAck);
        configureMemory(socket, requests.ring().descriptor());
        const auto channels = startQueue(socket, requests.ring());
        writeKick(channels.kick);
        require(nothingRuns(requests, *recorder, channels.call.readEnd.get()));

        require(gate->discard());
        awaitCall(channels.call.readEnd.get());
        requests.requireDiscarded(2);
        require(recorder->calls().empty());

        requests.append(2);
        writeKick(channels.kick);
        awaitCall(channels.call.readEnd.get());
        requests.requireDiscarded(3);
        require(recorder->calls().empty());

        gate->returnToHold();
        requests.append(3);
        writeKick(channels.kick);
        pollfd call {.fd = channels.call.readEnd.get(), .events = POLLIN};
        require(::poll(&call, 1, 200) == 0);
        require(requests.ring().usedIndex() == 3);
        require(recorder->calls().empty());

        gate->release();
        awaitCall(channels.call.readEnd.get());
        require(requests.ring().usedIndex() == 4);
        require(requests.ring().usedElement(3).length == 1);
        require(requests.ring().memory()
                [OrderedRequestRing::responseOffset + 3] == 0xa0U + 4U);
        require(std::ranges::equal(
            recorder->calls(), std::array<std::uint8_t, 1> {4}));
    });
}

/* A ring the guest disabled is one it no longer waits on: a discarding gate
 * leaves its requests where they are, as a closed gate does, until the ring is
 * enabled again. */
void aDiscardingGateLeavesADisabledRingAlone()
{
    auto device = std::make_unique<OrderedRequestDevice>(true, true);
    auto* recorder = device.get();
    const auto gate = std::make_shared<RequestGate>();
    VhostUserServer server {std::move(device)};
    server.setRequestGate(gate);
    const OrderedRequestRing requests {2};
    require(gate->hold(std::chrono::milliseconds {100}));

    withServerConnection(server, [&](int socket) {
        negotiateProtocolFeatures(socket, protocol::protocolFeatureReplyAck);
        configureMemory(socket, requests.ring().descriptor());
        const auto channels = startQueue(socket, requests.ring(), 0, false);
        const auto passOverTheRings = [&] {
            const auto before = recorder->drained();
            recorder->raise();
            awaitCondition([&] { return recorder->drained() != before; });
            require(recorder->drained() == before + 1U);
        };

        require(gate->discard());
        passOverTheRings();
        require(nothingRuns(requests, *recorder, channels.call.readEnd.get()));

        const protocol::VringState enabled {.index = 0, .value = 1};
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setVringEnable, bytesOf(enabled));
        passOverTheRings();
        awaitCall(channels.call.readEnd.get());
        requests.requireDiscarded(2);
        require(recorder->calls().empty());
    });
}

/* The Virtio-GPU control request a guest sends while it renders, header only:
 * a command that could not run as it stands, so that an answer that is not an
 * error shows it was not run. */
struct DiscardedControlRequest {
    protocol::GpuCommand command;
    std::uint32_t flags;
    std::uint64_t fenceId;
    std::uint32_t contextId;
    std::uint8_t ringIndex;
};

void aDiscardingGateAnswersGpuRequestsWithTheirFences()
{
    constexpr std::uint32_t header = sizeof(protocol::GpuControlHeader);
    constexpr std::size_t requestsOffset = 0x300;
    constexpr std::size_t responsesOffset = 0x800;
    constexpr std::size_t stride = 0x40;
    constexpr std::uint32_t both =
        protocol::gpuFlagFence | protocol::gpuFlagInfoRingIndex;
    /* Two requests wait, one arrives afterwards, and a fourth, the display
     * query, runs; the queue holds four chains. */
    const std::array<DiscardedControlRequest, 3> discarded {{
        {protocol::GpuCommand::submit3d, both, 0x1000'0000'0000'0001ULL, 7, 2},
        {protocol::GpuCommand::getDisplayInfo, 0, 0, 0, 0},
        {protocol::GpuCommand::resourceFlush, protocol::gpuFlagFence, 0x77, 0, 0},
    }};
    const SplitRing ring;
    const auto memory = ring.memory();
    std::array<protocol::VirtqueueDescriptor, 8> descriptors {};
    std::uint16_t published = 0;
    const auto append = [&](const DiscardedControlRequest& request) {
        const std::uint16_t index = published;
        const std::uint16_t first = static_cast<std::uint16_t>(2U * index);
        const auto chain = requestChain(
            requestsOffset + index * stride, header,
            responsesOffset + index * stride, header, first);
        descriptors[first] = chain[0];
        descriptors[first + 1U] = chain[1];
        protocol::GpuControlHeader wire {};
        wire.type = static_cast<std::uint32_t>(request.command);
        wire.flags = request.flags;
        wire.fenceId = request.fenceId;
        wire.contextId = request.contextId;
        wire.ringIndex = request.ringIndex;
        wire.padding = {1, 2, 3};
        std::memcpy(memory.data() + requestsOffset + index * stride,
            &wire, sizeof(wire));
        std::memset(memory.data() + responsesOffset + index * stride, 0xa5,
            header);
        ring.writeDescriptors(descriptors);
        const std::array<std::uint16_t, 1> head {first};
        ring.writeHeads(head, index);
        ++published;
        ring.setAvailableIndex(published);
    };
    const auto requireAnswered = [&](std::uint16_t index) {
        const auto& request = discarded[index];
        require(ring.usedElement(index).id == 2U * index);
        require(ring.usedElement(index).length == header);
        protocol::GpuControlHeader answer {};
        std::memcpy(&answer, memory.data() + responsesOffset + index * stride,
            sizeof(answer));
        require(answer.type ==
            static_cast<std::uint32_t>(protocol::GpuResponse::okNoData));
        require(answer.flags == (request.flags & both));
        require(answer.fenceId == request.fenceId);
        require(answer.contextId == request.contextId);
        require(answer.ringIndex == request.ringIndex);
        require(answer.padding == std::array<std::uint8_t, 3> {});
    };

    const auto gate = std::make_shared<RequestGate>();
    const auto ownedServer =
        makeGpuVhostUserServer("127.0.0.1", 0, {}, 64, 64, false);
    ownedServer->setRequestGate(gate);
    require(gate->hold(std::chrono::milliseconds {100}));
    append(discarded[0]);
    append(discarded[1]);

    withServerConnection(*ownedServer, [&](int socket) {
        negotiateProtocolFeatures(socket, protocol::protocolFeatureReplyAck);
        configureMemory(socket, ring.descriptor());
        const auto channels = startQueue(socket, ring);
        writeKick(channels.kick);
        pollfd call {.fd = channels.call.readEnd.get(), .events = POLLIN};
        require(::poll(&call, 1, 200) == 0);
        require(ring.usedIndex() == 0);

        require(gate->discard());
        awaitCall(channels.call.readEnd.get());
        require(ring.usedIndex() == 2);
        requireAnswered(0);
        requireAnswered(1);

        append(discarded[2]);
        writeKick(channels.kick);
        awaitCall(channels.call.readEnd.get());
        require(ring.usedIndex() == 3);
        requireAnswered(2);

        /* The next guest finds a plain closed gate, and a request the device
         * can run waits for the gate to open and then runs. */
        gate->returnToHold();
        const std::uint16_t index = published;
        const std::uint16_t first = static_cast<std::uint16_t>(2U * index);
        const auto chain = requestChain(
            requestsOffset + index * stride, header,
            responsesOffset + index * stride,
            sizeof(protocol::GpuDisplayInfo), first);
        descriptors[first] = chain[0];
        descriptors[first + 1U] = chain[1];
        protocol::GpuControlHeader wire {};
        wire.type =
            static_cast<std::uint32_t>(protocol::GpuCommand::getDisplayInfo);
        std::memcpy(memory.data() + requestsOffset + index * stride,
            &wire, sizeof(wire));
        ring.writeDescriptors(descriptors);
        const std::array<std::uint16_t, 1> head {first};
        ring.writeHeads(head, index);
        ++published;
        ring.setAvailableIndex(published);
        writeKick(channels.kick);
        require(::poll(&call, 1, 200) == 0);
        require(ring.usedIndex() == 3);

        gate->release();
        awaitCall(channels.call.readEnd.get());
        require(ring.usedIndex() == 4);
        require(ring.usedElement(3).length == sizeof(protocol::GpuDisplayInfo));
        protocol::GpuControlHeader answer {};
        std::memcpy(&answer, memory.data() + responsesOffset + index * stride,
            sizeof(answer));
        require(answer.type == static_cast<std::uint32_t>(
            protocol::GpuResponse::okDisplayInfo));
    });
}

/* The cursor queue has no response: its commands are consumed and completed
 * empty. */
void aDiscardingGateCompletesCursorCommandsEmpty()
{
    constexpr std::uint32_t header = sizeof(protocol::GpuControlHeader);
    constexpr std::size_t requestsOffset = 0x300;
    const SplitRing ring;
    const auto memory = ring.memory();
    std::array<protocol::VirtqueueDescriptor, 3> descriptors {};
    for (std::uint16_t index = 0; index < descriptors.size(); ++index) {
        descriptors[index] = {
            .address = SplitRing::guestBase + requestsOffset + index * 0x40U,
            .length = header,
            .flags = 0,
            .next = 0,
        };
        protocol::GpuControlHeader wire {};
        wire.type = static_cast<std::uint32_t>(
            index == 1 ? protocol::GpuCommand::moveCursor
                       : protocol::GpuCommand::updateCursor);
        std::memcpy(memory.data() + requestsOffset + index * 0x40U,
            &wire, sizeof(wire));
    }
    const std::array<std::uint16_t, 3> heads {0, 1, 2};
    ring.publish(descriptors, heads);

    const auto gate = std::make_shared<RequestGate>();
    const auto ownedServer =
        makeGpuVhostUserServer("127.0.0.1", 0, {}, 64, 64, false);
    ownedServer->setRequestGate(gate);
    require(gate->hold(std::chrono::milliseconds {100}));

    withServerConnection(*ownedServer, [&](int socket) {
        negotiateProtocolFeatures(socket, protocol::protocolFeatureReplyAck);
        configureMemory(socket, ring.descriptor());
        const auto channels =
            startQueue(socket, ring, protocol::gpuCursorQueue);
        writeKick(channels.kick);
        pollfd call {.fd = channels.call.readEnd.get(), .events = POLLIN};
        require(::poll(&call, 1, 200) == 0);
        require(ring.usedIndex() == 0);

        require(gate->discard());
        awaitCall(channels.call.readEnd.get());
        require(ring.usedIndex() == heads.size());
        for (std::size_t index = 0; index < heads.size(); ++index) {
            require(ring.usedElement(index).id == heads[index]);
            require(ring.usedElement(index).length == 0);
        }
    });
}

void validConfigurationProtocol(VhostUserServer& server)
{
    withServerConnection(server, [](int socket) {
        // A back-end that advertises VHOST_USER_F_PROTOCOL_FEATURES must
        // support protocol-feature discovery before SET_FEATURES.
        sendRequest(
            socket,
            protocol::VhostRequest::getProtocolFeatures,
            {},
            {},
            false);
        const auto offered = readValueReply<std::uint64_t>(
            socket, protocol::VhostRequest::getProtocolFeatures);
        require(
            (offered & protocol::protocolFeatureMultipleQueues) != 0);

        // MQ queue-count discovery is permitted after the feature was queried
        // and before the selected protocol-feature mask is sent.
        sendRequest(
            socket, protocol::VhostRequest::getQueueNum, {}, {}, false);
        require(readValueReply<std::uint64_t>(
                    socket, protocol::VhostRequest::getQueueNum) ==
                protocol::gpuQueueCount);

        constexpr std::uint64_t features =
            protocol::protocolFeatureMultipleQueues |
            protocol::protocolFeatureConfig;
        negotiateProtocolFeatures(socket, features);

        const auto reply = readConfig(socket, 0, sizeof(protocol::GpuConfig));
        const auto configuration = reply.as<protocol::GpuConfig>();
        require(reply.header.offset == 0);
        require(reply.header.size == sizeof(configuration));
        require(reply.header.flags == protocol::configFlagWritable);
        require(configuration.eventsRead == 0);
        require(configuration.scanoutCount == 1);
        require(configuration.capsetCount == protocol::venusCapsetIndex + 1U);

        const std::uint32_t clearEvents = UINT32_MAX;
        const protocol::ConfigHeader clearHeader {
            .offset = offsetof(protocol::GpuConfig, eventsClear),
            .size = sizeof(clearEvents),
            .flags = protocol::configFlagWritable,
        };
        const auto clearPayload = configPayload(clearHeader, bytesOf(clearEvents));
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setConfig, clearPayload);

        const std::uint32_t restoredScanoutCount = 1;
        const protocol::ConfigHeader migrationHeader {
            .offset = offsetof(protocol::GpuConfig, scanoutCount),
            .size = sizeof(restoredScanoutCount),
            .flags = protocol::configFlagMigration,
        };
        const auto migrationPayload =
            configPayload(migrationHeader, bytesOf(restoredScanoutCount));
        sendRequestAndReadAcknowledgement(
            socket, protocol::VhostRequest::setConfig, migrationPayload);
    });
}

void invalidControlMessagesAreRejected(VhostUserServer& server)
{
    expectRejected(server, [](int socket) {
        auto descriptor = openTestDescriptor();
        const std::array descriptors {descriptor.get()};
        sendRequest(
            socket,
            protocol::VhostRequest::getFeatures,
            {},
            descriptors);
    });
    expectRejected(server, [](int socket) {
        sendDeviceFeatures(socket);
        const protocol::VringState enable {.index = 0, .value = 2};
        sendRequest(
            socket,
            protocol::VhostRequest::setVringEnable,
            bytesOf(enable));
    });
    expectRejected(server, [](int socket) {
        const protocol::VringState enable {.index = 0, .value = 1};
        sendRequest(
            socket,
            protocol::VhostRequest::setVringEnable,
            bytesOf(enable));
    });
    expectRejected(server, [](int socket) {
        const protocol::VringState queueSize {.index = 0, .value = 3};
        sendRequest(
            socket,
            protocol::VhostRequest::setVringNum,
            bytesOf(queueSize));
    });
    expectRejected(server, [](int socket) {
        protocol::VringAddress address {};
        address.flags = 1;
        sendRequest(
            socket,
            protocol::VhostRequest::setVringAddr,
            bytesOf(address));
    });
    expectRejected(server, [](int socket) {
        constexpr std::uint64_t encoded =
            protocol::vhostVringNoDescriptor | (1ULL << 9U);
        sendRequest(
            socket,
            protocol::VhostRequest::setVringCall,
            bytesOf(encoded));
    });
    expectRejected(server, [](int socket) {
        constexpr std::uint64_t queue = 0;
        sendRequest(
            socket,
            protocol::VhostRequest::setVringKick,
            bytesOf(queue));
    });
    expectRejected(server, [](int socket) {
        auto descriptor = openTestDescriptor();
        constexpr std::uint64_t queue = protocol::vhostVringNoDescriptor;
        const std::array descriptors {descriptor.get()};
        sendRequest(
            socket,
            protocol::VhostRequest::setVringKick,
            bytesOf(queue),
            descriptors);
    });
    expectRejected(server, [](int socket) {
        const protocol::VringState kick {.index = 0, .value = 0};
        sendRequest(
            socket,
            protocol::VhostRequest::vringKick,
            bytesOf(kick));
    });
    expectRejected(server, [](int socket) {
        sendDeviceFeatures(socket);
        constexpr std::uint64_t invalidFeatures =
            protocol::protocolFeatureInbandNotifications;
        sendRequest(
            socket,
            protocol::VhostRequest::setProtocolFeatures,
            bytesOf(invalidFeatures));
    });
    expectRejected(server, [](int socket) {
        constexpr std::uint64_t features =
            protocol::protocolFeatureInbandNotifications |
            protocol::protocolFeatureSlaveRequest |
            protocol::protocolFeatureReplyAck;
        negotiateProtocolFeatures(socket, features);
        const protocol::VringState kick {.index = 0, .value = 1};
        sendRequest(
            socket,
            protocol::VhostRequest::vringKick,
            bytesOf(kick));
    });
    expectRejected(server, [](int socket) {
        const protocol::VringState state {.index = 0, .value = 1};
        sendRequest(
            socket,
            protocol::VhostRequest::getVringBase,
            bytesOf(state));
    });
    expectRejected(server, [](int socket) {
        const ScratchBacking memory {memoryBackingName, 4096};
        const protocol::MemoryTableHeader table {
            .regionCount = 1,
            .padding = 1,
        };
        const protocol::MemoryRegion region {
            .guestAddress = 0x1000,
            .size = 4096,
            .userAddress = 0x1000'0000,
            .mmapOffset = 0,
        };
        std::array<std::uint8_t, sizeof(table) + sizeof(region)> payload {};
        std::memcpy(payload.data(), &table, sizeof(table));
        std::memcpy(
            payload.data() + sizeof(table), &region, sizeof(region));
        const std::array descriptors {memory.descriptor()};
        sendRequest(
            socket,
            protocol::VhostRequest::setMemTable,
            payload,
            descriptors);
    });
}

void invalidConfigurationMessagesAreRejected(VhostUserServer& server)
{
    const auto rejectSet = [&server](protocol::ConfigHeader header,
                                     std::span<const std::uint8_t> contents) {
        expectRejected(server, [&](int socket) {
            negotiateProtocolFeatures(
                socket, protocol::protocolFeatureConfig);
            const auto payload = configPayload(header, contents);
            sendRequest(
                socket, protocol::VhostRequest::setConfig, payload);
        });
    };

    const std::uint32_t value = 1;
    rejectSet(
        {
            .offset = offsetof(protocol::GpuConfig, eventsRead),
            .size = sizeof(value),
            .flags = protocol::configFlagWritable,
        },
        bytesOf(value));
    rejectSet(
        {
            .offset = offsetof(protocol::GpuConfig, eventsClear),
            .size = sizeof(value),
            .flags = 2,
        },
        bytesOf(value));
    rejectSet(
        {
            .offset = sizeof(protocol::GpuConfig) + 1U,
            .size = sizeof(value),
            .flags = protocol::configFlagMigration,
        },
        bytesOf(value));
    rejectSet(
        {
            .offset = offsetof(protocol::GpuConfig, eventsClear),
            .size = sizeof(value) + 1U,
            .flags = protocol::configFlagWritable,
        },
        bytesOf(value));

    withServerConnection(server, [](int socket) {
        negotiateProtocolFeatures(socket, protocol::protocolFeatureConfig);
        const protocol::ConfigHeader header {
            .offset = 0,
            .size = sizeof(protocol::GpuConfig),
            .flags = protocol::configFlagMigration,
        };
        const std::array<std::uint8_t, sizeof(protocol::GpuConfig)> zeros {};
        const auto payload = configPayload(header, zeros);
        sendRequest(
            socket,
            protocol::VhostRequest::getConfig,
            payload,
            {},
            false);
        protocol::VhostHeader response {};
        readExact(socket, &response, sizeof(response));
        require(
            response.request ==
            static_cast<std::uint32_t>(protocol::VhostRequest::getConfig));
        require((response.flags & protocol::vhostUserReply) != 0);
        require(response.size == 0);
    });
    expectRejected(server, [](int socket) {
        negotiateProtocolFeatures(socket, protocol::protocolFeatureConfig);
        const protocol::ConfigHeader header {
            .offset = 0,
            .size = sizeof(protocol::GpuConfig),
            .flags = protocol::configFlagWritable,
        };
        sendRequest(
            socket,
            protocol::VhostRequest::getConfig,
            bytesOf(header));
    });
}

void misalignedTranslatedRingsAreRejected(VhostUserServer& server)
{
    const ScratchBacking memoryDescriptor {memoryBackingName, 4096};
    expectRejected(server, [&](int socket) {
        negotiateProtocolFeatures(socket, protocol::protocolFeatureReplyAck);

        constexpr std::uint64_t guestBase = 0x1000;
        constexpr std::uint64_t skewedUserBase = 0x1000'0001;
        const protocol::MemoryRegion region {
            .guestAddress = guestBase,
            .size = 4096,
            .userAddress = skewedUserBase,
            .mmapOffset = 0,
        };
        const auto payload = memoryTablePayload(std::span {&region, 1U});
        const std::array memoryDescriptors {memoryDescriptor.descriptor()};
        sendRequestAndReadAcknowledgement(
            socket,
            protocol::VhostRequest::setMemTable,
            payload,
            memoryDescriptors);

        const protocol::VringState queueSize {.index = 0, .value = 8};
        sendRequestAndReadAcknowledgement(
            socket,
            protocol::VhostRequest::setVringNum,
            bytesOf(queueSize));
        /* Each opaque address satisfies its wire alignment, but translation
         * through the odd region base would produce odd host pointers. */
        const protocol::VringAddress addresses {
            .index = 0,
            .flags = 0,
            .descriptor = 0x1000'0010,
            .used = 0x1000'0200,
            .available = 0x1000'0100,
            .log = 0,
        };
        sendRequestAndReadAcknowledgement(
            socket,
            protocol::VhostRequest::setVringAddr,
            bytesOf(addresses));
        const protocol::VringState enable {.index = 0, .value = 1};
        sendRequestAndReadAcknowledgement(
            socket,
            protocol::VhostRequest::setVringEnable,
            bytesOf(enable));
        constexpr std::uint64_t pollingQueue =
            protocol::vhostVringNoDescriptor;
        sendRequest(
            socket,
            protocol::VhostRequest::setVringKick,
            bytesOf(pollingQueue));
        readAcknowledgement(
            socket, protocol::VhostRequest::setVringKick);
    });
}

enum class QueueDeviceCommand {
    displayInfo,
    contextCreate,
    resourceCreate2d,
};

struct QueueScenario final {
    bool descriptorNotification {true};
    std::uint16_t availableIndex {1};
    bool reservedDescriptorFlag {};
    bool reservedAvailableFlag {};
    bool pollingCompletion {};
    bool resetOwnerBeforeStart {};
    bool resetOwnerRecreatesContext {};
    bool reversedDescriptorOrder {};
    bool suppressInterrupt {};
    bool enableQueue {true};
    bool inbandNotification {};
    bool legacyFeatureNegotiation {};
    QueueDeviceCommand command {QueueDeviceCommand::displayInfo};
};

void queueNotificationProcessesQueue(
    VhostUserServer& server,
    QueueScenario scenario)
{
    const SplitRing ring;
    const auto memory = ring.memory();

    constexpr std::uint64_t guestBase = SplitRing::guestBase;
    constexpr std::size_t availableOffset = SplitRing::availableOffset;
    constexpr std::size_t requestOffset = 0x300;
    constexpr std::size_t responseOffset = 0x400;
    const bool createContext = scenario.resetOwnerRecreatesContext ||
        scenario.command == QueueDeviceCommand::contextCreate;
    const bool createResource =
        scenario.command == QueueDeviceCommand::resourceCreate2d;
    const auto requestLength = createContext ?
        sizeof(protocol::GpuContextCreate) :
        createResource ? sizeof(protocol::GpuResourceCreate2d) :
                         sizeof(protocol::GpuControlHeader);
    const std::uint32_t responseLength =
        createContext || createResource ?
        sizeof(protocol::GpuControlHeader) :
        sizeof(protocol::GpuDisplayInfo);
    auto descriptors = requestChain(
        requestOffset,
        static_cast<std::uint32_t>(requestLength),
        responseOffset,
        responseLength);
    if (scenario.reservedDescriptorFlag) {
        descriptors[0].flags =
            static_cast<std::uint16_t>(descriptors[0].flags | 8U);
    }
    if (scenario.reversedDescriptorOrder) {
        descriptors = {
            protocol::VirtqueueDescriptor {
                .address = guestBase + responseOffset,
                .length = responseLength,
                .flags = static_cast<std::uint16_t>(
                    protocol::descriptorWrite | protocol::descriptorNext),
                .next = 1,
            },
            protocol::VirtqueueDescriptor {
                .address = guestBase + requestOffset,
                .length = static_cast<std::uint32_t>(requestLength),
                .flags = 0,
                .next = 0,
            },
        };
    }
    ring.writeDescriptors(descriptors);
    if (createContext) {
        protocol::GpuContextCreate request {};
        request.header.type =
            static_cast<std::uint32_t>(protocol::GpuCommand::contextCreate);
        request.header.contextId = 41;
        request.contextInit = protocol::virglCapsetId;
        std::memcpy(memory.data() + requestOffset, &request, sizeof(request));
    } else if (createResource) {
        protocol::GpuResourceCreate2d request {};
        request.header.type = static_cast<std::uint32_t>(
            protocol::GpuCommand::resourceCreate2d);
        request.resourceId = 73;
        request.format = static_cast<std::uint32_t>(
            protocol::GpuFormat::b8g8r8a8Unorm);
        request.width = 4;
        request.height = 4;
        std::memcpy(memory.data() + requestOffset, &request, sizeof(request));
    } else {
        protocol::GpuControlHeader request {};
        request.type =
            static_cast<std::uint32_t>(protocol::GpuCommand::getDisplayInfo);
        std::memcpy(memory.data() + requestOffset, &request, sizeof(request));
    }
    constexpr std::array<std::uint16_t, 1> head {0};
    const std::uint16_t availableFlags =
        static_cast<std::uint16_t>(
            (scenario.suppressInterrupt ?
                 protocol::availableSuppressInterrupt : 0U) |
            (scenario.reservedAvailableFlag ? 2U : 0U));
    const std::uint16_t initialAvailableFlags = 0;
    std::memcpy(
        memory.data() + availableOffset,
        &initialAvailableFlags,
        sizeof(initialAvailableFlags));
    ring.writeHeads(head);
    constexpr std::uint8_t responseSentinel = 0xa5;
    std::memset(
        memory.data() + responseOffset,
        responseSentinel,
        sizeof(protocol::GpuDisplayInfo));

    withServerConnection(server, [&](int socket) {
        if (scenario.legacyFeatureNegotiation) {
            constexpr std::uint64_t deviceFeatures =
                protocol::virtioFeatureVersion1;
            sendRequest(
                socket,
                protocol::VhostRequest::setFeatures,
                bytesOf(deviceFeatures),
                {},
                false);
        } else {
            std::uint64_t protocolFeatures =
                protocol::protocolFeatureReplyAck;
            if (scenario.inbandNotification) {
                protocolFeatures |=
                    protocol::protocolFeatureInbandNotifications |
                    protocol::protocolFeatureSlaveRequest;
            }
            negotiateProtocolFeatures(socket, protocolFeatures);
        }
        configureMemory(socket, ring.descriptor());
        configureQueue(
            socket,
            ring,
            0,
            scenario.enableQueue && !scenario.legacyFeatureNegotiation);
        const protocol::VringState enable {.index = 0, .value = 1};

        constexpr std::uint64_t queue = 0;
        Pipe call;
        SocketPair backendChannel;
        if (scenario.inbandNotification) {
            backendChannel = attachBackendChannel(socket);
        } else if (scenario.pollingCompletion) {
            constexpr std::uint64_t pollingQueue =
                protocol::vhostVringNoDescriptor;
            sendRequestAndReadAcknowledgement(
                socket,
                protocol::VhostRequest::setVringCall,
                bytesOf(pollingQueue));
        } else {
            call = makePipe();
            const std::array callDescriptors {call.writeEnd.get()};
            sendRequestAndReadAcknowledgement(
                socket,
                protocol::VhostRequest::setVringCall,
                bytesOf(queue),
                callDescriptors);
        }

        if (scenario.resetOwnerBeforeStart) {
            sendRequestAndReadAcknowledgement(
                socket, protocol::VhostRequest::resetOwner, {});
            sendRequestAndReadAcknowledgement(
                socket,
                protocol::VhostRequest::setVringEnable,
                bytesOf(enable));
        }

        Pipe kick;
        if (scenario.inbandNotification) {
            // VRING_KICK travels over the main channel below.
        } else if (scenario.descriptorNotification) {
            kick = makePipe();
            const std::array kickDescriptors {kick.readEnd.get()};
            sendRequestAndReadAcknowledgement(
                socket,
                protocol::VhostRequest::setVringKick,
                bytesOf(queue),
                kickDescriptors);
        } else {
            constexpr std::uint64_t pollingQueue =
                protocol::vhostVringNoDescriptor;
            sendRequestAndReadAcknowledgement(
                socket,
                protocol::VhostRequest::setVringKick,
                bytesOf(pollingQueue));
        }

        auto& sharedAvailableFlags = *reinterpret_cast<std::uint16_t*>(
            memory.data() + availableOffset);
        std::atomic_ref<std::uint16_t>(sharedAvailableFlags)
            .store(availableFlags, std::memory_order_relaxed);
        ring.setAvailableIndex(scenario.availableIndex);
        if (scenario.inbandNotification) {
            const protocol::VringState inbandKick {.index = 0, .value = 0};
            sendRequest(
                socket,
                protocol::VhostRequest::vringKick,
                bytesOf(inbandKick));
            const int backend = backendChannel.descriptors[0].get();
            protocol::VhostHeader notification {};
            protocol::VringState notifiedQueue {};
            readExact(backend, &notification, sizeof(notification));
            readExact(backend, &notifiedQueue, sizeof(notifiedQueue));
            require(
                notification.request == static_cast<std::uint32_t>(
                    protocol::VhostSlaveRequest::vringCall));
            require(
                notification.flags ==
                (protocol::vhostUserVersion |
                 protocol::vhostUserNeedReply));
            require(notification.size == sizeof(notifiedQueue));
            require(notifiedQueue.index == 0);
            require(notifiedQueue.value == 0);

            replyToBackend(backend, notification);
            readAcknowledgement(
                socket, protocol::VhostRequest::vringKick);
            (void)getFeatures(socket);
        } else if (scenario.descriptorNotification) {
            writeKick(kick);
        }
        if (scenario.availableIndex > 8 ||
            scenario.reservedDescriptorFlag ||
            scenario.reservedAvailableFlag ||
            scenario.reversedDescriptorOrder) {
            waitForConnectionClose(socket);
            return;
        }
        if (scenario.inbandNotification) {
            // The subsequent main-channel reply above is the completion
            // synchronization point after the backend acknowledgement.
        } else if (scenario.pollingCompletion || scenario.suppressInterrupt) {
            require(::uml::testing::waitUntil(
                [&] { return ring.usedIndex() == 1; },
                std::chrono::seconds(2),
                std::chrono::steady_clock::duration::zero()));
            if (scenario.suppressInterrupt &&
                !scenario.pollingCompletion) {
                pollfd completion {.fd = call.readEnd.get(), .events = POLLIN};
                require(::poll(&completion, 1, 50) == 0);
            }
        } else {
            if (!scenario.enableQueue && !scenario.legacyFeatureNegotiation) {
                /* The GPU keeps a request on a ring the front end has not
                 * enabled, untouched, and answers it once it is. */
                pollfd completion {.fd = call.readEnd.get(), .events = POLLIN};
                require(::poll(&completion, 1, 200) == 0);
                require(ring.usedIndex() == 0);
                require(std::ranges::all_of(
                    memory.subspan(
                        responseOffset, sizeof(protocol::GpuDisplayInfo)),
                    [](std::uint8_t byte) {
                        return byte == responseSentinel;
                    }));
                sendRequestAndReadAcknowledgement(
                    socket,
                    protocol::VhostRequest::setVringEnable,
                    bytesOf(enable));
            }
            awaitCall(call.readEnd.get());
        }

        require(ring.usedIndex() == 1);
        protocol::GpuControlHeader response {};
        std::memcpy(
            &response, memory.data() + responseOffset, sizeof(response));
        require(
            response.type == static_cast<std::uint32_t>(
                createContext || createResource ?
                    protocol::GpuResponse::okNoData :
                    protocol::GpuResponse::okDisplayInfo));

        if (scenario.resetOwnerRecreatesContext) {
            /* The second identical context creation succeeds only if
             * RESET_OWNER retired device state, while retaining the reusable
             * memory table and notification endpoints. */
            sendRequestAndReadAcknowledgement(
                socket, protocol::VhostRequest::resetOwner, {});
            sendRequestAndReadAcknowledgement(
                socket,
                protocol::VhostRequest::setVringEnable,
                bytesOf(enable));
            const std::array kickDescriptors {kick.readEnd.get()};
            sendRequestAndReadAcknowledgement(
                socket,
                protocol::VhostRequest::setVringKick,
                bytesOf(queue),
                kickDescriptors);

            constexpr std::array<std::uint16_t, 1> secondHead {0};
            ring.writeHeads(secondHead, 1);
            std::memset(
                memory.data() + responseOffset,
                responseSentinel,
                sizeof(protocol::GpuControlHeader));
            ring.setAvailableIndex(2);
            writeKick(kick);

            awaitCall(call.readEnd.get());
            require(ring.usedIndex() == 2);
            std::memcpy(
                &response, memory.data() + responseOffset, sizeof(response));
            require(response.type == static_cast<std::uint32_t>(
                protocol::GpuResponse::okNoData));
        }
    });
}

void disconnectReconnectIsPristine(VhostUserServer& server)
{
    // Each helper invocation opens and closes a distinct transport connection.
    // The second identical command can succeed only if disconnect retired both
    // host bookkeeping and the corresponding renderer object from the first.
    for (unsigned connection = 0; connection < 2; ++connection) {
        queueNotificationProcessesQueue(
            server, {.command = QueueDeviceCommand::contextCreate});
    }
    for (unsigned connection = 0; connection < 2; ++connection) {
        queueNotificationProcessesQueue(
            server, {.command = QueueDeviceCommand::resourceCreate2d});
    }
}

/* A control request whose writable descriptors cannot hold its reply does not
 * run, and the connection survives it: with room for a response header it is
 * answered with an invalid-parameter error, and with less it completes with
 * nothing written. The repeated context create shows that the unanswered one
 * did not run, since it would have taken the ID. */
void shortResponseBuffersAreAnsweredWithoutRunningTheCommand(
    VhostUserServer& server)
{
    const SplitRing ring;
    const auto memory = ring.memory();
    struct Chain {
        std::size_t requestOffset;
        std::uint32_t requestLength;
        std::size_t responseOffset;
        std::uint32_t responseLength;
    };
    constexpr std::uint32_t header = sizeof(protocol::GpuControlHeader);
    constexpr std::uint32_t create = sizeof(protocol::GpuContextCreate);
    constexpr std::uint32_t displayInfo = sizeof(protocol::GpuDisplayInfo);
    constexpr std::size_t responsesOffset = 0x400;
    constexpr std::array chains {
        Chain {0x300, header, responsesOffset, header},
        Chain {0x320, create, 0x600, header - 1},
        Chain {0x380, create, 0x700, header},
        Chain {0x3e0, header, 0x800, displayInfo},
    };
    std::array<protocol::VirtqueueDescriptor, 2 * chains.size()> descriptors {};
    std::array<std::uint16_t, chains.size()> heads {};
    for (std::size_t index = 0; index < chains.size(); ++index) {
        const auto& chain = chains[index];
        const auto first = static_cast<std::uint16_t>(2 * index);
        heads[index] = first;
        const auto pair = requestChain(
            chain.requestOffset,
            chain.requestLength,
            chain.responseOffset,
            chain.responseLength,
            first);
        descriptors[first] = pair[0];
        descriptors[first + 1U] = pair[1];
    }
    for (const auto& chain : {chains[0], chains[3]}) {
        protocol::GpuControlHeader request {};
        request.type =
            static_cast<std::uint32_t>(protocol::GpuCommand::getDisplayInfo);
        std::memcpy(
            memory.data() + chain.requestOffset, &request, sizeof(request));
    }
    for (const auto& chain : {chains[1], chains[2]}) {
        protocol::GpuContextCreate request {};
        request.header.type =
            static_cast<std::uint32_t>(protocol::GpuCommand::contextCreate);
        request.header.contextId = 51;
        request.contextInit = protocol::virglCapsetId;
        std::memcpy(
            memory.data() + chain.requestOffset, &request, sizeof(request));
    }
    constexpr std::uint8_t responseSentinel = 0xa5;
    std::memset(memory.data() + responsesOffset, responseSentinel,
        memory.size() - responsesOffset);
    ring.publish(descriptors, heads);

    const auto responseType = [&](const Chain& chain) {
        protocol::GpuControlHeader response {};
        std::memcpy(
            &response, memory.data() + chain.responseOffset, sizeof(response));
        return static_cast<protocol::GpuResponse>(response.type);
    };
    const auto untouched = [&](std::size_t offset, std::size_t size) {
        return std::ranges::all_of(
            memory.subspan(offset, size),
            [](std::uint8_t byte) { return byte == responseSentinel; });
    };
    withServerConnection(server, [&](int socket) {
        negotiateProtocolFeatures(socket, protocol::protocolFeatureReplyAck);
        configureMemory(socket, ring.descriptor());
        const auto channels = startQueue(socket, ring);
        awaitCall(channels.call.readEnd.get());
        require(ring.usedIndex() == chains.size());
        for (std::size_t index = 0; index < chains.size(); ++index)
            require(ring.usedElement(index).id == heads[index]);

        require(ring.usedElement(0).length == header);
        require(responseType(chains[0]) ==
            protocol::GpuResponse::errorInvalidParameter);
        require(untouched(
            chains[0].responseOffset + header, displayInfo - header));

        require(ring.usedElement(1).length == 0);
        require(untouched(chains[1].responseOffset, header));

        require(ring.usedElement(2).length == header);
        require(responseType(chains[2]) == protocol::GpuResponse::okNoData);

        require(ring.usedElement(3).length == displayInfo);
        require(responseType(chains[3]) ==
            protocol::GpuResponse::okDisplayInfo);
    });
}

} // namespace

void runVhostUserProtocolTests()
{
    const auto before = descriptorCount();
    multipleDeferredCompletionsRetainTheirDescriptorChains();
    deferredCompletionSupportsFourQueues();
    completedPrefixNotifiesBeforeRetry();
    aDisabledRingKeepsTheBuffersTheDeviceFills();
    aDisabledRingKeepsRequestsOnlyForADeviceThatAsksForThem();
    aGateDiscardsOnlyWhileItIsClosed();
    aDiscardingGateCompletesRequestsWithoutRunningThem();
    aDiscardingGateLeavesADisabledRingAlone();
    aDiscardingGateAnswersGpuRequestsWithTheirFences();
    aDiscardingGateCompletesCursorCommandsEmpty();
    heldRequestsRunInOrderAfterRelease();
    answeredKickRunsAfterItsReply();
    unansweredKickWaitsForTheQueue();
    kickWithoutAcknowledgementRunsTheQueue();
    heldRequestsSurviveADisabledRing(false);
    heldRequestsSurviveADisabledRing(true);
    holdWaitsForTheRunningRequestAndStopsTheNext();
    configChangeWaitsForTheBackendChannelAndRequiresAcknowledgement();
    configChangeWaitStillAnswersConfigReads();
    closedBackendChannelEndsTheConnectionQuietly();
    sharedMemoryRequiresBackendAcknowledgements();
    sharedMemoryConfigurationRequiresNegotiation();
    standardizedSharedMemoryNegotiationBatchesBackendRequestsBeforeReply();
    rejectedSharedMemoryActionClosesBackendChannel();
    {
        const auto ownedServer = makeGpuVhostUserServer(
            "127.0.0.1", 0, {}, 64, 64, false);
        auto& server = *ownedServer;
        validConfigurationProtocol(server);
        invalidControlMessagesAreRejected(server);
        invalidConfigurationMessagesAreRejected(server);
        misalignedTranslatedRingsAreRejected(server);
        queueNotificationProcessesQueue(server, {});
        queueNotificationProcessesQueue(
            server, {.descriptorNotification = false});
        queueNotificationProcessesQueue(
            server, {.availableIndex = 9});
        queueNotificationProcessesQueue(
            server, {.reservedDescriptorFlag = true});
        queueNotificationProcessesQueue(
            server, {.reservedAvailableFlag = true});
        queueNotificationProcessesQueue(
            server, {.pollingCompletion = true});
        queueNotificationProcessesQueue(
            server, {.resetOwnerBeforeStart = true});
        queueNotificationProcessesQueue(
            server, {.resetOwnerRecreatesContext = true});
        disconnectReconnectIsPristine(server);
        shortResponseBuffersAreAnsweredWithoutRunningTheCommand(server);
        queueNotificationProcessesQueue(
            server, {.reversedDescriptorOrder = true});
        queueNotificationProcessesQueue(
            server, {.suppressInterrupt = true});
        queueNotificationProcessesQueue(
            server, {.enableQueue = false});
        queueNotificationProcessesQueue(
            server, {.inbandNotification = true});
        queueNotificationProcessesQueue(
            server, {.legacyFeatureNegotiation = true});
    }
    require(descriptorCount() == before);
}

} // namespace uml::gpu::testing
