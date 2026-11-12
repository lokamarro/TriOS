// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/jit/jit_test_api.hpp"
#include "platform/apple/host_features.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace jit_test = uml::cpu::arm64::jit::testing;

namespace {

constexpr std::uint32_t register_mask =
    (31U << 16U) | (31U << 5U) | 31U;
constexpr std::uint32_t constrained_register_mask =
    (15U << 16U) | (31U << 5U) | 31U;
constexpr std::uint32_t two_register_mask = (31U << 5U) | 31U;
constexpr unsigned first_cached_vector = 16U;
constexpr unsigned last_cached_vector = 31U;
constexpr std::uint64_t code_base = 0x4f10'0000ULL;

using uml::testing::require;

[[nodiscard]] constexpr std::uint32_t
bitwise(unsigned operation, unsigned destination, unsigned left,
        unsigned right) noexcept {
  return 0x4e20'1c00U | ((operation & 3U) << 22U) |
         ((operation & 4U) << 27U) | ((right & 31U) << 16U) |
         ((left & 31U) << 5U) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
polynomial_multiply(unsigned destination, unsigned left,
                    unsigned right) noexcept {
  return 0x0e20'e000U | ((right & 31U) << 16U) |
         ((left & 31U) << 5U) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
modified_immediate(bool combine, bool clear, unsigned immediate,
                   unsigned destination) noexcept {
  const unsigned mode = combine ? 1U : 14U;
  return 0x4f00'0400U | (clear ? 0x2000'0000U : 0U) |
         ((immediate & 0xe0U) << 11U) | (mode << 12U) |
         ((immediate & 0x1fU) << 5U) | (destination & 31U);
}

/*
 * The six unrestricted operations bind V0-V15. The final definitions bind
 * V16/V17, filling q6/q7 after q16-q31. If a following constrained operation
 * prebinds V15 in q6, V16 fills the released high slot and V17 fills q7, so
 * all eighteen slots are still occupied before the checked fallback.
 */
constexpr std::array<std::uint32_t, 8> full_vector_cache_fillers{
    bitwise(4, 0, 1, 2),    bitwise(4, 3, 4, 5),
    bitwise(4, 6, 7, 8),    bitwise(4, 9, 10, 11),
    bitwise(4, 12, 13, 14), bitwise(4, 15, 0, 1),
    modified_immediate(false, false, 0x51U, 16U),
    modified_immediate(false, false, 0x52U, 17U),
};

[[nodiscard]] constexpr std::uint32_t
vector_unsigned_memory(bool load, unsigned base, unsigned transfer) noexcept {
  return 0x3d00'0000U | ((load ? 3U : 2U) << 22U) |
         ((base & 31U) << 5U) | (transfer & 31U);
}

[[nodiscard]] constexpr bool is_cached_gpr(unsigned reg) noexcept {
  return reg == 3U || reg == 4U || reg == 5U || reg == 6U ||
         reg == 7U || reg == 12U || reg == 13U || reg == 17U;
}

[[nodiscard]] constexpr unsigned destination(std::uint32_t instruction) {
  return instruction & 31U;
}

[[nodiscard]] constexpr unsigned left(std::uint32_t instruction) {
  return (instruction >> 5U) & 31U;
}

[[nodiscard]] constexpr unsigned right(std::uint32_t instruction) {
  return (instruction >> 16U) & 31U;
}

struct RegisterMap final {
  std::array<std::optional<unsigned>, 32> host{};

  void bind(unsigned guest, unsigned native) {
    require(native >= first_cached_vector && native <= last_cached_vector,
            "generated operation escaped the v16-v31 cache");
    auto &binding = host[guest];
    if (binding.has_value())
      require(*binding == native,
              "guest vector changed host binding inside the block");
    else
      binding = native;
  }
};

void bind_instruction(RegisterMap &mapping, std::uint32_t guest,
                      std::uint32_t native) {
  if (left(guest) != right(guest))
    require(left(native) != right(native),
            "distinct vector inputs share one host register");
  mapping.bind(destination(guest), destination(native));
  mapping.bind(left(guest), left(native));
  mapping.bind(right(guest), right(native));
}

[[maybe_unused, nodiscard]] bool
is_direct_remapped_run(std::span<const std::uint32_t> native,
                       std::span<const std::uint32_t> guest) {
  if (native.size() < guest.size())
    return false;

  for (std::size_t index = 0; index < guest.size(); ++index)
    if ((native[index] & ~register_mask) !=
        (guest[index] & ~register_mask))
      return false;

  RegisterMap mapping;
  for (std::size_t index = 0; index < guest.size(); ++index)
    bind_instruction(mapping, guest[index], native[index]);

  /*
   * Every result is the next operation's left input. Consecutive matching
   * words and identical bindings prove there is no CpuState vector
   * store/load pair between the guest operations.
   */
  for (std::size_t index = 1; index < guest.size(); ++index)
    require(destination(native[index - 1]) == left(native[index]),
            "vector dependency was not kept resident");
  return true;
}


} // namespace
