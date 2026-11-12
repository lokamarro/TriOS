// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>

namespace uml::gpu {

/* AppKit requires its event loop on the process main thread. This helper runs
 * the synchronous UML entry on a worker while the caller services the native
 * display. Closing the window invokes requestStop and waits for run to return. */
[[nodiscard]] int runNativeDisplayApplication(
    const std::function<int()>& run,
    const std::function<void()>& requestStop);

} // namespace uml::gpu
