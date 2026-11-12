// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * darwin_uml_guest_shutdown(), the bridge import through which the guest
 * kernel says how its run ends: what the run's context records, what the host
 * logs, and what the record keeps across the end of a run.
 */
#include "device/guest_end/guest_end.hpp"
#include "platform/run_routing.hpp"
#include "require.hpp"
#include "runtime/context.hpp"
#include "uml_darwin_host.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace std::chrono_literals;
using uml::device::GuestEndRegistration;
using uml::runtime::Context;
using uml::runtime::GuestShutdown;
using uml::runtime::GuestShutdownKind;
using uml::runtime::RunScope;
using uml::testing::require;

constexpr std::size_t command_limit =
	uml::runtime::guest_shutdown_command_limit;

/* What the import writes to standard error while `operation` runs. */
template<typename Operation>
std::string logged(Operation &&operation)
{
	int pipe_descriptors[2];
	require(pipe(pipe_descriptors) == 0);
	std::fflush(stderr);
	const int saved = dup(STDERR_FILENO);
	require(saved >= 0);
	require(dup2(pipe_descriptors[1], STDERR_FILENO) >= 0);
	operation();
	require(dup2(saved, STDERR_FILENO) >= 0);
	close(saved);
	close(pipe_descriptors[1]);
	std::string text;
	char buffer[1024];
	for (;;) {
		const ssize_t count = read(pipe_descriptors[0], buffer, sizeof(buffer));
		if (count <= 0)
			break;
		text.append(buffer, static_cast<std::size_t>(count));
	}
	close(pipe_descriptors[0]);
	return text;
}

std::string command_of(const GuestShutdown &record)
{
	return std::string{record.command};
}

/* The import resolves the run's context through the active route, as the
 * kernel's call does. */
class RoutedContext final {
public:
	RoutedContext() noexcept = default;
	RoutedContext(const RoutedContext &) = delete;
	RoutedContext &operator=(const RoutedContext &) = delete;

	[[nodiscard]] Context &context() noexcept { return context_; }

private:
	Context context_;
	uml::routing::ActiveRouteScope route_{&context_, nullptr};
};

void test_each_kind_is_recorded_and_logged()
{
	RoutedContext routed;
	auto &context = routed.context();
	RunScope run{context};
	require(run.entered());
	require(context.guest_shutdown().kind == GuestShutdownKind::none);
	require(command_of(context.guest_shutdown()).empty());

	struct Case {
		int kind;
		GuestShutdownKind recorded;
		const char *line;
	};
	const Case cases[] = {
		{DARWIN_UML_SHUTDOWN_POWER_OFF, GuestShutdownKind::power_off,
			"guest powered off\n"},
		{DARWIN_UML_SHUTDOWN_HALT, GuestShutdownKind::halt,
			"guest halted\n"},
		{DARWIN_UML_SHUTDOWN_RESTART, GuestShutdownKind::restart,
			"guest requested restart\n"},
	};
	for (const auto &entry : cases) {
		const auto text = logged([&] {
			darwin_uml_guest_shutdown(entry.kind, nullptr);
		});
		require(text == entry.line, text);
		const auto record = context.guest_shutdown();
		require(record.kind == entry.recorded);
		require(command_of(record).empty());
	}
}

void test_restart_command_is_recorded_and_named()
{
	RoutedContext routed;
	auto &context = routed.context();
	RunScope run{context};
	require(run.entered());

	const auto text = logged([] {
		darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART,
			"recovery");
	});
	require(text == "guest requested restart (command 'recovery')\n", text);
	const auto record = context.guest_shutdown();
	require(record.kind == GuestShutdownKind::restart);
	require(command_of(record) == "recovery");

	/* An empty command is no command. */
	const auto empty = logged([] {
		darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, "");
	});
	require(empty == "guest requested restart\n", empty);
	require(command_of(context.guest_shutdown()).empty());
}

void test_null_command_means_no_command()
{
	RoutedContext routed;
	auto &context = routed.context();
	RunScope run{context};
	require(run.entered());

	darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, "bootloader");
	require(command_of(context.guest_shutdown()) == "bootloader");
	/* The next report replaces the whole record, its command included. */
	darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, nullptr);
	const auto record = context.guest_shutdown();
	require(record.kind == GuestShutdownKind::restart);
	require(command_of(record).empty());
}

void test_command_is_cut_at_255_bytes()
{
	RoutedContext routed;
	auto &context = routed.context();
	RunScope run{context};
	require(run.entered());
	static_assert(command_limit == 255);

	/* Exactly the limit stays whole; a longer command keeps its first 255
	 * bytes and the byte after them is not read into the record. */
	std::string exact;
	for (std::size_t index = 0; index < command_limit; ++index)
		exact.push_back(static_cast<char>('a' + index % 26));
	darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, exact.c_str());
	require(command_of(context.guest_shutdown()) == exact);

	/* The cut command equals the record, which a repeat would not log. */
	darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, "short");
	const std::string longer = exact + "TAIL" + std::string(300, 'z');
	const auto text = logged([&] {
		darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART,
			longer.c_str());
	});
	const auto record = context.guest_shutdown();
	require(record.kind == GuestShutdownKind::restart);
	require(command_of(record) == exact);
	require(record.command[command_limit] == '\0');
	require(text == "guest requested restart (command '" + exact + "')\n",
		text);

	const std::string one_over = exact + "X";
	darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, one_over.c_str());
	require(command_of(context.guest_shutdown()) == exact);
}

void test_a_repeated_report_is_logged_once()
{
	RoutedContext routed;
	auto &context = routed.context();
	RunScope run{context};
	require(run.entered());

	/* The kernel reports from a reboot notifier and again from
	 * machine_restart(): one line, one record. */
	const auto first = logged([] {
		darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, "recovery");
	});
	require(first == "guest requested restart (command 'recovery')\n", first);
	const auto again = logged([] {
		darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, "recovery");
	});
	require(again.empty(), again);
	auto record = context.guest_shutdown();
	require(record.kind == GuestShutdownKind::restart);
	require(command_of(record) == "recovery");

	/* A report that differs is a new fact: a different command, a different
	 * kind, and the same report after a new run cleared the record. */
	const auto other = logged([] {
		darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, "bootloader");
	});
	require(other == "guest requested restart (command 'bootloader')\n",
		other);
	const auto kind = logged([] {
		darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_HALT, nullptr);
	});
	require(kind == "guest halted\n", kind);
	const auto repeat = logged([] {
		darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_HALT, nullptr);
	});
	require(repeat.empty(), repeat);
}

void test_log_line_keeps_control_characters_out()
{
	RoutedContext routed;
	auto &context = routed.context();
	RunScope run{context};
	require(run.entered());

	const auto text = logged([] {
		darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART,
			"a\nb\x1b" "c\x7f" "d");
	});
	require(text == "guest requested restart (command 'a?b?c?d')\n", text);
	/* The record keeps what the guest sent. */
	require(command_of(context.guest_shutdown()) == "a\nb\x1b" "c\x7f" "d");
}

void test_later_report_replaces_the_earlier()
{
	RoutedContext routed;
	auto &context = routed.context();
	RunScope run{context};
	require(run.entered());

	darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, "recovery");
	darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_HALT, nullptr);
	const auto record = context.guest_shutdown();
	require(record.kind == GuestShutdownKind::halt);
	require(command_of(record).empty());
}

void test_record_outlives_the_run_and_the_next_run_clears_it()
{
	RoutedContext routed;
	auto &context = routed.context();
	{
		RunScope run{context};
		require(run.entered());
		darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, "recovery");
	}
	/* The owner reads the record after the run has returned. */
	auto record = context.guest_shutdown();
	require(record.kind == GuestShutdownKind::restart);
	require(command_of(record) == "recovery");
	{
		RunScope run{context};
		require(run.entered());
		record = context.guest_shutdown();
		require(record.kind == GuestShutdownKind::none);
		require(command_of(record).empty());
	}
	require(context.guest_shutdown().kind == GuestShutdownKind::none);
}

void test_record_is_readable_while_an_exitcall_never_returns()
{
	RoutedContext routed;
	auto &context = routed.context();
	RunScope run{context};
	require(run.entered());

	std::mutex mutex;
	std::condition_variable changed;
	bool reported = false;
	bool release = false;
	/* The kernel's thread reports and then hangs in an exitcall. */
	std::thread kernel{[&] {
		darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, "recovery");
		std::unique_lock lock{mutex};
		reported = true;
		changed.notify_all();
		changed.wait(lock, [&] { return release; });
	}};
	{
		std::unique_lock lock{mutex};
		require(changed.wait_for(lock, 30s, [&] { return reported; }));
	}
	const auto record = context.guest_shutdown();
	require(record.kind == GuestShutdownKind::restart);
	require(command_of(record) == "recovery");
	{
		std::lock_guard lock{mutex};
		release = true;
	}
	changed.notify_all();
	kernel.join();
}

void test_an_unknown_kind_records_nothing()
{
	RoutedContext routed;
	auto &context = routed.context();
	RunScope run{context};
	require(run.entered());

	darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_HALT, nullptr);
	const auto text = logged([] {
		darwin_uml_guest_shutdown(0, "ignored");
		darwin_uml_guest_shutdown(99, nullptr);
	});
	require(text.find("guest reported shutdown kind 0") !=
		std::string::npos, text);
	require(text.find("guest reported shutdown kind 99") !=
		std::string::npos, text);
	const auto record = context.guest_shutdown();
	require(record.kind == GuestShutdownKind::halt);
	require(command_of(record).empty());
}

void test_without_a_run_context_the_report_is_only_logged()
{
	/* No route exists here: the kernel's thread resolves no context, so
	 * nothing is recorded, and the report still reaches the log. */
	const auto text = logged([] {
		darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, "recovery");
	});
	require(text == "guest requested restart (command 'recovery')\n", text);
}

/* What a device registered to learn that the guest is ending is told once
 * per report that is new to the run: the kernel reports from a reboot
 * notifier and again from machine_restart(), and a device that discards its
 * held requests on the first has nothing to do for the second. */
struct Notified final {
	std::atomic_int calls{};

	static void call(void *context) noexcept
	{
		static_cast<Notified *>(context)->calls.fetch_add(1);
	}
};

void test_a_new_report_tells_the_registered_devices_once()
{
	Notified notified;
	Notified unregistered;
	RoutedContext routed;
	auto &context = routed.context();
	{
		GuestEndRegistration registration{&Notified::call, &notified};
		{
			GuestEndRegistration gone{&Notified::call, &unregistered};
		}
		RunScope run{context};
		require(run.entered());
		require(notified.calls.load() == 0);

		(void)logged([] {
			darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, "recovery");
		});
		require(notified.calls.load() == 1);
		/* The repeat from machine_restart() is not a new report. */
		(void)logged([] {
			darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, "recovery");
		});
		require(notified.calls.load() == 1);
		/* A report that differs is: the guest's end is idempotent for the
		 * devices, but each fact reaches them. */
		(void)logged([] {
			darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_HALT, nullptr);
		});
		require(notified.calls.load() == 2);
		/* A kind the bridge does not know is no report. */
		(void)logged([] { darwin_uml_guest_shutdown(99, nullptr); });
		require(notified.calls.load() == 2);
	}
	/* The next run clears the record, so its first report is new again. */
	{
		GuestEndRegistration registration{&Notified::call, &notified};
		RunScope run{context};
		require(run.entered());
		(void)logged([] {
			darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_HALT, nullptr);
		});
		require(notified.calls.load() == 3);
	}
	/* Unregistered devices are not called, and nothing registered is fine. */
	{
		RunScope run{context};
		require(run.entered());
		(void)logged([] {
			darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_HALT, nullptr);
		});
	}
	require(notified.calls.load() == 3);
	require(unregistered.calls.load() == 0);
}

/* A run with no context reports to nobody: its devices are not told. */
void test_without_a_run_context_no_device_is_told()
{
	Notified notified;
	GuestEndRegistration registration{&Notified::call, &notified};
	(void)logged([] {
		darwin_uml_guest_shutdown(DARWIN_UML_SHUTDOWN_RESTART, "recovery");
	});
	require(notified.calls.load() == 0);
}

void test_the_registry_has_a_bounded_table_and_frees_slots()
{
	Notified notified;
	std::vector<std::unique_ptr<GuestEndRegistration>> held;
	bool full = false;
	for (int attempt = 0; attempt < 64 && !full; ++attempt) {
		try {
			held.push_back(std::make_unique<GuestEndRegistration>(
				&Notified::call, &notified));
		} catch (const std::length_error &) {
			full = true;
		}
	}
	require(full);
	require(!held.empty());
	uml::device::notify_guest_end();
	require(notified.calls.load() == static_cast<int>(held.size()));
	held.pop_back();
	/* A freed slot is taken again. */
	GuestEndRegistration again{&Notified::call, &notified};
	bool refused = false;
	try {
		GuestEndRegistration over{&Notified::call, &notified};
	} catch (const std::length_error &) {
		refused = true;
	}
	require(refused);
	held.clear();
	bool rejected = false;
	try {
		GuestEndRegistration none{nullptr, nullptr};
	} catch (const std::invalid_argument &) {
		rejected = true;
	}
	require(rejected);
}

} // namespace

int main()
{
	/* First: later tests route a context of their own. */
	test_without_a_run_context_the_report_is_only_logged();
	test_without_a_run_context_no_device_is_told();
	test_each_kind_is_recorded_and_logged();
	test_restart_command_is_recorded_and_named();
	test_null_command_means_no_command();
	test_command_is_cut_at_255_bytes();
	test_a_repeated_report_is_logged_once();
	test_log_line_keeps_control_characters_out();
	test_later_report_replaces_the_earlier();
	test_record_outlives_the_run_and_the_next_run_clears_it();
	test_record_is_readable_while_an_exitcall_never_returns();
	test_an_unknown_kind_records_nothing();
	test_a_new_report_tells_the_registered_devices_once();
	test_the_registry_has_a_bounded_table_and_frees_slots();
	std::puts("guest shutdown import: ok");
	return 0;
}
