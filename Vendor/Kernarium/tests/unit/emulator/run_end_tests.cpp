// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A run says how its guest ended. The kernel reports it before its exitcalls
 * run, the host's own stop request wins over the report, a kernel that said
 * nothing failed, and a run that did not run a guest ends nothing. Each run
 * starts from no end, so a later run never repeats an earlier one's.
 */
#include "uml/emulator.h"
#include "context.hpp"
#include "emulator_test_support.hpp"
#include "require.hpp"
#include "testing.hpp"
#include "uml_darwin_host.h"
#include "scratch_directory.hpp"
#include "wait_until.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using ::uml::runtime::GuestShutdownKind;
using uml::testing::make_emulator;
using uml::testing::require;

/* What the next kernel entry does. */
struct Script final {
	uml_emulator *emulator{};
	bool report{};
	GuestShutdownKind kind{GuestShutdownKind::none};
	const char *command{};
	bool request_stop{};
	int status{};
	int disk{-1};
	/* What the handle reported while the kernel ran. */
	int during_result{};
	uml_emulator_run_end during{};
};

Script script;

int scripted_kernel(int, char *const[], char *const[])
{
	if (script.disk >= 0) {
		struct flock lock {};
		lock.l_type = F_WRLCK;
		lock.l_whence = SEEK_SET;
		require(darwin_uml_fcntl(script.disk, F_SETLK,
			reinterpret_cast<long>(&lock)) == 0,
			"the kernel could not lock its disk");
	}
	script.during = {
		.struct_size = sizeof(script.during),
		.kind = UML_EMULATOR_RUN_END_FAILED,
		.restart_command = "stale",
	};
	script.during_result =
		uml_emulator_get_run_end(script.emulator, &script.during);
	if (script.request_stop)
		uml_emulator_request_stop(script.emulator);
	if (script.report)
		::uml::emulator::testing::report_guest_shutdown(
			script.kind, script.command);
	return script.status;
}

uml_emulator_run_end run_end(const uml_emulator *emulator)
{
	uml_emulator_run_end end{
		.struct_size = sizeof(end),
		.kind = UML_EMULATOR_RUN_END_FAILED,
		.restart_command = "stale",
	};
	require(uml_emulator_get_run_end(emulator, &end) == UML_EMULATOR_OK,
		"the run end could not be read");
	return end;
}

int run(uml_emulator *emulator, const char *extra = nullptr)
{
	const char *arguments[]{"uml-run-end", extra};
	return uml_emulator_run(emulator, extra == nullptr ? 1 : 2, arguments,
		nullptr);
}

void test_arguments_are_checked()
{
	auto emulator = make_emulator(scripted_kernel);
	uml_emulator_run_end end{.struct_size = sizeof(end)};
	require(uml_emulator_get_run_end(nullptr, &end) ==
		-UML_EMULATOR_INVALID_ARGUMENT, "a null handle was accepted");
	require(uml_emulator_get_run_end(emulator.get(), nullptr) ==
		-UML_EMULATOR_INVALID_ARGUMENT, "a null output was accepted");
	end.struct_size = sizeof(end) - 1;
	require(uml_emulator_get_run_end(emulator.get(), &end) ==
		-UML_EMULATOR_INVALID_ARGUMENT, "a short structure was accepted");
	const auto fresh = run_end(emulator.get());
	require(fresh.kind == UML_EMULATOR_RUN_END_NONE &&
		fresh.struct_size == sizeof(fresh) &&
		fresh.restart_command[0] == '\0',
		"a handle that never ran reported an end");
}

void test_each_end_is_reported()
{
	auto emulator = make_emulator(scripted_kernel);
	script = {.emulator = emulator.get(), .status = 7};

	/* The kernel's own status is returned unchanged whatever the end. */
	script.report = true;
	script.kind = GuestShutdownKind::restart;
	script.command = "recovery";
	require(run(emulator.get()) == 7, "the kernel's status was changed");
	auto end = run_end(emulator.get());
	require(end.kind == UML_EMULATOR_RUN_END_RESTART &&
		std::strcmp(end.restart_command, "recovery") == 0,
		"a restart was not reported with its command");
	require(script.during_result == UML_EMULATOR_OK &&
		script.during.kind == UML_EMULATOR_RUN_END_NONE &&
		script.during.restart_command[0] == '\0',
		"a running guest reported an end");

	script.command = nullptr;
	require(run(emulator.get()) == 7, "the second run failed");
	end = run_end(emulator.get());
	require(end.kind == UML_EMULATOR_RUN_END_RESTART &&
		end.restart_command[0] == '\0',
		"a restart without a command kept the earlier command");

	const std::string long_command(400, 'x');
	script.command = long_command.c_str();
	require(run(emulator.get()) == 7, "the third run failed");
	end = run_end(emulator.get());
	require(end.kind == UML_EMULATOR_RUN_END_RESTART &&
		std::strlen(end.restart_command) == 255,
		"a long restart command was not cut to 255 bytes");

	for (const auto kind : {GuestShutdownKind::power_off,
		     GuestShutdownKind::halt}) {
		script.kind = kind;
		script.command = "ignored";
		require(run(emulator.get()) == 7, "a power-off run failed");
		end = run_end(emulator.get());
		require(end.kind == UML_EMULATOR_RUN_END_POWER_OFF &&
			end.restart_command[0] == '\0',
			"a power-off or halt was not reported as a power-off");
	}

	script.report = false;
	require(run(emulator.get()) == 7, "a silent run failed");
	require(run_end(emulator.get()).kind == UML_EMULATOR_RUN_END_FAILED,
		"a kernel that said nothing did not fail");
}

void test_a_stop_wins_over_the_guest()
{
	auto emulator = make_emulator(scripted_kernel);
	script = {.emulator = emulator.get(), .report = true,
		.kind = GuestShutdownKind::restart, .command = "recovery",
		.request_stop = true};
	require(run(emulator.get()) == 0, "a stopped run failed");
	const auto end = run_end(emulator.get());
	require(end.kind == UML_EMULATOR_RUN_END_STOPPED &&
		end.restart_command[0] == '\0',
		"a stop did not win over the guest's restart");

	/* The stop is not inherited, and the record does not outlive its run. */
	script = {.emulator = emulator.get(), .report = true,
		.kind = GuestShutdownKind::power_off};
	require(run(emulator.get()) == 0, "the run after a stop failed");
	require(run_end(emulator.get()).kind == UML_EMULATOR_RUN_END_POWER_OFF,
		"a stop was inherited by the next run");
}

void test_a_run_that_ran_no_guest_ends_nothing()
{
	auto emulator = make_emulator(scripted_kernel);
	script = {.emulator = emulator.get(), .report = true,
		.kind = GuestShutdownKind::restart, .command = "recovery"};
	require(run(emulator.get()) == 0, "the first run failed");
	require(run_end(emulator.get()).kind == UML_EMULATOR_RUN_END_RESTART,
		"the restart was not reported");

	/* Arguments the handle rejects before it begins a run leave the record
	 * alone; a run that begins and is refused starts from no end. */
	require(uml_emulator_run(emulator.get(), -1, nullptr, nullptr) ==
		-UML_EMULATOR_INVALID_ARGUMENT, "negative argc was accepted");
	require(run_end(emulator.get()).kind == UML_EMULATOR_RUN_END_RESTART,
		"an argument check cleared the record");
	require(run(emulator.get(), "--execution-engine=nonsense") ==
		-UML_EMULATOR_INVALID_ARGUMENT, "an invalid engine was accepted");
	require(run_end(emulator.get()).kind == UML_EMULATOR_RUN_END_NONE,
		"a refused run repeated the earlier run's end");

	/* An informational run prints and returns: no guest ran. */
	require(run(emulator.get(), "--version") == 0,
		"an informational run failed");
	require(run_end(emulator.get()).kind == UML_EMULATOR_RUN_END_NONE,
		"an informational run ended a guest");

	/* A handle with no kernel runs nothing. */
	const uml_emulator_config no_kernel{
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
	};
	uml_emulator *bare = nullptr;
	require(uml_emulator_create(&no_kernel, &bare) == UML_EMULATOR_OK,
		"could not create a handle without a kernel");
	require(run(bare) == -UML_EMULATOR_UNSUPPORTED,
		"a handle without a kernel ran");
	require(run_end(bare).kind == UML_EMULATOR_RUN_END_NONE,
		"a handle without a kernel ended a guest");
	uml_emulator_destroy(bare);
}

void test_a_finished_run_releases_its_disk_lock()
{
	const uml::testing::ScratchSubdirectory directory{"run-end-lock.XXXXXX"};
	const auto path = (directory.path() / "data.img").string();
	const int disk = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
	require(disk >= 0, "create the guest disk");
	auto emulator = make_emulator(scripted_kernel);
	script = {.emulator = emulator.get(), .disk = disk};
	require(run(emulator.get()) == 0, "the guest run failed");
	/* The kernel may leave descriptors open until hand-back, but those
	 * descriptors must not keep iOS from suspending the idle application. */
	const pid_t child = fork();
	require(child >= 0, "start a separate disk owner");
	if (child == 0) {
		struct flock lock {};
		lock.l_type = F_WRLCK;
		lock.l_whence = SEEK_SET;
		_exit(fcntl(disk, F_SETLK, &lock) == 0 ? 0 : 1);
	}
	int status = 0;
	require(uml::testing::waitUntil([&] {
		return waitpid(child, &status, WNOHANG) == child;
	}, std::chrono::seconds{5}), "the disk owner did not finish");
	require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
		"a finished guest kept its host disk locked");
	close(disk);
}

} // namespace

int main()
{
	try {
		test_arguments_are_checked();
		test_each_end_is_reported();
		test_a_stop_wins_over_the_guest();
		test_a_run_that_ran_no_guest_ends_nothing();
		test_a_finished_run_releases_its_disk_lock();
	} catch (const std::exception &error) {
		std::fprintf(stderr, "run end test failed: %s\n", error.what());
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
