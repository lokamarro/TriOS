// SPDX-License-Identifier: GPL-2.0-or-later

#include "in_process_service.hpp"

#include "host_battery.hpp"
#include "vhost_user_server.hpp"
#include "virtio_battery_device.hpp"

namespace uml::battery {

std::unique_ptr<uml::gpu::InProcessVhostUserService> makeInProcessService()
{
    return std::make_unique<uml::gpu::InProcessVhostUserService>(
        "Virtio battery", "uml-battery", [] {
            return std::make_unique<VirtioBatteryDevice>(
                uml::platform::makeAppleHostBatteryProvider());
        });
}

} // namespace uml::battery
