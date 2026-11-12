// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace uml::testing {

/* The unit tests' assertions. A failure prints the caller's file:line and the
 * message to stderr and aborts, so neither a catch in the test nor one in the
 * code under test can swallow it, and a failure inside a callback or on
 * another thread still ends the binary. */
[[noreturn]] inline void fail(std::string_view message,
	const std::source_location location = std::source_location::current())
{
	std::fprintf(stderr, "%s:%u: %.*s\n", location.file_name(),
		static_cast<unsigned>(location.line()),
		static_cast<int>(message.size()), message.data());
	std::abort();
}

inline void require(bool condition,
	const std::source_location location = std::source_location::current())
{
	if (!condition)
		fail("requirement failed", location);
}

inline void require(bool condition, std::string_view message,
	const std::source_location location = std::source_location::current())
{
	if (!condition)
		fail(message, location);
}

/* As require() for a system call that reports failure through errno: the
 * failure message also names the errno the call left, so a failure that a
 * loaded host causes says which call failed and why. Call it right after the
 * system call, because it reads errno on entry. */
inline void requireSyscall(bool succeeded, std::string_view message,
	const std::source_location location = std::source_location::current())
{
	const int error = errno;
	if (!succeeded)
		fail(std::string{message} + ": " + std::strerror(error) +
				" (errno " + std::to_string(error) + ")",
			location);
}

/* Requires `operation` to throw an `Exception`. Anything else it throws
 * propagates. */
template<typename Exception = std::exception, typename Operation>
void requireThrows(Operation &&operation,
	std::string_view message = "the expected exception was not thrown",
	const std::source_location location = std::source_location::current())
{
	try {
		std::forward<Operation>(operation)();
	} catch (const Exception &) {
		return;
	}
	fail(message, location);
}

} // namespace uml::testing
