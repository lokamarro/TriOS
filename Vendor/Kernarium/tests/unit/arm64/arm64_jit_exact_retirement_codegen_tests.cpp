// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

using uml::emulator::tests::require;

namespace {

namespace jit_test = uml::cpu::arm64::jit::testing;

constexpr std::uint32_t add_x5_one = 0x9100'04a5U;
constexpr std::uint32_t load_pair_q0_q1_x0 = 0xad40'0400U;
constexpr std::uint32_t system_call = 0xd400'0001U;
constexpr std::uint32_t add_packed_result = 0x8b01'0000U;
constexpr std::uint32_t old_merge_packed_result = 0xaa01'0000U;

[[nodiscard]] constexpr std::uint32_t
move_wide(bool keep, unsigned destination, unsigned immediate,
          unsigned half) noexcept {
  return (keep ? 0xf280'0000U : 0xd280'0000U) |
         ((half & 3U) << 21U) | ((immediate & 0xffffU) << 5U) |
         (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
move_not_wide(unsigned destination, unsigned immediate,
              unsigned half) noexcept {
  return 0x9280'0000U | ((half & 3U) << 21U) |
         ((immediate & 0xffffU) << 5U) | (destination & 31U);
}

[[nodiscard]] std::span<const std::uint32_t>
hot(const jit_test::CompiledCode &compiled) {
  require(compiled.hot_words <= compiled.words.size());
  return {compiled.words.data(), compiled.hot_words};
}

[[nodiscard]] std::size_t count(std::span<const std::uint32_t> words,
                                std::uint32_t value) {
  return static_cast<std::size_t>(std::count(words.begin(), words.end(),
                                             value));
}

void constant_materialization_starts_at_the_first_nonzero_half() {
  constexpr unsigned destination = 7;
  const auto zero =
      jit_test::materialize_constant_for_testing(destination, 0);
  constexpr std::array zero_expected{
      move_wide(false, destination, 0, 0),
  };
  require(zero.size() == zero_expected.size() &&
          std::equal(zero_expected.begin(), zero_expected.end(), zero.begin()));

  const auto upper = jit_test::materialize_constant_for_testing(
      destination, std::uint64_t{0x1234} << 32U);
  constexpr std::array upper_expected{
      move_wide(false, destination, 0x1234, 2),
  };
  require(upper.size() == upper_expected.size() &&
          std::equal(upper_expected.begin(), upper_expected.end(),
                     upper.begin()));

  const auto sparse = jit_test::materialize_constant_for_testing(
      destination, 0xabcd'0000'5678'0000ULL);
  constexpr std::array sparse_expected{
      move_wide(false, destination, 0x5678, 1),
      move_wide(true, destination, 0xabcd, 3),
  };
  require(sparse.size() == sparse_expected.size() &&
          std::equal(sparse_expected.begin(), sparse_expected.end(),
                     sparse.begin()));
}

void constant_materialization_uses_inversion_only_when_shorter() {
  constexpr unsigned destination = 7;
  const auto all_ones = jit_test::materialize_constant_for_testing(
      destination, ~std::uint64_t{0});
  constexpr std::array all_ones_expected{
      move_not_wide(destination, 0, 0),
  };
  require(all_ones.size() == all_ones_expected.size() &&
          std::equal(all_ones_expected.begin(), all_ones_expected.end(),
                     all_ones.begin()));

  const auto complement_63 = jit_test::materialize_constant_for_testing(
      destination, ~std::uint64_t{63});
  constexpr std::array complement_63_expected{
      move_not_wide(destination, 63, 0),
  };
  require(complement_63.size() == complement_63_expected.size() &&
          std::equal(complement_63_expected.begin(),
                     complement_63_expected.end(), complement_63.begin()));

  const auto sparse_complement = jit_test::materialize_constant_for_testing(
      destination, 0xffff'1234'ffff'5678ULL);
  constexpr std::array sparse_complement_expected{
      move_not_wide(destination, 0xa987, 0),
      move_wide(true, destination, 0x1234, 2),
  };
  require(
      sparse_complement.size() == sparse_complement_expected.size() &&
      std::equal(sparse_complement_expected.begin(),
                 sparse_complement_expected.end(), sparse_complement.begin()));

  const auto tie = jit_test::materialize_constant_for_testing(
      destination, 0xffff'0000'ffff'0000ULL);
  constexpr std::array tie_expected{
      move_wide(false, destination, 0xffff, 1),
      move_wide(true, destination, 0xffff, 3),
  };
  require(tie.size() == tie_expected.size() &&
          std::equal(tie_expected.begin(), tie_expected.end(), tie.begin()));
}

void continued_result_uses_one_upper_half_move() {
  constexpr std::uint64_t pc = 0x5aff'0000;
  constexpr std::array guest{add_x5_one};
  const auto compiled = jit_test::compile_block(pc, guest);
  require(!compiled.guest_illegal);
  require(compiled.guest_instructions == guest.size());
  require(count(hot(compiled), move_wide(false, 0, 1, 2)) == 1);
  require(count(hot(compiled), move_wide(false, 0, 0, 0)) == 0);
}

void dynamic_result_uses_exact_arithmetic_composition() {
  constexpr std::uint64_t pc = 0x5b00'0000;
  constexpr std::array odd_prefix{
      add_x5_one,
      load_pair_q0_q1_x0,
      system_call,
  };
  const auto compiled = jit_test::compile_block(pc, odd_prefix);
  require(!compiled.guest_illegal);
  require(compiled.guest_instructions == odd_prefix.size());
  require(count(compiled.words, add_packed_result) == 1);
  require(count(compiled.words, old_merge_packed_result) == 0);

  constexpr std::array no_prefix{
      load_pair_q0_q1_x0,
      system_call,
  };
  const auto first = jit_test::compile_block(pc, no_prefix);
  require(!first.guest_illegal);
  require(first.guest_instructions == no_prefix.size());
  require(count(first.words, add_packed_result) == 0);
  require(count(first.words, old_merge_packed_result) == 0);
}

void successful_links_do_not_publish_terminal_metadata() {
  constexpr std::uint64_t direct_pc = 0x5b01'0000;
  constexpr std::array direct{0x1400'0001U}; // b pc + 4
  const auto direct_persistent =
      jit_test::compile_linked_block(direct_pc, direct);

  /*
   * These were STRs of target instruction_count and guest_pc through the
   * x19 chain-state pointer.
   */
  require(count(hot(direct_persistent), 0xf900'0a69U) == 0);
  require(count(hot(direct_persistent), 0xf900'0670U) == 0);

  constexpr std::uint64_t pic_pc = 0x5b02'0000;
  constexpr std::array indirect{0xd61f'00a0U}; // br x5
  const auto pic_persistent =
      jit_test::compile_indirect_linked_block(pic_pc, indirect);
  require(count(hot(pic_persistent), 0xf900'0a69U) == 0);
  require(count(hot(pic_persistent), 0xf900'0670U) == 0);
}

void exclusive_counter_loop_retains_one_translated_address() {
  constexpr std::array guest{
      0xc85f'7e80U, // ldxr x0, [x20]
      0x9100'0400U, // add x0, x0, #1
      0xc801'7e80U, // stxr w1, x0, [x20]
      0x35ff'ffa1U, // cbnz w1, pc - 12
  };
  const auto compiled = jit_test::compile_block(0x210174U, guest);
  const auto code = hot(compiled);
  const auto load = std::ranges::find_if(code, [](std::uint32_t word) {
    return (word & ~31U) == 0xc85f'7ea0U;
  });
  const auto store = std::ranges::find_if(code, [](std::uint32_t word) {
    return (word & ~((31U << 16) | 31U)) == 0xc800'7ea0U;
  });
  require(compiled.guest_instructions == guest.size());
  require(compiled.memory_write_mask == (std::uint64_t{1} << 2U));
  require(compiled.direct_memory_write_mask ==
          (std::uint64_t{1} << 2U));
  require(load != code.end() && store != code.end());
  require(std::distance(load, store) == 2);
}

void profiled_exclusive_integer_shapes_remain_native() {
  constexpr std::uint32_t load = 0xc85f'7c23U;  // ldxr x3, [x1]
  constexpr std::uint32_t store = 0xc805'7c23U; // stxr w5, x3, [x1]
  const auto verify = [&](std::span<const std::uint32_t> guest,
                          std::ptrdiff_t native_distance,
                          std::uint64_t store_mask) {
    const auto compiled = jit_test::compile_block(0x5b03'0000U, guest);
    const auto code = hot(compiled);
    const auto native_load =
        std::ranges::find_if(code, [](std::uint32_t word) {
          return (word & ~31U) == 0xc85f'7ea0U;
        });
    const auto native_store =
        std::ranges::find_if(code, [](std::uint32_t word) {
          return (word & ~((31U << 16) | 31U)) == 0xc800'7ea0U;
        });
    require(compiled.guest_instructions == guest.size());
    require(compiled.memory_write_mask == store_mask);
    require(compiled.direct_memory_write_mask == store_mask);
    require(native_load != code.end() && native_store != code.end());
    require(std::distance(native_load, native_store) == native_distance);
  };

  constexpr std::array adjacent{load, store};
  constexpr std::array add_register{
      load,
      0x8b04'0063U, // add x3, x3, x4
      store,
  };
  constexpr std::array logical_register{
      load,
      0xaa04'0063U, // orr x3, x3, x4
      store,
  };
  verify(adjacent, 1, std::uint64_t{1} << 1U);
  verify(add_register, 2, std::uint64_t{1} << 2U);
  verify(logical_register, 2, std::uint64_t{1} << 2U);

  constexpr std::array flag_setting{
      load,
      0xab04'0063U, // adds x3, x3, x4
      store,
  };
  verify(flag_setting, 2, std::uint64_t{1} << 2U);
}

void compare_retry_region_uses_native_exclusives() {
  constexpr std::array guest{
      0xc85f'7c23U, // ldxr x3, [x1]
      0xeb04'007fU, // cmp x3, x4
      0x5400'0061U, // b.ne pc + 12
      0xc805'7c22U, // stxr w5, x2, [x1]
      0x35ff'ff85U, // cbnz w5, pc - 16
  };
  const auto compiled = jit_test::compile_block(0x5b05'0000U, guest);
  const auto code = hot(compiled);
  require(compiled.guest_instructions == guest.size());
  require(compiled.direct_memory_write_mask == (std::uint64_t{1} << 3U));
  require(std::ranges::any_of(code, [](std::uint32_t word) {
    return (word & ~((31U << 16) | 31U)) == 0xc800'7ea0U;
  }));
}

void tracked_compare_retry_region_uses_the_semantic_path() {
  constexpr std::array guest{
      0xc85f'7c23U, // ldxr x3, [x1]
      0xeb04'007fU, // cmp x3, x4
      0x5400'0061U, // b.ne pc + 12
      0xc805'7c22U, // stxr w5, x2, [x1]
      0x35ff'ff85U, // cbnz w5, pc - 16
  };
  const auto compiled =
      jit_test::compile_tracked_block(0x5b06'0000U, guest);
  require(!compiled.words.empty());
  require(compiled.guest_instructions != 0);
  require(compiled.guest_instructions <= guest.size());
  require(compiled.direct_memory_write_mask == 0);
  require(std::ranges::none_of(hot(compiled), [](std::uint32_t word) {
    return (word & ~((31U << 16) | 31U)) == 0xc800'7ea0U;
  }));
}

void compare_retry_with_shared_result_and_status_is_not_fused() {
  constexpr std::array guest{
      0x885f'fc9bU, // ldxr w27, [x4]
      0x6b02'037fU, // cmp w27, w2
      0x5400'0061U, // b.ne pc + 12
      0x881b'fc83U, // stxr w27, w3, [x4]
      0xb5ff'ff9bU, // cbnz x27, pc - 16
  };
  const auto compiled = jit_test::compile_block(0x5b07'0000U, guest);
  require(!compiled.words.empty());
  require(compiled.guest_instructions != 0);
  require(std::ranges::none_of(hot(compiled), [](std::uint32_t word) {
    return (word & 0x3fa0'fc00U) == 0x08a0'7c00U;
  }));
}

} // namespace

int main() {
  constant_materialization_starts_at_the_first_nonzero_half();
  constant_materialization_uses_inversion_only_when_shorter();
  continued_result_uses_one_upper_half_move();
  dynamic_result_uses_exact_arithmetic_composition();
  successful_links_do_not_publish_terminal_metadata();
  exclusive_counter_loop_retains_one_translated_address();
  profiled_exclusive_integer_shapes_remain_native();
  compare_retry_region_uses_native_exclusives();
  tracked_compare_retry_region_uses_the_semantic_path();
  compare_retry_with_shared_result_and_status_is_not_fused();
  return 0;
}
