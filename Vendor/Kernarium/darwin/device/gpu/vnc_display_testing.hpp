// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
namespace uml::gpu {
class VncDisplay;
}

namespace uml::gpu::testing {

void failNextEventThreadStart() noexcept;
void eventThreadStartCheckpoint(std::uint16_t port);
[[nodiscard]] std::uint16_t failedEventThreadPort() noexcept;
void verifyPrivateMetalTextureReadback(VncDisplay& display);
void verifyScaledReadbackIsDamageBounded(VncDisplay& display);
void verifyClearDiscardsEarlierReadbacks(VncDisplay& display);

/* Holds every readback completion at its start, before it publishes, until
 * released, so a test can act while a readback is provably in flight. A held
 * completion gives up after a bounded wait rather than hang the test. */
void holdReadbackCompletions() noexcept;
void releaseReadbackCompletions() noexcept;
[[nodiscard]] unsigned heldReadbackCompletions() noexcept;
void readbackCompletionCheckpoint() noexcept;

} // namespace uml::gpu::testing
