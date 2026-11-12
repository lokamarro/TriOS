// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_sampler.hpp"

/*
 * The guest sampler of an App Store runtime: there is none. The sampler
 * (guest_sampler.cpp) suspends the vCPU carriers and reads their registers,
 * which a shipped application does not do to itself. A runtime built with
 * UML_APP_STORE_RUNTIME links this file in its place, so no executor is ever
 * registered and the bracketing calls never run.
 */
namespace uml::cpu::arm64::gadgets::diagnostics {

GuestSamplingSlot *guest_sampling_register(
	const SampledExecutor &) noexcept
{
	return nullptr;
}

void guest_sampling_enter(GuestSamplingSlot &) noexcept {}

void guest_sampling_leave(GuestSamplingSlot &) noexcept {}

void guest_sampling_thread(
	GuestSamplingSlot &, const std::uint64_t *) noexcept
{
}

void guest_sampling_unregister(GuestSamplingSlot &) noexcept {}

} // namespace uml::cpu::arm64::gadgets::diagnostics
