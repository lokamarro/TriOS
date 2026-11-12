// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace uml::platform {

enum class MicrophonePermission : std::uint8_t {
    // The user has not been asked.
    undetermined,
    denied,
    granted,
};

/* The system's permission to record from the microphone.
 *
 * permission() only reads the current answer and never shows a prompt.
 * request() may show the system prompt, and it does so only when the answer is
 * undetermined; the caller invokes it as the direct result of a user's act, such
 * as turning a Microphone setting on, and never at launch. The completion
 * runs once, on an arbitrary thread, with the answer. */
class MicrophoneAuthorization {
public:
    virtual ~MicrophoneAuthorization() = default;

    [[nodiscard]] virtual MicrophonePermission permission() const noexcept = 0;
    virtual void request(
        std::function<void(MicrophonePermission)> completion) = 0;
};

[[nodiscard]] std::shared_ptr<MicrophoneAuthorization>
makeAppleMicrophoneAuthorization();

} // namespace uml::platform
