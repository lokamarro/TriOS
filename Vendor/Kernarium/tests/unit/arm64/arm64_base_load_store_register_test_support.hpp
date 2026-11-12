// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "arm64_engine_test_support.hpp"
#include "require.hpp"

#include "uml_darwin_guest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace arm64_load_store_register_test {

namespace arm64 = uml::cpu::arm64;
namespace engine_tests = uml::emulator::tests;

inline constexpr std::size_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
inline constexpr std::uint64_t data_base = engine_tests::engine_test_data_base;

struct IntegerVariant final {
  unsigned size_code;
  unsigned operation;
  std::size_t size;
  bool load;
  std::string_view name;
  std::string_view inventory_id;
};

inline constexpr std::array integer_variants{
    IntegerVariant{0, 0, 1, false, "strb", "BASE-0426"},
    IntegerVariant{0, 1, 1, true, "ldrb", "BASE-0226"},
    IntegerVariant{0, 2, 1, true, "ldrsb-x", "BASE-0230"},
    IntegerVariant{0, 3, 1, true, "ldrsb-w", "BASE-0230"},
    IntegerVariant{1, 0, 2, false, "strh", "BASE-0428"},
    IntegerVariant{1, 1, 2, true, "ldrh", "BASE-0228"},
    IntegerVariant{1, 2, 2, true, "ldrsh-x", "BASE-0232"},
    IntegerVariant{1, 3, 2, true, "ldrsh-w", "BASE-0232"},
    IntegerVariant{2, 0, 4, false, "str-w", "BASE-0424"},
    IntegerVariant{2, 1, 4, true, "ldr-w", "BASE-0223"},
    IntegerVariant{2, 2, 4, true, "ldrsw-x", "BASE-0235"},
    IntegerVariant{3, 0, 8, false, "str-x", "BASE-0424"},
    IntegerVariant{3, 1, 8, true, "ldr-x", "BASE-0223"},
};

inline constexpr std::array legal_options{2U, 3U, 6U, 7U};

[[nodiscard]] inline constexpr std::uint32_t
encode(unsigned size_code, unsigned operation, unsigned rm, unsigned option,
       bool scaled, unsigned mode, unsigned rn, unsigned rt) {
  return 0x3820'0000U | (size_code << 30) | (operation << 22) |
         ((rm & 31U) << 16) | ((option & 7U) << 13) |
         (static_cast<unsigned>(scaled) << 12) | ((mode & 3U) << 10) |
         ((rn & 31U) << 5) | (rt & 31U);
}

[[nodiscard]] inline arm64::CpuState seeded_state(std::uint64_t seed) {
  arm64::CpuState cpu{};
  for (unsigned index = 0; index < cpu.registers.size(); ++index)
    cpu.registers[index] =
        seed ^ (0x8000'0000'1000'0000ULL |
                (static_cast<std::uint64_t>(index) << 8) | index);
  cpu.sp = seed ^ 0x1234'5678'9abc'def0ULL;
  cpu.nzcv = 0xa000'0000U;
  cpu.fpcr = 0x0040'0000U;
  cpu.fpsr = 0x0800'009fU;
  cpu.tls = seed ^ 0xfedc'ba98'7654'3210ULL;
  for (unsigned index = 0; index < cpu.vectors.size(); ++index)
    for (unsigned byte = 0; byte < cpu.vectors[index].bytes.size(); ++byte)
      cpu.vectors[index].bytes[byte] =
          static_cast<std::byte>(seed + index * 23U + byte);
  return cpu;
}

[[nodiscard]] inline constexpr std::uint64_t sign_extend(std::uint64_t value,
                                                         unsigned bits) {
  const auto mask =
      bits == 64U ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1U;
  const auto sign = std::uint64_t{1} << (bits - 1U);
  value &= mask;
  return (value ^ sign) - sign;
}

[[nodiscard]] inline constexpr std::uint64_t
extend_offset(std::uint64_t value, unsigned option, unsigned shift) {
  switch (option) {
  case 2:
    value &= 0xffff'ffffU;
    break;
  case 3:
    break;
  case 6:
    value = sign_extend(value & 0xffff'ffffU, 32);
    break;
  case 7:
    break;
  default:
    return 0;
  }
  return value << shift;
}

[[nodiscard]] inline constexpr std::uint64_t
read_register(const arm64::CpuState &cpu, unsigned index, bool stack_pointer) {
  if (index != 31U)
    return cpu.registers[index];
  return stack_pointer ? cpu.sp : 0;
}

[[nodiscard]] inline constexpr std::uint64_t
address(const arm64::CpuState &cpu, const IntegerVariant &variant,
        unsigned option, bool scaled, unsigned rm, unsigned rn) {
  return read_register(cpu, rn, true) +
         extend_offset(read_register(cpu, rm, false), option,
                       scaled ? variant.size_code : 0U);
}

[[nodiscard]] inline std::uint64_t matrix_raw(unsigned option,
                                              unsigned register_index) {
  const auto magnitude = 1U + (register_index & 15U);
  switch (option) {
  case 2:
    return 0xfedc'ba98'0000'0000ULL | magnitude;
  case 3:
    return magnitude;
  case 6:
    return 0x1234'5678'0000'0000ULL |
           static_cast<std::uint32_t>(0U - magnitude);
  case 7:
    return 0U - static_cast<std::uint64_t>(magnitude);
  default:
    return 0;
  }
}

[[nodiscard]] inline constexpr std::uint64_t
expected_load(const IntegerVariant &variant, std::uint64_t raw) {
  if (variant.operation < 2U)
    return variant.size == 8U
               ? raw
               : raw & ((std::uint64_t{1} << (variant.size * 8U)) - 1U);
  auto value = sign_extend(raw, static_cast<unsigned>(variant.size * 8U));
  if (variant.operation == 3U)
    value &= 0xffff'ffffU;
  return value;
}

inline void fill_memory(std::span<std::byte> image, unsigned salt) {
  for (std::size_t offset = 0; offset < image.size(); ++offset)
    image[offset] =
        static_cast<std::byte>(0x80U | ((offset * 37U + salt * 29U) & 0x7fU));
}

[[nodiscard]] inline std::uint64_t
decode_little_endian(std::span<const std::byte> image, std::size_t offset,
                     std::size_t size) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < size; ++index)
    value |= static_cast<std::uint64_t>(
                 std::to_integer<unsigned>(image[offset + index]))
             << (index * 8U);
  return value;
}

inline void encode_little_endian(std::span<std::byte> image, std::size_t offset,
                                 std::size_t size, std::uint64_t value) {
  for (std::size_t index = 0; index < size; ++index)
    image[offset + index] = static_cast<std::byte>(value >> (index * 8U));
}

using uml::testing::require;

} // namespace arm64_load_store_register_test
