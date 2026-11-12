// SPDX-License-Identifier: GPL-2.0-or-later

#include "guest_console.hpp"
#include "guest_console_test_support.hpp"
#include "require.hpp"
#include "socket_paths.hpp"
#include "wait_until.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using uml::testing::awaitRequest;
using uml::testing::bindServer;
using uml::testing::receiveRequest;
using uml::testing::require;
using uml::testing::sendReply;
using uml::testing::waitUntil;

const ::uml::device::GuestConsoleLocation &processLocation()
{
	const auto &location = ::uml::device::guest_console_location();
	require(location.has_value());
	return *location;
}

std::string consolePath()
{
	return processLocation().console;
}

/* The host narrows the guest's console to its owner once it sends to it. */
void requireOwnerOnly(const std::string &path)
{
	struct stat details {};
	require(::lstat(path.c_str(), &details) == 0 &&
		(details.st_mode & 0777) == (S_IRUSR | S_IWUSR));
}

/* What the guest said, and when it arrived: the time since the first
 * request. */
struct Heard final {
	std::string command;
	std::chrono::milliseconds at;
};

using Reply = std::pair<std::uint32_t, std::string>;

/* What a suspend-capable guest replies when it gave a suspend up, busy or
 * for another reason, and to a writeback request. */
const Reply busySuspend {1U, "Suspend failed: -16"};
const Reply failedSuspend {1U, "Suspend failed: -5"};
const Reply enteredSuspend {0U, "suspended"};
const Reply writeback {0U, "SysRq : Emergency Sync"};

/* Answers each request with what `policy` gives for it and its index, the
 * first one only after `firstDelay`, and keeps listening for a while after the
 * last, so that a request too many shows in the list. A reply whose request
 * gave up finds no socket. */
void answerAll(int descriptor, std::chrono::milliseconds firstDelay,
	std::function<Reply(const std::string &, std::size_t)> policy,
	std::vector<Heard> &heard)
{
	sockaddr_un client {};
	socklen_t clientLength {};
	std::chrono::steady_clock::time_point first;
	for (int wait = 10000; awaitRequest(descriptor, wait); wait = 500) {
		const auto command = receiveRequest(descriptor, client, clientLength);
		const auto now = std::chrono::steady_clock::now();
		if (heard.empty())
			first = now;
		heard.push_back({command,
			std::chrono::duration_cast<std::chrono::milliseconds>(
				now - first)});
		if (heard.size() == 1U)
			std::this_thread::sleep_for(firstDelay);
		const auto &[error, text] = policy(command, heard.size() - 1U);
		(void)sendReply(descriptor, client, clientLength, error, text);
	}
}

/* Runs suspend_guest against a console that answers by `policy` and returns
 * what the console heard. */
std::vector<Heard> suspendAgainst(
	std::function<Reply(const std::string &, std::size_t)> policy,
	std::chrono::milliseconds firstDelay,
	::uml::device::GuestSuspension &suspension)
{
	const ::uml::device::GuestConsoleRun run {true};
	const int server = bindServer(consolePath());
	std::vector<Heard> heard;
	std::thread observer {answerAll, server, firstDelay, std::move(policy),
		std::ref(heard)};
	suspension = ::uml::device::suspend_guest(0U);
	observer.join();
	requireOwnerOnly(consolePath());
	(void)::close(server);
	return heard;
}

std::vector<std::string> commandsOf(const std::vector<Heard> &heard)
{
	std::vector<std::string> commands;
	for (const auto &entry : heard)
		commands.push_back(entry.command);
	return commands;
}

/* A console whose suspend does not reply in time. It records what it hears,
 * with the time since the first request, remembers where each suspend came
 * from, and answers a wake with "running" and a writeback request as a guest
 * does. What it says to a suspend's address, and when, is the scenario's. */
struct HeldSuspend final {
	/* Sent to the first suspend's address once this long after the request
	 * arrived; nothing when it is empty. */
	std::optional<Reply> firstReply;
	std::chrono::milliseconds firstReplyAfter {0};
	/* Sent to the latest suspend's address right after the first wake is
	 * answered: the final word of a suspend the wake aborted. */
	std::optional<Reply> replyToWake;
};

struct Held final {
	std::vector<Heard> heard;
	/* Whether each send to a suspend's address reached it: a client that
	 * gave up has removed its socket. */
	std::vector<bool> sent;
};

void serveHeldSuspend(int server, const HeldSuspend &scenario,
	const std::atomic_bool &done, Held &held)
{
	sockaddr_un client {};
	socklen_t clientLength {};
	sockaddr_un suspendClient {};
	socklen_t suspendClientLength {};
	sockaddr_un firstClient {};
	socklen_t firstClientLength {};
	std::chrono::steady_clock::time_point first;
	std::chrono::steady_clock::time_point firstReplyAt;
	bool firstReplyPending = false;
	std::size_t suspends = 0U;
	bool wakeAnswered = false;
	while (!done.load()) {
		if (firstReplyPending &&
		    std::chrono::steady_clock::now() >= firstReplyAt) {
			firstReplyPending = false;
			held.sent.push_back(sendReply(server, firstClient,
				firstClientLength, scenario.firstReply->first,
				scenario.firstReply->second));
		}
		if (!awaitRequest(server, 20))
			continue;
		const auto command = receiveRequest(server, client, clientLength);
		const auto now = std::chrono::steady_clock::now();
		if (held.heard.empty())
			first = now;
		held.heard.push_back({command,
			std::chrono::duration_cast<std::chrono::milliseconds>(
				now - first)});
		if (command == "suspend") {
			suspendClient = client;
			suspendClientLength = clientLength;
			if (++suspends == 1U && scenario.firstReply) {
				firstClient = client;
				firstClientLength = clientLength;
				firstReplyAt = now + scenario.firstReplyAfter;
				firstReplyPending = true;
			}
		} else if (command == "wake") {
			require(sendReply(server, client, clientLength, 0U, "running"));
			if (scenario.replyToWake && !wakeAnswered) {
				wakeAnswered = true;
				held.sent.push_back(sendReply(server, suspendClient,
					suspendClientLength, scenario.replyToWake->first,
					scenario.replyToWake->second));
			}
		} else if (command == "sysrq s") {
			require(sendReply(server, client, clientLength, writeback.first,
				writeback.second));
		}
	}
}

/* Runs suspend_guest and the resume that follows it against such a console
 * and returns what the console heard. */
Held holdSuspend(const HeldSuspend &scenario,
	::uml::device::GuestSuspension &suspension)
{
	const ::uml::device::GuestConsoleRun run {true};
	const int server = bindServer(consolePath());
	Held held;
	std::atomic_bool done {false};
	std::thread console {serveHeldSuspend, server, std::cref(scenario),
		std::cref(done), std::ref(held)};
	suspension = ::uml::device::suspend_guest(0U);
	::uml::device::resume_guest(suspension);
	/* A request too many shows too. */
	std::this_thread::sleep_for(std::chrono::milliseconds {300});
	done.store(true);
	console.join();
	(void)::close(server);
	return held;
}

const std::vector<std::string> abortedAndWritten {
	"suspend", "wake", "sysrq s"};
const std::vector<std::string> abortedWrittenAndWoken {
	"suspend", "wake", "sysrq s", "wake"};

/* A suspend with no reply at its deadline is aborted with a wake, not taken
 * for a suspended guest, and the final word of the request is read from its
 * own socket, which stays open for it: a freezer that cannot finish refuses
 * the suspend long after the host's wait. A refusal is a guest known to run,
 * which the resume leaves alone. */
void aSuspendThatDoesNotReplyIsAborted()
{
	auto suspension = ::uml::device::GuestSuspension::system;
	const auto held = holdSuspend(
		{.replyToWake = busySuspend}, suspension);
	require(suspension == ::uml::device::GuestSuspension::none);
	require(commandsOf(held.heard) == abortedAndWritten);
	/* One deadline for the whole suspend, from the request. */
	require(held.heard[1].at >= std::chrono::milliseconds {9500} &&
		held.heard[1].at < std::chrono::milliseconds {11000});
	require(held.sent == std::vector<bool> {true},
		"the late refusal reached the request's socket");
}

/* A guest that says nothing after the wake may be asleep or running. */
void aSilentSuspendIsUnconfirmed()
{
	auto suspension = ::uml::device::GuestSuspension::system;
	const auto held = holdSuspend({}, suspension);
	require(suspension == ::uml::device::GuestSuspension::unconfirmed);
	require(commandsOf(held.heard) == abortedWrittenAndWoken,
		"the resume wakes an unconfirmed guest");
	require(held.heard[1].at >= std::chrono::milliseconds {9500} &&
		held.heard[1].at < std::chrono::milliseconds {11000});
}

/* The guest replies "suspended" before it checks for a wake, so the word does
 * not prove the sleep once the wake has been sent. */
void aSuspendedReplyAfterTheWakeIsUnconfirmed()
{
	auto suspension = ::uml::device::GuestSuspension::system;
	const auto held = holdSuspend(
		{.replyToWake = enteredSuspend}, suspension);
	require(suspension == ::uml::device::GuestSuspension::unconfirmed);
	require(commandsOf(held.heard) == abortedWrittenAndWoken,
		"the resume wakes an unconfirmed guest");
	require(held.sent == std::vector<bool> {true});
}

/* A busy refusal late in the wait does not start a wait of its own: the
 * retry's reply is awaited only until the one deadline. */
void aBusyRetryCannotOutlastTheDeadline()
{
	auto suspension = ::uml::device::GuestSuspension::system;
	const auto held = holdSuspend(
		{.firstReply = busySuspend,
		 .firstReplyAfter = std::chrono::milliseconds {7900}},
		suspension);
	require(suspension == ::uml::device::GuestSuspension::unconfirmed);
	require(commandsOf(held.heard) == std::vector<std::string> {
		"suspend", "suspend", "wake", "sysrq s", "wake"});
	require(held.heard[2].at >= std::chrono::milliseconds {9500} &&
		held.heard[2].at < std::chrono::milliseconds {10500},
		"the wake follows the deadline, not the retry's own wait");
}

/* Every guest is refused its first suspend busy: it is asked again every
 * 250 ms for two seconds after the first refusal, nothing stops it, and when
 * it stays busy it is left running after the writeback. `refusedAt` is when
 * the first refusal reached the host. */
void requireRetriedUntilTheDeadline(const std::vector<Heard> &heard,
	std::chrono::milliseconds refusedAt)
{
	require(heard.size() >= 4U, "a busy guest is asked again");
	std::size_t suspends = 0U;
	for (const auto &entry : heard) {
		require(entry.command == "suspend" || entry.command == "sysrq s",
			"nothing but suspends and the writeback is sent, never a stop");
		suspends += entry.command == "suspend" ? 1U : 0U;
	}
	require(heard.back().command == "sysrq s" &&
		suspends == heard.size() - 1U, "the writeback comes last");
	/* The first retry waits out the interval, and no retry follows the
	 * deadline: seven fit in two seconds, whatever the host's load adds. */
	require(heard[1].at >= refusedAt + std::chrono::milliseconds {200});
	const auto &lastSuspend = heard[heard.size() - 2U];
	require(lastSuspend.at < refusedAt + std::chrono::milliseconds {3000},
		"a busy guest is asked only within the deadline");
	require(suspends <= 8U);
}

/* A guest that stays busy is retried until the deadline and then left
 * running, never stopped. */
void aBusySuspendIsRetriedUntilTheDeadline()
{
	auto suspension = ::uml::device::GuestSuspension::system;
	const auto heard = suspendAgainst(
		[](const std::string &command, std::size_t) {
			return command == "sysrq s" ? writeback : busySuspend;
		},
		std::chrono::milliseconds {0}, suspension);
	require(suspension == ::uml::device::GuestSuspension::none);
	requireRetriedUntilTheDeadline(heard, std::chrono::milliseconds {0});
}

/* The guest replies to a suspend only after syncing its filesystems, so a
 * refusal can arrive seconds after the request. The two seconds start with
 * the refusal. */
void aLateBusyRefusalIsRetriedOnlyWithinTheDeadline()
{
	auto suspension = ::uml::device::GuestSuspension::system;
	const auto heard = suspendAgainst(
		[](const std::string &command, std::size_t) {
			return command == "sysrq s" ? writeback : busySuspend;
		},
		std::chrono::milliseconds {3000}, suspension);
	require(suspension == ::uml::device::GuestSuspension::none);
	requireRetriedUntilTheDeadline(heard, std::chrono::milliseconds {3000});
}

/* A guest whose busy state passes suspends: no stop, no writeback. */
void aRetryThatSucceedsSuspendsTheGuest()
{
	auto suspension = ::uml::device::GuestSuspension::none;
	const auto heard = suspendAgainst(
		[](const std::string &, std::size_t index) {
			return index < 2U ? busySuspend : enteredSuspend;
		},
		std::chrono::milliseconds {0}, suspension);
	require(suspension == ::uml::device::GuestSuspension::system);
	require(commandsOf(heard) ==
		std::vector<std::string> {"suspend", "suspend", "suspend"});
}

/* Only a busy guest is asked again. A kernel without the suspend command, any
 * other refusal, and a retry that fails some other way leave the guest running
 * after one writeback. */
void aRefusalThatIsNotBusyIsNotRetried()
{
	using ::uml::device::GuestSuspension;
	const std::vector<std::string> once {"suspend", "sysrq s"};
	const Reply notConfigured {1U, "System suspend is not configured"};
	for (const auto &refusal : {notConfigured, failedSuspend}) {
		auto suspension = GuestSuspension::system;
		const auto heard = suspendAgainst(
			[&refusal](const std::string &command, std::size_t) {
				return command == "sysrq s" ? writeback : refusal;
			},
			std::chrono::milliseconds {0}, suspension);
		require(suspension == GuestSuspension::none);
		require(commandsOf(heard) == once, refusal.second.c_str());
	}
	auto suspension = GuestSuspension::system;
	const auto heard = suspendAgainst(
		[](const std::string &command, std::size_t index) {
			if (command == "sysrq s")
				return writeback;
			return index == 0U ? busySuspend : failedSuspend;
		},
		std::chrono::milliseconds {0}, suspension);
	require(suspension == GuestSuspension::none);
	require(commandsOf(heard) ==
		std::vector<std::string> {"suspend", "suspend", "sysrq s"});
}

/* Answers one request with `answer` and `error` after `delayMs`. A client
 * that stopped waiting has removed its socket, so that reply goes nowhere. */
void answerLater(int descriptor, std::string_view answer, std::uint32_t error,
	int delayMs, std::vector<std::string> &requests)
{
	sockaddr_un client {};
	socklen_t clientLength {};
	if (!awaitRequest(descriptor, 10000))
		return;
	requests.push_back(receiveRequest(descriptor, client, clientLength));
	std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
	(void)sendReply(descriptor, client, clientLength, error, answer);
}

/* Runs `request` while the console answers its command, which must be
 * `expected`, and returns the result. */
template<typename Request>
auto answered(int server, Request &&request, std::string_view answer,
	std::uint32_t error, int delayMs, std::string_view expected)
{
	std::vector<std::string> requests;
	std::thread observer {answerLater, server, answer, error, delayMs,
		std::ref(requests)};
	const auto result = request();
	observer.join();
	require(requests == std::vector<std::string> {std::string {expected}});
	return result;
}

/* The power key's wake is one request, whatever the host last asked for. */
void aWakeSendsOneWakeRequest()
{
	const ::uml::device::GuestConsoleRun run {true};
	const auto path = consolePath();
	/* No guest console yet: nothing answers and nothing waits. */
	require(!::uml::device::wake_guest());
	const int server = bindServer(path);
	require(answered(server, ::uml::device::wake_guest, "running", 0U, 0,
		"wake"));
	requireOwnerOnly(path);
	/* Only a guest that did it has woken. */
	require(!answered(server, ::uml::device::wake_guest, "Unknown command",
		1U, 0, "wake"));
	require(!awaitRequest(server, 200));
	(void)::close(server);
}

/* A sync is one emergency sync, which counts only if the guest started it. */
void aWritebackIsOneEmergencySync()
{
	using ::uml::device::request_guest_writeback;
	const ::uml::device::GuestConsoleRun run {true};
	const auto path = consolePath();
	require(!request_guest_writeback(0U));
	const int server = bindServer(path);
	const auto writeback = [] { return request_guest_writeback(0U); };
	require(answered(server, writeback, "SysRq : Emergency Sync", 0U, 0,
		"sysrq s"));
	requireOwnerOnly(path);
	/* A guest with magic SysRq disabled takes the request and does nothing. */
	require(!answered(server, writeback, "SysRq : This sysrq operation is "
		"disabled.", 0U, 0, "sysrq s"));
	require(!awaitRequest(server, 200));
	(void)::close(server);
}

/* Turning trimming on or off is one request, which a kernel without the
 * command refuses. */
void trimmingIsSwitchedByOneRequest()
{
	using ::uml::device::GuestRequest;
	using ::uml::device::set_guest_page_cache_trim;
	const ::uml::device::GuestConsoleRun run {true};
	const auto path = consolePath();
	/* No guest console yet: nothing is sent and nothing waits. */
	require(set_guest_page_cache_trim(true) == GuestRequest::unreachable);
	const int server = bindServer(path);
	require(answered(server, [] { return set_guest_page_cache_trim(true); },
			"on", 0U, 0, "trim on") == GuestRequest::done);
	requireOwnerOnly(path);
	require(answered(server, [] { return set_guest_page_cache_trim(false); },
			"off", 0U, 0, "trim off") == GuestRequest::done);
	require(answered(server, [] { return set_guest_page_cache_trim(true); },
			"Unknown command", 1U, 0, "trim on") == GuestRequest::refused);
	require(!awaitRequest(server, 200));
	(void)::close(server);
}

/* A pass takes longer than an ordinary request may wait, and is waited for
 * up to its own bound and no longer. */
void aTrimPassIsWaitedForUpToItsBound()
{
	using ::uml::device::GuestRequest;
	using ::uml::device::trim_guest_page_cache;
	const ::uml::device::GuestConsoleRun run {true};
	const int server = bindServer(consolePath());
	require(answered(server, [] { return trim_guest_page_cache(8000U); },
			"trimmed 4096 KiB in 3 ms, compacted in 40 ms, "
			"released 4096 KiB in 2 ms",
			0U, 2500,
			"trim once") == GuestRequest::done);
	/* The reply comes 2.5 seconds late; the request gives up long
	 * before, and before an ordinary request's two seconds. */
	std::chrono::steady_clock::duration waited {};
	require(answered(server,
			[&waited] {
				const auto start = std::chrono::steady_clock::now();
				const auto result = trim_guest_page_cache(200U);
				waited = std::chrono::steady_clock::now() - start;
				return result;
			},
			"trimmed 0 KiB in 0 ms, compacted in 40 ms, "
			"released 0 KiB in 1 ms",
			0U, 2500,
			"trim once") == GuestRequest::unanswered);
	require(waited < std::chrono::milliseconds(1800));
	(void)::close(server);
}

/* ---- The guest's memory figures: "mem". ---- */

using ::uml::device::GuestMemoryFigures;
using ::uml::device::GuestMemoryQuery;
using ::uml::device::GuestMemoryResult;
using ::uml::device::parse_guest_memory_reply;

/* The line a guest answers, and every way the host refuses to read one. */
void theMemoryReplyIsParsedStrictly()
{
	constexpr std::uint64_t gib = 1ULL << 30;
	const auto plain = parse_guest_memory_reply(
		"total=2147483648 available=1073741824 free=536870912");
	require(plain && plain->total_bytes == 2 * gib &&
		plain->available_bytes == gib && plain->free_bytes == gib / 2);
	/* The field order and a line end are the guest's to choose; a field it
	 * adds later is not ours to refuse. */
	const auto reordered = parse_guest_memory_reply(
		"free=7 available=5 total=9 swap=4\n");
	require(reordered && reordered->total_bytes == 9 &&
		reordered->available_bytes == 5 && reordered->free_bytes == 7);
	/* `free` is informational. */
	const auto withoutFree = parse_guest_memory_reply("total=9 available=5");
	require(withoutFree && withoutFree->free_bytes == 0);
	const auto largest = parse_guest_memory_reply(
		"total=18446744073709551615 available=0 free=0");
	require(largest && largest->total_bytes == UINT64_MAX);

	for (const std::string_view bad : {
		     "", " ", "total=1", "available=1", "total=1 free=1",
		     "total= available=1", "total=1 available=", "=1 total=1",
		     "total=-1 available=1", "total=+1 available=1",
		     "total=1.5 available=1", "total=0x10 available=1",
		     "total=1 available=1x", "total=1  available=1",
		     "total=1 available=1 ", " total=1 available=1",
		     "total=1 total=2 available=1",
		     "total=1 available=1 available=1",
		     "total=18446744073709551616 available=1",
		     "total 1 available 1", "total:1 available:1",
		     "Unknown command", "total=1 available=1 stray",
		     "total=1\navailable=1"})
		require(!parse_guest_memory_reply(bad),
			"a malformed memory reply was accepted");
}

/* Sends the request and waits for its reply with the console answering as
 * `policy` says. */
GuestMemoryResult askMemory(int server, std::string_view answer,
	std::uint32_t error, int delayMs, std::chrono::milliseconds wait,
	GuestMemoryFigures &figures, std::chrono::milliseconds &took)
{
	std::vector<std::string> requests;
	std::thread observer {answerLater, server, answer, error, delayMs,
		std::ref(requests)};
	const auto began = std::chrono::steady_clock::now();
	auto query = GuestMemoryQuery::start();
	require(query.has_value(), "the request was not sent");
	const auto result = query->await(wait, figures);
	took = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - began);
	observer.join();
	require(requests == std::vector<std::string> {"mem"},
		"the guest must hear one \"mem\"");
	return result;
}

void aMemoryRequestReadsTheGuestsFigures()
{
	const ::uml::device::GuestConsoleRun run {true};
	/* No guest console yet: nothing is sent. */
	require(!GuestMemoryQuery::start().has_value());
	const int server = bindServer(consolePath());
	GuestMemoryFigures figures;
	std::chrono::milliseconds took {};
	constexpr std::chrono::milliseconds bound {500};
	require(askMemory(server,
		"total=1913519104 available=1056964608 free=196083712", 0U, 0,
		bound, figures, took) == GuestMemoryResult::done);
	require(figures.total_bytes == 1913519104ULL &&
		figures.available_bytes == 1056964608ULL &&
		figures.free_bytes == 196083712ULL);
	requireOwnerOnly(consolePath());
	/* The kernel's driver counts the NUL that ends a reply in its length. */
	constexpr char terminated[] = "total=2048 available=1024 free=512";
	require(askMemory(server, std::string_view {terminated, sizeof(terminated)},
		0U, 0, bound, figures, took) == GuestMemoryResult::done);
	require(figures.total_bytes == 2048ULL &&
		figures.available_bytes == 1024ULL && figures.free_bytes == 512ULL);
	/* A kernel without the command refuses it. */
	require(askMemory(server, "Unknown command", 1U, 0, bound, figures,
		took) == GuestMemoryResult::refused);
	/* A guest that answers something else did the command no good. */
	require(askMemory(server, "total=1", 0U, 0, bound, figures, took) ==
		GuestMemoryResult::malformed);
	require(askMemory(server, "hello", 0U, 0, bound, figures, took) ==
		GuestMemoryResult::malformed);
	require(!awaitRequest(server, 200));
	(void)::close(server);
}

/* A guest that does not answer is waited for up to the bound and no longer,
 * and a reply that comes after the bound goes nowhere. The request can be
 * waited on again until it is destroyed. */
void aSilentGuestIsWaitedForUpToTheBound()
{
	const ::uml::device::GuestConsoleRun run {true};
	const int server = bindServer(consolePath());
	GuestMemoryFigures figures;
	std::chrono::milliseconds took {};
	require(askMemory(server,
		"total=100 available=50 free=10", 0U, 1500,
		std::chrono::milliseconds {200}, figures, took) ==
		GuestMemoryResult::unanswered);
	require(took >= std::chrono::milliseconds {150} &&
		took < std::chrono::milliseconds {1200},
		"the wait must end at its bound, not at the reply");
	/* A zero wait takes only what has arrived. */
	std::vector<std::string> requests;
	std::thread observer {answerLater, server, "total=8 available=4 free=2",
		0U, 300, std::ref(requests)};
	auto query = GuestMemoryQuery::start();
	require(query.has_value());
	require(query->await(std::chrono::milliseconds {0}, figures) ==
		GuestMemoryResult::unanswered);
	require(query->await(std::chrono::milliseconds {5000}, figures) ==
		GuestMemoryResult::done);
	require(figures.total_bytes == 8 && figures.available_bytes == 4);
	observer.join();
	(void)::close(server);
}

/* The reply is waited for without the console's lock: another request
 * reaches the guest, and is answered, while a "mem" waits for a slow one. */
void aWaitingMemoryRequestDoesNotHoldTheConsole()
{
	const ::uml::device::GuestConsoleRun run {true};
	const int server = bindServer(consolePath());
	std::atomic_bool done {};
	std::mutex heardMutex;
	std::vector<std::string> heard;
	const auto heardCount = [&] {
		const std::lock_guard lock {heardMutex};
		return heard.size();
	};
	std::thread guest {[&] {
		sockaddr_un client {};
		socklen_t clientLength {};
		sockaddr_un slow {};
		socklen_t slowLength {};
		std::chrono::steady_clock::time_point slowAt;
		bool slowPending = false;
		while (!done.load()) {
			if (slowPending &&
			    std::chrono::steady_clock::now() >= slowAt) {
				slowPending = false;
				(void)sendReply(server, slow, slowLength, 0U,
					"total=64 available=32 free=16");
			}
			if (!awaitRequest(server, 20))
				continue;
			const auto command = receiveRequest(server, client,
				clientLength);
			{
				const std::lock_guard lock {heardMutex};
				heard.push_back(command);
			}
			if (command == "mem") {
				slow = client;
				slowLength = clientLength;
				slowAt = std::chrono::steady_clock::now() +
					std::chrono::milliseconds {800};
				slowPending = true;
			} else {
				(void)sendReply(server, client, clientLength, 0U,
					"running");
			}
		}
	}};
	auto query = GuestMemoryQuery::start();
	require(query.has_value());
	/* "mem" is out and unanswered; a wake must not queue behind it. */
	require(waitUntil([&] { return heardCount() == 1U; },
		std::chrono::seconds {10}), "the guest never heard the request");
	const auto began = std::chrono::steady_clock::now();
	require(::uml::device::wake_guest(), "the wake was not answered");
	require(std::chrono::steady_clock::now() - began <
		std::chrono::milliseconds {600},
		"a wake waited for the memory request's reply");
	GuestMemoryFigures figures;
	require(query->await(std::chrono::seconds {10}, figures) ==
		GuestMemoryResult::done);
	require(figures.total_bytes == 64);
	done.store(true);
	guest.join();
	require(heard == std::vector<std::string> {"mem", "wake"});
	(void)::close(server);
}

/* The widest reply socket a location may name. */
std::size_t widestClientPath(const ::uml::device::GuestConsoleLocation &location)
{
	return location.client_prefix.size() + 8U;
}

void aDirectoryHoldsShortNamesThatFit()
{
	/* $TMPDIR of an iOS application: 89 bytes with its trailing slash. */
	const std::string container =
		"/private/var/mobile/Containers/Data/Application/"
		"00000000-0000-4000-8000-000000000000/tmp";
	const auto location =
		::uml::device::compose_guest_console_location(container + "/");
	require(location.has_value());
	require(location->directory == container);
	require(location->console == container + "/mconsole");
	require(location->client_prefix == container + "/mc-");
	require(location->console.size() < sizeof(sockaddr_un {}.sun_path));
	require(widestClientPath(*location) < sizeof(sockaddr_un {}.sun_path));

	const auto slashes =
		::uml::device::compose_guest_console_location(container + "///");
	require(slashes.has_value() && slashes->directory == container);
}

void aDirectoryThatDoesNotFitIsRefused()
{
	/* The longest directory whose widest reply socket still fits. */
	constexpr std::size_t limit = sizeof(sockaddr_un {}.sun_path);
	const std::size_t longest = limit - 1U - std::string_view {"/mc-"}.size() -
		8U;
	const auto fitting = "/" + std::string(longest - 1U, 'd');
	const auto fits = ::uml::device::compose_guest_console_location(fitting);
	require(fits.has_value() && widestClientPath(*fits) == limit - 1U);

	/* One byte more is refused outright: never truncated. */
	require(!::uml::device::compose_guest_console_location(fitting + "d")
			 .has_value());
	require(!::uml::device::compose_guest_console_location(
		"/" + std::string(limit, 'd')).has_value());
}

void aDirectoryMustBeAbsolute()
{
	require(!::uml::device::compose_guest_console_location(
		std::string_view {}).has_value());
	require(!::uml::device::compose_guest_console_location("tmp/")
			 .has_value());
}

void aRunDirectoryGoesInTheFirstParentThatFits()
{
	const std::vector<std::string> temporary {
		"/var/folders/yb/lx9rftq56156_v7rkkngd8bc0000gn/T/"};
	const auto location =
		::uml::device::compose_run_console_location(temporary, "kern42");
	require(location.has_value());
	require(location->directory ==
		"/var/folders/yb/lx9rftq56156_v7rkkngd8bc0000gn/T/uml-kern42");
	require(location->console == location->directory + "/mconsole");
	require(location->client_prefix == location->directory + "/mc-");

	/* Unset, relative and too-long parents are passed over, as the
	 * Simulator's application directory is. */
	const std::vector<std::string> fallbacks {
		"", "relative", "/" + std::string(100U, 'd'), "/tmp//"};
	const auto fallback =
		::uml::device::compose_run_console_location(fallbacks, "kern42");
	require(fallback.has_value() &&
		fallback->directory == "/tmp/uml-kern42");
	const std::vector<std::string> root {"/"};
	require(::uml::device::compose_run_console_location(root, "kern42")
			->directory == "/uml-kern42");
	const std::vector<std::string> none {"", "/" + std::string(100U, 'd')};
	require(!::uml::device::compose_run_console_location(none, "kern42")
			 .has_value());
}

void theProcessNamesItsOwnDirectory()
{
	const auto &process = processLocation();
	const auto umid = "kern" + std::to_string(::getpid());
	require(std::filesystem::path {process.directory}.filename() ==
		"uml-" + umid);
	/* $TMPDIR is the first choice whenever it is set and short enough. */
	const char *temporary = std::getenv("TMPDIR");
	if (temporary != nullptr && temporary[0] == '/' &&
	    std::string_view {temporary}.size() < 70U) {
		require(std::filesystem::path {process.directory}.parent_path() ==
			std::filesystem::path {std::string {temporary} + "/"}
				.lexically_normal().parent_path());
	}
	const auto arguments =
		::uml::device::guest_management_console_arguments();
	require(arguments == std::vector<std::string> {
		"umid=" + umid, "uml_dir=" + process.directory});
}

bool directoryExists(const std::string &path)
{
	struct stat details {};
	return ::lstat(path.c_str(), &details) == 0 && S_ISDIR(details.st_mode);
}

bool socketExists(const std::string &path)
{
	struct stat details {};
	return ::lstat(path.c_str(), &details) == 0 &&
		S_ISSOCK(details.st_mode);
}

void aRunOwnsItsDirectoryUntilItReturns()
{
	const auto &location = processLocation();
	{
		const ::uml::device::GuestConsoleRun inactive {false};
		require(!directoryExists(location.directory));
	}
	int console = -1;
	{
		const ::uml::device::GuestConsoleRun run {true};
		struct stat details {};
		require(::lstat(location.directory.c_str(), &details) == 0 &&
			S_ISDIR(details.st_mode) && details.st_uid == ::geteuid() &&
			(details.st_mode & 07777) == S_IRWXU);
		/* The guest binds its console there, and a run killed before the
		 * guest shut down keeps it. */
		console = bindServer(location.console);
	}
	require(!directoryExists(location.directory));
	(void)::close(console);
}

void theTerminationPathRemovesTheRunDirectory()
{
	const auto &location = processLocation();
	const ::uml::device::GuestConsoleRun run {true};
	const int console = bindServer(location.console);
	/* What darwin_uml_exit, darwin_uml_abort and the host's signal
	 * handlers run; the guest's SIGINT and SIGTERM handler exits through
	 * darwin_uml_exit. */
	::uml::device::remove_registered_socket_paths();
	require(!directoryExists(location.directory));
	(void)::close(console);
}

void aDirectoryLeftByThisPidIsReplaced()
{
	const auto &location = processLocation();
	require(::mkdir(location.directory.c_str(), S_IRWXU) == 0);
	const int stale = bindServer(location.console);
	{
		const ::uml::device::GuestConsoleRun run {true};
		require(directoryExists(location.directory) &&
			!socketExists(location.console));
	}
	require(!directoryExists(location.directory));
	(void)::close(stale);
}

/* A pid whose process has exited and been reaped. */
pid_t exitedPid()
{
	const pid_t child = ::fork();
	require(child >= 0);
	if (child == 0)
		::_exit(0);
	int status = 0;
	require(::waitpid(child, &status, 0) == child);
	return child;
}

void createFile(const std::filesystem::path &path)
{
	require(::close(::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY,
		S_IRUSR | S_IWUSR)) == 0);
}

void aRunRemovesOnlyWhatExitedRunsLeft()
{
	const auto parent =
		std::filesystem::path {processLocation().directory}.parent_path();
	const auto dead = std::to_string(exitedPid());
	const auto live = std::to_string(::getppid());
	const auto abandoned = parent / ("uml-kern" + dead);
	const auto liveRun = parent / ("uml-kern" + live);
	const auto padded = parent / ("uml-kern0" + dead);
	const auto file = parent / ("uml-kern" + std::to_string(exitedPid()));
	/* A name that only begins like a run's. */
	const auto foreign = parent / ("uml-kern" + dead + "-mconsole");
	/* init is a live process that this user may not signal. */
	const auto unsignalable = parent / "uml-kern1";
	for (const auto &directory :
		{abandoned, liveRun, padded, foreign, unsignalable})
		require(std::filesystem::create_directory(directory));
	const int abandonedConsole = bindServer(abandoned / "mconsole");
	createFile(file);

	{
		const ::uml::device::GuestConsoleRun run {true};
	}
	/* Only an exited process's run directory goes. A live run's, one this
	 * user may not signal, another program's names and anything of another
	 * file type stay. */
	require(!std::filesystem::exists(abandoned));
	require(directoryExists(liveRun) && directoryExists(padded) &&
		directoryExists(unsignalable) && directoryExists(foreign) &&
		std::filesystem::is_regular_file(file));

	(void)::close(abandonedConsole);
	for (const auto &path : {liveRun, padded, file, foreign, unsignalable})
		std::filesystem::remove_all(path);
}

/* Runs a group under a private $TMPDIR, which must hold this process's run
 * directory. */
void runPrivately(void (*group)())
{
	/* The guest's console is not narrowed to its owner until the host
	 * sends to it, whatever the umask. */
	(void)::umask(0);
	const uml::testing::PrivateTemporaryDirectory temporary {
		"uml-console-test.XXXXXX"};
	require(std::filesystem::path {processLocation().directory}
			.parent_path() == temporary.path());
	group();
}

} // namespace

int main(int argc, char **argv)
{
	const std::string_view selected = argc > 1 ? argv[1] : "";
	if (selected == "location") {
		aDirectoryHoldsShortNamesThatFit();
		aDirectoryThatDoesNotFitIsRefused();
		aDirectoryMustBeAbsolute();
		aRunDirectoryGoesInTheFirstParentThatFits();
		theProcessNamesItsOwnDirectory();
	} else if (selected == "run") {
		runPrivately([] {
			aRunOwnsItsDirectoryUntilItReturns();
			theTerminationPathRemovesTheRunDirectory();
			aDirectoryLeftByThisPidIsReplaced();
			aRunRemovesOnlyWhatExitedRunsLeft();
		});
	} else if (selected == "wake") {
		runPrivately(aWakeSendsOneWakeRequest);
	} else if (selected == "writeback") {
		runPrivately(aWritebackIsOneEmergencySync);
	} else if (selected == "suspend-failure") {
		runPrivately([] {
			aBusySuspendIsRetriedUntilTheDeadline();
			aLateBusyRefusalIsRetriedOnlyWithinTheDeadline();
			aRetryThatSucceedsSuspendsTheGuest();
			aRefusalThatIsNotBusyIsNotRetried();
		});
	} else if (selected == "trim") {
		runPrivately([] {
			trimmingIsSwitchedByOneRequest();
			aTrimPassIsWaitedForUpToItsBound();
		});
	} else if (selected == "memory") {
		runPrivately([] {
			theMemoryReplyIsParsedStrictly();
			aMemoryRequestReadsTheGuestsFigures();
			aSilentGuestIsWaitedForUpToTheBound();
			aWaitingMemoryRequestDoesNotHoldTheConsole();
		});
	} else if (selected == "suspend-timeout") {
		runPrivately(aSuspendThatDoesNotReplyIsAborted);
	} else if (selected == "suspend-silent") {
		runPrivately(aSilentSuspendIsUnconfirmed);
	} else if (selected == "suspend-suspended-after-wake") {
		runPrivately(aSuspendedReplyAfterTheWakeIsUnconfirmed);
	} else if (selected == "suspend-retry-deadline") {
		runPrivately(aBusyRetryCannotOutlastTheDeadline);
	} else {
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
