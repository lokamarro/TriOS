// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "KernariumRuntime.h"

#include <cstddef>
#include <mutex>

namespace kernarium::console {

class Output final {
public:
    void setCallback(
        kernarium_runtime_console_output_callback callback,
        void* context)
    {
        std::lock_guard lock(callbackMutex_);
        callback_ = callback;
        context_ = context;
    }

    void publish(const void* buffer, std::size_t length)
    {
        kernarium_runtime_console_output_callback callback;
        void* context;
        {
            std::lock_guard lock(callbackMutex_);
            callback = callback_;
            context = context_;
        }
        if (callback != nullptr)
            callback(context, buffer, length);
    }

    [[nodiscard]] std::mutex& stdoutMutex() noexcept
    {
        return stdoutMutex_;
    }

private:
    std::mutex callbackMutex_;
    std::mutex stdoutMutex_;
    kernarium_runtime_console_output_callback callback_ {};
    void* context_ {};
};

/* exit() runs static destructors while the guest and its console relay can
 * still run. Both the registration and stdout lock must outlive that phase. */
[[nodiscard]] inline Output& output()
{
    static Output& instance = *new Output;
    return instance;
}

} // namespace kernarium::console
