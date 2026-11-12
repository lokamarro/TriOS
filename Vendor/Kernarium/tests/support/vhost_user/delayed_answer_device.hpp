// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "vhost_user_device.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace uml::testing {

/*
 * Test support: a vhost-user device that answers chosen requests late.
 *
 * It forwards everything to the device it wraps, which answers as it always
 * does, but it can hold back an answer the device gave at once and deliver it
 * through the transport later, as a device that queues its work may. A
 * device's reset drops every answer still held, as the transport drops the
 * requests themselves.
 *
 * The policy sees every request before the device does, on the transport's
 * thread, and returns how long to hold back its answer: nothing answers it at
 * once, a duration delays it, and `never` holds it until the device is reset.
 * A request the device itself defers is never held back. The log, when given,
 * hears of every answer held, delivered late, or dropped.
 *
 * An answerer, when given, sees each request first and may answer it in the
 * device's place: the wrapped device then never hears of the request, and the
 * answer goes back at once. That reaches the driver's paths for an answer the
 * device would give only when its own work fails.
 */
class DelayedAnswerDevice final : public uml::virtio::VhostUserDevice {
public:
    static constexpr std::chrono::milliseconds never {
        std::chrono::milliseconds::max()};
    using Policy = std::function<std::optional<std::chrono::milliseconds>(
        std::uint32_t queue, std::span<const std::uint8_t> request)>;
    using Log = std::function<void(std::string_view)>;
    using Answerer = std::function<std::optional<std::vector<std::uint8_t>>(
        std::uint32_t queue, std::span<const std::uint8_t> request)>;

    DelayedAnswerDevice(std::unique_ptr<uml::virtio::VhostUserDevice> device,
                        Policy policy, Log log = {}, Answerer answerer = {});
    ~DelayedAnswerDevice() override;

    DelayedAnswerDevice(const DelayedAnswerDevice&) = delete;
    DelayedAnswerDevice& operator=(const DelayedAnswerDevice&) = delete;

    [[nodiscard]] const char* name() const noexcept override;
    [[nodiscard]] std::uint64_t features() const noexcept override;
    [[nodiscard]] std::size_t queueCount() const noexcept override;
    [[nodiscard]] std::vector<std::uint8_t> configuration() const override;
    void configurationRead(std::uint32_t offset,
                           std::uint32_t length) noexcept override;
    void writeConfiguration(std::uint32_t offset, std::uint32_t flags,
                            std::span<const std::uint8_t> bytes) override;
    [[nodiscard]] uml::virtio::VhostUserQueueResult processQueue(
        std::uint32_t index, std::uint64_t requestId,
        std::span<const std::uint8_t> request,
        std::size_t responseCapacity) override;
    void reset() noexcept override;
    void prepareReset() noexcept override;
    void setGuestMapper(GuestMapper mapper) override;
    void setSharedMemoryRegions(
        std::shared_ptr<const uml::virtio::SharedMemoryRegions>
            regions) override;
    [[nodiscard]] std::optional<uml::virtio::VhostUserSharedMemoryRegion>
        sharedMemoryRegion() const noexcept override;
    [[nodiscard]] bool takeSharedMemoryAction(
        uml::virtio::VhostUserSharedMemoryAction& action) override;
    void completeSharedMemoryAction(std::uint64_t actionId,
                                    bool succeeded) noexcept override;
    /* Readable when a held answer falls due or the wrapped device's own
     * event source is. */
    [[nodiscard]] int eventDescriptor() const noexcept override;
    void drainEventSource() override;
    [[nodiscard]] bool takeQueueCompletion(
        uml::virtio::VhostUserQueueCompletion& completion) override;
    [[nodiscard]] bool configChangePending() const noexcept override;
    [[nodiscard]] bool claimConfigChange() noexcept override;
    void releaseConfigChange() noexcept override;
    void clearConfigChange() noexcept override;
    void setStopRequest(const std::atomic_bool* stop) noexcept override;
    [[nodiscard]] std::string readinessDescription() const override;

private:
    struct Held {
        std::optional<std::chrono::steady_clock::time_point> due;
        uml::virtio::VhostUserQueueCompletion completion;
        bool signalled {};
    };

    void runTimer();
    void note(std::string_view message) const;

    std::unique_ptr<uml::virtio::VhostUserDevice> device_;
    Policy policy_;
    Log log_;
    Answerer answerer_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Held> held_;
    bool stopping_ {};
    int wake_[2] {-1, -1};
    int events_ {-1};
    std::thread timer_;
};

} // namespace uml::testing
