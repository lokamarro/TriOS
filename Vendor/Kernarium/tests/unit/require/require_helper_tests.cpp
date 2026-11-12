// SPDX-License-Identifier: GPL-2.0-or-later
/* The shared unit-test helpers that do more than assert. */

#include "named_tests.hpp"
#include "require.hpp"
#include "wait_until.hpp"
#include "watchdog.hpp"
#include "wire_bytes.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace std::chrono_literals;

using uml::testing::require;

/* The exit status of a child whose assertion failed: it catches the SIGABRT
 * that `fail` raises, so that no crash report is written. */
constexpr int aborted = 99;

struct Outcome final {
	int status{};
	std::string output;
};

/* Runs `body` in a child and returns its exit status with everything it
 * wrote to stdout and stderr. Call it while no other thread exists. */
template<typename Body>
Outcome inChild(Body &&body)
{
	int pipes[2];
	require(::pipe(pipes) == 0, "cannot create a pipe");
	const pid_t child = ::fork();
	require(child >= 0, "cannot fork");
	if (child == 0) {
		::close(pipes[0]);
		::dup2(pipes[1], STDOUT_FILENO);
		::dup2(pipes[1], STDERR_FILENO);
		std::signal(SIGABRT, [](int) { ::_exit(aborted); });
		const int status = body();
		std::fflush(nullptr);
		::_exit(status);
	}
	::close(pipes[1]);
	Outcome outcome;
	char buffer[256];
	for (ssize_t count; (count = ::read(pipes[0], buffer, sizeof buffer)) > 0;)
		outcome.output.append(buffer, static_cast<std::size_t>(count));
	::close(pipes[0]);
	int status{};
	require(::waitpid(child, &status, 0) == child, "cannot wait for child");
	require(WIFEXITED(status), "the child did not exit");
	outcome.status = WEXITSTATUS(status);
	return outcome;
}

[[nodiscard]] bool mentions(const Outcome &outcome, const char *text)
{
	return outcome.output.find(text) != std::string::npos;
}

void waitUntilReturnsWhatTheDeadlineLeavesTrue()
{
	int calls = 0;
	require(uml::testing::waitUntil([&] { ++calls; return true; }, 10s),
		"a wait that holds returns at once");
	require(calls == 1, "a wait that holds is judged once");

	const auto started = std::chrono::steady_clock::now();
	require(!uml::testing::waitUntil([] { return false; }, 30ms),
		"a wait that never holds times out");
	require(std::chrono::steady_clock::now() - started >= 30ms,
		"a timeout waits out the whole limit");

	calls = 0;
	require(uml::testing::waitUntil([&] { return ++calls == 2; }, 0ms),
		"the condition is judged once more after the deadline");

	std::atomic<bool> flag{};
	std::thread setter{[&] {
		std::this_thread::sleep_for(20ms);
		flag.store(true);
	}};
	require(uml::testing::waitUntil([&] { return flag.load(); }, 10s, 0ms),
		"a spinning wait sees another thread's store");
	setter.join();
}

void aWatchdogOnlyFiresWhenTheGuardedCodeIsLate()
{
	const auto started = std::chrono::steady_clock::now();
	{
		uml::testing::Watchdog watchdog{"a quick call", 10s};
	}
	require(std::chrono::steady_clock::now() - started < 5s,
		"ending a watch does not wait out its limit");

	const auto late = inChild([] {
		uml::testing::Watchdog watchdog{"the late call", 100ms};
		std::this_thread::sleep_for(10s);
		return 0;
	});
	require(late.status == EXIT_FAILURE, "a late call ends the process");
	require(mentions(late, "FAIL: the late call did not return within 0.1 s"),
		"a late call is named");
}

int cases;

void passes() { ++cases; }
void throws() { throw std::runtime_error("boom"); }

void runNamedTestsReportsEveryCaseAndFailsOnAThrow()
{
	const uml::testing::NamedTest tests[] = {
		{"first", passes},
		{"second", throws},
		{"third", passes, "no such host"},
		{"fourth", passes},
	};
	const auto outcome = inChild([&] {
		const int result = uml::testing::runNamedTests(tests);
		return result == 1 && cases == 2 ? 1 : 2;
	});
	require(outcome.status == 1, "a throwing case fails the binary");
	const auto first = outcome.output.find("ok: first (");
	const auto second = outcome.output.find("FAIL: second: boom");
	const auto third = outcome.output.find("unsupported: third: no such host");
	const auto fourth = outcome.output.find("ok: fourth (");
	require(first < second && second < third && third < fourth &&
		fourth != std::string::npos,
		"the cases are reported in order, and the later ones still run");
	require(!mentions(outcome, "ok: second") &&
		!mentions(outcome, "ok: third"),
		"a failed or skipped case is not reported as ok");

	const uml::testing::NamedTest clean[] = {{"only", passes}};
	require(uml::testing::runNamedTests(clean) == 0, "clean cases return 0");
}

void requireSyscallNamesTheErrnoOfTheFailedCall()
{
	errno = EINTR;
	uml::testing::requireSyscall(true, "a call that succeeded");

	const auto failed = inChild([] {
		errno = ENOENT;
		uml::testing::requireSyscall(false, "cannot open the file");
		return 0;
	});
	require(failed.status == aborted, "a failed call ends the process");
	require(mentions(failed,
			"cannot open the file: No such file or directory (errno 2)"),
		"a failed call is reported with its errno");
	require(mentions(failed, "require_helper_tests.cpp:"),
		"a failed call is reported at its caller");
}

struct Header final {
	std::uint16_t kind;
	std::uint32_t length;
};

void wireBytesRoundTripAndRefuseTheWrongSize()
{
	using uml::testing::load;
	using uml::testing::loadExact;

	const Header header{7, 0x01020304};
	auto wire = uml::testing::bytes(header);
	require(wire.size() == sizeof header, "bytes() is the record");
	uml::testing::append(wire, std::uint8_t{9});
	require(uml::testing::bytes(std::uint16_t{1}, std::uint32_t{2}).size() == 6,
		"bytes() joins its values");
	require(load<Header>(wire).kind == 7 &&
		load<Header>(wire).length == 0x01020304,
		"load() reads the record");
	require(load<std::uint8_t>(wire, sizeof header) == 9,
		"load() reads at an offset");
	const auto exact = std::span<const std::uint8_t>{wire}.first(sizeof header);
	require(loadExact<Header>(exact).kind == 7,
		"loadExact() reads a record of exactly that size");

	const auto refused = [](auto &&read) {
		return inChild([&] { read(); return 0; }).status == aborted;
	};
	const auto shortBytes = std::span<const std::uint8_t>{wire}.first(5);
	require(refused([&] { (void)load<Header>(shortBytes); }),
		"load() refuses bytes that are too short");
	require(refused([&] { (void)load<Header>(wire, wire.size()); }),
		"load() refuses an offset past the record");
	require(refused([&] { (void)loadExact<Header>(wire); }),
		"loadExact() refuses bytes that are too long");
	require(refused([&] { (void)loadExact<Header>(shortBytes); }),
		"loadExact() refuses bytes that are too short");
}

} // namespace

int main()
{
	waitUntilReturnsWhatTheDeadlineLeavesTrue();
	aWatchdogOnlyFiresWhenTheGuardedCodeIsLate();
	runNamedTestsReportsEveryCaseAndFailsOnAThrow();
	requireSyscallNamesTheErrnoOfTheFailedCall();
	wireBytesRoundTripAndRefuseTheWrongSize();
	return 0;
}
