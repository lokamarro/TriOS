// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>

namespace uml::gpu {

class VhostUserServer;

} // namespace uml::gpu

namespace uml::gpu::testing {

enum class StartupFailure {
    none,
    afterTransportSocketPair,
    afterInputSocketPair,
    workerEntry,
};

void failNextStartup(StartupFailure failure) noexcept;
void startupCheckpoint(StartupFailure failure);
[[nodiscard]] std::unique_ptr<VhostUserServer> makeTransportTestServer();

} // namespace uml::gpu::testing
