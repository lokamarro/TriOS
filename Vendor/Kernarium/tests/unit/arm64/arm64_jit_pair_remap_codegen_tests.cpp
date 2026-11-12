// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "require.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace jit_test = uml::cpu::arm64::jit::testing;

namespace {

constexpr std::uint32_t three_register_mask =
    (31U << 16U) | (31U << 5U) | 31U;
constexpr std::uint32_t pair_register_mask =
    (31U << 10U) | (31U << 5U) | 31U;
constexpr std::uint64_t code_base = 0x4f20'0000ULL;

using uml::testing::fail;
using uml::testing::require;

[[nodiscard]] constexpr std::uint32_t
three_register(std::uint32_t operation, unsigned destination, unsigned left,
               unsigned right) noexcept {
  return operation | ((right & 31U) << 16U) | ((left & 31U) << 5U) |
         (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
bitwise(unsigned operation, unsigned destination, unsigned left,
        unsigned right) noexcept {
  return 0x4e20'1c00U | ((operation & 3U) << 22U) |
         ((operation & 4U) << 27U) | ((right & 31U) << 16U) |
         ((left & 31U) << 5U) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
pair(unsigned operation, bool vector, bool load, unsigned mode, unsigned imm7,
     unsigned base, unsigned first, unsigned second) noexcept {
  return 0x2800'0000U | ((operation & 3U) << 30U) |
         (static_cast<unsigned>(vector) << 26U) | ((mode & 3U) << 23U) |
         (static_cast<unsigned>(load) << 22U) | ((imm7 & 0x7fU) << 15U) |
         ((second & 31U) << 10U) | ((base & 31U) << 5U) |
         (first & 31U);
}

[[nodiscard]] unsigned destination(std::uint32_t instruction) noexcept {
  return instruction & 31U;
}

[[nodiscard]] unsigned left(std::uint32_t instruction) noexcept {
  return (instruction >> 5U) & 31U;
}

[[nodiscard]] unsigned pair_second(std::uint32_t instruction) noexcept {
  return (instruction >> 10U) & 31U;
}

[[nodiscard]] std::size_t
find_matching(std::span<const std::uint32_t> words, std::uint32_t reference,
              std::uint32_t register_mask, std::size_t start = 0) {
  for (std::size_t index = start; index < words.size(); ++index)
    if ((words[index] & ~register_mask) ==
        (reference & ~register_mask))
      return index;
  fail("expected native instruction was not emitted");
}

[[nodiscard]] std::span<const std::uint32_t>
hot_code(const jit_test::CompiledCode &compiled, std::size_t expected_guest) {
  require(compiled.guest_instructions == expected_guest,
          "JIT did not compile the complete pair block");
  require(compiled.hot_words <= compiled.words.size(),
          "JIT hot-word boundary exceeds generated pair code");
  return std::span<const std::uint32_t>{compiled.words}.first(
      compiled.hot_words);
}

void integer_store_uses_resident_sources_and_base() {
  constexpr auto bind_base =
      three_register(0x8b00'0000U, 28, 29, 30);
  constexpr auto bind_first =
      three_register(0x8b00'0000U, 3, 4, 5);
  constexpr auto bind_second =
      three_register(0x8b00'0000U, 17, 18, 19);
  constexpr auto store =
      pair(2, false, false, 3, 2, 28, 3, 17);
  constexpr std::array guest{bind_base, bind_first, bind_second, store};

  const auto compiled = jit_test::compile_block(code_base, guest);
  const auto hot = hot_code(compiled, guest.size());
  const auto base_index =
      find_matching(hot, bind_base, three_register_mask);
  const auto first_index =
      find_matching(hot, bind_first, three_register_mask, base_index + 1U);
  const auto second_index =
      find_matching(hot, bind_second, three_register_mask, first_index + 1U);
  const auto pair_index = find_matching(
      hot, pair(2, false, false, 3, 0, 0, 0, 0), pair_register_mask,
      second_index + 1U);

  const auto native_pair = hot[pair_index];
  require(destination(native_pair) == destination(hot[first_index]),
          "integer pair store copied its first resident source to scratch");
  require(pair_second(native_pair) == destination(hot[second_index]),
          "integer pair store copied its second resident source to scratch");
  require(left(native_pair) == 8U,
          "integer pair store bypassed the checked host address");

  const auto base_host = destination(hot[base_index]);
  const auto writeback =
      0x9100'0000U | (16U << 10U) | (base_host << 5U) | base_host;
  bool found_writeback = false;
  for (std::size_t index = pair_index + 1U; index < hot.size(); ++index)
    found_writeback = found_writeback || hot[index] == writeback;
  require(found_writeback,
          "pair writeback did not update the resident base directly");
}

void duplicate_integer_store_reuses_one_binding() {
  constexpr auto bind_source =
      three_register(0xca00'0000U, 3, 4, 5);
  constexpr auto store =
      pair(2, false, false, 2, 0, 28, 3, 3);
  constexpr std::array guest{bind_source, store};

  const auto compiled = jit_test::compile_block(code_base + 0x1000U, guest);
  const auto hot = hot_code(compiled, guest.size());
  const auto source_index =
      find_matching(hot, bind_source, three_register_mask);
  const auto pair_index = find_matching(
      hot, pair(2, false, false, 2, 0, 0, 0, 0), pair_register_mask,
      source_index + 1U);
  const auto source_host = destination(hot[source_index]);
  require(destination(hot[pair_index]) == source_host &&
              pair_second(hot[pair_index]) == source_host,
          "duplicate integer pair source did not reuse its resident binding");
}

void integer_load_results_remain_resident() {
  constexpr auto load =
      pair(2, false, true, 2, 0, 28, 3, 17);
  constexpr auto use_first =
      three_register(0x9ac0'2c00U, 4, 3, 5);
  constexpr auto use_second =
      three_register(0x9ac0'2c00U, 18, 17, 19);
  constexpr std::array guest{load, use_first, use_second};

  const auto compiled = jit_test::compile_block(code_base + 0x2000U, guest);
  const auto hot = hot_code(compiled, guest.size());
  auto pair_index = find_matching(
      hot, pair(2, false, true, 2, 0, 0, 0, 0), pair_register_mask);
  while (left(hot[pair_index]) != 8U)
    pair_index = find_matching(
        hot, pair(2, false, true, 2, 0, 0, 0, 0), pair_register_mask,
        pair_index + 1U);
  const auto first_index =
      find_matching(hot, use_first, three_register_mask, pair_index + 1U);
  const auto second_index =
      find_matching(hot, use_second, three_register_mask, first_index + 1U);

  require(destination(hot[pair_index]) == left(hot[first_index]),
          "first integer pair result was stored and reloaded");
  require(pair_second(hot[pair_index]) == left(hot[second_index]),
          "second integer pair result was stored and reloaded");
}

void vector_pair_operands_use_resident_bindings() {
  constexpr auto bind_first = bitwise(4, 3, 4, 5);
  constexpr auto bind_second = bitwise(2, 17, 18, 19);
  constexpr auto store =
      pair(2, true, false, 2, 0, 28, 3, 17);
  constexpr std::array store_guest{bind_first, bind_second, store};

  const auto store_compiled =
      jit_test::compile_block(code_base + 0x3000U, store_guest);
  const auto store_hot = hot_code(store_compiled, store_guest.size());
  const auto first_index =
      find_matching(store_hot, bind_first, three_register_mask);
  const auto second_index = find_matching(
      store_hot, bind_second, three_register_mask, first_index + 1U);
  const auto store_index = find_matching(
      store_hot, pair(2, true, false, 2, 0, 0, 0, 0),
      pair_register_mask, second_index + 1U);
  require(destination(store_hot[store_index]) ==
              destination(store_hot[first_index]),
          "vector pair store copied its first resident source to scratch");
  require(pair_second(store_hot[store_index]) ==
              destination(store_hot[second_index]),
          "vector pair store copied its second resident source to scratch");

  constexpr auto load =
      pair(2, true, true, 2, 0, 28, 3, 17);
  constexpr auto use_first = bitwise(4, 4, 3, 5);
  constexpr auto use_second = bitwise(2, 18, 17, 19);
  constexpr std::array load_guest{load, use_first, use_second};

  const auto load_compiled =
      jit_test::compile_block(code_base + 0x4000U, load_guest);
  const auto load_hot = hot_code(load_compiled, load_guest.size());
  const auto load_index = find_matching(
      load_hot, pair(2, true, true, 2, 0, 0, 0, 0),
      pair_register_mask);
  const auto first_use =
      find_matching(load_hot, use_first, three_register_mask, load_index + 1U);
  const auto second_use = find_matching(
      load_hot, use_second, three_register_mask, first_use + 1U);
  require(destination(load_hot[load_index]) == left(load_hot[first_use]),
          "first vector pair result was stored and reloaded");
  require(pair_second(load_hot[load_index]) == left(load_hot[second_use]),
          "second vector pair result was stored and reloaded");
}

} // namespace

int main() {
  integer_store_uses_resident_sources_and_base();
  duplicate_integer_store_reuses_one_binding();
  integer_load_results_remain_resident();
  vector_pair_operands_use_resident_bindings();
  return 0;
}
