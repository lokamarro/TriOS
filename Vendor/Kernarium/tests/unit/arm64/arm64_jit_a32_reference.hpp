// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* A reference for the AArch32 integer core that the JIT tests compare
 * against: the ARM ARM pseudocode written straight from the instruction
 * fields, with no use of the decoder or of any code the JIT shares. It
 * covers the classes the tests generate (data processing, shifts, the
 * multiplies and the bit and extension instructions). */

#include <array>
#include <bit>
#include <cstdint>

namespace uml::emulator::tests::a32::ref {

struct State final {
  std::array<std::uint32_t, 16> r{};
  bool n{}, z{}, c{}, v{}, q{};
  std::uint32_t ge{}; // GE[3:0]

  [[nodiscard]] std::uint32_t nzcv() const noexcept {
    return (n ? 1U << 31 : 0U) | (z ? 1U << 30 : 0U) | (c ? 1U << 29 : 0U) |
           (v ? 1U << 28 : 0U);
  }
  void set_nzcv(std::uint32_t value) noexcept {
    n = value >> 31 & 1U;
    z = value >> 30 & 1U;
    c = value >> 29 & 1U;
    v = value >> 28 & 1U;
  }
};

[[nodiscard]] inline bool condition_passed(const State &s,
                                           unsigned cond) noexcept {
  bool result;
  switch (cond >> 1) {
  case 0: result = s.z; break;
  case 1: result = s.c; break;
  case 2: result = s.n; break;
  case 3: result = s.v; break;
  case 4: result = s.c && !s.z; break;
  case 5: result = s.n == s.v; break;
  case 6: result = s.n == s.v && !s.z; break;
  default: return true;
  }
  if ((cond & 1U) != 0U && cond != 15U)
    result = !result;
  return result;
}

enum class Shift : unsigned { lsl = 0, lsr = 1, asr = 2, ror = 3, rrx = 4 };

struct Shifted final {
  std::uint32_t value;
  bool carry;
};

/* Shift_C with the amount already decoded (immediate shifts have mapped #0
 * of LSR/ASR to 32 and of ROR to RRX; register shifts pass Rs<7:0>). */
[[nodiscard]] inline Shifted shift_c(std::uint32_t x, Shift type,
                                     unsigned amount, bool carry_in) noexcept {
  if (type == Shift::rrx)
    return {static_cast<std::uint32_t>((carry_in ? 1U << 31 : 0U) | x >> 1),
            (x & 1U) != 0U};
  if (amount == 0)
    return {x, carry_in};
  switch (type) {
  case Shift::lsl:
    if (amount > 32) return {0, false};
    if (amount == 32) return {0, (x & 1U) != 0U};
    return {x << amount, (x >> (32 - amount) & 1U) != 0U};
  case Shift::lsr:
    if (amount > 32) return {0, false};
    if (amount == 32) return {0, (x >> 31 & 1U) != 0U};
    return {x >> amount, (x >> (amount - 1) & 1U) != 0U};
  case Shift::asr: {
    const auto sign = static_cast<std::int32_t>(x) < 0;
    if (amount >= 32)
      return {sign ? 0xffff'ffffU : 0U, sign};
    return {static_cast<std::uint32_t>(static_cast<std::int32_t>(x) >> amount),
            (x >> (amount - 1) & 1U) != 0U};
  }
  case Shift::ror: {
    const unsigned m = amount % 32U;
    const auto result = m == 0 ? x : std::rotr(x, static_cast<int>(m));
    return {result, (result >> 31 & 1U) != 0U};
  }
  case Shift::rrx:
    break;
  }
  return {x, carry_in};
}

struct Sum final {
  std::uint32_t value;
  bool carry, overflow;
};

[[nodiscard]] inline Sum add_with_carry(std::uint32_t x, std::uint32_t y,
                                        bool carry_in) noexcept {
  const std::uint64_t unsigned_sum =
      std::uint64_t{x} + std::uint64_t{y} + (carry_in ? 1U : 0U);
  const std::int64_t signed_sum =
      std::int64_t{static_cast<std::int32_t>(x)} +
      std::int64_t{static_cast<std::int32_t>(y)} + (carry_in ? 1 : 0);
  const auto result = static_cast<std::uint32_t>(unsigned_sum);
  return {result, std::uint64_t{result} != unsigned_sum,
          std::int64_t{static_cast<std::int32_t>(result)} != signed_sum};
}

/* The data-processing opcodes in their A32 encoding order. */
enum class DpOp : unsigned {
  and_ = 0, eor, sub, rsb, add, adc, sbc, rsc, tst, teq, cmp, cmn, orr, mov,
  bic, mvn,
};

[[nodiscard]] inline bool dp_tests_only(DpOp op) noexcept {
  return op >= DpOp::tst && op <= DpOp::cmn;
}

/* One data-processing operation on its first operand and second operand
 * (already shifted, with the shifter's carry-out). Returns the result and
 * updates the flags when `set_flags`. */
inline std::uint32_t data_processing(State &s, DpOp op, std::uint32_t rn,
                                     std::uint32_t operand2, bool shifter_carry,
                                     bool set_flags) noexcept {
  std::uint32_t result = 0;
  bool carry = shifter_carry, overflow = s.v;
  bool arithmetic = false;
  switch (op) {
  case DpOp::and_: case DpOp::tst: result = rn & operand2; break;
  case DpOp::eor: case DpOp::teq: result = rn ^ operand2; break;
  case DpOp::orr: result = rn | operand2; break;
  case DpOp::bic: result = rn & ~operand2; break;
  case DpOp::mov: result = operand2; break;
  case DpOp::mvn: result = ~operand2; break;
  case DpOp::sub: case DpOp::cmp: {
    const auto sum = add_with_carry(rn, ~operand2, true);
    result = sum.value; carry = sum.carry; overflow = sum.overflow;
    arithmetic = true; break;
  }
  case DpOp::rsb: {
    const auto sum = add_with_carry(~rn, operand2, true);
    result = sum.value; carry = sum.carry; overflow = sum.overflow;
    arithmetic = true; break;
  }
  case DpOp::add: case DpOp::cmn: {
    const auto sum = add_with_carry(rn, operand2, false);
    result = sum.value; carry = sum.carry; overflow = sum.overflow;
    arithmetic = true; break;
  }
  case DpOp::adc: {
    const auto sum = add_with_carry(rn, operand2, s.c);
    result = sum.value; carry = sum.carry; overflow = sum.overflow;
    arithmetic = true; break;
  }
  case DpOp::sbc: {
    const auto sum = add_with_carry(rn, ~operand2, s.c);
    result = sum.value; carry = sum.carry; overflow = sum.overflow;
    arithmetic = true; break;
  }
  case DpOp::rsc: {
    const auto sum = add_with_carry(~rn, operand2, s.c);
    result = sum.value; carry = sum.carry; overflow = sum.overflow;
    arithmetic = true; break;
  }
  }
  if (set_flags) {
    s.n = (result >> 31 & 1U) != 0U;
    s.z = result == 0;
    s.c = carry;
    if (arithmetic)
      s.v = overflow;
  }
  return result;
}

[[nodiscard]] inline std::uint32_t arm_expand_imm(unsigned imm12) noexcept {
  return std::rotr(static_cast<std::uint32_t>(imm12 & 0xffU),
                   static_cast<int>(2U * (imm12 >> 8)));
}

/* ARMExpandImm_C. */
[[nodiscard]] inline Shifted arm_expand_imm_c(unsigned imm12,
                                              bool carry_in) noexcept {
  const auto value = arm_expand_imm(imm12);
  const unsigned rot = 2U * (imm12 >> 8);
  return {value, rot == 0 ? carry_in : (value >> 31 & 1U) != 0U};
}

} // namespace uml::emulator::tests::a32::ref
