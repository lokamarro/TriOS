// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "shared_memory_regions.hpp"
#include "unique_descriptor.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace uml::virtio {

enum class VhostUserQueueAction : std::uint8_t {
    retry,
    complete,
    defer,
};

struct VhostUserQueueResult final {
    VhostUserQueueAction action {VhostUserQueueAction::retry};
    std::vector<std::uint8_t> response;
};

struct VhostUserQueueCompletion final {
    std::uint64_t requestId {};
    std::vector<std::uint8_t> response;
};

/* The bytes of trivially copyable values laid end to end, as a response or
 * a configuration carries them. */
template<typename... Values>
[[nodiscard]] std::vector<std::uint8_t> toBytes(const Values&... values)
{
    static_assert((std::is_trivially_copyable_v<Values> && ...));
    std::vector<std::uint8_t> result((sizeof(Values) + ... + 0U));
    std::size_t offset = 0;
    ((std::memcpy(result.data() + offset, &values, sizeof(Values)),
      offset += sizeof(Values)),
     ...);
    return result;
}

/* The trivially copyable value at the start of `bytes`. The caller has
 * checked that `bytes` holds at least sizeof(Value). */
template<typename Value>
[[nodiscard]] Value fromBytes(std::span<const std::uint8_t> bytes)
{
    static_assert(std::is_trivially_copyable_v<Value>);
    Value value {};
    std::memcpy(&value, bytes.data(), sizeof(value));
    return value;
}

struct VhostUserSharedMemoryRegion final {
    std::uint8_t id {};
    std::uint64_t size {};
};

struct VhostUserSharedMemoryAction final {
    enum class Type : std::uint8_t {
        map,
        unmap,
    };

    Type type {};
    std::uint64_t actionId {};
    std::uint8_t regionId {};
    std::uint64_t descriptorOffset {};
    std::uint64_t regionOffset {};
    std::uint64_t length {};
    bool writable {};
    uml::gpu::UniqueDescriptor descriptor;
};

class VhostUserDevice {
public:
    using GuestMapper = std::function<std::span<std::uint8_t>(std::uint64_t, std::size_t)>;
    virtual ~VhostUserDevice() = default;

    [[nodiscard]] virtual const char* name() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t features() const noexcept = 0;
    [[nodiscard]] virtual std::size_t queueCount() const noexcept = 0;
    [[nodiscard]] virtual std::vector<std::uint8_t> configuration() const = 0;
    virtual void configurationRead(
        std::uint32_t,
        std::uint32_t) noexcept
    {
    }
    virtual void writeConfiguration(
        std::uint32_t offset,
        std::uint32_t flags,
        std::span<const std::uint8_t> bytes) = 0;
    [[nodiscard]] virtual VhostUserQueueResult processQueue(
        std::uint32_t index,
        std::uint64_t requestId,
        std::span<const std::uint8_t> request,
        std::size_t responseCapacity) = 0;
    /* The answer to a request that is not run because the guest that sent
     * it is ending and its gate has been told to discard (request_gate.hpp).
     * The guest's own shutdown waits for these completions, so the answer is
     * the one a guest cannot tell from success while nothing has happened:
     * the bytes of a response that reports no error, carrying whatever the
     * request needs echoed for its sender to retire it, a fence above all.
     * Empty for a request whose queue returns no response. The device is not
     * called otherwise; this reads the request and changes nothing. */
    [[nodiscard]] virtual std::vector<std::uint8_t> discardedResponse(
        std::uint32_t /* queueIndex */,
        std::span<const std::uint8_t> /* request */) const
    {
        return {};
    }
    virtual void reset() noexcept = 0;
    virtual void prepareReset() noexcept {}
    /* Whether a request buffer on a disabled ring stays there until the ring
     * is enabled again, rather than being completed empty at once. A guest
     * suspends by disabling its rings and sends nothing after, so a device
     * whose requests are asynchronous, like the GPU's, keeps what a late
     * submission or a held request left behind and runs it in order after
     * the resume. A device whose guest can wait for a request synchronously,
     * as a console's output waits with interrupts off, must not: a buffer it
     * never completes would hang the guest's suspend. */
    [[nodiscard]] virtual bool keepsRequestsOnDisabledRing() const noexcept
    {
        return false;
    }
    /* Whether the transport answers a guest's kick before it runs the
     * kicked queue rather than after. The kicking vCPU waits for that answer
     * with its interrupts off, so a device whose requests can run long, or
     * whose guest waits for their completions anyway, frees the vCPU for the
     * length of the work. While such a queue runs the guest adds buffers
     * without kicking, and the transport looks for them before it finishes.
     */
    [[nodiscard]] virtual bool answersKicksBeforeRunning() const noexcept
    {
        return false;
    }
    // Called only on the transport worker. Returned spans are borrowed until
    // reset; devices must release all retained guest state in reset().
    virtual void setGuestMapper(GuestMapper) {}
    /* The shared-memory ranges published to this guest, for a device whose
     * guest names buffers by region and offset instead of by guest-physical
     * address. Shared with the backend that owns the region. */
    virtual void setSharedMemoryRegions(
        std::shared_ptr<const SharedMemoryRegions>)
    {
    }

    [[nodiscard]] virtual std::optional<VhostUserSharedMemoryRegion>
        sharedMemoryRegion() const noexcept
    {
        return std::nullopt;
    }
    [[nodiscard]] virtual bool takeSharedMemoryAction(
        VhostUserSharedMemoryAction&)
    {
        return false;
    }
    virtual void completeSharedMemoryAction(
        std::uint64_t,
        bool) noexcept
    {
    }

    // A device with asynchronous device-to-driver data can expose one
    // readiness descriptor. The transport drains it on its worker and then
    // retries queues which may previously have had no device data available.
    [[nodiscard]] virtual int eventDescriptor() const noexcept { return -1; }
    virtual void drainEventSource() {}
    [[nodiscard]] virtual bool takeQueueCompletion(
        VhostUserQueueCompletion&)
    {
        return false;
    }
    /* A device event may also require a transport-level Virtio config
     * interrupt rather than a virtqueue retry or completion. */
    [[nodiscard]] virtual bool configChangePending() const noexcept
    {
        return false;
    }
    [[nodiscard]] virtual bool claimConfigChange() noexcept
    {
        return configChangePending();
    }
    virtual void releaseConfigChange() noexcept {}
    virtual void clearConfigChange() noexcept {}
    virtual void setStopRequest(const std::atomic_bool*) noexcept {}
    /* Printed after the transport's own readiness line when the device has
     * more to announce, such as where its display can be reached. */
    [[nodiscard]] virtual std::string readinessDescription() const
    {
        return {};
    }
};

} // namespace uml::virtio
