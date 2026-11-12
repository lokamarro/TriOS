// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A launch that states the process's memory limit gives its run the budgets
 * that derive from it, as the limit iOS reports does on a device: the option
 * and the variable both state it, the option wins, and a run that states
 * nothing gets what it got before. Each run is its own word, so one run's
 * option does not outlive it.
 */
#include "uml/emulator.h"
#include "cpu/gadget_cache_budget.hpp"
#include "emulator_test_support.hpp"
#include "platform/host_environment.hpp"
#include "require.hpp"
#include "testing.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

namespace {

using uml::testing::make_emulator;
using uml::testing::require;
namespace platform = uml::platform;

constexpr std::size_t mib = 1024U * 1024U;

/* What the kernel saw of the process while it ran. */
struct Seen final {
	std::uint64_t limit{};
	std::size_t budget{};
};

Seen seen;

int observing_kernel(int, char *const[], char *const[])
{
	seen = {platform::process_memory_limit_bytes(),
		uml::cpu::gadget_cache::budget()};
	return 0;
}

Seen run(uml_emulator *emulator, const char *option = nullptr,
	const char *cpus = "ncpus=1")
{
	const char *arguments[]{"uml-test", "--execution-engine=gadgets",
		"mem=1536M", cpus, option};
	seen = {};
	require(uml_emulator_run(emulator, option == nullptr ? 4 : 5, arguments,
			nullptr) == 0,
		"the observing kernel did not run");
	return seen;
}

void theLaunchStatesTheLimit()
{
	auto emulator = make_emulator(observing_kernel);

	/* Nothing stated: a quarter of the guest, which is under no limit's
	 * eighth. */
	auto result = run(emulator.get());
	require(result.limit == 0 && result.budget == 384 * mib,
		"a run that states nothing changed");

	/* The iPad mini 6's limit, by option: an eighth of it is 358 MiB, above
	 * the cap of one vCPU and of eight. */
	result = run(emulator.get(), "--memory-limit=2867M");
	require(result.limit == 2867 * mib && result.budget == 128 * mib,
		"the option did not state the limit, or one vCPU's cap");
	result = run(emulator.get(), "--memory-limit=2867M", "ncpus=8");
	require(result.limit == 2867 * mib && result.budget == 272 * mib,
		"eight vCPUs' cap");
	require(2867 * mib / 8 / mib == 358);

	/* The option does not outlive its run. */
	result = run(emulator.get());
	require(result.limit == 0 && result.budget == 384 * mib,
		"a run kept the previous run's option");

	/* The variable states it too, and the option beats it. */
	require(::setenv(platform::memory_limit_variable, "2867", 1) == 0);
	result = run(emulator.get());
	require(result.limit == 2867 * mib && result.budget == 128 * mib,
		"the variable did not state the limit");
	result = run(emulator.get(), "--memory-limit=1024M");
	require(result.limit == 1024 * mib && result.budget == 128 * mib,
		"the option did not beat the variable");
	::unsetenv(platform::memory_limit_variable);

	/* A budget the run names is its own. */
	result = run(emulator.get(), "--gadget-cache-budget=96M");
	require(result.budget == 96 * mib, "an explicit budget was overridden");
}

} // namespace

int main()
{
	try {
		::unsetenv(platform::memory_limit_variable);
		if (platform::process_memory_limit_bytes() != 0) {
			std::puts("unsupported: the host enforces its own limit");
			return EXIT_SUCCESS;
		}
		theLaunchStatesTheLimit();
	} catch (const std::exception &error) {
		std::fprintf(stderr, "memory limit test failed: %s\n", error.what());
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
