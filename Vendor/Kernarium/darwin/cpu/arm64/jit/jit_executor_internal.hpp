// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "jit_arena.hpp"
#include "jit_cache.hpp"
#include "jit_cache_geometry.hpp"
#include "jit_executor.hpp"
#include "jit_publication.hpp"
#include "jit_runtime_contract.hpp"
#include "jit_shared_translations.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#ifndef UML_JIT_RUNTIME_DIAGNOSTICS
#define UML_JIT_RUNTIME_DIAGNOSTICS 0
#endif

namespace uml::cpu::arm64 {

namespace jit::a32 {
struct Result;
}

using ::uml::memory::Access;
using ::uml::memory::StaticGuestMemory;
using ::uml::memory::Tlb;

enum class JitCacheValidationResult {
  hit,
  mismatch,
  fault,
  sequence_race,
};

class JitExecutor::Implementation final {
public:
  explicit Implementation(
      JitCacheExecutorLease lease, JitExecutorOptions options
#if UML_JIT_RUNTIME_DIAGNOSTICS
      , JitExecutorDiagnosticsOptions diagnostics
#endif
  );
  ~Implementation();

  [[nodiscard]] bool begin_execution(unsigned long generation,
                                     std::uint64_t guest_pc);
  void end_execution() noexcept;
  [[nodiscard]] Event run_block(CpuState &cpu, StaticGuestMemory &memory,
                                unsigned long generation,
                                std::size_t &executed);
  /* As run_block, for the translation the AArch32 key in cpu.pc names
   * (jit_executor_a32.cpp). */
  [[nodiscard]] Event run_a32_block(CpuState &cpu, StaticGuestMemory &memory,
                                    unsigned long generation,
                                    std::size_t &executed);
  [[nodiscard]] bool preempted() const noexcept;
  void set_single_step(bool enabled) noexcept { single_step_ = enabled; }
  [[nodiscard]] std::uint64_t fault_address() const noexcept;
  [[nodiscard]] Access fault_access() const noexcept;
  [[nodiscard]] bool fault_alignment() const noexcept;
  [[nodiscard]] std::uint32_t floating_point_syndrome() const noexcept;
  [[nodiscard]] bool budget_admitted() const noexcept;
#if UML_JIT_RUNTIME_DIAGNOSTICS
  [[nodiscard]] JitCacheValidationDiagnostics
  cache_validation_diagnostics() const noexcept;
  [[nodiscard]] JitIndirectLinkDiagnostics
  indirect_link_diagnostics() const noexcept;
#endif

private:
  struct PendingIndirect final {
    /* The missing site's translation key, and the key of its target in
     * the site's own mode: the next block trains the site only when it
     * is that translation, so a site never links to another mode. */
    std::uint64_t source_pc{};
    std::uint64_t target_key{};
    CpuState *cpu{};
    unsigned long generation{};
    std::uint64_t code_changes{};
    std::uint64_t link_epoch{};
    bool valid{};
  };

  /* A private, unlinked translation of exactly one guest instruction. */
  struct SingleInstructionTranslation final {
    JitFunction function{};
    std::uint64_t memory_write_mask{};
    std::uint64_t direct_memory_write_mask{};
    bool accesses_fp_state{};
    bool native_fp{};
  };

  [[nodiscard]] JitFunction
  tracked_store_function(CompiledBlock &block, StaticGuestMemory &memory);
  /* The tracked variant of `block`, stored in the tracked-store arena, when
   * `compiled` (the block's code built again from memory with tracking on,
   * `guest_units` units of the key's ISA) is the same translation: its
   * instruction and unit counts, link state and direct writes are the
   * block's. Null otherwise, or when the arena has no room even after a
   * reset. */
  [[nodiscard]] JitFunction
  store_tracked_variant(CompiledBlock &block,
                        const JitCompiler::Result &compiled,
                        std::size_t guest_units);
  void clear_tracked_store_cache() noexcept;
  [[nodiscard]] Event run_floating_exception_mode(
      CpuState &cpu, StaticGuestMemory &memory, std::size_t &executed);
  void select_cpu(CpuState &cpu) noexcept;
  void release_current_reservation() noexcept;
  void reconcile_reservation(CpuState &cpu) noexcept;
  [[nodiscard]] bool fetch_instruction(StaticGuestMemory &memory,
                                       std::uint64_t pc,
                                       std::uint32_t &instruction);
  /* Leaves the fault of the guest fetch that just failed in `memory` in the
   * TLB, for the entry to report as Event::memory_fault. */
  void record_fetch_fault(StaticGuestMemory &memory) noexcept {
    const auto fault = memory.last_fault();
    tlb_.fault_address = fault.address;
    tlb_.fault_access = ::uml::memory::translated_access(fault.access);
  }
  /* Builds the translation in the single-instruction arena, replacing the
   * previous one. Empty when the instruction cannot be lowered or stored.
   */
  [[nodiscard]] std::optional<SingleInstructionTranslation>
  translate_single_instruction(std::uint64_t pc, std::uint32_t instruction,
                               bool track_direct_stores, std::uint8_t mode);
  /* The translation of one instruction, built by a compiler for it, stored
   * in the single-instruction arena. Empty when it is not exactly one
   * instruction with a usable link state, or does not fit. */
  [[nodiscard]] std::optional<SingleInstructionTranslation>
  store_single_translation(const JitCompiler::Result &compiled);
  [[nodiscard]] Event invoke_single_instruction(
      const SingleInstructionTranslation &translation, CpuState &target,
      bool tracked_stores, bool restore_environment, std::size_t &executed,
      std::uint32_t &deferred_exclusive);
  /* Runs `translation`, the untracked translation of the instruction at the
   * PC, under the store-publication protocol: as it is when no vCPU holds a
   * reservation, or as `tracked_translation()` builds it again when one
   * does.
   * Empty when that variant does not write memory as the first does. */
  template <typename TrackedTranslation>
  [[nodiscard]] std::optional<Event> invoke_single_instruction_published(
      CpuState &cpu, const SingleInstructionTranslation &translation,
      TrackedTranslation &&tracked_translation, std::size_t &executed,
      std::uint32_t &deferred_exclusive) {
    if (translation.direct_memory_write_mask == 0)
      return invoke_single_instruction(translation, cpu, false, false,
                                       executed, deferred_exclusive);
    JitBlockPublication publication{store_participant_};
    if (!publication.needs_tracking())
      return invoke_single_instruction(translation, cpu, false, false,
                                       executed, deferred_exclusive);
    const auto tracked = tracked_translation();
    if (!tracked || tracked->direct_memory_write_mask !=
                        translation.direct_memory_write_mask)
      return std::nullopt;
    return invoke_single_instruction(*tracked, cpu, true, false, executed,
                                     deferred_exclusive);
  }
  /*
   * Runs one guest instruction when its block cannot run natively: the block
   * translation failed, did not fit the arena, or its tracked variant could
   * not be built. The instruction runs as its own native translation under
   * the ordinary store-publication protocol. Failing to translate a single
   * instruction is an internal error and reports Event::fatal.
   */
  [[nodiscard]] Event run_single_instruction(CpuState &cpu,
                                             std::uint32_t instruction,
                                             std::size_t &executed);
  /* Runs the exclusive instruction that generated code recorded in
   * JitChainState after its direct-store publication has ended. */
  [[nodiscard]] Event run_deferred_exclusive_instruction(
      CpuState &cpu, std::uint32_t instruction, std::size_t &executed);

  [[nodiscard]] void *execution_context(unsigned long generation);
  [[nodiscard]] static const void *
  resolve_block_link(void *context, std::uint64_t pc);
  [[nodiscard]] static const void *
  resolve_indirect_link(void *context, std::uint64_t source_pc);
  [[nodiscard]] JitBlockLink *link_for(std::uint64_t pc);
  [[nodiscard]] JitIndirectLink *
  indirect_link_for(std::uint64_t source_pc);
  void record_pending_indirect(void *pointer, std::uint64_t source_pc,
                               CpuState &cpu, unsigned long generation,
                               std::uint64_t changes) noexcept;
  [[nodiscard]] std::optional<std::uint64_t>
  consume_pending_indirect(CpuState &cpu, unsigned long generation,
                           std::uint64_t changes,
                           std::uint64_t target_key) noexcept;
  void train_indirect(std::uint64_t source_key,
                      const CompiledBlock &target);
  void discard_links() noexcept;
  void deactivate_links() noexcept;
  void deactivate_indirect_links() noexcept;
  void prepare_links(unsigned long generation, std::uint64_t changes);
  void refresh_cached_link_target(std::uint64_t key);
  [[gnu::noinline]] void refresh_link(CompiledBlock &block);
  [[gnu::always_inline]] void activate_link(CompiledBlock &block) {
    if (block.link_epoch != link_epoch_)
      refresh_link(block);
  }

  [[nodiscard]] SharedTranslationCache::CodegenProfile
  shared_codegen_profile(std::uint8_t mode) const noexcept;
  /* Rewrites a shared translation's link pointers to this executor's tables.
   * False when a link record could not be obtained, in which case the
   * translation's code shape does not match what this executor can run and it
   * must compile for itself.
   */
  [[nodiscard]] std::optional<SharedTranslationMaterializationFailure>
  materialize_shared_translation(const SharedTranslation &translation);
  /* Installs the shared translation of the fetched `guest_words` at `pc`
   * (an A64 PC, or an AArch32 key with mode 0), or returns null. The words
   * are the fetched prefix, any padding included, and the translation's
   * identity is a prefix of them. */
  [[nodiscard]] CompiledBlock *install_shared_translation(
      unsigned long generation, std::uint64_t changes, std::uint64_t pc,
      std::uint8_t mode, std::span<const std::uint32_t> guest_words);
  /* Publishes the translation to the shared tier, keyed by `pc` (an A64 PC,
   * or an AArch32 key with mode 0) and the fetched words its identity
   * covers; null when it cannot be shared. The first form is the A64 one:
   * the identity is the translation's instructions, a word each. In the
   * second, `validation_words` are the elements that hold `guest_word_count`
   * units of the key's ISA, halfwords for T32. */
  [[nodiscard]] std::shared_ptr<const SharedTranslation>
  publish_shared_translation(
      std::uint64_t pc, std::uint8_t mode,
      std::span<const std::uint32_t> guest_words,
      const JitCompiler::Result &compiled);
  [[nodiscard]] std::shared_ptr<const SharedTranslation>
  publish_shared_translation(
      std::uint64_t pc, std::uint8_t mode,
      std::span<const std::uint32_t> validation_words,
      std::size_t guest_word_count, const JitCompiler::Result &compiled);

  [[nodiscard]] JitCacheValidationResult validate_stale_block(
      const CompiledBlock &block, std::uint64_t changes,
      StaticGuestMemory &memory);
  /* The block cache keys a block by generation, code changes and its
   * translation_key(pc, mode). */
  [[nodiscard]] CompiledBlock *find(unsigned long generation,
                                    std::uint64_t changes, std::uint64_t key,
                                    StaticGuestMemory &memory);
  [[nodiscard]] CacheSlot replacement(unsigned long generation,
                                      std::uint64_t changes,
                                      std::uint64_t key);
  /* Stores a translation as the cache slot's block. `guest_words` holds the
   * validation words kept in the executor's fallback slab when the
   * translation has none of its own (`saved_guest_words` empty). The block
   * counts `instruction_count` instructions and validates `units` guest
   * units: words for A64 and A32, halfwords for T32. */
  CompiledBlock &store_native(unsigned long generation, std::uint64_t changes,
                              std::uint64_t key, JitFunction function,
                              std::uintptr_t internal_entry,
                              JitBlockLink *target_link,
                              std::span<const JitRelocation> relocations,
                              std::size_t direct_link_refusals,
                              std::span<const std::uint32_t> guest_words,
                              std::shared_ptr<const std::uint32_t[]>
                                  saved_guest_words,
                              std::size_t instruction_count,
                              std::size_t units,
                              std::uint64_t memory_write_mask,
                              std::uint64_t direct_memory_write_mask,
                              bool accesses_fp_state, bool link_target_safe,
                              bool native_fp,
                              bool guest_illegal
  );
  void clear_cache() noexcept;

  // The AArch32 entry (jit_executor_a32.cpp), which stores, installs and
  // publishes through the A64 functions above; a T32 block counts its saved
  // halfwords apart from its instructions.
  [[nodiscard]] JitFunction
  tracked_store_function_a32(CompiledBlock &block, StaticGuestMemory &memory);
  [[nodiscard]] std::optional<SingleInstructionTranslation>
  translate_a32_single(std::uint64_t key, std::span<const std::byte> bytes,
                       std::uint8_t itstate, bool track_direct_stores);
  [[nodiscard]] Event run_a32_single(CpuState &cpu, StaticGuestMemory &memory,
                                     std::size_t &executed);
  [[nodiscard]] Event run_deferred_a32_exclusive(CpuState &cpu,
                                                 std::uint32_t word,
                                                 std::size_t &executed);

  struct LinkContext final {
    unsigned long generation{};
    std::uint64_t code_changes{};
    std::uint64_t epoch{};
    std::uint64_t stamp{};
    JitBlockLink *active_links{};
    JitIndirectLink *active_indirect_links{};
  };

  JitCacheExecutorLease cache_lease_;
  JitCacheGeometry geometry_;
  std::unique_ptr<BlockCache> block_cache_;
  /* Allocated without value-initializing the word payload. Private
   * translations touch only their cache slots; shared translations retain
   * the global immutable owner and leave these pages uncommitted.
   */
  std::shared_ptr<SavedGuestWordSet[]> fallback_saved_guest_words_;
  JitBlockLink empty_link_;
  JitBlockLinkTable links_;
  JitIndirectLinkTable indirect_links_;
  /* The AArch32 translations' second-level table of indirect targets, made
   * by the first AArch32 block (jit_linking.hpp). */
  std::unique_ptr<JitIndirectTarget[]> a32_targets_;
  // The inline context directory is included in the fixed executor charge.
  // Records remain bounded by the existing link-table capacities.
  std::array<LinkContext, 64> link_contexts_{};
  LinkContext *link_context_{};
  std::uint64_t next_link_epoch_{};
  std::uint64_t link_context_stamp_{};
  jit::detail::ExecutableArena arena_;
  jit::detail::ExecutableArena tracked_store_arena_;
  jit::detail::ExecutableArena single_instruction_arena_;
  Tlb tlb_;
  void *execution_context_{};
  unsigned long execution_generation_{};
  /* The execution context's code-change sequence, which every run
   * publishes in CpuState::code_sequence for the ISB to check. */
  const std::uint64_t *code_sequence_{&arm64::no_code_changes};
  StaticGuestMemory *prepared_memory_{};
  unsigned long mapping_generation_{};
  std::uint64_t mapping_changes_{};
  unsigned long link_generation_{};
  std::uint64_t link_code_changes_{};
  std::uint64_t link_epoch_{1};
  std::uint64_t compiled_in_link_sequence_{};
  std::uint64_t stamp_{};
  std::array<JitStoreRange, maximum_block_instructions>
      tracked_store_ranges_{};
  /* Link pointers resolved for a reused translation, applied by the arena as
   * it stores the code.
   */
  std::vector<std::uintptr_t> relocated_link_pointers_;
  JitStoreParticipant store_participant_;
  CpuState *current_cpu_{};
  std::uint32_t floating_point_syndrome_{};
#if UML_JIT_RUNTIME_DIAGNOSTICS
  JitCacheValidationDiagnostics cache_validation_diagnostics_{};
  JitIndirectLinkDiagnostics indirect_link_diagnostics_{};
  void *shared_publication_context_{};
  JitExecutorDiagnosticsOptions::BeforeSharedPublication
      before_shared_publication_{};
#endif
  PendingIndirect pending_indirect_{};
  bool preempted_{};
  bool single_step_{};
#if UML_JIT_RUNTIME_DIAGNOSTICS
  bool reuse_validated_stale_blocks_{};
#endif
  bool link_sequence_initialized_{};
  ::uml::cpu::HostCapabilities host_capabilities_;
};

} // namespace uml::cpu::arm64
