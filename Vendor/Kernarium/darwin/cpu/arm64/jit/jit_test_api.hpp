// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/execution_types.hpp"
#include "cpu/host_capabilities.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace uml::memory {
struct Tlb;
}

namespace uml::cpu::arm64 {
struct CpuState;
}

namespace uml::cpu::arm64::jit::testing {

/* Compiles the instructions at pc as one unlinked A64 block for the
 * translation mode `mode` (the SVCR mode, and translation_mode_tbi; or, for
 * the A32 form, the guest bytes at the key as an AArch32 block) and runs it
 * once on cpu with tlb as its software TLB, which the caller has filled: the
 * block's inline probes meet exactly the entries left there. A nonzero SVCR
 * mode needs cpu's SME state bound and loaded as the executor leaves it.
 * Returns the block's exit event, or fatal when the block needs link
 * relocations or cannot be published. */
[[nodiscard]] ::uml::cpu::EngineEvent
run_block_on_tlb_for_testing(std::uint64_t pc,
                             std::span<const std::uint32_t> instructions,
                             CpuState &cpu, ::uml::memory::Tlb &tlb,
                             std::uint8_t mode = 0);
[[nodiscard]] ::uml::cpu::EngineEvent
run_a32_block_on_tlb_for_testing(std::uint64_t key,
                                 std::span<const std::byte> bytes,
                                 CpuState &cpu, ::uml::memory::Tlb &tlb);

struct CompiledCode final {
  std::vector<std::uint32_t> words;
  /* The executable prefix of words, before the literal pool. */
  std::size_t executable_words{};
  std::size_t hot_words{};
  std::size_t direct_link_refusals{};
  std::size_t indirect_link_refusals{};
  std::size_t guest_instructions{};
  std::size_t internal_entry_word{};
  std::uint64_t memory_write_mask{};
  std::uint64_t direct_memory_write_mask{};
  bool accesses_fp_state{};
  bool link_target_safe{};
  bool native_fp{};
  bool guest_illegal{};
};

[[nodiscard]] CompiledCode
compile_block(std::uint64_t pc,
              std::span<const std::uint32_t> instructions);

/* An AArch32 block (jit/a32) compiled from the guest bytes at the key's
 * address, unlinked, with the ITSTATE the entry holds. `single` translates
 * exactly the first instruction. */
struct A32Compilation final {
  CompiledCode code;
  std::size_t guest_units{};
};
[[nodiscard]] A32Compilation
compile_a32_block(std::uint64_t key, std::span<const std::byte> bytes,
                  std::uint8_t itstate = 0, bool single = false);
/* The same block with link resolvers, as the executor compiles it: its exits
 * are linked, and the core registers stay in the host across them. */
[[nodiscard]] A32Compilation
compile_a32_linked_block(std::uint64_t key, std::span<const std::byte> bytes);

/* How compile_block_in_mode() wires the compiler's link operands. */
enum class LinkShape {
  unlinked,
  direct,
  indirect,
  tracked,
};

struct ModeCompilation final {
  CompiledCode code;
  /* The keys the direct and indirect link resolvers were asked for, and
   * those the translation's relocations record. */
  std::vector<std::uint64_t> direct_keys;
  std::vector<std::uint64_t> indirect_keys;
  std::vector<std::uint64_t> relocation_keys;
};

/* Compiles for the translation mode `mode` on the scoped host profile
 * (HostCapabilitiesScope). */
[[nodiscard]] ModeCompilation
compile_block_in_mode(std::uint64_t pc,
                      std::span<const std::uint32_t> instructions,
                      std::uint8_t mode,
                      LinkShape shape = LinkShape::unlinked);

[[nodiscard]] CompiledCode
compile_block_without_table_list_planning(
    std::uint64_t pc, std::span<const std::uint32_t> instructions);

[[nodiscard]] CompiledCode
compile_block_with_feature_override(
    std::uint64_t pc, std::span<const std::uint32_t> instructions,
    ::uml::cpu::ArmFeature feature, bool available);

[[nodiscard]] CompiledCode
compile_tracked_block(std::uint64_t pc,
                      std::span<const std::uint32_t> instructions);

[[nodiscard]] CompiledCode
compile_linked_block(std::uint64_t pc,
                     std::span<const std::uint32_t> instructions);

[[nodiscard]] CompiledCode
compile_linked_block_with_capacity(
    std::uint64_t pc, std::span<const std::uint32_t> instructions,
    std::size_t capacity);

[[nodiscard]] CompiledCode
compile_indirect_linked_block(
    std::uint64_t pc, std::span<const std::uint32_t> instructions);

[[nodiscard]] CompiledCode
compile_indirect_linked_block_with_capacity(
    std::uint64_t pc, std::span<const std::uint32_t> instructions,
    std::size_t capacity);

[[nodiscard]] std::vector<std::uint32_t>
compile_stable_pointer_load_for_testing(std::uintptr_t value,
                                        std::size_t literal_range_words);

[[nodiscard]] std::vector<std::uint32_t>
compile_two_stable_pointer_loads_for_testing(
    std::uintptr_t first, std::uintptr_t second,
    std::size_t literal_range_words);

[[nodiscard]] std::vector<std::uint32_t>
materialize_constant_for_testing(unsigned destination, std::uint64_t value);

/* Runs the AdvSIMD-free CNT sequence of streaming translations on value,
 * as CNT Xd (wide) or CNT Wd. */
[[nodiscard]] std::uint64_t
gpr_population_count_for_testing(bool wide, std::uint64_t value);

[[nodiscard]] std::vector<std::uint32_t>
lower_memory_barrier_for_testing(std::uint32_t instruction, bool native_sb);

/* The index of the reservation version slot that host location `host`
 * hashes to; locations with equal indices share one version. */
[[nodiscard]] std::size_t reservation_slot_for_testing(const void *host) noexcept;

[[nodiscard]] std::size_t successful_link_host_word_savings() noexcept;
[[nodiscard]] std::size_t link_chain_instruction_limit() noexcept;
[[nodiscard]] std::size_t link_capability_selector_count() noexcept;
[[nodiscard]] std::size_t block_link_record_size() noexcept;
[[nodiscard]] std::size_t block_link_record_alignment() noexcept;
[[nodiscard]] std::size_t block_link_internal_entry_offset() noexcept;
[[nodiscard]] std::size_t indirect_link_entry_size() noexcept;
[[nodiscard]] std::uint8_t
link_selector_for_capability_mask(std::uint8_t capabilities) noexcept;
[[nodiscard]] std::uint8_t
checked_link_selector_for_capability_mask(std::uint8_t capabilities) noexcept;
[[nodiscard]] std::uint8_t
link_capability_mask_for_selector(std::size_t selector) noexcept;

[[nodiscard]] bool invalid_emitter_metadata_is_rejected_for_testing();

[[nodiscard]] bool
block_link_table_is_bounded(std::size_t capacity);
[[nodiscard]] bool
indirect_link_table_is_bounded(std::size_t capacity);

[[nodiscard]] bool
exclusive_operations_are_serialized_for_testing();
[[nodiscard]] bool
normal_store_leaves_exclusive_gate_unchanged_for_testing();
[[nodiscard]] bool
store_first_publication_handshake_for_testing();
[[nodiscard]] bool
waiting_store_precedes_later_helper_for_testing();
[[nodiscard]] bool
waiting_store_precedes_already_queued_helper_for_testing();
[[nodiscard]] bool
all_waiting_stores_precede_queued_helper_for_testing();
[[nodiscard]] bool
late_store_waits_for_current_helper_for_testing();
[[nodiscard]] bool
tracked_store_invalidation_does_not_deadlock_for_testing();
[[nodiscard]] bool
publication_gate_randomized_stress_for_testing();
[[nodiscard]] bool
publication_participant_lifetime_is_idempotent_for_testing();

/* The JIT's process-wide exclusive monitor: the reservations that are held,
 * the reservations recorded, the registered store participants, and whether
 * the exclusive gate is idle. All of it is empty when no JIT vCPU exists, as
 * between two runs of a guest that restarts itself. */
struct ExclusiveMonitorState final {
  unsigned active_reservations{};
  std::size_t reservations{};
  std::size_t store_participants{};
  bool gate_quiescent{};
};
[[nodiscard]] ExclusiveMonitorState exclusive_monitor_state_for_testing();

} // namespace uml::cpu::arm64::jit::testing
