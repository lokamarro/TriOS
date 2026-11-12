// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <mach/mach.h>

#include <cstdint>

namespace uml::platform {

/* What the system holds this process's memory footprint to: the footprint plus
 * what the system lets it grow by, which is the limit iOS enforces (jetsam) and
 * 2,867 MiB on the iPad mini 6. Zero where the system sets none (macOS, and an
 * iOS application running on a Mac). One task_info call.
 *
 * Header-only and free of the rest of the platform, so the application, which
 * does not link the runtime, reads the figure the same way
 * process_memory_limit_bytes() does. */
[[nodiscard]] inline std::uint64_t os_memory_limit_bytes() noexcept
{
	task_vm_info_data_t info{};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO,
		    reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS ||
	    count < TASK_VM_INFO_COUNT || info.limit_bytes_remaining == 0)
		return 0;
	/* What the footprint may still grow by, on top of what it is. */
	return info.phys_footprint + info.limit_bytes_remaining;
}

} // namespace uml::platform
