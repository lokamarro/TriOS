// SPDX-License-Identifier: GPL-2.0-or-later
/* An executor lets go of the address spaces of a generation the MMU has
 * destroyed at its next entry into translated code, private and shared,
 * keeps the live ones, leaves the window's live residency as it was, and never
 * runs a dead one's translations again.
 *
 * Runs against the unit-test variant of the gadget executor
 * (uml_arm64_gadget_sweep_stress_executor), which counts live spaces. */
#include "allocation_failure.hpp"
#include "arm64_gadget_runtime_test_support.hpp"
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/gadgets/executor.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "memory/domain_contract.hpp"
#include "memory/domain_heap.hpp"
#include "static_mmu_test_support.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string_view>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;
namespace stress = gadgets::stress;
using uml::emulator::tests::JitBacking;
using uml::emulator::tests::JitMapping;
using uml::emulator::tests::require;
using namespace uml::emulator::tests::gadget_runtime;

namespace {

void set_shared(bool shared)
{
	require(setenv("DARWIN_UML_GADGET_SHARED_BODIES", shared ? "1" : "0",
		1) == 0);
}

/* D1: one executor runs two address spaces; the first one's generation is
 * destroyed. Nothing happens until the executor enters translated code again,
 * then its space goes, and the other keeps every translation it had. */
void a_dead_space_goes_at_the_next_entry(unsigned long first, bool shared)
{
	set_shared(shared);
	JitBacking dying_backing{1};
	put_program(dying_backing, 1, 2);
	JitBacking live_backing{1};
	put_program(live_backing, 3, 4);
	auto dying = std::make_unique<JitMapping>(first, code_base,
		dying_backing, 1);
	JitMapping live{first + 1, code_base, live_backing, 1};
	const auto spaces = stress::live_spaces();
	{
		const auto vcpu = std::make_unique<Vcpu>();
		auto &executor = vcpu->executor;
		auto &memory = vcpu->memory;
		auto &cpu = vcpu->cpu;
		require(run_to_system_call(executor, memory, first, cpu,
			code_base) == 4);
		require(cpu.registers[0] == 1 && cpu.registers[1] == 2);
		require(run_to_system_call(executor, memory, first + 1, cpu,
			code_base) == 4);
		require(cpu.registers[0] == 3 && cpu.registers[1] == 4);
		require(stress::live_spaces() == spaces + 2);
		const auto translations = executor.cache_diagnostics().translations;

		dying.reset();
		require(stress::live_spaces() == spaces + 2);
		require(run_to_system_call(executor, memory, first + 1, cpu,
			code_base) == 4);
		require(cpu.registers[0] == 3 && cpu.registers[1] == 4);
		require(stress::live_spaces() == spaces + 1);
		require(executor.cache_diagnostics().translations == translations);
	}
	require(stress::live_spaces() == spaces);
}

/* D2: two executors share a space whose generation dies. It lives until the
 * last of them enters translated code again: an executor that does not keeps
 * it, as it keeps a closed one. */
void a_shared_dead_space_goes_with_its_last_executor(unsigned long first)
{
	set_shared(true);
	JitBacking dying_backing{1};
	put_program(dying_backing, 1, 2);
	JitBacking live_backing{1};
	put_program(live_backing, 3, 4);
	auto dying = std::make_unique<JitMapping>(first, code_base,
		dying_backing, 1);
	JitMapping live{first + 1, code_base, live_backing, 1};
	const auto spaces = stress::live_spaces();
	{
		const auto a_vcpu = std::make_unique<Vcpu>();
		const auto b_vcpu = std::make_unique<Vcpu>();
		for (auto *vcpu : {a_vcpu.get(), b_vcpu.get()})
			for (const auto generation : {first, first + 1})
				require(run_to_system_call(vcpu->executor,
					vcpu->memory, generation, vcpu->cpu,
					code_base) == 4);
		require(b_vcpu->executor.cache_diagnostics().adoptions >= 1);
		require(stress::live_spaces() == spaces + 2);

		dying.reset();
		require(run_to_system_call(a_vcpu->executor, a_vcpu->memory,
			first + 1, a_vcpu->cpu, code_base) == 4);
		require(stress::live_spaces() == spaces + 2);
		require(run_to_system_call(b_vcpu->executor, b_vcpu->memory,
			first + 1, b_vcpu->cpu, code_base) == 4);
		require(b_vcpu->cpu.registers[0] == 3 &&
			b_vcpu->cpu.registers[1] == 4);
		require(stress::live_spaces() == spaces + 1);
	}
	require(stress::live_spaces() == spaces);
}

/* D3: a generation number the MMU gives out again after destroying it (which
 * the kernel never does) names new code. The cache must have let go of the old
 * translations, or a lookup would match a slot validated at the same
 * code-change count and run the dead address space's code. */
void a_number_given_out_again_runs_its_new_code(unsigned long first,
	bool shared)
{
	set_shared(shared);
	JitBacking old_backing{1};
	put_program(old_backing, 1, 2);
	JitBacking new_backing{1};
	put_program(new_backing, 5, 6);
	JitBacking elsewhere_backing{1};
	put_program(elsewhere_backing, 3, 4);
	auto mapping = std::make_unique<JitMapping>(first, code_base,
		old_backing, 1);
	JitMapping elsewhere{first + 1, code_base, elsewhere_backing, 1};
	{
		const auto vcpu = std::make_unique<Vcpu>();
		auto &executor = vcpu->executor;
		auto &memory = vcpu->memory;
		auto &cpu = vcpu->cpu;
		require(run_to_system_call(executor, memory, first, cpu,
			code_base) == 4);
		require(cpu.registers[0] == 1 && cpu.registers[1] == 2);
		mapping.reset();
		require(run_to_system_call(executor, memory, first + 1, cpu,
			code_base) == 4);
		mapping = std::make_unique<JitMapping>(first, code_base,
			new_backing, 1);
		require(run_to_system_call(executor, memory, first, cpu,
			code_base) == 4);
		require(cpu.registers[0] == 5 && cpu.registers[1] == 6);
	}
}

/* D4: an executor's streaming-mode caches hold address spaces too. A ZA-mode
 * run (SVCR.ZA, outside streaming mode, so any host runs it through an
 * injected SME profile) puts the dying generation's space in the ZA cache;
 * the next entry, in mode 0 elsewhere, lets go of it there as well. */
void a_dead_space_goes_from_a_streaming_mode_cache(unsigned long first,
	bool shared)
{
	set_shared(shared);
	auto host = ::uml::cpu::host_capabilities();
	for (const auto feature : {uml::cpu::ArmFeature::sme,
		     uml::cpu::ArmFeature::sme_f64f64,
		     uml::cpu::ArmFeature::sme_i16i64,
		     uml::cpu::ArmFeature::lse2})
		host.supported.set(static_cast<std::size_t>(feature));
	host.sme_svl_bytes = 64;
	host.sme_hardware = false;
	JitBacking dying_backing{1};
	put_program(dying_backing, 1, 2);
	JitBacking live_backing{1};
	put_program(live_backing, 3, 4);
	auto dying = std::make_unique<JitMapping>(first, code_base,
		dying_backing, 1);
	JitMapping live{first + 1, code_base, live_backing, 1};
	const auto spaces = stress::live_spaces();
	{
		const auto memory = std::make_unique<StaticGuestMemory>();
		const auto executor = std::make_unique<gadgets::GadgetExecutor>(
			gadgets::BuildOptions{.host_capabilities = host});
		const auto file = arm64::SmeRegisterFile::for_host(host);
		require(file.data() != nullptr);
		std::vector<std::byte> task(arm64::sme_storage_size(file.svl()));
		const auto cpu = std::make_unique<arm64::CpuState>();
		const auto run = [&](unsigned long generation, std::uint8_t svcr) {
			*cpu = {};
			cpu->bind_sme_registers(file);
			cpu->defer_sme_load(task.data(), svcr, 0);
			cpu->pc = code_base;
			for (std::size_t block = 0; block < 4; ++block) {
				const auto outcome =
					run_once(*executor, *memory, generation, *cpu);
				if (outcome.event == ::uml::cpu::EngineEvent::system_call)
					return;
				require(outcome.event ==
					::uml::cpu::EngineEvent::continued);
			}
			require(false);
		};
		run(first, arm64::svcr_za);
		require(cpu->svcr == arm64::svcr_za);
		require(cpu->registers[0] == 1 && cpu->registers[1] == 2);
		require(stress::live_spaces() == spaces + 1);
		dying.reset();
		run(first + 1, 0);
		require(cpu->registers[0] == 3 && cpu->registers[1] == 4);
		/* The ZA cache's space went as the mode-0 one came. */
		require(stress::live_spaces() == spaces + 1);
	}
	require(stress::live_spaces() == spaces);
}

/* D5: a dead space leaves its window place behind until the least-recently-
 * used rule reaches it, so the live spaces a window keeps are the ones it kept
 * before. The executor runs generations 0..7 of the window in order; 3 dies;
 * entering an eighth one retires 0, the least recently used, and keeps 1. With
 * the place given up, nothing would be retired and 0 would still hit. Once 1,
 * 0 and a ninth have been entered, the LRU has reached 3's place and retired
 * it instead of 4, so 4 still hits. */
void a_dead_space_keeps_its_window_place(unsigned long first, bool shared)
{
	set_shared(shared);
	constexpr std::size_t window = UML_GADGET_RESIDENT_GENERATIONS;
	std::vector<std::unique_ptr<JitBacking>> backings;
	std::vector<std::unique_ptr<JitMapping>> mappings;
	for (std::size_t index = 0; index <= window + 1; ++index) {
		backings.push_back(std::make_unique<JitBacking>(1));
		put_program(*backings.back(), 10 + static_cast<unsigned>(index),
			20 + static_cast<unsigned>(index));
		mappings.push_back(std::make_unique<JitMapping>(first + index,
			code_base, *backings.back(), 1));
	}
	const auto vcpu = std::make_unique<Vcpu>();
	auto &executor = vcpu->executor;
	auto &memory = vcpu->memory;
	auto &cpu = vcpu->cpu;
	for (std::size_t index = 0; index < window; ++index)
		require(run_to_system_call(executor, memory, first + index, cpu,
			code_base) == 4);
	mappings[3].reset();
	require(run_to_system_call(executor, memory, first + window, cpu,
		code_base) == 4);
	require(cpu.registers[0] == 10 + window);
	const auto translations = executor.cache_diagnostics().translations;
	require(run_to_system_call(executor, memory, first + 1, cpu,
		code_base) == 4);
	require(cpu.registers[0] == 11);
	require(executor.cache_diagnostics().translations == translations);
	require(run_to_system_call(executor, memory, first, cpu,
		code_base) == 4);
	require(cpu.registers[0] == 10);
	require(executor.cache_diagnostics().translations == translations + 2);
	require(run_to_system_call(executor, memory, first + window + 1, cpu,
		code_base) == 4);
	require(cpu.registers[0] == 11 + window);
	const auto later = executor.cache_diagnostics().translations;
	require(run_to_system_call(executor, memory, first + 4, cpu,
		code_base) == 4);
	require(cpu.registers[0] == 14);
	require(executor.cache_diagnostics().translations == later);
}

/* D6: letting go of a dead space puts the bodies it held in limbo, which
 * allocates. Whichever allocation of that fails, the entry still runs, the
 * space is asked for again, and the next entry lets go of it. */
void a_dead_space_goes_after_a_failed_allocation(unsigned long first,
	bool shared)
{
	set_shared(shared);
	JitBacking dying_backing{1};
	put_program(dying_backing, 1, 2);
	JitBacking live_backing{1};
	put_program(live_backing, 3, 4);
	JitMapping live{first + 1, code_base, live_backing, 1};
	const auto spaces = stress::live_spaces();
	for (std::size_t allowed = 0;; ++allowed) {
		require(allowed < 256, "the failure point never moved past the "
			"end of letting go of a dead space");
		auto dying = std::make_unique<JitMapping>(first, code_base,
			dying_backing, 1);
		const auto vcpu = std::make_unique<Vcpu>();
		auto &executor = vcpu->executor;
		auto &memory = vcpu->memory;
		auto &cpu = vcpu->cpu;
		require(run_to_system_call(executor, memory, first, cpu,
			code_base) == 4);
		require(run_to_system_call(executor, memory, first + 1, cpu,
			code_base) == 4);
		require(stress::live_spaces() == spaces + 2);
		dying.reset();
		uml::testing::fail_after_allocations(allowed);
		require(run_to_system_call(executor, memory, first + 1, cpu,
			code_base) == 4);
		/* True once the entry made fewer allocations than allowed. */
		const bool swept =
			uml::testing::cancel_pending_allocation_failure();
		require(cpu.registers[0] == 3 && cpu.registers[1] == 4);
		require(run_to_system_call(executor, memory, first + 1, cpu,
			code_base) == 4);
		require(cpu.registers[0] == 3 && cpu.registers[1] == 4);
		require(stress::live_spaces() == spaces + 1,
			"a dead space was not let go of at the next entry");
		if (swept)
			return;
	}
}

/* D7: an executor that goes lets go of every body its caches hold, which
 * puts them in limbo. Whichever allocation of that fails, it goes whole, and
 * its address spaces go with it. */
void an_executor_goes_whole_after_a_failed_allocation(unsigned long first,
	bool shared)
{
	set_shared(shared);
	JitBacking one_backing{1};
	put_program(one_backing, 1, 2);
	JitBacking two_backing{1};
	put_program(two_backing, 3, 4);
	JitMapping one{first, code_base, one_backing, 1};
	JitMapping two{first + 1, code_base, two_backing, 1};
	const auto spaces = stress::live_spaces();
	for (std::size_t allowed = 0;; ++allowed) {
		require(allowed < 256, "the failure point never moved past the "
			"end of an executor");
		auto vcpu = std::make_unique<Vcpu>();
		for (const auto generation : {first, first + 1})
			require(run_to_system_call(vcpu->executor, vcpu->memory,
				generation, vcpu->cpu, code_base) == 4);
		require(stress::live_spaces() == spaces + 2);
		uml::testing::fail_after_allocations(allowed);
		vcpu.reset();
		/* True once the executor's end made fewer allocations than
		 * allowed. */
		const bool swept =
			uml::testing::cancel_pending_allocation_failure();
		require(stress::live_spaces() == spaces,
			"an executor that went kept an address space");
		if (swept)
			return;
	}
}

/* A wait that load can stretch without limit and only a wedge can end: it
 * returns once `done`, and fails when `progress` has not moved for
 * `wedge_limit`. What a run takes is measured in the rounds it completes,
 * never in seconds, so a loaded host only makes it slower. */
constexpr std::chrono::seconds wedge_limit{300};

template <typename Done, typename Progress>
void wait_for(Done done, Progress progress, const char *what)
{
	auto last = progress();
	auto moved = std::chrono::steady_clock::now();
	while (!done()) {
		std::this_thread::yield();
		const auto now = progress();
		if (now != last) {
			last = now;
			moved = std::chrono::steady_clock::now();
		} else {
			require(std::chrono::steady_clock::now() - moved <
					wedge_limit, what);
		}
	}
}

/* S1: vCPUs run generations from a moving window while the main thread maps
 * new ones and destroys the oldest once no vCPU can pick it any more. Every
 * run must see its own generation's code; at the end, after every vCPU has
 * entered once more, only the newest generation's spaces are alive. */
void concurrent_executors_follow_dead_generations(unsigned long first,
	bool shared)
{
	set_shared(shared);
	constexpr std::size_t vcpus = 6;
	constexpr std::size_t pickable = 4;
	constexpr std::size_t backings_count = 8;
	constexpr unsigned long rounds = 200;
	std::vector<std::unique_ptr<JitBacking>> backings;
	for (std::size_t index = 0; index < backings_count; ++index) {
		backings.push_back(std::make_unique<JitBacking>(1));
		put_program(*backings.back(), 100 + static_cast<unsigned>(index),
			200 + static_cast<unsigned>(index));
	}
	std::vector<std::unique_ptr<JitMapping>> mappings;
	const auto map = [&](unsigned long generation) {
		mappings.push_back(std::make_unique<JitMapping>(generation,
			code_base, *backings[generation % backings_count], 1));
	};
	const auto alive = stress::live_spaces();
	std::atomic<unsigned long> lowest{first};
	std::atomic<unsigned long> highest{first + pickable - 1};
	for (unsigned long generation = first; generation < first + pickable;
	     ++generation)
		map(generation);
	std::array<std::atomic<unsigned long>, vcpus> seen{};
	for (auto &value : seen)
		value.store(first);
	std::atomic<bool> stop{};
	std::atomic<std::size_t> runs{};
	std::atomic<std::size_t> parked{};
	std::atomic<bool> resume{};
	std::atomic<std::size_t> finished{};
	std::atomic<bool> release{};
	const auto vcpu = [&](std::size_t index) {
		auto owned_memory = std::make_unique<StaticGuestMemory>();
		auto owned_executor =
			std::make_unique<gadgets::GadgetExecutor>(sharing_options());
		auto owned_cpu = std::make_unique<arm64::CpuState>();
		auto &memory = *owned_memory;
		auto &executor = *owned_executor;
		auto &cpu = *owned_cpu;
		std::size_t round = 0;
		while (!stop.load()) {
			const auto low = lowest.load();
			seen[index].store(low);
			const auto high = highest.load();
			const auto generation =
				low + (round * (index + 1) + index) % (high - low + 1);
			++round;
			(void)run_to_system_call(executor, memory, generation, cpu,
				code_base);
			require(cpu.registers[0] == 100 + generation % backings_count);
			require(cpu.registers[1] == 200 + generation % backings_count);
			runs.fetch_add(1);
		}
		/* Out of translated code until every generation but the newest is
		 * gone; one more entry then lets go of them. */
		parked.fetch_add(1);
		while (!resume.load())
			std::this_thread::yield();
		const auto last = highest.load();
		(void)run_to_system_call(executor, memory, last, cpu, code_base);
		require(cpu.registers[0] == 100 + last % backings_count);
		finished.fetch_add(1);
		while (!release.load())
			std::this_thread::yield();
	};
	std::vector<std::thread> threads;
	for (std::size_t index = 0; index < vcpus; ++index)
		threads.emplace_back(vcpu, index);

	const auto all_seen = [&](unsigned long low) {
		for (const auto &value : seen)
			if (value.load() < low)
				return false;
		return true;
	};
	const auto seen_progress = [&] {
		unsigned long total = 0;
		for (const auto &value : seen)
			total += value.load();
		return total;
	};
	unsigned long destroyed = first;
	while (destroyed < first + rounds) {
		const auto goal = runs.load() + 50;
		wait_for([&] { return runs.load() >= goal; },
			[&] { return runs.load(); },
			"no vCPU completes a run");
		map(highest.load() + 1);
		highest.fetch_add(1);
		lowest.fetch_add(1);
		wait_for([&] { return all_seen(destroyed + 1); }, seen_progress,
			"a vCPU never starts a round on the newer window");
		mappings.erase(mappings.begin());
		++destroyed;
	}
	stop.store(true);
	wait_for([&] { return parked.load() == vcpus; },
		[&] { return parked.load(); }, "a vCPU never parks");
	while (mappings.size() > 1)
		mappings.erase(mappings.begin());
	resume.store(true);
	wait_for([&] { return finished.load() == vcpus; },
		[&] { return finished.load(); }, "a vCPU never finishes");
	/* Only the newest generation's spaces are alive: one shared by every
	 * vCPU, or one each. */
	const auto live = stress::live_spaces() - alive;
	release.store(true);
	for (auto &thread : threads)
		thread.join();
	require(destroyed == first + rounds);
	require(live == (shared ? 1U : vcpus));
	require(stress::live_spaces() == alive);
}

} // namespace

std::atomic_uint64_t heap_case_domain{};

bool provide_heap_case_domain(std::uint64_t &domain) noexcept
{
	domain = heap_case_domain.load(std::memory_order_acquire);
	return domain != 0;
}

/* D8: an executor's program slabs and its address spaces' record tables are
 * in heaps of the memory layer. They are committed in the heap region while
 * the executor runs, and the region has every byte back when the executor and
 * its spaces are gone and the memory domain has retired, whichever way the
 * space is held. The case runs in a domain of its own on a thread of its own:
 * the static MMU's scratch chunk goes back when the thread ends. */
void an_executor_and_its_spaces_give_their_heaps_back(
	unsigned long generation, bool shared)
{
	constexpr std::uint64_t domain = 61'000;
	set_shared(shared);
	uml::memory::set_domain_provider(&provide_heap_case_domain);
	auto &region = uml::memory::HeapRegion::instance();
	const auto heap_before = region.committed();
	const auto spaces = stress::live_spaces();
	std::size_t heap_running = 0;
	std::thread thread{[&] {
		heap_case_domain.store(domain, std::memory_order_release);
		JitBacking backing{1};
		put_program(backing, 1, 2);
		{
			const JitMapping mapping{generation, code_base, backing, 1};
			{
				const auto vcpu = std::make_unique<Vcpu>();
				require(run_to_system_call(vcpu->executor,
					vcpu->memory, generation, vcpu->cpu,
					code_base) == 4);
				require(vcpu->cpu.registers[0] == 1 &&
					vcpu->cpu.registers[1] == 2);
				heap_running = region.committed();
			}
		}
		heap_case_domain.store(0, std::memory_order_release);
	}};
	thread.join();
	uml::memory::retire_domain(domain);
	uml::memory::set_domain_provider(nullptr);
	/* Three heaps hold a first chunk of 256 KiB each: the domain's index, the
	 * engine's record tables and the cache's program slabs. */
	require(heap_running >= heap_before + 3 * (256U << 10),
		"the executor's translations are not in heaps");
	require(stress::live_spaces() == spaces);
	require(region.committed() == heap_before,
		"the executor left heap pages committed");
	require(uml::memory::LeasedHeap::engine().leases() == 0);
}

int main(int argc, char **argv)
{
	const uml::testing::RuntimeTestRoute runtime_route;
	stress::set_renewal_period(0);
	stress::set_renewal_floor(std::numeric_limits<std::size_t>::max());
	/* An argument names the one case to run, for mutation testing. */
	const std::string_view only = argc > 1 ? argv[1] : "";
	const std::pair<const char *, std::function<void()>> cases[] = {
		{"d1p", [] { a_dead_space_goes_at_the_next_entry(2000, false); }},
		{"d1s", [] { a_dead_space_goes_at_the_next_entry(2010, true); }},
		{"d2", [] { a_shared_dead_space_goes_with_its_last_executor(2020); }},
		{"d3p", [] { a_number_given_out_again_runs_its_new_code(2030, false); }},
		{"d3s", [] { a_number_given_out_again_runs_its_new_code(2040, true); }},
		{"d4p", [] { a_dead_space_goes_from_a_streaming_mode_cache(2050, false); }},
		{"d4s", [] { a_dead_space_goes_from_a_streaming_mode_cache(2060, true); }},
		{"d5p", [] { a_dead_space_keeps_its_window_place(2100, false); }},
		{"d5s", [] { a_dead_space_keeps_its_window_place(2120, true); }},
		{"d6p", [] { a_dead_space_goes_after_a_failed_allocation(2140, false); }},
		{"d6s", [] { a_dead_space_goes_after_a_failed_allocation(2150, true); }},
		{"d7p", [] { an_executor_goes_whole_after_a_failed_allocation(2160, false); }},
		{"d7s", [] { an_executor_goes_whole_after_a_failed_allocation(2170, true); }},
		{"d8p", [] { an_executor_and_its_spaces_give_their_heaps_back(2200, false); }},
		{"d8s", [] { an_executor_and_its_spaces_give_their_heaps_back(2210, true); }},
		{"s1p", [] { concurrent_executors_follow_dead_generations(3000, false); }},
		{"s1s", [] { concurrent_executors_follow_dead_generations(4000, true); }},
	};
	for (const auto &[name, run] : cases)
		if (only.empty() || only == name)
			run();
	return 0;
}
