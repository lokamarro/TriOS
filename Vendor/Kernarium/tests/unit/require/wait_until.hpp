// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <thread>

namespace uml::testing {

/* Polls `done` until it holds or `timeout` passes, and returns whether it
 * holds, judged once more after the deadline. A zero `poll` yields between
 * evaluations instead of sleeping, for a wait that should spin. A timeout is
 * the caller's to act on: `require(waitUntil(...), "what never happened")`. */
template<typename Predicate>
[[nodiscard]] bool waitUntil(Predicate &&done,
	std::chrono::steady_clock::duration timeout,
	std::chrono::steady_clock::duration poll =
		std::chrono::milliseconds{1})
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (!done()) {
		if (std::chrono::steady_clock::now() >= deadline)
			return done();
		if (poll > std::chrono::steady_clock::duration::zero())
			std::this_thread::sleep_for(poll);
		else
			std::this_thread::yield();
	}
	return true;
}

} // namespace uml::testing
