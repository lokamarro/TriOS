// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <cstdio>
#include <exception>
#include <span>

namespace uml::testing {

/* One case of a test binary's table. A case with an `unsupported` reason is
 * reported with it and skipped, for a host that cannot run the case. */
struct NamedTest final {
	const char *name;
	void (*run)();
	const char *unsupported = nullptr;
};

/* Runs each case in order and prints `ok: <name> (<ms> ms)`. A case that
 * throws is reported as `FAIL: <name>: <what>` and the rest still run; the
 * result is 1 when any failed, for `return runNamedTests(tests);`. A failed
 * `require` aborts the binary before this reports anything. */
[[nodiscard]] inline int runNamedTests(std::span<const NamedTest> tests)
{
	int failures = 0;
	for (const auto &test : tests) {
		if (test.unsupported != nullptr) {
			std::printf("unsupported: %s: %s\n", test.name,
				test.unsupported);
			continue;
		}
		const auto started = std::chrono::steady_clock::now();
		try {
			test.run();
		} catch (const std::exception &error) {
			std::printf("FAIL: %s: %s\n", test.name, error.what());
			++failures;
			continue;
		}
		const auto elapsed = std::chrono::duration_cast<
			std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - started);
		std::printf("ok: %s (%lld ms)\n", test.name,
			static_cast<long long>(elapsed.count()));
	}
	return failures == 0 ? 0 : 1;
}

} // namespace uml::testing
