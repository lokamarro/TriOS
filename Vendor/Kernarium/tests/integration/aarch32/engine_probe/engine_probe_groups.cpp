// SPDX-License-Identifier: GPL-2.0-or-later
/* The groups of the AArch32 engine probe, in the order they run. */
#include "engine_probe.hpp"

namespace probe {

const Group groups[] = {
	{"basic", group_basic},
	{"interwork", group_interwork},
	{"signals", group_signals},
	{"kuser", group_kuser},
	{"tls", group_tls},
	{"cacheflush", group_cacheflush},
	{"faults", group_faults},
	{"ill", group_ill},
	{"ptrace", group_ptrace},
	{"swp", group_swp},
	{"atomics", group_atomics},
	{"absent", group_absent},
	{"simd", group_simd},
	{"armv8fp", group_armv8_fp},
	{"complete", group_complete},
};
const unsigned group_count = sizeof(groups) / sizeof(groups[0]);

} // namespace probe
