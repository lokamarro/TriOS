// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_compiler_families.hpp"

#include "cpu/host_capabilities.hpp"

#include <cstddef>

namespace uml::cpu::arm64::jit {

[[nodiscard]] bool JitCompilerFamilies::emit_crc32(std::uint32_t instruction) {
  if (!host_capabilities().crc32 ||
      (instruction & 0x7fe0'e000U) != 0x1ac0'4000U)
    return false;
  const unsigned operation = (instruction >> 10) & 7U;
  const unsigned size = operation & 3U;
  if (((instruction >> 31) != 0) != (size == 3U))
    return false;
  const unsigned destination = instruction & 31U;
  if (destination == 31U)
    return true;
  const unsigned accumulator = (instruction >> 5) & 31U;
  const unsigned source = (instruction >> 16) & 31U;
  const auto native_accumulator =
      read_native_gpr(accumulator, false, 8U);
  const auto native_source =
      source == accumulator ? native_accumulator
                            : read_native_gpr(source, false, 9U);
  const auto native_destination =
      write_native_gpr(destination, false, false, 10U);
  emit((instruction & ~0x001f'03ffU) | (native_source.host << 16) |
       (native_accumulator.host << 5) | native_destination.host);
  commit_native_gpr(native_destination, destination, false);
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_simd_aes(std::uint32_t instruction) {
  const auto &features = host_capabilities();
  if (!features.advanced_simd || !features.aes ||
      (instruction & 0xffff'8c00U) != 0x4e28'0800U)
    return false;
  const unsigned operation = (instruction >> 12) & 7U;
  if (operation < 4U)
    return false;
  emit_native_vector_accumulate(instruction, false);
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_simd_pmull(std::uint32_t instruction) {
  if ((instruction & 0xbf20'fc00U) != 0x0e20'e000U)
    return false;
  const unsigned size = (instruction >> 22) & 3U;
  if (size != 0U && size != 3U)
    return false;
  const auto &features = host_capabilities();
  if (!features.advanced_simd || (size == 3U && !features.pmull))
    return false;
  emit_native_vector_binary(
      instruction, false, {.left = 1U, .right = 2U, .destination = 0U});
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_simd_sha1_sha2(std::uint32_t instruction) {
  const bool three = (instruction & 0xffe0'8c00U) == 0x5e00'0000U;
  const bool two = (instruction & 0xffff'cc00U) == 0x5e28'0800U;
  if (!three && !two)
    return false;
  const unsigned operation = (instruction >> 12) & (three ? 7U : 3U);
  const bool sha1 = three ? operation <= 3U : operation <= 1U;
  const bool sha2 =
      three ? operation >= 4U && operation <= 6U : operation == 2U;
  if ((!sha1 && !sha2) || (sha1 && !host_capabilities().sha1) ||
      (sha2 && !host_capabilities().sha2))
    return false;
  emit_native_vector_accumulate(instruction, three);
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_simd_sha512(std::uint32_t instruction) {
  if (!host_capabilities().sha512)
    return false;
  const auto operation = instruction & 0xffe0'fc00U;
  const bool three = operation == 0xce60'8000U || operation == 0xce60'8400U ||
                     operation == 0xce60'8800U;
  const bool two = (instruction & 0xffff'fc00U) == 0xcec0'8000U;
  if (!three && !two)
    return false;
  emit_native_vector_accumulate(instruction, three);
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_simd_sha3(std::uint32_t instruction) {
  if (!host_capabilities().sha3)
    return false;
  const auto three_operation = instruction & 0xffe0'fc00U;
  const bool four = (instruction & 0xffe0'8000U) == 0xce00'0000U ||
                    (instruction & 0xffe0'8000U) == 0xce20'0000U;
  const bool three = three_operation == 0xce60'8c00U ||
                     (instruction & 0xffe0'0000U) == 0xce80'0000U;
  if (!four && !three)
    return false;
  if (three) {
    emit_native_vector_binary(
        instruction, false, {.left = 1U, .right = 2U, .destination = 0U});
    return true;
  }
  const unsigned destination = instruction & 31U;
  const unsigned first = (instruction >> 5) & 31U;
  const unsigned second = (instruction >> 16) & 31U;
  const unsigned third = (instruction >> 10) & 31U;
  const auto native_first = read_native_vector(first, 1U);
  const auto native_second =
      second == first ? native_first : read_native_vector(second, 2U);
  const auto native_third =
      third == first
          ? native_first
          : third == second ? native_second : read_native_vector(third, 3U);
  const auto native_destination =
      write_native_vector(destination, false, 0U);
  emit((instruction & ~0x001f'7fffU) | (native_second.host << 16) |
       (native_third.host << 10) | (native_first.host << 5) |
       native_destination.host);
  commit_native_vector(native_destination, destination);
  return true;
}

} // namespace uml::cpu::arm64::jit
