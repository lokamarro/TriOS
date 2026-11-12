// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "in_process_vhost_user_service.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace uml::console {

[[nodiscard]] std::unique_ptr<uml::gpu::InProcessVhostUserService>
makeInProcessService(
    std::filesystem::path socketPath,
    std::string portName = {});
// Duplicates an already-connected SOCK_STREAM descriptor, served as the
// named serial port `portName`, which must not be empty.
[[nodiscard]] std::unique_ptr<uml::gpu::InProcessVhostUserService>
makeInProcessService(int connectedDescriptor, std::string portName);

} // namespace uml::console
