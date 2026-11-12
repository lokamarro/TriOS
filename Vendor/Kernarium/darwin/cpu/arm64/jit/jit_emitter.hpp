// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "jit_runtime_contract.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#endif

namespace uml::cpu::arm64 {

template <typename Function>
[[nodiscard]] Function sign_jit_function(void *pointer) noexcept {
#if __has_feature(ptrauth_calls)
  return reinterpret_cast<Function>(
      ptrauth_sign_unauthenticated(pointer, ptrauth_key_function_pointer, 0));
#else
  return reinterpret_cast<Function>(pointer);
#endif
}

[[nodiscard]] inline std::uintptr_t
callable_address(const void *pointer) noexcept {
#if __has_feature(ptrauth_calls)
  return reinterpret_cast<std::uintptr_t>(
      ptrauth_strip(pointer, ptrauth_key_function_pointer));
#else
  return reinterpret_cast<std::uintptr_t>(pointer);
#endif
}

class Arm64Emitter final {
public:
  using Label = std::size_t;
  using ExitFlush = void (*)(void *);
  using FlagClobber = void (*)(void *);

  enum class BranchKind {
    unconditional,
    conditional,
    compare_zero,
    compare_nonzero,
    test_zero,
    test_nonzero,
  };

  struct Patch final {
    std::size_t word;
    Label label;
    BranchKind kind;
  };

  struct StablePointerPatch final {
    std::size_t word;
    std::uint64_t value;
    unsigned destination;
    /* Set only for executor-owned link pointers, which a reusing executor
     * must rewrite. See JitRelocation in jit_runtime_contract.hpp.
     */
    bool relocatable{};
    JitRelocationKind kind{};
    std::uint64_t key{};
  };

  explicit Arm64Emitter(
      std::size_t literal_range_words = std::size_t{1} << 18U) noexcept;

  [[nodiscard]] Label make_label();
  void bind(Label label);
  void emit(std::uint32_t instruction);
  void attach_exit_flush(void *context, ExitFlush flush) noexcept;
  void attach_flag_clobber(void *context, FlagClobber clobber) noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  /* The word emitted at index. A branch or literal load is complete only
   * after finish(), so copy only a range for which patched() is false. */
  [[nodiscard]] std::uint32_t word(std::size_t index) const noexcept;
  [[nodiscard]] bool patched(std::size_t begin, std::size_t end) const noexcept;
  void branch(Label label);
  void branch_condition(Label label, unsigned condition);
  void compare_zero(Label label, unsigned reg, bool is_64_bit, bool nonzero);
  void test_zero(Label label, unsigned reg, unsigned bit, bool nonzero);
  void load_x(unsigned destination, unsigned base, std::size_t offset);
  void load_pair_x(unsigned first, unsigned second, unsigned base,
                   std::ptrdiff_t offset);
  void store_pair_x(unsigned first, unsigned second, unsigned base,
                    std::ptrdiff_t offset);
  void store_x(unsigned source, unsigned base, std::size_t offset);
  void load_w(unsigned destination, unsigned base, std::size_t offset);
  void store_w(unsigned source, unsigned base, std::size_t offset);
  void load_b(unsigned destination, unsigned base, std::size_t offset);
  void store_b(unsigned source, unsigned base, std::size_t offset);
  void load_q(unsigned destination, unsigned base, std::size_t offset);
  void store_q(unsigned source, unsigned base, std::size_t offset);
  void move_q(unsigned destination, unsigned source);
  void move_x(unsigned destination, unsigned source);
  void load_constant(unsigned destination, std::uint64_t value);
  void load_literal_constant(unsigned destination, std::uint64_t value);
  void load_stable_pointer(unsigned destination, std::uintptr_t value);
  void load_relocatable_pointer(unsigned destination, std::uintptr_t value,
                                JitRelocationKind kind, std::uint64_t key);
  void add_immediate(unsigned destination, unsigned source, unsigned immediate,
                     bool shift_12 = false);
  void subtract_immediate(unsigned destination, unsigned source,
                          unsigned immediate, bool shift_12 = false);
  void add_shifted(unsigned destination, unsigned left, unsigned right,
                   unsigned shift);
  void logical_shift_right_variable(unsigned destination, unsigned source,
                                    unsigned shift_register);
  void ubfm(unsigned destination, unsigned source, unsigned rotate,
            unsigned mask);
  void compare_register(unsigned left, unsigned right);
  void emit_internal_flag_clobber(std::uint32_t instruction);
  /* A host ABI call. In a streaming translation it first leaves streaming
   * mode, spilling the guest's vector state; every caller is a terminal
   * helper whose return reaches the epilogue. */
  void call(const void *function);
  void call_preserving_flags(const void *function);
  void branch_register(unsigned reg);
  /* The CpuState::sme_host_mode the translation runs in
   * (jit::jit_sme_host_mode()). A nonzero mode puts the enter sequence at
   * the end of the external prologue, before the internal entry, and the
   * leave sequence before every host call and in the epilogue. */
  void set_sme_host_mode(std::uint8_t mode) noexcept {
    sme_host_mode_ = mode;
  }
  /* An extended frame also saves x22-x28, which an AArch32 translation uses
   * for its core registers. Every translation of a chain must agree on it. */
  void set_extended_frame(bool extended) noexcept {
    extended_frame_ = extended;
  }
  void prologue();
  void epilogue();
  void return_constant(Event event, std::size_t instructions);
  void return_dynamic(std::size_t instructions);
  [[nodiscard]] std::span<const std::uint32_t> finish();

  /* Valid after finish(). Empty when the translation embedded no executor
   * owned pointer; see relocatable() for the case where one could not be
   * recorded.
   */
  [[nodiscard]] std::span<const JitRelocation> relocations() const noexcept {
    return relocations_;
  }

  /* False when a link pointer had to be materialized by a cold MOV-wide stub
   * instead of a literal pool slot, which happens only for translations large
   * enough to put the pool outside imm19 reach. Those cannot be reused by
   * another executor, because the pointer is spread across instruction
   * immediates rather than sitting in one relocatable slot.
   */
  [[nodiscard]] bool relocatable() const noexcept { return relocatable_; }

  /* Valid after finish(): the words before the literal pool's padding and
   * data, which no branch reaches. */
  [[nodiscard]] std::size_t executable_words() const noexcept {
    return executable_words_;
  }

private:
  static constexpr std::size_t unbound =
      std::numeric_limits<std::size_t>::max();

  void patch(Label label, BranchKind kind, std::uint32_t instruction);
  /* Calls a jit_sme_transition.S routine, which preserves NZCV. */
  void call_transition(const void *routine);

  std::vector<std::uint32_t> words_;
  std::vector<std::size_t> labels_;
  std::vector<Patch> patches_;
  std::vector<StablePointerPatch> literal_patches_;
  std::vector<JitRelocation> relocations_;
  bool relocatable_{true};
  std::size_t executable_words_{};
  std::size_t literal_range_words_{};
  bool valid_{true};
  void *exit_context_{};
  ExitFlush exit_flush_{};
  void *flag_context_{};
  FlagClobber flag_clobber_{};
  std::uint8_t sme_host_mode_{};
  bool extended_frame_{};
};

[[nodiscard]] constexpr std::size_t
register_offset(unsigned reg, bool stack_pointer) noexcept {
  return reg == 31U && stack_pointer ? offsetof(CpuState, sp)
                                     : reg * sizeof(std::uint64_t);
}

} // namespace uml::cpu::arm64
