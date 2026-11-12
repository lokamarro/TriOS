// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "require.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace jit_test = uml::cpu::arm64::jit::testing;

namespace {

constexpr std::uint64_t code_base = 0x4f30'0000ULL;

[[nodiscard]] std::int64_t sign_extend(std::uint32_t value,
                                       unsigned bits) noexcept {
  const auto shift = 32U - bits;
  return static_cast<std::int32_t>(value << shift) >> shift;
}

using uml::testing::fail;
using uml::testing::require;

[[nodiscard]] constexpr std::uint32_t
add_register(unsigned destination, unsigned left, unsigned right) noexcept {
  return 0x8b00'0000U | ((right & 31U) << 16U) |
         ((left & 31U) << 5U) | (destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t
compare_branch(bool nonzero, unsigned source, unsigned displacement) noexcept {
  return (nonzero ? 0xb500'0000U : 0xb400'0000U) |
         ((displacement & 0x7ffffU) << 5U) | (source & 31U);
}

[[nodiscard]] constexpr std::uint32_t
test_branch(bool nonzero, unsigned source, unsigned bit,
            unsigned displacement) noexcept {
  return (nonzero ? 0x3700'0000U : 0x3600'0000U) |
         ((bit & 32U) << 26U) | ((bit & 31U) << 19U) |
         ((displacement & 0x3fffU) << 5U) | (source & 31U);
}

[[nodiscard]] std::span<const std::uint32_t>
hot_code(const jit_test::CompiledCode &compiled, std::size_t expected_guest) {
  require(compiled.guest_instructions == expected_guest,
          "JIT did not compile the complete control block");
  require(compiled.hot_words <= compiled.words.size(),
          "JIT hot-word boundary exceeds generated control code");
  return std::span<const std::uint32_t>{compiled.words}.first(
      compiled.hot_words);
}

void compare_branch_reads_resident_producer() {
  constexpr auto producer = add_register(5, 6, 7);
  constexpr auto branch = compare_branch(true, 5, 2);
  constexpr std::array guest{producer, branch};
  const auto compiled = jit_test::compile_block(code_base, guest);
  const auto hot = hot_code(compiled, guest.size());

  std::size_t producer_index = hot.size();
  unsigned producer_host = 32U;
  for (std::size_t index = 0; index < hot.size(); ++index) {
    if ((hot[index] & ~((31U << 16U) | (31U << 5U) | 31U)) !=
        (producer & ~((31U << 16U) | (31U << 5U) | 31U)))
      continue;
    producer_index = index;
    producer_host = hot[index] & 31U;
    break;
  }
  require(producer_index != hot.size(),
          "compare-branch producer was not emitted");

  for (std::size_t index = producer_index + 1U; index < hot.size(); ++index) {
    /* The local edge is the inverse of the guest condition: CBZ skips the
     * taken CBNZ successor. */
    if ((hot[index] & 0x7f00'0000U) != 0x3400'0000U)
      continue;
    require((hot[index] & 31U) == producer_host,
            "compare branch copied its resident source to scratch");
    return;
  }
  fail("generated compare branch was not found");
}

void test_branch_reads_resident_producer() {
  constexpr auto producer = add_register(5, 6, 7);
  constexpr auto branch = test_branch(false, 5, 40, 2);
  constexpr std::array guest{producer, branch};
  const auto compiled = jit_test::compile_block(code_base + 0x1000U, guest);
  const auto hot = hot_code(compiled, guest.size());

  std::size_t producer_index = hot.size();
  unsigned producer_host = 32U;
  for (std::size_t index = 0; index < hot.size(); ++index) {
    if ((hot[index] & ~((31U << 16U) | (31U << 5U) | 31U)) !=
        (producer & ~((31U << 16U) | (31U << 5U) | 31U)))
      continue;
    producer_index = index;
    producer_host = hot[index] & 31U;
    break;
  }
  require(producer_index != hot.size(), "test-branch producer was not emitted");

  for (std::size_t index = producer_index + 1U; index < hot.size(); ++index) {
    /* The local edge is the inverse of the guest TBZ condition. */
    if ((hot[index] & 0x7f00'0000U) != 0x3700'0000U)
      continue;
    require((hot[index] & 31U) == producer_host,
            "test branch copied its resident source to scratch");
    return;
  }
  fail("generated test branch was not found");
}

void pc_relative_result_feeds_native_add_without_state_round_trip() {
  constexpr std::uint32_t adrp = 0x9000'0005U;
  constexpr std::uint32_t add = 0x9100'c0a6U; // add x6, x5, #48
  constexpr std::array guest{adrp, add};
  const auto compiled = jit_test::compile_block(code_base + 0x2000U, guest);
  const auto hot = hot_code(compiled, guest.size());

  std::size_t add_index = hot.size();
  unsigned source_host = 32U;
  for (std::size_t index = 0; index < hot.size(); ++index) {
    if ((hot[index] & ~((31U << 5U) | 31U)) !=
        (add & ~((31U << 5U) | 31U)))
      continue;
    add_index = index;
    source_host = (hot[index] >> 5U) & 31U;
    break;
  }
  require(add_index != hot.size(), "ADRP consumer was not emitted");
  require(source_host != 8U,
          "ADRP result remained in the legacy scratch register");

  bool constant_targets_source = false;
  constexpr auto guest_five_state_access =
      0xf900'0000U | (5U << 10U) | (15U << 5U);
  constexpr auto guest_five_state_load =
      0xf940'0000U | (5U << 10U) | (15U << 5U);
  for (std::size_t index = 0; index < add_index; ++index) {
    const auto word = hot[index];
    if ((word & 0xff00'001fU) ==
        (0x5800'0000U | source_host)) {
      const auto displacement =
          sign_extend((word >> 5U) & 0x7ffffU, 19);
      const auto signed_target =
          static_cast<std::int64_t>(index) + displacement;
      require(signed_target >=
                  static_cast<std::int64_t>(compiled.hot_words),
              "ADRP literal overlaps executable hot code");
      const auto target = static_cast<std::size_t>(signed_target);
      require((target & 1U) == 0U &&
                  target + 1U < compiled.words.size(),
              "ADRP literal is unaligned or outside generated storage");
      const auto value =
          static_cast<std::uint64_t>(compiled.words[target]) |
          (static_cast<std::uint64_t>(compiled.words[target + 1U]) << 32U);
      require(value == code_base + 0x2000U,
              "ADRP literal does not contain its translated page address");
      constant_targets_source = true;
    }
    require((word & ~31U) != guest_five_state_access,
            "ADRP result was stored to CpuState before its consumer");
    require((word & ~31U) != guest_five_state_load,
            "ADRP result was reloaded from CpuState before its consumer");
  }
  require(constant_targets_source,
          "ADRP constant was not loaded once into its resident binding");
}

} // namespace

int main() {
  compare_branch_reads_resident_producer();
  test_branch_reads_resident_producer();
  pc_relative_result_feeds_native_add_without_state_round_trip();
  return 0;
}
