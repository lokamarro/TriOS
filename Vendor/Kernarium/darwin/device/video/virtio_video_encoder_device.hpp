// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "vhost_user_device.hpp"
#include "video_encoder.hpp"

#include <functional>
#include <memory>
#include <vector>

namespace uml::video {

inline constexpr std::uint32_t encoderDeviceId = 30;

/* The Virtio video encoder, device ID 30: a stateful V4L2 encoder in front of
 * the platform's hardware encoder. It owns command validation, stream state,
 * guest buffer mappings, the V4L2 control table and the translation of a
 * stream's controls into encoder settings; the encoding itself belongs to
 * `uml::platform::VideoEncoder`. All of its methods run on the transport
 * worker, which is also the only thread that touches guest memory. */
class VirtioVideoEncoderDevice final : public uml::virtio::VhostUserDevice {
public:
    using EncoderFactory = std::function<std::unique_ptr<uml::platform::VideoEncoder>()>;
    VirtioVideoEncoderDevice(std::vector<uml::platform::VideoEncodeCapability> capabilities,
                             EncoderFactory factory);
    ~VirtioVideoEncoderDevice() override;
    const char* name() const noexcept override;
    std::uint64_t features() const noexcept override;
    std::size_t queueCount() const noexcept override;
    std::vector<std::uint8_t> configuration() const override;
    void writeConfiguration(std::uint32_t, std::uint32_t, std::span<const std::uint8_t>) override;
    void setGuestMapper(GuestMapper mapper) override;
    void setSharedMemoryRegions(
        std::shared_ptr<const uml::virtio::SharedMemoryRegions> regions) override;
    uml::virtio::VhostUserQueueResult processQueue(std::uint32_t index, std::uint64_t requestId,
        std::span<const std::uint8_t> request, std::size_t responseCapacity) override;
    bool takeQueueCompletion(uml::virtio::VhostUserQueueCompletion&) override;
    void reset() noexcept override;
    /* Readable when any stream's encoder has output to take. */
    int eventDescriptor() const noexcept override;
    void drainEventSource() override;

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace uml::video
