// SPDX-License-Identifier: GPL-2.0-or-later

#include "in_process_service.hpp"

#include "virtio_vsock_device.hpp"
#include "vhost_user_server.hpp"

namespace uml::vsock {

InProcessService makeInProcessService()
{
    auto gateway = std::make_shared<Gateway>();
    InProcessService result;
    result.service = std::make_unique<uml::gpu::InProcessVhostUserService>(
        "Virtio vsock", "uml-vsock", [gateway] {
            return std::make_unique<VirtioVsockDevice>(gateway);
        });
    result.endpoint = std::make_shared<Endpoint>(std::move(gateway));
    return result;
}

} // namespace uml::vsock
