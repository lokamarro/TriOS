// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu_test_support.hpp"
#include "guest_end.hpp"
#include "in_process_service.hpp"
#include "in_process_service_testing.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string_view>

namespace uml::gpu::testing {
namespace {

void forceRepeatedFailure(StartupFailure failure)
{
    for (unsigned iteration = 0; iteration < 16; ++iteration) {
        const auto before = descriptorCount();
        failNextStartup(failure);
        requireThrows<std::runtime_error>([] {
            InProcessGpuService service {InProcessGpuOptions {}};
        });
        require(descriptorCount() == before);
    }
}

void endpointsTransferExactlyOnce()
{
    InProcessGpuOptions options;
    options.vncPort = 0;
    options.width = 64;
    options.height = 64;
    InProcessGpuService service {std::move(options)};
    const auto transportName = service.takeConnectionName();
    const auto inputName = service.takeInputConnectionName();
    const int transport = descriptorFromName(transportName);
    const int input = descriptorFromName(inputName);
    require(transport >= 0 && input >= 0);

    requireThrows<std::logic_error>(
        [&] { (void)service.takeConnectionName(); });
    requireThrows<std::logic_error>(
        [&] { (void)service.takeInputConnectionName(); });
    require(::close(transport) == 0);
    require(::close(input) == 0);
    service.stop();
}

void transferredEndpointsAreNotClosedByService()
{
    const auto before = descriptorCount();
    std::vector<UniqueDescriptor> replacements;
    int transport = -1;
    int input = -1;
    {
        InProcessGpuOptions options;
        options.vncPort = 0;
        options.width = 64;
        options.height = 64;
        auto service = std::make_unique<InProcessGpuService>(
            std::move(options));
        transport = descriptorFromName(service->takeConnectionName());
        input = descriptorFromName(service->takeInputConnectionName());
        require(transport != input);
        require(::close(transport) == 0);
        require(::close(input) == 0);

        bool reusedTransport = false;
        bool reusedInput = false;
        for (unsigned attempt = 0;
             attempt < 64 && (!reusedTransport || !reusedInput);
             ++attempt) {
            auto replacement = openTestDescriptor();
            reusedTransport = reusedTransport ||
                replacement.get() == transport;
            reusedInput = reusedInput || replacement.get() == input;
            replacements.push_back(std::move(replacement));
        }
        require(reusedTransport && reusedInput);
        service->stop();
        service.reset();
        for (const auto& replacement : replacements)
            require(::fcntl(replacement.get(), F_GETFD) >= 0);
    }
    replacements.clear();
    require(descriptorCount() == before);
}

void stopWithoutEndpointTransferIsBalanced()
{
    const auto before = descriptorCount();
    {
        InProcessGpuOptions options;
        options.vncPort = 0;
        options.width = 64;
        options.height = 64;
        InProcessGpuService service {std::move(options)};
        service.stop();
        service.stop();
    }
    require(descriptorCount() == before);
}

void partialFrameStopUsesOwnedWake(bool partialPayload)
{
    const auto before = descriptorCount();
    {
        InProcessGpuOptions options;
        options.vncPort = 0;
        options.width = 64;
        options.height = 64;
        InProcessGpuService service {std::move(options)};
        const int transport =
            descriptorFromName(service.takeConnectionName());
        require(transport >= 0);
        writePartialFrame(transport, partialPayload);

        service.stop();
        require(::close(transport) == 0);
    }
    require(descriptorCount() == before);
}

/*
 * The guest endpoint of the vhost-user control socket must stay blocking.
 *
 * With in-band notifications negotiated the front-end's only queue
 * notification is a synchronous VHOST_USER_VRING_KICK round trip on this
 * socket, and virtqueue_notify() latches vq->broken permanently and
 * irreversibly the moment that round trip reports a failure. A blocking
 * endpoint is what makes every remaining failure a genuine one: there is no
 * would-block to mistake for a dead backend, and a reply delayed behind GPU
 * work -- a request that arrives while a queue runs waits for it -- is
 * waited out rather than failed.
 */
void guestTransportEndpointBlocks()
{
    InProcessGpuOptions options;
    options.vncPort = 0;
    options.width = 64;
    options.height = 64;
    InProcessGpuService service {std::move(options)};
    const int transport = descriptorFromName(service.takeConnectionName());
    require(transport >= 0);

    const int flags = ::fcntl(transport, F_GETFL);
    require(flags >= 0);
    require((flags & O_NONBLOCK) == 0);

    service.stop();
    require(::close(transport) == 0);
}

/* A request the guest queues while the service holds its requests must stay
 * in the ring, and the held service must run it once released. The release
 * must wake a server that has nothing else to do. `socket` is the guest end of
 * the connection the request travels on. */
void heldRequestRunsAfterRelease(InProcessGpuService& service, int socket)
{
    const SplitRing ring;
    publishDisplayInfoRequest(ring);
    constexpr std::uint8_t sentinel = 0x5a;
    std::memset(ring.memory().data() + displayInfoResponseOffset, sentinel,
        sizeof(protocol::GpuDisplayInfo));
    const auto responseType = [&] {
        protocol::GpuControlHeader response {};
        std::memcpy(
            &response,
            ring.memory().data() + displayInfoResponseOffset,
            sizeof(response));
        return response.type;
    };

    negotiateProtocolFeatures(socket, protocol::protocolFeatureReplyAck);
    configureMemory(socket, ring.descriptor());
    const auto channels = startQueue(socket, ring);
    writeKick(channels.kick);
    pollfd call {.fd = channels.call.readEnd.get(), .events = POLLIN};
    require(::poll(&call, 1, 200) == 0);
    require(ring.usedIndex() == 0);
    require(ring.memory()[displayInfoResponseOffset] == sentinel);

    service.releaseRequests();
    awaitCall(channels.call.readEnd.get());
    require(ring.usedIndex() == 1);
    require(responseType() == static_cast<std::uint32_t>(
        protocol::GpuResponse::okDisplayInfo));
}

/* The application holds the GPU's requests before it enters the background,
 * through the service it created. */
void holdRequestsHoldsTheServicesRequests()
{
    InProcessGpuOptions options;
    options.vncPort = 0;
    options.width = 64;
    options.height = 64;
    InProcessGpuService service {std::move(options)};
    UniqueDescriptor transport {descriptorFromName(service.takeConnectionName())};
    UniqueDescriptor input {descriptorFromName(service.takeInputConnectionName())};
    require(service.holdRequests(std::chrono::milliseconds {100}));
    /* The guest's display is a question the service answers whether or not its
     * requests are held. Nothing has bound a scanout, so it is not on, whether
     * or not the service has made its display yet. */
    require(!service.guestScanoutActive().value_or(false));

    heldRequestRunsAfterRelease(service, transport.get());

    /* Holding again, with nothing running, is quiet and holds the next one. */
    require(service.holdRequests(std::chrono::milliseconds {100}));
    service.releaseRequests();
    service.stop();
}

/* The display's frame counters are the display's own: the service has them
 * as soon as it has its display, whether or not its requests are held, and
 * none once it has let the display go. */
void displayStatisticsBelongToTheDisplay()
{
    InProcessGpuOptions options;
    options.vncPort = 0;
    options.width = 64;
    options.height = 64;
    InProcessGpuService service {std::move(options)};
    const auto before = service.displayStatistics();
    require(before.has_value());
    require(before->guestFrames == 0 && before->presentedFrames == 0 &&
        before->droppedFrames == 0 && before->cpuFallbackFrames == 0);
    require(service.holdRequests(std::chrono::milliseconds {100}));
    require(service.displayStatistics().has_value());
    service.releaseRequests();
    service.stop();
    require(!service.displayStatistics().has_value());
}

/* A guest that restarts itself is a new guest on the same machine. The next
 * connection serves it like the first, the old guest's endpoints are the
 * guest's to close, and a hold the application placed before the restart is
 * still in place, because the new device starts closed to the guest. */
void nextConnectionServesTheNextGuest()
{
    const auto before = descriptorCount();
    {
        InProcessGpuOptions options;
        options.vncPort = 0;
        options.width = 64;
        options.height = 64;
        InProcessGpuService service {std::move(options)};
        {
            UniqueDescriptor transport {
                descriptorFromName(service.takeConnectionName())};
            UniqueDescriptor input {
                descriptorFromName(service.takeInputConnectionName())};
            require(service.holdRequests(std::chrono::milliseconds {100}));
            heldRequestRunsAfterRelease(service, transport.get());
            require(service.holdRequests(std::chrono::milliseconds {100}));
            /* The old guest goes: its endpoints close here. */
        }
        service.startNextConnection();
        require(service.requestsHeld());

        UniqueDescriptor transport {
            descriptorFromName(service.takeConnectionName())};
        UniqueDescriptor input {
            descriptorFromName(service.takeInputConnectionName())};
        requireThrows<std::logic_error>(
            [&] { (void)service.takeConnectionName(); });
        requireThrows<std::logic_error>(
            [&] { (void)service.takeInputConnectionName(); });
        heldRequestRunsAfterRelease(service, transport.get());
        require(!service.requestsHeld());

        /* And again: the connection after that one is as good. */
        transport = {};
        input = {};
        service.startNextConnection();
        require(!service.requestsHeld());
        UniqueDescriptor last {
            descriptorFromName(service.takeConnectionName())};
        UniqueDescriptor lastInput {
            descriptorFromName(service.takeInputConnectionName())};
        require(service.holdRequests(std::chrono::milliseconds {100}));
        heldRequestRunsAfterRelease(service, last.get());
        service.stop();
    }
    require(descriptorCount() == before);
}

/* Fenced control requests a rendering guest has in flight, queued in `ring`
 * one after another: a SUBMIT_3D on a Venus ring, header only, so a device
 * that ran it would answer with an error and not with success. */
class FencedRequests {
public:
    static constexpr std::size_t requestsOffset = 0x300;
    static constexpr std::size_t responsesOffset = 0x800;
    static constexpr std::size_t stride = 0x40;
    static constexpr std::uint32_t flags =
        protocol::gpuFlagFence | protocol::gpuFlagInfoRingIndex;

    explicit FencedRequests(const SplitRing& ring) : ring_(ring) {}

    void append(std::uint64_t fenceId)
    {
        const std::uint16_t index = published_;
        const std::uint16_t first = static_cast<std::uint16_t>(2U * index);
        const auto chain = requestChain(
            requestsOffset + index * stride,
            sizeof(protocol::GpuControlHeader),
            responsesOffset + index * stride,
            sizeof(protocol::GpuControlHeader),
            first);
        descriptors_[first] = chain[0];
        descriptors_[first + 1U] = chain[1];
        protocol::GpuControlHeader request {};
        request.type =
            static_cast<std::uint32_t>(protocol::GpuCommand::submit3d);
        request.flags = flags;
        request.fenceId = fenceId;
        request.contextId = 7;
        request.ringIndex = 2;
        std::memcpy(
            ring_.memory().data() + requestsOffset + index * stride,
            &request,
            sizeof(request));
        std::memset(
            ring_.memory().data() + responsesOffset + index * stride,
            0xa5,
            sizeof(request));
        ring_.writeDescriptors(descriptors_);
        const std::array<std::uint16_t, 1> head {first};
        ring_.writeHeads(head, index);
        ++published_;
        ring_.setAvailableIndex(published_);
    }

    /* Requires request `index`, queued with `fenceId`, to have completed with
     * success that carries its fence back. */
    void requireAnswered(std::uint16_t index, std::uint64_t fenceId) const
    {
        require(ring_.usedIndex() > index);
        require(ring_.usedElement(index).id == 2U * index);
        require(ring_.usedElement(index).length ==
            sizeof(protocol::GpuControlHeader));
        protocol::GpuControlHeader answer {};
        std::memcpy(
            &answer,
            ring_.memory().data() + responsesOffset + index * stride,
            sizeof(answer));
        require(answer.type ==
            static_cast<std::uint32_t>(protocol::GpuResponse::okNoData));
        require(answer.flags == flags);
        require(answer.fenceId == fenceId);
        require(answer.contextId == 7);
        require(answer.ringIndex == 2);
    }

private:
    const SplitRing& ring_;
    std::array<protocol::VirtqueueDescriptor, 8> descriptors_ {};
    std::uint16_t published_ {};
};

/* A guest that ends behind the application's hold, as an Android guest that
 * reboots in the background does, cannot finish stopping while its GPU
 * requests wait in the ring: its shutdown waits for them. The guest's end,
 * reported through the registry the runtime calls, turns the hold to
 * discarding: each request, those waiting and those that arrive, completes as
 * success with its fence, and none reaches the renderer. The hold itself stays:
 * the next guest finds it as a plain one. */
void theGuestEndingDiscardsTheHeldRequests()
{
    const auto before = descriptorCount();
    {
        InProcessGpuOptions options;
        options.vncPort = 0;
        options.width = 64;
        options.height = 64;
        InProcessGpuService service {std::move(options)};
        {
            UniqueDescriptor transport {
                descriptorFromName(service.takeConnectionName())};
            UniqueDescriptor input {
                descriptorFromName(service.takeInputConnectionName())};
            require(service.holdRequests(std::chrono::milliseconds {100}));

            const SplitRing ring;
            FencedRequests requests {ring};
            requests.append(0x1000'0000'0000'0001ULL);
            negotiateProtocolFeatures(
                transport.get(), protocol::protocolFeatureReplyAck);
            configureMemory(transport.get(), ring.descriptor());
            const auto channels = startQueue(transport.get(), ring);
            writeKick(channels.kick);
            pollfd call {.fd = channels.call.readEnd.get(), .events = POLLIN};
            require(::poll(&call, 1, 200) == 0);
            require(ring.usedIndex() == 0);

            uml::device::notify_guest_end();
            awaitCall(channels.call.readEnd.get());
            require(ring.usedIndex() == 1);
            requests.requireAnswered(0, 0x1000'0000'0000'0001ULL);
            require(service.requestsHeld());

            requests.append(0x1000'0000'0000'0002ULL);
            writeKick(channels.kick);
            awaitCall(channels.call.readEnd.get());
            require(ring.usedIndex() == 2);
            requests.requireAnswered(1, 0x1000'0000'0000'0002ULL);
            /* The old guest goes: its endpoints close here. */
        }
        service.startNextConnection();
        require(service.requestsHeld());

        UniqueDescriptor transport {
            descriptorFromName(service.takeConnectionName())};
        UniqueDescriptor input {
            descriptorFromName(service.takeInputConnectionName())};
        /* A plain hold again: the request waits, and runs when it opens. */
        heldRequestRunsAfterRelease(service, transport.get());
        require(!service.requestsHeld());
        service.stop();
    }
    require(descriptorCount() == before);
}

/* A guest that ends with the gate open has nothing held, and everything it
 * sent runs. */
void aGuestThatEndsBehindAnOpenGateRunsItsRequests()
{
    const auto before = descriptorCount();
    {
        InProcessGpuOptions options;
        options.vncPort = 0;
        options.width = 64;
        options.height = 64;
        InProcessGpuService service {std::move(options)};
        UniqueDescriptor transport {
            descriptorFromName(service.takeConnectionName())};
        UniqueDescriptor input {
            descriptorFromName(service.takeInputConnectionName())};
        const SplitRing ring;
        publishDisplayInfoRequest(ring);
        negotiateProtocolFeatures(
            transport.get(), protocol::protocolFeatureReplyAck);
        configureMemory(transport.get(), ring.descriptor());
        const auto channels = startQueue(transport.get(), ring);

        uml::device::notify_guest_end();
        require(!service.requestsHeld());
        writeKick(channels.kick);
        awaitCall(channels.call.readEnd.get());
        protocol::GpuControlHeader response {};
        std::memcpy(
            &response,
            ring.memory().data() + displayInfoResponseOffset,
            sizeof(response));
        require(response.type == static_cast<std::uint32_t>(
            protocol::GpuResponse::okDisplayInfo));
        service.stop();
    }
    require(descriptorCount() == before);
}

/* The application can enter the background just after the guest said it is
 * ending, and a hold placed then holds a guest that can no longer stop: it
 * discards from the start. The next guest finds a plain hold. */
void aHoldAfterTheGuestEndedDiscards()
{
    const auto before = descriptorCount();
    {
        InProcessGpuOptions options;
        options.vncPort = 0;
        options.width = 64;
        options.height = 64;
        InProcessGpuService service {std::move(options)};
        {
            UniqueDescriptor transport {
                descriptorFromName(service.takeConnectionName())};
            UniqueDescriptor input {
                descriptorFromName(service.takeInputConnectionName())};
            uml::device::notify_guest_end();
            require(!service.requestsHeld());
            require(service.holdRequests(std::chrono::milliseconds {100}));
            require(service.requestsHeld());

            const SplitRing ring;
            FencedRequests requests {ring};
            requests.append(0x42);
            negotiateProtocolFeatures(
                transport.get(), protocol::protocolFeatureReplyAck);
            configureMemory(transport.get(), ring.descriptor());
            const auto channels = startQueue(transport.get(), ring);
            writeKick(channels.kick);
            awaitCall(channels.call.readEnd.get());
            require(ring.usedIndex() == 1);
            requests.requireAnswered(0, 0x42);
        }
        service.startNextConnection();
        require(service.requestsHeld());
        UniqueDescriptor transport {
            descriptorFromName(service.takeConnectionName())};
        UniqueDescriptor input {
            descriptorFromName(service.takeInputConnectionName())};
        heldRequestRunsAfterRelease(service, transport.get());
        service.stop();
    }
    require(descriptorCount() == before);
}

/* A connection that cannot be opened leaves the service as it says it does.
 * One that fails before the old connection is touched leaves that connection
 * in place; one that fails after leaves none, with its descriptors given
 * back, and the call can be made again. */
void failedNextConnectionLeavesTheServiceRecoverable()
{
    const auto before = descriptorCount();
    {
        InProcessGpuOptions options;
        options.vncPort = 0;
        options.width = 64;
        options.height = 64;
        InProcessGpuService service {std::move(options)};

        failNextStartup(StartupFailure::afterInputSocketPair);
        requireThrows<std::runtime_error>(
            [&] { service.startNextConnection(); });
        /* The connection that was there is untouched, endpoints included. */
        UniqueDescriptor transport {
            descriptorFromName(service.takeConnectionName())};
        UniqueDescriptor input {
            descriptorFromName(service.takeInputConnectionName())};
        require(::fcntl(transport.get(), F_GETFD) >= 0);

        for (const auto failure :
             {StartupFailure::afterTransportSocketPair,
              StartupFailure::workerEntry}) {
            failNextStartup(failure);
            requireThrows<std::runtime_error>(
                [&] { service.startNextConnection(); });
            requireThrows<std::logic_error>(
                [&] { (void)service.takeConnectionName(); });
            /* And it says why: there is no connection, not that its endpoint
             * went already. */
            try {
                (void)service.takeInputConnectionName();
                require(false, "a service without a connection has an input");
            } catch (const std::logic_error& error) {
                require(
                    std::string_view {error.what()}.find("no connection") !=
                        std::string_view::npos,
                    "the error does not say there is no connection");
            }
        }
        transport = {};
        input = {};

        service.startNextConnection();
        transport = UniqueDescriptor {
            descriptorFromName(service.takeConnectionName())};
        input = UniqueDescriptor {
            descriptorFromName(service.takeInputConnectionName())};
        require(service.holdRequests(std::chrono::milliseconds {100}));
        heldRequestRunsAfterRelease(service, transport.get());
        service.stop();
        /* A stopped service has no display to carry on. */
        requireThrows<std::logic_error>(
            [&] { service.startNextConnection(); });
    }
    require(descriptorCount() == before);
}

} // namespace

void runInProcessServiceTests()
{
    holdRequestsHoldsTheServicesRequests();
    displayStatisticsBelongToTheDisplay();
    nextConnectionServesTheNextGuest();
    theGuestEndingDiscardsTheHeldRequests();
    aGuestThatEndsBehindAnOpenGateRunsItsRequests();
    aHoldAfterTheGuestEndedDiscards();
    failedNextConnectionLeavesTheServiceRecoverable();
    guestTransportEndpointBlocks();
    forceRepeatedFailure(StartupFailure::afterTransportSocketPair);
    forceRepeatedFailure(StartupFailure::afterInputSocketPair);
    forceRepeatedFailure(StartupFailure::workerEntry);
    endpointsTransferExactlyOnce();
    transferredEndpointsAreNotClosedByService();
    stopWithoutEndpointTransferIsBalanced();
    partialFrameStopUsesOwnedWake(false);
    partialFrameStopUsesOwnedWake(true);
}

} // namespace uml::gpu::testing
