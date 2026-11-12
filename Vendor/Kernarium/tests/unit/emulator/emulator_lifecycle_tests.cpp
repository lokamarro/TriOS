// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The lifecycle calls reach the guest only through its management console, so
 * a datagram socket bound where the guest would bind its console sees exactly
 * what a guest would be asked, in order. The same socket orders the guest's
 * wake against the hibernation steps a test supplies: when the guest is woken,
 * its RAM and threads are already back.
 */
#include "uml/emulator.h"
#include "emulator_test_support.hpp"
#include "guest_console.hpp"
#include "guest_console_test_support.hpp"
#include "guest_hibernation.hpp"
#include "kernel_threads.hpp"
#include "require.hpp"
#include "testing.hpp"
#include "thread_nap.hpp"
#include "uml_darwin_host.h"
#include "wait_until.hpp"
#include "watchdog.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
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

/* Stands where the guest's console would be and answers every request the
 * way a guest that has the trim command would. */
class FakeGuestConsole final {
public:
	FakeGuestConsole()
	{
		const auto &location = ::uml::device::guest_console_location();
		require(location.has_value());
		path_ = location->console;
		descriptor_ = bindServer(path_);
	}

	FakeGuestConsole(const FakeGuestConsole &) = delete;
	FakeGuestConsole &operator=(const FakeGuestConsole &) = delete;

	~FakeGuestConsole()
	{
		(void)::close(descriptor_);
		(void)::unlink(path_.c_str());
	}

	/* Called on the console's thread with each request, before its reply. */
	std::function<void(const std::string &)> onRequest;
	/* A guest that cannot suspend: it refuses "suspend" for a reason that
	 * waiting does not cure (not busy), and keeps running. */
	bool refuseSuspend {false};
	/* A guest whose suspend is held, as by a freezer that cannot finish: it
	 * never replies to "suspend", answers the wake that aborts it, and then,
	 * for `lateFailure`, gives the suspend up on the request's own socket. */
	enum class Hold { none, silent, lateFailure };
	Hold holdSuspend {Hold::none};
	/* How the guest answers "mem": with its figures, as a kernel that lacks
	 * the command does, with a line that is not the figures, not at all, or
	 * (deferred) with the figures just before it answers its next request,
	 * so that the next request is heard while "mem" still waits. */
	enum class Memory { figures, unknown, malformed, silent, deferred };
	Memory memory {Memory::figures};
	static constexpr std::string_view memoryFigures {
		"total=2147483648 available=1073741824 free=104857600"};

	/* Answers `count` requests in another thread while `operation` runs,
	 * and returns them in the order they arrived. */
	template<typename Operation>
	std::vector<std::string> requests(std::size_t count,
		Operation &&operation)
	{
		std::vector<std::string> received;
		std::thread guest {[this, count, &received] {
			for (std::size_t index = 0; index < count; ++index) {
				/* The operation sends at once; a missing request
				 * fails the comparison instead of waiting here. */
				if (!awaitRequest(descriptor_,
					    holdSuspend == Hold::none ? 10000 : 20000))
					return;
				received.push_back(answer());
			}
		}};
		std::forward<Operation>(operation)();
		guest.join();
		/* Nothing else arrives either. */
		require(!awaitRequest(descriptor_, 200));
		return received;
	}

private:
	std::string answer() const
	{
		sockaddr_un client {};
		socklen_t clientLength {};
		const auto command = receiveRequest(descriptor_, client, clientLength);
		if (onRequest)
			onRequest(command);
		if (command == "suspend" && holdSuspend != Hold::none) {
			suspendClient_ = client;
			suspendClientLength_ = clientLength;
			return command;
		}
		std::string_view text = "running";
		std::uint32_t error = 0U;
		if (command == "mem") {
			switch (memory) {
			case Memory::figures:
				text = memoryFigures;
				break;
			case Memory::unknown:
				text = "Unknown command: mem";
				error = 1U;
				break;
			case Memory::malformed:
				text = "total=lots";
				break;
			case Memory::silent:
				return command;
			case Memory::deferred:
				memoryClient_ = client;
				memoryClientLength_ = clientLength;
				return command;
			}
		} else if (memoryClientLength_ != 0) {
			require(sendReply(descriptor_, memoryClient_,
				memoryClientLength_, 0U, memoryFigures),
				"the waiting memory request is still open");
			memoryClientLength_ = 0;
		}
		if (command == "suspend" && refuseSuspend) {
			text = "Suspend failed: refused by the test";
			error = 1U;
		} else if (command == "suspend") {
			text = "suspended";
		}
		if (command == "trim once")
			text = "trimmed 0 KiB in 0 ms, compacted in 40 ms, "
			       "released 0 KiB in 1 ms";
		else if (command.starts_with("trim "))
			text = std::string_view {command}.substr(5);
		require(sendReply(descriptor_, client, clientLength, error, text));
		if (command == "wake" && holdSuspend == Hold::lateFailure)
			require(sendReply(descriptor_, suspendClient_,
				suspendClientLength_, 1U, "Suspend failed: -16"),
				"the aborted suspend's socket is still open");
		return command;
	}

	std::string path_;
	int descriptor_ {-1};
	mutable sockaddr_un suspendClient_ {};
	mutable socklen_t suspendClientLength_ {};
	mutable sockaddr_un memoryClient_ {};
	mutable socklen_t memoryClientLength_ {};
};

using Requests = std::vector<std::string>;

/* A handle with no kernel: the lifecycle calls reach only the console. */
uml::testing::Emulator makeEmulator()
{
	return uml::testing::create_emulator({
		.abi_version = UML_EMULATOR_ABI_VERSION,
		.struct_size = sizeof(uml_emulator_config),
	});
}

/* A quiesced guest keeps what it holds, so it trims before it suspends. */
void quiesceTrimsBeforeItSuspends(uml_emulator *emulator,
	FakeGuestConsole &console)
{
	require(console.requests(2U, [emulator] {
		require(uml_emulator_quiesce(emulator) == 0);
	}) == Requests {"trim once", "suspend"});
}

/* Any request would wake a quiesced guest, so nothing reaches it until the
 * resume, which carries the trimming change after the wake. */
void aQuiescedGuestHearsOnlyTheResume(uml_emulator *emulator,
	FakeGuestConsole &console)
{
	require(console.requests(0U, [emulator] {
		require(uml_emulator_trim_page_cache(emulator) ==
			UML_EMULATOR_OK);
		require(uml_emulator_set_page_cache_trimming(emulator, 1) ==
			UML_EMULATOR_OK);
		require(uml_emulator_quiesce(emulator) == 0);
	}).empty());
	require(console.requests(2U, [emulator] {
		uml_emulator_resume(emulator);
	}) == Requests {"wake", "trim on"});
}

void aRunningGuestIsAskedAtOnce(uml_emulator *emulator,
	FakeGuestConsole &console)
{
	require(console.requests(1U, [emulator] {
		require(uml_emulator_set_page_cache_trimming(emulator, 0) ==
			UML_EMULATOR_OK);
	}) == Requests {"trim off"});
	require(console.requests(1U, [emulator] {
		require(uml_emulator_trim_page_cache(emulator) ==
			UML_EMULATOR_OK);
	}) == Requests {"trim once"});
	/* Resuming a running guest changes nothing it was told. */
	require(console.requests(0U, [emulator] {
		uml_emulator_resume(emulator);
	}).empty());
}

void withoutAConsoleNothingWaits(uml_emulator *emulator)
{
	require(uml_emulator_set_page_cache_trimming(emulator, 1) ==
		-UML_EMULATOR_UNSUPPORTED);
	require(uml_emulator_trim_page_cache(emulator) ==
		-UML_EMULATOR_UNSUPPORTED);
	uml_emulator_guest_memory memory {.struct_size = sizeof(memory)};
	require(uml_emulator_get_guest_memory(emulator, &memory) ==
		-UML_EMULATOR_UNSUPPORTED);
	require(uml_emulator_set_page_cache_trimming(nullptr, 1) ==
		-UML_EMULATOR_INVALID_ARGUMENT);
	require(uml_emulator_trim_page_cache(nullptr) ==
		-UML_EMULATOR_INVALID_ARGUMENT);
	require(uml_emulator_quiesce_resident(nullptr) ==
		-UML_EMULATOR_INVALID_ARGUMENT);
}

/* ---- Hibernating the guest's RAM around a quiesce. ---- */

/* What happened, in order, on every thread: hibernation steps and the
 * guest's console requests share it. */
class Log final {
public:
	void add(std::string entry)
	{
		const std::lock_guard lock {mutex_};
		entries_.push_back(std::move(entry));
	}

	[[nodiscard]] std::vector<std::string> take()
	{
		const std::lock_guard lock {mutex_};
		return std::exchange(entries_, {});
	}

private:
	std::mutex mutex_;
	std::vector<std::string> entries_;
};

Log hibernationLog;
std::atomic<int> restoreFailures {0};

struct Steps final : uml::runtime::HibernationSteps {
	bool restoreSucceeds {true};
	bool holdSucceeds {true};
	/* Called on the quiescing thread as the steps run. */
	std::function<void()> atHold;
	std::function<void()> atWrite;
	/* The write waits for the abort request that resume sends. */
	bool writeWaitsForAbort {false};
	std::atomic<bool> aborted {false};
	std::atomic<bool> writing {false};

	bool prepare() noexcept override
	{
		hibernationLog.add("prepare");
		return true;
	}
	bool hold(uml::runtime::HibernationReport &,
		uml::runtime::HoldCondition) noexcept override
	{
		hibernationLog.add("hold");
		if (atHold)
			atHold();
		return holdSucceeds;
	}
	bool write(uml::runtime::HibernationReport &) noexcept override
	{
		if (atWrite)
			atWrite();
		writing = true;
		if (writeWaitsForAbort) {
			const bool abortedInTime = uml::testing::waitUntil(
				[this] { return aborted.load(); },
				std::chrono::seconds {10},
				std::chrono::milliseconds {2});
			hibernationLog.add(
				abortedInTime ? "write:aborted" : "write:timeout");
		} else {
			hibernationLog.add("write");
		}
		return true;
	}
	bool restore(uml::runtime::HibernationReport &) noexcept override
	{
		hibernationLog.add("restore");
		return restoreSucceeds;
	}
	void release() noexcept override { hibernationLog.add("release"); }
	void set_abort(bool abort) noexcept override
	{
		if (abort) {
			hibernationLog.add("abort");
			aborted = true;
		} else {
			aborted = false;
		}
	}
};

void countRestoreFailure() noexcept
{
	hibernationLog.add("failure");
	++restoreFailures;
}

/* Installs `steps` on `emulator`; they stay owned by it, and the test keeps a
 * pointer to flip switches. */
Steps *installSteps(uml_emulator *emulator)
{
	auto steps = std::make_unique<Steps>();
	Steps *const raw = steps.get();
	uml::emulator::testing::use_hibernation(emulator, std::move(steps),
		&countRestoreFailure);
	return raw;
}

using Entries = std::vector<std::string>;

/* The wake is the first thing the guest can act on, so by then the RAM must be
 * whole and every thread released. */
void aHibernatedGuestIsWholeBeforeItIsWoken()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.onRequest = [](const std::string &command) {
		hibernationLog.add("console:" + command);
	};
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	require(console.requests(2U, [&] {
		require(uml_emulator_quiesce(emulator.get()) == 0);
	}) == Requests {"trim once", "suspend"});
	require(hibernationLog.take() == Entries {"console:trim once",
		"console:suspend", "prepare", "hold", "write"},
		"a quiesce hibernates after the guest is asleep");
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	require(hibernationLog.take() == Entries {"abort", "restore", "release",
		"console:wake"}, "the RAM is back and the threads run before the wake");
}

void aSecondQuiesceKeepsTheHibernation()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	require(console.requests(2U, [&] {
		require(uml_emulator_quiesce(emulator.get()) == 0);
	}) == Requests {"trim once", "suspend"});
	require(console.requests(0U, [&] {
		require(uml_emulator_quiesce(emulator.get()) == 0);
	}).empty());
	require(hibernationLog.take() == Entries {"prepare", "hold", "write"},
		"a guest that is already hibernated is not hibernated again");
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	(void)hibernationLog.take();
}

/* A host short of the space an image takes quiesces resident: the guest
 * sleeps and its threads are held, and not a byte of RAM is written. */
void aResidentQuiesceWritesNoRam()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.onRequest = [](const std::string &command) {
		hibernationLog.add("console:" + command);
	};
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	require(console.requests(2U, [&] {
		require(uml_emulator_quiesce_resident(emulator.get()) == 0);
	}) == Requests {"trim once", "suspend"},
		"the guest is trimmed and suspended as by any quiesce");
	require(hibernationLog.take() == Entries {"console:trim once",
		"console:suspend"}, "the hibernation is not prepared, held or written");
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	require(hibernationLog.take() == Entries {"abort", "console:wake"},
		"nothing was taken out of the RAM, so nothing is restored");
	/* The next quiesce is the host's own choice again. */
	require(console.requests(2U, [&] {
		require(uml_emulator_quiesce(emulator.get()) == 0);
	}) == Requests {"trim once", "suspend"});
	require(hibernationLog.take() == Entries {"console:trim once",
		"console:suspend", "prepare", "hold", "write"},
		"a quiesce after a resident one hibernates");
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	(void)hibernationLog.take();
}

/* The first hold stands: a guest already hibernated is not made resident, and
 * one held resident is not hibernated behind its holder's back. */
void aQuiesceKeepsWhateverHoldsTheGuest()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	require(console.requests(2U, [&] {
		require(uml_emulator_quiesce(emulator.get()) == 0);
	}) == Requests {"trim once", "suspend"});
	require(console.requests(0U, [&] {
		require(uml_emulator_quiesce_resident(emulator.get()) == 0);
	}).empty());
	require(hibernationLog.take() == Entries {"prepare", "hold", "write"},
		"a hibernated guest stays hibernated");
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	(void)hibernationLog.take();
	require(console.requests(2U, [&] {
		require(uml_emulator_quiesce_resident(emulator.get()) == 0);
	}) == Requests {"trim once", "suspend"});
	require(console.requests(0U, [&] {
		require(uml_emulator_quiesce(emulator.get()) == 0);
	}).empty());
	require(hibernationLog.take().empty(),
		"a guest held resident is not hibernated by a later quiesce");
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	(void)hibernationLog.take();
}

void aGuestThatCouldNotSuspendIsNotHibernated()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.refuseSuspend = true;
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	const auto requests = console.requests(3U, [&] {
		require(uml_emulator_quiesce(emulator.get()) == 0);
	});
	require(requests == Requests {"trim once", "suspend", "sysrq s"},
		"a refused suspend is written back and left running, never stopped");
	require(hibernationLog.take().empty(),
		"only a system suspension is hibernated");
	require(console.requests(0U, [&] { uml_emulator_resume(emulator.get()); })
			.empty(),
		"a guest that kept running has nothing to be woken from");
}

/* A suspend with no reply at its deadline is aborted with a wake, not taken
 * for a suspended guest, which would be hibernated while its freezer still
 * runs. The request's final refusal says the guest runs: its resume sends
 * nothing. */
void aSuspendThatTimedOutIsNotHibernated()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.holdSuspend = FakeGuestConsole::Hold::lateFailure;
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	require(console.requests(4U, [&] {
		require(uml_emulator_quiesce(emulator.get()) == 0);
	}) == Requests {"trim once", "suspend", "wake", "sysrq s"},
		"a suspend that does not reply is aborted and written back");
	require(hibernationLog.take().empty(),
		"only a confirmed system suspension is hibernated");
	/* A guest known to run has no sleep to end, so it is asked. */
	uml_emulator_guest_memory memory {.struct_size = sizeof(memory)};
	require(console.requests(1U, [&] {
		require(uml_emulator_get_guest_memory(emulator.get(), &memory) ==
			UML_EMULATOR_OK);
	}) == Requests {"mem"}, "a guest known to run was not asked");
	require(console.requests(0U, [&] { uml_emulator_resume(emulator.get()); })
			.empty(),
		"a guest known to run has nothing to be woken from");
}

/* Without a final word the guest may be asleep, so the resume wakes it. */
void aSuspendThatNeverAnsweredIsWokenAtTheResume()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.holdSuspend = FakeGuestConsole::Hold::silent;
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	require(console.requests(4U, [&] {
		require(uml_emulator_quiesce(emulator.get()) == 0);
	}) == Requests {"trim once", "suspend", "wake", "sysrq s"});
	require(hibernationLog.take().empty(),
		"an unconfirmed suspension is not hibernated");
	/* It may be asleep, and a request would end that sleep. */
	uml_emulator_guest_memory memory {.struct_size = sizeof(memory)};
	require(console.requests(0U, [&] {
		require(uml_emulator_get_guest_memory(emulator.get(), &memory) ==
			-UML_EMULATOR_UNSUPPORTED);
	}).empty(), "a guest that may be asleep was asked for its memory");
	require(console.requests(1U, [&] { uml_emulator_resume(emulator.get()); })
			== Requests {"wake"},
		"an unconfirmed guest is woken at the resume");
}

void aGuestWithoutAConsoleIsNotHibernated()
{
	const auto emulator = makeEmulator();
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	require(uml_emulator_quiesce(emulator.get()) == 0);
	require(hibernationLog.take().empty(), "nothing suspended, nothing to do");
	uml_emulator_resume(emulator.get());
}

/* A failed restore must not wake anything: the guest would run on missing
 * memory. The failure handler ends the process in production. */
void aFailedRestoreWakesNothing()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	console.onRequest = [](const std::string &command) {
		hibernationLog.add("console:" + command);
	};
	Steps *const steps = installSteps(emulator.get());
	steps->restoreSucceeds = false;
	require(console.requests(2U, [&] {
		require(uml_emulator_quiesce(emulator.get()) == 0);
	}) == Requests {"trim once", "suspend"});
	(void)hibernationLog.take();
	require(console.requests(0U, [&] {
		uml_emulator_resume(emulator.get());
	}).empty(), "a guest whose RAM is not back is not woken");
	require(hibernationLog.take() == Entries {"abort", "restore", "failure"},
		"the failure is reported and nothing is released");
	require(restoreFailures.load() == 1, "once");
	/* The guest stays quiesced; a later resume tries again. */
	steps->restoreSucceeds = true;
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	require(hibernationLog.take() == Entries {"abort", "restore", "release",
		"console:wake"}, "and wakes it once the RAM is back");
}

/* A resume that arrives while the image is still being written does not wait
 * for the whole write: it asks the write to stop, then restores. */
void aResumeDuringTheWriteStopsTheWrite()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	Steps *const steps = installSteps(emulator.get());
	steps->writeWaitsForAbort = true;
	(void)hibernationLog.take();
	/* The guest answers the quiesce's two requests and the resume's wake. */
	Requests heard;
	std::thread quiescing {[&] {
		heard = console.requests(3U, [&] {
			require(uml_emulator_quiesce(emulator.get()) == 0);
		});
	}};
	require(uml::testing::waitUntil([&] { return steps->writing.load(); },
		std::chrono::seconds {5}, std::chrono::milliseconds {2}),
		"the write must start");
	uml_emulator_resume(emulator.get());
	quiescing.join();
	require(heard == Requests {"trim once", "suspend", "wake"});
	const auto entries = hibernationLog.take();
	require(entries == Entries {"prepare", "hold", "abort", "write:aborted",
		"restore", "release"} || entries == Entries {"prepare", "hold",
		"abort", "write:aborted", "abort", "restore", "release"},
		"the resume stops the write, then restores");
}

/* Threads that are held cannot see a stop, so a stop restores first. */
void aStopBringsTheRamBack()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	require(console.requests(2U, [&] {
		require(uml_emulator_quiesce(emulator.get()) == 0);
	}) == Requests {"trim once", "suspend"});
	(void)hibernationLog.take();
	uml_emulator_request_stop(emulator.get());
	require(hibernationLog.take() == Entries {"abort", "restore", "release"},
		"a stop runs the threads that were held");
	(void)console.requests(1U, [&] { uml_emulator_resume(emulator.get()); });
}

/* ---- The guest's disks while their locks are released. ---- */

/* A disk the way the kernel has one: opened and locked through the host
 * bridge, as os_lock_file locks it. */
class GuestDisk final {
public:
	GuestDisk()
	{
		const char *const directory = std::getenv("TMPDIR");
		require(directory != nullptr, "the test has a private directory");
		path_ = std::string {directory} + "/guest-disk.img";
		descriptor_ = darwin_uml_open(path_.c_str(),
			O_RDWR | O_CREAT | O_TRUNC, 0600);
		require(descriptor_ >= 0, "open the guest disk");
		struct flock lock {};
		lock.l_type = F_WRLCK;
		lock.l_whence = SEEK_SET;
		require(darwin_uml_fcntl(descriptor_, F_SETLK,
			reinterpret_cast<long>(&lock)) == 0, "lock the guest disk");
	}

	GuestDisk(const GuestDisk &) = delete;
	GuestDisk &operator=(const GuestDisk &) = delete;

	~GuestDisk()
	{
		(void)::close(descriptor_);
		(void)::unlink(path_.c_str());
	}

	[[nodiscard]] int descriptor() const noexcept { return descriptor_; }

	/* Whether another process cannot lock the disk, as a second host would
	 * find it. A forked child asks, through the descriptor it inherits: the
	 * kernel's lock belongs to the open file description, which the child
	 * shares, and conflicts with a lock query from any other process. */
	[[nodiscard]] bool lockedByAnotherProcess() const
	{
		int result[2];
		require(::pipe(result) == 0);
		const pid_t child = ::fork();
		require(child >= 0);
		if (child == 0) {
			(void)::close(result[0]);
			struct flock query {};
			query.l_type = F_WRLCK;
			query.l_whence = SEEK_SET;
			const char answer = ::fcntl(descriptor_, F_GETLK, &query) == 0 ?
				(query.l_type == F_UNLCK ? 'n' : 'y') : 'e';
			::_exit(::write(result[1], &answer, 1) == 1 ? 0 : 1);
		}
		(void)::close(result[1]);
		char answer = 'e';
		require(::read(result[0], &answer, 1) == 1);
		(void)::close(result[0]);
		int status = 0;
		require(::waitpid(child, &status, 0) == child);
		require(answer != 'e', "query the disk's lock");
		return answer == 'y';
	}

private:
	std::string path_;
	int descriptor_ {-1};
};

/* A thread of the kernel that writes the guest's disk whenever it runs, as a
 * guest does that its own wake source has woken. */
class WakingGuest final {
public:
	explicit WakingGuest(int disk) : disk_{disk}
	{
		require(::uml::runtime::kernel_pthread_create(&thread_, nullptr,
			&writeUntilStopped, this) == 0, "create the guest's thread");
		require(runsAgain(), "the guest's thread must run");
	}

	WakingGuest(const WakingGuest &) = delete;
	WakingGuest &operator=(const WakingGuest &) = delete;

	~WakingGuest()
	{
		stop_.store(true);
		require(::pthread_join(thread_, nullptr) == 0);
	}

	/* The guest writes again within a few seconds. */
	[[nodiscard]] bool runsAgain() const
	{
		const auto before = writes_.load();
		return uml::testing::waitUntil(
			[&] { return writes_.load() > before + 2U; },
			std::chrono::seconds {5}, std::chrono::milliseconds {2});
	}

	/* The guest wrote nothing for a while, which no thread that runs can
	 * promise: it writes every few hundred microseconds. */
	[[nodiscard]] bool stoodStill() const
	{
		const auto before = writes_.load();
		std::this_thread::sleep_for(std::chrono::milliseconds {100});
		return writes_.load() == before;
	}

private:
	static void *writeUntilStopped(void *argument)
	{
		auto *const guest = static_cast<WakingGuest *>(argument);
		while (!guest->stop_.load(std::memory_order_acquire)) {
			if (::pwrite(guest->disk_, "w", 1, 0) == 1)
				guest->writes_.fetch_add(1, std::memory_order_relaxed);
			uml::testing::nap(300);
		}
		return nullptr;
	}

	int disk_;
	pthread_t thread_ {};
	std::atomic<std::uint64_t> writes_ {0};
	std::atomic<bool> stop_ {false};
};

/* A confirmed system suspension ends at the guest's own wake sources (its RTC
 * alarm, VNC input) as well as at the host's wake, so the guest can run, and
 * write its disks, while the host holds no lock on them. Every thread that
 * can run it is held from before the locks leave until the host has them back
 * and verified the disks, whether or not the guest's RAM is hibernated. */
void aGuestThatWakesItselfCannotWriteDisksItDoesNotLock()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	GuestDisk disk;
	WakingGuest guest {disk.descriptor()};
	require(disk.lockedByAnotherProcess(), "a running guest owns its disk");
	require(console.requests(2U, [&] {
		require(uml_emulator_quiesce(emulator.get()) == 0);
	}) == Requests {"trim once", "suspend"});
	require(!disk.lockedByAnotherProcess(),
		"a suspended guest leaves its disk unlocked for the system");
	require(guest.stoodStill(),
		"the guest wrote its disk while the host held no lock on it");
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	require(disk.lockedByAnotherProcess(), "the resume locks the disk again");
	require(guest.runsAgain(), "the guest runs once it is woken");
}

/* A thread the fence cannot hold, here one inside the C library for as long
 * as the test says, leaves the guest able to run. The locks then stay, as an
 * unconfirmed suspension keeps them, and the resume has nothing to verify. */
std::atomic<bool> serviceBlocked {true};

int blockInComparator(const void *, const void *)
{
	while (serviceBlocked.load(std::memory_order_acquire))
		uml::testing::nap(500);
	return 0;
}

void aGuestThatCannotBeHeldKeepsItsLocks()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	GuestDisk disk;
	WakingGuest guest {disk.descriptor()};
	serviceBlocked.store(true);
	std::atomic<bool> started {false};
	std::thread service {[&] {
		(void)::pthread_setname_np("uml-test-service");
		int values[2] = {2, 1};
		started.store(true);
		(void)::heapsort(values, 2, sizeof values[0], &blockInComparator);
	}};
	require(uml::testing::waitUntil([&] { return started.load(); },
		std::chrono::seconds {5}));
	uml::testing::nap(50000);
	require(console.requests(2U, [&] {
		require(uml_emulator_quiesce(emulator.get()) == 0);
	}) == Requests {"trim once", "suspend"});
	require(disk.lockedByAnotherProcess(),
		"locks may not leave while the guest's threads can run");
	serviceBlocked.store(false);
	service.join();
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	require(disk.lockedByAnotherProcess());
	require(guest.runsAgain(), "the guest runs once it is woken");
}

/* A thread of the kernel that asks the host bridge for its disk's lock without
 * pause, as os_lock_file does on every open. Each request spends most of its
 * time inside the guest file registry's critical section, so a thread stopped
 * at a random moment is very likely to stop inside it, owning the registry's
 * mutex. */
class LockingGuest final {
public:
	explicit LockingGuest(int disk) : disk_{disk}
	{
		require(::uml::runtime::kernel_pthread_create(&thread_, nullptr,
			&lockUntilStopped, this) == 0, "create the guest's thread");
		require(runsAgain(), "the guest's thread must run");
	}

	LockingGuest(const LockingGuest &) = delete;
	LockingGuest &operator=(const LockingGuest &) = delete;

	~LockingGuest()
	{
		stop_.store(true);
		require(::pthread_join(thread_, nullptr) == 0);
	}

	/* The guest gets its disk's lock again: a request is granted only while
	 * the host has given the locks back. */
	[[nodiscard]] bool runsAgain() const
	{
		const auto before = granted_.load();
		return uml::testing::waitUntil(
			[&] { return granted_.load() > before + 2U; },
			std::chrono::seconds {5}, std::chrono::milliseconds {2});
	}

private:
	static void *lockUntilStopped(void *argument)
	{
		auto *const guest = static_cast<LockingGuest *>(argument);
		while (!guest->stop_.load(std::memory_order_acquire)) {
			struct flock lock {};
			lock.l_type = F_WRLCK;
			lock.l_whence = SEEK_SET;
			if (darwin_uml_fcntl(guest->disk_, F_SETLK,
				    reinterpret_cast<long>(&lock)) == 0)
				guest->granted_.fetch_add(1, std::memory_order_relaxed);
		}
		return nullptr;
	}

	int disk_;
	pthread_t thread_ {};
	std::atomic<std::uint64_t> granted_ {0};
	std::atomic<bool> stop_ {false};
};

/* The hold stops the guest's threads wherever they are, and the host then
 * takes the registry's mutex to release the disk locks and, at the resume, to
 * restore them. A thread stopped inside the registry would own that mutex for
 * as long as it is stopped, and the release would wait for it forever with
 * the process asleep. The hold is therefore accepted only at a moment when no
 * stopped thread owns the registry. Every suspension below stops a guest that
 * is inside the registry nearly all the time; the watchdog turns a wedged one
 * into a failure. */
void aGuestInsideTheRegistryDoesNotWedgeTheSuspension()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	GuestDisk disk;
	LockingGuest guest {disk.descriptor()};
	const uml::testing::Watchdog wedged {
		"a suspension with the guest inside the registry",
		std::chrono::seconds {60}};
	for (int round = 0; round < 25; ++round) {
		require(console.requests(2U, [&] {
			require(uml_emulator_quiesce(emulator.get()) == 0);
		}) == Requests {"trim once", "suspend"});
		require(!disk.lockedByAnotherProcess(),
			"a suspended guest leaves its disk unlocked for the system");
		require(console.requests(1U, [&] {
			uml_emulator_resume(emulator.get());
		}) == Requests {"wake"});
		require(disk.lockedByAnotherProcess(),
			"the resume locks the disk again");
		require(guest.runsAgain(), "the guest runs once it is woken");
	}
}

/* With the RAM hibernated the one hold covers both: the locks leave once the
 * threads are held and before the write, which can take long enough for the
 * system to suspend the process. */
void theLocksLeaveBetweenTheHoldAndTheWrite()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	GuestDisk disk;
	Steps *const steps = installSteps(emulator.get());
	bool lockedAtHold {false};
	bool lockedAtWrite {true};
	steps->atHold = [&] { lockedAtHold = disk.lockedByAnotherProcess(); };
	steps->atWrite = [&] { lockedAtWrite = disk.lockedByAnotherProcess(); };
	(void)hibernationLog.take();
	require(console.requests(2U, [&] {
		require(uml_emulator_quiesce(emulator.get()) == 0);
	}) == Requests {"trim once", "suspend"});
	require(hibernationLog.take() == Entries {"prepare", "hold", "write"});
	require(lockedAtHold, "the locks stay until the threads are held");
	require(!lockedAtWrite, "the locks are gone before the image is written");
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	require(disk.lockedByAnotherProcess(), "the resume locks the disk again");
	(void)hibernationLog.take();
}

/* A hibernation that cannot hold the threads changes nothing, the locks
 * included. */
void aHibernationThatCannotHoldKeepsTheLocks()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	GuestDisk disk;
	Steps *const steps = installSteps(emulator.get());
	steps->holdSucceeds = false;
	(void)hibernationLog.take();
	require(console.requests(2U, [&] {
		require(uml_emulator_quiesce(emulator.get()) == 0);
	}) == Requests {"trim once", "suspend"});
	require(hibernationLog.take() == Entries {"prepare", "hold"},
		"nothing is written where nothing is held");
	require(disk.lockedByAnotherProcess(),
		"the locks stay where the threads are not held");
	require(console.requests(1U, [&] {
		uml_emulator_resume(emulator.get());
	}) == Requests {"wake"});
	require(disk.lockedByAnotherProcess());
	(void)hibernationLog.take();
}

/* ---- The guest's memory figures. ---- */

uml_emulator_guest_memory askedMemory()
{
	return {.struct_size = sizeof(uml_emulator_guest_memory)};
}

/* One datagram out, and the guest's own total and available memory back. */
void theGuestsMemoryIsRead(uml_emulator *emulator, FakeGuestConsole &console)
{
	auto memory = askedMemory();
	require(console.requests(1U, [&] {
		require(uml_emulator_get_guest_memory(emulator, &memory) ==
			UML_EMULATOR_OK);
	}) == Requests {"mem"});
	require(memory.struct_size == sizeof(memory) &&
		memory.total_bytes == 2147483648ULL &&
		memory.available_bytes == 1073741824ULL,
		"the guest's figures were not returned");
}

/* A kernel without the command and a reply that is not the figures are not
 * a reading, and a silent guest is given up on at the bound. */
void anAnswerThatIsNotTheFiguresIsNoReading(uml_emulator *emulator,
	FakeGuestConsole &console)
{
	auto memory = askedMemory();
	memory.total_bytes = 7U;
	console.memory = FakeGuestConsole::Memory::unknown;
	require(console.requests(1U, [&] {
		require(uml_emulator_get_guest_memory(emulator, &memory) ==
			-UML_EMULATOR_UNSUPPORTED);
	}) == Requests {"mem"}, "a kernel without the command is unsupported");
	console.memory = FakeGuestConsole::Memory::malformed;
	require(console.requests(1U, [&] {
		require(uml_emulator_get_guest_memory(emulator, &memory) ==
			-UML_EMULATOR_RUNTIME_ERROR);
	}) == Requests {"mem"}, "a reply that is not the figures is an error");
	require(memory.total_bytes == 7U, "a refused reading changed the output");

	console.memory = FakeGuestConsole::Memory::silent;
	const auto started = std::chrono::steady_clock::now();
	require(console.requests(1U, [&] {
		require(uml_emulator_get_guest_memory(emulator, &memory) ==
			-UML_EMULATOR_RUNTIME_ERROR);
	}) == Requests {"mem"}, "a silent guest is asked once");
	require(std::chrono::steady_clock::now() - started >=
		std::chrono::milliseconds {450}, "the reply was not waited for");
	console.memory = FakeGuestConsole::Memory::figures;
}

/* A request would end a suspend, so a quiesced guest is not asked, and is
 * asked again once it has been resumed. */
void aQuiescedGuestIsNotAskedForItsMemory()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	require(console.requests(2U, [&] {
		require(uml_emulator_quiesce(emulator.get()) == 0);
	}) == Requests {"trim once", "suspend"});
	auto memory = askedMemory();
	memory.total_bytes = 7U;
	require(console.requests(0U, [&] {
		require(uml_emulator_get_guest_memory(emulator.get(), &memory) ==
			-UML_EMULATOR_UNSUPPORTED);
	}).empty(), "a quiesced guest was woken to be asked");
	require(memory.total_bytes == 7U, "a refused reading changed the output");
	require(console.requests(1U, [&] { uml_emulator_resume(emulator.get()); })
		== Requests {"wake"});
	theGuestsMemoryIsRead(emulator.get(), console);
	(void)hibernationLog.take();
}

/* A lifecycle call in progress is neither waited for nor waited on: the
 * request is refused at once and sends nothing. This holds through the whole
 * quiesce, so also while its suspend is starting: the guest has been asked
 * and has not yet replied that it sleeps. `held` is the request whose answer
 * the guest keeps back. */
void aLifecycleCallInProgressIsNotWaitedFor(const std::string &held)
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	std::mutex mutex;
	std::condition_variable changed;
	bool trimming {false};
	bool released {false};
	/* The guest holds its answer to `held`, and the quiesce its lifecycle
	 * mutex, until the test lets go or the guard does. */
	console.onRequest = [&](const std::string &command) {
		if (command != held)
			return;
		std::unique_lock lock {mutex};
		trimming = true;
		changed.notify_all();
		(void)changed.wait_for(lock, std::chrono::seconds {5},
			[&] { return released; });
	};
	Requests heard;
	std::thread quiescing {[&] {
		heard = console.requests(2U, [&] {
			require(uml_emulator_quiesce(emulator.get()) == 0);
		});
	}};
	{
		std::unique_lock lock {mutex};
		require(changed.wait_for(lock, std::chrono::seconds {10},
			[&] { return trimming; }), "the quiesce never sent the request");
	}
	auto memory = askedMemory();
	const auto started = std::chrono::steady_clock::now();
	const int result = uml_emulator_get_guest_memory(emulator.get(), &memory);
	const auto waited = std::chrono::steady_clock::now() - started;
	{
		const std::lock_guard lock {mutex};
		released = true;
	}
	changed.notify_all();
	quiescing.join();
	require(result == -UML_EMULATOR_UNSUPPORTED,
		"a request ran beside a quiesce");
	require(waited < std::chrono::seconds {2},
		"a request waited for a quiesce");
	require(heard == Requests {"trim once", "suspend"},
		"a request reached the guest beside a quiesce");
	require(console.requests(1U, [&] { uml_emulator_resume(emulator.get()); })
		== Requests {"wake"});
	(void)hibernationLog.take();
}

/* The wait for the reply is outside the lifecycle mutex, so a quiesce that
 * starts after the request was sent is heard while it waits. A request that
 * held the mutex would time out before the quiesce was heard. */
void aQuiesceDoesNotWaitForAMemoryRequest()
{
	const auto emulator = makeEmulator();
	FakeGuestConsole console;
	(void)installSteps(emulator.get());
	(void)hibernationLog.take();
	console.memory = FakeGuestConsole::Memory::deferred;
	std::mutex mutex;
	std::condition_variable changed;
	bool asked {false};
	console.onRequest = [&](const std::string &command) {
		if (command != "mem")
			return;
		const std::lock_guard lock {mutex};
		asked = true;
		changed.notify_all();
	};
	auto memory = askedMemory();
	const auto heard = console.requests(3U, [&] {
		std::thread quiescing {[&] {
			std::unique_lock lock {mutex};
			require(changed.wait_for(lock, std::chrono::seconds {10},
				[&] { return asked; }), "the request never arrived");
			lock.unlock();
			require(uml_emulator_quiesce(emulator.get()) == 0);
		}};
		const int result =
			uml_emulator_get_guest_memory(emulator.get(), &memory);
		quiescing.join();
		require(result == UML_EMULATOR_OK,
			"the request was not answered while the quiesce was heard");
	});
	require(heard == Requests {"mem", "trim once", "suspend"});
	require(memory.total_bytes == 2147483648ULL &&
		memory.available_bytes == 1073741824ULL);
	require(console.requests(1U, [&] { uml_emulator_resume(emulator.get()); })
		== Requests {"wake"});
	(void)hibernationLog.take();
}

} // namespace

int main(int argc, char **argv)
{
	const std::string_view selected = argc > 1 ? argv[1] : "";
	const uml::testing::PrivateTemporaryDirectory temporary {
		"uml-lifecycle-test.XXXXXX"};
	const ::uml::device::GuestConsoleRun run {true};
	if (selected == "suspend-timeout") {
		aSuspendThatTimedOutIsNotHibernated();
		return EXIT_SUCCESS;
	}
	if (selected == "suspend-silent") {
		aSuspendThatNeverAnsweredIsWokenAtTheResume();
		return EXIT_SUCCESS;
	}
	if (selected == "unheld-guest") {
		aGuestThatCannotBeHeldKeepsItsLocks();
		return EXIT_SUCCESS;
	}
	if (selected == "registry-owner") {
		aGuestInsideTheRegistryDoesNotWedgeTheSuspension();
		return EXIT_SUCCESS;
	}
	if (!selected.empty())
		return EXIT_FAILURE;
	const auto emulator = makeEmulator();
	withoutAConsoleNothingWaits(emulator.get());
	FakeGuestConsole console;
	quiesceTrimsBeforeItSuspends(emulator.get(), console);
	aQuiescedGuestHearsOnlyTheResume(emulator.get(), console);
	aRunningGuestIsAskedAtOnce(emulator.get(), console);
	theGuestsMemoryIsRead(emulator.get(), console);
	anAnswerThatIsNotTheFiguresIsNoReading(emulator.get(), console);
	aHibernatedGuestIsWholeBeforeItIsWoken();
	aSecondQuiesceKeepsTheHibernation();
	aResidentQuiesceWritesNoRam();
	aQuiesceKeepsWhateverHoldsTheGuest();
	aGuestThatCouldNotSuspendIsNotHibernated();
	aGuestWithoutAConsoleIsNotHibernated();
	aFailedRestoreWakesNothing();
	aResumeDuringTheWriteStopsTheWrite();
	aStopBringsTheRamBack();
	aGuestThatWakesItselfCannotWriteDisksItDoesNotLock();
	theLocksLeaveBetweenTheHoldAndTheWrite();
	aHibernationThatCannotHoldKeepsTheLocks();
	aQuiescedGuestIsNotAskedForItsMemory();
	aLifecycleCallInProgressIsNotWaitedFor("trim once");
	aLifecycleCallInProgressIsNotWaitedFor("suspend");
	aQuiesceDoesNotWaitForAMemoryRequest();
	return EXIT_SUCCESS;
}
