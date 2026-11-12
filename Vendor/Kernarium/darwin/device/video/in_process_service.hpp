// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "in_process_vhost_user_service.hpp"
#include "shared_memory_regions.hpp"

#include <memory>

namespace uml::video {

/* `regions` is the shared-memory index of the backend that owns the guest's
 * host-visible region, when the guest has one. With it the decoder can fill
 * a capture buffer that lives in that region. */
[[nodiscard]] std::unique_ptr<uml::gpu::InProcessVhostUserService>
makeInProcessService(
    std::shared_ptr<uml::virtio::SharedMemoryRegions> regions = {});

/* The encoder service, device ID 30, one transport thread beside the
 * decoder's. A host with no hardware encoder fails the service's start, so a
 * caller that must not fail checks `videoEncodeCapabilities()` first.
 * `regions` is as for the decoder: with it the encoder reads a raw frame that
 * lives in the guest's host-visible region. */
[[nodiscard]] std::unique_ptr<uml::gpu::InProcessVhostUserService>
makeInProcessEncoderService(
    std::shared_ptr<uml::virtio::SharedMemoryRegions> regions = {});

} // namespace uml::video
