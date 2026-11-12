// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cerrno>
#include <new>

namespace uml::memory {

/* Runs one C bridge entry point against the address space `resolve` returns
 * and reports the outcome the way the bridge does: -1 with ESRCH when there
 * is no space, otherwise the call's own result, with EFAULT when it fails
 * without setting errno. An exception becomes -1 with ENOMEM or EIO, after
 * `abandon` has dropped whatever partial state the call left. The space is
 * resolved and released inside the call, before `abandon` runs. */
template<typename Resolve, typename Call, typename Abandon>
[[nodiscard]] int bridge_call(Resolve &&resolve, Call &&call,
	Abandon &&abandon) noexcept
{
	try {
		decltype(auto) space = resolve();
		if (!space) {
			if (errno == 0)
				errno = ESRCH;
			return -1;
		}
		errno = 0;
		const int result = call(*space);
		if (result < 0 && errno == 0)
			errno = EFAULT;
		return result;
	} catch (const std::bad_alloc &) {
		abandon();
		errno = ENOMEM;
		return -1;
	} catch (...) {
		abandon();
		errno = EIO;
		return -1;
	}
}

} // namespace uml::memory
