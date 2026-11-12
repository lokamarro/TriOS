// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_executor_internal.hpp"

#include "jit_semantic_reservation.hpp"

#include "cpu/arm64/detail/floating_exception.hpp"
#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstring>
#include <optional>
#include <span>

namespace uml::cpu::arm64 {

namespace {

/* The SVCR mode of the translation cpu's next block runs. */
[[nodiscard]] std::uint8_t guest_sme_mode(const CpuState &cpu) noexcept {
  return static_cast<std::uint8_t>(cpu.svcr & svcr_mask);
}

/* The mode of the translation cpu's next block runs: its SVCR mode, and
 * whether the guest's data addresses ignore their top byte. */
[[nodiscard]] std::uint8_t guest_translation_mode(const CpuState &cpu) noexcept {
  return static_cast<std::uint8_t>(
      guest_sme_mode(cpu) | (cpu.tbi_enabled ? translation_mode_tbi : 0U));
}

/* Bits of an A64 PC that a translation key uses for its own state, which no
 * guest address a task can map has set. */
inline constexpr std::uint64_t key_state_bits =
    aarch32::aarch32_key_bit | aarch32::thumb_key_bit |
    aarch32::a64_tbi_key_bit;

} // namespace

[[nodiscard]] Event JitExecutor::Implementation::run_block(CpuState &cpu, StaticGuestMemory &memory,
                              unsigned long generation,
                              std::size_t &executed) {
  if (!cache_lease_.admitted()) {
    executed = 0;
    return Event::fatal;
  }
  const ::uml::cpu::HostCapabilitiesScope capabilities_scope{
      host_capabilities_};
  executed = 0;
  floating_point_syndrome_ = 0;
  tlb_.fault_alignment = 0;
  select_cpu(cpu);
  const auto sequences =
      darwin_uml_static_vm_execution_context_sequences(execution_context_);
  preempted_ = sequences.execution_interrupt != 0;
  if (preempted_)
    return Event::interrupt;
  /* What the translations this run reaches are validated at, below: an ISB
   * leaves when the address space's code has changed since. */
  cpu.code_sequence = code_sequence_;
  cpu.code_sequence_seen = sequences.code_changes;
  const bool mapping_changed = mapping_generation_ != generation ||
                               mapping_changes_ != sequences.mapping_changes;
  /* StaticGuestMemory belongs to the task-carrier thread, while a pooled JIT
   * executor may serve a different carrier on its next guest entry. Prepare
   * each distinct per-entry memory object even when the executor's global
   * generation/change view is already current. */
  if (prepared_memory_ != &memory || mapping_changed) {
    memory.prepare(generation, sequences.mapping_changes);
    prepared_memory_ = &memory;
  }
  if (mapping_changed) {
    tlb_.prepare(generation, sequences.mapping_changes);
    mapping_generation_ = generation;
    mapping_changes_ = sequences.mapping_changes;
  }
  prepare_links(generation, sequences.code_changes);
  /*
   * A PC with a bit that a translation key reserves for its own state is
   * no mapped address; leaving it to the lookup below would run the
   * translation of another PC or ISA under it. It faults as a fetch from
   * an unmapped address does.
   */
  if ((cpu.pc & key_state_bits) != 0U) [[unlikely]] {
    pending_indirect_ = {};
    std::uint32_t unmapped{};
    (void)fetch_instruction(memory, cpu.pc, unmapped);
    return Event::memory_fault;
  }
  /*
   * The guest SVCR mode and top-byte-ignore select the translation
   * (translation_key()). Only a run that implements SME can be in a nonzero
   * SVCR mode, and the state that mode makes live is accessible before any
   * of its code runs. Nothing here changes SVCR before the block runs, so
   * each use reads it from CpuState rather than keeping it live across this
   * dispatch path.
   */
  if (guest_sme_mode(cpu) != 0U) [[unlikely]] {
    if (cpu.sme_registers == nullptr || !implements_sme(host_capabilities_))
      return Event::fatal;
    if ((cpu.pc & (alignof(std::uint32_t) - 1U)) != 0U)
      return Event::pc_alignment;
    cpu.access_sme_state();
    /* XNU resets FPSR on every kernel entry while a thread is in streaming
     * mode, so SM=1 code keeps the guest's FPCR and FPSR in CpuState and
     * never runs with the native environment active. */
    if ((cpu.svcr & svcr_sm) != 0U)
      arm64::restore_native_fp_environment(cpu);
  }
  const auto block_key =
      translation_key(cpu.pc, guest_translation_mode(cpu));
  const auto pending_source = consume_pending_indirect(
      cpu, generation, sequences.code_changes, block_key);

  /*
   * Enabled guest exception traps and single steps are rare.  Keep them
   * completely outside the cached multi-instruction hot path so no
   * translated block can run past a potentially trapping or stepped
   * instruction.  This cold path still lowers and executes the exact native
   * JIT form; it merely limits the boundary to one guest instruction so a
   * register-only FP/SIMD operation can be committed transactionally.
   */
  const auto guest_fpcr = cpu.effective_fpcr();
  /* Streaming mode never traps a floating-point exception. */
  if (single_step_ ||
      (arm64::detail::enabled_exception_flags(guest_fpcr) != 0U &&
       (cpu.svcr & svcr_sm) == 0U)) {
#if UML_JIT_RUNTIME_DIAGNOSTICS
    indirect_link_diagnostics_.unsafe_target_refusals +=
        pending_source.has_value();
#endif
    return run_floating_exception_mode(cpu, memory, executed);
  }

  auto *block = find(generation, sequences.code_changes, block_key, memory);
  if (block == nullptr) {
    const auto mode = guest_translation_mode(cpu);
    const auto key = translation_key(cpu.pc, mode);
    std::array<std::uint32_t, maximum_block_instructions> instructions{};
    std::size_t count = 0;
    while (count < instructions.size()) {
      std::array<std::byte, sizeof(std::uint32_t)> encoded{};
      if (!memory.fetch(cpu.pc + count * 4, encoded))
        break;
      std::memcpy(&instructions[count], encoded.data(), encoded.size());
      const auto terminates =
          obvious_translated_block_terminator(instructions[count]);
      ++count;
      if (terminates) {
        // A compare/exchange retry has a conditional exit before its store.
        // Look ahead only far enough to recognize that bounded native region.
        // A speculative fetch failure leaves the ordinary prefix intact.
        if (count == 3U &&
            (instructions[0] & 0x3fe0'0000U) == 0x0840'0000U &&
            (instructions[1] & 0x7fe0'fc1fU) == 0x6b00'001fU &&
            (instructions[2] & 0xff00'001fU) == 0x5400'0001U) {
          for (; count < 5U; ++count) {
            if (!memory.fetch(cpu.pc + count * 4U, encoded))
              break;
            std::memcpy(&instructions[count], encoded.data(), encoded.size());
          }
        }
        break;
      }
    }
    if (count == 0) {
      record_fetch_fault(memory);
      return Event::memory_fault;
    }
    /*
     * Another executor may already have decoded and emitted exactly these
     * guest words at exactly this PC. Reuse that work by copying its
     * translation and rewriting the link pointers for this executor's own
     * tables; the host code, arena and link records all stay private.
     */
    const auto fetched = std::span{instructions}.first(count);
    auto *shared = install_shared_translation(
        generation, sequences.code_changes, cpu.pc, mode, fetched);
    if (shared != nullptr) {
      block = shared;
    } else {
      JitCompiler compiler{
          this, &Implementation::resolve_block_link, false,
          &Implementation::resolve_indirect_link
      };
      compiler.set_translation_mode(mode);
      // Compile once on a miss. Speculatively lowering the whole block a
      // second time for GPR residency delays cold multi-process workloads.
      const auto compiled = compiler.compile(cpu.pc, fetched);
#if UML_JIT_RUNTIME_DIAGNOSTICS
      cache_validation_diagnostics_.direct_link_refusals +=
          compiled.direct_link_refusals;
      cache_validation_diagnostics_.indirect_link_refusals +=
          compiled.indirect_link_refusals;
#endif
      {
        const auto valid_compilation = [&](const auto &candidate) {
          return !candidate.words.empty() && candidate.guest_instructions != 0 &&
                 candidate.guest_instructions <= instructions.size() &&
                 valid_link_state_metadata(candidate.accesses_fp_state,
                                           candidate.native_fp);
        };
        const auto install_compilation =
            [&](const auto &candidate,
                const jit::detail::ExecutableArena::StoredBlock &stored,
                const std::shared_ptr<const SharedTranslation> &translation) {
          const auto internal_entry =
              reinterpret_cast<std::uintptr_t>(stored.code) +
              candidate.internal_entry_word * sizeof(std::uint32_t);
          auto saved_guest_words =
              translation == nullptr
                  ? std::shared_ptr<const std::uint32_t[]>{}
                  : translation->guest_words;
          return &store_native(
              generation, sequences.code_changes, key,
              sign_jit_function<JitFunction>(stored.code), internal_entry,
              stored.target_link, candidate.relocations,
              stored.direct_link_refusals,
              std::span{instructions}.first(candidate.guest_instructions),
              std::move(saved_guest_words), candidate.guest_instructions,
              candidate.guest_instructions,
              candidate.memory_write_mask, candidate.direct_memory_write_mask,
              candidate.accesses_fp_state, candidate.link_target_safe,
              candidate.native_fp,
              candidate.guest_illegal
          );
        };
        if (!valid_compilation(compiled))
          return run_single_instruction(cpu, instructions[0], executed);
        /* Link records are keyed by address-space generation and leave only
         * with the whole cache, so exited address spaces keep theirs. A site
         * refused by a table that holds records recycles the cache, as a
         * full arena or direct-link table does, instead of publishing an
         * unlinked block. */
        jit::detail::ExecutableArena::StoredBlock stored{.links_exhausted = true};
        if (compiled.indirect_link_refusals == 0 || indirect_links_.size() == 0)
          stored = arena_.store_block(
              key, compiled.words,
              compiled.relocations, {}, links_, empty_link_, generation);
        if (stored) {
          auto shared =
              publish_shared_translation(cpu.pc, mode, fetched, compiled);
          block = install_compilation(compiled, stored, shared);
        } else {
#if UML_JIT_RUNTIME_DIAGNOSTICS
          if (stored.links_exhausted)
            ++cache_validation_diagnostics_.link_table_resets;
          else
            ++cache_validation_diagnostics_.arena_resets;
#endif
          clear_cache();
          arena_.reset();

          /*
           * Indirect PIC operands are absolute pointers into the descriptor
           * table. clear_cache() invalidates that storage, so the retry must
           * be freshly lowered rather than copying its stale site pointer.
           * Direct descriptors are regenerated in the private link table.
           */
          JitCompiler retry_compiler{
              this, &Implementation::resolve_block_link, false,
              &Implementation::resolve_indirect_link
          };
          retry_compiler.set_translation_mode(mode);
          const auto retry_compiled = retry_compiler.compile(
              cpu.pc, std::span{instructions}.first(count));
#if UML_JIT_RUNTIME_DIAGNOSTICS
          cache_validation_diagnostics_.direct_link_refusals +=
              retry_compiled.direct_link_refusals;
          cache_validation_diagnostics_.indirect_link_refusals +=
              retry_compiled.indirect_link_refusals;
#endif
          if (!valid_compilation(retry_compiled))
            return run_single_instruction(cpu, instructions[0], executed);
          stored = arena_.store_block(
              key, retry_compiled.words,
              retry_compiled.relocations, {}, links_, empty_link_,
              generation);
          if (!stored)
            return run_single_instruction(cpu, instructions[0], executed);
          auto shared = publish_shared_translation(cpu.pc, mode, fetched,
                                                   retry_compiled);
          block = install_compilation(retry_compiled, stored, shared);
        }
      }
    }
  }
  if (pending_source)
    train_indirect(*pending_source, *block);
  std::uint32_t deferred_exclusive = 0;
  const auto execute_native = [&](JitFunction function,
                                  bool tracked_stores) -> Event {
    if (tracked_stores)
      tracked_store_ranges_.fill({});
    std::uint8_t link_capabilities =
        block->direct_memory_write_mask != 0
            ? link_capability(JitLinkCapability::store_publication)
            : std::uint8_t{0};
    JitChainState chain{
        .stores = tracked_stores ? tracked_store_ranges_.data() : nullptr,
    };
    if (block->accesses_fp_state) {
      cpu.access_fp_state();
      link_capabilities |=
          link_capability(JitLinkCapability::fp_state_materialized);
      if (block->native_fp) {
        activate_native_fp_environment(cpu);
        link_capabilities |=
            link_capability(JitLinkCapability::native_fp_environment);
      }
    }
    chain.link_selector = checked_link_selector(link_capabilities);
    const auto result = function(&cpu, &tlb_, &chain);
    debug_check_host_streaming_state(host_capabilities_);
    if (tracked_stores)
      jit::semantic::invalidate_store_ranges(
          cpu, std::span<const JitStoreRange>{tracked_store_ranges_}.first(
                   block->instruction_count));
    const auto [event, current_executed] = unpack_jit_result(result);
    deferred_exclusive = chain.deferred_exclusive_instruction;
    executed = maximum_chain_instructions -
                   static_cast<std::size_t>(chain.remaining_instructions) +
               current_executed;
    if (event == Event::memory_fault)
      tlb_.discard(tlb_.fault_address);
    if (event == Event::continued && chain.indirect_miss != nullptr)
      record_pending_indirect(chain.indirect_miss,
                              chain.indirect_source_pc, cpu, generation,
                              sequences.code_changes);
    if (deferred_exclusive == 0U)
      reconcile_reservation(cpu);
    return event;
  };

  if (block->direct_memory_write_mask == 0)
    return execute_native(block->function, false);
  Event event{};
  bool tracked_unavailable = false;
  {
    JitBlockPublication publication{store_participant_};
    if (!publication.needs_tracking()) {
      event = execute_native(block->function, false);
    } else {
      const auto tracked = tracked_store_function(*block, memory);
      if (tracked == nullptr)
        tracked_unavailable = true;
      else
        event = execute_native(tracked, true);
    }
  }
  if (tracked_unavailable) {
    /*
     * The block's tracked variant could not be rebuilt or stored. Nothing has
     * executed yet, so make progress with the block's first instruction as
     * its own one-instruction translation, which republishes and tracks its
     * stores exactly as a tracked block does.
     */
    std::uint32_t instruction{};
    if (!fetch_instruction(memory, cpu.pc, instruction))
      return Event::memory_fault;
    return run_single_instruction(cpu, instruction, executed);
  }
  if (deferred_exclusive != 0U) {
    const auto retired_prefix = executed;
    std::size_t exclusive_executed = 0;
    event = run_deferred_exclusive_instruction(cpu, deferred_exclusive,
                                               exclusive_executed);
    executed = retired_prefix + exclusive_executed;
  }
  reconcile_reservation(cpu);
  return event;
}

[[nodiscard]] JitFunction
JitExecutor::Implementation::tracked_store_function(CompiledBlock &block, StaticGuestMemory &memory) {
  if (block.tracked_function != nullptr)
    return block.tracked_function;

  std::array<std::uint32_t, maximum_block_instructions> instructions{};
  for (std::size_t index = 0; index < block.instruction_count; ++index) {
    if (!memory.fetch(
            translation_key_pc(block.key) + index * sizeof(std::uint32_t),
            std::as_writable_bytes(std::span{&instructions[index], 1})))
      return nullptr;
  }
  JitCompiler compiler{nullptr, nullptr, true};
  compiler.set_translation_mode(translation_key_mode(block.key));
  const auto compiled = compiler.compile(
      translation_key_pc(block.key),
      std::span{instructions}.first(block.instruction_count));
  return store_tracked_variant(block, compiled, compiled.guest_instructions);
}

[[nodiscard]] JitFunction JitExecutor::Implementation::store_tracked_variant(
    CompiledBlock &block, const JitCompiler::Result &compiled,
    std::size_t guest_units) {
  if (compiled.words.empty() ||
      compiled.guest_instructions != block.instruction_count ||
      guest_units != block.saved_units ||
      !valid_link_state_metadata(compiled.accesses_fp_state,
                                 compiled.native_fp) ||
      compiled.direct_memory_write_mask != block.direct_memory_write_mask)
    return nullptr;

  auto *code = tracked_store_arena_.store(compiled.words);
  if (code == nullptr) {
    clear_tracked_store_cache();
    code = tracked_store_arena_.store(compiled.words);
    if (code == nullptr)
      return nullptr;
  }
  block.tracked_function = sign_jit_function<JitFunction>(code);
  return block.tracked_function;
}

void JitExecutor::Implementation::clear_tracked_store_cache() noexcept {
  for (auto &set : *block_cache_)
    for (auto &block : set.ways)
      block.tracked_function = nullptr;
  tracked_store_arena_.reset();
}

[[nodiscard]] Event JitExecutor::Implementation::run_floating_exception_mode(
    CpuState &cpu, StaticGuestMemory &memory, std::size_t &executed) {
  executed = 0;
  std::uint32_t instruction{};
  if (!fetch_instruction(memory, cpu.pc, instruction))
    return Event::memory_fault;

  /* The transaction below runs on a copy of CpuState, which would alias
   * the SME register file; streaming mode needs none, as it never traps. */
  const auto guest_fpcr = cpu.effective_fpcr();
  if ((cpu.svcr & svcr_sm) != 0U ||
      !arm64::detail::needs_floating_exception_transaction(guest_fpcr,
                                                           instruction))
    return run_single_instruction(cpu, instruction, executed);

  /*
   * A previous native block may have retained the guest environment in
   * host registers.  Materialize it before taking the isolated snapshot.
   */
  arm64::restore_native_fp_environment(cpu);
  cpu.access_fp_state();
  const auto translation =
      translate_single_instruction(cpu.pc, instruction, false,
                                   guest_translation_mode(cpu));
  /*
   * The transactional class is register-only FP/SIMD data processing. A
   * translation that could not be built, or that would write memory the
   * snapshot cannot roll back, is an internal failure.
   */
  if (!translation || translation->memory_write_mask != 0)
    return Event::fatal;
  const auto original_fpsr = cpu.fpsr;
  auto isolated = cpu;
  isolated.fpsr &= ~arm64::detail::fpsr_exception_cumulative_mask;
  isolated.gadget_host_fpcr = 0;
  isolated.gadget_host_fpsr = 0;
  isolated.gadget_fp_active = false;
  isolated.gadget_indirect_miss = nullptr;
  std::uint32_t deferred_exclusive = 0;
  const auto event = invoke_single_instruction(
      *translation, isolated, false, true, executed, deferred_exclusive);
  if (event != Event::continued)
    return event;
  const auto decision = arm64::detail::decide_a64_floating_exception(
      cpu.fpcr, original_fpsr, isolated.fpsr, instruction,
      cpu.vectors[(instruction >> 5U) & 31U].bytes,
      isolated.vectors[instruction & 31U].bytes);
  if (decision.traps()) {
    cpu.fpsr = decision.committed_fpsr;
    floating_point_syndrome_ = decision.syndrome;
    executed = 0;
    return Event::floating_point_trap;
  }
  isolated.fpsr =
      (isolated.fpsr & ~arm64::detail::fpsr_exception_cumulative_mask) |
      (decision.committed_fpsr &
       arm64::detail::fpsr_exception_cumulative_mask);
  cpu = isolated;
  reconcile_reservation(cpu);
  return event;
}

[[nodiscard]] bool JitExecutor::Implementation::fetch_instruction(
    StaticGuestMemory &memory, std::uint64_t pc, std::uint32_t &instruction) {
  if (memory.fetch(pc, std::as_writable_bytes(
                           std::span{&instruction, std::size_t{1}})))
    return true;
  record_fetch_fault(memory);
  return false;
}

[[nodiscard]] std::optional<
    JitExecutor::Implementation::SingleInstructionTranslation>
JitExecutor::Implementation::translate_single_instruction(
    std::uint64_t pc, std::uint32_t instruction, bool track_direct_stores,
    std::uint8_t mode) {
  JitCompiler compiler{nullptr, nullptr, track_direct_stores};
  compiler.set_translation_mode(mode);
  return store_single_translation(
      compiler.compile(pc, std::span{&instruction, std::size_t{1}}));
}

[[nodiscard]] std::optional<
    JitExecutor::Implementation::SingleInstructionTranslation>
JitExecutor::Implementation::store_single_translation(
    const JitCompiler::Result &compiled) {
  if (compiled.words.empty() || compiled.guest_instructions != 1U ||
      !valid_link_state_metadata(compiled.accesses_fp_state,
                                 compiled.native_fp))
    return std::nullopt;
  /* The arena holds only the translation about to run, so a reset cannot
   * discard code that is still executing. */
  single_instruction_arena_.reset();
  auto *code = single_instruction_arena_.store(compiled.words);
  if (code == nullptr)
    return std::nullopt;
  return SingleInstructionTranslation{
      .function = sign_jit_function<JitFunction>(code),
      .memory_write_mask = compiled.memory_write_mask,
      .direct_memory_write_mask = compiled.direct_memory_write_mask,
      .accesses_fp_state = compiled.accesses_fp_state,
      .native_fp = compiled.native_fp,
  };
}

[[nodiscard]] Event JitExecutor::Implementation::invoke_single_instruction(
    const SingleInstructionTranslation &translation, CpuState &target,
    bool tracked_stores, bool restore_environment, std::size_t &executed,
    std::uint32_t &deferred_exclusive) {
  if (tracked_stores)
    tracked_store_ranges_.fill({});
  std::uint8_t link_capabilities =
      translation.direct_memory_write_mask != 0
          ? link_capability(JitLinkCapability::store_publication)
          : std::uint8_t{0};
  JitChainState chain{
      .stores = tracked_stores ? tracked_store_ranges_.data() : nullptr,
  };
  if (translation.accesses_fp_state) {
    target.access_fp_state();
    link_capabilities |=
        link_capability(JitLinkCapability::fp_state_materialized);
    if (translation.native_fp) {
      activate_native_fp_environment(target);
      link_capabilities |=
          link_capability(JitLinkCapability::native_fp_environment);
    }
  }
  chain.link_selector = checked_link_selector(link_capabilities);
  const auto result = translation.function(&target, &tlb_, &chain);
  if (restore_environment)
    arm64::restore_native_fp_environment(target);
  if (tracked_stores)
    jit::semantic::invalidate_store_ranges(
        target,
        std::span<const JitStoreRange>{tracked_store_ranges_}.first(1U));
  const auto unpacked = unpack_jit_result(result);
  executed = unpacked.retired;
  deferred_exclusive = chain.deferred_exclusive_instruction;
  const auto event = unpacked.event;
  if (event == Event::memory_fault)
    tlb_.discard(tlb_.fault_address);
  return event;
}

[[nodiscard]] Event JitExecutor::Implementation::run_single_instruction(
    CpuState &cpu, std::uint32_t instruction, std::size_t &executed) {
  executed = 0;
  const auto translation =
      translate_single_instruction(cpu.pc, instruction, false,
                                   guest_translation_mode(cpu));
  if (!translation)
    return Event::fatal;
  std::uint32_t deferred_exclusive = 0;
  const auto published = invoke_single_instruction_published(
      cpu, *translation,
      [&] {
        return translate_single_instruction(cpu.pc, instruction, true,
                                            guest_translation_mode(cpu));
      },
      executed, deferred_exclusive);
  if (!published)
    return Event::fatal;
  auto event = *published;
  if (deferred_exclusive != 0U) {
    const auto retired_prefix = executed;
    std::size_t exclusive_executed = 0;
    event = run_deferred_exclusive_instruction(cpu, deferred_exclusive,
                                               exclusive_executed);
    executed = retired_prefix + exclusive_executed;
  }
  reconcile_reservation(cpu);
  return event;
}

void JitExecutor::Implementation::select_cpu(CpuState &cpu) noexcept {
  if (current_cpu_ == &cpu)
    return;
  pending_indirect_ = {};
  if (current_cpu_ != nullptr)
    arm64::restore_native_fp_environment(*current_cpu_);
  release_current_reservation();
  current_cpu_ = &cpu;
}

void JitExecutor::Implementation::release_current_reservation() noexcept {
  if (current_cpu_ == nullptr ||
      store_participant_.reservation_cpu.load(
          std::memory_order_seq_cst) != current_cpu_)
    return;
  store_participant_.reservation_cpu.store(nullptr,
                                           std::memory_order_seq_cst);
  end_jit_reservation(*current_cpu_);
}

void JitExecutor::Implementation::reconcile_reservation(CpuState &cpu) noexcept {
  if (cpu.exclusive_size != 0) {
    store_participant_.reservation_cpu.store(
        &cpu, std::memory_order_seq_cst);
    return;
  }
  if (store_participant_.reservation_cpu.load(
          std::memory_order_seq_cst) != &cpu)
    return;
  store_participant_.reservation_cpu.store(nullptr,
                                           std::memory_order_seq_cst);
  end_jit_reservation(cpu);
}

[[nodiscard]] Event
JitExecutor::Implementation::run_deferred_exclusive_instruction(
    CpuState &cpu, std::uint32_t instruction, std::size_t &executed) {
  const auto result =
      uml_arm64_jit_execute_exclusive(&cpu, &tlb_, instruction);
  const auto [event, retired] = unpack_jit_result(result);
  executed = retired;
  if (event == Event::memory_fault)
    tlb_.discard(tlb_.fault_address);
  return event;
}

void JitExecutor::Implementation::record_pending_indirect(void *pointer, std::uint64_t source_pc,
                             CpuState &cpu,
                             unsigned long generation,
                             std::uint64_t changes) noexcept {
  /*
   * Resolve the translation-owned source address before touching the
   * generated-code pointer. A miss can therefore never turn a corrupted or
   * stale pointer into a host dereference.
   */
  /* An AArch32 key never has the top-byte-ignore bit, so only an A64 PC
   * that would alias a translation of another mode has it. */
  if ((cpu.pc & (alignof(std::uint32_t) - 1U)) != 0U ||
      (cpu.pc & aarch32::a64_tbi_key_bit) != 0U) {
    pending_indirect_ = {};
    return;
  }
  auto *site = indirect_links_.find(source_pc, generation);
  if (site == nullptr || static_cast<void *>(site) != pointer) {
    pending_indirect_ = {};
    return;
  }
  pending_indirect_ = {
      .source_pc = source_pc,
      .target_key =
          translation_key(cpu.pc, translation_key_mode(source_pc)),
      .cpu = &cpu,
      .generation = generation,
      .code_changes = changes,
      .link_epoch = link_epoch_,
      .valid = true,
  };
#if UML_JIT_RUNTIME_DIAGNOSTICS
  ++indirect_link_diagnostics_.misses;
#endif
}

[[nodiscard]] std::optional<std::uint64_t>
JitExecutor::Implementation::consume_pending_indirect(CpuState &cpu, unsigned long generation,
                         std::uint64_t changes,
                         std::uint64_t target_key) noexcept {
  const auto pending = pending_indirect_;
  pending_indirect_ = {};
  if (!pending.valid || pending.cpu != &cpu ||
      pending.generation != generation ||
      pending.code_changes != changes ||
      pending.link_epoch != link_epoch_ ||
      pending.target_key != target_key) {
#if UML_JIT_RUNTIME_DIAGNOSTICS
    indirect_link_diagnostics_.scope_rejections += pending.valid;
#endif
    return std::nullopt;
  }
  return pending.source_pc;
}

void JitExecutor::Implementation::prepare_links(unsigned long generation, std::uint64_t changes) {
  if (link_generation_ == generation && link_code_changes_ == changes &&
      link_context_ != nullptr)
    return;
  if (!link_sequence_initialized_)
    link_sequence_initialized_ = true;
  compiled_in_link_sequence_ = 0;
  pending_indirect_ = {};

  auto *selected = &link_contexts_.front();
  for (auto &context : link_contexts_) {
    if (context.epoch != 0 && context.generation == generation) {
      selected = &context;
      break;
    }
    if (context.stamp < selected->stamp)
      selected = &context;
  }
  link_context_ = selected;
  if (selected->epoch == 0 || selected->generation != generation ||
      selected->code_changes != changes) {
    deactivate_indirect_links();
    deactivate_links();
    *selected = {.generation = generation,
                 .code_changes = changes,
                 .epoch = ++next_link_epoch_};
  }
  selected->stamp = ++link_context_stamp_;
  link_epoch_ = selected->epoch;
  link_generation_ = generation;
  link_code_changes_ = changes;
}

} // namespace uml::cpu::arm64
