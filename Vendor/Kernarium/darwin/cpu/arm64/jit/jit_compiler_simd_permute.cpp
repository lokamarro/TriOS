// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_compiler_families.hpp"

#include "cpu/host_capabilities.hpp"

#include <array>
#include <cstddef>

namespace uml::cpu::arm64::jit {

[[nodiscard]] bool
JitCompilerFamilies::emit_simd_extract(std::uint32_t instruction) {
  if (!host_capabilities().advanced_simd ||
      (instruction & 0xbfe0'8400U) != 0x2e00'0000U)
    return false;
  const bool q = (instruction & 0x4000'0000U) != 0;
  const unsigned offset = (instruction >> 11) & 15U;
  if (!q && offset >= 8U)
    return false;
  emit_native_vector_binary(instruction, false);
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_simd_table_lookup(std::uint32_t instruction) {
  if (!host_capabilities().advanced_simd ||
      (instruction & 0xbfe0'8c00U) != 0x0e00'0000U)
    return false;
  const unsigned destination = instruction & 31U;
  const unsigned first_table = (instruction >> 5) & 31U;
  const unsigned index = (instruction >> 16) & 31U;
  const unsigned table_count = ((instruction >> 13) & 3U) + 1U;
  const bool extend = (instruction & 0x1000U) != 0;
  std::array<NativeVector, 4> native_tables{};
  for (unsigned table = 0; table < table_count; ++table)
    native_tables[table] = read_native_vector(
        (first_table + table) & 31U, table);

  bool consecutive = true;
  for (unsigned table = 1; table < table_count; ++table)
    consecutive &=
        native_tables[table].host ==
        ((native_tables[0].host + table) & 31U);
  unsigned native_first_table = native_tables[0].host;
  if (!consecutive) {
    /*
     * TBL/TBX require a consecutive physical table list. Only that list is
     * staged; ordinary index and destination operands remain resident.
     */
    for (unsigned table = 0; table < table_count; ++table)
      load_q(table, 15,
             offsetof(arm64::CpuState, vectors) +
                 ((first_table + table) & 31U) *
                     sizeof(arm64::VectorRegister));
    native_first_table = 0U;
  }
  const auto native_index = read_native_vector(index, 4U);
  const auto native_destination =
      write_native_vector(destination, extend, 5U);
  emit((instruction & ~((31U << 16) | (31U << 5) | 31U)) |
       (native_index.host << 16) | (native_first_table << 5) |
       native_destination.host);
  commit_native_vector(native_destination, destination);
  return true;
}

[[nodiscard]] bool
JitCompilerFamilies::emit_simd_permute(std::uint32_t instruction) {
  if (!host_capabilities().advanced_simd)
    return false;
  const auto operation = instruction & 0xbf20'fc00U;
  if (operation != 0x0e00'1800U && operation != 0x0e00'2800U &&
      operation != 0x0e00'3800U && operation != 0x0e00'5800U &&
      operation != 0x0e00'6800U && operation != 0x0e00'7800U)
    return false;
  const bool q = (instruction & 0x4000'0000U) != 0;
  const unsigned size = (instruction >> 22) & 3U;
  if (!q && size == 3U)
    return false;
  emit_native_vector_binary(instruction, false);
  return true;
}

} // namespace uml::cpu::arm64::jit
