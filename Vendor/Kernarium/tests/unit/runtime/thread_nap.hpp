// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <mach/mach_time.h>

namespace uml::testing {

/* Sleeps without leaving libsystem_kernel: usleep is a libsystem_c function,
 * a library the fence keeps its distance from. */
inline void nap(unsigned microseconds)
{
	static const mach_timebase_info_data_t base = [] {
		mach_timebase_info_data_t value{};
		mach_timebase_info(&value);
		return value;
	}();
	mach_wait_until(mach_absolute_time() +
		static_cast<std::uint64_t>(microseconds) * 1000ULL * base.denom /
		base.numer);
}

} // namespace uml::testing
