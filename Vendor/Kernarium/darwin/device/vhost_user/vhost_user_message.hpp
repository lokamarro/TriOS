// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "vhost_user_protocol.hpp"
#include "unique_descriptor.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace uml::gpu {

inline constexpr std::size_t maximumVhostUserMessagePayload =
    1024U * 1024U;
// Linux vhost-user memory tables may contain up to 64 regions. Keep the wire
// receiver's descriptor capacity aligned with that protocol limit so a valid
// table cannot be rejected solely by the ancillary-data buffer size.
inline constexpr std::size_t maximumVhostUserDescriptors = 64;

class ReceivedDescriptors final {
public:
    [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
    [[nodiscard]] std::size_t size() const noexcept { return count_; }

    [[nodiscard]] UniqueDescriptor& operator[](std::size_t index) noexcept
    {
        return descriptors_[index];
    }

    [[nodiscard]] const UniqueDescriptor& operator[](
        std::size_t index) const noexcept
    {
        return descriptors_[index];
    }

    [[nodiscard]] bool adopt(UniqueDescriptor descriptor) noexcept
    {
        if (count_ == descriptors_.size())
            return false;
        descriptors_[count_++] = std::move(descriptor);
        return true;
    }

private:
    std::array<UniqueDescriptor, maximumVhostUserDescriptors> descriptors_;
    std::size_t count_ {};
};

struct VhostUserMessage final {
    protocol::VhostHeader header {};
    std::vector<std::uint8_t> payload;
    ReceivedDescriptors descriptors;
};

// This process's open-descriptor ceiling, as the kernel applies it.
struct DescriptorCeiling final {
    std::uint64_t soft {};
    std::uint64_t hard {};
    /* The kernel's own per-process cap. However high the soft limit is set, no
     * descriptor is ever allocated past this; zero when the host will not say.
     */
    std::uint64_t perProcess {};

    // The number that actually decides how many descriptors this process gets.
    [[nodiscard]] std::uint64_t effective() const noexcept
    {
        if (perProcess != 0 && perProcess < soft)
            return perProcess;
        return soft;
    }
};

[[nodiscard]] DescriptorCeiling descriptorCeiling() noexcept;

// One line naming that ceiling, for a startup note or a failure.
[[nodiscard]] std::string describeDescriptorCeiling();

/* Raise the soft open-descriptor ceiling as far as the host permits.
 *
 * A vhost-user backend that publishes a shared-memory region hands the guest
 * one descriptor for every host-visible blob it maps, and the guest holds each
 * one for as long as the mapping lives. The transport is an in-process
 * socketpair, so those descriptors and the backend's own share a single table.
 * Darwin refuses to install an arriving SCM_RIGHTS once that table is full and
 * reports the refusal as EMSGSIZE on recvmsg, which ends the connection and
 * takes the guest's GPU with it. Nothing else in this process raises the
 * ceiling, and an iOS application starts with a small one.
 *
 * Returns the soft ceiling in force afterwards.
 */
std::uint64_t raiseDescriptorCeiling() noexcept;

// Returns false only for an orderly peer close or a truncated stream.
[[nodiscard]] bool receiveVhostUserMessage(
    int socket,
    VhostUserMessage& message,
    const std::atomic_bool* stopRequested = nullptr);

} // namespace uml::gpu
