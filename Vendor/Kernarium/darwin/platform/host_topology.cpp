// SPDX-License-Identifier: GPL-2.0-or-later
#include "host_topology.hpp"

#include <cstddef>
#include <limits>
#include <sys/sysctl.h>
#include <unistd.h>

namespace uml::platform {

unsigned online_processor_count() noexcept
{
	const auto count = sysconf(_SC_NPROCESSORS_ONLN);
	if (count <= 0)
		return 1;
	if (static_cast<unsigned long>(count) >
	    std::numeric_limits<unsigned>::max())
		return std::numeric_limits<unsigned>::max();
	return static_cast<unsigned>(count);
}

unsigned physical_processor_count() noexcept
{
	int count = 0;
	std::size_t size = sizeof(count);
	if (sysctlbyname("hw.physicalcpu", &count, &size, nullptr, 0) != 0 ||
	    size != sizeof(count) || count <= 0)
		return online_processor_count();
	return static_cast<unsigned>(count);
}

} // namespace uml::platform
