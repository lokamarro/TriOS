// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_compiler_families.hpp"

#include "cpu/arm64/detail/bfloat_dot.hpp"
#include "cpu/arm64/detail/bfloat_matrix.hpp"
#include "cpu/arm64/detail/bfloat_multiply.hpp"
#include "cpu/arm64/detail/floating_complex.hpp"
#include "cpu/arm64/detail/half_fused_long.hpp"
#include "cpu/arm64/detail/floating_compare.hpp"
#include "cpu/host_capabilities.hpp"

#include <array>
#include <cstddef>

namespace uml::cpu::arm64::jit {

[[nodiscard]] bool JitCompilerFamilies::recognized_simd_fmulx(
    std::uint32_t instruction) noexcept {
  return (instruction & 0xbfe0'fc00U) == 0x0e40'1c00U ||
         (instruction & 0xffe0'fc00U) == 0x5e40'1c00U ||
         (instruction & 0xbfa0'fc00U) == 0x0e20'dc00U ||
         (instruction & 0xffa0'fc00U) == 0x5e20'dc00U;
}

[[nodiscard]] bool JitCompilerFamilies::valid_simd_fmulx(
    std::uint32_t instruction) noexcept {
  if (!recognized_simd_fmulx(instruction))
    return false;
  const bool vector = (instruction & 0x1000'0000U) == 0;
  const bool half = (instruction & 0x00e0'0000U) == 0x0040'0000U;
  const bool double_precision =
      !half && (instruction & 0x0040'0000U) != 0;
  const bool q = (instruction & 0x4000'0000U) != 0;
  return !vector || !double_precision || q;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_simd_fmulx(std::uint32_t instruction) {
  const bool half = (instruction & 0x00e0'0000U) == 0x0040'0000U;
  if (!valid_simd_fmulx(instruction) ||
      !host_capabilities().advanced_simd ||
      (half && !host_capabilities().fp16))
    return false;
  emit_native_vector_binary(instruction, false);
  mark_native_fp();
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_simd_floating_complex(
    std::uint32_t instruction) {
  const auto decoded =
      arm64::detail::decode_floating_complex(instruction);
  if (!decoded.valid || !host_capabilities().advanced_simd ||
      !host_capabilities().has(ArmFeature::fcma) ||
      (decoded.width == 16U && !host_capabilities().fp16))
    return false;
  /* The decoded second register is Rm, bits 16-20, in every form. */
  if (decoded.operation ==
      arm64::detail::FloatingComplexOperation::add)
    emit_native_vector_binary(instruction, false);
  else
    emit_native_vector_accumulate(instruction, true);
  mark_native_fp();
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_bfloat_dot(std::uint32_t instruction) {
  const auto decoded = arm64::detail::decode_bfloat_dot(instruction);
  if (!decoded.recognized ||
      !host_capabilities().has(ArmFeature::bf16))
    return false;
  emit_native_vector_binary(instruction, true);
  mark_native_fp();
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_half_fused_long(std::uint32_t instruction) {
  const auto decoded =
      arm64::detail::decode_half_fused_long(instruction);
  const auto &host = host_capabilities();
  /* FEAT_AFP is not required: the native FMLAL/FMLSL follow FPCR.AH and FIZ
   * where the virtual CPU implements FEAT_AFP and the controls are RES0
   * elsewhere, so they compute the architected result on every host with
   * FHM. */
  if (!decoded.recognized || !host.has(ArmFeature::fhm))
    return false;
  if (decoded.by_element)
    emit_native_vector_by_element(instruction);
  else
    emit_native_vector_binary(instruction, true);
  mark_native_fp();
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_bfloat_multiply_long(
    std::uint32_t instruction) {
  const auto decoded =
      arm64::detail::decode_bfloat_multiply_long(instruction);
  if (!decoded.recognized ||
      !host_capabilities().has(ArmFeature::bf16))
    return false;
  if (decoded.by_element)
    emit_native_vector_by_element(instruction);
  else
    emit_native_vector_binary(instruction, true);
  mark_native_fp();
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_bfloat_matrix(std::uint32_t instruction) {
  if (!arm64::detail::decode_bfloat_matrix_multiply(instruction) ||
      !host_capabilities().has(ArmFeature::bf16))
    return false;
  emit_native_vector_binary(instruction, true);
  mark_native_fp();
  return true;
}

[[nodiscard]] bool JitCompilerFamilies::recognized_simd_dot(
    std::uint32_t instruction) noexcept {
  const bool vector_ordinary =
      (instruction & 0x9f20'fc00U) == 0x0e00'9400U;
  const bool vector_mixed =
      (instruction & 0xbf20'fc00U) == 0x0e00'9c00U;
  const bool scalar_ordinary =
      (instruction & 0xdf20'fc00U) == 0x5e00'9400U;
  const bool scalar_mixed =
      (instruction & 0xff20'fc00U) == 0x5e00'9c00U;
  const bool indexed_vector =
      (instruction & 0x9f00'0400U) == 0x0f00'0000U;
  const bool indexed_scalar =
      (instruction & 0xdf00'0400U) == 0x5f00'0000U;
  if (vector_ordinary || vector_mixed ||
      scalar_ordinary || scalar_mixed)
    return true;
  if (!indexed_vector && !indexed_scalar)
    return false;
  const bool unsigned_group = (instruction & 0x2000'0000U) != 0U;
  const unsigned size = (instruction >> 22) & 3U;
  const unsigned operation = (instruction >> 12) & 15U;
  return operation == 14U ||
         (!unsigned_group && operation == 15U &&
          (size == 0U || size == 2U));
}

[[nodiscard]] bool JitCompilerFamilies::valid_simd_dot(
    std::uint32_t instruction) noexcept {
  if (!recognized_simd_dot(instruction))
    return false;
  if ((instruction & 0x1000'0000U) != 0U)
    return false;
  const bool indexed =
      (instruction & 0x9f00'0400U) == 0x0f00'0000U;
  const unsigned size = (instruction >> 22) & 3U;
  if (!indexed)
    return size == 2U;
  const unsigned operation = (instruction >> 12) & 15U;
  return operation == 15U || size == 2U;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_simd_dot(std::uint32_t instruction) {
  if (!valid_simd_dot(instruction) ||
      !host_capabilities().advanced_simd)
    return false;
  const bool indexed =
      (instruction & 0x9f00'0400U) == 0x0f00'0000U;
  const bool mixed = indexed
                         ? ((instruction >> 12) & 15U) == 15U
                         : (instruction & 0xbf20'fc00U) == 0x0e00'9c00U;
  const auto feature = mixed ? ArmFeature::i8mm : ArmFeature::dotprod;
  if (!host_capabilities().has(feature))
    return false;
  /* The indexed forms' M:Rm is bits 16-20 as well. */
  emit_native_vector_binary(instruction, true);
  return true;
}

[[nodiscard]] bool JitCompilerFamilies::recognized_simd_matrix(
    std::uint32_t instruction) noexcept {
  return (instruction & 0x9f20'f400U) == 0x0e00'a400U;
}

[[nodiscard]] bool JitCompilerFamilies::valid_simd_matrix(
    std::uint32_t instruction) noexcept {
  if (!recognized_simd_matrix(instruction))
    return false;
  const bool q = (instruction & 0x4000'0000U) != 0;
  const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
  const unsigned size = (instruction >> 22) & 3U;
  const bool alternate = (instruction & 0x800U) != 0;
  return q && size == 2U && !(unsigned_group && alternate);
}

[[nodiscard]] bool
JitCompilerFamilies::emit_simd_matrix(std::uint32_t instruction) {
  if (!valid_simd_matrix(instruction) ||
      !host_capabilities().advanced_simd ||
      !host_capabilities().has(ArmFeature::i8mm))
    return false;
  emit_native_vector_binary(instruction, true);
  return true;
}

[[nodiscard]] bool JitCompilerFamilies::recognized_simd_floating_indexed(
    std::uint32_t instruction) noexcept {
  const bool vector = (instruction & 0x9f00'0400U) == 0x0f00'0000U;
  const bool scalar = (instruction & 0xdf00'0400U) == 0x5f00'0000U;
  if (!vector && !scalar)
    return false;
  const bool extended = (instruction & 0x2000'0000U) != 0;
  const unsigned size = (instruction >> 22) & 3U;
  const unsigned operation = (instruction >> 12) & 15U;
  const bool floating =
      operation == 9U || (!extended && (operation == 1U || operation == 5U));
  if (!floating)
    return false;
  /* The vector size=01, ordinary-multiply slot is FDOT after FEAT_F16F32DOT,
   * not a reserved member of this family. */
  return !(vector && size == 1U && !extended && operation == 9U);
}

[[nodiscard]] bool JitCompilerFamilies::valid_simd_floating_indexed(
    std::uint32_t instruction) noexcept {
  if (!recognized_simd_floating_indexed(instruction))
    return false;
  const bool vector = (instruction & 0x1000'0000U) == 0;
  const bool q = (instruction & 0x4000'0000U) != 0;
  const unsigned size = (instruction >> 22) & 3U;
  return size != 1U && !(size == 3U && vector && !q) &&
         !(size == 3U && (instruction & 0x0020'0000U) != 0);
}

[[nodiscard]] bool JitCompilerFamilies::recognized_simd_integer_indexed(
    std::uint32_t instruction) noexcept {
  const bool vector = (instruction & 0x9f00'0400U) == 0x0f00'0000U;
  const bool scalar = (instruction & 0xdf00'0400U) == 0x5f00'0000U;
  if (!vector && !scalar)
    return false;
  const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
  const unsigned operation = (instruction >> 12) & 15U;
  if (unsigned_group)
    return operation == 0U || operation == 2U || operation == 4U ||
           operation == 6U || operation == 10U || operation == 13U ||
           operation == 15U;
  return operation == 2U || operation == 3U || operation == 6U ||
         operation == 7U || operation == 8U || operation == 10U ||
         operation == 11U || operation == 12U || operation == 13U;
}

[[nodiscard]] bool JitCompilerFamilies::valid_simd_integer_indexed(
    std::uint32_t instruction) noexcept {
  if (!recognized_simd_integer_indexed(instruction))
    return false;
  const unsigned size = (instruction >> 22) & 3U;
  if (size != 1U && size != 2U)
    return false;
  const bool scalar = (instruction & 0x1000'0000U) != 0;
  if (!scalar)
    return true;
  const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
  const unsigned operation = (instruction >> 12) & 15U;
  return unsigned_group
             ? operation == 13U || operation == 15U
             : operation == 3U || operation == 7U || operation == 11U ||
                   operation == 12U || operation == 13U;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_simd_integer_indexed(
    std::uint32_t instruction) {
  if (!valid_simd_integer_indexed(instruction) ||
      !host_capabilities().advanced_simd)
    return false;
  const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
  const unsigned operation = (instruction >> 12) & 15U;
  if (unsigned_group && (operation == 13U || operation == 15U) &&
      !host_capabilities().has(ArmFeature::rdm))
    return false;

  const unsigned size = (instruction >> 22) & 3U;
  const unsigned destination = instruction & 31U;
  const unsigned left = (instruction >> 5) & 31U;
  unsigned element = (instruction >> 16) & 15U;
  if (size == 2U)
    element |= ((instruction >> 20) & 1U) << 4;

  const bool reads_destination =
      operation == 0U || operation == 2U || operation == 3U ||
      operation == 4U || operation == 6U || operation == 7U ||
      (unsigned_group && (operation == 13U || operation == 15U));
  NativeVector native_left;
  unsigned native_element;
  unsigned destination_fallback;
  if (size == 1U) {
    const auto constrained =
        read_constrained_native_vector(element, 2U);
    native_element = constrained.host;
    native_left =
        left == element ? constrained : read_native_vector(left, 1U);
    destination_fallback = 0U;
  } else {
    native_left = read_native_vector(left, 0U);
    native_element =
        element == left ? native_left.host
                        : read_native_vector(element, 1U).host;
    destination_fallback = 2U;
  }
  const auto native_destination =
      write_native_vector(destination, reads_destination,
                          destination_fallback);

  auto native = instruction & ~(0x000f'0000U | (31U << 5) | 31U);
  /* In the 32-bit indexed shape, M is Rm<4>; in the 16-bit shape it
   * remains one of the three lane-index bits. */
  if (size == 2U)
    native &= ~0x0010'0000U;
  native |= (native_element << 16) | (native_left.host << 5) |
            native_destination.host;
  emit(native);
  commit_native_vector(native_destination, destination);

  const bool saturating =
      (!unsigned_group && (operation == 3U || operation == 7U ||
                           operation == 11U || operation == 12U ||
                           operation == 13U)) ||
      (unsigned_group && (operation == 13U || operation == 15U));
  if (saturating)
    mark_native_fp();
  return true;
}

[[nodiscard]] bool JitCompilerFamilies::recognized_simd_rdm(
    std::uint32_t instruction) noexcept {
  return (instruction & 0xbf20'f400U) == 0x2e00'8400U ||
         (instruction & 0xff20'f400U) == 0x7e00'8400U;
}

[[nodiscard]] bool JitCompilerFamilies::valid_simd_rdm(
    std::uint32_t instruction) noexcept {
  if (!recognized_simd_rdm(instruction))
    return false;
  const unsigned size = (instruction >> 22) & 3U;
  return size == 1U || size == 2U;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_simd_rdm(std::uint32_t instruction) {
  if (!valid_simd_rdm(instruction) ||
      !host_capabilities().advanced_simd ||
      !host_capabilities().has(ArmFeature::rdm))
    return false;
  emit_native_vector_accumulate(instruction, true);
  mark_native_fp();
  return true;
}

[[nodiscard]] bool JitCompilerFamilies::emit_simd_floating_compare(
    std::uint32_t instruction) {
  const auto decoded =
      arm64::detail::decode_simd_floating_compare(instruction);
  if (!decoded.valid || !host_capabilities().advanced_simd ||
      (decoded.width == 16U && !host_capabilities().fp16))
    return false;

  if (decoded.zero) {
    const unsigned destination = instruction & 31U;
    const unsigned left = (instruction >> 5) & 31U;
    const auto native_left = read_native_vector(left, 0U);
    const auto native_destination =
        write_native_vector(destination, false, 2U);
    emit((instruction & ~0x3ffU) | (native_left.host << 5) |
         native_destination.host);
    commit_native_vector(native_destination, destination);
  } else {
    emit_native_vector_binary(instruction, false);
  }
  mark_native_fp();
  return true;
}

[[nodiscard]] bool JitCompilerFamilies::valid_simd_three_same_integer(
    std::uint32_t instruction) noexcept {
  const bool vector = (instruction & 0x9f20'0400U) == 0x0e20'0400U;
  const bool scalar = (instruction & 0xdf20'0400U) == 0x5e20'0400U;
  if (!vector && !scalar)
    return false;
  const unsigned opcode = (instruction >> 11) & 31U;
  if (opcode >= 24U)
    return false;
  const bool q = (instruction & 0x4000'0000U) != 0;
  const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
  const unsigned size = (instruction >> 22) & 3U;
  if (scalar) {
    if (opcode == 1U || opcode == 5U || opcode == 9U || opcode == 11U)
      return true;
    if (opcode == 6U || opcode == 7U || opcode == 8U || opcode == 10U ||
        opcode == 16U || opcode == 17U)
      return size == 3U;
    return opcode == 22U && (size == 1U || size == 2U);
  }
  if (opcode == 3U)
    return true;
  if (!q && size == 3U)
    return false;
  if (size == 3U)
    return opcode == 1U || opcode == 5U || opcode == 6U || opcode == 7U ||
           opcode == 8U || opcode == 9U || opcode == 10U || opcode == 11U ||
           opcode == 16U || opcode == 17U ||
           (opcode == 23U && !unsigned_group);
  if (opcode == 19U && unsigned_group)
    return size == 0U;
  if (opcode == 22U)
    return size == 1U || size == 2U;
  if (opcode == 23U)
    return !unsigned_group;
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_simd_bitwise_register(std::uint32_t instruction) {
  if (!host_capabilities().advanced_simd ||
      (instruction & 0x9f20'fc00U) != 0x0e20'1c00U)
    return false;

  const unsigned operation =
      ((instruction >> 22) & 3U) | ((instruction >> 27) & 4U);
  const unsigned destination = instruction & 31U;
  const unsigned left = (instruction >> 5) & 31U;
  const unsigned right = (instruction >> 16) & 31U;
  const bool q = (instruction & 0x4000'0000U) != 0;
  if (operation == 2U && left == right && q && destination == left)
    return true;
  emit_native_vector_binary(instruction, operation >= 5U);
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_simd_three_same_integer(std::uint32_t instruction) {
  if (!host_capabilities().advanced_simd ||
      !valid_simd_three_same_integer(instruction))
    return false;
  const unsigned opcode = (instruction >> 11) & 31U;
  /* Only ABA and MLA/MLS read the old destination. This decision is made
   * while translating so ordinary arithmetic does not pay an unnecessary
   * live-in load and the emitted instruction remains one-for-one. */
  emit_native_vector_binary(instruction, opcode == 15U || opcode == 18U);
  if (opcode == 1U || opcode == 5U || opcode == 9U || opcode == 11U ||
      opcode == 22U)
    mark_native_fp();
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_simd_arithmetic_narrow(std::uint32_t instruction) {
  if (!host_capabilities().advanced_simd)
    return false;
  const auto operation = instruction & 0x9f20'fc00U;
  if ((operation != 0x0e20'4000U && operation != 0x0e20'6000U) ||
      ((instruction >> 22) & 3U) == 3U)
    return false;
  const bool upper = (instruction & 0x4000'0000U) != 0;
  emit_native_vector_binary(instruction, upper);
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_simd_widening(std::uint32_t instruction) {
  if (!host_capabilities().advanced_simd)
    return false;
  const bool vector = (instruction & 0x9f20'0c00U) == 0x0e20'0000U;
  const bool scalar = (instruction & 0xdf20'0c00U) == 0x5e20'0000U;
  if (!vector && !scalar)
    return false;
  const unsigned operation = (instruction >> 12) & 15U;
  const bool unsigned_values = (instruction & 0x2000'0000U) != 0;
  const unsigned size = (instruction >> 22) & 3U;
  if (size > 2U)
    return false;
  bool valid = false;
  if (scalar) {
    valid = !unsigned_values && size != 0U &&
            (operation == 9U || operation == 11U || operation == 13U);
  } else {
    switch (operation) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 5:
    case 7:
    case 8:
    case 10:
    case 12:
      valid = true;
      break;
    case 9:
    case 11:
    case 13:
      valid = !unsigned_values && size != 0U;
      break;
    default:
      break;
    }
  }
  if (!valid)
    return false;
  const bool accumulates = operation == 5U ||
                           (operation >= 8U && operation <= 11U);
  emit_native_vector_binary(instruction, accumulates);
  if (operation == 9U || operation == 11U || operation == 13U)
    mark_native_fp();
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_simd_narrow_move(std::uint32_t instruction) {
  if (!host_capabilities().advanced_simd ||
      (instruction & 0xbf3f'fc00U) != 0x0e21'2800U ||
      ((instruction >> 22) & 3U) == 3U)
    return false;
  const bool upper = (instruction & 0x4000'0000U) != 0;
  /*
   * Import Rn before defining Rd. Lower-half XTN overwrites/zeroes all of Rd,
   * whereas XTN2 preserves its lower half. The ordering still keeps Rd == Rn
   * correct: the aliased source is live before the zeroing form defines it.
   */
  emit_native_vector_unary(instruction, upper);
  return true;
}

[[nodiscard]] bool JitCompilerFamilies::emit_simd_two_register_integer(
    std::uint32_t instruction) {
  if (!host_capabilities().advanced_simd)
    return false;
  const bool vector = (instruction & 0x9f3e'0c00U) == 0x0e20'0800U;
  const bool scalar = (instruction & 0xdf3e'0c00U) == 0x5e20'0800U;
  if (!vector && !scalar)
    return false;
  const unsigned opcode = (instruction >> 12) & 31U;
  const bool unsigned_group = (instruction & 0x2000'0000U) != 0;
  const bool q = (instruction & 0x4000'0000U) != 0;
  const unsigned size = (instruction >> 22) & 3U;
  bool valid = false;
  switch (opcode) {
  case 2:
  case 4:
  case 6:
    valid = vector && size < 3U;
    break;
  case 3:
  case 7:
    valid = scalar || q || size < 3U;
    break;
  case 11:
    valid = scalar ? size == 3U : q || size < 3U;
    break;
  case 18:
    valid = size < 3U && (unsigned_group || vector);
    break;
  case 19:
    valid = size < 3U && unsigned_group && vector;
    break;
  case 20:
    valid = size < 3U;
    break;
  default:
    return false;
  }
  if (!valid)
    return false;

  const unsigned destination = instruction & 31U;
  const unsigned source = (instruction >> 5) & 31U;
  const bool reads_destination =
      opcode == 3U || opcode == 6U ||
      (q && (opcode == 18U || opcode == 20U));
  const bool destructive = reads_destination && destination == source;
  NativeVector native_source;
  NativeVector native_destination;
  if (destructive) {
    native_destination =
        write_native_vector(destination, true, 0U);
    native_source = native_destination;
  } else {
    native_source = read_native_vector(source, 1U);
    native_destination =
        write_native_vector(destination, reads_destination, 0U);
  }
  emit((instruction & ~((31U << 5) | 31U)) |
       (native_source.host << 5) | native_destination.host);
  commit_native_vector(native_destination, destination);
  if (opcode == 3U || opcode == 7U || opcode == 20U ||
      (opcode == 18U && unsigned_group))
    mark_native_fp();
  return true;
}

[[nodiscard]] bool JitCompilerFamilies::emit_simd_unsigned_estimate(
    std::uint32_t instruction) {
  if (!host_capabilities().advanced_simd ||
      (instruction & 0x9fbf'fc00U) != 0x0ea1'c800U ||
      (instruction & 0x0040'0000U) != 0)
    return false;
  emit_native_vector_unary(instruction, false);
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_simd_floating_indexed(std::uint32_t instruction) {
  if (!valid_simd_floating_indexed(instruction) ||
      !host_capabilities().floating_point || !host_capabilities().advanced_simd)
    return false;
  const unsigned size = (instruction >> 22) & 3U;
  if (size == 0U && !host_capabilities().fp16)
    return false;
  const unsigned destination = instruction & 31U;
  const unsigned left = (instruction >> 5) & 31U;
  unsigned element = (instruction >> 16) & 15U;
  if (size != 0U)
    element |= ((instruction >> 20) & 1U) << 4;
  NativeVector native_left;
  unsigned native_element;
  unsigned destination_fallback;
  if (size == 0U) {
    const auto constrained =
        read_constrained_native_vector(element, 2U);
    native_element = constrained.host;
    native_left =
        left == element ? constrained : read_native_vector(left, 1U);
    destination_fallback = 0U;
  } else {
    native_left = read_native_vector(left, 0U);
    native_element =
        element == left ? native_left.host
                        : read_native_vector(element, 1U).host;
    destination_fallback = 2U;
  }
  const unsigned operation = (instruction >> 12) & 15U;
  const bool reads_destination = operation == 1U || operation == 5U;
  const auto native_destination =
      write_native_vector(destination, reads_destination,
                          destination_fallback);
  auto native = instruction & ~(0x000f'0000U | (31U << 5) | 31U);
  if (size != 0U)
    native &= ~0x0010'0000U;
  native |= (native_element << 16) | (native_left.host << 5) |
            native_destination.host;
  emit(native);
  commit_native_vector(native_destination, destination);
  mark_native_fp();
  return true;
}


} // namespace uml::cpu::arm64::jit
