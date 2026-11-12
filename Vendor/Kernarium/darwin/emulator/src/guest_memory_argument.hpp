/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace uml::emulator {

/* The last mem= argument's size as Linux memparse() reads it, or 0 without
 * one. Launchers size host resources from it, so they agree with the kernel
 * and with the emulator's own budget on every spelling of mem=. */
[[nodiscard]] std::size_t configured_guest_memory_bytes(
	const std::vector<std::string> &arguments);

} // namespace uml::emulator
