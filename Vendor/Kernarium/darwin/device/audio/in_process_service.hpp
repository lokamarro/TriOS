// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "in_process_vhost_user_service.hpp"
#include "microphone_switch.hpp"

#include <cstdint>
#include <memory>

namespace uml::audio {

enum class OutputMode : std::uint8_t {
    // The Mac's or iPhone's default output.
    host,
    // Nothing is played; the guest still sees a device that plays in real
    // time.
    null,
};

struct ServiceOptions final {
    OutputMode output {OutputMode::host};
    /* The user's Microphone setting. Without one the guest records silence
     * and the product never touches the microphone. */
    std::shared_ptr<MicrophoneSwitch> microphone;
};

[[nodiscard]] std::unique_ptr<uml::gpu::InProcessVhostUserService>
makeInProcessService(const ServiceOptions& options = {});

} // namespace uml::audio
