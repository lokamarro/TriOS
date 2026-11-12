// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace uml::emulator::tests {

/*
 * Apple Arm64 exposes the architectural virtual counter to EL0. Both engines
 * return the host's own frequency and count registers to the guest, so tests
 * read the same registers to bound those results.
 */
[[nodiscard]] inline std::uint64_t read_host_counter_frequency() noexcept
{
	std::uint64_t value{};
	asm volatile("mrs %0, cntfrq_el0" : "=r"(value));
	return value;
}

[[nodiscard]] inline std::uint64_t read_host_virtual_counter() noexcept
{
	std::uint64_t value{};
	asm volatile("mrs %0, cntvct_el0" : "=r"(value));
	return value;
}

} // namespace uml::emulator::tests
