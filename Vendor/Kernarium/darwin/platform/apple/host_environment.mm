// SPDX-License-Identifier: GPL-2.0-or-later
#include "host_environment.hpp"

#include "os_memory_limit.hpp"

#import <Foundation/Foundation.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace uml::platform {

bool running_ios_app_on_macos() noexcept
{
	/* The answer is fixed for the life of the process, and callers reach it
	 * on the boot path, so resolve it once. */
	static const bool answer = [] {
		@autoreleasepool {
			return NSProcessInfo.processInfo.isiOSAppOnMac ? true
								       : false;
		}
	}();
	return answer;
}

namespace {

constexpr std::uint64_t mebibyte = 1024ULL * 1024ULL;
constexpr std::uint64_t maximum_limit_mib = 1024ULL * 1024ULL;

/* The launch option's value; zero until one is given. */
std::atomic<std::uint64_t> option_limit_bytes{0};

} // namespace

std::uint64_t parse_memory_limit_mib(const char *text) noexcept
{
	if (text == nullptr || *text == '\0')
		return 0;
	std::uint64_t mib = 0;
	for (const char *cursor = text; *cursor != '\0'; ++cursor) {
		if (*cursor < '0' || *cursor > '9')
			return 0;
		mib = mib * 10U + static_cast<std::uint64_t>(*cursor - '0');
		if (mib > maximum_limit_mib)
			return 0;
	}
	return mib * mebibyte;
}

std::uint64_t configured_process_memory_limit_bytes() noexcept
{
	if (const auto stated = option_limit_bytes.load(std::memory_order_relaxed);
	    stated != 0)
		return stated;
#ifdef UML_APP_STORE_RUNTIME
	return 0;
#else
	const char *text = std::getenv(memory_limit_variable);
	if (text == nullptr || *text == '\0')
		return 0;
	const auto bytes = parse_memory_limit_mib(text);
	if (bytes == 0) {
		static std::atomic<bool> reported{false};
		if (!reported.exchange(true))
			std::fprintf(stderr,
				"uml: %s=%s is not a whole number of MiB from 1 to "
				"1048576; no memory limit is stated\n",
				memory_limit_variable, text);
	}
	return bytes;
#endif
}

void set_configured_process_memory_limit(std::uint64_t bytes) noexcept
{
	option_limit_bytes.store(bytes, std::memory_order_relaxed);
}

bool process_memory_limit_is_virtual() noexcept
{
	return os_memory_limit_bytes() == 0 &&
		configured_process_memory_limit_bytes() != 0;
}

std::uint64_t process_memory_limit_bytes() noexcept
{
	return resolve_process_memory_limit(os_memory_limit_bytes(),
		configured_process_memory_limit_bytes());
}

} // namespace uml::platform
