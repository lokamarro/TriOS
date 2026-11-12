// SPDX-License-Identifier: GPL-2.0-or-later

#include "guest_sync.hpp"

#include "guest_files.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace uml::runtime {

GuestSyncResult sync_guest_files() noexcept
{
	const auto result = ::uml::platform::sync_guest_files();
	return {.flushed = result.flushed, .failed = result.failed};
}

bool guest_file_registry_idle() noexcept
{
	return ::uml::platform::guest_file_registry_idle();
}

unsigned release_guest_file_locks(bool retiring) noexcept
{
	return ::uml::platform::release_guest_file_locks(retiring);
}

void restore_guest_file_locks() noexcept
{
	if (::uml::platform::restore_guest_file_locks())
		return;
	std::fprintf(stderr, "uml: refusing to resume: a guest disk changed or "
		"cannot be relocked (errno %d)\n", errno);
	std::abort();
}

} // namespace uml::runtime
