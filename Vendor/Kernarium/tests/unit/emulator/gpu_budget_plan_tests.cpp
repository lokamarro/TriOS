// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The host GPU's budget as a launch plans it: the arguments it is given, the
 * gadget cache the run will set, and the limit the launch states. The macOS
 * launcher and the iOS runtime both ask this one function.
 */
#include "gpu_budget_plan.hpp"
#include "named_tests.hpp"
#include "options.hpp"
#include "platform/host_environment.hpp"
#include "require.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using uml::testing::require;
namespace emulator = uml::emulator;
namespace platform = uml::platform;

constexpr std::uint64_t mib = 1024ULL * 1024ULL;

/* A launch that states none of A, E_rest and S: the arithmetic of the formula
 * alone. The conservative launch set is what a launch takes without these. */
constexpr emulator::GpuBudgetParameters no_terms{
	.application = 0, .engine_rest = 0, .headroom = 0};

/* Leaves the environment and the launch option as the case found them. */
class LimitScope final {
public:
	LimitScope()
	{
		::unsetenv(platform::memory_limit_variable);
		platform::set_configured_process_memory_limit(0);
	}

	~LimitScope()
	{
		platform::set_configured_process_memory_limit(0);
		::unsetenv(platform::memory_limit_variable);
	}

	LimitScope(const LimitScope &) = delete;
	LimitScope &operator=(const LimitScope &) = delete;
};

void aLaunchParameterIsANumberWithAnOptionalSuffix()
{
	std::uint64_t bytes = 7;
	require(emulator::parse_gpu_budget_bytes("0", bytes) && bytes == 0);
	require(emulator::parse_gpu_budget_bytes("512", bytes) && bytes == 512);
	require(emulator::parse_gpu_budget_bytes("4k", bytes) && bytes == 4096);
	require(emulator::parse_gpu_budget_bytes("300M", bytes) &&
		bytes == 300 * mib);
	require(emulator::parse_gpu_budget_bytes("2g", bytes) &&
		bytes == 2048 * mib);
	bytes = 7;
	for (const char *refused : {"", "M", "-1", "1.5M", "12MB", "0x10", " 5",
		     "99999999999999999999", "18446744073709551615G"})
		require(!emulator::parse_gpu_budget_bytes(refused, bytes), refused);
	require(bytes == 7, "a refused parameter leaves the value alone");
}

void theArgumentsStateTheLimitAndTheCache()
{
	const LimitScope scope;
	const std::vector<std::string> arguments{
		"uml-darwin", "mem=1536M", "--memory-limit=2867M",
		"--gadget-cache-budget=300M", "ncpus=4"};
	require(emulator::configured_option_bytes(arguments, "--memory-limit=") ==
		2867 * mib);
	require(emulator::configured_option_bytes(arguments,
			"--gadget-cache-budget=") == 300 * mib);
	require(emulator::configured_option_bytes(arguments, "--jit-tier=") == 0);

	const auto plan = emulator::plan_gpu_budget(arguments, no_terms);
	require(plan.guest_memory == 1536 * mib);
	require(plan.guest_cpus == 4);
	require(plan.process_limit == 2867 * mib);
	require(plan.engine == 316 * mib,
		"the cache the launch states and four vCPUs' slot tags");
	/* Without parameters the launch takes the conservative set: 64 MiB for
	 * the application, 320 for the rest of the engine, 149 of headroom. */
	const auto launch = emulator::plan_gpu_budget(arguments, {});
	require(launch.engine == (316 + 320) * mib);
	require(launch.budget.available ==
		(2867 - 1536 - 64 - 636 - 149) * mib);
	require(launch.budget.bytes == 482 * mib);
	require(plan.budget.available == 2867 * mib - 1536 * mib - 316 * mib);
	require(plan.budget.bytes == 768 * mib);
	require(!plan.budget.short_of_floor);
}

void theVCPUCountComesFromTheLastValidArgument()
{
	require(emulator::configured_guest_cpu_count({"uml-darwin", "ncpus=4"}) ==
		4);
	require(emulator::configured_guest_cpu_count(
			{"uml-darwin", "ncpus=2", "ncpus=6"}) == 6);
	require(emulator::configured_guest_cpu_count(
			{"uml-darwin", "ncpus=3", "ncpus=lots"}) == 3,
		"a malformed value leaves the valid one");
	require(emulator::configured_guest_cpu_count(
			{"uml-darwin", "ncpus=9999"}) ==
			uml::runtime::maximum_guest_cpu_count,
		"clamped to the guest maximum");
	require(emulator::configured_guest_cpu_count({"uml-darwin"}) >= 1,
		"the host's default");
}

/* Without a cache option the run derives the budget from the guest, the limit
 * and the vCPU count; the plan counts that figure, not its own. */
void theDerivedGadgetCacheCountsAsEngineMemory()
{
	const LimitScope scope;
	const auto planned = [](const char *limit, const char *cpus) {
		return emulator::plan_gpu_budget(
			std::vector<std::string>{"uml-darwin", "mem=1536M", limit,
				cpus},
			no_terms);
	};
	/* A quarter of 1536 MiB is 384, and an eighth of the limit is 358: one
	 * vCPU's cap is under both, and sixteen vCPUs' above both. */
	const auto one = planned("--memory-limit=2867M", "ncpus=1");
	require(one.engine == (128 + 4) * mib, "one vCPU's cap and slot tags");
	const auto sixteen = planned("--memory-limit=2867M", "ncpus=16");
	require(sixteen.engine == 2867 * mib / 8 + 64 * mib,
		"an eighth of the limit and sixteen vCPUs' slot tags");
	const auto roomy = planned("--memory-limit=8192M", "ncpus=16");
	require(roomy.engine == (384 + 64) * mib, "a quarter of the guest");
	const auto roomy_one = planned("--memory-limit=8192M", "ncpus=1");
	require(roomy_one.engine == (128 + 4) * mib, "one vCPU's cap");
}

/* The most RAM the application's settings offer is the largest guest for which
 * the plan, with the maximum set (none of A, E_rest and S), is not short of
 * the floor: the plan and the bound agree. */
void thePlanAgreesWithTheMaximumGuestMemory()
{
	const LimitScope scope;
	if (platform::process_memory_limit_bytes() != 0)
		return; /* a host that sets its own limit */
	for (const unsigned cpus : {1U, 2U, 3U, 4U, 6U, 8U}) {
		const auto maximum =
			uml::runtime::guest_memory::maximum_guest_memory_mib(
				2867 * mib, cpus);
		const auto at = emulator::plan_gpu_budget(
			std::uint64_t{maximum} * mib, 2867 * mib, 0, cpus, no_terms);
		require(at.process_limit == 2867 * mib);
		require(!at.budget.short_of_floor, "the maximum fits");
		require(at.budget.available >= 384 * mib);
		const auto above = emulator::plan_gpu_budget(
			std::uint64_t{maximum + 64U} * mib, 2867 * mib, 0, cpus,
			no_terms);
		require(above.budget.short_of_floor,
			"the next size up is short of the floor");
	}
}

/* The default guest RAM is the largest half-gibibyte step the conservative
 * launch set leaves the floor for: the plan of a launch that takes the default
 * is not short of the floor, the first size above the launch set's own
 * maximum is, and so is the 2048 MiB the settings still offer. */
void thePlanAtTheDefaultLeavesTheGPUItsFloor()
{
	const LimitScope scope;
	if (platform::process_memory_limit_bytes() != 0)
		return; /* a host that sets its own limit */
	namespace budget = uml::runtime::guest_memory;
	const emulator::GpuBudgetParameters launch;
	for (const unsigned cpus : {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U}) {
		const auto chosen = budget::default_guest_memory_mib(
			2867 * mib, cpus);
		const auto at = emulator::plan_gpu_budget(
			std::uint64_t{chosen} * mib, 2867 * mib, 0, cpus, launch);
		require(!at.budget.short_of_floor, "the default fits");
		const auto launch_maximum = budget::maximum_guest_memory_mib(
			2867 * mib, cpus, budget::launch_memory_set);
		require(chosen <= launch_maximum);
		const auto above = emulator::plan_gpu_budget(
			std::uint64_t{launch_maximum + 64U} * mib, 2867 * mib, 0,
			cpus, launch);
		require(above.budget.short_of_floor,
			"the next size up is short of the floor");
		const auto big = emulator::plan_gpu_budget(
			2048 * mib, 2867 * mib, 0, cpus, launch);
		require(big.budget.short_of_floor, "2048 MiB is short of the floor");
	}
}

/* The host GPU's budget of the default guest on an iPad mini 6 (2867 MiB,
 * 1536 MiB) under the conservative launch set at 1 through 8 vCPUs: the limit
 * less 64 MiB for the application, 1536 for the guest, the cache cap, 4 MiB
 * of slot tags per vCPU, 320 MiB for the rest of the engine and 149 of
 * headroom. */
void theGPUBudgetOfTheDefaultOnAnIPadMini()
{
	const LimitScope scope;
	if (platform::process_memory_limit_bytes() != 0)
		return; /* a host that sets its own limit */
	constexpr std::uint64_t cache[8]{128, 160, 192, 224, 240, 240, 272, 272};
	constexpr std::uint64_t expected[8]{666, 630, 594, 558, 538, 534, 498, 494};
	for (unsigned cpus = 1; cpus <= 8; ++cpus) {
		const auto plan = emulator::plan_gpu_budget(
			1536 * mib, 2867 * mib, 0, cpus, {});
		require(plan.engine == (cache[cpus - 1] + 4 * cpus + 320) * mib);
		require(plan.budget.available ==
			(2867 - 64 - 1536 - cache[cpus - 1] - 4 * cpus - 320 - 149) *
				mib);
		require(plan.budget.bytes == expected[cpus - 1] * mib,
			std::to_string(cpus).c_str());
		require(!plan.budget.short_of_floor);
	}
}

/* A launcher's default mem=: its own size without a stated limit, the shared
 * default for the limit with one (--memory-limit=, else UML_MEMORY_LIMIT_MIB)
 * at the stated vCPU count. */
void aLauncherTakesTheDefaultGuestMemoryOnlyWhereALimitIsStated()
{
	const LimitScope scope;
	if (platform::process_memory_limit_bytes() != 0)
		return; /* a host that sets its own limit */
	const auto argument = [](std::vector<std::string> arguments) {
		arguments.insert(arguments.begin(), "uml-darwin");
		return emulator::default_guest_memory_argument(arguments, "mem=8G");
	};
	require(argument({}) == "mem=8G", "no limit: the launcher's own");
	require(argument({"ncpus=6"}) == "mem=8G");
	/* The iPad mini 6's limit gives 1536 MiB at every vCPU count. */
	for (const char *cpus : {"ncpus=1", "ncpus=4", "ncpus=6", "ncpus=8"})
		require(argument({"--memory-limit=2867M", cpus}) == "mem=1536M",
			cpus);
	require(argument({"--memory-limit=2867M"}) == "mem=1536M",
		"the applications' default vCPU count");
	require(argument({"--memory-limit=3584M", "ncpus=6"}) == "mem=2048M");
	require(argument({"--memory-limit=4096M", "ncpus=6"}) == "mem=2560M");
	/* Where a guest may have 4096 MiB the default also keeps the GPU a
	 * gibibyte: 3584 MiB would leave it 739 at six vCPUs, 3072 leaves 1251. */
	require(argument({"--memory-limit=5120M", "ncpus=6"}) == "mem=3072M");
	require(argument({"--memory-limit=8192M", "ncpus=8"}) == "mem=4096M",
		"the range's top");
	require(argument({"--memory-limit=2048M", "ncpus=4"}) == "mem=1408M",
		"a limit under the range's bottom keeps the maximum on its steps");
	/* The last --memory-limit= wins, as the run reads it. */
	require(argument({"--memory-limit=8192M", "--memory-limit=2867M",
			"ncpus=6"}) == "mem=1536M");
	/* The environment states the limit when the option does not. */
	::setenv(platform::memory_limit_variable, "4096", 1);
	require(argument({"ncpus=6"}) == "mem=2560M");
	require(argument({"--memory-limit=2867M", "ncpus=6"}) == "mem=1536M",
		"the option wins over the variable");
}

void theVCPUCountMovesTheBudget()
{
	const LimitScope scope;
	if (platform::process_memory_limit_bytes() != 0)
		return; /* a host that sets its own limit */
	const auto plan = [](unsigned cpus) {
		return emulator::plan_gpu_budget(2240 * mib, 2867 * mib, 0, cpus,
			no_terms);
	};
	/* Two vCPUs: a 160 MiB cache and 8 MiB of tags leave 459 MiB. */
	require(plan(2).engine == 168 * mib);
	require(plan(2).budget.available == 459 * mib);
	require(!plan(2).budget.short_of_floor);
	/* Eight: 272 MiB and 32 MiB leave 323, under the floor. */
	require(plan(8).engine == 304 * mib);
	require(plan(8).budget.available == 323 * mib);
	require(plan(8).budget.short_of_floor);
	require(plan(8).budget.bytes == 384 * mib);
}

void theLaunchParametersDefaultToTheLaunchSet()
{
	const emulator::GpuBudgetParameters defaults;
	const auto &set = uml::runtime::guest_memory::launch_memory_set;
	require(defaults.application == set.application);
	require(defaults.engine_rest == set.engine_rest);
	require(defaults.headroom == set.headroom);
	require(defaults.application == 64 * mib);
	require(defaults.engine_rest == 320 * mib);
	require(defaults.headroom == 149 * mib);
	require(!(defaults == no_terms), "the set counts terms");
}

void theLaunchParametersAreSubtracted()
{
	const LimitScope scope;
	const std::vector<std::string> arguments{"uml-darwin", "mem=1536M",
		"--memory-limit=2867M", "--gadget-cache-budget=358M", "ncpus=2"};
	const auto plan = emulator::plan_gpu_budget(arguments,
		{.application = 300 * mib, .engine_rest = 50 * mib,
			.headroom = 100 * mib});
	require(plan.engine == 416 * mib, "E is the cache, the tags and E_rest");
	require(plan.budget.available ==
		(2867 - 1536 - 416 - 300 - 100) * mib);
	require(plan.budget.bytes == 515 * mib, "the remainder");
	require(!plan.budget.short_of_floor);

	const auto exact = emulator::plan_gpu_budget(arguments,
		{.application = 300 * mib, .engine_rest = 50 * mib,
			.headroom = 231 * mib});
	require(exact.budget.available == 384 * mib);
	require(exact.budget.bytes == 384 * mib, "the floor");
	require(!exact.budget.short_of_floor, "the floor itself is not short");

	const auto starved = emulator::plan_gpu_budget(arguments,
		{.application = 400 * mib, .engine_rest = 50 * mib,
			.headroom = 200 * mib});
	require(starved.budget.short_of_floor);
	require(starved.budget.bytes == 384 * mib);
	const auto line = emulator::describe_gpu_budget_shortfall(starved);
	require(line.find('\n') == std::string::npos, "one line");
	require(line.find("2867 MiB") != std::string::npos, line.c_str());
	require(line.find("384 MiB") != std::string::npos, line.c_str());
}

void noLimitLeavesHalfTheGuest()
{
	const LimitScope scope;
	if (platform::process_memory_limit_bytes() != 0)
		return; /* a host that sets its own limit */
	const auto plan = emulator::plan_gpu_budget(
		std::vector<std::string>{"uml-darwin", "mem=2048M"},
		{.application = 500 * mib, .headroom = 500 * mib});
	require(plan.process_limit == 0);
	require(plan.budget.bytes == 1024 * mib);
	require(!plan.budget.short_of_floor);
}

} // namespace

int main()
{
	static constexpr uml::testing::NamedTest tests[] {
		{"a-launch-parameter-is-a-number-with-an-optional-suffix",
			aLaunchParameterIsANumberWithAnOptionalSuffix},
		{"the-arguments-state-the-limit-and-the-cache",
			theArgumentsStateTheLimitAndTheCache},
		{"the-vcpu-count-comes-from-the-last-valid-argument",
			theVCPUCountComesFromTheLastValidArgument},
		{"the-derived-gadget-cache-counts-as-engine-memory",
			theDerivedGadgetCacheCountsAsEngineMemory},
		{"the-plan-agrees-with-the-maximum-guest-memory",
			thePlanAgreesWithTheMaximumGuestMemory},
		{"the-plan-at-the-default-leaves-the-gpu-its-floor",
			thePlanAtTheDefaultLeavesTheGPUItsFloor},
		{"the-gpu-budget-of-the-default-on-an-ipad-mini",
			theGPUBudgetOfTheDefaultOnAnIPadMini},
		{"a-launcher-takes-the-default-guest-memory-only-where-a-limit-is-stated",
			aLauncherTakesTheDefaultGuestMemoryOnlyWhereALimitIsStated},
		{"the-vcpu-count-moves-the-budget", theVCPUCountMovesTheBudget},
		{"the-launch-parameters-default-to-the-launch-set",
			theLaunchParametersDefaultToTheLaunchSet},
		{"the-launch-parameters-are-subtracted",
			theLaunchParametersAreSubtracted},
		{"no-limit-leaves-half-the-guest", noLimitLeavesHalfTheGuest},
	};
	return uml::testing::runNamedTests(tests);
}
