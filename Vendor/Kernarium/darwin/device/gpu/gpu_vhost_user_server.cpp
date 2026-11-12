// SPDX-License-Identifier: GPL-2.0-or-later

#include "gpu_vhost_user_server.hpp"

#include "deferred_fences.hpp"
#include "platform/descriptor.hpp"
#include "protocol.hpp"
#include "unique_descriptor.hpp"
#include "vhost_user_device.hpp"
#include "virtio_gpu_device.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/event.h>
#include <utility>
#include <vector>

namespace uml::gpu {
namespace {

/* The kqueue's identifier for the timer that trims idle guest contexts. The
 * fence timer's is deferred_fences.hpp's. */
constexpr std::uintptr_t idleTrimTimerIdentifier = 2;
/* The timers of the memory report: a change of graphics memory arms the first
 * for a short coalescing delay on whichever thread made it, and a refused
 * allocation for a millisecond; the second is armed on this thread for the
 * moment a sustained pressure, a calm or a held-back refusal event is due. */
constexpr std::uintptr_t memoryTickTimerIdentifier = 3;
constexpr std::uintptr_t memoryDeadlineTimerIdentifier = 4;
constexpr std::int64_t memoryTickMicroseconds = 250'000;
constexpr std::int64_t memoryRefusalTickMicroseconds = 1'000;

template<typename Value>
Value loadUnaligned(const void* address)
{
    Value value {};
    std::memcpy(&value, address, sizeof(value));
    return value;
}

class GpuVhostUserDevice final : public uml::virtio::VhostUserDevice {
public:
    GpuVhostUserDevice(
        std::shared_ptr<Display> display,
        std::string vncBindAddress,
        std::uint32_t dotsPerInch)
        : sharedDisplay_(std::move(display))
        , display_(*sharedDisplay_)
        , vncBindAddress_(std::move(vncBindAddress))
        , gpu_(
              display_,
              [this](
                  std::uint64_t address,
                  std::span<std::uint8_t> destination) {
                  return readGuest(address, destination);
              },
              [this](std::uint64_t address, std::size_t length) {
                  return mapper_(address, length);
              },
              dotsPerInch)
        , hasEdid_(dotsPerInch != 0)
        , eventQueue_(::kqueue())
    {
        if (eventQueue_.get() < 0)
            throw std::runtime_error("creating GPU event queue failed");
        uml::platform::configureDescriptorOrThrow(
            eventQueue_.get(),
            {.closeOnExec = true},
            "close-on-exec descriptor");
        addFenceTimer(eventQueue_.get());
        const int resizeDescriptor = display_.resizeEventDescriptor();
        if (resizeDescriptor >= 0) {
            struct kevent change {};
            EV_SET(
                &change,
                static_cast<std::uintptr_t>(resizeDescriptor),
                EVFILT_READ,
                EV_ADD | EV_ENABLE,
                0,
                0,
                nullptr);
            int result;
            do {
                result = ::kevent(
                    eventQueue_.get(), &change, 1, nullptr, 0, nullptr);
            } while (result < 0 && errno == EINTR);
            if (result < 0) {
                throw std::runtime_error(
                    "registering GPU display-resize event failed");
            }
        }
        gpu_.setMemoryWake([this](VirtioGpuDevice::MemoryWake wake) {
            armMemoryTick(wake == VirtioGpuDevice::MemoryWake::immediate);
        });
    }

    /* The ledger's observer reaches this device's queue from other threads,
     * so it goes before the queue does. */
    ~GpuVhostUserDevice() override { gpu_.setMemoryWake({}); }

    [[nodiscard]] const char* name() const noexcept override
    {
        return "Virtio-GPU";
    }

    [[nodiscard]] std::uint64_t features() const noexcept override
    {
        return protocol::virtioGpuFeatureVirgl |
            (hasEdid_ ? protocol::virtioGpuFeatureEdid : 0U) |
            protocol::virtioGpuFeatureResourceBlob |
            protocol::virtioGpuFeatureContextInit |
            (gpu_.offersMemoryReport()
                    ? protocol::virtioGpuFeatureUmlMemoryReport
                    : 0U);
    }

    [[nodiscard]] std::size_t queueCount() const noexcept override
    {
        return protocol::gpuQueueCount;
    }

    [[nodiscard]] std::vector<std::uint8_t> configuration() const override
    {
        const auto config = gpu_.config();
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&config);
        return {bytes, bytes + sizeof(config)};
    }

    void configurationRead(
        std::uint32_t offset,
        std::uint32_t size) noexcept override
    {
        gpu_.recordConfigRead(offset, size);
    }

    void writeConfiguration(
        std::uint32_t offset,
        std::uint32_t flags,
        std::span<const std::uint8_t> bytes) override
    {
        constexpr std::uint32_t configSize = sizeof(protocol::GpuConfig);
        constexpr std::uint32_t eventsClearOffset =
            offsetof(protocol::GpuConfig, eventsClear);
        constexpr std::uint32_t eventsClearEnd =
            eventsClearOffset + sizeof(protocol::GpuConfig::eventsClear);
        const bool validFlag =
            flags == protocol::configFlagWritable ||
            flags == protocol::configFlagMigration;
        const bool validRange =
            !bytes.empty() && offset <= configSize &&
            bytes.size() <= configSize - offset;
        const bool writableRange =
            offset >= eventsClearOffset && offset <= eventsClearEnd &&
            bytes.size() <= eventsClearEnd - offset;
        if (!validFlag || !validRange ||
            (flags == protocol::configFlagWritable && !writableRange)) {
            throw std::runtime_error("invalid Virtio-GPU config write");
        }
        if (flags == protocol::configFlagWritable) {
            std::uint32_t clearEvents = 0;
            const auto relativeOffset =
                static_cast<std::size_t>(offset - eventsClearOffset);
            std::memcpy(
                reinterpret_cast<std::uint8_t*>(&clearEvents) + relativeOffset,
                bytes.data(),
                bytes.size());
            gpu_.clearEvents(clearEvents);
            if ((clearEvents & protocol::gpuEventDisplay) != 0 &&
                gpu_.displayEventPending()) {
                configChangePending_ = true;
            }
            if ((clearEvents & protocol::gpuEventMemory) != 0 &&
                gpu_.memoryEventPending()) {
                memoryChangePending_ = true;
            }
        }
    }

    [[nodiscard]] uml::virtio::VhostUserQueueResult processQueue(
        std::uint32_t index,
        std::uint64_t requestId,
        std::span<const std::uint8_t> request,
        std::size_t responseCapacity) override
    {
        const bool cursorQueue = index == protocol::gpuCursorQueue;
        /* A control request with no room even for an error header does not
         * run: it completes with nothing written. */
        if (!cursorQueue &&
            responseCapacity < sizeof(protocol::GpuControlHeader)) {
            return {.action = uml::virtio::VhostUserQueueAction::complete};
        }
        if (!fences_.empty() && !fences_.orderingRelaxed() &&
            !gpu_.canProcessWhileFencesPending(request, cursorQueue)) {
            return {.action = uml::virtio::VhostUserQueueAction::retry};
        }

        const bool mayDefer = canDeferFence(cursorQueue, request);
        if (mayDefer && fences_.full())
            return {.action = uml::virtio::VhostUserQueueAction::retry};
        /* Holding the command once the device has run it must not fail. */
        if (mayDefer)
            fences_.reserveDefer();
        if (mayDefer && fences_.empty())
            armFenceTimer(eventQueue_.get());

        auto result = gpu_.processWithDeferredFence(
            request, cursorQueue, requestId, responseCapacity);
        if (isMemoryQuery(cursorQueue, request))
            armMemoryDeadline();
        if (result.destroyedContextId) {
            /* The context's held commands complete on the next tick. */
            const bool held = !fences_.empty();
            fences_.forgetContext(*result.destroyedContextId);
            if (held)
                armFenceTimer(eventQueue_.get());
        }
        if (result.fence) {
            if (!mayDefer)
                throw std::runtime_error("invalid deferred GPU fence");
            fences_.defer(
                requestId,
                loadUnaligned<protocol::GpuControlHeader>(request.data()),
                std::move(result.response),
                *result.fence,
                DeferredFences::Clock::now());
            armIdleTrimTimer();
            return {.action = uml::virtio::VhostUserQueueAction::defer};
        }
        if (fences_.empty() && gpu_.rendererHasPendingWork())
            armFenceTimer(eventQueue_.get());
        armIdleTrimTimer();
        return {
            .action = uml::virtio::VhostUserQueueAction::complete,
            .response = std::move(result.response),
        };
    }

    [[nodiscard]] bool keepsRequestsOnDisabledRing() const noexcept override
    {
        return true;
    }

    // A submission can run for hundreds of milliseconds on the host.
    [[nodiscard]] bool answersKicksBeforeRunning() const noexcept override
    {
        return true;
    }

    [[nodiscard]] std::vector<std::uint8_t> discardedResponse(
        std::uint32_t index,
        std::span<const std::uint8_t> request) const override
    {
        return gpu_.discardedResponse(
            request, index == protocol::gpuCursorQueue);
    }

    void reset() noexcept override
    {
        cancelFenceTimer(eventQueue_.get());
        fences_.clear();
        gpu_.reset();
        configChangePending_ = false;
        memoryChangePending_ = false;
    }

    [[nodiscard]] int eventDescriptor() const noexcept override
    {
        return eventQueue_.get();
    }

    void drainEventSource() override
    {
        std::array<struct kevent, 5> events {};
        const timespec timeout {};
        int eventCount;
        do {
            eventCount = ::kevent(
                eventQueue_.get(),
                nullptr,
                0,
                events.data(),
                static_cast<int>(events.size()),
                &timeout);
        } while (eventCount < 0 && errno == EINTR);
        if (eventCount < 0)
            throw std::runtime_error("draining GPU events failed");

        bool resizeReady = false;
        bool fenceTimerReady = false;
        bool idleTrimTimerReady = false;
        bool memoryTickReady = false;
        bool memoryDeadlineReady = false;
        const int resizeDescriptor = display_.resizeEventDescriptor();
        for (int index = 0; index < eventCount; ++index) {
            const auto& event = events[static_cast<std::size_t>(index)];
            if ((event.flags & EV_ERROR) != 0)
                throw std::runtime_error("GPU event source failed");
            if (event.filter == EVFILT_TIMER &&
                event.ident == fenceTimerIdentifier) {
                fenceTimerReady = true;
                continue;
            }
            if (event.filter == EVFILT_TIMER &&
                event.ident == idleTrimTimerIdentifier) {
                idleTrimTimerReady = true;
                continue;
            }
            if (event.filter == EVFILT_TIMER &&
                event.ident == memoryTickTimerIdentifier) {
                memoryTickReady = true;
                continue;
            }
            if (event.filter == EVFILT_TIMER &&
                event.ident == memoryDeadlineTimerIdentifier) {
                memoryDeadlineReady = true;
                continue;
            }
            if (resizeDescriptor >= 0 && event.filter == EVFILT_READ &&
                event.ident ==
                    static_cast<std::uintptr_t>(resizeDescriptor)) {
                resizeReady = true;
                continue;
            }
            throw std::runtime_error("unexpected GPU event source");
        }

        if (resizeReady && display_.drainResizeEvent()) {
            gpu_.notifyDisplayChanged();
            configChangePending_ = true;
        }
        if (memoryTickReady || memoryDeadlineReady) {
            /* Disarmed before the figures are read, so a change that follows
             * arms the next evaluation. */
            if (memoryTickReady)
                memoryTickArmed_.store(false, std::memory_order_release);
            if (gpu_.evaluateMemory(std::chrono::steady_clock::now()))
                memoryChangePending_ = true;
            armMemoryDeadline();
        }
        if (idleTrimTimerReady) {
            /* A one-shot timer is gone once it has fired. */
            idleTrimTimerArmed_ = false;
            gpu_.trimIdleContexts();
            armIdleTrimTimer();
        }
        if (!fenceTimerReady)
            return;
        if (fences_.empty()) {
            /* A guest thread blocked on a query result spins on the buffer
             * the renderer fills and issues no further commands, so no fence
             * of its own is left to drive the scan that completes the query.
             * Keep polling for as long as the renderer owns such work. */
            if (!gpu_.rendererHasPendingWork())
                return;
            if (gpu_.pollRenderer() != 0)
                throw std::runtime_error("polling GPU fences failed");
            if (gpu_.rendererHasPendingWork())
                armFenceTimer(eventQueue_.get());
            return;
        }

        /* A Virgl context retires through the renderer-wide fence scan, so
         * one poll drains every Virgl context. A Venus context owns its own
         * render-server timeline and retires only when it is polled itself,
         * so each distinct pending context needs its own poll. */
        fences_.forEachContext([this](std::uint32_t contextId) {
            if (gpu_.pollContextFences(contextId) != 0)
                throw std::runtime_error("polling GPU fences failed");
        });
        fences_.retire(
            DeferredFences::Clock::now(),
            [this](const DeferredFence& fence) {
                return gpu_.contextFenceStatus(fence);
            });
        if (!fences_.empty() || gpu_.rendererHasPendingWork())
            armFenceTimer(eventQueue_.get());
    }

    [[nodiscard]] bool takeQueueCompletion(
        uml::virtio::VhostUserQueueCompletion& completion) override
    {
        DeferredFences::Completion completed;
        if (!fences_.takeCompletion(completed))
            return false;
        completion = {
            .requestId = completed.requestId,
            .response = std::move(completed.response),
        };
        return true;
    }

    [[nodiscard]] bool configChangePending() const noexcept override
    {
        return configChangePending_ || memoryChangePending_;
    }

    /* A display change waits for the display's resize lifecycle; a memory
     * change does not. What a claim covers is what the acknowledgement
     * clears, so a display change the lifecycle held back stays pending. */
    [[nodiscard]] bool claimConfigChange() noexcept override
    {
        displayLocked_ =
            configChangePending_ && display_.claimResizeNotification();
        deliveredDisplay_ = displayLocked_;
        deliveredMemory_ = memoryChangePending_;
        return deliveredDisplay_ || deliveredMemory_;
    }

    void releaseConfigChange() noexcept override
    {
        if (displayLocked_) {
            displayLocked_ = false;
            display_.releaseResizeNotification();
        }
    }

    void clearConfigChange() noexcept override
    {
        if (deliveredDisplay_)
            configChangePending_ = false;
        if (deliveredMemory_)
            memoryChangePending_ = false;
        deliveredDisplay_ = false;
        deliveredMemory_ = false;
    }

    void prepareReset() noexcept override
    {
        gpu_.prepareReset();
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
        uml::virtio::VhostUserSharedMemoryAction& destination) override
    {
        auto action = gpu_.takeSharedMemoryAction();
        if (!action)
            return false;
        destination = uml::virtio::VhostUserSharedMemoryAction {
            .type = action->type ==
                    VirtioGpuDevice::SharedMemoryAction::Type::map
                ? uml::virtio::VhostUserSharedMemoryAction::Type::map
                : uml::virtio::VhostUserSharedMemoryAction::Type::unmap,
            .actionId = action->actionId,
            .regionId = action->regionId,
            .descriptorOffset = action->descriptorOffset,
            .regionOffset = action->regionOffset,
            .length = action->length,
            .writable = action->writable,
            .descriptor = std::move(action->descriptor),
        };
        return true;
    }

    void completeSharedMemoryAction(
        std::uint64_t actionId,
        bool success) noexcept override
    {
        gpu_.completeSharedMemoryAction(actionId, success);
    }

    void setStopRequest(
        const std::atomic_bool* stopRequested) noexcept override
    {
        gpu_.setStopRequest(stopRequested);
    }

    void setGuestMapper(GuestMapper mapper) override
    {
        mapper_ = std::move(mapper);
    }

    [[nodiscard]] std::string readinessDescription() const override
    {
        const auto size = std::to_string(display_.width()) + "x" +
            std::to_string(display_.height());
        if (const auto port = display_.vncPort()) {
            return "VNC display: " + vncBindAddress_ + ":" +
                std::to_string(static_cast<unsigned int>(*port)) + " (" +
                size + ")\n";
        }
        return "Native Metal display: " + size + "\n";
    }

private:
    /* Arms the evaluation of the memory report, unless it is armed already.
     * Any thread: the ledger's observer calls it when graphics memory changes
     * while the guest is subscribed. A burst of changes costs one evaluation,
     * a quarter of a second after the first. A refusal (`immediate`) moves
     * the evaluation to now, armed or not. */
    void armMemoryTick(bool immediate) noexcept
    {
        if (immediate) {
            memoryTickArmed_.store(true, std::memory_order_release);
            if (!armOneShot(
                    memoryTickTimerIdentifier, memoryRefusalTickMicroseconds))
                memoryTickArmed_.store(false, std::memory_order_release);
            return;
        }
        if (memoryTickArmed_.exchange(true, std::memory_order_acq_rel))
            return;
        if (!armOneShot(memoryTickTimerIdentifier, memoryTickMicroseconds))
            memoryTickArmed_.store(false, std::memory_order_release);
    }

    /* Arms the timer for the moment the memory report's evaluator next has
     * something due, or leaves it alone when nothing is. Transport thread. */
    void armMemoryDeadline() noexcept
    {
        const auto deadline = gpu_.memoryDeadline();
        if (!deadline)
            return;
        constexpr std::int64_t shortest = 1000;
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            *deadline - std::chrono::steady_clock::now());
        (void)armOneShot(
            memoryDeadlineTimerIdentifier,
            std::max<std::int64_t>(shortest, remaining.count() + shortest));
    }

    [[nodiscard]] bool armOneShot(
        std::uintptr_t identifier, std::int64_t microseconds) noexcept
    {
        struct kevent change {};
        EV_SET(
            &change,
            identifier,
            EVFILT_TIMER,
            EV_ADD | EV_ENABLE | EV_ONESHOT,
            NOTE_USECONDS,
            static_cast<std::intptr_t>(microseconds),
            nullptr);
        int result;
        do {
            result = ::kevent(
                eventQueue_.get(), &change, 1, nullptr, 0, nullptr);
        } while (result < 0 && errno == EINTR);
        return result == 0;
    }

    [[nodiscard]] static bool isMemoryQuery(
        bool cursorQueue, std::span<const std::uint8_t> request) noexcept
    {
        if (cursorQueue || request.size() < sizeof(protocol::GpuControlHeader))
            return false;
        return loadUnaligned<protocol::GpuControlHeader>(request.data()).type ==
            static_cast<std::uint32_t>(protocol::GpuCommand::umlGetMemory);
    }

    /* Arms the timer that trims the guest contexts that have gone quiet, for
     * the earliest moment one could be, unless it is armed already: a timer
     * that fires early only finds nothing due and arms the next. Trimming is a
     * courtesy to the host's memory, so a queue that cannot take the timer
     * costs the trim and nothing else. */
    void armIdleTrimTimer() noexcept
    {
        if (idleTrimTimerArmed_)
            return;
        const auto delay = gpu_.nextIdleTrimDelay();
        if (!delay)
            return;
        constexpr std::int64_t shortest = 1000;
        const auto microseconds = std::max<std::int64_t>(
            shortest,
            std::chrono::duration_cast<std::chrono::microseconds>(*delay)
                    .count() +
                shortest);
        struct kevent change {};
        EV_SET(
            &change,
            idleTrimTimerIdentifier,
            EVFILT_TIMER,
            EV_ADD | EV_ENABLE | EV_ONESHOT,
            NOTE_USECONDS,
            static_cast<std::intptr_t>(microseconds),
            nullptr);
        int result;
        do {
            result = ::kevent(
                eventQueue_.get(), &change, 1, nullptr, 0, nullptr);
        } while (result < 0 && errno == EINTR);
        idleTrimTimerArmed_ = result == 0;
    }

    /* The transport's own translation: a range that one mapped region holds
     * whole, copied out. */
    [[nodiscard]] bool readGuest(
        std::uint64_t address,
        std::span<std::uint8_t> destination)
    {
        const auto source = mapper_(address, destination.size());
        if (source.data() == nullptr)
            return false;
        std::memcpy(destination.data(), source.data(), destination.size());
        return true;
    }

    [[nodiscard]] bool canDeferFence(
        bool cursorQueue,
        std::span<const std::uint8_t> request) const noexcept
    {
        if (cursorQueue || request.size() < sizeof(protocol::GpuControlHeader))
            return false;
        const auto header =
            loadUnaligned<protocol::GpuControlHeader>(request.data());
        const auto command = static_cast<protocol::GpuCommand>(header.type);
        const bool fence = (header.flags & protocol::gpuFlagFence) != 0;
        const bool pipelineCommand =
            command == protocol::GpuCommand::submit3d ||
            command == protocol::GpuCommand::transferToHost3d ||
            command == protocol::GpuCommand::transferFromHost3d;
        /* Virgl retires one fence timeline per context and Venus one per
         * guest ring. The device knows each context's capset, so it decides
         * which rings defer and completes the rest synchronously. */
        return pipelineCommand && fence && header.contextId != 0;
    }

    /* The display may outlive this device: a service that serves one guest
     * after another keeps it, and each device only borrows it. It is declared
     * before the device so that the device resets it before it can go. */
    std::shared_ptr<Display> sharedDisplay_;
    Display& display_;
    std::string vncBindAddress_;
    GuestMapper mapper_;
    VirtioGpuDevice gpu_;
    bool hasEdid_;
    bool configChangePending_ {};
    /* The memory event is pending, and what the claim now in flight covers. */
    bool memoryChangePending_ {};
    bool displayLocked_ {};
    bool deliveredDisplay_ {};
    bool deliveredMemory_ {};
    std::atomic_bool memoryTickArmed_ {};
    bool idleTrimTimerArmed_ {};
    UniqueDescriptor eventQueue_;
    DeferredFences fences_;
};

} // namespace

std::unique_ptr<VhostUserServer> makeGpuVhostUserServer(
    std::shared_ptr<Display> display,
    std::string vncBindAddress,
    std::uint32_t dotsPerInch,
    bool selfTestPattern)
{
    if (!display)
        throw std::invalid_argument("Virtio-GPU server requires a display");
    auto& presented = *display;
    auto server = std::make_unique<VhostUserServer>(
        std::make_unique<GpuVhostUserDevice>(
            std::move(display), std::move(vncBindAddress), dotsPerInch));
    if (selfTestPattern)
        presented.fillSelfTestPattern();
    return server;
}

std::unique_ptr<VhostUserServer> makeGpuVhostUserServer(
    std::string vncBindAddress,
    std::uint16_t vncPort,
    std::string vncPassword,
    std::uint32_t width,
    std::uint32_t height,
    bool selfTestPattern,
    uml::input::vnc::VncInputSink inputSink,
    DisplayMode displayMode,
    void* nativeView,
    std::uint32_t dotsPerInch,
    uml::input::vnc::PointerMode pointerMode,
    bool autoResizeGuest,
    OffscreenDisplayOptions offscreen)
{
    std::shared_ptr<Display> display = makeGpuDisplay(
        vncBindAddress,
        vncPort,
        std::move(vncPassword),
        width,
        height,
        std::move(inputSink),
        displayMode,
        nativeView,
        dotsPerInch,
        pointerMode,
        autoResizeGuest,
        offscreen);
    return makeGpuVhostUserServer(
        std::move(display),
        std::move(vncBindAddress),
        dotsPerInch,
        selfTestPattern);
}

std::unique_ptr<Display> makeGpuDisplay(
    const std::string& vncBindAddress,
    std::uint16_t vncPort,
    std::string vncPassword,
    std::uint32_t width,
    std::uint32_t height,
    uml::input::vnc::VncInputSink inputSink,
    DisplayMode displayMode,
    void* nativeView,
    std::uint32_t dotsPerInch,
    uml::input::vnc::PointerMode pointerMode,
    bool autoResizeGuest,
    OffscreenDisplayOptions offscreen)
{
    return makeDisplay(
        displayMode,
        vncBindAddress,
        vncPort,
        std::move(vncPassword),
        width,
        height,
        std::move(inputSink),
        nativeView,
        pointerMode,
        dotsPerInch == 0 ?
            protocol::maximumDisplayDimension :
            protocol::maximumEdidDimension,
        autoResizeGuest,
        offscreen);
}

} // namespace uml::gpu
