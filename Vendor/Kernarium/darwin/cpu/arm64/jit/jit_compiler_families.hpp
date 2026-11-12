// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/jit/jit_runtime_contract.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/engine.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace uml::cpu::arm64::jit {

namespace a32 {
class A32Compiler;
}

namespace arm64 = ::uml::cpu::arm64;
using ::uml::cpu::ArmFeature;
using ::uml::cpu::ArmFeatureSet;
using ::uml::cpu::host_capabilities;

/*
 * Family emitters live in separate translation units.  This small bridge
 * deliberately hides the executor's concrete Arm64Emitter: family code sees
 * only the operations needed to translate an instruction and cannot reach
 * executor/cache state.
 */
class JitCompilerFamilies {
  /* The AArch32 compiler (jit/a32) keeps its guest registers in the same
   * bindings and flag state, and branches over conditional bodies on them. */
  friend class a32::A32Compiler;

protected:
  using Event = ::uml::cpu::EngineEvent;
  using Label = std::size_t;
  using LinkResolver = const void *(*)(void *, std::uint64_t);
  using IndirectLinkResolver = const void *(*)(void *, std::uint64_t);

  JitCompilerFamilies() = default;

  void attach_family_emitter(void *emitter, bool *native_fp,
                             bool enable_gpr_cache) noexcept;
  void attach_link_resolver(void *context, LinkResolver resolver) noexcept;
  /* The translation keeps its core registers in the host across every link:
   * a successor finds them in their hosts, a block entered from the executor
   * loads them all before its internal entry, and every return to the
   * executor writes all of them back. Its link sequences use x10 and x11,
   * which no binding holds, and a pair of bindings is written back with one
   * store. */
  void keep_gprs_across_links() noexcept {
    resident_across_links_ = true;
    link_pointer_ = 10U;
    link_scratch_ = 11U;
  }
  [[nodiscard]] bool gprs_kept_across_links() const noexcept {
    return resident_across_links_;
  }
  void begin_link_observation() noexcept;
  [[nodiscard]] std::size_t direct_link_refusals() const noexcept;
  [[nodiscard]] std::size_t indirect_link_refusals() const noexcept;
  void attach_indirect_link_resolver(
      void *context, IndirectLinkResolver resolver) noexcept;

  /* The mode the translation is compiled for: the guest SVCR mode (svcr_sm,
   * svcr_za) and translation_mode_tbi, which strips the top byte of every
   * non-SP data address inline. Its links and indirect sites use
   * translation_key() with it. */
  void set_translation_mode(std::uint8_t mode) noexcept {
    sme_mode_ = static_cast<std::uint8_t>(mode & svcr_mask);
    top_byte_ignore_ = (mode & translation_mode_tbi) != 0U;
  }
  [[nodiscard]] std::uint8_t sme_mode() const noexcept { return sme_mode_; }
  [[nodiscard]] std::uint8_t translation_mode() const noexcept {
    return static_cast<std::uint8_t>(
        sme_mode_ | (top_byte_ignore_ ? translation_mode_tbi : 0U));
  }
  [[nodiscard]] bool top_byte_ignore() const noexcept {
    return top_byte_ignore_;
  }
  [[nodiscard]] bool streaming_translation() const noexcept {
    return (sme_mode_ & svcr_sm) != 0U;
  }

  void emit(std::uint32_t instruction);
  [[nodiscard]] Label make_label();
  void bind(Label label);
  void branch_condition(Label label, unsigned condition);
  void compare_zero(Label label, unsigned reg, bool is_64_bit, bool nonzero);
  void test_zero(Label label, unsigned reg, unsigned bit, bool nonzero);
  void load_constant(unsigned destination, std::uint64_t value);
  void load_literal_constant(unsigned destination, std::uint64_t value);
  void load_x(unsigned destination, unsigned base, std::size_t offset);
  void store_x(unsigned source, unsigned base, std::size_t offset);
  void load_q(unsigned destination, unsigned base, std::size_t offset);
  void store_q(unsigned source, unsigned base, std::size_t offset);
  void return_constant(Event event, std::size_t instructions);
  void return_dynamic(std::size_t instructions);
  void load_gpr(unsigned host, unsigned guest, bool stack_pointer);
  void store_gpr(unsigned host, unsigned guest, bool stack_pointer);

  struct NativeGpr final {
    unsigned host{};
    bool resident{};
  };

  [[nodiscard]] bool gpr_slots_unbound() const noexcept;
  [[nodiscard]] constexpr std::size_t gpr_slot_capacity() const noexcept {
    return gpr_host_limit_;
  }

  [[nodiscard]] NativeGpr read_native_gpr(unsigned guest,
                                          bool stack_pointer,
                                          unsigned fallback);
  [[nodiscard]] NativeGpr write_native_gpr(unsigned guest,
                                           bool stack_pointer,
                                           bool reads_old,
                                           unsigned fallback);
  void commit_native_gpr(NativeGpr value, unsigned guest,
                         bool stack_pointer);

  struct NativeVector final {
    unsigned host{};
    bool resident{};
  };

  [[nodiscard]] std::optional<unsigned>
  constrained_vector_source(std::uint32_t instruction) const noexcept;
  void pin_constrained_vector(unsigned guest) noexcept;
  struct ConsecutiveVectorList final {
    unsigned first{};
    unsigned count{};
  };
  [[nodiscard]] std::optional<ConsecutiveVectorList>
  consecutive_vector_list(std::uint32_t instruction) const noexcept;
  [[nodiscard]] bool
  pin_consecutive_vector_list(unsigned first, unsigned count) noexcept;
  [[nodiscard]] NativeVector read_native_vector(unsigned guest,
                                                unsigned fallback);
  [[nodiscard]] NativeVector
  read_constrained_native_vector(unsigned guest, unsigned fallback);
  [[nodiscard]] NativeVector write_native_vector(unsigned guest,
                                                 bool reads_old,
                                                 unsigned fallback);
  void commit_native_vector(NativeVector value, unsigned guest);
  /*
   * These emit instruction with its Rn (bits 5-9), Rm and Rd (bits 0-4)
   * fields naming native vector bindings. Operands bind in the order
   * listed, each from the fallback host register given when the cache is
   * full, and an operand naming an earlier operand's register shares its
   * binding.
   *
   * Unary: Rn (v1), then Rd (v0), whose old value is loaded when
   * reads_destination is set.
   */
  void emit_native_vector_unary(std::uint32_t instruction,
                                bool reads_destination);
  /* Binary: Rn, Rm in bits 16-20, then Rd, whose old value is loaded when
   * reads_destination is set. The fallbacks default to v0, v1 and v2; the
   * floating forms, PMULL and three-register SHA3 pass v1, v2 and v0. */
  struct BinaryFallbacks final {
    unsigned left;
    unsigned right;
    unsigned destination;
  };
  void emit_native_vector_binary(
      std::uint32_t instruction, bool reads_destination,
      BinaryFallbacks fallbacks = {.left = 0U, .right = 1U, .destination = 2U});
  /* By element, with Rm in bits 16-19: Rm from the constrained v0-v15
   * bindings (v2), Rn (v1), then Rd, which is read (v0). */
  void emit_native_vector_by_element(std::uint32_t instruction);
  /* Accumulating: Rd, which is read (v0), Rn (v1), then, with has_second,
   * Rm in bits 16-20 (v2). */
  void emit_native_vector_accumulate(std::uint32_t instruction,
                                     bool has_second);

  [[nodiscard]] std::uint16_t dirty_gpr_mask() const noexcept;
  [[nodiscard]] std::uint32_t dirty_vector_mask() const noexcept;
  void set_exit_gpr_mask(std::uint16_t mask) noexcept;
  void set_exit_vector_mask(std::uint32_t mask) noexcept;
  void flush_exit_gprs();
  void spill_gprs_for_terminal_call();
  void flush_link_transition_state();

  void store_pc(std::uint64_t pc);
  enum class GuestFlagsState : unsigned char {
    memory,
    synchronized,
    host_dirty,
  };
  /*
   * Guest NZCV has three translation-time states:
   *   memory       CpuState is canonical; host NZCV is scratch.
   *   synchronized CpuState and host NZCV both contain the guest value.
   *   host_dirty   host NZCV is canonical; CpuState is stale.
   *
   * load_flags/store_flags retain their historical producer/consumer names,
   * but now move only when the state machine requires it.
   */
  void load_flags();
  void store_flags();
  void synchronize_flags();
  void spill_flags_for_internal_clobber();
  void flush_flags_for_exit();
  void assume_flags_in_memory() noexcept;
  [[nodiscard]] GuestFlagsState snapshot_flags() const noexcept;
  void publish_cold_flags(GuestFlagsState state);
  void read_flags(unsigned destination);
  void mark_native_fp() noexcept;
  void emit_linked_exit(std::uint64_t target, std::size_t count);
  /* `target_table`: a miss of both cells looks in the executor's table of
   * trained targets (JitIndirectTarget) before it leaves. */
  void emit_indirect_linked_exit(unsigned target_host, bool writes_link,
                                 std::uint64_t source_pc,
                                 std::size_t count,
                                 bool target_table = false);

  void emit_native_binary(std::uint32_t instruction, bool stack_pointer,
                          bool sets_flags);
  void emit_native_unary(std::uint32_t instruction);
  void emit_conditional_compare(std::uint32_t instruction);
  void emit_bitfield(std::uint32_t instruction);
  void emit_direct_branch(std::uint64_t target, std::size_t count);
  void emit_conditional_pc(unsigned condition, std::uint64_t taken,
                           std::uint64_t fallthrough, std::size_t count);
  void emit_compare_branch(std::uint32_t instruction, std::uint64_t pc,
                           std::size_t count);
  void emit_test_branch(std::uint32_t instruction, std::uint64_t pc,
                        std::size_t count);
  void emit_register_branch(std::uint32_t instruction, std::uint64_t pc,
                            std::size_t count);
  void emit_pc_relative(std::uint32_t instruction, std::uint64_t pc);
  void emit_move_wide(std::uint32_t instruction);
  void emit_add_sub_immediate(std::uint32_t instruction);
  void emit_add_sub_extended(std::uint32_t instruction);
  void emit_logical_immediate(std::uint32_t instruction);
  void emit_logical_shifted(std::uint32_t instruction);
  void emit_add_sub_carry(std::uint32_t instruction);
  void emit_conditional_select(std::uint32_t instruction);
  void emit_integer_min_max(std::uint32_t instruction, bool immediate);
  void emit_bit_reverse_count(std::uint32_t instruction);
  /* CNT without FEAT_CSSC or AdvSIMD, for streaming translations. */
  void emit_gpr_population_count(bool wide, unsigned source,
                                 unsigned result);
  void emit_multiply_add(std::uint32_t instruction);
  [[nodiscard]] bool emit_flag_manipulation(std::uint32_t instruction);
  [[nodiscard]] static bool valid_move_wide(std::uint32_t instruction) noexcept;
  [[nodiscard]] static bool valid_shifted_register(std::uint32_t instruction,
                                                   bool add_sub) noexcept;
  [[nodiscard]] static bool
  valid_extended_register(std::uint32_t instruction) noexcept;
  [[nodiscard]] static bool
  valid_logical_immediate(std::uint32_t instruction) noexcept;
  [[nodiscard]] static bool valid_bitfield(std::uint32_t instruction) noexcept;
  [[nodiscard]] static bool
  valid_multiply_add(std::uint32_t instruction) noexcept;
  [[nodiscard]] static bool
  valid_bit_reverse_count(std::uint32_t instruction) noexcept;

  [[nodiscard]] bool emit_basic_floating(std::uint32_t instruction);
  [[nodiscard]] bool emit_fused_floating(std::uint32_t instruction);
  [[nodiscard]] bool emit_integer_to_floating(std::uint32_t instruction);
  [[nodiscard]] bool emit_floating_to_integer(std::uint32_t instruction);
  [[nodiscard]] bool
  emit_floating_javascript_convert(std::uint32_t instruction);
  [[nodiscard]] bool emit_floating_precision_convert(std::uint32_t instruction);
  [[nodiscard]] bool emit_floating_round(std::uint32_t instruction);
  [[nodiscard]] bool emit_floating_unary(std::uint32_t instruction);
  [[nodiscard]] bool emit_floating_horizontal(std::uint32_t instruction);
  [[nodiscard]] bool emit_floating_reciprocal(std::uint32_t instruction);
  [[nodiscard]] bool
  host_supports_floating_type(std::uint32_t instruction) const;
  void emit_floating_compare(std::uint32_t instruction, bool conditional);
  void emit_floating_select(std::uint32_t instruction);
  void emit_floating_move(std::uint32_t instruction, bool immediate);

  [[nodiscard]] static bool
  valid_simd_three_same_integer(std::uint32_t instruction) noexcept;
  [[nodiscard]] bool emit_simd_bitwise_register(std::uint32_t instruction);
  [[nodiscard]] bool emit_simd_three_same_integer(std::uint32_t instruction);
  [[nodiscard]] bool emit_simd_arithmetic_narrow(std::uint32_t instruction);
  [[nodiscard]] bool emit_simd_widening(std::uint32_t instruction);
  [[nodiscard]] bool emit_simd_narrow_move(std::uint32_t instruction);
  [[nodiscard]] bool
  emit_simd_two_register_integer(std::uint32_t instruction);
  [[nodiscard]] bool emit_simd_unsigned_estimate(std::uint32_t instruction);
  [[nodiscard]] static bool
  recognized_simd_fmulx(std::uint32_t instruction) noexcept;
  [[nodiscard]] static bool
  valid_simd_fmulx(std::uint32_t instruction) noexcept;
  [[nodiscard]] bool emit_simd_fmulx(std::uint32_t instruction);
  [[nodiscard]] bool emit_bfloat_dot(std::uint32_t instruction);
  [[nodiscard]] bool emit_half_fused_long(std::uint32_t instruction);
  [[nodiscard]] bool emit_bfloat_multiply_long(std::uint32_t instruction);
  [[nodiscard]] bool emit_bfloat_matrix(std::uint32_t instruction);
  [[nodiscard]] static bool
  recognized_simd_dot(std::uint32_t instruction) noexcept;
  [[nodiscard]] static bool
  valid_simd_dot(std::uint32_t instruction) noexcept;
  [[nodiscard]] bool emit_simd_dot(std::uint32_t instruction);
  [[nodiscard]] static bool
  recognized_simd_matrix(std::uint32_t instruction) noexcept;
  [[nodiscard]] static bool
  valid_simd_matrix(std::uint32_t instruction) noexcept;
  [[nodiscard]] bool emit_simd_matrix(std::uint32_t instruction);
  [[nodiscard]] static bool
  recognized_simd_floating_indexed(std::uint32_t instruction) noexcept;
  [[nodiscard]] static bool
  valid_simd_floating_indexed(std::uint32_t instruction) noexcept;
  [[nodiscard]] static bool
  recognized_simd_integer_indexed(std::uint32_t instruction) noexcept;
  [[nodiscard]] static bool
  valid_simd_integer_indexed(std::uint32_t instruction) noexcept;
  [[nodiscard]] bool emit_simd_integer_indexed(std::uint32_t instruction);
  [[nodiscard]] static bool
  recognized_simd_rdm(std::uint32_t instruction) noexcept;
  [[nodiscard]] static bool
  valid_simd_rdm(std::uint32_t instruction) noexcept;
  [[nodiscard]] bool emit_simd_rdm(std::uint32_t instruction);
  [[nodiscard]] bool
  emit_simd_floating_compare(std::uint32_t instruction);
  [[nodiscard]] bool
  emit_simd_floating_complex(std::uint32_t instruction);
  [[nodiscard]] bool emit_simd_floating_indexed(std::uint32_t instruction);
  [[nodiscard]] bool emit_simd_extract(std::uint32_t instruction);
  [[nodiscard]] bool emit_simd_table_lookup(std::uint32_t instruction);
  [[nodiscard]] bool emit_simd_permute(std::uint32_t instruction);

  [[nodiscard]] bool emit_crc32(std::uint32_t instruction);
  [[nodiscard]] bool emit_simd_aes(std::uint32_t instruction);
  [[nodiscard]] bool emit_simd_pmull(std::uint32_t instruction);
  [[nodiscard]] bool emit_simd_sha1_sha2(std::uint32_t instruction);
  [[nodiscard]] bool emit_simd_sha512(std::uint32_t instruction);
  [[nodiscard]] bool emit_simd_sha3(std::uint32_t instruction);

private:
  static constexpr unsigned unbound_guest = 32U;
  /* The CpuState offset of a vector binding's identity. */
  [[nodiscard]] static std::size_t
  vector_slot_offset(unsigned guest) noexcept;
  /* The first eight hold an A64 translation's registers; an AArch32
   * translation, whose guest has fifteen core registers, takes all of them
   * (x22-x28 are callee-saved, which its frame preserves). */
  static constexpr std::array<unsigned, 15> resident_gpr_hosts{
      3U, 4U, 5U, 6U, 7U, 12U, 13U, 17U, 22U, 23U, 24U, 25U, 26U, 27U, 28U};
  static constexpr std::size_t a64_gpr_hosts = 8;
  /* x19-x21 carry linked-block state, and Darwin clears x18 when it
   * interrupts the thread. */
  static_assert([] {
    for (const auto host : resident_gpr_hosts)
      if (host >= 18U && host <= 21U)
        return false;
    return true;
  }());
  static_assert(resident_gpr_hosts.size() <= 16U,
                "the dirty masks are sixteen bits");
  static constexpr std::size_t pinned_vector_slot_count = 2;
  static constexpr std::array<unsigned, 18> resident_vector_hosts{
      6U,  7U,  16U, 17U, 18U, 19U, 20U, 21U, 22U,
      23U, 24U, 25U, 26U, 27U, 28U, 29U, 30U, 31U};

  struct GprSlot final {
    unsigned guest{unbound_guest};
    bool stack_pointer{};
    bool loaded{};
    bool dirty{};
  };

  struct VectorSlot final {
    unsigned guest{unbound_guest};
    bool loaded{};
    bool dirty{};
  };

  [[nodiscard]] std::optional<std::size_t>
  find_gpr_slot(unsigned guest, bool stack_pointer) const noexcept;
  [[nodiscard]] std::optional<std::size_t>
  allocate_gpr_slot(unsigned guest, bool stack_pointer);
  [[nodiscard]] std::optional<std::size_t>
  find_vector_slot(unsigned guest) const noexcept;
  [[nodiscard]] std::optional<std::size_t>
  allocate_vector_slot(unsigned guest);
  void flush_gprs(std::uint16_t mask);
  void flush_vectors(std::uint32_t mask);

  std::array<GprSlot, resident_gpr_hosts.size()> gpr_slots_{};
  std::array<VectorSlot, resident_vector_hosts.size()> vector_slots_{};
  std::size_t gpr_host_limit_{a64_gpr_hosts};
  std::uint16_t exit_gpr_mask_{};
  bool resident_across_links_{};
  unsigned link_pointer_{12U};
  unsigned link_scratch_{13U};
  std::uint32_t exit_vector_mask_{};
  GuestFlagsState guest_flags_state_{GuestFlagsState::memory};
  bool exit_gpr_mask_set_{};
  bool exit_vector_mask_set_{};
  bool gpr_cache_enabled_{};
  void *family_emitter_{};
  bool *family_native_fp_{};
  void *link_context_{};
  LinkResolver link_resolver_{};
  std::size_t direct_link_refusals_{};
  void *indirect_link_context_{};
  IndirectLinkResolver indirect_link_resolver_{};
  std::size_t indirect_link_refusals_{};
  std::uint8_t sme_mode_{};
  bool top_byte_ignore_{};
};

} // namespace uml::cpu::arm64::jit
