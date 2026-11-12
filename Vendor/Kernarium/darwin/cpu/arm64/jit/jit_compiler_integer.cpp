// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_compiler_families.hpp"

#include "cpu/arm64/detail/instruction_primitives.hpp"
#include "cpu/host_capabilities.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace uml::cpu::arm64::jit {

using arm64::detail::sign_extend;

void JitCompilerFamilies::emit_native_binary(std::uint32_t instruction,
                                             bool stack_pointer,
                                             bool sets_flags) {
  const unsigned destination = instruction & 31U;
  const unsigned left = (instruction >> 5) & 31U;
  const unsigned right = (instruction >> 16) & 31U;
  if (destination == 31U && !sets_flags && !stack_pointer)
    return;

  const auto native_left = read_native_gpr(left, stack_pointer, 8);
  const auto native_right =
      !stack_pointer && right == left
          ? native_left
          : read_native_gpr(right, false, 9);
  const bool destination_is_sp = stack_pointer && !sets_flags;
  const auto native_destination =
      write_native_gpr(destination, destination_is_sp, false, 10);
  const auto native = (instruction & ~0x001f'03ffU) |
                      (native_right.host << 16) |
                      (native_left.host << 5) |
                      native_destination.host;
  emit(native);
  commit_native_gpr(native_destination, destination, destination_is_sp);
  if (sets_flags)
    store_flags();
}

void JitCompilerFamilies::emit_native_unary(std::uint32_t instruction) {
  const unsigned destination = instruction & 31U;
  const unsigned source = (instruction >> 5) & 31U;
  const auto native_source = read_native_gpr(source, false, 8);
  const auto native_destination =
      write_native_gpr(destination, false, false, 9);
  const auto native = (instruction & ~0x3ffU) |
                      (native_source.host << 5) |
                      native_destination.host;
  emit(native);
  commit_native_gpr(native_destination, destination, false);
}

void JitCompilerFamilies::emit_conditional_compare(std::uint32_t instruction) {
  const bool immediate = (instruction & 0x0000'0800U) != 0;
  const unsigned left = (instruction >> 5) & 31U;
  const unsigned condition = (instruction >> 12) & 15U;
  auto native = instruction;
  if (condition < 14U)
    load_flags();
  NativeGpr native_left{31U, true};
  if (left != 31U) {
    native_left = read_native_gpr(left, false, 8);
    native = (native & ~(31U << 5)) | (native_left.host << 5);
  }
  NativeGpr native_right{31U, true};
  if (!immediate) {
    const unsigned right = (instruction >> 16) & 31U;
    if (right != 31U) {
      if (right == left) {
        native_right = native_left;
      } else {
        native_right = read_native_gpr(right, false, 9);
      }
      native = (native & ~(31U << 16)) | (native_right.host << 16);
    }
  }
  /*
   * AL/NV do not consume the incoming flags. Keep the original conditional
   * compare when its immediate form names ZR: unlike ADD/SUB immediate,
   * CCMN/CCMP encode register 31 as ZR, so the guest word is already the
   * shortest correct native operation. Other unconditional forms retain the
   * flag-independent ADD/SUB lowering below.
   */
  if (condition >= 14U && !(immediate && left == 31U)) {
    const bool wide = (instruction & 0x8000'0000U) != 0;
    const bool subtract = (instruction & 0x4000'0000U) != 0;
    if (immediate) {
      const unsigned value = (instruction >> 16) & 31U;
      native = 0x3100'001fU | (wide ? 0x8000'0000U : 0U) |
               (subtract ? 0x4000'0000U : 0U) |
               (value << 10) |
               (native_left.host << 5);
    } else {
      native = 0x2b00'001fU | (wide ? 0x8000'0000U : 0U) |
               (subtract ? 0x4000'0000U : 0U) |
               (native_right.host << 16) | (native_left.host << 5);
    }
  }
  emit(native);
  store_flags();
}

void JitCompilerFamilies::emit_bitfield(std::uint32_t instruction) {
  const unsigned destination = instruction & 31U;
  const unsigned source = (instruction >> 5) & 31U;
  const auto native_source = read_native_gpr(source, false, 8);
  /* BFM/BFXIL is destructive: bits outside the inserted field come
   * from the old destination. SBFM and UBFM do not read Rd.
   */
  const bool reads_old = ((instruction >> 29) & 3U) == 1U;
  const auto native_destination =
      write_native_gpr(destination, false, reads_old, 9);
  emit((instruction & ~0x3ffU) | (native_source.host << 5) |
       native_destination.host);
  commit_native_gpr(native_destination, destination, false);
}

void JitCompilerFamilies::emit_direct_branch(std::uint64_t target,
                                             std::size_t count) {
  emit_linked_exit(target, count);
}

void JitCompilerFamilies::emit_conditional_pc(unsigned condition,
                                              std::uint64_t taken,
                                              std::uint64_t fallthrough,
                                              std::size_t count) {
  if (condition >= 14U) {
    emit_direct_branch(taken, count);
    return;
  }
  const auto not_taken = make_label();
  load_flags();
  /*
   * Each successor performs private publication/fairness comparisons. Publish
   * a dirty guest NZCV once before the branch so both arms can safely hand
   * host flags to their internal link machinery.
   */
  synchronize_flags();
  branch_condition(not_taken, condition ^ 1U);
  emit_direct_branch(taken, count);
  bind(not_taken);
  emit_direct_branch(fallthrough, count);
}

void JitCompilerFamilies::emit_compare_branch(std::uint32_t instruction,
                                              std::uint64_t pc,
                                              std::size_t count) {
  const auto target = pc + sign_extend((instruction >> 5) & 0x7ffffU, 19) * 4;
  const auto fallthrough = pc + 4;
  const auto not_taken = make_label();
  const unsigned source = instruction & 31U;
  const bool nonzero = (instruction & 0x0100'0000U) != 0;
  if (source == 31U) {
    emit_direct_branch(nonzero ? fallthrough : target, count);
    return;
  }
  const auto native_source = read_native_gpr(source, false, 8);
  /*
   * CBZ/CBNZ does not modify NZCV, but either successor's link checks do.
   * Make CpuState canonical on the common path before control splits.
   */
  synchronize_flags();
  compare_zero(not_taken, native_source.host, (instruction >> 31) != 0,
               !nonzero);
  emit_direct_branch(target, count);
  bind(not_taken);
  emit_direct_branch(fallthrough, count);
}

void JitCompilerFamilies::emit_test_branch(std::uint32_t instruction,
                                           std::uint64_t pc,
                                           std::size_t count) {
  const auto target = pc + sign_extend((instruction >> 5) & 0x3fffU, 14) * 4;
  const auto fallthrough = pc + 4;
  const auto not_taken = make_label();
  const unsigned source = instruction & 31U;
  const unsigned bit =
      ((instruction >> 26) & 0x20U) | ((instruction >> 19) & 0x1fU);
  const bool nonzero = (instruction & 0x0100'0000U) != 0;
  if (source == 31U) {
    emit_direct_branch(nonzero ? fallthrough : target, count);
    return;
  }
  const auto native_source = read_native_gpr(source, false, 8);
  synchronize_flags();
  test_zero(not_taken, native_source.host, bit, !nonzero);
  emit_direct_branch(target, count);
  bind(not_taken);
  emit_direct_branch(fallthrough, count);
}

void JitCompilerFamilies::emit_register_branch(std::uint32_t instruction,
                                               std::uint64_t pc,
                                               std::size_t count) {
  const unsigned source = (instruction >> 5) & 31U;
  const auto native_source = read_native_gpr(source, false, 8);
  const bool writes_link =
      (instruction & 0xffff'fc1fU) == 0xd63f'0000U;
  emit_indirect_linked_exit(native_source.host, writes_link, pc, count);
}

void JitCompilerFamilies::emit_pc_relative(std::uint32_t instruction,
                                           std::uint64_t pc) {
  if ((instruction & 31U) == 31U)
    return;
  const auto immediate = sign_extend(
      ((static_cast<std::uint64_t>(instruction >> 5) & 0x7ffffU) << 2) |
          ((instruction >> 29) & 3U),
      21);
  const bool page = (instruction & 0x8000'0000U) != 0;
  const auto value = page ? (pc & ~std::uint64_t{0xfff}) +
                                (static_cast<std::uint64_t>(immediate) << 12)
                          : pc + static_cast<std::uint64_t>(immediate);
  const unsigned destination = instruction & 31U;
  const auto native_destination =
      write_native_gpr(destination, false, false, 8);
  load_literal_constant(native_destination.host, value);
  commit_native_gpr(native_destination, destination, false);
}

void JitCompilerFamilies::emit_move_wide(std::uint32_t instruction) {
  const unsigned destination = instruction & 31U;
  const bool reads_old = ((instruction >> 29) & 3U) == 3U;
  const auto native_destination =
      write_native_gpr(destination, false, reads_old, 8);
  const auto native =
      (instruction & ~31U) | native_destination.host;
  emit(native);
  commit_native_gpr(native_destination, destination, false);
}

void JitCompilerFamilies::emit_add_sub_immediate(std::uint32_t instruction) {
  const unsigned destination = instruction & 31U;
  const unsigned source = (instruction >> 5) & 31U;
  const bool flags = (instruction & 0x2000'0000U) != 0;
  const auto native_source = read_native_gpr(source, true, 8);
  const auto native_destination =
      write_native_gpr(destination, !flags, false, 9);
  const auto native = (instruction & ~0x3ffU) |
                      (native_source.host << 5) |
                      native_destination.host;
  emit(native);
  commit_native_gpr(native_destination, destination, !flags);
  if (flags)
    store_flags();
}

void JitCompilerFamilies::emit_add_sub_extended(std::uint32_t instruction) {
  const unsigned destination = instruction & 31U;
  const unsigned left = (instruction >> 5) & 31U;
  const unsigned right = (instruction >> 16) & 31U;
  const bool flags = (instruction & 0x2000'0000U) != 0;
  const auto native_left = read_native_gpr(left, true, 8);
  const auto native_right = read_native_gpr(right, false, 9);
  const auto native_destination =
      write_native_gpr(destination, !flags, false, 10);
  emit((instruction & ~0x001f'03ffU) |
       (native_right.host << 16) | (native_left.host << 5) |
       native_destination.host);
  commit_native_gpr(native_destination, destination, !flags);
  if (flags)
    store_flags();
}

void JitCompilerFamilies::emit_logical_immediate(std::uint32_t instruction) {
  const unsigned destination = instruction & 31U;
  const unsigned source = (instruction >> 5) & 31U;
  const bool flags = ((instruction >> 29) & 3U) == 3U;
  auto native = instruction;
  if (source != 31U) {
    const auto native_source = read_native_gpr(source, false, 8);
    native = (native & ~(31U << 5)) | (native_source.host << 5);
  }
  const auto result =
      flags && destination == 31U
          ? NativeGpr{31U, true}
          : write_native_gpr(destination, !flags, false, 9);
  native = (native & ~31U) | result.host;
  emit(native);
  if (result.host != 31U)
    commit_native_gpr(result, destination, !flags);
  if (flags)
    store_flags();
}

void JitCompilerFamilies::emit_logical_shifted(std::uint32_t instruction) {
  const unsigned destination = instruction & 31U;
  const unsigned left = (instruction >> 5) & 31U;
  const unsigned right = (instruction >> 16) & 31U;
  const bool flags = ((instruction >> 29) & 3U) == 3U;
  if (destination == 31U && !flags)
    return;

  auto native = instruction;
  NativeGpr native_left{31U, true};
  if (left != 31U) {
    native_left = read_native_gpr(left, false, 8);
    native = (native & ~(31U << 5)) | (native_left.host << 5);
  }
  if (right != 31U) {
    const auto native_right =
        right == left ? native_left : read_native_gpr(right, false, 9);
    native = (native & ~(31U << 16)) | (native_right.host << 16);
  }
  const auto result =
      destination == 31U
          ? NativeGpr{31U, true}
          : write_native_gpr(destination, false, false, 10);
  native = (native & ~31U) | result.host;
  emit(native);
  if (result.host != 31U)
    commit_native_gpr(result, destination, false);
  if (flags)
    store_flags();
}

void JitCompilerFamilies::emit_add_sub_carry(std::uint32_t instruction) {
  if ((instruction & 31U) == 31U &&
      (instruction & 0x2000'0000U) == 0U)
    return;
  load_flags();
  emit_native_binary(instruction, false, (instruction & 0x2000'0000U) != 0);
}

void JitCompilerFamilies::emit_conditional_select(std::uint32_t instruction) {
  const unsigned destination = instruction & 31U;
  if (destination == 31U)
    return;
  const unsigned left = (instruction >> 5) & 31U;
  const unsigned right = (instruction >> 16) & 31U;
  const unsigned operation =
      ((instruction >> 29) & 2U) | ((instruction >> 10) & 1U);
  const unsigned condition = (instruction >> 12) & 15U;
  if (condition >= 14U || (operation == 0U && left == right)) {
    const bool wide = (instruction & 0x8000'0000U) != 0;
    if (wide && destination == left)
      return;
    const auto native_left = read_native_gpr(left, false, 8);
    const auto native_destination =
        write_native_gpr(destination, false, false, 9);
    emit((wide ? 0xaa00'03e0U : 0x2a00'03e0U) |
         (native_left.host << 16) | native_destination.host);
    commit_native_gpr(native_destination, destination, false);
    return;
  }
  load_flags();
  emit_native_binary(instruction, false, false);
}

void JitCompilerFamilies::emit_integer_min_max(std::uint32_t instruction,
                                               bool immediate) {
  const bool wide = (instruction & 0x8000'0000U) != 0;
  const unsigned destination = instruction & 31U;
  if (destination == 31U)
    return;
  const unsigned operation =
      immediate ? (instruction >> 18) & 3U : (instruction >> 10) & 3U;
  const unsigned left = (instruction >> 5) & 31U;
  std::optional<unsigned> copy_source;
  if (immediate) {
    const unsigned encoded = (instruction >> 10) & 0xffU;
    if (operation == 1U && encoded == 0U)
      copy_source = left; // UMAX #0
  } else {
    const unsigned right = (instruction >> 16) & 31U;
    if (left == right)
      copy_source = left;
    else if (operation == 1U) {
      if (left == 31U)
        copy_source = right;
      else if (right == 31U)
        copy_source = left;
    } else if (operation == 3U && (left == 31U || right == 31U)) {
      copy_source = 31U;
    }
  }
  if (copy_source) {
    if (wide && destination == *copy_source)
      return;
    const auto native_source = read_native_gpr(*copy_source, false, 8);
    const auto native_destination =
        write_native_gpr(destination, false, false, 9);
    emit((wide ? 0xaa00'03e0U : 0x2a00'03e0U) |
         (native_source.host << 16) | native_destination.host);
    commit_native_gpr(native_destination, destination, false);
    return;
  }

  if (host_capabilities().cssc) {
    if (!immediate) {
      emit_native_binary(instruction, false, false);
      return;
    }
    const auto native_source = read_native_gpr(left, false, 8);
    const auto native_destination =
        write_native_gpr(destination, false, false, 10);
    const auto native = (instruction & ~0x3ffU) |
                        (native_source.host << 5) |
                        native_destination.host;
    emit(native);
    commit_native_gpr(native_destination, destination, false);
    return;
  }

  /*
   * Without host FEAT_CSSC, CMP and CSEL. The comparison is internal, so
   * publish any guest NZCV the host flags hold first. SMAX, UMAX, SMIN and
   * UMIN keep the first operand when it is greater, higher, less or lower.
   */
  static constexpr std::array<unsigned, 4> keep_first{0xcU, 0x8U, 0xbU,
                                                      0x3U};
  const std::uint32_t sf = wide ? 0x8000'0000U : 0U;
  const unsigned condition = keep_first[operation];
  if (immediate) {
    const unsigned encoded = (instruction >> 10) & 0xffU;
    const bool signed_operation = (operation & 1U) == 0U;
    const std::int64_t value =
        signed_operation ? sign_extend(encoded, 8)
                         : static_cast<std::int64_t>(encoded);
    if (left == 31U) {
      /* CMP's immediate form reads SP as register 31, and the operation of
       * a constant pair is itself a constant. */
      const std::int64_t zero = 0;
      std::int64_t result = value;
      switch (operation) {
      case 0: result = std::max(zero, value); break;
      case 1: result = std::max<std::uint64_t>(0U, encoded); break;
      case 2: result = std::min(zero, value); break;
      default: result = 0; break;
      }
      const auto native_destination =
          write_native_gpr(destination, false, false, 10);
      load_constant(native_destination.host,
                    wide ? static_cast<std::uint64_t>(result)
                         : static_cast<std::uint32_t>(result));
      commit_native_gpr(native_destination, destination, false);
      return;
    }
    spill_flags_for_internal_clobber();
    const auto native_left = read_native_gpr(left, false, 8);
    const auto native_destination =
        write_native_gpr(destination, false, false, 10);
    /* CMP #imm, or CMN #-imm for a negative immediate. */
    emit((value < 0 ? 0x3100'001fU : 0x7100'001fU) | sf |
         (static_cast<std::uint32_t>(value < 0 ? -value : value) << 10) |
         (native_left.host << 5));
    load_constant(11U, wide ? static_cast<std::uint64_t>(value)
                            : static_cast<std::uint32_t>(value));
    emit(0x1a80'0000U | sf | (11U << 16) | (condition << 12) |
         (native_left.host << 5) | native_destination.host); // csel
    commit_native_gpr(native_destination, destination, false);
    return;
  }

  const unsigned right = (instruction >> 16) & 31U;
  spill_flags_for_internal_clobber();
  const auto native_left = read_native_gpr(left, false, 8);
  const auto native_right = read_native_gpr(right, false, 9);
  const auto native_destination =
      write_native_gpr(destination, false, false, 10);
  emit(0x6b00'001fU | sf | (native_right.host << 16) |
       (native_left.host << 5)); // cmp
  emit(0x1a80'0000U | sf | (native_right.host << 16) | (condition << 12) |
       (native_left.host << 5) | native_destination.host); // csel
  commit_native_gpr(native_destination, destination, false);
}

namespace {

/* AND and ORR (immediate) with N:immr:imms = 0:000000:`imms`, the bit
 * patterns the population count below needs. */
[[nodiscard]] constexpr std::uint32_t
logical_pattern(bool orr, bool wide, unsigned imms, unsigned source,
                unsigned destination) noexcept {
  return (orr ? 0x3200'0000U : 0x1200'0000U) | (wide ? 0x8000'0000U : 0U) |
         (imms << 10) | (source << 5) | destination;
}

constexpr unsigned pattern_01 = 0x3cU;       // 0x5555...
constexpr unsigned pattern_0011 = 0x39U;     // 0x3333...
constexpr unsigned pattern_00001111 = 0x33U; // 0x0f0f...
constexpr unsigned pattern_00000001 = 0x30U; // 0x0101...

static_assert(arm64::detail::decode_logical_immediate(
                  true, 0, 0, pattern_01) == 0x5555'5555'5555'5555ULL);
static_assert(arm64::detail::decode_logical_immediate(
                  true, 0, 0, pattern_0011) == 0x3333'3333'3333'3333ULL);
static_assert(arm64::detail::decode_logical_immediate(
                  true, 0, 0, pattern_00001111) ==
              0x0f0f'0f0f'0f0f'0f0fULL);
static_assert(arm64::detail::decode_logical_immediate(
                  true, 0, 0, pattern_00000001) ==
              0x0101'0101'0101'0101ULL);

} // namespace

void JitCompilerFamilies::emit_gpr_population_count(bool wide,
                                                    unsigned source,
                                                    unsigned result) {
  /* The SWAR count in scratch x10 and x11, without NZCV or AdvSIMD. A W
   * form's 32-bit operations ignore the source's upper half. */
  const std::uint32_t sf = wide ? 0x8000'0000U : 0U;
  const unsigned width = wide ? 64U : 32U;
  const auto shift_right = [&](unsigned to, unsigned from, unsigned shift) {
    emit((wide ? 0xd340'0000U : 0x5300'0000U) | (shift << 16) |
         ((width - 1U) << 10) | (from << 5) | to); // lsr
  };
  shift_right(10, source, 1);
  emit(logical_pattern(false, wide, pattern_01, 10, 10));
  emit(0x4b00'0000U | sf | (10U << 16) | (source << 5) | 10U); // sub
  shift_right(11, 10, 2);
  emit(logical_pattern(false, wide, pattern_0011, 11, 11));
  emit(logical_pattern(false, wide, pattern_0011, 10, 10));
  emit(0x0b00'0000U | sf | (11U << 16) | (10U << 5) | 10U); // add
  emit(0x0b40'0000U | sf | (10U << 16) | (4U << 10) | (10U << 5) |
       10U); // add x10, x10, x10, lsr #4
  emit(logical_pattern(false, wide, pattern_00001111, 10, 10));
  emit(logical_pattern(true, wide, pattern_00000001, 31, 11)); // mov
  emit(0x1b00'7c00U | sf | (11U << 16) | (10U << 5) | 10U);    // mul
  shift_right(result, 10, width - 8U);
}

void JitCompilerFamilies::emit_bit_reverse_count(std::uint32_t instruction) {
  const unsigned operation = (instruction >> 10) & 31U;
  if (operation < 6U || host_capabilities().cssc) {
    emit_native_unary(instruction);
    return;
  }
  /* CTZ, CNT and ABS without host FEAT_CSSC, in sequences that leave NZCV
   * alone. None has an effect beyond its destination. */
  const unsigned destination = instruction & 31U;
  if (destination == 31U)
    return;
  const bool wide = (instruction & 0x8000'0000U) != 0;
  const std::uint32_t sf = wide ? 0x8000'0000U : 0U;
  const auto native_source =
      read_native_gpr((instruction >> 5) & 31U, false, 8);
  const auto native_destination =
      write_native_gpr(destination, false, false, 9);
  const unsigned source = native_source.host;
  const unsigned result = native_destination.host;
  switch (operation) {
  case 6: // CTZ
    emit(0x5ac0'0000U | sf | (source << 5) | result); // rbit
    emit(0x5ac0'1000U | sf | (result << 5) | result); // clz
    break;
  case 7: // CNT
    if (streaming_translation()) {
      emit_gpr_population_count(wide, source, result);
      break;
    }
    /* Count the bytes' bits in scratch v0, then sum them. */
    emit((wide ? 0x9e67'0000U : 0x1e27'0000U) | (source << 5)); // fmov
    emit(0x0e20'5800U);                                        // cnt v0.8b
    emit(0x0e31'b800U);                                        // addv b0
    emit(0x1e26'0000U | result); // fmov w<result>, s0
    break;
  default: // ABS: (x ^ s) - s with s the sign replicated.
    emit((wide ? 0x937f'fc00U : 0x131f'7c00U) | (source << 5) | 11U); // asr
    emit(0x4a00'0000U | sf | (11U << 16) | (source << 5) | result); // eor
    emit(0x4b00'0000U | sf | (11U << 16) | (result << 5) | result); // sub
    break;
  }
  commit_native_gpr(native_destination, destination, false);
}

void JitCompilerFamilies::emit_multiply_add(std::uint32_t instruction) {
  const unsigned destination = instruction & 31U;
  if (destination == 31U)
    return;
  const unsigned left = (instruction >> 5) & 31U;
  const unsigned accumulator = (instruction >> 10) & 31U;
  const unsigned right = (instruction >> 16) & 31U;
  const unsigned operation = (instruction >> 21) & 7U;
  auto native = instruction;
  NativeGpr native_left{31U, true};
  if (left != 31U) {
    native_left = read_native_gpr(left, false, 8);
    native = (native & ~(31U << 5)) | (native_left.host << 5);
  }
  if (right != 31U) {
    const auto native_right =
        right == left ? native_left : read_native_gpr(right, false, 9);
    native = (native & ~(31U << 16)) | (native_right.host << 16);
  }
  if (operation != 2U && operation != 6U && accumulator != 31U) {
    const auto native_accumulator =
        accumulator == left
            ? native_left
            : read_native_gpr(accumulator, false, 10);
    native = (native & ~(31U << 10)) |
             (native_accumulator.host << 10);
  }
  const auto native_destination =
      write_native_gpr(destination, false, false, 11);
  native = (native & ~31U) | native_destination.host;
  emit(native);
  commit_native_gpr(native_destination, destination, false);
}

[[nodiscard]] bool
JitCompilerFamilies::emit_flag_manipulation(std::uint32_t instruction) {
  ArmFeature required;
  if (instruction == 0xd500'401fU) {
    required = ArmFeature::flag_m;
  } else if (instruction == 0xd500'405fU ||
             instruction == 0xd500'403fU) {
    required = ArmFeature::flag_m2;
  } else if ((instruction & 0xffe0'7c10U) == 0xba00'0400U ||
             (instruction & 0xffff'bc1fU) == 0x3a00'080dU) {
    required = ArmFeature::flag_m;
  } else {
    return false;
  }
  if (!host_capabilities().has(required))
    return false;

  auto native = instruction;
  if ((instruction & 0xffe0'7c10U) == 0xba00'0400U ||
      (instruction & 0xffff'bc1fU) == 0x3a00'080dU) {
    const unsigned source = (instruction >> 5) & 31U;
    const auto native_source = read_native_gpr(source, false, 8);
    native =
        (instruction & ~(31U << 5)) | (native_source.host << 5);
  }
  load_flags();
  emit(native);
  store_flags();
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::valid_move_wide(std::uint32_t instruction) noexcept {
  const bool wide = (instruction & 0x8000'0000U) != 0;
  const unsigned operation = (instruction >> 29) & 3U;
  const unsigned half = (instruction >> 21) & 3U;
  return operation != 1U && (wide || half < 2U);
}

[[nodiscard]] bool
JitCompilerFamilies::valid_shifted_register(std::uint32_t instruction,
                                            bool add_sub) noexcept {
  const bool wide = (instruction & 0x8000'0000U) != 0;
  const unsigned shift_type = (instruction >> 22) & 3U;
  const unsigned shift = (instruction >> 10) & 63U;
  return (!add_sub || shift_type != 3U) && (wide || shift < 32U);
}

[[nodiscard]] bool JitCompilerFamilies::valid_extended_register(
    std::uint32_t instruction) noexcept {
  return ((instruction >> 10) & 7U) <= 4U;
}

[[nodiscard]] bool JitCompilerFamilies::valid_logical_immediate(
    std::uint32_t instruction) noexcept {
  return arm64::detail::decode_logical_immediate(
             (instruction & 0x8000'0000U) != 0, (instruction >> 22) & 1U,
             (instruction >> 16) & 63U, (instruction >> 10) & 63U)
      .has_value();
}

[[nodiscard]] bool
JitCompilerFamilies::valid_bitfield(std::uint32_t instruction) noexcept {
  const bool wide = (instruction & 0x8000'0000U) != 0;
  const unsigned operation = (instruction >> 29) & 3U;
  const unsigned n = (instruction >> 22) & 1U;
  const unsigned rotate = (instruction >> 16) & 63U;
  const unsigned imms = (instruction >> 10) & 63U;
  return operation != 3U && n == static_cast<unsigned>(wide) &&
         (wide || ((rotate | imms) & 32U) == 0) &&
         arm64::detail::decode_bit_masks(wide, n, rotate, imms, false)
             .has_value();
}

[[nodiscard]] bool
JitCompilerFamilies::valid_multiply_add(std::uint32_t instruction) noexcept {
  const bool wide = (instruction & 0x8000'0000U) != 0;
  const unsigned operation = (instruction >> 21) & 7U;
  if (((instruction >> 29) & 3U) != 0 || (operation != 0U && !wide) ||
      (operation != 0U && operation != 1U && operation != 2U &&
       operation != 5U && operation != 6U))
    return false;
  return (operation != 2U && operation != 6U) ||
         (((instruction >> 10) & 31U) == 31U && (instruction & 0x8000U) == 0);
}

[[nodiscard]] bool JitCompilerFamilies::valid_bit_reverse_count(
    std::uint32_t instruction) noexcept {
  const bool wide = (instruction & 0x8000'0000U) != 0;
  const unsigned operation = (instruction >> 10) & 31U;
  if (operation == 3U && !wide)
    return false;
  return operation <= 8U;
}

} // namespace uml::cpu::arm64::jit
