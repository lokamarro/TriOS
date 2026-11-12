// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_compiler.hpp"

#include "cpu/arm64/code_synchronization.hpp"
#include "cpu/arm64/detail/floating_exception.hpp"
#include "cpu/arm64/detail/fp_state_access.hpp"
#include "cpu/arm64/detail/instruction_primitives.hpp"
#include "cpu/arm64/detail/memory_copy_set.hpp"
#include "cpu/arm64/floating_controls.hpp"
#include "cpu/arm64/id_registers.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace uml::cpu::arm64 {

JitCompiler::JitCompiler(void *link_context,
                       LinkResolver link_resolver,
                       bool track_direct_stores,
                       IndirectLinkResolver indirect_link_resolver) noexcept
      : emitter_{std::size_t{1} << 18U},
        cold_{this},
        configured_link_context_{link_context},
        configured_link_resolver_{link_resolver},
        configured_indirect_link_resolver_{indirect_link_resolver},
        track_direct_stores_{track_direct_stores} {
    attach_family_emitter(&emitter_, &native_fp_, !track_direct_stores_);
    attach_link_resolver(link_context, link_resolver);
    attach_indirect_link_resolver(link_context, indirect_link_resolver);
    emitter_.attach_exit_flush(this, [](void *context) {
      static_cast<JitCompiler *>(context)->flush_exit_gprs();
    });
    emitter_.attach_flag_clobber(this, [](void *context) {
      static_cast<JitCompiler *>(context)
          ->spill_flags_for_internal_clobber();
    });
  }

JitCompiler::JitCompiler(
              const ArmFeatureSet &translation_features) noexcept
      : JitCompiler{} {
    translation_features_ = &translation_features;
  }

void JitCompiler::set_table_list_planning_enabled(bool enabled) noexcept {
    table_list_planning_enabled_ = enabled;
  }

void JitCompiler::set_translation_mode(std::uint8_t mode) noexcept {
    JitCompilerFamilies::set_translation_mode(mode);
    emitter_.set_sme_host_mode(jit::jit_sme_host_mode(
        sme_mode(), sme_profile(host_capabilities()) == SmeProfile::sme2));
  }

JitCompiler::Result JitCompiler::compile(std::uint64_t guest_pc,
                 std::span<const std::uint32_t> instructions) {
    return compile_once(guest_pc, instructions);
  }

JitCompiler::Result JitCompiler::compile_once(std::uint64_t guest_pc,
                      std::span<const std::uint32_t> instructions) {
    begin_link_observation();
    /* A streaming translation never binds a host vector register: AdvSIMD
     * is illegal there, and the guest's vectors stay in the host's own. */
    const bool streaming = streaming_translation();
    if (!streaming) {
      select_constrained_vector_pins(instructions);
      if (table_list_planning_enabled_)
        select_consecutive_vector_lists(instructions);
    } else {
      /* The SME enter installs the guest's FPCR from CpuState, and the
       * register-only forms accumulate its FPSR there. */
      accesses_fp_state_ = true;
    }
    emitter_.prologue();
    const auto internal_entry_word = emitter_.size();
    while (count_ < instructions.size()) {
      const auto pc = guest_pc + count_ * sizeof(std::uint32_t);
      if (const auto run = streaming ? std::size_t{0}
                                     : try_emit_simd_unsigned_run(
                                           instructions.subspan(count_), pc);
          run != 0) {
        accesses_fp_state_ = true;
        count_ += run;
        continue;
      }
      if (const auto run = try_emit_sp_unsigned_run(
              instructions.subspan(count_), pc);
          run != 0) {
        count_ += run;
        continue;
      }
      if (const auto run = try_emit_gpr_unsigned_run(
              instructions.subspan(count_), pc);
          run != 0) {
        count_ += run;
        continue;
      }
      if (try_emit_native_exclusive_run(instructions.subspan(count_), pc))
        continue;
      /* The inline copy uses AdvSIMD; streaming code takes the helper. */
      if (const auto run =
              streaming ? std::size_t{0}
                        : try_emit_memory_copy_set_sequence(
                              instructions.subspan(count_), pc);
          run != 0) {
        count_ += run;
        continue;
      }
      const auto instruction = instructions[count_];
      if (reservation_sensitive_allocation(instruction)) {
        /*
         * A reservation-establishing helper waits for every other vCPU's
         * published direct-store block.  It therefore cannot execute inside
         * a publication inherited from an earlier instruction or linked
         * predecessor: two such vCPUs could otherwise wait on one another.
         *
         * Split before the allocation and mark its one-instruction block as
         * an ineligible chain target.  Both decisions happen while
         * translating, leaving ordinary generated blocks branch-free.
         */
        if (count_ != 0) {
          store_pc(pc);
          emitter_.return_constant(Event::continued, count_);
          terminated_ = true;
          break;
        }
        link_target_safe_ = false;
      }
      const auto action = emit_instruction(instruction, pc);
      if (action == Action::unsupported) {
        /*
         * Keep a supported native prefix instead of discarding it
         * merely because the following instruction is unsupported.
         * A block beginning with an unsupported or unavailable
         * instruction becomes a precise guest illegal-instruction
         * terminal. The generated block contains no runtime decode or
         * feature check, and the faulting instruction does not retire.
         */
        if (count_ == 0) {
          guest_illegal_ = true;
          store_pc(pc);
          emitter_.return_constant(Event::illegal_instruction, count_ + 1U);
          ++count_;
          terminated_ = true;
        } else {
          store_pc(pc);
          emitter_.return_constant(Event::continued, count_);
          terminated_ = true;
        }
        break;
      }
      accesses_fp_state_ =
          accesses_fp_state_ ||
          arm64::detail::instruction_accesses_fp_state(instruction);
      ++count_;
      if (action == Action::terminate) {
        terminated_ = true;
        break;
      }
    }
    if (!terminated_) {
      /*
       * The fetch window cut the block short. Its successor is as static
       * as a jump target, so the exit asks for a link as a branch does; an
       * unlinked one returns to the executor after every full window.
       */
      emit_direct_branch(guest_pc + count_ * sizeof(std::uint32_t), count_);
    }
    const auto hot_words = emitter_.size();
    for (const auto &translation : translation_cold_)
      emit_translation_cold(translation);
    for (const auto &straddle : run_straddles_)
      emit_run_straddle(straddle);
    for (const auto &cold : cold_) {
      set_exit_gpr_mask(cold.dirty_gprs);
      set_exit_vector_mask(cold.dirty_vectors);
      emitter_.bind(cold.label);
      /*
       * Every edge leading here is flag-neutral. Publish the translation-time
       * snapshot only on the cold arm, then let the existing terminal helpers
       * consume CpuState as their canonical source.
       */
      publish_cold_flags(cold.flags);
      assume_flags_in_memory();
      /* The cold arms that call one C++ helper and leave the block. */
      const auto terminal = [&](auto *helper) {
        emit_terminal_helper(reinterpret_cast<const void *>(helper),
                             cold.instruction, cold.pc, cold.count);
      };
      if (cold.action == ColdPath::Action::memory_fault) {
        store_pc(cold.pc);
        emitter_.return_constant(Event::memory_fault, cold.count);
      } else if (cold.action == ColdPath::Action::pair_slow) {
        terminal(&uml_arm64_jit_execute_pair);
      } else if (cold.action == ColdPath::Action::simd_structure_slow) {
        terminal(&uml_arm64_jit_execute_simd_structure);
      } else if (cold.action == ColdPath::Action::alignment_fault) {
        terminal(&uml_arm64_jit_alignment_fault);
      } else if (cold.action == ColdPath::Action::ordered_slow) {
        terminal(&uml_arm64_jit_execute_ordered);
      } else if (cold.action == ColdPath::Action::exclusive_slow) {
        /*
         * A native atomic block publishes its possible direct store for the
         * whole generated invocation. Its semantic fallback must therefore
         * unwind that publication before entering the serialized exclusive
         * helper. Otherwise two vCPUs can deadlock: the turn owner waits for
         * the other publication while that vCPU waits for the turn with its
         * own publication still active.
         */
        if (direct_memory_write_mask_ != 0U)
          emit_deferred_exclusive_slow(cold.instruction, cold.pc,
                                       cold.count);
        else
          terminal(&uml_arm64_jit_execute_exclusive);
      } else if (cold.action ==
                 ColdPath::Action::deferred_exclusive_slow) {
        emit_deferred_exclusive_slow(cold.instruction, cold.pc, cold.count);
      } else if (cold.action == ColdPath::Action::memory_slow) {
        terminal(&uml_arm64_jit_execute_memory);
      } else if (cold.action == ColdPath::Action::memory_copy_set_slow) {
        emit_memory_copy_set(cold.instruction, cold.pc, cold.count, 3U);
      } else if (cold.action == ColdPath::Action::sme_slow) {
        terminal(&uml_arm64_jit_execute_sme);
      } else if (cold.action == ColdPath::Action::sm0_spill_slow) {
        terminal(&uml_arm64_jit_execute_sm0_spill);
      } else if (cold.action == ColdPath::Action::code_changed) {
        store_pc(cold.pc);
        emitter_.return_constant(Event::continued, cold.count);
      } else if (cold.action == ColdPath::Action::exclusive_compare_mismatch) {
        emitter_.emit(0xd503'3f5fU); // clrex
        clear_local_exclusive_monitor();
        store_pc(cold.pc);
        emitter_.return_constant(Event::continued, cold.count);
      }
    }
    const auto words = emitter_.finish();
    Result result{
        .words = words,
        .executable_words = emitter_.executable_words(),
        .relocations = emitter_.relocations(),
        .relocatable = emitter_.relocatable(),
        .direct_link_refusals = direct_link_refusals(),
        .indirect_link_refusals = indirect_link_refusals(),
        .guest_instructions = count_,
        .hot_words = hot_words,
        .internal_entry_word = internal_entry_word,
        .memory_write_mask = memory_write_mask_,
        .direct_memory_write_mask = direct_memory_write_mask_,
        .accesses_fp_state = accesses_fp_state_,
        .link_target_safe = link_target_safe_,
        .native_fp = native_fp_,
        .guest_illegal = guest_illegal_,
    };
    return result;
  }

void JitCompiler::select_constrained_vector_pins(
      std::span<const std::uint32_t> instructions) noexcept {
    std::array<unsigned, 32> frequency{};
    std::array<std::size_t, 32> first_use{};
    first_use.fill(std::numeric_limits<std::size_t>::max());
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      const bool reservation_boundary =
          reservation_sensitive_allocation(instructions[index]);
      if (index != 0U && reservation_boundary)
        break;
      const auto guest = constrained_vector_source(instructions[index]);
      if (guest) {
        ++frequency[*guest];
        first_use[*guest] = std::min(first_use[*guest], index);
      }
      if (reservation_boundary ||
          obvious_translated_block_terminator(instructions[index]))
        break;
    }

    for (std::size_t pin = 0; pin < 2U; ++pin) {
      unsigned selected = 32U;
      for (unsigned guest = 0; guest < 32U; ++guest) {
        if (frequency[guest] == 0U)
          continue;
        if (selected == 32U ||
            frequency[guest] > frequency[selected] ||
            (frequency[guest] == frequency[selected] &&
             first_use[guest] < first_use[selected]))
          selected = guest;
      }
      if (selected == 32U)
        break;
      pin_constrained_vector(selected);
      frequency[selected] = 0U;
    }
  }

void JitCompiler::select_consecutive_vector_lists(
      std::span<const std::uint32_t> instructions) noexcept {
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      const bool reservation_boundary =
          reservation_sensitive_allocation(instructions[index]);
      if (index != 0U && reservation_boundary)
        break;
      if (const auto list =
              consecutive_vector_list(instructions[index]))
        static_cast<void>(
            pin_consecutive_vector_list(list->first, list->count));
      if (reservation_boundary ||
          obvious_translated_block_terminator(instructions[index]))
        break;
    }
  }

[[nodiscard]] bool
  JitCompiler::translation_supports(ArmFeature feature) const noexcept {
    if (translation_features_ != nullptr)
      return translation_features_->test(
          static_cast<std::size_t>(feature));
    return host_capabilities().has(feature);
  }

[[nodiscard]] bool
  JitCompiler::reservation_sensitive_allocation(std::uint32_t instruction) noexcept {
    const bool native_scalar_lse =
        (instruction & 0x0100'0000U) == 0U &&
        host_capabilities().has(ArmFeature::lse) &&
        ((instruction & 0x3f20'0c00U) == 0x3820'0000U ||
         (instruction & 0x3fa0'0000U) == 0x08a0'0000U);
    if (native_scalar_lse)
      return false;
    /* LDAR runs natively, and its misaligned fallback only reads: it
     * establishes no reservation and waits for no publication. */
    if ((instruction & 0x3fff'fc00U) == 0x08df'fc00U)
      return false;
    const bool scalar_atomic =
        (instruction & 0x3f20'0c00U) == 0x3820'0000U;
    const bool exclusive_or_ordered =
        (instruction & 0x3f00'0000U) == 0x0800'0000U;
    return scalar_atomic || exclusive_or_ordered;
  }

void JitCompiler::mark_memory_write() noexcept {
    if (count_ < maximum_block_instructions)
      memory_write_mask_ |= std::uint64_t{1} << count_;
  }

void JitCompiler::mark_direct_memory_write() noexcept {
    mark_memory_write();
    if (count_ < maximum_block_instructions)
      direct_memory_write_mask_ |= std::uint64_t{1} << count_;
  }

void JitCompiler::mark_direct_memory_writes(std::size_t length) noexcept {
    for (std::size_t index = 0; index < length; ++index) {
      const auto instruction_index = count_ + index;
      if (instruction_index >= maximum_block_instructions)
        break;
      const auto bit = std::uint64_t{1} << instruction_index;
      memory_write_mask_ |= bit;
      direct_memory_write_mask_ |= bit;
    }
  }

[[nodiscard]] Arm64Emitter::Label
  JitCompiler::cold_exit(std::uint32_t instruction, std::uint64_t pc,
                         ColdPath::Action action) {
    const auto label = emitter_.make_label();
    cold_.push_back({label, instruction, pc, count_ + 1, action});
    return label;
  }

void JitCompiler::track_direct_store(unsigned address, std::size_t size) {
    track_direct_store(address, size, count_);
  }

void JitCompiler::track_direct_store(unsigned address, std::size_t size,
                          std::size_t instruction_index) {
    if (!track_direct_stores_ ||
        instruction_index >= maximum_block_instructions)
      return;
    /* Every caller records after emit_translate(), so address is the
     * permission-checked host backing pointer rather than a guest VA.
     */
    const auto offset =
        instruction_index * sizeof(JitStoreRange);
    emitter_.load_x(3, 31, 32);
    emitter_.load_x(3, 3, offsetof(JitChainState, stores));
    emitter_.store_x(address, 3, offset + offsetof(JitStoreRange, address));
    emitter_.load_constant(4, size);
    emitter_.store_x(4, 3, offset + offsetof(JitStoreRange, size));
  }

void JitCompiler::track_direct_store_register(unsigned address,
                                              unsigned size,
                                              std::size_t instruction_index) {
    if (!track_direct_stores_ ||
        instruction_index >= maximum_block_instructions)
      return;
    const auto offset =
        instruction_index * sizeof(JitStoreRange);
    emitter_.load_x(3, 31, 32);
    emitter_.load_x(3, 3, offsetof(JitChainState, stores));
    emitter_.store_x(address, 3, offset + offsetof(JitStoreRange, address));
    emitter_.store_x(size, 3, offset + offsetof(JitStoreRange, size));
  }

[[nodiscard]] bool JitCompiler::emit_primary_hint(std::uint32_t instruction) {
    if ((instruction & 0xffff'f01fU) != 0xd503'201fU)
      return false;

    /*
     * CHKFEAT and GCSB are NOPs: the virtual CPU enables none of the
     * features CHKFEAT reports, so X16 keeps its value.
     */
    const unsigned immediate = (instruction >> 5) & 0x7fU;
    /*
     * These hints directly affect the host execution carrying the guest
     * block. Optional HINT-space encodings are architecturally NOP on older
     * hosts, so no runtime feature branch or fallback sequence is needed.
     */
    if (immediate == 1U || immediate == 6U || immediate == 20U ||
        immediate == 22U) {
      emitter_.emit(instruction);
      return true;
    }
    return true;
  }

void JitCompiler::mask_register(unsigned reg, std::uint64_t mask, bool wide) {
    emitter_.load_constant(9, mask);
    emitter_.emit((wide ? 0x8a00'0000U : 0x0a00'0000U) | (9U << 16) |
                  ((reg & 31U) << 5) | (reg & 31U));
  }

[[nodiscard]] bool JitCompiler::emit_system_register(std::uint32_t instruction) {
    /* FIZ, AH and NEP join the writable fields only where the virtual CPU
     * implements FEAT_AFP; the choice is fixed per translation. */
    const std::uint32_t fpcr_mask =
        translation_features_ != nullptr
            ? arm64::fpcr_writable_mask_for(*translation_features_)
            : arm64::fpcr_writable_mask_for(host_capabilities());
    const auto system = instruction & 0xffff'ffe0U;
    const unsigned transfer = instruction & 31U;
    const auto write_destination = [&](auto emit_value) {
      if (transfer == 31U)
        return;
      const auto native_destination =
          write_native_gpr(transfer, false, false, 8);
      emit_value(native_destination.host);
      commit_native_gpr(native_destination, transfer, false);
    };
    const auto copy_source_to_scratch = [&] {
      const auto native_source =
          read_native_gpr(transfer, false, 8);
      if (native_source.host != 8U)
        emitter_.move_x(8, native_source.host);
    };
    switch (system) {
    case 0xd53b'd040U: // TPIDR_EL0
      write_destination([&](unsigned destination) {
        emitter_.load_x(destination, 15, offsetof(CpuState, tls));
      });
      return true;
    case 0xd51b'd040U: {
      const auto native_source =
          read_native_gpr(transfer, false, 8);
      emitter_.store_x(native_source.host, 15, offsetof(CpuState, tls));
      return true;
    }
    case 0xd53b'd060U: // TPIDRRO_EL0
      write_destination(
          [&](unsigned destination) {
            emitter_.load_constant(destination, 0);
          });
      return true;
    case 0xd53b'00e0U: // DCZID_EL0
      write_destination(
          [&](unsigned destination) {
            emitter_.load_constant(destination, 4U);
          });
      return true;
    case 0xd53b'0020U: // CTR_EL0
      write_destination([&](unsigned destination) {
        emitter_.load_constant(destination, 0x8444'8004U);
      });
      return true;
    case 0xd53b'4400U: // FPCR
      write_destination([&](unsigned destination) {
        emitter_.load_w(destination, 15, offsetof(CpuState, fpcr));
        mask_register(destination, fpcr_mask, false);
      });
      return true;
    case 0xd51b'4400U:
      copy_source_to_scratch();
      mask_register(8, fpcr_mask, false);
      emitter_.store_w(8, 15, offsetof(CpuState, fpcr));
      mask_register(
          8, fpcr_mask & ~arm64::detail::fpcr_exception_enable_mask, false);
      emitter_.emit(0xd51b'4408U); // msr fpcr, x8
      /* A streaming translation runs with the guest's FPCR in the host's
       * (uml_arm64_jit_sme_enter installs it), so the write goes to both,
       * but keeps the guest's FPSR in CpuState and never takes the native
       * environment: XNU resets FPSR on every kernel entry while a thread
       * is in streaming mode. */
      native_fp_ = native_fp_ || !streaming_translation();
      return true;
    case 0xd53b'4420U:             // FPSR
      write_destination([&](unsigned destination) {
        if (streaming_translation())
          emitter_.load_w(destination, 15, offsetof(CpuState, fpsr));
        else
          emitter_.emit(0xd53b'4420U | destination);
        mask_register(destination, arm64::fpsr_writable_mask, false);
      });
      native_fp_ = native_fp_ || !streaming_translation();
      return true;
    case 0xd51b'4420U:
      copy_source_to_scratch();
      mask_register(8, arm64::fpsr_writable_mask, false);
      emitter_.store_w(8, 15, offsetof(CpuState, fpsr));
      if (streaming_translation())
        return true;
      emitter_.emit(0xd51b'4428U); // msr fpsr, x8
      native_fp_ = true;
      return true;
    case 0xd53b'4200U: // NZCV
      write_destination([&](unsigned destination) {
        read_flags(destination);
      });
      return true;
    case 0xd51b'4200U:
      copy_source_to_scratch();
      mask_register(8, 0xf000'0000U, false);
      emitter_.store_w(8, 15, offsetof(CpuState, nzcv));
      /* The guest write supersedes any formerly resident host value. */
      assume_flags_in_memory();
      return true;
    /* DIT is the register view of PSTATE.DIT that the MSR immediate form
     * writes: read is Zeros(39):PSTATE.DIT:Zeros(24), write takes X[t]<24>.
     * Both directions are gated on the same feature as that immediate form so
     * a guest can always read back what it just set under this engine. */
    case 0xd53b'42a0U: // DIT
      if (!translation_supports(ArmFeature::dit))
        return false;
      write_destination([&](unsigned destination) {
        emitter_.load_b(destination, 15, offsetof(CpuState, pstate_dit));
        emitter_.ubfm(destination, destination, 40, 39); // lsl #24
      });
      return true;
    case 0xd51b'42a0U:
      if (!translation_supports(ArmFeature::dit))
        return false;
      copy_source_to_scratch();
      emitter_.ubfm(8, 8, 24, 24); // ubfx #24, #1
      emitter_.store_b(8, 15, offsetof(CpuState, pstate_dit));
      return true;
    case 0xd53b'e000U: // CNTFRQ_EL0
      write_destination([&](unsigned destination) {
        emitter_.emit(0xd53b'e000U | destination);
      });
      return true;
    case 0xd53b'e040U: // CNTVCT_EL0
      write_destination([&](unsigned destination) {
        emitter_.emit(0xd53b'e040U | destination);
      });
      return true;
    /* FEAT_ECV's self-synchronized view of the same count, for which a
     * guest need not order earlier loads with an ISB. Only a host that
     * implements it can execute the register. CNTPCTSS_EL0 stays UNDEFINED
     * with CNTPCT_EL0: Linux gives EL0 no physical count. */
    case 0xd53b'e0c0U: // CNTVCTSS_EL0
      if (!translation_supports(ArmFeature::ecv))
        return false;
      write_destination([&](unsigned destination) {
        emitter_.emit(0xd53b'e0c0U | destination);
      });
      return true;
    default:
      break;
    }
    /* Behind the named EL0 registers so ordinary translation still leaves this
     * function through the same switch. An emulated identification register
     * holds one value for the whole process, so the read compiles to the
     * constant itself rather than a call back into the engine. */
    if (!detail::emulated_id_register(instruction))
      return false;
    write_destination([&](unsigned destination) {
      emitter_.load_constant(
          destination, detail::emulated_id_register_value(instruction));
    });
    return true;
  }

void JitCompiler::emit_cache_zero(std::uint32_t instruction, std::uint64_t pc) {
    mark_direct_memory_write();
    const unsigned source = instruction & 31U;
    load_gpr(8, source, false);
    emit_untag_data_address(8);
    mask_register(8, ~std::uint64_t{63}, true);
    const auto fault =
        cold_exit(instruction, pc, ColdPath::Action::memory_fault);
    /* The advertised 64-byte block size divides every supported guest
     * page, so an aligned DC ZVA block needs exactly one translation.
     */
    emit_translate(8, Access::write, fault, fault, 1);
    track_direct_store(8, 64);
    for (unsigned offset = 0; offset < 64; offset += 16)
      emitter_.emit(0xa900'0000U | ((offset / 8U) << 15) | (31U << 10) |
                    (8U << 5) | 31U);
  }

/*
 * After an ISB, a context synchronization point: when the address space's
 * code-change sequence has moved since the executor validated the
 * translations this run reaches (CpuState::code_sequence_seen), another vCPU
 * changed and invalidated code, and a linked successor may be stale. Leave at
 * the fall-through, which the executor then fetches anew. The guard is
 * flag-neutral, so guest flags may stay live in the host's NZCV.
 */
void JitCompiler::emit_code_change_check(std::uint32_t instruction,
                                         std::uint64_t pc) {
    emitter_.load_x(8, 15, offsetof(CpuState, code_sequence));
    emitter_.load_x(9, 15, offsetof(CpuState, code_sequence_seen));
    emitter_.load_x(8, 8, 0);
    emitter_.emit(0xcb09'0108U); // sub x8, x8, x9
    emitter_.compare_zero(
        cold_exit(instruction, pc + sizeof(std::uint32_t),
                  ColdPath::Action::code_changed),
        8, true, true);
  }

void JitCompiler::emit_code_synchronize(std::uint32_t instruction, std::uint64_t pc,
                             std::size_t count) {
    /*
     * This instruction is an architectural translated-code boundary. Publish
     * the complete native register cache before the cold helper, then return
     * to C++ at the known fallthrough so the changed code sequence is
     * observed before another translated instruction executes.
     */
    store_pc(pc + sizeof(std::uint32_t));
    spill_gprs_for_terminal_call();
    const unsigned source = instruction & 31U;
    if (source == 31U)
      emitter_.move_x(0, 31);
    else {
      emitter_.load_x(0, 15, register_offset(source, false));
      emit_untag_data_address(0);
    }
    emitter_.call(
        reinterpret_cast<const void *>(&uml_arm64_guest_code_synchronize));
    emitter_.return_constant(Event::continued, count);
  }

void JitCompiler::emit_deferred_exclusive_slow(std::uint32_t instruction,
                                    std::uint64_t pc,
                                    std::size_t count) {
    store_pc(pc);
    emitter_.load_constant(0, instruction);
    emitter_.store_w(
        0, 19, offsetof(JitChainState, deferred_exclusive_instruction));
    /*
     * The instruction at pc has not executed. return_constant() flushes the
     * live guest prefix before unwinding the one outer generated frame.
     */
    emitter_.return_constant(Event::continued,
                             count == 0U ? 0U : count - 1U);
  }

void JitCompiler::emit_memory_copy_set(std::uint32_t instruction,
                                       std::uint64_t pc, std::size_t count,
                                       unsigned stages) {
    store_pc(pc);
    spill_gprs_for_terminal_call();
    emitter_.move_x(0, 15);
    emitter_.move_x(1, 14);
    emitter_.load_constant(2, instruction);
    emitter_.load_constant(3, stages);
    emitter_.call(
        reinterpret_cast<const void *>(&uml_arm64_jit_execute_mops));
    emitter_.return_dynamic(count);
  }

namespace {

/* LDP/STP of two Q registers, signed offset. */
[[nodiscard]] constexpr std::uint32_t pair_q(bool load, unsigned first,
                                             unsigned second, unsigned base,
                                             int offset) noexcept {
  return (load ? 0xad40'0000U : 0xad00'0000U) |
         ((static_cast<std::uint32_t>(offset / 16) & 0x7fU) << 15) |
         (second << 10) | (base << 5) | first;
}

/* LDUR/STUR of one SIMD&FP register of 1 << size_log2 bytes. */
[[nodiscard]] constexpr std::uint32_t single_simd(bool load,
                                                  unsigned size_log2,
                                                  unsigned reg, unsigned base,
                                                  int offset) noexcept {
  const std::uint32_t size = size_log2 == 4U ? 0U : size_log2;
  const std::uint32_t opc =
      (size_log2 == 4U ? 2U : 0U) | (load ? 1U : 0U);
  return 0x3c00'0000U | (size << 30) | (opc << 22) |
         ((static_cast<std::uint32_t>(offset) & 0x1ffU) << 12) |
         (base << 5) | reg;
}

[[nodiscard]] constexpr std::uint32_t add_x(unsigned destination,
                                            unsigned left,
                                            unsigned right) noexcept {
  return 0x8b00'0000U | (right << 16) | (left << 5) | destination;
}

[[nodiscard]] constexpr std::uint32_t subtract_x(unsigned destination,
                                                 unsigned left,
                                                 unsigned right) noexcept {
  return 0xcb00'0000U | (right << 16) | (left << 5) | destination;
}

/* cmp Xn, #imm, with imm below 4096. */
[[nodiscard]] constexpr std::uint32_t compare_immediate(
    unsigned reg, unsigned immediate) noexcept {
  return 0xf100'001fU | (immediate << 10) | (reg << 5);
}

} // namespace

void JitCompiler::emit_small_copy_set(bool set, unsigned source_start,
                                      unsigned source_end,
                                      unsigned destination_start,
                                      unsigned destination_end,
                                      unsigned remaining,
                                      Arm64Emitter::Label done) {
    /*
     * 1 to 63 bytes. Each size class moves the first and the last bytes of
     * its width, which together cover every count in the class, and loads
     * both before it stores either: one block, in either direction.
     */
    std::array<Arm64Emitter::Label, 5> classes{};
    for (auto &label : classes)
      label = emitter_.make_label();
    for (std::size_t index = 0; index < classes.size(); ++index) {
      emitter_.emit_internal_flag_clobber(
          compare_immediate(remaining, 32U >> index));
      emitter_.branch_condition(classes[index], 2); // b.hs
    }
    if (!set)
      emitter_.emit(single_simd(true, 0, 0, source_start, 0));
    emitter_.emit(single_simd(false, 0, 0, destination_start, 0));
    emitter_.branch(done);
    for (std::size_t index = 0; index < classes.size(); ++index) {
      emitter_.bind(classes[index]);
      if (index == 0U) {
        if (set) {
          emitter_.emit(pair_q(false, 0, 0, destination_start, 0));
          emitter_.emit(pair_q(false, 0, 0, destination_end, -32));
        } else {
          emitter_.emit(pair_q(true, 0, 1, source_start, 0));
          emitter_.emit(pair_q(true, 2, 3, source_end, -32));
          emitter_.emit(pair_q(false, 0, 1, destination_start, 0));
          emitter_.emit(pair_q(false, 2, 3, destination_end, -32));
        }
      } else {
        const unsigned size_log2 = 5U - static_cast<unsigned>(index);
        const int width = 1 << size_log2;
        if (set) {
          emitter_.emit(single_simd(false, size_log2, 0, destination_start, 0));
          emitter_.emit(
              single_simd(false, size_log2, 0, destination_end, -width));
        } else {
          emitter_.emit(single_simd(true, size_log2, 0, source_start, 0));
          emitter_.emit(single_simd(true, size_log2, 1, source_end, -width));
          emitter_.emit(single_simd(false, size_log2, 0, destination_start, 0));
          emitter_.emit(
              single_simd(false, size_log2, 1, destination_end, -width));
        }
      }
      emitter_.branch(done);
    }
  }

[[nodiscard]] std::size_t JitCompiler::try_emit_memory_copy_set_sequence(
      std::span<const std::uint32_t> instructions, std::uint64_t pc) {
    /*
     * A compiler emits each operation as its prologue, main and epilogue in
     * sequence, and glibc's memcpy, memmove and memset are exactly that. When
     * the three words agree in everything but their stage, an untagged
     * transfer that stays inside one destination page and one source page
     * runs inline, with the shared helper's choices: 64-byte blocks, each
     * loaded completely before it is stored, from the first byte in the
     * copy's direction, and a CPY whose source starts below an overlapping
     * destination running backward. Where the helper would move a whole
     * region with one memmove, the blocks give the same bytes. Any other
     * transfer, and a TLB miss, takes the helper for the whole sequence from
     * its prologue. Any other arrangement executes stage by stage through
     * emit_instruction().
     */
    if (instructions.size() < 3U)
      return 0;
    const auto prologue = instructions[0];
    const auto mops = arm64::detail::decode_mops(prologue);
    if (!mops.recognized || !mops.valid || mops.stage != 0U ||
        instructions[1] !=
            arm64::detail::mops_stage_word(prologue, mops.family, 1U) ||
        instructions[2] !=
            arm64::detail::mops_stage_word(prologue, mops.family, 2U))
      return 0;
    using arm64::detail::MopsFamily;
    const bool set = mops.family == MopsFamily::set;
    const std::size_t epilogue = count_ + 2U;
    if (epilogue < maximum_block_instructions) {
      memory_write_mask_ |= std::uint64_t{1} << epilogue;
      direct_memory_write_mask_ |= std::uint64_t{1} << epilogue;
    }

    /* The guards compare, so publish the guest NZCV first; the helper arm
     * then starts from CpuState with every operand unmodified. */
    spill_flags_for_internal_clobber();
    const auto destination = read_native_gpr(mops.destination, false, 11);
    const auto count = read_native_gpr(mops.count, false, 10);
    const auto source = read_native_gpr(mops.source, false, 8);
    if (destination.host != 11U)
      emitter_.move_x(11, destination.host);
    if (count.host != 10U)
      emitter_.move_x(10, count.host);
    if (!set && source.host != 8U)
      emitter_.move_x(8, source.host);
    const auto slow =
        cold_exit(prologue, pc, ColdPath::Action::memory_copy_set_slow);
    const auto forward_done = emitter_.make_label();
    const auto update = emitter_.make_label();
    emitter_.compare_zero(forward_done, 10, true, false);
    static_assert(page_size % 4096U == 0U && page_size / 4096U < 4096U);
    constexpr auto page_compare = 0xf140'001fU |
        static_cast<std::uint32_t>((page_size / 4096U) << 10); // cmp xN, #page
    /* The count bound comes first, so the page sums below cannot wrap. */
    emitter_.emit_internal_flag_clobber(page_compare | (10U << 5));
    emitter_.branch_condition(slow, 8); // b.hi
    /* A tagged address takes the helper, so these checks and the direction
     * test see the 56-bit addresses the prologue compares. */
    if (set) {
      emitter_.ubfm(0, 11, 56, 63);
    } else {
      emitter_.emit(0xaa0b'0100U); // orr x0, x8, x11
      emitter_.ubfm(0, 0, 56, 63);
    }
    emitter_.compare_zero(slow, 0, true, true);
    for (const unsigned address : {11U, 8U}) {
      if (set && address == 8U)
        break;
      emitter_.ubfm(0, address, 0, page_shift - 1U);
      emitter_.emit(add_x(0, 0, 10));
      emitter_.emit_internal_flag_clobber(page_compare);
      emitter_.branch_condition(slow, 8); // b.hi
    }
    const auto backward = emitter_.make_label();
    if (mops.family == MopsFamily::copy_general) {
      /* A destination above an overlapping source copies backward. */
      const auto forward = emitter_.make_label();
      emitter_.emit(subtract_x(0, 11, 8));
      emitter_.emit_internal_flag_clobber(0xeb0a'001fU); // cmp x0, x10
      emitter_.branch_condition(forward, 2);             // b.hs
      emitter_.compare_zero(backward, 0, true, true);
      emitter_.bind(forward);
    }

    /* Forward: x11 and x8 advance, x9 counts down. */
    emit_translate(11, Access::write, slow, slow, 1, false);
    if (!set)
      emit_translate(8, Access::read, slow, slow, 1, false);
    track_direct_store_register(11, 10, epilogue);
    if (set)
      emitter_.emit(0x4e01'0c00U | (source.host << 5)); // dup v0.16b, ws
    emitter_.move_x(9, 10);
    {
      const auto loop = emitter_.make_label();
      const auto tail = emitter_.make_label();
      emitter_.emit_internal_flag_clobber(compare_immediate(9, 64));
      emitter_.branch_condition(tail, 3); // b.lo
      emitter_.bind(loop);
      if (set) {
        emitter_.emit(pair_q(false, 0, 0, 11, 0));
        emitter_.emit(pair_q(false, 0, 0, 11, 32));
      } else {
        emitter_.emit(pair_q(true, 0, 1, 8, 0));
        emitter_.emit(pair_q(true, 2, 3, 8, 32));
        emitter_.emit(pair_q(false, 0, 1, 11, 0));
        emitter_.emit(pair_q(false, 2, 3, 11, 32));
        emitter_.add_immediate(8, 8, 64);
      }
      emitter_.add_immediate(11, 11, 64);
      emitter_.subtract_immediate(9, 9, 64);
      emitter_.emit_internal_flag_clobber(compare_immediate(9, 64));
      emitter_.branch_condition(loop, 2); // b.hs
      emitter_.compare_zero(forward_done, 9, true, false);
      emitter_.bind(tail);
      if (!set)
        emitter_.emit(add_x(2, 8, 9));
      emitter_.emit(add_x(0, 11, 9));
      emit_small_copy_set(set, 8, 2, 11, 0, 9, forward_done);
    }
    emitter_.bind(forward_done);
    /* The addresses advance by the count; NZCV keeps the forward format. */
    emitter_.move_x(9, 10);
    emitter_.load_constant(0, 0x2000'0000U);

    if (mops.family == MopsFamily::copy_general) {
      emitter_.branch(update);
      /* Backward: x1 and x2 retreat from the ends, x9 counts down. */
      emitter_.bind(backward);
      const auto backward_done = emitter_.make_label();
      emit_translate(11, Access::write, slow, slow, 1, false);
      emit_translate(8, Access::read, slow, slow, 1, false);
      track_direct_store_register(11, 10, epilogue);
      emitter_.emit(add_x(1, 11, 10));
      emitter_.emit(add_x(2, 8, 10));
      emitter_.move_x(9, 10);
      const auto loop = emitter_.make_label();
      const auto tail = emitter_.make_label();
      emitter_.emit_internal_flag_clobber(compare_immediate(9, 64));
      emitter_.branch_condition(tail, 3); // b.lo
      emitter_.bind(loop);
      emitter_.emit(pair_q(true, 0, 1, 2, -64));
      emitter_.emit(pair_q(true, 2, 3, 2, -32));
      emitter_.emit(pair_q(false, 0, 1, 1, -64));
      emitter_.emit(pair_q(false, 2, 3, 1, -32));
      emitter_.subtract_immediate(2, 2, 64);
      emitter_.subtract_immediate(1, 1, 64);
      emitter_.subtract_immediate(9, 9, 64);
      emitter_.emit_internal_flag_clobber(compare_immediate(9, 64));
      emitter_.branch_condition(loop, 2); // b.hs
      emitter_.compare_zero(backward_done, 9, true, false);
      emitter_.bind(tail);
      emitter_.emit(subtract_x(8, 2, 9));
      emitter_.emit(subtract_x(11, 1, 9));
      emit_small_copy_set(false, 8, 2, 11, 1, 9, backward_done);
      emitter_.bind(backward_done);
      /* The prologue moved the addresses to the ends and the epilogue back
       * to the start; NZCV keeps the backward format (N and C set). */
      emitter_.move_x(9, 31);
      emitter_.load_constant(0, 0xa000'0000U);
    }
    emitter_.bind(update);

    /* The epilogue's result: the addresses advance by x9, the count is
     * zero, and NZCV is x0. */
    if (!set) {
      const auto advanced = write_native_gpr(mops.source, false, true, 10);
      emitter_.emit(add_x(advanced.host, advanced.host, 9));
      commit_native_gpr(advanced, mops.source, false);
    }
    const auto advanced = write_native_gpr(mops.destination, false, true, 10);
    emitter_.emit(add_x(advanced.host, advanced.host, 9));
    commit_native_gpr(advanced, mops.destination, false);
    const auto remaining = write_native_gpr(mops.count, false, false, 10);
    emitter_.emit(0xaa1f'03e0U | remaining.host); // mov xN, #0
    commit_native_gpr(remaining, mops.count, false);
    emitter_.emit(0xd51b'4200U); // msr nzcv, x0
    store_flags();
    return 3;
  }

void JitCompiler::emit_simd_modified_immediate(std::uint32_t instruction,
                                    bool reads_destination) {
    const unsigned destination = instruction & 31U;
    const auto native_destination =
        write_native_vector(destination, reads_destination, 0);
    emitter_.emit((instruction & ~31U) | native_destination.host);
    commit_native_vector(native_destination, destination);
  }

void JitCompiler::emit_simd_duplicate_general(std::uint32_t instruction) {
    const unsigned source = (instruction >> 5) & 31U;
    const unsigned destination = instruction & 31U;
    const auto native_source = read_native_gpr(source, false, 8);
    const auto native_destination =
        write_native_vector(destination, false, 0);
    emitter_.emit((instruction & ~((31U << 5) | 31U)) |
                  (native_source.host << 5) | native_destination.host);
    commit_native_vector(native_destination, destination);
  }

[[nodiscard]] bool JitCompiler::emit_simd_insert(std::uint32_t instruction) {
    if (!host_capabilities().advanced_simd)
      return false;
    const bool from_general = (instruction & 0xffe0'fc00U) == 0x4e00'1c00U;
    const bool from_element = (instruction & 0xffe0'8400U) == 0x6e00'0400U;
    if (!from_general && !from_element)
      return false;
    if (!arm64::detail::decode_simd_lane_index((instruction >> 16) & 31U))
      return false;
    if (from_element) {
      emit_native_vector_unary(instruction, true);
      return true;
    }
    const unsigned destination = instruction & 31U;
    const unsigned source = (instruction >> 5) & 31U;
    const auto native_source = read_native_gpr(source, false, 8);
    const auto native_destination =
        write_native_vector(destination, true, 0);
    emitter_.emit((instruction & ~0x3ffU) | (native_source.host << 5) |
                  native_destination.host);
    commit_native_vector(native_destination, destination);
    return true;
  }

[[nodiscard]] bool JitCompiler::emit_simd_duplicate_element(std::uint32_t instruction) {
    if (!host_capabilities().advanced_simd)
      return false;
    const bool vector = (instruction & 0xbfe0'fc00U) == 0x0e00'0400U;
    const bool scalar = (instruction & 0xffe0'fc00U) == 0x5e00'0400U;
    if (!vector && !scalar)
      return false;
    const auto lane =
        arm64::detail::decode_simd_lane_index((instruction >> 16) & 31U);
    const bool q = (instruction & 0x4000'0000U) != 0;
    if (!lane || (vector && !q && lane->element_shift == 3U))
      return false;
    emit_native_vector_unary(instruction, false);
    return true;
  }

[[nodiscard]] bool JitCompiler::emit_simd_bit_unary(std::uint32_t instruction) {
    if (!host_capabilities().advanced_simd)
      return false;
    const auto operation = instruction & 0x9f3f'fc00U;
    if (operation != 0x0e20'5800U && operation != 0x0e20'0800U &&
        operation != 0x0e20'1800U)
      return false;
    const unsigned opcode = (instruction >> 10) & 63U;
    const bool alternate = (instruction & 0x2000'0000U) != 0;
    const unsigned size = (instruction >> 22) & 3U;
    const bool valid =
        opcode == 22U
            ? ((!alternate && size == 0U) || (alternate && size <= 1U))
        : opcode == 2U ? ((!alternate && size < 3U) || (alternate && size < 2U))
                       : opcode == 6U && !alternate && size == 0U;
    if (!valid)
      return false;
    emit_native_vector_unary(instruction, false);
    return true;
  }

[[nodiscard]] bool JitCompiler::emit_simd_integer_reduce(std::uint32_t instruction) {
    if (!host_capabilities().advanced_simd ||
        (instruction & 0x9f3e'0c00U) != 0x0e30'0800U)
      return false;
    const unsigned operation = (instruction >> 12) & 31U;
    const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
    if (operation != 3U && operation != 10U && operation != 26U &&
        (operation != 27U || unsigned_group))
      return false;
    const bool q = (instruction & 0x4000'0000U) != 0;
    const unsigned size = (instruction >> 22) & 3U;
    if (size > 2U || (!q && size == 2U))
      return false;
    emit_native_vector_unary(instruction, false);
    return true;
  }

[[nodiscard]] bool JitCompiler::emit_simd_scalar_pairwise_add(std::uint32_t instruction) {
    if (!host_capabilities().advanced_simd ||
        (instruction & 0xffff'fc00U) != 0x5ef1'b800U)
      return false;
    emit_native_vector_unary(instruction, false);
    return true;
  }

[[nodiscard]] bool JitCompiler::emit_simd_shift_immediate(std::uint32_t instruction) {
    if (!host_capabilities().advanced_simd)
      return false;
    const bool vector = (instruction & 0x9f00'0400U) == 0x0f00'0400U;
    const bool scalar = (instruction & 0xdf00'0400U) == 0x5f00'0400U;
    if (!vector && !scalar)
      return false;
    const unsigned opcode = (instruction >> 11) & 31U;
    const bool alternate = (instruction & 0x2000'0000U) != 0;
    if (opcode != 0U && opcode != 2U && opcode != 4U && opcode != 6U &&
        opcode != 8U && opcode != 10U && opcode != 12U && opcode != 14U &&
        opcode != 16U && opcode != 17U && opcode != 18U && opcode != 19U)
      return false;
    if ((opcode == 8U || opcode == 12U) && !alternate)
      return false;
    const unsigned encoded = (instruction >> 16) & 0x7fU;
    if (encoded < 8U)
      return false;
    const unsigned lane_bits = std::bit_floor(encoded);
    if (lane_bits > 64U)
      return false;
    const bool q = (instruction & 0x4000'0000U) != 0;
    const bool narrow = opcode >= 16U;
    const bool saturating_left =
        opcode == 12U || opcode == 14U;
    if (narrow) {
      if (encoded >= 64U || (scalar && opcode <= 17U && !alternate))
        return false;
    } else {
      if (scalar && !saturating_left && lane_bits != 64U)
        return false;
      const unsigned vector_bits = scalar ? lane_bits : q ? 128U : 64U;
      if (lane_bits > vector_bits || (!scalar && lane_bits == 64U && !q))
        return false;
      const bool shift_left =
          opcode == 10U || opcode == 12U || opcode == 14U;
      const unsigned shift =
          shift_left ? encoded - lane_bits : 2U * lane_bits - encoded;
      if ((shift_left && shift >= lane_bits) ||
          (!shift_left && (shift == 0U || shift > lane_bits)))
        return false;
    }
    const bool reads_destination =
        opcode == 2U || opcode == 6U || opcode == 8U ||
        (opcode == 10U && alternate) || (narrow && !scalar && q);
    emit_native_vector_unary(instruction, reads_destination);
    const bool saturating =
        saturating_left || opcode >= 18U ||
        ((opcode == 16U || opcode == 17U) && alternate);
    if (saturating)
      native_fp_ = true;
    return true;
  }

[[nodiscard]] bool JitCompiler::emit_simd_shift_left_long(std::uint32_t instruction) {
    if (!host_capabilities().advanced_simd ||
        (instruction & 0x9f80'fc00U) != 0x0f00'a400U)
      return false;
    const unsigned encoded = (instruction >> 16) & 0x7fU;
    const unsigned source_bits = std::bit_floor(encoded);
    if (source_bits < 8U || source_bits > 32U ||
        encoded - source_bits >= source_bits)
      return false;
    emit_native_vector_unary(instruction, false);
    return true;
  }

[[nodiscard]] bool JitCompiler::emit_simd_general_move(std::uint32_t instruction) {
    const auto opcode = instruction & 0xffff'fc00U;
    bool vector_to_gpr = (instruction & 0xbfe0'fc00U) == 0x0e00'3c00U ||
                         (instruction & 0xbfe0'fc00U) == 0x0e00'2c00U;
    if (vector_to_gpr) {
      const bool signed_move = (instruction & 0x1000U) == 0;
      const auto lane =
          arm64::detail::decode_simd_lane_index((instruction >> 16) & 31U);
      const bool wide = (instruction & 0x4000'0000U) != 0;
      if (!lane || (signed_move ? lane->element_shift >= (wide ? 3U : 2U)
                                : (wide ? lane->element_shift != 3U
                                        : lane->element_shift >= 3U)))
        return false;
    } else {
      switch (opcode) {
      case 0x1ee6'0000U:
      case 0x9ee6'0000U:
      case 0x9e66'0000U:
      case 0x1e26'0000U:
      case 0x9eae'0000U:
        vector_to_gpr = true;
        break;
      case 0x9e67'0000U:
      case 0x1ee7'0000U:
      case 0x9ee7'0000U:
      case 0x1e27'0000U:
      case 0x9eaf'0000U:
        break;
      default:
        return false;
      }
      if ((opcode == 0x1ee6'0000U || opcode == 0x9ee6'0000U ||
           opcode == 0x1ee7'0000U || opcode == 0x9ee7'0000U) &&
          !host_capabilities().fp16)
        return false;
    }
    const unsigned source = (instruction >> 5) & 31U;
    const unsigned destination = instruction & 31U;
    auto native = instruction & ~((31U << 5) | 31U);
    if (vector_to_gpr) {
      const auto native_source = read_native_vector(source, 0);
      const auto native_destination =
          write_native_gpr(destination, false, false, 8);
      native |= (native_source.host << 5) | native_destination.host;
      emitter_.emit(native);
      commit_native_gpr(native_destination, destination, false);
    } else {
      const auto native_source = read_native_gpr(source, false, 8);
      const bool reads_destination = opcode == 0x9eaf'0000U;
      const auto native_destination =
          write_native_vector(destination, reads_destination, 0);
      native |= (native_source.host << 5) | native_destination.host;
      emitter_.emit(native);
      commit_native_vector(native_destination, destination);
    }
    return true;
  }

} // namespace uml::cpu::arm64
