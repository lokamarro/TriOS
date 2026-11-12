// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "allocation_failure.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_cache.hpp"
#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/arm64/jit/jit_runtime_contract.hpp"
#include "cpu/arm64/jit/jit_shared_translations.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <vector>

using namespace uml::emulator::tests;

namespace {

namespace arm64 = uml::cpu::arm64;
using uml::cpu::arm64::JitCacheValidationDiagnostics;
using uml::cpu::arm64::JitExecutor;
using uml::cpu::arm64::JitExecutorDiagnosticsOptions;
using uml::cpu::arm64::JitExecutorOptions;
using uml::memory::StaticGuestMemory;

constexpr std::uint32_t system_call = 0xd400'0001U;

static_assert(sizeof(arm64::CacheSet) ==
              arm64::cache_ways * sizeof(arm64::CompiledBlock));
static_assert(sizeof(arm64::CacheSet) <
              arm64::cache_ways *
                  (sizeof(arm64::CompiledBlock) +
                   arm64::maximum_saved_guest_word_bytes));

[[nodiscard]] constexpr std::uint32_t
move_immediate(unsigned destination, unsigned immediate) noexcept
{
	return 0xd280'0000U | ((immediate & 0xffffU) << 5U) |
		(destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
branch(std::uint64_t source, std::uint64_t target) noexcept
{
	const auto displacement = (static_cast<std::int64_t>(target) -
		static_cast<std::int64_t>(source)) / 4;
	return 0x1400'0000U |
		(static_cast<std::uint32_t>(displacement) & 0x03ff'ffffU);
}

[[nodiscard]] constexpr std::uint32_t
branch_register(unsigned source) noexcept
{
	return 0xd61f'0000U | ((source & 31U) << 5U);
}

struct Run final {
	::uml::cpu::EngineEvent event{};
	std::size_t executed{};
};

[[nodiscard]] Run run_once(JitExecutor &executor, StaticGuestMemory &memory,
	unsigned long generation, arm64::CpuState &cpu)
{
	require(executor.begin_execution(generation, cpu.pc));
	std::size_t executed = 0;
	const auto event = executor.run_block(cpu, memory, generation, executed);
	executor.end_execution();
	return {event, executed};
}

void write_mapped32(unsigned long generation, std::uint64_t address,
	std::uint32_t value)
{
	void *destination = nullptr;
	require(darwin_uml_static_vm_translate(
		generation, address, DARWIN_UML_STATIC_WRITE, &destination) ==
		DARWIN_UML_STATIC_TRANSLATED);
	std::memcpy(destination, &value, sizeof(value));
}

struct PublicationAllocationFailure final {
	std::size_t successful_allocations{};
};

[[nodiscard]] std::shared_ptr<arm64::SharedTranslation>
shared_translation_for(std::span<const std::uint32_t> guest_words);

void fail_shared_publication_allocation(void *opaque) noexcept
{
	const auto *failure =
		static_cast<const PublicationAllocationFailure *>(opaque);
	uml::testing::fail_after_allocations(failure->successful_allocations);
}

void publication_allocation_failure_installs_private_code(
	unsigned long generation, std::uint64_t base,
	std::size_t successful_allocations,
	bool preseed_entry_vector = false)
{
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	JitBacking backing{2};
	backing.put32(0, move_immediate(0, 19));
	backing.put32(4, system_call);
	JitMapping mapping{generation, base, backing, 2};
	PublicationAllocationFailure failure{successful_allocations};
	const JitExecutorOptions options{};
	JitExecutor executor{
		options,
		JitExecutorDiagnosticsOptions{
			.shared_publication_context = &failure,
			.before_shared_publication =
				&fail_shared_publication_allocation,
		}};
	/* Sharing is gated on two live executors. The peer need not run. */
	[[maybe_unused]] JitExecutor publication_peer;
	StaticGuestMemory memory;
	arm64::CpuState cpu{};
	auto &shared_cache = arm64::SharedTranslationCache::instance();
	if (preseed_entry_vector) {
		/* Keep the same map node but a different exact translation identity.
		 * The production lookup and preflight therefore miss, try_emplace()
		 * finds the existing node without allocating, and allocation offset
		 * three deterministically reaches vector<Entry>::push_back after the
		 * candidate's object, validation-array, and host-word allocations.
		 */
		const std::array occupant_words{
			move_immediate(0, 18), system_call};
		const arm64::SharedTranslationCache::CodegenProfile profile{
			.capabilities = options.host_capabilities,
		};
		require(shared_cache.insert(
			profile, base, occupant_words,
			shared_translation_for(occupant_words)) != nullptr);
	}
	const auto before = shared_cache.statistics();

	cpu.pc = base;
	auto run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(run.executed == 2);
	require(cpu.registers[0] == 19);
	require(uml::testing::last_allocation_failure().observed);
	require(!uml::testing::cancel_pending_allocation_failure());
	auto diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.translations == 1);
	require(diagnostics.private_validation_word_installs == 1);
	require(diagnostics.shared_validation_word_installs == 0);
	const auto after_failure =
		shared_cache.statistics();
	require(after_failure.insertions == before.insertions);
	require(after_failure.rejected == before.rejected);
	require(after_failure.entries == before.entries);
	require(after_failure.payload_bytes == before.payload_bytes);
	require(after_failure.accounted_bytes == before.accounted_bytes);

	/* An unrelated code-sequence change forces exact saved-word validation.
	 * Reusing the block proves the private fallback owner survived the failed
	 * opportunistic publication.
	 */
	darwin_uml_static_vm_invalidate_code(base + page);
	cpu.pc = base;
	cpu.registers[0] = 0;
	run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(run.executed == 2);
	require(cpu.registers[0] == 19);
	diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.translations == 1);
	require(diagnostics.validation_attempts == 1);
	require(diagnostics.validation_hits == 1);
	require(diagnostics.private_validation_word_installs == 1);
	require(diagnostics.shared_validation_word_installs == 0);
}

void publication_allocation_failures_install_private_code()
{
	/* A two-instruction SVC block performs exactly three candidate allocations:
	 * the shared object/control block, validation array, and nonempty host-word
	 * vector. Offset zero fails construction; offset three first reaches a new
	 * unordered-map node, then reaches existing entry-vector growth when that
	 * same-PC node is pre-seeded.
	 */
	publication_allocation_failure_installs_private_code(
		/*generation=*/8118, /*base=*/0x158'0000,
		/*successful_allocations=*/0);
	publication_allocation_failure_installs_private_code(
		/*generation=*/8119, /*base=*/0x15c'0000,
		/*successful_allocations=*/3);
	publication_allocation_failure_installs_private_code(
		/*generation=*/8120, /*base=*/0x160'0000,
		/*successful_allocations=*/3,
		/*preseed_entry_vector=*/true);
}

void unrelated_page_change_reuses_exact_block()
{
	constexpr unsigned long generation = 8101;
	constexpr std::uint64_t base = 0x90'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	JitBacking backing{2};
	backing.put32(0, move_immediate(0, 1));
	backing.put32(4, system_call);
	JitMapping mapping{generation, base, backing, 2};
	JitExecutor executor;
	StaticGuestMemory memory;
	arm64::CpuState cpu{};

	cpu.pc = base;
	auto run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(run.executed == 2);
	require(cpu.registers[0] == 1);
	auto diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.translations == 1);
	require(diagnostics.private_validation_word_installs == 1);
	require(diagnostics.shared_validation_word_installs == 0);

	darwin_uml_static_vm_invalidate_code(base + page);
	cpu.pc = base;
	cpu.registers[0] = 0;
	run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(run.executed == 2);
	require(cpu.registers[0] == 1);
	diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.translations == 1);
	require(diagnostics.validation_attempts == 1);
	require(diagnostics.validation_hits == 1);
	require(diagnostics.validation_mismatches == 0);
	require(diagnostics.validation_faults == 0);
	require(diagnostics.private_validation_word_installs == 1);
	require(diagnostics.shared_validation_word_installs == 0);
}

void in_block_change_rejects_saved_words()
{
	constexpr unsigned long generation = 8102;
	constexpr std::uint64_t base = 0x91'0000;
	JitBacking backing{1};
	backing.put32(0, move_immediate(0, 1));
	backing.put32(4, system_call);
	JitMapping mapping{generation, base, backing, 1};
	JitExecutor executor;
	StaticGuestMemory memory;
	arm64::CpuState cpu{};

	cpu.pc = base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	write_mapped32(generation, base, move_immediate(0, 2));
	darwin_uml_static_vm_invalidate_code(base);

	cpu.pc = base;
	cpu.registers[0] = 0;
	const auto run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 2);
	const auto diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.translations == 2);
	require(diagnostics.validation_attempts == 1);
	require(diagnostics.validation_hits == 0);
	require(diagnostics.validation_mismatches == 1);
}

void executable_permission_change_rejects_saved_block()
{
	constexpr unsigned long generation = 8103;
	constexpr std::uint64_t base = 0x92'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	JitBacking backing{1};
	backing.put32(0, move_immediate(0, 3));
	backing.put32(4, system_call);
	JitMapping mapping{generation, base, backing, 1};
	JitExecutor executor;
	StaticGuestMemory memory;
	arm64::CpuState cpu{};

	cpu.pc = base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	require(darwin_uml_static_vm_protect(
		reinterpret_cast<void *>(base), page,
		PROT_READ | PROT_WRITE) == 0);

	cpu.pc = base;
	auto run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::memory_fault);
	auto diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.translations == 1);
	require(diagnostics.validation_attempts == 1);
	require(diagnostics.validation_faults == 1);
	require(diagnostics.validation_hits == 0);

	require(darwin_uml_static_vm_protect(
		reinterpret_cast<void *>(base), page,
		PROT_READ | PROT_WRITE | PROT_EXEC) == 0);
	cpu.pc = base;
	run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.translations == 2);
	require(diagnostics.validation_hits == 0);
}

void remapped_code_is_refetched_from_the_current_mapping()
{
	constexpr unsigned long generation = 8111;
	constexpr std::uint64_t base = 0x93'0000;
	JitBacking original{1};
	JitBacking replacement{1};
	original.put32(0, move_immediate(0, 3));
	original.put32(4, system_call);
	replacement.put32(0, move_immediate(0, 6));
	replacement.put32(4, system_call);
	JitMapping mapping{generation, base, original, 1};
	JitExecutor executor;
	StaticGuestMemory memory;
	arm64::CpuState cpu{};

	cpu.pc = base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 3);
	mapping.replace_page(base, replacement);

	cpu.pc = base;
	cpu.registers[0] = 0;
	const auto run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 6);
	const auto diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.translations == 2);
	require(diagnostics.validation_attempts == 1);
	require(diagnostics.validation_mismatches == 1);
	require(diagnostics.validation_hits == 0);
}

void cross_page_block_is_validated_completely()
{
	constexpr unsigned long generation = 8104;
	constexpr std::uint64_t base = 0x94'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	const auto start = base + page - sizeof(std::uint32_t);
	JitBacking backing{3};
	backing.put32(page - sizeof(std::uint32_t), move_immediate(0, 4));
	backing.put32(page, system_call);
	JitMapping mapping{generation, base, backing, 3};
	JitExecutor executor;
	StaticGuestMemory memory;
	arm64::CpuState cpu{};

	cpu.pc = start;
	auto run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(run.executed == 2);
	require(cpu.registers[0] == 4);

	darwin_uml_static_vm_invalidate_code(base + 2 * page);
	cpu.pc = start;
	cpu.registers[0] = 0;
	run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(run.executed == 2);
	require(cpu.registers[0] == 4);
	const auto diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.translations == 1);
	require(diagnostics.validation_attempts == 1);
	require(diagnostics.validation_hits == 1);

	/* The changed word is on the second executable page. */
	write_mapped32(generation, base + page, move_immediate(0, 5));
	write_mapped32(generation, base + page + 4U, system_call);
	darwin_uml_static_vm_invalidate_code(base + page);
	cpu.pc = start;
	cpu.registers[0] = 0;
	run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(run.executed == 3);
	require(cpu.registers[0] == 5);
	const auto changed = executor.cache_validation_diagnostics();
	require(changed.translations == 2);
	require(changed.validation_attempts == 2);
	require(changed.validation_hits == 1);
	require(changed.validation_mismatches == 1);
}

void sequence_change_deactivates_unvalidated_successor_link()
{
	constexpr unsigned long generation = 8105;
	constexpr std::uint64_t base = 0x98'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	JitBacking backing{2};
	backing.put32(0, 0x1400'0002U); // b base + 8
	backing.put32(8, 0x9100'0400U); // add x0, x0, #1
	backing.put32(12, system_call);
	JitMapping mapping{generation, base, backing, 2};
	JitExecutor executor;
	StaticGuestMemory memory;
	arm64::CpuState cpu{};

	cpu.pc = base;
	auto run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::continued);
	require(run.executed == 1);
	require(cpu.pc == base + 8);

	run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(run.executed == 2);
	require(cpu.registers[0] == 1);

	cpu.pc = base;
	cpu.registers[0] = 0;
	run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(run.executed == 3);
	require(cpu.registers[0] == 1);

	darwin_uml_static_vm_invalidate_code(base + page);
	cpu.pc = base;
	cpu.registers[0] = 0;
	run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::continued);
	require(run.executed == 1);
	require(cpu.pc == base + 8);
	require(cpu.registers[0] == 0);

	run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(run.executed == 2);
	require(cpu.registers[0] == 1);

	cpu.pc = base;
	cpu.registers[0] = 0;
	run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(run.executed == 3);
	require(cpu.registers[0] == 1);
	const auto diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.translations == 2);
	require(diagnostics.validation_hits == 2);
}

void process_switches_retain_only_their_own_validated_links()
{
	constexpr unsigned long first_generation = 8300;
	constexpr std::uint64_t base = 0xec'0000;
	constexpr auto target = base + 8;
	// Exceed the bounded context directory as well as exercising a warm return.
	constexpr std::size_t contexts = 66;
	std::array<std::unique_ptr<JitBacking>, contexts> backings;
	std::array<std::unique_ptr<JitMapping>, contexts> mappings;
	for (std::size_t index = 0; index < contexts; ++index) {
		backings[index] = std::make_unique<JitBacking>(1);
		backings[index]->put32(0, branch(base, target));
		backings[index]->put32(8, move_immediate(0, index + 1));
		backings[index]->put32(12, system_call);
		mappings[index] = std::make_unique<JitMapping>(
			first_generation + index, base, *backings[index], 1);
	}
	JitExecutor executor;
	StaticGuestMemory memory;
	arm64::CpuState cpu{};
	const auto run = [&](std::size_t index, bool linked, unsigned expected) {
		const auto generation = first_generation + index;
		require(darwin_uml_static_vm_activate(generation) == 0);
		cpu.pc = base;
		cpu.registers[0] = 0;
		auto result = run_once(executor, memory, generation, cpu);
		if (linked) {
			require(result.event == ::uml::cpu::EngineEvent::system_call);
			require(result.executed == 3);
		} else {
			require(result.event == ::uml::cpu::EngineEvent::continued);
			require(result.executed == 1);
			require(cpu.pc == target);
			result = run_once(executor, memory, generation, cpu);
			require(result.event == ::uml::cpu::EngineEvent::system_call);
			require(result.executed == 2);
		}
		require(cpu.registers[0] == expected);
	};
	run(0, false, 1);
	run(1, false, 2);
	run(0, true, 1);
	run(1, true, 2);

	// Publishing to an inactive context must revoke its old successor before
	// that context can be entered again. The other context remains intact.
	require(darwin_uml_static_vm_activate(first_generation) == 0);
	write_mapped32(first_generation, target, move_immediate(0, 99));
	darwin_uml_static_vm_invalidate_code(target);
	run(1, true, 2);
	run(0, false, 99);
	run(0, true, 99);
	for (std::size_t index = 2; index < contexts; ++index)
		run(index, false, index + 1);
	// Evicted context anchors must be inactive, even while their native code
	// remains resident in the larger block cache.
	run(0, false, 99);
	run(0, true, 99);
}

/* Link records are keyed by address space, so each short-lived process
 * leaves its records behind. Many more processes than the tables hold must
 * still get linked blocks: a full table recycles the cache instead of
 * publishing unlinked exits.
 */
void link_many_short_lived_address_spaces(
	JitExecutorDiagnosticsOptions diagnostics, unsigned long first_generation)
{
	constexpr std::uint64_t base = 0x158'0000;
	constexpr auto direct_target = base + 8;
	constexpr auto indirect_source = base + 0x100;
	constexpr auto indirect_target = base + 0x108;
	// Each process needs four direct records and one indirect site.
	constexpr std::size_t processes = 24;
	JitExecutor executor{JitExecutorOptions{}, diagnostics};
	StaticGuestMemory memory;
	for (std::size_t index = 0; index < processes; ++index) {
		const auto generation = first_generation + index;
		const auto value = static_cast<unsigned>(index + 1);
		JitBacking backing{1};
		backing.put32(0, branch(base, direct_target));
		backing.put32(direct_target - base, move_immediate(0, value));
		backing.put32(direct_target - base + 4, system_call);
		backing.put32(indirect_source - base, branch_register(5));
		backing.put32(indirect_target - base, move_immediate(1, value));
		backing.put32(indirect_target - base + 4, system_call);
		JitMapping mapping{generation, base, backing, 1};
		arm64::CpuState cpu{};
		/* The cold pass stops at the uncompiled target and runs it in the
		 * same execution, where a pending indirect site trains; the next
		 * pass from the source must run as one linked chain. */
		const auto cold_then_linked = [&](std::uint64_t source) {
			cpu.pc = source;
			require(executor.begin_execution(generation, cpu.pc));
			std::size_t executed = 0;
			require(executor.run_block(cpu, memory, generation,
					executed) ==
				::uml::cpu::EngineEvent::continued);
			require(executed == 1);
			require(executor.run_block(cpu, memory, generation,
					executed) ==
				::uml::cpu::EngineEvent::system_call);
			require(executed == 2);
			executor.end_execution();
			cpu.pc = source;
			cpu.registers[0] = 0;
			cpu.registers[1] = 0;
			const auto linked =
				run_once(executor, memory, generation, cpu);
			require(linked.event ==
				::uml::cpu::EngineEvent::system_call);
			require(linked.executed == 3);
		};

		cold_then_linked(base);
		require(cpu.registers[0] == value);
		cpu.registers[5] = indirect_target;
		cold_then_linked(indirect_source);
		require(cpu.registers[1] == value);
	}
	const auto cache = executor.cache_validation_diagnostics();
	require(cache.link_table_resets != 0);
	require(cache.arena_resets == 0);
	require(cache.target_link_refusals == 0);
	require(cache.direct_link_refusals == 0);
	require(executor.indirect_link_diagnostics().trains == processes);
}

void exited_address_spaces_do_not_exhaust_link_tables()
{
	// Eight direct records hold two processes; four sites hold four.
	link_many_short_lived_address_spaces(
		{.direct_link_capacity = 8}, 8400);
	link_many_short_lived_address_spaces(
		{.indirect_link_capacity = 4}, 8500);
}

void cache_replacement_keeps_validation_metadata_bounded()
{
	constexpr unsigned long generation = 8106;
	constexpr std::uint64_t base = 0xa0'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	/* Five offsets that land in one cache set, so they contend for four
	 * ways. The set count is chosen at runtime from the cache budget, so
	 * these are searched for rather than written down.
	 */
	const auto sets = arm64::jit_cache_geometry().cache_sets;
	std::array<std::size_t, 5> offsets{};
	{
		const auto target = arm64::cache_index(generation, base, sets);
		std::size_t found = 1;
		for (std::size_t offset = 4; found < offsets.size();
		     offset += 4) {
			require(offset < (std::size_t{1} << 30U));
			if (arm64::cache_index(generation, base + offset,
				    sets) == target)
				offsets[found++] = offset;
		}
	}
	const std::size_t pages = offsets.back() / page + 1U;

	JitBacking backing{pages};
	for (const auto offset : offsets)
		backing.put32(offset, system_call);
	JitMapping mapping{generation, base, backing, pages};
	JitExecutor executor;
	StaticGuestMemory memory;
	arm64::CpuState cpu{};
	for (const auto offset : offsets) {
		cpu.pc = base + offset;
		const auto run = run_once(executor, memory, generation, cpu);
		require(run.event == ::uml::cpu::EngineEvent::system_call);
		require(run.executed == 1);
	}
	auto diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.translations == offsets.size());
	require(diagnostics.cache_replacements == 1);

	darwin_uml_static_vm_invalidate_code(base + page - 4U);
	cpu.pc = base + offsets.back();
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.validation_attempts == 1);
	require(diagnostics.validation_hits == 1);

	const auto translations = diagnostics.translations;
	cpu.pc = base + offsets.front();
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.validation_attempts == 1);
	require(diagnostics.translations == translations + 1U);
}

void arena_reset_discards_saved_validation_words()
{
	constexpr unsigned long generation = 8107;
	constexpr std::uint64_t base = 0xa4'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	std::vector<std::uint32_t> instructions(page / sizeof(std::uint32_t),
		system_call);
	JitBacking backing{1};
	backing.put(0, std::as_bytes(std::span{instructions}));
	JitMapping mapping{generation, base, backing, 1};
	JitExecutor executor{
		JitExecutorOptions{},
		JitExecutorDiagnosticsOptions{
			.reuse_validated_stale_blocks = true,
			.code_arena_size = 16U * 1024U,
		}};
	StaticGuestMemory memory;
	arm64::CpuState cpu{};

	std::size_t compiled = 0;
	for (; compiled < instructions.size(); ++compiled) {
		cpu.pc = base + compiled * sizeof(std::uint32_t);
		require(run_once(executor, memory, generation, cpu).event ==
			::uml::cpu::EngineEvent::system_call);
		if (executor.cache_validation_diagnostics().arena_resets != 0)
			break;
	}
	require(compiled < instructions.size());
	auto diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.arena_resets == 1);
	const auto translations = diagnostics.translations;
	const auto attempts = diagnostics.validation_attempts;

	cpu.pc = base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.translations == translations + 1U);
	require(diagnostics.validation_attempts == attempts);
}

void arena_reset_recompiles_link_descriptors()
{
	constexpr unsigned long generation = 8112;
	constexpr std::uint64_t base = 0xb4'0000;
	constexpr auto target =
		base + DARWIN_UML_GUEST_PAGE_SIZE - sizeof(std::uint32_t);
	constexpr std::size_t sources = 512;
	JitBacking backing{1};
	for (std::size_t index = 0; index < sources; ++index) {
		const auto source = base + index * sizeof(std::uint32_t);
		backing.put32(index * sizeof(std::uint32_t),
			branch(source, target));
	}
	backing.put32(target - base, system_call);
	JitMapping mapping{generation, base, backing, 1};
	JitExecutor executor{
		JitExecutorOptions{},
		JitExecutorDiagnosticsOptions{.code_arena_size = 4096}};
	StaticGuestMemory memory;
	arm64::CpuState cpu{};

	cpu.pc = target;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	std::uint64_t reset_source = 0;
	for (std::size_t index = 0; index < sources; ++index) {
		cpu.pc = base + index * sizeof(std::uint32_t);
		const auto run = run_once(executor, memory, generation, cpu);
		if (executor.cache_validation_diagnostics().arena_resets != 0) {
			reset_source = base + index * sizeof(std::uint32_t);
			require(run.event == ::uml::cpu::EngineEvent::continued);
			require(cpu.pc == target);
			break;
		}
		require(run.event == ::uml::cpu::EngineEvent::system_call);
		require(run.executed == 2);
	}
	require(reset_source != 0);
	require(executor.cache_validation_diagnostics().arena_resets == 1);

	cpu.pc = target;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	cpu.pc = reset_source;
	const auto linked = run_once(executor, memory, generation, cpu);
	require(linked.event == ::uml::cpu::EngineEvent::system_call);
	require(linked.executed == 2);
}

void reordered_words_cannot_hide_behind_a_digest_collision()
{
	constexpr unsigned long generation = 8108;
	constexpr std::uint64_t base = 0xa8'0000;
	constexpr auto first = move_immediate(0, 1);
	constexpr std::uint32_t second = 0x9100'0800U; // add x0, x0, #2
	static_assert((first ^ second) == (second ^ first));
	static_assert(static_cast<std::uint32_t>(first + second) ==
		static_cast<std::uint32_t>(second + first));
	JitBacking backing{1};
	backing.put32(0, first);
	backing.put32(4, second);
	backing.put32(8, system_call);
	JitMapping mapping{generation, base, backing, 1};
	JitExecutor executor;
	StaticGuestMemory memory;
	arm64::CpuState cpu{};

	cpu.pc = base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 3);

	write_mapped32(generation, base, second);
	write_mapped32(generation, base + 4, first);
	darwin_uml_static_vm_invalidate_code(base);
	cpu.pc = base;
	cpu.registers[0] = 0;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == 1);
	const auto diagnostics = executor.cache_validation_diagnostics();
	require(diagnostics.translations == 2);
	require(diagnostics.validation_mismatches == 1);
	require(diagnostics.validation_hits == 0);
}

struct Timing final {
	std::chrono::nanoseconds elapsed{};
	JitCacheValidationDiagnostics diagnostics{};
};

[[nodiscard]] Timing repeated_unrelated_changes(
	bool reuse, unsigned long generation, std::uint64_t base)
{
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr std::size_t iterations = 128;
	JitBacking backing{2};
	backing.put32(0, move_immediate(0, 7));
	backing.put32(4, system_call);
	JitMapping mapping{generation, base, backing, 2};
	JitExecutor executor{
		JitExecutorOptions{},
		JitExecutorDiagnosticsOptions{
			.reuse_validated_stale_blocks = reuse,
		}};
	StaticGuestMemory memory;
	arm64::CpuState cpu{};
	cpu.pc = base;
	require(run_once(executor, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);

	const auto start = std::chrono::steady_clock::now();
	for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
		darwin_uml_static_vm_invalidate_code(base + page);
		cpu.pc = base;
		require(run_once(executor, memory, generation, cpu).event ==
			::uml::cpu::EngineEvent::system_call);
	}
	const auto end = std::chrono::steady_clock::now();
	const auto diagnostics = executor.cache_validation_diagnostics();
	if (reuse) {
		require(diagnostics.translations == 1);
		require(diagnostics.validation_hits == iterations);
	} else {
		require(diagnostics.translations == iterations + 1U);
		require(diagnostics.validation_attempts == 0);
	}
	return {
		.elapsed =
			std::chrono::duration_cast<std::chrono::nanoseconds>(end - start),
		.diagnostics = diagnostics,
	};
}

void stale_sequence_control_reports_avoided_translation_work()
{
	const auto reused = repeated_unrelated_changes(true, 8109, 0xac'0000);
	const auto control = repeated_unrelated_changes(false, 8110, 0xb0'0000);
	std::fprintf(stderr,
		"JIT exact cache validation: reused=%llu ns/%llu translations "
		"control=%llu ns/%llu translations\n",
		static_cast<unsigned long long>(reused.elapsed.count()),
		static_cast<unsigned long long>(reused.diagnostics.translations),
		static_cast<unsigned long long>(control.elapsed.count()),
		static_cast<unsigned long long>(control.diagnostics.translations));
}

/* The private cache is a bounded L1 in front of the much larger canonical
 * translation cache. Fill half of its configured entry population and require
 * it to remain useful on a second pass; the global cache tests separately pin
 * cross-executor canonical ownership and reuse.
 */
void private_cache_retains_its_bounded_working_set()
{
	constexpr unsigned long generation = 8111;
	constexpr std::uint64_t base = 0x100'0000;
	const auto geometry = arm64::jit_cache_geometry();
	require(geometry.total_bytes() <=
		arm64::default_jit_cache_budget_per_executor);
	/* Consecutive two-instruction blocks vary PC in eight-byte steps, so this
	 * pattern exercises one eighth of the low-bit-indexed sets before the
	 * higher-bit mix changes. Fill all four ways of that effective set range.
	 */
	const auto blocks = std::min<std::size_t>(
		geometry.cache_sets * arm64::cache_ways / 8U, 16384U);
	const auto pages =
		blocks * 8U / DARWIN_UML_GUEST_PAGE_SIZE;
	require(blocks != 0 &&
		blocks * 8U % DARWIN_UML_GUEST_PAGE_SIZE == 0);

	JitBacking backing{pages};
	for (std::size_t block = 0; block < blocks; ++block) {
		backing.put32(block * 8U,
			move_immediate(0, static_cast<unsigned>(block & 0xffffU)));
		backing.put32(block * 8U + 4U, system_call);
	}
	JitMapping mapping{generation, base, backing, pages};
	JitExecutor executor;
	StaticGuestMemory memory;
	arm64::CpuState cpu{};

	for (std::size_t block = 0; block < blocks; ++block) {
		cpu.pc = base + block * 8U;
		const auto run = run_once(executor, memory, generation, cpu);
		require(run.event == ::uml::cpu::EngineEvent::system_call);
	}
	const auto filled = executor.cache_validation_diagnostics().translations;
	require(filled == blocks);

	for (std::size_t block = 0; block < blocks; ++block) {
		cpu.pc = base + block * 8U;
		const auto run = run_once(executor, memory, generation, cpu);
		require(run.event == ::uml::cpu::EngineEvent::system_call);
	}
	const auto retranslated =
		executor.cache_validation_diagnostics().translations - filled;
	std::fprintf(stderr,
		"JIT private cache capacity: %zu blocks filled, %llu retranslated "
		"on the second pass\n",
		blocks, static_cast<unsigned long long>(retranslated));
	/* Set-associative collisions evict a small tail. The former capacity
	 * would have evicted essentially all of it, and an arena reset would
	 * too, so this bound covers both regressions.
	 */
	require(retranslated * 50U < blocks);
}

void saved_word_owners_remain_inside_representative_budgets()
{
	constexpr std::size_t gib = std::size_t{1024} * 1024U * 1024U;
	constexpr std::array guest_sizes{4U * gib, 8U * gib, 16U * gib};
	std::optional<arm64::JitCacheGeometry> expected_geometry;
	std::size_t preceding_shared = 0;
	for (const auto guest_bytes : guest_sizes) {
		const auto whole_process_budget = guest_bytes / 2U;
		const auto geometry =
			arm64::derive_jit_cache_geometry(whole_process_budget, 10);
		const auto shared = arm64::derive_shared_translation_cache_budget(
			whole_process_budget, 10);
		require(geometry.total_bytes() <=
			arm64::default_jit_cache_budget_per_executor);
		if (whole_process_budget >=
			10U * arm64::default_jit_cache_budget_per_executor +
			arm64::minimum_shared_translation_cache_bytes) {
			if (expected_geometry)
				require(geometry == *expected_geometry);
			else
				expected_geometry = geometry;
		}
		require(shared > preceding_shared);
		require(geometry.total_bytes() * 10U + shared <=
			whole_process_budget);
		preceding_shared = shared;
	}
}

[[nodiscard]] std::shared_ptr<arm64::SharedTranslation>
shared_translation_for(std::span<const std::uint32_t> guest_words)
{
	auto translation = std::make_shared<arm64::SharedTranslation>();
	auto saved =
		std::make_shared_for_overwrite<std::uint32_t[]>(guest_words.size());
	std::copy(guest_words.begin(), guest_words.end(), saved.get());
	translation->guest_words = std::move(saved);
	translation->guest_word_count = guest_words.size();
	translation->guest_instructions = guest_words.size();
	translation->words = {0xd503'201fU, 0xd65f'03c0U};
	translation->hot_words = translation->words.size();
	translation->internal_entry_word = 1;
	return translation;
}

void a_full_shared_translation_shard_requires_private_fallback()
{
	arm64::SharedTranslationCache cache{1};
	cache.add_executor();
	cache.add_executor();
	const arm64::SharedTranslationCache::CodegenProfile profile{};
	constexpr std::uint64_t pc = 0x138'0000;
	const std::array first_words{system_call};
	const std::array second_words{move_immediate(0, 1)};

	const auto first = cache.insert(profile, pc, first_words,
		shared_translation_for(first_words));
	require(first != nullptr);
	const auto duplicate = cache.insert(profile, pc, first_words,
		shared_translation_for(first_words));
	require(duplicate == first);
	auto statistics = cache.statistics();
	require(statistics.entries == 1);
	require(statistics.insertions == 1);
	require(statistics.rejected == 0);
	const auto refused = cache.insert(profile, pc, second_words,
		shared_translation_for(second_words));
	require(refused == nullptr);
	statistics = cache.statistics();
	require(statistics.entries == 1);
	require(statistics.insertions == 1);
	require(statistics.rejected == 1);
	cache.remove_executor();
	cache.remove_executor();
}

void shared_translation_payload_bytes_are_bounded()
{
	arm64::SharedTranslationCache cache{/*shard_capacity=*/8,
		/*maximum_accounted_bytes=*/1};
	cache.add_executor();
	cache.add_executor();
	const arm64::SharedTranslationCache::CodegenProfile profile{};
	constexpr std::uint64_t pc = 0x139'0000;
	const std::array words{system_call};
	require(cache.insert(profile, pc, words,
		shared_translation_for(words)) == nullptr);
	const auto statistics = cache.statistics();
	require(statistics.entries == 0);
	require(statistics.insertions == 0);
	require(statistics.rejected == 1);
	require(statistics.payload_bytes == 0);
	cache.remove_executor();
	cache.remove_executor();
}

/* A translation embeds pointers into the link tables of the executor that
 * compiled it, so reuse is only correct if every one of them is rewritten for
 * the reusing executor. The first executor is destroyed before the second
 * runs: a missed relocation therefore branches through freed memory, which
 * the sanitizer gates catch rather than silently taking another executor's
 * link. The source modifies x8/x9 and the reused target consumes them, so the
 * successful relocated edge also proves that guest register values survive
 * the link.
 */
void a_second_executor_reuses_the_first_translation_safely()
{
	constexpr unsigned long generation = 8112;
	constexpr std::uint64_t base = 0x140'0000;
	constexpr auto target = base + 0x40;
	JitBacking backing{1};
	/* Two blocks, the first ending in a direct branch, so the translation
	 * has a link pointer that has to be relocated.
	 */
	backing.put32(0, move_immediate(0, 7));
	backing.put32(4, 0x9100'0508U); // add x8, x8, #1
	backing.put32(8, 0x9100'0929U); // add x9, x9, #2
	backing.put32(12, branch(base + 12, target));
	backing.put32(target - base, move_immediate(1, 9));
	backing.put32(target - base + 4,
		0x8b09'0108U); // add x8, x8, x9
	backing.put32(target - base + 8,
		0x9100'1129U); // add x9, x9, #4
	backing.put32(target - base + 12, system_call);
	JitMapping mapping{generation, base, backing, 1};
	StaticGuestMemory memory;

	const auto before =
		arm64::SharedTranslationCache::instance().statistics();
	struct Completion final {
		arm64::CpuState cpu;
		unsigned invocations;
		std::size_t final_executed;
	};
	const auto run_to_system_call = [&](JitExecutor &executor) {
		arm64::CpuState cpu{};
		cpu.pc = base;
		cpu.registers[8] = 10;
		cpu.registers[9] = 20;
		for (unsigned step = 0; step < 8; ++step) {
			const auto run = run_once(executor, memory, generation, cpu);
			if (run.event == ::uml::cpu::EngineEvent::system_call)
				return Completion{cpu, step + 1U, run.executed};
			require(run.event == ::uml::cpu::EngineEvent::continued);
		}
		require(false);
		return Completion{};
	};
	/* The second executor exists while the first compiles, because sharing
	 * is only enabled for concurrent executors, but it does not run until
	 * the first has been destroyed.
	 */
	JitExecutor second;
	std::uint64_t first_private_installs{};
	std::uint64_t first_shared_installs{};
	{
		JitExecutor first;
		const auto cold = run_to_system_call(first);
		require(cold.invocations == 2);
		require(cold.cpu.registers[0] == 7);
		require(cold.cpu.registers[1] == 9);
		require(cold.cpu.registers[8] == 33);
		require(cold.cpu.registers[9] == 26);
		const auto linked = run_to_system_call(first);
		require(linked.invocations == 1 && linked.final_executed == 8);
		require(linked.cpu.registers[0] == 7);
		require(linked.cpu.registers[1] == 9);
		require(linked.cpu.registers[8] == 33);
		require(linked.cpu.registers[9] == 26);
		const auto diagnostics = first.cache_validation_diagnostics();
		first_private_installs =
			diagnostics.private_validation_word_installs;
		first_shared_installs =
			diagnostics.shared_validation_word_installs;
	}
	const auto published =
		arm64::SharedTranslationCache::instance().statistics();
	require(published.insertions > before.insertions);
	require(first_shared_installs > 0);
	require(first_private_installs == 0);

	const auto cold = run_to_system_call(second);
	require(cold.invocations == 2);
	require(cold.cpu.registers[0] == 7);
	require(cold.cpu.registers[1] == 9);
	require(cold.cpu.registers[8] == 33);
	require(cold.cpu.registers[9] == 26);
	const auto linked = run_to_system_call(second);
	require(linked.invocations == 1 && linked.final_executed == 8);
	require(linked.cpu.registers[0] == 7);
	require(linked.cpu.registers[1] == 9);
	require(linked.cpu.registers[8] == 33);
	require(linked.cpu.registers[9] == 26);
	const auto reused =
		arm64::SharedTranslationCache::instance().statistics();
	const auto second_diagnostics = second.cache_validation_diagnostics();
	require(second_diagnostics.target_link_refusals == 0);
	require(second_diagnostics.direct_link_table_capacity > 0);
	require(second_diagnostics.direct_link_table_high_water > 0);
	require(second_diagnostics.direct_link_table_high_water <=
		second_diagnostics.direct_link_table_capacity);
	require(reused.matches > published.matches);
	require(reused.reuses > published.reuses);
	require(second_diagnostics.shared_validation_word_installs > 0);
	require(second_diagnostics.private_validation_word_installs == 0);
	std::fprintf(stderr,
		"JIT shared translations: %llu published, %llu reused by the "
		"second executor\n",
		static_cast<unsigned long long>(published.insertions -
			before.insertions),
		static_cast<unsigned long long>(reused.reuses - published.reuses));
}

/* The direct-link case above pins one relocation kind. BR translations embed
 * an executor-owned PIC site as well. Train the publisher's site before its
 * destruction so an unrelocated reuse would read that freed site and branch
 * through the publisher's freed target record/code. The surviving executor
 * must instead observe a cold miss, train its own site, and then take its own
 * hot PIC path. Its source modifies x8/x9 and its target consumes both, so the
 * final run also proves that guest x8/x9 values survive that relocated
 * successful edge.
 */
void a_second_executor_reuses_an_indirect_translation_safely()
{
	constexpr unsigned long generation = 8113;
	constexpr std::uint64_t base = 0x144'0000;
	constexpr auto target = base + 0x100;
	JitBacking backing{1};
	backing.put32(0, 0x9100'0508U); // add x8, x8, #1
	backing.put32(4, 0x9100'0929U); // add x9, x9, #2
	backing.put32(8, branch_register(5));
	backing.put32(target - base,
		0x8b09'0108U); // add x8, x8, x9
	backing.put32(target - base + 4,
		0x9100'1129U); // add x9, x9, #4
	backing.put32(target - base + 8, move_immediate(0, 11));
	backing.put32(target - base + 12, system_call);
	JitMapping mapping{generation, base, backing, 1};
	StaticGuestMemory memory;

	const auto run_active_block = [&](JitExecutor &executor,
		arm64::CpuState &cpu) {
		std::size_t executed = 0;
		const auto event =
			executor.run_block(cpu, memory, generation, executed);
		return Run{event, executed};
	};
	const auto before =
		arm64::SharedTranslationCache::instance().statistics();

	/* Publication requires two concurrent executors. Keep the consumer alive
	 * but idle until the publisher's PIC has been trained and taken.
	 */
	JitExecutor second;
	{
		JitExecutor first;
		JitExecutor excess;
		require(first.budget_admitted());
		require(second.budget_admitted());
		require(!excess.budget_admitted());
		require(!excess.begin_execution(generation, base));
		arm64::CpuState excess_cpu{};
		StaticGuestMemory excess_memory;
		std::size_t excess_executed = 1;
		require(excess.run_block(excess_cpu, excess_memory, generation,
			excess_executed) == ::uml::cpu::EngineEvent::fatal);
		require(excess_executed == 0);
		arm64::CpuState cpu{};
		cpu.pc = base;
		cpu.registers[5] = target;
		cpu.registers[8] = 10;
		cpu.registers[9] = 20;
		require(first.begin_execution(generation, cpu.pc));

		auto run = run_active_block(first, cpu);
		require(run.event == ::uml::cpu::EngineEvent::continued);
		require(run.executed == 3);
		require(cpu.pc == target);
		require(cpu.registers[8] == 11 && cpu.registers[9] == 22);
		run = run_active_block(first, cpu);
		require(run.event == ::uml::cpu::EngineEvent::system_call);
		require(run.executed == 4);
		require(cpu.registers[0] == 11);
		require(cpu.registers[8] == 33 && cpu.registers[9] == 26);

		cpu.pc = base;
		cpu.registers[0] = 0;
		cpu.registers[8] = 10;
		cpu.registers[9] = 20;
		run = run_active_block(first, cpu);
		require(run.event == ::uml::cpu::EngineEvent::system_call);
		require(run.executed == 7);
		require(cpu.registers[0] == 11);
		require(cpu.registers[8] == 33 && cpu.registers[9] == 26);
		const auto links = first.indirect_link_diagnostics();
		require(links.misses == 1);
		require(links.trains == 1);
		const auto cache = first.cache_validation_diagnostics();
		require(cache.translations == 2);
		require(cache.shared_validation_word_installs == 2);
		require(cache.private_validation_word_installs == 0);
		first.end_execution();
	}

	const auto published =
		arm64::SharedTranslationCache::instance().statistics();
	require(published.insertions == before.insertions + 2);
	require(published.misses == before.misses + 2);

	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[5] = target;
	cpu.registers[8] = 10;
	cpu.registers[9] = 20;
	require(second.begin_execution(generation, cpu.pc));

	/* A correctly relocated, executor-private PIC starts cold even though the
	 * destroyed publisher had already trained the same guest target.
	 */
	auto run = run_active_block(second, cpu);
	require(run.event == ::uml::cpu::EngineEvent::continued);
	require(run.executed == 3);
	require(cpu.pc == target);
	require(cpu.registers[8] == 11 && cpu.registers[9] == 22);
	auto links = second.indirect_link_diagnostics();
	require(links.misses == 1);
	require(links.trains == 0);

	run = run_active_block(second, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(run.executed == 4);
	require(cpu.registers[0] == 11);
	require(cpu.registers[8] == 33 && cpu.registers[9] == 26);
	links = second.indirect_link_diagnostics();
	require(links.misses == 1);
	require(links.trains == 1);

	cpu.pc = base;
	cpu.registers[0] = 0;
	cpu.registers[8] = 10;
	cpu.registers[9] = 20;
	run = run_active_block(second, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(run.executed == 7);
	require(cpu.registers[0] == 11);
	require(cpu.registers[8] == 33 && cpu.registers[9] == 26);
	links = second.indirect_link_diagnostics();
	require(links.misses == 1);
	require(links.trains == 1);
	require(links.unsafe_target_refusals == 0);
	require(links.scope_rejections == 0);
	require(links.capacity_refusals == 0);
	require(links.table_capacity > 0);
	require(links.table_high_water > 0);
	require(links.table_high_water <= links.table_capacity);
	second.end_execution();

	const auto reused =
		arm64::SharedTranslationCache::instance().statistics();
	const auto cache = second.cache_validation_diagnostics();
	require(cache.translations == 2);
	require(reused.matches == published.matches + 2);
	require(reused.reuses == published.reuses + 2);
	require(reused.misses == published.misses);
	require(cache.shared_validation_word_installs == 2);
	require(cache.private_validation_word_installs == 0);
	std::fprintf(stderr,
		"JIT shared indirect translation: %llu published, %llu reused, "
		"%llu PIC miss, %llu PIC train\n",
		static_cast<unsigned long long>(published.insertions -
			before.insertions),
		static_cast<unsigned long long>(reused.reuses - published.reuses),
		static_cast<unsigned long long>(links.misses),
		static_cast<unsigned long long>(links.trains));
}

void arena_adjacent_direct_links_degrade_without_blocking_reuse()
{
	constexpr unsigned long generation = 8114;
	constexpr std::uint64_t base = 0x148'0000;
	constexpr auto target = base + 0x40;
	JitBacking backing{1};
	backing.put32(0, move_immediate(0, 13));
	backing.put32(4, 0x9100'0508U); // add x8, x8, #1
	backing.put32(8, 0x9100'0929U); // add x9, x9, #2
	backing.put32(12, branch(base + 12, target));
	backing.put32(target - base, 0x8b09'0108U); // add x8, x8, x9
	backing.put32(target - base + 4, 0x9100'1129U); // add x9, x9, #4
	backing.put32(target - base + 8, system_call);
	JitMapping mapping{generation, base, backing, 1};
	StaticGuestMemory memory;
	JitExecutor publisher;
	JitExecutor consumer{
		JitExecutorOptions{},
		JitExecutorDiagnosticsOptions{.direct_link_capacity = 0}};

	const auto run_to_system_call = [&](JitExecutor &executor) {
		arm64::CpuState cpu{};
		cpu.pc = base;
		cpu.registers[8] = 10;
		cpu.registers[9] = 20;
		for (unsigned step = 0; step < 4; ++step) {
			const auto run = run_once(executor, memory, generation, cpu);
			if (run.event == ::uml::cpu::EngineEvent::system_call)
				return cpu;
			require(run.event == ::uml::cpu::EngineEvent::continued);
			require(cpu.pc == target);
			require(cpu.registers[8] == 11 &&
				cpu.registers[9] == 22);
		}
		require(false);
		return cpu;
	};

	const auto published_cpu = run_to_system_call(publisher);
	require(published_cpu.registers[0] == 13 &&
		published_cpu.registers[8] == 33 &&
		published_cpu.registers[9] == 26);
	const auto published =
		arm64::SharedTranslationCache::instance().statistics();
	const auto consumer_cpu = run_to_system_call(consumer);
	require(consumer_cpu.registers[0] == 13 &&
		consumer_cpu.registers[8] == 33 &&
		consumer_cpu.registers[9] == 26);
	const auto after =
		arm64::SharedTranslationCache::instance().statistics();
	require(after.matches > published.matches);
	require(after.materialization_direct_link_failures ==
		published.materialization_direct_link_failures);
	require(after.materialization_indirect_link_failures ==
		published.materialization_indirect_link_failures);
	require(after.degraded_rejected == published.degraded_rejected);
	require(after.reuses == published.reuses + 2U);
	const auto diagnostics = consumer.cache_validation_diagnostics();
	require(diagnostics.direct_link_table_capacity == 0);
	require(diagnostics.direct_link_table_high_water == 0);
	require(diagnostics.target_link_refusals > 0);
	require(diagnostics.direct_link_refusals > 0);
	/* A refused reverse-index registration leaves the adjacent descriptor
	 * cold, so its generated exit takes the semantic fallback. Code shape
	 * and relocation metadata remain valid and both canonical blocks can
	 * still be materialized without recompilation.
	 */
	require(diagnostics.private_validation_word_installs == 0);
	require(diagnostics.shared_validation_word_installs > 0);
}

void an_indirect_link_capacity_match_is_not_counted_as_reuse()
{
	constexpr unsigned long generation = 8115;
	constexpr std::uint64_t base = 0x14c'0000;
	constexpr auto target = base + 0x100;
	JitBacking backing{1};
	backing.put32(0, 0x9100'0508U); // add x8, x8, #1
	backing.put32(4, 0x9100'0929U); // add x9, x9, #2
	backing.put32(8, branch_register(5));
	backing.put32(target - base, 0x8b09'0108U); // add x8, x8, x9
	backing.put32(target - base + 4, 0x9100'1129U); // add x9, x9, #4
	backing.put32(target - base + 8, system_call);
	JitMapping mapping{generation, base, backing, 1};
	StaticGuestMemory memory;
	JitExecutor publisher;
	JitExecutor consumer{
		JitExecutorOptions{},
		JitExecutorDiagnosticsOptions{.indirect_link_capacity = 0}};

	const auto run_to_system_call = [&](JitExecutor &executor) {
		arm64::CpuState cpu{};
		cpu.pc = base;
		cpu.registers[5] = target;
		cpu.registers[8] = 10;
		cpu.registers[9] = 20;
		for (unsigned step = 0; step < 4; ++step) {
			const auto run = run_once(executor, memory, generation, cpu);
			if (run.event == ::uml::cpu::EngineEvent::system_call)
				return cpu;
			require(run.event == ::uml::cpu::EngineEvent::continued);
			require(cpu.pc == target);
			require(cpu.registers[8] == 11 &&
				cpu.registers[9] == 22);
		}
		require(false);
		return cpu;
	};

	const auto published_cpu = run_to_system_call(publisher);
	require(published_cpu.registers[8] == 33 &&
		published_cpu.registers[9] == 26);
	const auto published =
		arm64::SharedTranslationCache::instance().statistics();
	const auto consumer_cpu = run_to_system_call(consumer);
	require(consumer_cpu.registers[8] == 33 &&
		consumer_cpu.registers[9] == 26);
	const auto after =
		arm64::SharedTranslationCache::instance().statistics();
	require(after.matches > published.matches);
	require(after.materialization_indirect_link_failures ==
		published.materialization_indirect_link_failures + 1U);
	require(after.materialization_direct_link_failures ==
		published.materialization_direct_link_failures);
	require(after.reuses < after.matches);
	const auto links = consumer.indirect_link_diagnostics();
	require(links.table_capacity == 0);
	require(links.table_high_water == 0);
	require(links.capacity_refusals > 0);
	const auto diagnostics = consumer.cache_validation_diagnostics();
	require(diagnostics.indirect_link_refusals > 0);
}

void an_arena_capacity_match_is_not_counted_as_reuse()
{
	constexpr unsigned long generation = 8116;
	constexpr std::uint64_t base = 0x150'0000;
	JitBacking backing{1};
	backing.put32(0, system_call);
	JitMapping mapping{generation, base, backing, 1};
	StaticGuestMemory memory;
	JitExecutor publisher;
	JitExecutor consumer{
		JitExecutorOptions{},
		JitExecutorDiagnosticsOptions{.code_arena_size = 1}};

	arm64::CpuState cpu{};
	cpu.pc = base;
	require(run_once(publisher, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	const auto published =
		arm64::SharedTranslationCache::instance().statistics();
	cpu.pc = base;
	require(run_once(consumer, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	const auto after =
		arm64::SharedTranslationCache::instance().statistics();
	require(after.matches == published.matches + 1U);
	require(after.reuses == published.reuses);
	require(after.materialization_arena_failures ==
		published.materialization_arena_failures + 1U);
	const auto diagnostics = consumer.cache_validation_diagnostics();
	require(diagnostics.arena_resets == 1);
	require(diagnostics.translations == 0);
}

void defensive_install_rejects_mutated_canonical_metadata()
{
	constexpr unsigned long generation = 8117;
	constexpr std::uint64_t base = 0x154'0000;
	const std::array guest_words{system_call};
	JitBacking backing{1};
	backing.put32(0, guest_words[0]);
	JitMapping mapping{generation, base, backing, 1};
	StaticGuestMemory memory;
	JitExecutor consumer;
	auto candidate = shared_translation_for(guest_words);
	{
		JitExecutor publication_gate;
		const JitExecutorOptions options{};
		const arm64::SharedTranslationCache::CodegenProfile profile{
			.capabilities = options.host_capabilities,
		};
		require(arm64::SharedTranslationCache::instance().insert(
			profile, base, guest_words, candidate) == candidate);
		/* The production publisher relinquishes its mutable owner. This
		 * mutation simulates corruption through a direct API caller and pins
		 * the executor's independent install-time validation boundary.
		 */
		candidate->hot_words = 0;
	}
	const auto before =
		arm64::SharedTranslationCache::instance().statistics();
	arm64::CpuState cpu{};
	cpu.pc = base;
	require(run_once(consumer, memory, generation, cpu).event ==
		::uml::cpu::EngineEvent::system_call);
	const auto after =
		arm64::SharedTranslationCache::instance().statistics();
	require(after.matches == before.matches + 1U);
	require(after.reuses == before.reuses);
	require(after.materialization_invalid_metadata_failures ==
		before.materialization_invalid_metadata_failures + 1U);
	const auto diagnostics = consumer.cache_validation_diagnostics();
	/* Install-time validation rejected the corrupted code metadata. The local
	 * recompilation may still retain the exact canonical owner's independent
	 * immutable guest-word storage, because publication preflight resolves that
	 * owner before applying the live-executor admission gate.
	 */
	require(diagnostics.private_validation_word_installs == 0);
	require(diagnostics.shared_validation_word_installs == 1);
}

void native_translation_preserves_fault_prefixes()
{
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr unsigned long generation = 19'701U;
	constexpr std::uint64_t base = 0x410'0000U;
	JitBacking backing{2};
	backing.put32(0, move_immediate(0, 19));
	backing.put32(4, system_call);
	backing.put32(64, move_immediate(0, 23));
	backing.put32(68, 0xf900'0022U); // str x2, [x1]
	backing.put32(72, system_call);
	backing.put32(128, move_immediate(0, 29));
	backing.put32(132, 0U); // UDF: the faulting instruction does not retire.
	backing.put32(page - 4U, move_immediate(0, 31));
	JitMapping mapping{generation, base, backing, 1};
	JitExecutor executor;
	StaticGuestMemory memory;
	arm64::CpuState cpu{};
	cpu.pc = base;
	auto run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(run.executed == 2 && cpu.registers[0] == 19);
	require(executor.cache_validation_diagnostics().translations == 1);
	const auto translated = executor.cache_validation_diagnostics();
	cpu.pc = base;
	run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(executor.cache_validation_diagnostics().translations ==
		translated.translations);

	cpu.pc = base + 64U;
	cpu.registers[1] = base + page;
	cpu.registers[2] = 0x1234U;
	run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::memory_fault);
	require(run.executed == 1 && cpu.pc == base + 68U);
	require(cpu.registers[0] == 23);
	require(executor.fault_address() == base + page);
	require(executor.fault_access() == uml::memory::Access::write);
	require(backing.get64(page) == 0);

	cpu.pc = base + 128U;
	run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::illegal_instruction);
	require(run.executed == 1 && cpu.pc == base + 132U);
	require(cpu.registers[0] == 29);

	// A speculative fetch of the next page must not suppress its valid prefix.
	cpu.pc = base + page - 4U;
	run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::continued);
	require(run.executed == 1 && cpu.pc == base + page);
	require(cpu.registers[0] == 31);
	run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::memory_fault);
	require(run.executed == 0);
	require(executor.fault_access() == uml::memory::Access::execute);
}

void peer_translation_reuse_requires_current_code()
{
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr unsigned long generation = 19'703U;
	constexpr std::uint64_t base = 0x430'0000U;
	JitBacking backing{2};
	backing.put32(0, move_immediate(0, 47));
	backing.put32(4, 0xf900'0022U); // str x2, [x1]
	backing.put32(8, system_call);
	JitMapping mapping{generation, base, backing, 1};
	JitExecutor producer;
	StaticGuestMemory memory;
	const auto run_fault = [&](JitExecutor &executor, unsigned expected) {
		arm64::CpuState cpu{};
		cpu.pc = base;
		cpu.registers[1] = base + page;
		const auto run = run_once(executor, memory, generation, cpu);
		require(run.event == ::uml::cpu::EngineEvent::memory_fault);
		require(run.executed == 1 && cpu.pc == base + 4U);
		require(cpu.registers[0] == expected);
		require(executor.fault_address() == base + page);
		require(executor.fault_access() == uml::memory::Access::write);
	};
	{
		JitExecutor consumer;
		run_fault(producer, 47);
		run_fault(consumer, 47);
		const auto diagnostics = consumer.cache_validation_diagnostics();
		require(diagnostics.translations == 1);
		require(diagnostics.shared_validation_word_installs == 1);
	}

	// A peer's translation never substitutes for exact current bytes.
	write_mapped32(generation, base, move_immediate(0, 53));
	JitExecutor changed_consumer;
	run_fault(changed_consumer, 53);
	require(changed_consumer.cache_validation_diagnostics().translations == 1);
}

/* The virtual CPU does not implement FEAT_PAuth: every non-hint PAC, AUT,
 * XPAC, PACGA, LDRA, and authenticated-branch encoding is UNDEFINED, and the
 * hint-space forms retire as NOPs. */
void pointer_authentication_instructions_are_undefined()
{
	constexpr unsigned long generation = 19'705U;
	constexpr std::uint64_t base = 0x450'0000U;
	constexpr std::array unavailable{
		0xdac1'0020U, // pacia x0, x1
		0xdac1'23e0U, // paciza x0
		0xdac1'43e0U, // xpaci x0
		0x9ac2'3020U, // pacga x0, x1, x2
		0xf820'0420U, // ldraa x0, [x1]
		0xf8a0'0420U, // ldrab x0, [x1]
		0xd71f'0801U, // braa x0, x1
		0xd71f'0c01U, // brab x0, x1
		0xd73f'0801U, // blraa x0, x1
		0xd61f'081fU, // braaz x0
		0xd65f'0bffU, // retaa
		0xd65f'0fffU, // retab
	};
	JitBacking backing{1};
	for (std::size_t index = 0; index < unavailable.size(); ++index) {
		backing.put32(index * 16U, move_immediate(9, 61));
		backing.put32(index * 16U + 4U, unavailable[index]);
		backing.put32(index * 16U + 8U, system_call);
	}
	constexpr std::array hints{
		7U, 8U, 10U, 12U, 14U, 24U, 25U, 26U, 27U, 28U, 29U, 30U, 31U,
		39U,
	};
	constexpr auto hint_offset = unavailable.size() * 16U;
	for (std::size_t index = 0; index < hints.size(); ++index)
		backing.put32(hint_offset + index * 4U,
			0xd503'201fU | hints[index] << 5);
	backing.put32(hint_offset + hints.size() * 4U, system_call);
	JitMapping mapping{generation, base, backing, 1};
	JitExecutor executor;
	StaticGuestMemory memory;
	arm64::CpuState cpu{};
	for (std::size_t index = 0; index < unavailable.size(); ++index) {
		cpu.pc = base + index * 16U;
		cpu.registers[0] = base;
		cpu.registers[1] = base;
		cpu.registers[9] = 0;
		const auto run = run_once(executor, memory, generation, cpu);
		require(run.event == ::uml::cpu::EngineEvent::illegal_instruction);
		require(run.executed == 1 && cpu.pc == base + index * 16U + 4U);
		require(cpu.registers[0] == base && cpu.registers[9] == 61);
	}
	/* Every hint-space form is a NOP: no signing, authentication, or
	 * stripping of X30, X17, or SP-relative state. */
	cpu.pc = base + hint_offset;
	cpu.registers[16] = 0x0012'3456'789a'bcdeU;
	cpu.registers[17] = base + 0x40U;
	cpu.registers[30] = 0x0000'7fff'1234'5678U;
	auto expected = cpu;
	const auto run = run_once(executor, memory, generation, cpu);
	require(run.event == ::uml::cpu::EngineEvent::system_call);
	require(run.executed == hints.size() + 1U);
	require(cpu.registers == expected.registers && cpu.sp == expected.sp);
}

void exclusive_and_system_blocks_are_translated()
{
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr unsigned long generation = 19'702U;
	constexpr std::uint64_t base = 0x420'0000U;
	JitBacking backing{2};
	backing.put32(0, 0xc85f'7c23U); // ldxr x3, [x1]
	backing.put32(4, system_call);
	backing.put32(64, 0xd503'3fdfU); // isb
	backing.put32(68, system_call);
	backing.put64(page, 41);
	JitMapping mapping{generation, base, backing, 2};
	JitExecutor executor;
	StaticGuestMemory memory;
	arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[1] = base + page;
	auto run = run_once(executor, memory, generation, cpu);
	require(run.executed == 1 && cpu.registers[3] == 41);
	require(executor.cache_validation_diagnostics().translations != 0);
	cpu.pc = base + 64U;
	run = run_once(executor, memory, generation, cpu);
	require(run.executed >= 1);
}

/* The AArch32 entry stores what it translates the way the A64 one does: each
 * block is a translation, one with a direct exit takes a link record, and the
 * validation words of a block the shared tier holds are the shared ones. The
 * first executor publishes both blocks; the second reuses them, relocated
 * for its own link table. */
void aarch32_blocks_are_counted_like_aarch64_ones()
{
	constexpr unsigned long generation = 19'706U;
	constexpr std::uint32_t base = 0x460'0000U;
	constexpr std::uint32_t target = base + 0x40U;
	JitBacking backing{1};
	backing.put32(0, 0xe280'0001U); // add r0, r0, #1
	/* b target: the offset counts words from the PC, which reads eight
	 * ahead of the branch. */
	backing.put32(4, 0xea00'0000U |
		((target - (base + 4U + 8U)) / 4U & 0x00ff'ffffU));
	backing.put32(0x40, 0xe280'000aU); // add r0, r0, #10
	backing.put32(0x44, 0xef00'0000U); // svc #0
	JitMapping mapping{generation, base, backing, 1};
	StaticGuestMemory memory;
	const auto run_to_svc = [&](JitExecutor &executor) {
		arm64::CpuState cpu{};
		cpu.a32 = 1;
		cpu.pc = arm64::aarch32::a32_key(base);
		for (unsigned step = 0; step < 4; ++step) {
			require(executor.begin_execution(generation, cpu.pc));
			std::size_t executed = 0;
			const auto event = executor.run_a32_block(
				cpu, memory, generation, executed);
			executor.end_execution();
			if (event == ::uml::cpu::EngineEvent::system_call) {
				require(cpu.registers[0] == 11);
				return;
			}
			require(event == ::uml::cpu::EngineEvent::continued);
		}
		require(false);
	};
	/* Sharing is only enabled for concurrent executors; the second does not
	 * run until the first is gone, so a missed relocation reads freed
	 * memory. */
	JitExecutor second;
	{
		JitExecutor first;
		run_to_svc(first);
		const auto diagnostics = first.cache_validation_diagnostics();
		require(diagnostics.translations == 2);
		require(diagnostics.target_link_refusals == 0);
		require(diagnostics.direct_link_table_high_water > 0);
		require(diagnostics.shared_validation_word_installs == 2);
		require(diagnostics.private_validation_word_installs == 0);
	}
	const auto published =
		arm64::SharedTranslationCache::instance().statistics();
	run_to_svc(second);
	const auto reused = arm64::SharedTranslationCache::instance().statistics();
	require(reused.reuses >= published.reuses + 2);
	const auto diagnostics = second.cache_validation_diagnostics();
	require(diagnostics.translations == 2);
	require(diagnostics.target_link_refusals == 0);
	require(diagnostics.direct_link_table_high_water > 0);
	require(diagnostics.shared_validation_word_installs == 2);
	require(diagnostics.private_validation_word_installs == 0);
}

} // namespace

int main()
{
	const uml::testing::RuntimeTestRoute runtime_route;
	constexpr std::size_t gib = std::size_t{1024} * 1024U * 1024U;
	require(arm64::set_jit_cache_budget(1U * gib, 2));
	native_translation_preserves_fault_prefixes();
	exclusive_and_system_blocks_are_translated();
	pointer_authentication_instructions_are_undefined();
	aarch32_blocks_are_counted_like_aarch64_ones();
	peer_translation_reuse_requires_current_code();
	publication_allocation_failures_install_private_code();
	defensive_install_rejects_mutated_canonical_metadata();
	an_arena_capacity_match_is_not_counted_as_reuse();
	an_indirect_link_capacity_match_is_not_counted_as_reuse();
	arena_adjacent_direct_links_degrade_without_blocking_reuse();
	a_second_executor_reuses_the_first_translation_safely();
	a_second_executor_reuses_an_indirect_translation_safely();
	shared_translation_payload_bytes_are_bounded();
	a_full_shared_translation_shard_requires_private_fallback();
	saved_word_owners_remain_inside_representative_budgets();
	private_cache_retains_its_bounded_working_set();
	unrelated_page_change_reuses_exact_block();
	in_block_change_rejects_saved_words();
	executable_permission_change_rejects_saved_block();
	remapped_code_is_refetched_from_the_current_mapping();
	cross_page_block_is_validated_completely();
	sequence_change_deactivates_unvalidated_successor_link();
	process_switches_retain_only_their_own_validated_links();
	exited_address_spaces_do_not_exhaust_link_tables();
	cache_replacement_keeps_validation_metadata_bounded();
	arena_reset_discards_saved_validation_words();
	arena_reset_recompiles_link_descriptors();
	reordered_words_cannot_hide_behind_a_digest_collision();
	stale_sequence_control_reports_avoided_translation_work();
	return 0;
}
