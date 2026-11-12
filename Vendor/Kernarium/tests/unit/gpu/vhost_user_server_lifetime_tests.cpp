// SPDX-License-Identifier: GPL-2.0-or-later

#include "fence_deadline.hpp"
#include "gpu_test_support.hpp"
#include "gpu_vhost_user_server.hpp"
#include "in_process_service_testing.hpp"
#include "vhost_user_message_testing.hpp"
#include "wait_until.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <pthread.h>
#include <sys/wait.h>

namespace uml::gpu::testing {
namespace {

void verifyControlRetry(VhostUserServer& server)
{
    withServerConnection(server, [](int socket) {
        require(getFeatures(socket) != 0);
    });
}

[[noreturn]] void runPartialFrameChild(
    bool partialPayload,
    bool carryDescriptor)
{
    int exitStatus = EXIT_SUCCESS;
    {
        auto server = makeTransportTestServer();
        // Compare against the live server stable ownership so a partial
        // transport frame cannot leak a descriptor into the next connection.
        const auto before = descriptorCount();
        {
            auto sockets = makeSocketPair();
            auto sent = carryDescriptor ?
                openTestDescriptor() : UniqueDescriptor {};
            std::atomic_bool stopRequested {};
            std::atomic_bool ready {};
            std::atomic_bool finished {};
            std::thread worker {[&] {
                server->runConnected(
                    sockets.descriptors[1].release(),
                    stopRequested,
                    [&] { ready.store(true, std::memory_order_release); });
                finished.store(true, std::memory_order_release);
            }};
            while (!ready.load(std::memory_order_acquire))
                std::this_thread::yield();

            const std::array descriptors {sent.get()};
            const std::span<const int> sentDescriptors = carryDescriptor ?
                std::span<const int> {descriptors} : std::span<const int> {};
            // The server reads a frame only once its socket is readable, so
            // it waits for more only after consuming the fragment below.
            testing::resetStopWaitObserver();
            writePartialFrame(
                sockets.descriptors[0].get(), partialPayload, sentDescriptors);

            if (!testing::waitForStopWaitCount(1, 2000))
                std::_Exit(102);
            stopRequested.store(true, std::memory_order_release);
            if (!::uml::testing::waitUntil(
                    [&] { return finished.load(std::memory_order_acquire); },
                    std::chrono::seconds {2},
                    std::chrono::milliseconds {10})) {
                std::_Exit(101);
            }
            worker.join();
        }
        verifyControlRetry(*server);
        const auto after = descriptorCount();
        if (after != before) {
            std::fprintf(
                stderr,
                "partial-frame child descriptor imbalance: %zu -> %zu\n",
                before,
                after);
            exitStatus = 102;
        }
    }
    std::_Exit(exitStatus);
}

void partialFrameStopIsPrompt(
    const char* executable,
    bool partialPayload,
    bool carryDescriptor)
{
    const auto before = descriptorCount();
    const char* mode = partialPayload ?
        (carryDescriptor ? "--partial-payload-fd-child"
                         : "--partial-payload-child") :
        (carryDescriptor ? "--partial-header-fd-child"
                         : "--partial-header-child");
    const pid_t child = launchChild(executable, mode);
    const int status = waitForChild(child, std::chrono::seconds {10});
    require(WIFEXITED(status));
    if (WEXITSTATUS(status) != EXIT_SUCCESS) {
        std::fprintf(
            stderr,
            "partial-frame child exited with status %d\n",
            WEXITSTATUS(status));
    }
    require(WEXITSTATUS(status) == EXIT_SUCCESS);
    require(descriptorCount() == before);
}

void unsupportedRequestReclaimsDescriptor()
{
    const auto before = descriptorCount();
    {
        const auto ownedServer = makeGpuVhostUserServer(
            "127.0.0.1", 0, {}, 64, 64, false);
        auto& server = *ownedServer;
        withServerConnection(server, [](int socket) {
            auto sent = openTestDescriptor();
            const protocol::VhostHeader header {
                .request = 0xffff'ffffU,
                .flags = protocol::vhostUserVersion,
                .size = 0,
            };
            const std::array descriptors {sent.get()};
            sendMessageWithDescriptors(socket, header, descriptors);
            waitForConnectionClose(socket);
        });
    }
    require(descriptorCount() == before);
}

void configureCallDescriptor(int socket, int descriptor)
{
    constexpr std::uint64_t queue = 0;
    const std::array descriptors {descriptor};
    sendRequestAndReadAcknowledgement(
        socket,
        protocol::VhostRequest::setVringCall,
        bytesOf(queue),
        descriptors);
}

void memoryReplacementReleasesQueueDescriptors()
{
    const auto before = descriptorCount();
    {
        const auto ownedServer = makeGpuVhostUserServer(
            "127.0.0.1", 0, {}, 64, 64, false);
        auto& server = *ownedServer;
        const ScratchBacking memory {memoryBackingName, 4096};
        std::size_t stableConfiguredCount = 0;
        withServerConnection(server, [&](int socket) {
            for (unsigned iteration = 0; iteration < 16; ++iteration) {
                configureMemory(socket, memory.descriptor());
                auto call = makePipe();
                configureCallDescriptor(socket, call.writeEnd.get());
                call.readEnd.reset();
                call.writeEnd.reset();
                const auto configuredCount = descriptorCount();
                if (iteration == 0)
                    stableConfiguredCount = configuredCount;
                require(configuredCount == stableConfiguredCount);
            }
        });
    }
    require(descriptorCount() == before);
}

void failedMemoryReplacementIsRetryable(
    std::uint64_t size,
    std::uint64_t mmapOffset,
    std::size_t backingSize = 4096)
{
    const auto before = descriptorCount();
    {
        const auto ownedServer = makeGpuVhostUserServer(
            "127.0.0.1", 0, {}, 64, 64, false);
        auto& server = *ownedServer;
        const ScratchBacking memory {memoryBackingName, backingSize};
        withServerConnection(server, [&](int socket) {
            configureMemory(socket, memory.descriptor());
            const protocol::MemoryRegion invalidRegion {
                .guestAddress = 0x1000,
                .size = size,
                .userAddress = 0x1000'0000,
                .mmapOffset = mmapOffset,
            };
            const auto payload =
                memoryTablePayload(std::span {&invalidRegion, 1U});
            const std::array descriptors {memory.descriptor()};
            sendRequest(
                socket,
                protocol::VhostRequest::setMemTable,
                payload,
                descriptors);
            waitForConnectionClose(socket);
        });
        withServerConnection(server, [&](int socket) {
            configureMemory(socket, memory.descriptor(), backingSize);
        });
    }
    require(descriptorCount() == before);
}

void twoRegionFailureRollsBackEveryOwner()
{
    const auto before = descriptorCount();
    {
        const auto ownedServer = makeGpuVhostUserServer(
            "127.0.0.1", 0, {}, 64, 64, false);
        auto& server = *ownedServer;
        const ScratchBacking first {memoryBackingName, 4096};
        const ScratchBacking shortSecond {memoryBackingName, 1024};
        const std::array regions {
            protocol::MemoryRegion {
                .guestAddress = 0x1000,
                .size = 4096,
                .userAddress = 0x1000'0000,
                .mmapOffset = 0,
            },
            protocol::MemoryRegion {
                .guestAddress = 0x3000,
                .size = 4096,
                .userAddress = 0x1000'2000,
                .mmapOffset = 0,
            },
        };
        const auto payload = memoryTablePayload(regions);
        const std::array descriptors {first.descriptor(), shortSecond.descriptor()};
        withServerConnection(server, [&](int socket) {
            sendRequest(
                socket,
                protocol::VhostRequest::setMemTable,
                payload,
                descriptors);
            waitForConnectionClose(socket);
        });
        withServerConnection(server, [&](int socket) {
            configureMemory(socket, first.descriptor());
        });
    }
    require(descriptorCount() == before);
}

std::atomic_uint interruptedWriterWaits;

void countInterruptedWriterWait(int)
{
    interruptedWriterWaits.fetch_add(1, std::memory_order_relaxed);
}

/* Sends GET_FEATURES requests whose replies the caller does not read until
 * the socket refuses more. The replies fill the server's direction first; the
 * server then waits to write and stops reading, and the requests fill the
 * other direction. A pass that adds nothing after a pause means the server
 * has stopped reading. Returns the number of requests sent. */
[[nodiscard]] std::size_t fillUntilTheServerWaitsToWrite(int socket)
{
    const protocol::VhostHeader header {
        .request = static_cast<std::uint32_t>(
            protocol::VhostRequest::getFeatures),
        .flags = protocol::vhostUserVersion,
        .size = 0,
    };
    std::size_t sent = 0;
    for (;;) {
        std::size_t pass = 0;
        for (;;) {
            const auto count = ::send(socket, &header, sizeof(header), 0);
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            require(count == static_cast<ssize_t>(sizeof(header)));
            ++pass;
        }
        sent += pass;
        if (pass == 0 && sent != 0)
            return sent;
        std::this_thread::sleep_for(std::chrono::milliseconds {200});
    }
}

/* A peer that stops reading leaves the server's replies on a full socket. The
 * reply write waits for room: an interrupted wait and a would-block write are
 * retried, every reply arrives whole and in order once the peer reads again,
 * and a stop request still ends a write that is waiting. */
void backpressuredRepliesArriveWholeAndStopEndsTheWait()
{
    const auto before = descriptorCount();
    {
        auto server = makeTransportTestServer();
        auto sockets = makeSocketPair();
        const int peer = sockets.descriptors[0].get();
        require(::fcntl(peer, F_SETNOSIGPIPE, 1) == 0);
        const int statusFlags = ::fcntl(peer, F_GETFL);
        require(statusFlags >= 0);
        std::atomic_bool stopRequested {};
        std::atomic_bool finished {};
        std::thread worker {[&, connection = sockets.descriptors[1].release()] {
            server->runConnected(connection, stopRequested);
            finished.store(true, std::memory_order_release);
        }};

        struct sigaction interrupt {};
        interrupt.sa_handler = countInterruptedWriterWait;
        sigemptyset(&interrupt.sa_mask);
        interrupt.sa_flags = 0;
        struct sigaction previous {};
        require(::sigaction(SIGUSR2, &interrupt, &previous) == 0);
        interruptedWriterWaits.store(0, std::memory_order_relaxed);

        require(::fcntl(peer, F_SETFL, statusFlags | O_NONBLOCK) == 0);
        const auto queued = fillUntilTheServerWaitsToWrite(peer);
        for (unsigned signal = 0; signal < 3; ++signal) {
            require(::pthread_kill(worker.native_handle(), SIGUSR2) == 0);
            std::this_thread::sleep_for(std::chrono::milliseconds {20});
        }
        require(::fcntl(peer, F_SETFL, statusFlags) == 0);
        std::uint64_t expectedFeatures = 0;
        for (std::size_t index = 0; index < queued; ++index) {
            const auto features = readValueReply<std::uint64_t>(
                peer, protocol::VhostRequest::getFeatures);
            require(features != 0);
            if (index == 0)
                expectedFeatures = features;
            require(features == expectedFeatures);
        }
        require(interruptedWriterWaits.load(std::memory_order_relaxed) == 3);
        require(::sigaction(SIGUSR2, &previous, nullptr) == 0);

        require(::fcntl(peer, F_SETFL, statusFlags | O_NONBLOCK) == 0);
        (void)fillUntilTheServerWaitsToWrite(peer);
        require(!finished.load(std::memory_order_acquire));
        stopRequested.store(true, std::memory_order_release);
        const bool stopped = ::uml::testing::waitUntil(
            [&] { return finished.load(std::memory_order_acquire); },
            std::chrono::seconds {5},
            std::chrono::milliseconds {10});
        (void)::shutdown(peer, SHUT_RDWR);
        worker.join();
        require(stopped);
    }
    require(descriptorCount() == before);
}

void concurrentRendererConstructionFailsAndLaterRetries()
{
    const auto before = descriptorCount();
    {
        const auto first = makeGpuVhostUserServer(
            "127.0.0.1", 0, {}, 64, 64, false);
        requireThrows<std::runtime_error>([] {
            (void)makeGpuVhostUserServer("127.0.0.1", 0, {}, 64, 64, false);
        });
    }
    require(descriptorCount() == before);
    {
        const auto retry = makeGpuVhostUserServer(
            "127.0.0.1", 0, {}, 64, 64, false);
    }
    require(descriptorCount() == before);
}

void configureQueueForOneRequest(
    int socket,
    const SplitRing& ring,
    int callDescriptor)
{
    publishDisplayInfoRequest(ring);
    configureQueue(socket, ring, 0);
    configureCallDescriptor(socket, callDescriptor);
}

[[noreturn]] void runClosedNotificationDescriptorChild()
{
    (void)::signal(SIGPIPE, SIG_DFL);
    auto server = makeTransportTestServer();
    {
        const SplitRing ring;
        withServerConnection(*server, [&](int socket) {
            constexpr std::uint64_t protocolFeatures =
                protocol::protocolFeatureInbandNotifications |
                protocol::protocolFeatureSlaveRequest |
                protocol::protocolFeatureReplyAck;
            negotiateProtocolFeatures(socket, protocolFeatures);
            configureMemory(socket, ring.descriptor());
            auto call = makePipe();
            configureQueueForOneRequest(socket, ring, call.writeEnd.get());
            call.readEnd.reset();
            call.writeEnd.reset();
            const protocol::VringState kick {.index = 0, .value = 0};
            sendRequest(
                socket,
                protocol::VhostRequest::vringKick,
                bytesOf(kick));
            readAcknowledgement(socket, protocol::VhostRequest::vringKick);
            waitForConnectionClose(socket);
        });
    }
    std::_Exit(EXIT_SUCCESS);
}

void closedNotificationDescriptorDoesNotSignalProcess(const char* executable)
{
    const pid_t child = launchChild(
        executable, "--closed-notification-child");

    int status = 0;
    require(::waitpid(child, &status, 0) == child);
    require(WIFEXITED(status));
    require(WEXITSTATUS(status) == EXIT_SUCCESS);
}

} // namespace

[[noreturn]] void runVhostUserServerChild(std::string_view mode)
{
    if (mode == "--closed-notification-child")
        runClosedNotificationDescriptorChild();
    if (mode == "--partial-header-child")
        runPartialFrameChild(false, false);
    if (mode == "--partial-header-fd-child")
        runPartialFrameChild(false, true);
    if (mode == "--partial-payload-child")
        runPartialFrameChild(true, false);
    if (mode == "--partial-payload-fd-child")
        runPartialFrameChild(true, true);
    std::_Exit(126);
}

/* A renderer fence that stays busy must never cost the guest its GPU device:
 * the signal it waits for can be sitting in a command that fence ordering is
 * holding back, so ordering is released before the renderer is given up on.
 * Tearing the connection down instead ends every workload in the session and
 * leaves the guest unable to shut down cleanly. */
void anOverdueFenceEscapesOrderingBeforeItIsRetired()
{
    const auto now = std::chrono::steady_clock::now();
    const auto later = now + std::chrono::seconds(1);

    require(classifyOverdueFence(now, later, false) ==
        OverdueFenceAction::wait);
    require(classifyOverdueFence(now, later, true) ==
        OverdueFenceAction::wait);

    /* First expiry releases ordering rather than retiring the fence, so a
     * held-back signal still has a chance to reach the renderer. */
    require(classifyOverdueFence(later, now, false) ==
        OverdueFenceAction::releaseOrdering);

    /* Only once ordering has already been released does the fence retire. */
    require(classifyOverdueFence(later, now, true) ==
        OverdueFenceAction::retire);

    /* The deadline itself is inclusive: reaching it is expiry, not waiting. */
    require(classifyOverdueFence(now, now, false) ==
        OverdueFenceAction::releaseOrdering);

    /* The escape is granted per fence, not per connection. Several fences
     * enqueued together expire together, so passing a connection-wide
     * "ordering is relaxed" flag here would retire every fence after the
     * first one immediately, without the release step or the second window
     * that the first fence received. Each fence carries its own bit. */
    struct Fence {
        std::chrono::steady_clock::time_point deadline;
        bool orderingReleased;
    };
    Fence fences[] = {
        {now, false},
        {now, false},
    };
    bool connectionRelaxed = false;
    for (auto& fence : fences) {
        const auto action = classifyOverdueFence(
            later, fence.deadline, fence.orderingReleased);
        require(action == OverdueFenceAction::releaseOrdering);
        fence.orderingReleased = true;
        connectionRelaxed = true;
    }
    require(connectionRelaxed);
    for (const auto& fence : fences) {
        require(classifyOverdueFence(later, fence.deadline,
                    fence.orderingReleased) == OverdueFenceAction::retire);
    }
}

/* One wedged submission costs its ring two windows and one report. A ring that
 * needs that again, and again, is not carrying a stuck submission: the
 * renderer has stopped answering for it, and every further fence buys the same
 * nothing at the same price. A Vulkan CTS fraction paid thirty seconds per
 * test case for 2,700 seconds and said nothing beyond the per-fence pair, so
 * what has to be watched is the ring's count, not the fence. */
void aRingThatStopsRetiringIsCountedAndRevivable()
{
    require(!ringHasStoppedRetiring(0));
    require(!ringHasStoppedRetiring(maximumRingEscapes - 1));
    require(ringHasStoppedRetiring(maximumRingEscapes));
    require(ringHasStoppedRetiring(maximumRingEscapes + 1));

    /* One wedged submission can never reach the count on its own: the escape
     * spends both of its windows on that one fence and retires it once. */
    require(maximumRingEscapes > 1);

    /* Model exactly what the drain loop keeps: every renderer-less retirement
     * raises the count, the ring is reported once as it crosses, and a ring
     * already known dead is neither counted again nor reported again. */
    unsigned consecutive = 0;
    unsigned reports = 0;
    const auto retiredWithoutTheRenderer = [&] {
        if (ringHasStoppedRetiring(consecutive))
            return;
        ++consecutive;
        if (ringHasStoppedRetiring(consecutive))
            ++reports;
    };
    for (unsigned escape = 0; escape < maximumRingEscapes * 4; ++escape)
        retiredWithoutTheRenderer();
    require(consecutive == maximumRingEscapes);
    require(reports == 1);

    /* A retirement the renderer does make clears the count, and the ring pays
     * the whole escape again before it can be called dead a second time. */
    consecutive = 0;
    require(!ringHasStoppedRetiring(consecutive));
    for (unsigned escape = 0; escape + 1 < maximumRingEscapes; ++escape)
        retiredWithoutTheRenderer();
    require(!ringHasStoppedRetiring(consecutive));
    require(reports == 1);
    retiredWithoutTheRenderer();
    require(ringHasStoppedRetiring(consecutive));
    require(reports == 2);
}

void runVhostUserServerTests(const char* executable)
{
    // Metal's compiler service is not a supported post-fork initialization
    // path. Exec each isolated child before it constructs the production
    // renderer, preserving process-signal/watchdog coverage without inheriting
    // framework or dispatch state.
    closedNotificationDescriptorDoesNotSignalProcess(executable);
    partialFrameStopIsPrompt(executable, false, false);
    partialFrameStopIsPrompt(executable, true, false);
    partialFrameStopIsPrompt(executable, false, true);
    partialFrameStopIsPrompt(executable, true, true);
    {
        const auto warmRuntime = makeGpuVhostUserServer(
            "127.0.0.1", 0, {}, 64, 64, false);
    }
    concurrentRendererConstructionFailsAndLaterRetries();
    unsupportedRequestReclaimsDescriptor();
    memoryReplacementReleasesQueueDescriptors();
    failedMemoryReplacementIsRetryable(0, 0);
    failedMemoryReplacementIsRetryable(4096, UINT64_MAX);
    failedMemoryReplacementIsRetryable(8192, 0);
    twoRegionFailureRollsBackEveryOwner();
    backpressuredRepliesArriveWholeAndStopEndsTheWait();
    anOverdueFenceEscapesOrderingBeforeItIsRetired();
    aRingThatStopsRetiringIsCountedAndRevivable();
}

} // namespace uml::gpu::testing
