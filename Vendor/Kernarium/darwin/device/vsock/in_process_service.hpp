// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "endpoint.hpp"
#include "in_process_vhost_user_service.hpp"

#include <memory>

namespace uml::vsock {

/* The device's service and the way to open connections through it. The
 * endpoint outlives the service and refuses with ENODEV once it has
 * stopped. */
struct InProcessService final {
    std::unique_ptr<uml::gpu::InProcessVhostUserService> service;
    std::shared_ptr<Endpoint> endpoint;
};

/* Starts the device on a service thread named "uml-vsock". */
[[nodiscard]] InProcessService makeInProcessService();

} // namespace uml::vsock
