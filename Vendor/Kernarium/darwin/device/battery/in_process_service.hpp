// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "in_process_vhost_user_service.hpp"

#include <memory>

namespace uml::battery {

[[nodiscard]] std::unique_ptr<uml::gpu::InProcessVhostUserService>
makeInProcessService();

} // namespace uml::battery
