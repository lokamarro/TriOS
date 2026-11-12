// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * What a handle says of its running guest. The vCPU carriers a test kernel
 * starts stand for a guest's: the handle sums their CPU time from any thread
 * while its run is active, and says nothing before and after, nor for a handle
 * that does not run. The guest's memory figures come from the management
 * console, whose gating the lifecycle tests cover.
 */
#include "uml/emulator.h"
#include "emulator_test_support.hpp"
#include "named_spinner.hpp"
#include "named_tests.hpp"
#include "require.hpp"
#include "testing.hpp"

#include <cstdint>
#include <thread>

namespace {

using uml::testing::NamedSpinner;
using uml::testing::make_emulator;
using uml::testing::require;

constexpr std::uint64_t untouched = 0x5a5a5a5a5a5a5a5aULL;

uml_emulator_statistics untouchedStatistics()
{
	return {
		.struct_size = sizeof(uml_emulator_statistics),
		.guest_cpu_count = 0x5a5a5a5aU,
		.guest_cpu_time_ns = untouched,
		.guest_ram_bytes = untouched,
		.guest_ram_held_bytes = untouched,
	};
}

bool isUntouched(const uml_emulator_statistics &statistics)
{
	return statistics.guest_cpu_count == 0x5a5a5a5aU &&
		statistics.guest_cpu_time_ns == untouched &&
		statistics.guest_ram_bytes == untouched &&
		statistics.guest_ram_held_bytes == untouched;
}

/* What the test kernel does and what it saw. */
struct Script final {
	uml_emulator *emulator {};
	/* A handle that is not running, asked while `emulator` runs. */
	uml_emulator *idle {};
	std::uint64_t carrierNanoseconds {};
	int duringResult {};
	uml_emulator_statistics during {};
	int otherThreadResult {};
	uml_emulator_statistics otherThread {};
	int idleResult {};
	uml_emulator_statistics idleStatistics {};
};

Script script;

int scriptedKernel(int, char *const[], char *const[])
{
	NamedSpinner first {"uml-vcpu-0-carrier", std::chrono::milliseconds {40}};
	NamedSpinner second {"uml-vcpu-1-carrier", std::chrono::milliseconds {40}};
	NamedSpinner decoy {"uml-irq-notify", std::chrono::milliseconds {300}};
	script.carrierNanoseconds = first.awaitSpent() + second.awaitSpent();
	(void)decoy.awaitSpent();
	script.during = untouchedStatistics();
	script.duringResult =
		uml_emulator_get_statistics(script.emulator, &script.during);
	/* Any thread may ask: the carriers of a guest are not the caller's. */
	std::thread other {[] {
		script.otherThread = untouchedStatistics();
		script.otherThreadResult = uml_emulator_get_statistics(
			script.emulator, &script.otherThread);
		script.idleStatistics = untouchedStatistics();
		script.idleResult = uml_emulator_get_statistics(
			script.idle, &script.idleStatistics);
	}};
	other.join();
	return 0;
}

int run(uml_emulator *emulator)
{
	const char *arguments[] {"uml-statistics"};
	return uml_emulator_run(emulator, 1, arguments, nullptr);
}

void argumentsAreChecked()
{
	auto emulator = make_emulator(scriptedKernel);
	uml_emulator_statistics statistics = untouchedStatistics();
	require(uml_emulator_get_statistics(nullptr, &statistics) ==
		-UML_EMULATOR_INVALID_ARGUMENT, "a null handle was accepted");
	require(uml_emulator_get_statistics(emulator.get(), nullptr) ==
		-UML_EMULATOR_INVALID_ARGUMENT, "a null output was accepted");
	statistics.struct_size = sizeof(statistics) - 1U;
	require(uml_emulator_get_statistics(emulator.get(), &statistics) ==
		-UML_EMULATOR_INVALID_ARGUMENT, "a short structure was accepted");
	require(statistics.guest_cpu_time_ns == untouched,
		"a refused call wrote its output");

	uml_emulator_guest_memory memory {.struct_size = sizeof(memory)};
	require(uml_emulator_get_guest_memory(nullptr, &memory) ==
		-UML_EMULATOR_INVALID_ARGUMENT, "a null handle was accepted");
	require(uml_emulator_get_guest_memory(emulator.get(), nullptr) ==
		-UML_EMULATOR_INVALID_ARGUMENT, "a null output was accepted");
	memory.struct_size = sizeof(memory) - 1U;
	require(uml_emulator_get_guest_memory(emulator.get(), &memory) ==
		-UML_EMULATOR_INVALID_ARGUMENT, "a short structure was accepted");
}

void aHandleThatDoesNotRunHasNoStatistics()
{
	auto emulator = make_emulator(scriptedKernel);
	auto statistics = untouchedStatistics();
	require(uml_emulator_get_statistics(emulator.get(), &statistics) ==
			-UML_EMULATOR_UNSUPPORTED && isUntouched(statistics),
		"an idle handle reported statistics");

	/* No console stands where this process's guest would bind it, so the
	 * request is not sent and does not wait. */
	uml_emulator_guest_memory memory {.struct_size = sizeof(memory)};
	require(uml_emulator_get_guest_memory(emulator.get(), &memory) ==
		-UML_EMULATOR_UNSUPPORTED, "an idle handle asked a console");
}

void aRunningGuestHasStatistics()
{
	auto emulator = make_emulator(scriptedKernel);
	auto idle = make_emulator(scriptedKernel);
	script = {.emulator = emulator.get(), .idle = idle.get()};
	require(run(emulator.get()) == 0, "the run failed");

	/* The sum is the carriers' own time and no other thread's. */
	require(script.duringResult == UML_EMULATOR_OK,
		"a running guest has no statistics");
	require(script.during.struct_size == sizeof(script.during) &&
		script.during.guest_cpu_count == 2U,
		"the carriers were not counted");
	require(script.during.guest_cpu_time_ns + 1000000ULL >=
			script.carrierNanoseconds &&
		script.during.guest_cpu_time_ns <=
			script.carrierNanoseconds + 10000000ULL,
		"the sum is not the carriers' time");
	/* No test kernel claims guest RAM. */
	require(script.during.guest_ram_bytes == 0U &&
		script.during.guest_ram_held_bytes == 0U,
		"RAM was reported before the guest claimed it");

	require(script.otherThreadResult == UML_EMULATOR_OK &&
		script.otherThread.guest_cpu_count == 2U &&
		script.otherThread.guest_cpu_time_ns >=
			script.during.guest_cpu_time_ns,
		"another thread could not read the statistics");
	require(script.idleResult == -UML_EMULATOR_UNSUPPORTED &&
		isUntouched(script.idleStatistics),
		"a handle that does not run reported another's guest");

	/* The guest ends with its run. */
	auto after = untouchedStatistics();
	require(uml_emulator_get_statistics(emulator.get(), &after) ==
			-UML_EMULATOR_UNSUPPORTED && isUntouched(after),
		"a handle reported statistics after its run");
}

} // namespace

int main()
{
	constexpr uml::testing::NamedTest tests[] = {
		{"arguments-are-checked", argumentsAreChecked},
		{"a-handle-that-does-not-run-has-no-statistics",
			aHandleThatDoesNotRunHasNoStatistics},
		{"a-running-guest-has-statistics", aRunningGuestHasStatistics},
	};
	return uml::testing::runNamedTests(tests);
}
