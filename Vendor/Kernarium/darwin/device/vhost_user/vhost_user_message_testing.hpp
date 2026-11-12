// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::gpu::testing {

// Available only from the test-owned transport compiled with
// UML_GPU_STOP_WAIT_TESTING=1.
void resetStopWaitObserver() noexcept;
[[nodiscard]] bool waitForStopWaitCount(
    std::uint64_t count,
    std::uint32_t timeoutMilliseconds) noexcept;

} // namespace uml::gpu::testing
