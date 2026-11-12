// SPDX-License-Identifier: GPL-2.0-or-later
/* The order of a hibernation, against steps that record it, and the same
 * hibernation against real threads and a real guest RAM. The order is the
 * contract: a thread is held before any page leaves, and no thread is let go
 * before every page is back and verified. */
#include "guest_hibernation.hpp"
#include "kernel_threads.hpp"
#include "memory/guest_ram_commitment.hpp"
#include "named_tests.hpp"
#include "require.hpp"
#include "scratch_directory.hpp"
#include "thread_nap.hpp"
#include "watchdog.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mach/mach.h>
#include <memory>
#include <pthread.h>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace uml::runtime;
using uml::testing::nap;
using uml::testing::require;

enum class Event { prepare, hold, write, restore, release, abort_on, abort_off,
	failure };

std::vector<Event> events;

/* What each fake step answers, and what it checks of the others. */
struct Script final {
	bool prepare{true};
	bool hold{true};
	bool write{true};
	bool restore{true};
};

class RecordingSteps final : public HibernationSteps {
public:
	explicit RecordingSteps(Script script) : script_(script) {}

	bool prepare() noexcept override
	{
		events.push_back(Event::prepare);
		require(!held_, "prepared while threads are held");
		return script_.prepare;
	}

	bool hold(HibernationReport &, HoldCondition condition) noexcept override
	{
		events.push_back(Event::hold);
		heldWith = condition;
		require(!held_, "held twice");
		held_ = script_.hold;
		return script_.hold;
	}

	bool write(HibernationReport &) noexcept override
	{
		events.push_back(Event::write);
		require(held_, "RAM written out while threads still run");
		return script_.write;
	}

	bool restore(HibernationReport &) noexcept override
	{
		events.push_back(Event::restore);
		require(held_, "RAM restored after the threads were let go");
		restored_ = script_.restore;
		return script_.restore;
	}

	void release() noexcept override
	{
		events.push_back(Event::release);
		require(held_, "released without holding");
		require(restored_, "threads let go before the RAM was whole");
		held_ = false;
	}

	void set_abort(bool abort) noexcept override
	{
		events.push_back(abort ? Event::abort_on : Event::abort_off);
	}

	/* The condition the last hold was given. */
	static inline HoldCondition heldWith{};

private:
	Script script_;
	bool held_{false};
	bool restored_{false};
};

[[nodiscard]] bool accept_always() noexcept
{
	return true;
}

void the_failure_handler() noexcept
{
	events.push_back(Event::failure);
}

[[nodiscard]] GuestHibernation make(Script script)
{
	return GuestHibernation{std::make_unique<RecordingSteps>(script),
		&the_failure_handler};
}

void require_events(std::initializer_list<Event> expected, const char *message)
{
	require(events == std::vector<Event>(expected), message);
	events.clear();
}

void a_hibernation_holds_before_it_writes_and_restores_before_it_releases()
{
	events.clear();
	GuestHibernation hibernation = make({});
	require(hibernation.hibernate(), "the hibernation must take place");
	require(hibernation.hibernated(), "and say so");
	require_events({Event::abort_off, Event::prepare, Event::hold,
		Event::write}, "hibernate: prepare, hold, then write");
	require(hibernation.restore(), "the restore must succeed");
	require(!hibernation.hibernated(), "and the guest is whole again");
	require_events({Event::restore, Event::release},
		"restore: the RAM comes back before any thread is let go");
}

/* A caller that must act once the threads are held, and before the RAM goes,
 * takes the two steps apart. Where there is nothing to hibernate it learns
 * that no thread was held, and where the threads cannot be held that nothing
 * changed. */
void the_hold_comes_before_the_write_and_says_what_it_found()
{
	events.clear();
	GuestHibernation held = make({});
	require(held.hold() == HibernationHold::held, "the threads are held");
	require_events({Event::abort_off, Event::prepare, Event::hold},
		"the hold alone writes nothing");
	require(held.hold() == HibernationHold::held, "a second hold is the first");
	require_events({}, "and holds nothing twice");
	held.write();
	require_events({Event::write}, "the write follows with the threads held");
	held.write();
	require_events({}, "and happens once");
	require(held.restore(), "the restore succeeds");
	require_events({Event::restore, Event::release}, "in order");

	GuestHibernation unwritten = make({});
	require(unwritten.hold() == HibernationHold::held, "held");
	events.clear();
	require(unwritten.restore(), "a hold without a write is restored too");
	require_events({Event::restore, Event::release},
		"and the threads run once the RAM is whole");

	GuestHibernation nothing = make({.prepare = false});
	require(nothing.hold() == HibernationHold::unavailable,
		"nothing to hibernate holds no thread");
	nothing.write();
	events.clear();
	GuestHibernation unheld = make({.hold = false});
	require(unheld.hold() == HibernationHold::failed,
		"threads that cannot be held are reported");
	unheld.write();
	require_events({Event::abort_off, Event::prepare, Event::hold},
		"and a write after a refusal writes nothing");
}

/* What the caller needs of the moment the threads are held reaches the steps,
 * for the hold alone and for the two steps together. */
void the_callers_condition_reaches_the_steps()
{
	events.clear();
	GuestHibernation plain = make({});
	require(plain.hold() == HibernationHold::held, "held");
	require(RecordingSteps::heldWith == nullptr,
		"no condition is asked of a caller that gave none");
	require(plain.restore(), "restored");

	GuestHibernation asked = make({});
	require(asked.hold(&accept_always) == HibernationHold::held, "held");
	require(RecordingSteps::heldWith == &accept_always,
		"the hold passes the caller's condition to the steps");
	require(asked.restore(), "restored");

	GuestHibernation both = make({});
	require(both.hibernate(&accept_always), "hibernated");
	require(RecordingSteps::heldWith == &accept_always,
		"so does the hibernation");
	require(both.restore(), "restored");
	events.clear();
	RecordingSteps::heldWith = nullptr;
}

void nothing_happens_when_there_is_nothing_to_hibernate()
{
	events.clear();
	GuestHibernation hibernation = make({.prepare = false});
	require(!hibernation.hibernate(), "no hibernation");
	require_events({Event::abort_off, Event::prepare},
		"nothing but the preparation ran");
	require(hibernation.restore(), "a restore of nothing succeeds");
	require_events({}, "and touches nothing");
}

void threads_that_cannot_be_held_leave_the_ram_alone()
{
	events.clear();
	GuestHibernation hibernation = make({.hold = false});
	require(!hibernation.hibernate(), "no hibernation");
	require(!hibernation.hibernated(), "and nothing to restore");
	require_events({Event::abort_off, Event::prepare, Event::hold},
		"the RAM is not written when the threads are not held");
	require(hibernation.restore(), "a restore of nothing succeeds");
	require_events({}, "and touches nothing");
}

void a_partial_write_is_still_restored()
{
	events.clear();
	GuestHibernation hibernation = make({.write = false});
	require(hibernation.hibernate(), "what was evicted must come back");
	require(hibernation.hibernated(), "so the guest is hibernated");
	events.clear();
	require(hibernation.restore(), "the restore succeeds");
	require_events({Event::restore, Event::release}, "and runs in order");
}

void a_restore_that_fails_lets_nothing_run()
{
	events.clear();
	GuestHibernation hibernation = make({.restore = false});
	require(hibernation.hibernate(), "hibernated");
	events.clear();
	require(!hibernation.restore(), "the restore must fail");
	require_events({Event::restore, Event::failure},
		"the failure ends the process; no thread is let go");
	require(hibernation.hibernated(), "and the guest stays hibernated");
}

void an_abort_request_reaches_the_steps()
{
	events.clear();
	GuestHibernation hibernation = make({});
	hibernation.request_abort();
	require_events({Event::abort_on}, "the request reaches the steps");
	require(hibernation.hibernate(), "a hibernation");
	require(events.front() == Event::abort_off,
		"a hibernation starts with the request cleared");
	events.clear();
	require(hibernation.restore(), "restored");
	events.clear();
}

void the_policy_follows_the_mode_and_the_wiring()
{
	require(hibernation_applies(HibernationMode::automatic, true),
		"a wired guest hibernates by default");
	require(!hibernation_applies(HibernationMode::automatic, false),
		"an unwired guest does not");
	require(!hibernation_applies(HibernationMode::off, true), "off is off");
	require(hibernation_applies(HibernationMode::on, false), "on forces it");
}

/* ---- Real threads and real guest RAM. ---- */

constexpr std::size_t held_pages = 192;
constexpr std::size_t spare_pages = 64;

struct Ram final {
	std::byte *root{};
	std::size_t page{};
	std::size_t size{};
	uml::memory::GuestRamCommitment *commitment{};
	std::atomic<std::uint64_t> pattern_errors{0};
	std::atomic<std::uint64_t> ticks{0};
	std::atomic<std::uint64_t> changes{0};
	std::atomic<bool> stop{false};
	/* Odd while the kernel thread is to make no change to what the guest
	 * holds; it names the request it last saw in `acknowledged`. */
	std::atomic<unsigned> quiet{0};
	std::atomic<unsigned> acknowledged{0};
};

[[nodiscard]] std::uint64_t word_of(std::size_t page, std::size_t word)
{
	return (page + 1) * 0x9e3779b97f4a7c15ULL ^ (word * 0xff51afd7ed558ccdULL);
}

/* A kernel thread: it reads guest RAM, and changes what the guest holds, as
 * the kernel does. It makes no other call that a hold could be stuck in. */
void *kernel_body(void *argument)
{
	auto *ram = static_cast<Ram *>(argument);
	std::size_t round = 0;
	while (!ram->stop.load(std::memory_order_acquire)) {
		const std::size_t page = round++ % held_pages;
		const auto *words = reinterpret_cast<const std::uint64_t *>(
			ram->root + page * ram->page);
		for (std::size_t word = 0; word < ram->page / 8; word += 97)
			if (words[word] != word_of(page, word))
				ram->pattern_errors.fetch_add(1);
		const unsigned asked = ram->quiet.load(std::memory_order_acquire);
		if ((asked & 1) == 0) {
			/* The spare pages come and go, as the guest's page reporting
			 * has them. */
			const std::uint64_t at =
				(held_pages + round % spare_pages) * ram->page;
			if ((round & 1) != 0)
				(void)ram->commitment->release(at, ram->page);
			else
				(void)ram->commitment->commit(at, ram->page);
			ram->changes.fetch_add(1);
		} else {
			ram->acknowledged.store(asked, std::memory_order_release);
		}
		ram->ticks.fetch_add(1, std::memory_order_relaxed);
		nap(50);
	}
	return nullptr;
}

void wait_for_ticks(const Ram &ram)
{
	const auto before = ram.ticks.load();
	for (int attempt = 0; attempt < 1000; ++attempt) {
		if (ram.ticks.load() > before + 3)
			return;
		nap(2000);
	}
	uml::testing::fail("the kernel thread does not run");
}

[[nodiscard]] bool stood_still(const Ram &ram)
{
	const auto ticks = ram.ticks.load();
	const auto changes = ram.changes.load();
	nap(40000);
	return ram.ticks.load() == ticks && ram.changes.load() == changes;
}

/* Returns when the kernel thread has made its last change to what the guest
 * holds, and makes no other until `resume_changes`. It goes on reading guest
 * RAM. The hold refuses a moment at which a thread is stopped inside a change
 * (GuestRamImage::freeze_changes), so a test that counts the moments the hold
 * was asked about must keep the thread it owns out of the commitment. */
void stop_changes(Ram &ram)
{
	const unsigned request = ram.quiet.fetch_add(1) + 1;
	for (int attempt = 0; attempt < 1000; ++attempt) {
		if (ram.acknowledged.load(std::memory_order_acquire) == request)
			return;
		nap(2000);
	}
	uml::testing::fail("the kernel thread does not stop its changes");
}

void resume_changes(Ram &ram)
{
	ram.quiet.fetch_add(1);
}

/* Refuses the first two moments it is asked about, as a caller does whose
 * lock a stopped thread owns. */
std::atomic<unsigned> conditionAsked{0};

[[nodiscard]] bool refuse_twice() noexcept
{
	return conditionAsked.fetch_add(1) >= 2;
}

/* Accepts every moment it is asked about. At the first, the test lets go of
 * the kernel thread it stopped inside a change; the hold has stopped it too,
 * so the change cannot finish until the hold lets everything run. The thread
 * makes no change after that one. */
std::atomic<Ram *> stoppedRam{nullptr};
std::atomic<thread_act_t> stoppedThread{MACH_PORT_NULL};
std::atomic<unsigned> momentsAccepted{0};

[[nodiscard]] bool accept_and_let_the_change_finish() noexcept
{
	if (momentsAccepted.fetch_add(1) == 0) {
		stoppedRam.load()->quiet.fetch_add(1);
		(void)thread_resume(stoppedThread.load());
	}
	return true;
}

/* Stops `thread` while it is inside a change, as it is for a short part of
 * every round. A thread stopped there shows as a change in flight and cannot
 * leave the change until it is let go. */
[[nodiscard]] bool stop_inside_a_change(const Ram &ram, thread_act_t thread)
{
	for (int attempt = 0; attempt < 50000; ++attempt) {
		if (thread_suspend(thread) != KERN_SUCCESS)
			return false;
		if (ram.commitment->changes_in_flight() != 0)
			return true;
		(void)thread_resume(thread);
		nap(100);
	}
	return false;
}

/* A hold refuses a moment that the caller accepts when a thread is stopped
 * inside a change, and asks the caller again once it has been let go: every
 * moment but the last is a refusal that the hold counts, whoever made it. */
void a_change_in_flight_refuses_a_moment_the_caller_accepts(Ram &ram,
	GuestHibernation &hibernation, pthread_t thread)
{
	const thread_act_t act = pthread_mach_thread_np(thread);
	require(stop_inside_a_change(ram, act),
		"the kernel thread is never found inside a change");
	stoppedRam.store(&ram);
	stoppedThread.store(act);
	momentsAccepted.store(0);
	require(hibernation.hibernate(&accept_and_let_the_change_finish),
		"the hibernation must take place");
	const HibernationReport &report = hibernation.report();
	require(report.hold_unsettled >= 1,
		"a change in flight must refuse the moment the caller accepts");
	require(momentsAccepted.load() == report.hold_unsettled + 1,
		"every moment but the last must be a refusal the hold counted");
	require(hibernation.restore(), "the restore must succeed");
	resume_changes(ram);
}

void a_real_hibernation_holds_the_kernel_threads_and_brings_the_ram_back()
{
	Ram ram;
	ram.page = static_cast<std::size_t>(getpagesize());
	ram.size = (held_pages + spare_pages) * ram.page;
	void *mapping = mmap(nullptr, ram.size, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANON, -1, 0);
	require(mapping != MAP_FAILED, "mapping");
	ram.root = static_cast<std::byte *>(mapping);
	for (std::size_t page = 0; page < held_pages; ++page) {
		auto *words = reinterpret_cast<std::uint64_t *>(
			ram.root + page * ram.page);
		for (std::size_t word = 0; word < ram.page / 8; ++word)
			words[word] = word_of(page, word);
	}
	auto commitment = std::make_unique<uml::memory::GuestRamCommitment>(
		ram.root, ram.size, 0, 0, true, ram.page);
	ram.commitment = commitment.get();
	uml::memory::publish_guest_ram_commitment(std::move(commitment));
	require(ram.commitment->commit(0, ram.size) == 0, "commit");

	const std::string directory = uml::testing::scratch_directory().string();
	std::unique_ptr<GuestHibernation> hibernation =
		GuestHibernation::create(HibernationMode::on, directory);
	pthread_t thread;
	require(kernel_pthread_create(&thread, nullptr, &kernel_body, &ram) == 0,
		"the kernel thread");
	wait_for_ticks(ram);
	a_change_in_flight_refuses_a_moment_the_caller_accepts(ram,
		*hibernation, thread);
	wait_for_ticks(ram);

	for (int cycle = 0; cycle < 6; ++cycle) {
		/* The first hold is refused twice and settles on the third try. The
		 * kernel thread makes no change meanwhile: one in flight at the
		 * third moment would refuse it as well. */
		if (cycle == 0)
			stop_changes(ram);
		require(hibernation->hibernate(cycle == 0 ? &refuse_twice : nullptr),
			"the hibernation must take place");
		const HibernationReport &report = hibernation->report();
		if (cycle == 0) {
			require(conditionAsked.load() == 3 && report.hold_unsettled == 2,
				"the caller's condition decides the moment");
			resume_changes(ram);
		}
		require(report.threads_held == 1 && report.threads_registered == 1,
			"the kernel thread is the one thread held");
		require(report.written.evicted_bytes != 0, "RAM left the process");
		require(stood_still(ram), "the kernel thread runs while hibernated");
		const std::string image = directory + "/" +
			uml::memory::guest_ram_image_name(static_cast<int>(getpid()));
		require(!std::filesystem::exists(image),
			"the private RAM image must have no directory entry during suspension");
		require(hibernation->restore(), "the restore must succeed");
		require(hibernation->report().restored.chunks_failed == 0,
			"no chunk failed");
		wait_for_ticks(ram);
	}
	require(ram.pattern_errors.load() == 0,
		"the kernel thread saw guest RAM that was not whole");
	ram.stop.store(true);
	require(pthread_join(thread, nullptr) == 0, "join");
	for (std::size_t page = 0; page < held_pages; ++page) {
		const auto *words = reinterpret_cast<const std::uint64_t *>(
			ram.root + page * ram.page);
		for (std::size_t word = 0; word < ram.page / 8; ++word)
			require(words[word] == word_of(page, word),
				"guest RAM differs after the cycles");
	}
	hibernation.reset();
	require(!std::filesystem::exists(directory + "/" +
		uml::memory::guest_ram_image_name(static_cast<int>(getpid()))),
		"the image file must not outlive the hibernation");
}

} // namespace

int main()
{
	const uml::testing::Watchdog watchdog{"the guest hibernation tests",
		std::chrono::seconds{180}};
	const uml::testing::NamedTest tests[] = {
		{"a hibernation holds before it writes and restores before it releases",
			a_hibernation_holds_before_it_writes_and_restores_before_it_releases},
		{"the hold comes before the write and says what it found",
			the_hold_comes_before_the_write_and_says_what_it_found},
		{"the caller's condition reaches the steps",
			the_callers_condition_reaches_the_steps},
		{"nothing happens when there is nothing to hibernate",
			nothing_happens_when_there_is_nothing_to_hibernate},
		{"threads that cannot be held leave the RAM alone",
			threads_that_cannot_be_held_leave_the_ram_alone},
		{"a partial write is still restored", a_partial_write_is_still_restored},
		{"a restore that fails lets nothing run",
			a_restore_that_fails_lets_nothing_run},
		{"an abort request reaches the steps", an_abort_request_reaches_the_steps},
		{"the policy follows the mode and the wiring",
			the_policy_follows_the_mode_and_the_wiring},
		{"a real hibernation holds the kernel threads and brings the RAM back",
			a_real_hibernation_holds_the_kernel_threads_and_brings_the_ram_back},
	};
	return uml::testing::runNamedTests(tests);
}
