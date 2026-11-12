// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/fp_environment.hpp"
#include "cpu/engine.hpp"
#include "memory/tlb.hpp"
#include "memory/static_guest_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#ifndef UML_JIT_RUNTIME_DIAGNOSTICS
#define UML_JIT_RUNTIME_DIAGNOSTICS 0
#endif

namespace uml::cpu::arm64 {

class JitExecutorPool;

using JitExecutorOptions = ::uml::cpu::EngineConfiguration;

#if UML_JIT_RUNTIME_DIAGNOSTICS
struct JitExecutorDiagnosticsOptions final {
	using BeforeSharedPublication = void (*)(void *) noexcept;

	bool reuse_validated_stale_blocks{true};
	/* Zero selects the ordinary production arena size. */
	std::size_t code_arena_size{};
	/* Empty selects the production geometry. A supplied value, including
	 * zero, caps the corresponding stable table for focused exhaustion tests.
	 */
	std::optional<std::size_t> direct_link_capacity;
	std::optional<std::size_t> indirect_link_capacity;
	/* Test-support-only boundary after the compiler and capacity preflight but
	 * before the first opportunistic publication allocation. Production
	 * executor objects preprocess this complete diagnostics structure out. */
	void *shared_publication_context{};
	BeforeSharedPublication before_shared_publication{};
};

struct JitCacheValidationDiagnostics final {
	std::uint64_t translations{};
	std::uint64_t validation_attempts{};
	std::uint64_t validation_hits{};
	std::uint64_t validation_mismatches{};
	std::uint64_t validation_faults{};
	std::uint64_t validation_sequence_races{};
	std::uint64_t cache_replacements{};
	std::uint64_t arena_resets{};
	std::uint64_t link_table_resets{};
	std::uint64_t private_validation_word_installs{};
	std::uint64_t shared_validation_word_installs{};
	std::uint64_t direct_link_refusals{};
	std::uint64_t indirect_link_refusals{};
	std::uint64_t target_link_refusals{};
	std::uint64_t direct_link_table_high_water{};
	std::uint64_t direct_link_table_capacity{};
};

struct JitIndirectLinkDiagnostics final {
	std::uint64_t misses{};
	std::uint64_t trains{};
	std::uint64_t unsafe_target_refusals{};
	std::uint64_t scope_rejections{};
	std::uint64_t deactivated_sites{};
	std::uint64_t capacity_refusals{};
	std::uint64_t table_high_water{};
	std::uint64_t table_capacity{};
};
#endif

class JitExecutor final {
public:
	JitExecutor();
	explicit JitExecutor(JitExecutorOptions options);
#if UML_JIT_RUNTIME_DIAGNOSTICS
	JitExecutor(JitExecutorOptions options,
		    JitExecutorDiagnosticsOptions diagnostics);
#endif
	~JitExecutor();

	JitExecutor(const JitExecutor &) = delete;
	JitExecutor &operator=(const JitExecutor &) = delete;

	[[nodiscard]] bool begin_execution(unsigned long generation,
					   std::uint64_t guest_pc);
	void end_execution() noexcept;
	[[nodiscard]] ::uml::cpu::EngineEvent run_block(
		CpuState &cpu, ::uml::memory::StaticGuestMemory &memory,
		unsigned long generation, std::size_t &executed);
	/* As run_block, for the AArch32 translation (A32 or T32) that the key in
	 * cpu.pc names (aarch32/guest_key.hpp). */
	[[nodiscard]] ::uml::cpu::EngineEvent run_a32_block(
		CpuState &cpu, ::uml::memory::StaticGuestMemory &memory,
		unsigned long generation, std::size_t &executed);
	[[nodiscard]] bool preempted() const noexcept;
	/* While set, every run_block retires at most one guest instruction. */
	void set_single_step(bool enabled) noexcept;
	[[nodiscard]] std::uint64_t fault_address() const noexcept;
	[[nodiscard]] ::uml::memory::Access fault_access() const noexcept;
	[[nodiscard]] bool fault_alignment() const noexcept;
	[[nodiscard]] std::uint32_t floating_point_syndrome() const noexcept;
	/* False when the process-wide configured executor ceiling was full, or when
	 * standalone construction raced or overlapped a managed pool session. Such
	 * an object owns no cache or executable mappings; begin_execution() fails
	 * and run_block() fail-closes if called directly despite that result.
	 */
	[[nodiscard]] bool budget_admitted() const noexcept;
#if UML_JIT_RUNTIME_DIAGNOSTICS
	[[nodiscard]] JitCacheValidationDiagnostics
	cache_validation_diagnostics() const noexcept;
	[[nodiscard]] JitIndirectLinkDiagnostics
	indirect_link_diagnostics() const noexcept;
#endif

private:
	friend class JitExecutorPool;
	struct ManagedPoolConstruction final {};
	[[nodiscard]] static std::unique_ptr<JitExecutor>
	make_for_managed_pool(JitExecutorOptions options);
	JitExecutor(ManagedPoolConstruction, JitExecutorOptions options);

	class Implementation;
	std::unique_ptr<Implementation> implementation_;
};

[[nodiscard]] bool native_jit_available() noexcept;

} // namespace uml::cpu::arm64
