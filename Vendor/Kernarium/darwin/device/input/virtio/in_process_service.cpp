// SPDX-License-Identifier: GPL-2.0-or-later

#include "in_process_service.hpp"

#include "vhost_user_server.hpp"

namespace uml::input::virtio {

InProcessInputService::InProcessInputService(DeviceKind kind)
    : events_(std::make_shared<EventQueue>())
    , service_(std::make_unique<uml::gpu::InProcessVhostUserService>(
          "virtio-input",
          kind == DeviceKind::touchscreen ? "uml-touch" : "uml-keyboard",
          [kind, events = events_] {
              return std::make_unique<VirtioInputDevice>(kind, events);
          }))
{
}

InProcessInputService::~InProcessInputService() = default;

std::string InProcessInputService::takeConnectionName()
{
    return service_->takeConnectionName();
}

void InProcessInputService::send(
    std::span<const protocol::InputEvent> events) noexcept
{
    events_->enqueue(events);
}

void InProcessInputService::stop() noexcept
{
    service_->stop();
}

} // namespace uml::input::virtio
