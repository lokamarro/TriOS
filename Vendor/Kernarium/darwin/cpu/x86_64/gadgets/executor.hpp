// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "program.hpp"
#include "uml_darwin_static_mmu.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace uml::cpu::x86_64 {
struct NativeTlbEntry;
class CodePageRegistry;
}

namespace uml::cpu::x86_64::gadgets {

struct ProgramSelection final {
	BuildStatus status{BuildStatus::illegal_instruction};
	const Program *program{};
	std::uint64_t fault_address{};
	bool cache_hit{};
};

struct ProgramCacheDiagnostics final {
	std::uint64_t translations{};
	std::uint64_t successor_hits{};
	std::uint64_t validation_attempts{};
	std::uint64_t validation_stamp_hits{};
	std::uint64_t validation_hits{};
	std::uint64_t validation_mismatches{};
	std::uint64_t validation_faults{};
	std::uint64_t validation_sequence_races{};
	/* Sweeps of an address space's link records and indirect sites, and
	 * the records and sites they released. */
	std::uint64_t link_sweeps{};
	std::uint64_t released_links{};
	std::uint64_t released_sites{};
};

struct ProgramCacheSequences final {
	std::uint64_t mapping_changes{};
	std::uint64_t code_changes{};
	bool interrupted{};
};
using ProgramCacheSequenceReader = ProgramCacheSequences (*)(
	void *context) noexcept;

/* The address space writes these ranges into the cache's buffers itself, so
 * they are its own bridge types. */
using ProgramCodeChange = darwin_uml_static_code_change;

/* The guest ranges whose mapping changed after `sequence`, or a negative
 * result when the caller has to retire every software-TLB entry. */
using ProgramMappingChange = darwin_uml_static_mapping_change;
using MappingChangeReader = int (*)(void *context, std::uint64_t generation,
	std::uint64_t sequence, ProgramMappingChange *changes,
	std::size_t capacity, std::uint64_t *current_sequence) noexcept;

/* The guest ranges whose code changed after `sequence`, or a negative result
 * when the caller has to retire every cached link. */
using ProgramCacheCodeChangeReader = int (*)(void *context,
	std::uint64_t sequence, ProgramCodeChange *changes,
	std::size_t capacity, std::uint64_t *current_sequence) noexcept;

/* Whether the MMU still has a generation: 1 while it does, 0 once it was
 * destroyed (a generation is never reused), negative when it cannot say. */
using GenerationLiveness = int (*)(unsigned long generation) noexcept;

class Executor final {
public:
	Executor();
	~Executor();
	Executor(Executor &&) noexcept;
	Executor &operator=(Executor &&) noexcept;

	Executor(const Executor &) = delete;
	Executor &operator=(const Executor &) = delete;

	void prepare(::uml::memory::GuestAddressSpace &memory,
		std::uint64_t generation = 0,
		std::uint64_t mapping_changes = 0,
		std::uint64_t code_changes = 0,
		void *execution_context = nullptr,
		ProgramCacheSequenceReader sequence_reader = nullptr,
		ProgramCacheCodeChangeReader code_change_reader = nullptr,
		MappingChangeReader mapping_change_reader = nullptr) noexcept;
	[[nodiscard]] NativeTlbEntry *tlb_entries() noexcept;
	/* The words a checked instruction's stream is built in, which a run
	 * keeps for the next. */
	[[nodiscard]] std::vector<std::uintptr_t> &checked_stream() noexcept;
	/* True when this executor holds a translation whose source covers the
	 * page. A store to a page without one cannot invalidate any cached block.
	 */
	[[nodiscard]] bool page_holds_cached_code(std::uint64_t address) noexcept;
	/* The pages holding cached code in any executor of the prepared address
	 * space, or null before the first prepare. */
	[[nodiscard]] CodePageRegistry *code_page_registry() const noexcept;
	/* Bracket every native run so a translating executor can wait for the
	 * stores this one has already translated. */
	void enter_engine() noexcept;
	void leave_engine() noexcept;
	/* Bracket every run of translated code and the handling of its exit,
	 * which may follow a link into a body that only the address space
	 * holds, so that executors sharing the address space free no body this
	 * one may be inside of (DARWIN_UML_GADGET_SHARED_BODIES). An executor
	 * that never brackets keeps the bodies of its address space for as long
	 * as it lives. */
	void begin_run() noexcept;
	void end_run() noexcept;
	[[nodiscard]] void *execution_context() const noexcept;
	[[nodiscard]] std::uint64_t prepared_code_changes() const noexcept;
	[[nodiscard]] std::uint64_t current_code_changes() const noexcept;
	void invalidate_cached_code(
		std::uint64_t address, std::size_t size) noexcept;
	[[nodiscard]] bool accept_code_changes(
		std::uint64_t code_changes) noexcept;
	/* Build for a guest that has enabled the exceptions in `checked`
	 * (`checked_exceptions`), so that no linked block runs an instruction
	 * that needs a check. The mask holds for everything the cache keeps: a
	 * caller that needs another sets it on a fresh executor. */
	void check_exceptions(unsigned checked) noexcept;
	[[nodiscard]] unsigned checked_exceptions() const noexcept;
	[[nodiscard]] ProgramSelection select_program(
		::uml::memory::GuestAddressSpace &memory, std::uint64_t address,
		ExecutionMode mode,
		std::size_t maximum_instructions = 64) noexcept;
	/* The cached program at `address`, the block a run is in, whose
	 * stream holds `word`, or null. */
	[[nodiscard]] const Program *program_holding(std::uint64_t address,
		ExecutionMode mode, const std::uintptr_t *word) const noexcept;
	void train_indirect(IndirectLink *site, std::uint64_t target,
		ExecutionMode mode) noexcept;
	[[nodiscard]] const TargetLinkEntry *target_links(
		ExecutionMode mode) noexcept;
	[[nodiscard]] const ProgramCacheDiagnostics &cache_diagnostics()
		const noexcept;
	[[nodiscard]] std::size_t resident_generations() const noexcept;
	/* Before a native run: let go of the address spaces whose generation
	 * `live` reports destroyed, if `retirements`, the MMU's count of
	 * destroyed generations, moved since the last call. A destroyed
	 * generation never runs again, so nothing it holds can be looked up
	 * again. Its number keeps its place in the order the least recently used
	 * address space is retired in, so the cache keeps the live address
	 * spaces it would have kept. Out of memory part way, what was not let go
	 * of stays held and the next call asks again. The caller runs no
	 * translated code of this executor until after its next prepare(). */
	void leave_gone_generations(unsigned long long retirements,
		GenerationLiveness live) noexcept;

private:
	struct Storage;
	std::unique_ptr<Storage> storage_;
};

} // namespace uml::cpu::x86_64::gadgets
