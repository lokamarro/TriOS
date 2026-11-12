// SPDX-License-Identifier: GPL-2.0-or-later
#include "platform/apple/host_features.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "require.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace jit_test = uml::cpu::arm64::jit::testing;

namespace {

constexpr std::uint32_t register_mask =
    (31U << 16U) | (31U << 5U) | 31U;
constexpr std::uint64_t code_base = 0x4f70'0000ULL;

using uml::testing::require;

[[nodiscard]] constexpr std::uint32_t
bitwise_xor(unsigned destination, unsigned left, unsigned right) noexcept {
  return 0x6e20'1c00U | ((right & 31U) << 16U) |
         ((left & 31U) << 5U) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
table_lookup(bool extend, unsigned count, unsigned destination,
             unsigned first, unsigned indices) noexcept {
  return 0x4e00'0000U | ((indices & 31U) << 16U) |
         (((count - 1U) & 3U) << 13U) |
         (extend ? 0x1000U : 0U) | ((first & 31U) << 5U) |
         (destination & 31U);
}

[[nodiscard]] constexpr unsigned destination(std::uint32_t word) noexcept {
  return word & 31U;
}

[[nodiscard]] constexpr unsigned first(std::uint32_t word) noexcept {
  return (word >> 5U) & 31U;
}

[[nodiscard]] constexpr unsigned indices(std::uint32_t word) noexcept {
  return (word >> 16U) & 31U;
}

[[nodiscard]] constexpr bool
is_vector_move(std::uint32_t word) noexcept {
  return (word & ~register_mask) ==
         (0x4ea0'1c00U & ~register_mask);
}

[[nodiscard]] constexpr bool
is_cpu_state_vector_load(std::uint32_t word) noexcept {
  return (word & 0xffc0'03e0U) == 0x3dc0'01e0U;
}

[[nodiscard]] constexpr bool
is_cpu_state_vector_store(std::uint32_t word) noexcept {
  return (word & 0xffc0'03e0U) == 0x3d80'01e0U;
}

[[nodiscard]] std::span<const std::uint32_t>
hot_code(const jit_test::CompiledCode &compiled) {
  require(compiled.hot_words <= compiled.words.size(),
          "JIT hot boundary exceeds generated table-list code");
  return std::span<const std::uint32_t>{compiled.words}
      .first(compiled.hot_words);
}

[[nodiscard]] std::optional<std::uint32_t>
find_native(std::span<const std::uint32_t> hot,
            std::uint32_t guest) {
  std::optional<std::uint32_t> result;
  for (const auto word : hot) {
    if ((word & ~register_mask) != (guest & ~register_mask))
      continue;
    require(!result.has_value(),
            "JIT emitted a table-list semantic word twice");
    result = word;
  }
  return result;
}

[[nodiscard]] std::size_t
count_if(std::span<const std::uint32_t> words,
         bool (*predicate)(std::uint32_t)) {
  std::size_t result{};
  for (const auto word : words)
    result += predicate(word);
  return result;
}

void single_four_member_list_is_planned() {
  if (!::uml::platform::apple::probed_host_capabilities().advanced_simd)
    return;
  constexpr auto target = table_lookup(false, 4U, 0U, 1U, 5U);
  constexpr std::array guest{target};
  const auto compiled = jit_test::compile_block(code_base, guest);
  require(compiled.guest_instructions == guest.size(),
          "JIT did not compile a four-member table list");
  const auto hot = hot_code(compiled);
  const auto native = find_native(hot, target);
  require(native.has_value(),
          "JIT omitted a four-member table semantic word");
  require(first(*native) == 28U && indices(*native) == 16U &&
              destination(*native) == 17U,
          "four-member table list was not reserved in q28-q31");
  require(count_if(hot, &is_vector_move) == 0U,
          "four-member table list emitted a staging move");
  require(count_if(hot, &is_cpu_state_vector_load) == 5U &&
              count_if(hot, &is_cpu_state_vector_store) == 1U,
          "four-member table list imported or exported unexpected state");
}

void interleaved_members_are_prebound_before_first_use() {
  if (!::uml::platform::apple::probed_host_capabilities().advanced_simd)
    return;
  constexpr auto target = table_lookup(false, 4U, 0U, 1U, 5U);
  constexpr std::array guest{
      bitwise_xor(8U, 1U, 9U),
      bitwise_xor(10U, 2U, 11U),
      bitwise_xor(12U, 3U, 13U),
      bitwise_xor(14U, 4U, 15U),
      target,
  };
  const auto compiled =
      jit_test::compile_block(code_base + 0x1000U, guest);
  const auto unplanned =
      jit_test::compile_block_without_table_list_planning(
          code_base + 0x1000U, guest);
  require(compiled.guest_instructions == guest.size(),
          "JIT split an interleaved table-list block");
  const auto hot = hot_code(compiled);
  const auto native = find_native(hot, target);
  require(native.has_value(),
          "JIT omitted the interleaved table semantic word");
  require(first(*native) == 28U && indices(*native) == 24U &&
              destination(*native) == 25U,
          "interleaved members did not retain q28-q31 bindings");
  require(count_if(hot, &is_vector_move) == 0U,
          "interleaved table members were staged at execution time");
  require(count_if(hot, &is_cpu_state_vector_load) == 9U &&
              count_if(hot, &is_cpu_state_vector_store) == 5U,
          "interleaved table block has unexpected state traffic");
  require(compiled.hot_words == 37U && compiled.words.size() == 40U,
          "planned interleaved table block changed exact code size");
  require(unplanned.hot_words == compiled.hot_words + 4U &&
              unplanned.words.size() == compiled.words.size() + 4U &&
              unplanned.guest_instructions ==
                  compiled.guest_instructions &&
              unplanned.memory_write_mask ==
                  compiled.memory_write_mask &&
              unplanned.direct_memory_write_mask ==
                  compiled.direct_memory_write_mask &&
              unplanned.accesses_fp_state ==
                  compiled.accesses_fp_state,
          "four-member planning did not remove exactly four staging moves");
}

void wrapped_aliases_use_one_consecutive_native_list() {
  if (!::uml::platform::apple::probed_host_capabilities().advanced_simd)
    return;
  constexpr auto target = table_lookup(true, 4U, 31U, 31U, 0U);
  constexpr std::array guest{target};
  const auto compiled =
      jit_test::compile_block(code_base + 0x2000U, guest);
  const auto hot = hot_code(compiled);
  const auto native = find_native(hot, target);
  require(native.has_value(),
          "JIT omitted the wrapped alias table semantic word");
  require(first(*native) == 28U && indices(*native) == 29U &&
              destination(*native) == 28U,
          "wrapped alias table list lost its consecutive binding");
  require(count_if(hot, &is_vector_move) == 0U,
          "wrapped alias table list emitted a staging move");
  require(count_if(hot, &is_cpu_state_vector_load) == 4U &&
              count_if(hot, &is_cpu_state_vector_store) == 1U,
          "wrapped alias table list has unexpected state traffic");
}

void independent_lists_receive_disjoint_runs() {
  if (!::uml::platform::apple::probed_host_capabilities().advanced_simd)
    return;
  constexpr auto first_target =
      table_lookup(false, 4U, 0U, 1U, 5U);
  constexpr auto second_target =
      table_lookup(false, 4U, 16U, 17U, 21U);
  constexpr std::array guest{first_target, second_target};
  const auto compiled =
      jit_test::compile_block(code_base + 0x3000U, guest);
  const auto hot = hot_code(compiled);
  std::array<std::uint32_t, 2> natives{};
  std::size_t native_count{};
  for (const auto word : hot) {
    if ((word & ~register_mask) !=
        (first_target & ~register_mask))
      continue;
    require(native_count < natives.size(),
            "JIT emitted too many independent table semantic words");
    natives[native_count++] = word;
  }
  require(native_count == natives.size(),
          "JIT omitted an independently planned table list");
  require(first(natives[0]) == 28U &&
              first(natives[1]) == 24U,
          "independent table lists did not receive disjoint high runs");
  require(count_if(hot, &is_vector_move) == 0U,
          "independent table lists emitted a staging move");
}

void planning_stops_at_a_translated_terminal() {
  if (!::uml::platform::apple::probed_host_capabilities().advanced_simd)
    return;
  constexpr auto ordinary = bitwise_xor(0U, 1U, 2U);
  constexpr std::uint32_t system_call = 0xd400'0001U;
  constexpr auto future = table_lookup(false, 4U, 0U, 1U, 5U);
  constexpr std::array prefix{ordinary, system_call};
  constexpr std::array fetched{ordinary, system_call, future};
  const auto expected =
      jit_test::compile_block(code_base + 0x5000U, prefix);
  const auto actual =
      jit_test::compile_block(code_base + 0x5000U, fetched);
  require(actual.guest_instructions == prefix.size(),
          "future table list escaped the translated terminal");
  require(actual.words.size() == expected.words.size() &&
              actual.hot_words == expected.hot_words &&
              actual.internal_entry_word == expected.internal_entry_word &&
              actual.memory_write_mask == expected.memory_write_mask &&
              actual.direct_memory_write_mask ==
                  expected.direct_memory_write_mask &&
              actual.accesses_fp_state == expected.accesses_fp_state &&
              actual.native_fp == expected.native_fp,
          "future table list changed a terminal prefix");
}

} // namespace

int main() {
  single_four_member_list_is_planned();
  interleaved_members_are_prebound_before_first_use();
  wrapped_aliases_use_one_consecutive_native_list();
  independent_lists_receive_disjoint_runs();
  planning_stops_at_a_translated_terminal();
  return 0;
}
