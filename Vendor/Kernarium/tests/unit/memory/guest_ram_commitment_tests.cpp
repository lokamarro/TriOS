// SPDX-License-Identifier: GPL-2.0-or-later
/* Guest RAM commitment against real anonymous memory: a released range must
 * leave the process footprint, a committed one must be counted (and wired)
 * again, and repeated, overlapping or concurrent calls must reach the host
 * only for pages whose state they change. A wire must also survive the
 * signals guest CPU threads take while they commit. */
#include "mach_vm_region_test_support.hpp"
#include "memory/guest_ram_commitment.hpp"
#include "named_tests.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"
#include "wait_until.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/thread_act.h>
#include <memory>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using uml::memory::GuestRamCommitment;

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define UML_COMMITMENT_TEST_UNDER_ASAN 1
#endif
#endif

/* AddressSanitizer intercepts mlock() and does nothing, so no range it
 * builds is ever wired. */
#if defined(UML_COMMITMENT_TEST_UNDER_ASAN)
constexpr const char *wiring_unsupported =
	"AddressSanitizer ignores mlock()";
#else
constexpr const char *wiring_unsupported = nullptr;
#endif

constexpr std::size_t mib = 1024U * 1024U;

using uml::testing::host_page;
using uml::testing::require;
using uml::testing::requireSyscall;
using uml::testing::task_footprint;
using uml::testing::wired_count;

class Mapping final {
public:
	explicit Mapping(std::size_t size) : size_(size)
	{
		void *mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANON, -1, 0);
		require(mapping != MAP_FAILED, "anonymous mapping failed");
		data_ = static_cast<std::byte *>(mapping);
	}
	~Mapping()
	{
		(void)munlock(data_, size_);
		(void)munmap(data_, size_);
	}
	Mapping(const Mapping &) = delete;
	Mapping &operator=(const Mapping &) = delete;

	[[nodiscard]] std::byte *data() const noexcept { return data_; }

	void fill(std::size_t offset, std::size_t length, std::byte value)
	{
		std::memset(data_ + offset, static_cast<int>(value), length);
	}

private:
	std::byte *data_{};
	std::size_t size_{};
};

void release_leaves_and_commit_restores_the_footprint()
{
	const std::size_t size = 64 * mib;
	Mapping ram{size};
	ram.fill(0, size, std::byte{1});
	GuestRamCommitment commitment{ram.data(), size, 0, size, false,
		host_page()};
	require(commitment.released_bytes() == 0,
		"an unwired host starts with everything committed");

	const std::uint64_t before = task_footprint();
	require(commitment.release(0, size / 2) == 0, "release failed");
	const std::uint64_t released = task_footprint();
	require(before - released >= size / 2 - 4 * mib,
		"a released range must leave the footprint");
	require(commitment.released_bytes() == size / 2,
		"released bytes must count the released range");

	require(commitment.commit(0, size / 2) == 0, "commit failed");
	ram.fill(0, size / 2, std::byte{2});
	require(task_footprint() >= before - 4 * mib,
		"a committed range must be counted again once written");
	require(commitment.released_bytes() == 0, "commit must clear the range");
	for (std::size_t offset = 0; offset < size / 2; offset += host_page())
		require(ram.data()[offset] == std::byte{2},
			"data written after commit must survive");
}

void repeated_and_overlapping_calls_change_state_once()
{
	const std::size_t page = host_page();
	const std::size_t size = 256 * page;
	Mapping ram{size};
	ram.fill(0, size, std::byte{1});
	GuestRamCommitment commitment{ram.data(), size, 0, size, false, page};

	require(commitment.release(0, 64 * page) == 0, "first release failed");
	require(commitment.release(0, 64 * page) == 0, "repeated release failed");
	require(commitment.release(32 * page, 64 * page) == 0,
		"overlapping release failed");
	require(commitment.released_bytes() == 96 * page,
		"overlapping releases must count each page once");
	require(commitment.commit(16 * page, 16 * page) == 0,
		"partial commit failed");
	require(commitment.commit(16 * page, 16 * page) == 0,
		"repeated commit failed");
	require(commitment.released_bytes() == 80 * page,
		"a partial commit must leave the rest released");
	require(commitment.commit(0, size) == 0, "whole commit failed");
	require(commitment.released_bytes() == 0,
		"committing everything must leave nothing released");
}

void ranges_outside_the_managed_window_are_ignored_or_refused()
{
	const std::size_t page = host_page();
	const std::size_t size = 64 * page;
	Mapping ram{size};
	ram.fill(0, size, std::byte{1});
	/* The first 16 pages stand for the signed kernel image. */
	GuestRamCommitment commitment{ram.data(), size, 16 * page, size, false,
		page};
	require(commitment.managed_bytes() == 48 * page,
		"managed bytes must exclude the unmanaged prefix");
	require(commitment.release(0, 32 * page) == 0,
		"a range reaching into the prefix is not an error");
	require(commitment.released_bytes() == 16 * page,
		"only the managed part of a range may be released");

	errno = 0;
	require(commitment.release(1, page) == -1 && errno == EINVAL,
		"a misaligned offset must be refused");
	errno = 0;
	require(commitment.commit(0, page + 1) == -1 && errno == EINVAL,
		"a misaligned length must be refused");
	errno = 0;
	require(commitment.release(size - page, 2 * page) == -1 &&
			errno == EINVAL,
		"a range past the backing must be refused");
}

void a_wired_host_wires_on_commit_and_unwires_on_release()
{
	const std::size_t page = host_page();
	const std::size_t size = 64 * page;
	Mapping ram{size};
	GuestRamCommitment commitment{ram.data(), size, 0, 0, true, page};
	require(commitment.released_bytes() == size,
		"a wired host starts with everything released");
	require(wired_count(ram.data()) == 0, "nothing is wired before commit");

	require(commitment.commit(0, 8 * page) == 0, "commit failed");
	require(wired_count(ram.data()) > 0, "a committed range must be wired");
	require(wired_count(ram.data() + 16 * page) == 0,
		"an uncommitted range must stay unwired");
	require(commitment.release(0, 8 * page) == 0, "release failed");
	require(wired_count(ram.data()) == 0,
		"a released range must be unwired");
	require(commitment.wire_failures() == 0, "wiring must not have failed");
}

void concurrent_callers_keep_the_count_exact()
{
	const std::size_t page = host_page();
	constexpr std::size_t threads = 8;
	constexpr std::size_t chunk_pages = 32;
	const std::size_t size = threads * 8 * chunk_pages * page;
	Mapping ram{size};
	ram.fill(0, size, std::byte{1});
	GuestRamCommitment commitment{ram.data(), size, 0, size, false, page};

	std::vector<std::thread> workers;
	for (std::size_t worker = 0; worker < threads; ++worker) {
		workers.emplace_back([&, worker] {
			for (unsigned round = 0; round < 200; ++round) {
				/* Neighbouring workers share words of the state map. */
				const std::size_t chunk = (round * threads + worker) %
					(size / (chunk_pages * page));
				const std::uint64_t offset =
					chunk * chunk_pages * page;
				(void)commitment.release(offset, chunk_pages * page);
				(void)commitment.commit(offset, chunk_pages * page);
			}
		});
	}
	for (auto &worker : workers)
		worker.join();
	require(commitment.commit(0, size) == 0, "final commit failed");
	require(commitment.released_bytes() == 0,
		"concurrent release and commit must balance");
}

/* A shared file mapping of pages the host has never cached: a wire of it
 * reads every page from storage, so the wire's faults wait, as they wait for
 * memory on a device under pressure. */
class ColdFile final {
public:
	explicit ColdFile(std::size_t size) : size_(size)
	{
		std::string path =
			uml::testing::scratch_template("cold-wire-XXXXXX");
		descriptor_ = mkstemp(path.data());
		requireSyscall(descriptor_ >= 0, "cannot create the file to wire");
		(void)unlink(path.c_str());
		requireSyscall(fcntl(descriptor_, F_NOCACHE, 1) == 0,
			"cannot keep the file out of the host cache");
		const std::vector<std::byte> block(mib, std::byte{0x5a});
		for (std::size_t offset = 0; offset < size; offset += mib)
			requireSyscall(pwrite(descriptor_, block.data(), mib,
						static_cast<off_t>(offset)) ==
						static_cast<ssize_t>(mib),
				"cannot fill the file to wire");
		requireSyscall(fsync(descriptor_) == 0,
			"cannot flush the file to wire");
		void *mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE,
			MAP_SHARED, descriptor_, 0);
		requireSyscall(mapping != MAP_FAILED,
			"cannot map the file to wire");
		data_ = static_cast<std::byte *>(mapping);
	}
	~ColdFile()
	{
		(void)munlock(data_, size_);
		(void)munmap(data_, size_);
		(void)close(descriptor_);
	}
	ColdFile(const ColdFile &) = delete;
	ColdFile &operator=(const ColdFile &) = delete;

	[[nodiscard]] std::byte *data() const noexcept { return data_; }

private:
	std::byte *data_{};
	std::size_t size_{};
	int descriptor_{-1};
};

std::atomic<unsigned long> signals_taken{0};

void take_signal(int) noexcept
{
	signals_taken.fetch_add(1, std::memory_order_relaxed);
}

/* Posts SIGUSR1 to one thread every few microseconds while it lives, as
 * guest CPUs post their IPI wake signal to one another. Its constructor
 * returns once the first is posted, because a loaded host can schedule the
 * sender only after the work it is meant to disturb is over. The handler
 * stays installed, so a signal still pending when the storm ends is
 * harmless. */
class SignalStorm final {
public:
	explicit SignalStorm(pthread_t target)
	{
		struct sigaction action {};
		action.sa_handler = take_signal;
		sigemptyset(&action.sa_mask);
		require(sigaction(SIGUSR1, &action, nullptr) == 0,
			"cannot catch SIGUSR1");
		sender_ = std::thread([this, target] {
			do {
				(void)pthread_kill(target, SIGUSR1);
				posted_.fetch_add(1, std::memory_order_release);
				std::this_thread::sleep_for(
					std::chrono::microseconds{20});
			} while (!stopping_.load(std::memory_order_relaxed));
		});
		const auto sent = [this] {
			return posted_.load(std::memory_order_acquire) != 0;
		};
		require(uml::testing::waitUntil(sent, std::chrono::seconds{10}),
			"the signal sender never ran");
	}
	~SignalStorm()
	{
		stopping_.store(true, std::memory_order_relaxed);
		sender_.join();
	}
	SignalStorm(const SignalStorm &) = delete;
	SignalStorm &operator=(const SignalStorm &) = delete;

private:
	std::atomic<bool> stopping_{false};
	std::atomic<unsigned long> posted_{0};
	std::thread sender_;
};

constexpr std::size_t cold_size = 8 * mib;
constexpr unsigned cold_attempts = 4;

/* The cold files a case wires, all built before its SignalStorm starts. A
 * signal that reaches a thread inside a setup call can end the call with
 * EINTR (creating the file did, on a loaded host), so the storm disturbs the
 * wire under test and nothing the case builds. */
class ColdFiles final {
public:
	explicit ColdFiles(std::size_t count)
	{
		for (std::size_t file = 0; file < count; ++file)
			files_.push_back(std::make_unique<ColdFile>(cold_size));
	}

	[[nodiscard]] const ColdFile &operator[](std::size_t index) const
	{
		return *files_[index];
	}

	/* Unwires and unmaps a file once the attempt that used it is over. */
	void retire(std::size_t index) { files_[index].reset(); }

private:
	std::vector<std::unique_ptr<ColdFile>> files_;
};

/* XNU ends a wire whose faults wait when a signal reaches the thread, and
 * mlock then fails with ENOMEM. This is the hazard the commitment guards
 * against; if a host stops showing it, the guard and this case need a new
 * look rather than a quiet pass. */
void a_signalled_wire_of_cold_pages_fails_with_enomem()
{
	const ColdFiles files{cold_attempts};
	bool ended = false;
	int error = 0;
	SignalStorm storm{pthread_self()};
	for (unsigned attempt = 0; attempt < cold_attempts && !ended;
	     ++attempt) {
		errno = 0;
		ended = mlock(files[attempt].data(), cold_size) != 0;
		error = errno;
	}
	require(ended && error == ENOMEM,
		"a signalled mlock of cold pages must fail with ENOMEM");
}

/* Guest CPU threads take IPI signals while they commit; a commit, and the
 * readmission that ends a hibernation, must wire regardless. */
void a_signal_does_not_cost_a_commit_its_wiring()
{
	const std::size_t page = host_page();
	/* Each attempt wires two files nothing has read yet. */
	ColdFiles files{2 * cold_attempts};
	const unsigned long before =
		signals_taken.load(std::memory_order_relaxed);
	SignalStorm storm{pthread_self()};
	for (unsigned attempt = 0; attempt < cold_attempts; ++attempt) {
		const ColdFile &committed = files[2 * attempt];
		GuestRamCommitment commitment{committed.data(), cold_size, 0, 0,
			true, page};
		requireSyscall(commitment.commit(0, cold_size) == 0,
			"a signalled commit must succeed");
		require(commitment.wire_failures() == 0,
			"a signalled commit must wire");
		require(wired_count(committed.data()) > 0,
			"a signalled commit must leave its range wired");

		const ColdFile &readmitted = files[2 * attempt + 1];
		GuestRamCommitment held{readmitted.data(), cold_size, 0,
			cold_size, true, page};
		requireSyscall(held.evict(0, cold_size / page) == 0,
			"evict failed");
		requireSyscall(held.readmit(0, cold_size / page) == 0,
			"readmit failed");
		require(held.wire_failures() == 0,
			"a signalled readmission must wire");
		require(wired_count(readmitted.data()) > 0,
			"a signalled readmission must leave its range wired");
		files.retire(2 * attempt);
		files.retire(2 * attempt + 1);
	}
	require(signals_taken.load(std::memory_order_relaxed) > before,
		"the committing thread must have taken signals");
}

/* Issues thread_abort_safely() on one thread, `delay` after fire(): the call
 * XNU's psignal makes on a thread that has a signal pending and not held. */
class AbortRequest final {
public:
	explicit AbortRequest(mach_port_t target)
		: target_(target), issuer_([this] { issue(); })
	{
	}
	~AbortRequest()
	{
		stopping_.store(true, std::memory_order_release);
		issuer_.join();
	}
	AbortRequest(const AbortRequest &) = delete;
	AbortRequest &operator=(const AbortRequest &) = delete;

	void fire(std::chrono::microseconds delay)
	{
		delay_ = delay;
		phase_.store(armed, std::memory_order_release);
	}
	/* Returns once the request has been issued. */
	void issued()
	{
		const auto done = [this] {
			return phase_.load(std::memory_order_acquire) == issued_phase;
		};
		require(uml::testing::waitUntil(done, std::chrono::seconds{10},
				std::chrono::milliseconds{0}),
			"the abort request was never issued");
		phase_.store(idle, std::memory_order_release);
	}

private:
	static constexpr int idle = 0;
	static constexpr int armed = 1;
	static constexpr int issued_phase = 2;

	void issue()
	{
		const auto wanted = [this] {
			return phase_.load(std::memory_order_acquire) == armed ||
				stopping_.load(std::memory_order_acquire);
		};
		while (!stopping_.load(std::memory_order_acquire)) {
			if (!uml::testing::waitUntil(wanted, std::chrono::seconds{1},
				    std::chrono::milliseconds{0}) ||
			    stopping_.load(std::memory_order_acquire))
				continue;
			const auto begin = std::chrono::steady_clock::now();
			while (std::chrono::steady_clock::now() - begin < delay_) {
			}
			(void)thread_abort_safely(target_);
			phase_.store(issued_phase, std::memory_order_release);
		}
	}

	const mach_port_t target_;
	std::chrono::microseconds delay_{};
	std::atomic<int> phase_{idle};
	std::atomic<bool> stopping_{false};
	std::thread issuer_;
};

/* The hold is not a complete guard. psignal sets a thread's abort flag when a
 * signal is not held, and a post that read the thread's mask just before the
 * hold took effect sets it after, inside the wire, whose next abortable wait
 * then fails. This lands that call on the committing thread at delays through
 * each commit, from before its mlock starts to after the pages are read in, as
 * a post delayed on a loaded host does. The flag fails the wire it lands in
 * and ends with the call, so the commit must wire all the same. */
void an_abort_after_the_hold_does_not_cost_a_commit_its_wiring()
{
	const std::size_t page = host_page();
	constexpr std::size_t chunk = 256 * 1024;
	constexpr unsigned rounds = 64;
	/* Every round wires a chunk nothing has read yet. */
	const ColdFile file{rounds * chunk};
	GuestRamCommitment commitment{file.data(), rounds * chunk, 0, 0, true,
		page};
	const mach_port_t committer = mach_thread_self();
	{
		AbortRequest abort{committer};
		for (unsigned round = 0; round < rounds; ++round) {
			abort.fire(std::chrono::microseconds{round % 8 * 25});
			const int result = commitment.commit(round * chunk, chunk);
			const int error = errno;
			abort.issued();
			errno = error;
			requireSyscall(result == 0,
				"a commit lost its wiring to an abort after the hold");
			require(commitment.wire_failures() == 0,
				"a commit must wire");
			require(wired_count(file.data() + round * chunk) > 0,
				"a commit must leave its range wired");
		}
	}
	(void)mach_port_deallocate(mach_task_self(), committer);
}

} // namespace

int main()
{
	const uml::testing::NamedTest tests[] = {
		{"release leaves and commit restores the footprint",
			release_leaves_and_commit_restores_the_footprint},
		{"repeated and overlapping calls change state once",
			repeated_and_overlapping_calls_change_state_once},
		{"ranges outside the managed window are ignored or refused",
			ranges_outside_the_managed_window_are_ignored_or_refused},
		{"a wired host wires on commit and unwires on release",
			a_wired_host_wires_on_commit_and_unwires_on_release,
			wiring_unsupported},
		{"concurrent callers keep the count exact",
			concurrent_callers_keep_the_count_exact},
		{"a signalled wire of cold pages fails with ENOMEM",
			a_signalled_wire_of_cold_pages_fails_with_enomem,
			wiring_unsupported},
		{"a signal does not cost a commit its wiring",
			a_signal_does_not_cost_a_commit_its_wiring,
			wiring_unsupported},
		{"an abort after the hold does not cost a commit its wiring",
			an_abort_after_the_hold_does_not_cost_a_commit_its_wiring,
			wiring_unsupported},
	};
	return uml::testing::runNamedTests(tests);
}
