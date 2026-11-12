// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_emitter.hpp"

#include "jit_sme_transition.hpp"

namespace uml::cpu::arm64 {

Arm64Emitter::Arm64Emitter(std::size_t literal_range_words) noexcept
      : literal_range_words_{
            std::min(literal_range_words, std::size_t{1} << 18U)} {}

[[nodiscard]] Arm64Emitter::Label Arm64Emitter::make_label() {
    labels_.push_back(unbound);
    return labels_.size() - 1;
  }

void Arm64Emitter::bind(Label label) { labels_.at(label) = words_.size(); }

void Arm64Emitter::emit(std::uint32_t instruction) { words_.push_back(instruction); }

void Arm64Emitter::attach_exit_flush(void *context, ExitFlush flush) noexcept {
    exit_context_ = context;
    exit_flush_ = flush;
  }

void Arm64Emitter::attach_flag_clobber(void *context, FlagClobber clobber) noexcept {
    flag_context_ = context;
    flag_clobber_ = clobber;
  }

[[nodiscard]] std::size_t Arm64Emitter::size() const noexcept { return words_.size(); }

[[nodiscard]] std::uint32_t
Arm64Emitter::word(std::size_t index) const noexcept {
  return index < words_.size() ? words_[index] : 0U;
}

[[nodiscard]] bool Arm64Emitter::patched(std::size_t begin,
                                         std::size_t end) const noexcept {
  for (const auto &entry : patches_)
    if (entry.word >= begin && entry.word < end)
      return true;
  for (const auto &entry : literal_patches_)
    if (entry.word >= begin && entry.word < end)
      return true;
  return false;
}

void Arm64Emitter::branch(Label label) {
    patch(label, BranchKind::unconditional, 0x1400'0000U);
  }

void Arm64Emitter::branch_condition(Label label, unsigned condition) {
    patch(label, BranchKind::conditional, 0x5400'0000U | (condition & 15U));
  }

void Arm64Emitter::compare_zero(Label label, unsigned reg, bool is_64_bit, bool nonzero) {
    const auto base = is_64_bit ? (nonzero ? 0xb500'0000U : 0xb400'0000U)
                                : (nonzero ? 0x3500'0000U : 0x3400'0000U);
    patch(label,
          nonzero ? BranchKind::compare_nonzero : BranchKind::compare_zero,
          base | (reg & 31U));
  }

void Arm64Emitter::test_zero(Label label, unsigned reg, unsigned bit, bool nonzero) {
    const auto base = nonzero ? 0x3700'0000U : 0x3600'0000U;
    const auto encoded_bit = ((bit & 32U) << 26) | ((bit & 31U) << 19);
    patch(label, nonzero ? BranchKind::test_nonzero : BranchKind::test_zero,
          base | encoded_bit | (reg & 31U));
  }

void Arm64Emitter::load_x(unsigned destination, unsigned base, std::size_t offset) {
    emit(0xf940'0000U | (static_cast<std::uint32_t>(offset / 8U) << 10) |
         ((base & 31U) << 5) | (destination & 31U));
  }

void Arm64Emitter::load_pair_x(unsigned first, unsigned second, unsigned base,
                   std::ptrdiff_t offset) {
    if ((offset % 8) != 0 || offset < -512 || offset > 504) {
      /* Invalid emitter metadata is a translation failure, never a reason to
       * terminate the host. finish() returns an empty program and the JIT
       * executor runs the instruction as its own one-instruction
       * translation. */
      valid_ = false;
      return;
    }
    const auto scaled =
        static_cast<std::uint32_t>(offset / 8) & 0x7fU;
    emit(0xa940'0000U | (scaled << 15U) | ((second & 31U) << 10U) |
         ((base & 31U) << 5U) | (first & 31U));
  }

void Arm64Emitter::store_pair_x(unsigned first, unsigned second,
                                unsigned base, std::ptrdiff_t offset) {
    if ((offset % 8) != 0 || offset < -512 || offset > 504) {
      valid_ = false;
      return;
    }
    const auto scaled =
        static_cast<std::uint32_t>(offset / 8) & 0x7fU;
    emit(0xa900'0000U | (scaled << 15U) | ((second & 31U) << 10U) |
         ((base & 31U) << 5U) | (first & 31U));
  }

void Arm64Emitter::store_x(unsigned source, unsigned base, std::size_t offset) {
    emit(0xf900'0000U | (static_cast<std::uint32_t>(offset / 8U) << 10) |
         ((base & 31U) << 5) | (source & 31U));
  }

void Arm64Emitter::load_w(unsigned destination, unsigned base, std::size_t offset) {
    emit(0xb940'0000U | (static_cast<std::uint32_t>(offset / 4U) << 10) |
         ((base & 31U) << 5) | (destination & 31U));
  }

void Arm64Emitter::store_w(unsigned source, unsigned base, std::size_t offset) {
    emit(0xb900'0000U | (static_cast<std::uint32_t>(offset / 4U) << 10) |
         ((base & 31U) << 5) | (source & 31U));
  }

void Arm64Emitter::load_b(unsigned destination, unsigned base, std::size_t offset) {
    emit(0x3940'0000U | (static_cast<std::uint32_t>(offset) << 10) |
         ((base & 31U) << 5) | (destination & 31U));
  }

void Arm64Emitter::store_b(unsigned source, unsigned base, std::size_t offset) {
    emit(0x3900'0000U | (static_cast<std::uint32_t>(offset) << 10) |
         ((base & 31U) << 5) | (source & 31U));
  }

void Arm64Emitter::load_q(unsigned destination, unsigned base, std::size_t offset) {
    emit(0x3dc0'0000U | (static_cast<std::uint32_t>(offset / 16U) << 10) |
         ((base & 31U) << 5) | (destination & 31U));
  }

void Arm64Emitter::store_q(unsigned source, unsigned base, std::size_t offset) {
    emit(0x3d80'0000U | (static_cast<std::uint32_t>(offset / 16U) << 10) |
         ((base & 31U) << 5) | (source & 31U));
  }

void Arm64Emitter::move_q(unsigned destination, unsigned source) {
    emit(0x4ea0'1c00U | ((source & 31U) << 16) |
         ((source & 31U) << 5) | (destination & 31U));
  }

void Arm64Emitter::move_x(unsigned destination, unsigned source) {
    emit(0xaa00'03e0U | ((source & 31U) << 16) | (destination & 31U));
  }

void Arm64Emitter::load_constant(unsigned destination, std::uint64_t value) {
    unsigned move_zero_words = 0;
    unsigned move_not_words = 0;
    for (unsigned half = 0; half < 4; ++half) {
      const auto part = static_cast<std::uint16_t>(value >> (half * 16U));
      move_zero_words += part != 0;
      move_not_words += part != 0xffffU;
    }
    if (move_zero_words == 0)
      move_zero_words = 1;
    if (move_not_words == 0)
      move_not_words = 1;
    /*
     * MOVZ leaves every other half zero; MOVN leaves every other half one.
     * Select MOVN only when that default removes at least one instruction.
     * Ties deliberately retain the stable MOVZ-first shape.
     */
    const bool inverted = move_not_words < move_zero_words;
    bool emitted = false;
    for (unsigned half = 0; half < 4; ++half) {
      const auto part = static_cast<std::uint16_t>(value >> (half * 16U));
      if (part == (inverted ? 0xffffU : 0U))
        continue;
      const auto immediate =
          inverted && !emitted ? static_cast<std::uint16_t>(~part) : part;
      const auto opcode = emitted ? 0xf280'0000U
                                  : inverted ? 0x9280'0000U : 0xd280'0000U;
      emit(opcode | (half << 21) |
           (static_cast<std::uint32_t>(immediate) << 5) |
           (destination & 31U));
      emitted = true;
    }
    if (!emitted)
      emit((inverted ? 0x9280'0000U : 0xd280'0000U) |
           (destination & 31U));
  }

void Arm64Emitter::load_literal_constant(unsigned destination, std::uint64_t value) {
    /*
     * This placeholder becomes one LDR (literal) after every executable
     * path has been emitted. Translation-resolved guest constants and
     * host-owned link pointers share the immutable tail pool. If an unusually
     * large translation puts the data outside imm19 reach, finish() changes
     * this slot to an immediate branch through a cold MOV-wide
     * materialization stub.
     */
    literal_patches_.push_back(
        {words_.size(), value, destination & 31U});
    emit(0x5800'0000U | (destination & 31U));
  }

void Arm64Emitter::load_stable_pointer(unsigned destination, std::uintptr_t value) {
    load_literal_constant(destination, static_cast<std::uint64_t>(value));
  }

void Arm64Emitter::load_relocatable_pointer(unsigned destination,
                                            std::uintptr_t value,
                                            JitRelocationKind kind,
                                            std::uint64_t key) {
    load_literal_constant(destination, static_cast<std::uint64_t>(value));
    auto &patch = literal_patches_.back();
    patch.relocatable = true;
    patch.kind = kind;
    patch.key = key;
  }

void Arm64Emitter::add_immediate(unsigned destination, unsigned source, unsigned immediate,
                     bool shift_12) {
    emit(0x9100'0000U | (shift_12 ? 1U << 22 : 0U) |
         ((immediate & 0xfffU) << 10) | ((source & 31U) << 5) |
         (destination & 31U));
  }

void Arm64Emitter::subtract_immediate(unsigned destination, unsigned source,
                          unsigned immediate, bool shift_12) {
    emit(0xd100'0000U | (shift_12 ? 1U << 22 : 0U) |
         ((immediate & 0xfffU) << 10) | ((source & 31U) << 5) |
         (destination & 31U));
  }

void Arm64Emitter::add_shifted(unsigned destination, unsigned left, unsigned right,
                               unsigned shift) {
  emit(0x8b00'0000U | ((right & 31U) << 16) | ((shift & 63U) << 10) |
       ((left & 31U) << 5) | (destination & 31U));
}

void Arm64Emitter::logical_shift_right_variable(
    unsigned destination, unsigned source, unsigned shift_register) {
  emit(0x9ac0'2400U | ((shift_register & 31U) << 16U) |
       ((source & 31U) << 5U) | (destination & 31U));
}

void Arm64Emitter::ubfm(unsigned destination, unsigned source, unsigned rotate,
            unsigned mask) {
    emit(0xd340'0000U | ((rotate & 63U) << 16) | ((mask & 63U) << 10) |
         ((source & 31U) << 5) | (destination & 31U));
  }

void Arm64Emitter::compare_register(unsigned left, unsigned right) {
    if (flag_clobber_ != nullptr)
      flag_clobber_(flag_context_);
    emit(0xeb00'001fU | ((right & 31U) << 16) |
         ((left & 31U) << 5));
  }

void Arm64Emitter::emit_internal_flag_clobber(std::uint32_t instruction) {
    /*
     * This callback runs while translating. The generated instruction remains
     * branch-free; any required guest-NZCV publication is emitted before it.
     */
    if (flag_clobber_ != nullptr)
      flag_clobber_(flag_context_);
    emit(instruction);
  }

void Arm64Emitter::call(const void *function) {
    /* A host ABI call owns NZCV. Canonicalize guest flags at translation. */
    if (flag_clobber_ != nullptr)
      flag_clobber_(flag_context_);
    /* Host code never runs in streaming mode. The leave sequence keeps
     * x0-x8, so the helper's arguments survive it. */
    if (sme_host_mode_ != 0U)
      call_transition(
          reinterpret_cast<const void *>(&uml_arm64_jit_sme_leave));
    load_stable_pointer(16, callable_address(function));
    emit(0xd63f'0200U);
  }

void Arm64Emitter::call_transition(const void *routine) {
    load_stable_pointer(16, callable_address(routine));
    emit(0xd63f'0200U); // blr x16
  }

void Arm64Emitter::call_preserving_flags(const void *function) {
    /*
     * This is reserved for narrow assembly bridges whose complete contract
     * includes saving and restoring NZCV. Ordinary host calls must continue
     * through call() so a compiler bug cannot silently expose host flags.
     */
    load_stable_pointer(16, callable_address(function));
    emit(0xd63f'0200U);
  }

void Arm64Emitter::branch_register(unsigned reg) {
    emit(0xd61f'0000U | ((reg & 31U) << 5));
  }

void Arm64Emitter::prologue() {
    /*
     * Preserve the original argument slots, especially x2 at +32: tracked
     * store lowering consumes that stable stack ABI. x19-x21 remain private
     * to the outer generated invocation and survive every host helper.
     */
    emit(extended_frame_ ? 0xa9b8'7bfdU  // stp x29, x30, [sp, #-128]!
                         : 0xa9bc'7bfdU); // stp x29, x30, [sp, #-64]!
    emit(0x9100'03fdU); // mov x29, sp
    emit(0xa901'07e0U); // stp x0, x1, [sp, #16]
    emit(0xa902'57e2U); // stp x2, x21, [sp, #32]
    emit(0xa903'53f3U); // stp x19, x20, [sp, #48]
    if (extended_frame_) {
      emit(0xa904'5ff6U); // stp x22, x23, [sp, #64]
      emit(0xa905'67f8U); // stp x24, x25, [sp, #80]
      emit(0xa906'6ffaU); // stp x26, x27, [sp, #96]
      emit(0xf900'3bfcU); // str x28, [sp, #112]
    }
    move_x(15, 0);
    move_x(14, 1);
    move_x(19, 2);
    static_assert(maximum_chain_instructions <= 0xffffU);
    load_constant(20, maximum_chain_instructions);
    load_b(21, 19, offsetof(JitChainState, link_selector));
    /* Only the C++ dispatcher enters here: a linked predecessor of the same
     * mode branches to the internal entry with the host already in it. */
    if (sme_host_mode_ != 0U) {
      load_constant(9, sme_host_mode_);
      call_transition(
          reinterpret_cast<const void *>(&uml_arm64_jit_sme_enter));
    }
  }

void Arm64Emitter::epilogue() {
    /*
     * Leave streaming mode before returning to C++; after a terminal
     * helper's own leave this finds nothing to do. It keeps x0, the result.
     * A helper may have changed x15, which AAPCS64 does not preserve, so
     * the vCPU pointer comes back from the prologue's frame first.
     */
    if (sme_host_mode_ != 0U) {
      load_x(15, 31, 16); // ldr x15, [sp, #16]
      call_transition(
          reinterpret_cast<const void *>(&uml_arm64_jit_sme_leave));
    }
    /*
     * x20 contains the remaining budget before the current translated
     * block. Publish it exactly once when the outer generated invocation
     * returns, then restore every host callee-saved register.
     */
    store_x(20, 19, offsetof(JitChainState, remaining_instructions));
    emit(0xa943'53f3U); // ldp x19, x20, [sp, #48]
    emit(0xf940'17f5U); // ldr x21, [sp, #40]
    if (extended_frame_) {
      emit(0xa944'5ff6U); // ldp x22, x23, [sp, #64]
      emit(0xa945'67f8U); // ldp x24, x25, [sp, #80]
      emit(0xa946'6ffaU); // ldp x26, x27, [sp, #96]
      emit(0xf940'3bfcU); // ldr x28, [sp, #112]
      emit(0xa8c8'7bfdU); // ldp x29, x30, [sp], #128
    } else {
      emit(0xa8c4'7bfdU); // ldp x29, x30, [sp], #64
    }
    emit(0xd65f'03c0U);
  }

void Arm64Emitter::return_constant(Event event, std::size_t instructions) {
    if (exit_flush_ != nullptr)
      exit_flush_(exit_context_);
    if (terminal_does_not_retire(event)) {
      if (instructions != 0)
        --instructions;
    }
    load_constant(0, pack_jit_result(event, instructions));
    epilogue();
  }

void Arm64Emitter::return_dynamic(std::size_t instructions) {
    if (exit_flush_ != nullptr)
      exit_flush_(exit_context_);
    const auto retired_prefix = instructions == 0 ? 0 : instructions - 1U;
    if (retired_prefix != 0U) {
      load_constant(1, static_cast<std::uint64_t>(retired_prefix) << 32U);
      /*
       * The helper returns the current instruction's exact 0/1 retirement
       * delta. Addition, unlike a bitwise merge, composes an odd prefix with
       * the successful +1 carry into the upper 32-bit count.
       */
      emit(0x8b01'0000U); // add x0, x0, x1
    }
    epilogue();
  }

[[nodiscard]] std::span<const std::uint32_t> Arm64Emitter::finish() {
    if (!valid_)
      return {};
    for (const auto &entry : patches_) {
      if (entry.label >= labels_.size() || labels_[entry.label] == unbound)
        return {};
      const auto target = static_cast<std::int64_t>(labels_[entry.label]);
      const auto source = static_cast<std::int64_t>(entry.word);
      const auto displacement = target - source;
      auto &instruction = words_[entry.word];
      switch (entry.kind) {
      case BranchKind::unconditional:
        if (displacement < -(1 << 25) || displacement >= (1 << 25))
          return {};
        instruction |= static_cast<std::uint32_t>(displacement) & 0x03ff'ffffU;
        break;
      case BranchKind::conditional:
      case BranchKind::compare_zero:
      case BranchKind::compare_nonzero:
        if (displacement < -(1 << 18) || displacement >= (1 << 18))
          return {};
        instruction |= (static_cast<std::uint32_t>(displacement) & 0x7ffffU)
                       << 5;
        break;
      case BranchKind::test_zero:
      case BranchKind::test_nonzero:
        if (displacement < -(1 << 13) || displacement >= (1 << 13))
          return {};
        instruction |= (static_cast<std::uint32_t>(displacement) & 0x3fffU)
                       << 5;
        break;
      }
    }

    executable_words_ = words_.size();
    if (literal_patches_.empty())
      return words_;

    /*
     * Reserve the worst possible cold fallback footprint when deciding
     * whether a literal is reachable. This makes the choice conservative and
     * independent of which other pointers need a fallback.
     */
    constexpr std::size_t maximum_fallback_words = 5;
    const auto conservative_pool_start =
        (words_.size() +
         literal_patches_.size() * maximum_fallback_words + 1U) &
        ~std::size_t{1};
    std::vector<bool> literal_mask(literal_patches_.size());
    std::size_t literal_count = 0;
    for (std::size_t index = 0; index < literal_patches_.size(); ++index) {
      const auto source = literal_patches_[index].word;
      const auto target = conservative_pool_start + index * 2U;
      const bool use_literal =
          target >= source && target - source < literal_range_words_;
      literal_mask[index] = use_literal;
      literal_count += use_literal;
    }

    const auto encode_unconditional_branch =
        [](std::size_t source, std::size_t target,
           std::uint32_t &instruction) noexcept {
          const auto displacement = static_cast<std::int64_t>(target) -
                                    static_cast<std::int64_t>(source);
          if (displacement < -(std::int64_t{1} << 25U) ||
              displacement >= (std::int64_t{1} << 25U))
            return false;
          instruction =
              0x1400'0000U |
              (static_cast<std::uint32_t>(displacement) & 0x03ff'ffffU);
          return true;
        };

    /*
     * Emit out-of-range materializations as executable cold stubs before
     * the data tail. Both branches are immediate and translation-owned.
     */
    for (std::size_t index = 0; index < literal_patches_.size(); ++index) {
      if (literal_mask[index])
        continue;
      const auto patch = literal_patches_[index];
      /* Spread across MOV-wide immediates rather than one slot, so another
       * executor cannot rewrite it. See relocatable().
       */
      relocatable_ &= !patch.relocatable;
      const auto fallback = words_.size();
      if (!encode_unconditional_branch(patch.word, fallback,
                                       words_[patch.word]))
        return {};
      load_constant(patch.destination, patch.value);
      std::uint32_t resume{};
      if (!encode_unconditional_branch(words_.size(), patch.word + 1U,
                                       resume))
        return {};
      emit(resume);
    }
    executable_words_ = words_.size();
    if (literal_count == 0)
      return words_;

    /*
     * Code storage starts at a 16-byte boundary, so an even word index gives
     * every 64-bit literal its required alignment. Padding and literals are
     * deliberately appended after the final cold branch/return and are never
     * executable branch targets.
     */
    if ((words_.size() & 1U) != 0U)
      emit(0);
    for (std::size_t index = 0; index < literal_patches_.size(); ++index) {
      if (!literal_mask[index])
        continue;
      const auto patch = literal_patches_[index];
      const auto target = words_.size();
      const auto displacement = static_cast<std::int64_t>(target) -
                                static_cast<std::int64_t>(patch.word);
      if (displacement < -(std::int64_t{1} << 18U) ||
          displacement >= (std::int64_t{1} << 18U) ||
          static_cast<std::size_t>(displacement) >= literal_range_words_)
        return {};
      words_[patch.word] =
          0x5800'0000U |
          ((static_cast<std::uint32_t>(displacement) & 0x7ffffU) << 5U) |
          patch.destination;
      if (patch.relocatable)
        relocations_.push_back({target, patch.key, patch.kind});
      emit(static_cast<std::uint32_t>(patch.value));
      emit(static_cast<std::uint32_t>(patch.value >> 32U));
    }
    return words_;
  }

void Arm64Emitter::patch(Label label, BranchKind kind, std::uint32_t instruction) {
    patches_.push_back({words_.size(), label, kind});
    emit(instruction);
  }

} // namespace uml::cpu::arm64
