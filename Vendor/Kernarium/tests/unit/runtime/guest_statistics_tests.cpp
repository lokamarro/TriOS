// SPDX-License-Identifier: GPL-2.0-or-later
/* What the host reads of a guest without asking it: the CPU time of the vCPU
 * carriers, found by their kernel-given names among the process's threads,
 * and the RAM the guest's commitment says the host holds. */
#include "guest_statistics.hpp"
#include "memory/guest_ram_commitment.hpp"
#include "named_spinner.hpp"
#include "named_tests.hpp"
#include "require.hpp"
#include "wait_until.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using uml::runtime::GuestCpuTime;
using uml::runtime::GuestRamRetirementScope;
using uml::runtime::GuestRamUsage;
using uml::testing::NamedSpinner;
using uml::testing::require;
using uml::testing::waitUntil;

constexpr std::chrono::seconds bound {30};

void a_carrier_is_named_uml_vcpu_n_carrier()
{
	using uml::runtime::is_vcpu_carrier_name;
	require(is_vcpu_carrier_name("uml-vcpu-0-carrier"));
	require(is_vcpu_carrier_name("uml-vcpu-7-carrier"));
	require(is_vcpu_carrier_name("uml-vcpu-63-carrier"));
	require(!is_vcpu_carrier_name(""));
	require(!is_vcpu_carrier_name("uml-vcpu--carrier"));
	require(!is_vcpu_carrier_name("uml-vcpu-0"));
	require(!is_vcpu_carrier_name("uml-vcpu-0-carrier2"));
	require(!is_vcpu_carrier_name("uml-vcpu-x-carrier"));
	require(!is_vcpu_carrier_name("uml-vcpu-0x1-carrier"));
	require(!is_vcpu_carrier_name("uml-vcpu-0-carrier-extra"));
	require(!is_vcpu_carrier_name("xuml-vcpu-0-carrier"));
	require(!is_vcpu_carrier_name("uml-irq-notify"));
	require(!is_vcpu_carrier_name("uml-rtc-alarm"));
	require(!is_vcpu_carrier_name("uml-vcpu-carrier"));
}

/* The sum is the carriers' own time and no other thread's, however much the
 * others spend: a carrier counts, and a thread of the kernel or the host that
 * is not one does not, named uml-* or not. */
void only_carriers_count_and_their_time_is_their_own()
{
	constexpr std::chrono::milliseconds carrier_spend {60};
	constexpr std::chrono::milliseconds decoy_spend {400};
	NamedSpinner first {"uml-vcpu-0-carrier", carrier_spend};
	NamedSpinner second {"uml-vcpu-1-carrier", carrier_spend};
	NamedSpinner notify {"uml-irq-notify", decoy_spend};
	NamedSpinner rtc {"uml-rtc-alarm", decoy_spend};
	NamedSpinner unnamed {"", decoy_spend};
	NamedSpinner lookalike {"uml-vcpu-0-carrier-extra", decoy_spend};
	const std::uint64_t expected =
		first.awaitSpent() + second.awaitSpent();
	(void)notify.awaitSpent();
	(void)rtc.awaitSpent();
	(void)unnamed.awaitSpent();
	(void)lookalike.awaitSpent();

	const GuestCpuTime time = uml::runtime::guest_cpu_time();
	require(time.listed, "the process's threads must be listed");
	require(time.carriers == 2, "exactly the two carriers are found");
	/* The system adds the little a thread used after it last read its own
	 * clock, and keeps microseconds. */
	require(time.nanoseconds + 1000000ULL >= expected,
		"the sum must cover what the carriers used");
	require(time.nanoseconds <= expected + 10000000ULL,
		"the sum must not include any other thread's time");
	require(time.nanoseconds <
			expected + 2 * static_cast<std::uint64_t>(
				decoy_spend.count()) * 1000000ULL,
		"a decoy's time was counted");
}

void an_ended_carrier_takes_its_time_with_it()
{
	NamedSpinner first {"uml-vcpu-0-carrier", std::chrono::milliseconds {40}};
	auto second = std::make_unique<NamedSpinner>("uml-vcpu-1-carrier",
		std::chrono::milliseconds {40});
	const std::uint64_t first_used = first.awaitSpent();
	const std::uint64_t second_used = second->awaitSpent();
	const GuestCpuTime both = uml::runtime::guest_cpu_time();
	require(both.carriers == 2 &&
		both.nanoseconds + 1000000ULL >= first_used + second_used,
		"both carriers count");
	second.reset();
	require(waitUntil([] {
		return uml::runtime::guest_cpu_time().carriers == 1;
	}, bound), "an ended carrier must drop out");
	const GuestCpuTime alone = uml::runtime::guest_cpu_time();
	require(alone.nanoseconds + 1000000ULL >= first_used &&
		alone.nanoseconds < both.nanoseconds,
		"the sum starts again from the carriers that are left");
}

void no_carrier_is_no_time()
{
	const GuestCpuTime time = uml::runtime::guest_cpu_time();
	require(time.listed && time.carriers == 0 && time.nanoseconds == 0,
		"a process with no carrier has no guest CPU time");
}

class Mapping final {
public:
	explicit Mapping(std::size_t size) : size_ {size}
	{
		void *const mapping = ::mmap(nullptr, size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANON, -1, 0);
		require(mapping != MAP_FAILED, "anonymous mapping failed");
		data_ = static_cast<std::byte *>(mapping);
	}
	~Mapping() { (void)::munmap(data_, size_); }
	Mapping(const Mapping &) = delete;
	Mapping &operator=(const Mapping &) = delete;

	[[nodiscard]] std::byte *data() const noexcept { return data_; }

private:
	std::byte *data_ {};
	std::size_t size_ {};
};

void publish(Mapping &ram, std::size_t pages, std::size_t page)
{
	uml::memory::publish_guest_ram_commitment(
		std::make_unique<uml::memory::GuestRamCommitment>(
			ram.data(), pages * page, 0, pages * page, false, page));
	require(uml::memory::guest_ram_commitment() != nullptr,
		"the commitment must be published");
}

void retire()
{
	const GuestRamRetirementScope retirement;
	uml::memory::retire_guest_ram_commitment();
}

/* The held RAM is the guest's RAM less what it released, and nothing is
 * claimed before the guest's kernel has claimed it or after it was handed
 * back. */
void the_held_ram_follows_the_commitment()
{
	const std::size_t page = static_cast<std::size_t>(::getpagesize());
	constexpr std::size_t pages = 64;
	Mapping ram {pages * page};
	require(!uml::runtime::guest_ram_usage().has_value(),
		"nothing is held before the guest claims its RAM");

	publish(ram, pages, page);
	auto usage = uml::runtime::guest_ram_usage();
	require(usage.has_value() && usage->ram_bytes == pages * page &&
		usage->held_bytes == pages * page,
		"a fresh commitment holds all of the RAM");

	auto *const commitment = uml::memory::guest_ram_commitment();
	require(commitment->release(8 * page, 16 * page) == 0, "release failed");
	usage = uml::runtime::guest_ram_usage();
	require(usage.has_value() && usage->ram_bytes == pages * page &&
		usage->held_bytes == (pages - 16) * page,
		"released pages leave the held RAM");

	require(commitment->commit(8 * page, 4 * page) == 0, "commit failed");
	usage = uml::runtime::guest_ram_usage();
	require(usage.has_value() && usage->held_bytes == (pages - 12) * page,
		"committed pages are held again");

	retire();
	require(!uml::runtime::guest_ram_usage().has_value(),
		"nothing is held once the guest is handed back");
}

/* A reader that arrives while the commitment is retired and published again
 * (a restart's hand-back) sees nothing or a whole commitment, never one that
 * is being deleted: the retirement scope excludes it. Under a sanitizer a
 * reader that met the deleted object would be reported. */
void a_reader_never_meets_a_commitment_being_retired()
{
	const std::size_t page = static_cast<std::size_t>(::getpagesize());
	constexpr std::size_t pages = 32;
	Mapping ram {pages * page};
	std::atomic_bool stop {};
	std::atomic<unsigned> seen {};
	std::thread reader {[&] {
		while (!stop.load(std::memory_order_acquire)) {
			const auto usage = uml::runtime::guest_ram_usage();
			if (!usage)
				continue;
			require(usage->ram_bytes == pages * page &&
				usage->held_bytes <= usage->ram_bytes,
				"a reading is a whole commitment's");
			seen.fetch_add(1, std::memory_order_relaxed);
		}
	}};
	const auto deadline =
		std::chrono::steady_clock::now() + std::chrono::milliseconds {300};
	while (std::chrono::steady_clock::now() < deadline) {
		publish(ram, pages, page);
		std::this_thread::sleep_for(std::chrono::microseconds {200});
		retire();
	}
	stop.store(true, std::memory_order_release);
	reader.join();
	require(seen.load() != 0, "the reader never saw a commitment");
	require(!uml::memory::guest_ram_commitment(), "the last one is retired");
}

/* The held RAM is not read while a hand-back retires it, and the reader
 * does not wait for the hand-back to end. */
void a_hand_back_in_progress_is_not_waited_for()
{
	const std::size_t page = static_cast<std::size_t>(::getpagesize());
	constexpr std::size_t pages = 16;
	Mapping ram {pages * page};
	publish(ram, pages, page);
	std::mutex mutex;
	std::condition_variable changed;
	bool held {};
	bool read {};
	std::thread retiring {[&] {
		const GuestRamRetirementScope retirement;
		std::unique_lock lock {mutex};
		held = true;
		changed.notify_all();
		changed.wait(lock, [&] { return read; });
	}};
	{
		std::unique_lock lock {mutex};
		require(changed.wait_for(lock, bound, [&] { return held; }),
			"the hand-back never started");
	}
	require(!uml::runtime::guest_ram_usage().has_value(),
		"the RAM was read while it was being handed back");
	{
		const std::lock_guard lock {mutex};
		read = true;
	}
	changed.notify_all();
	retiring.join();
	require(uml::runtime::guest_ram_usage().has_value(),
		"the RAM was not read once the scope ended");
	retire();
}

} // namespace

int main()
{
	constexpr uml::testing::NamedTest tests[] = {
		{"a-carrier-is-named-uml-vcpu-n-carrier",
			a_carrier_is_named_uml_vcpu_n_carrier},
		{"no-carrier-is-no-time", no_carrier_is_no_time},
		{"only-carriers-count-and-their-time-is-their-own",
			only_carriers_count_and_their_time_is_their_own},
		{"an-ended-carrier-takes-its-time-with-it",
			an_ended_carrier_takes_its_time_with_it},
		{"the-held-ram-follows-the-commitment",
			the_held_ram_follows_the_commitment},
		{"a-reader-never-meets-a-commitment-being-retired",
			a_reader_never_meets_a_commitment_being_retired},
		{"a-hand-back-in-progress-is-not-waited-for",
			a_hand_back_in_progress_is_not_waited_for},
	};
	return uml::testing::runNamedTests(tests);
}
