// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace jit_test = uml::cpu::arm64::jit::testing;

namespace {

constexpr std::uint64_t code_base = 0x4fc0'0000ULL;

using uml::testing::require;

[[nodiscard]] std::span<const std::uint32_t>
hot_code(const jit_test::CompiledCode &compiled, std::size_t expected_guest) {
  require(compiled.guest_instructions == expected_guest,
          "JIT did not compile the complete flag block");
  require(compiled.hot_words <= compiled.words.size(),
          "JIT hot-word boundary exceeds generated flag code");
  return std::span<const std::uint32_t>{compiled.words}.first(
      compiled.hot_words);
}

[[nodiscard]] constexpr bool is_nzcv_mrs(std::uint32_t word) noexcept {
  return (word & 0xffff'ffe0U) == 0xd53b'4200U;
}

[[nodiscard]] constexpr bool is_nzcv_msr(std::uint32_t word) noexcept {
  return (word & 0xffff'ffe0U) == 0xd51b'4200U;
}

[[nodiscard]] constexpr bool is_nzcv_state_load(std::uint32_t word) noexcept {
  constexpr auto offset = offsetof(uml::cpu::arm64::CpuState, nzcv);
  constexpr auto encoding =
      0xb940'0000U | ((offset / sizeof(std::uint32_t)) << 10U) | (15U << 5U);
  return (word & ~31U) == encoding;
}

[[nodiscard]] constexpr bool is_nzcv_state_store(std::uint32_t word) noexcept {
  constexpr auto offset = offsetof(uml::cpu::arm64::CpuState, nzcv);
  constexpr auto encoding =
      0xb900'0000U | ((offset / sizeof(std::uint32_t)) << 10U) | (15U << 5U);
  return (word & ~31U) == encoding;
}

[[nodiscard]] constexpr bool is_nzcv_transfer(std::uint32_t word) noexcept {
  return is_nzcv_mrs(word) || is_nzcv_msr(word) ||
         is_nzcv_state_load(word) || is_nzcv_state_store(word);
}

[[nodiscard]] constexpr bool
is_conditional_branch(std::uint32_t word) noexcept {
  return (word & 0xff00'0010U) == 0x5400'0000U;
}

[[nodiscard]] constexpr bool is_tbnz(std::uint32_t word) noexcept {
  return (word & 0x7f00'0000U) == 0x3700'0000U;
}

[[nodiscard]] constexpr bool
is_tlb_tag_data_pair_load(std::uint32_t word) noexcept {
  constexpr auto displacement_mask = std::uint32_t{0x7fU} << 15U;
  constexpr auto shape = 0xa940'0000U | (9U << 10U) | 2U;
  return (word & ~displacement_mask) == shape;
}

[[nodiscard]] constexpr bool
is_old_scalar_tlb_field_load(std::uint32_t word) noexcept {
  if ((word & 0xffc0'03e0U) != 0xf940'0000U)
    return false;
  const auto destination = word & 31U;
  return destination == 2U || destination == 9U;
}

[[nodiscard]] std::size_t
transfer_count(std::span<const std::uint32_t> words) {
  return static_cast<std::size_t>(
      std::ranges::count_if(words, is_nzcv_transfer));
}

template <typename Predicate>
[[nodiscard]] std::size_t find_word(std::span<const std::uint32_t> words,
                                    Predicate predicate,
                                    std::size_t start = 0) {
  for (std::size_t index = start; index < words.size(); ++index)
    if (predicate(words[index]))
      return index;
  return words.size();
}

void compare_to_conditional_select_has_no_round_trip() {
  constexpr std::array guest{
      0xeb02'003fU, // cmp x1, x2
      0x9100'0529U, // add x9, x9, #1 (flag-neutral)
      0x9a88'00e6U, // csel x6, x7, x8, eq
  };
  const auto compiled = jit_test::compile_block(code_base, guest);
  const auto hot = hot_code(compiled, guest.size());
  constexpr auto compare_class = 0xffe0'fc1fU;
  constexpr auto select_class = 0xffe0'0c00U;
  const auto compare = find_word(hot, [](std::uint32_t word) {
    return (word & compare_class) == 0xeb00'001fU;
  });
  const auto select = find_word(
      hot,
      [](std::uint32_t word) {
        return (word & select_class) == 0x9a80'0000U;
      },
      compare + 1U);
  require(compare < select && select < hot.size(),
          "native CMP/CSEL pair was not emitted");
  require(transfer_count(hot.subspan(compare + 1U,
                                     select - compare - 1U)) == 0,
          "CMP/CSEL still round-trips NZCV through CpuState");
  require(transfer_count(hot) == 2,
          "CMP/CSEL must publish NZCV exactly once at block exit");
  /* Legacy emitted MRS+STR after CMP and LDR+MSR before CSEL (four words).
   * Lazy residency defers one MRS+STR to the observable exit: two net words
   * saved for this complete block, four on the producer-consumer edge. */
  constexpr std::size_t legacy_transfers = 4;
  require(legacy_transfers - transfer_count(hot) == 2,
          "CMP/CSEL host-word saving changed");
}

void add_compare_select_chain_stays_native() {
  constexpr std::array guest{
      0xab02'0025U, // adds x5, x1, x2
      0xfa44'106aU, // ccmp x3, x4, #0xa, ne
      0x9a88'00e6U, // csel x6, x7, x8, eq
  };
  const auto compiled = jit_test::compile_block(code_base + 0x1000U, guest);
  const auto hot = hot_code(compiled, guest.size());
  const auto add = find_word(hot, [](std::uint32_t word) {
    return (word & ~((31U << 16U) | (31U << 5U) | 31U)) ==
           0xab00'0000U;
  });
  const auto compare = find_word(
      hot,
      [](std::uint32_t word) {
        return (word & ~((31U << 16U) | (31U << 5U))) ==
               (0xfa44'106aU & ~((31U << 16U) | (31U << 5U)));
      },
      add + 1U);
  const auto select = find_word(
      hot,
      [](std::uint32_t word) {
        return (word & 0xffe0'0c00U) == 0x9a80'0000U;
      },
      compare + 1U);
  require(add < compare && compare < select && select < hot.size(),
          "native ADDS/CCMP/CSEL chain was not emitted");
  require(transfer_count(
              hot.subspan(add + 1U, select - add - 1U)) == 0,
          "ADDS/CCMP/CSEL chain contains an NZCV round trip");
  require(transfer_count(hot) == 2,
          "flag chain must publish NZCV exactly once at exit");
  /* Legacy transfers: store after ADDS, load+store around CCMP, and load
   * before CSEL = eight words. The retained chain emits only the two-word
   * final publication. */
  constexpr std::size_t legacy_transfers = 8;
  require(legacy_transfers - transfer_count(hot) == 6,
          "ADDS/CCMP/CSEL host-word saving changed");
}

void compare_to_conditional_branch_publishes_once() {
  constexpr std::array guest{
      0xeb02'003fU, // cmp x1, x2
      0x5400'0040U, // b.eq +8
  };
  const auto compiled = jit_test::compile_block(code_base + 0x2000U, guest);
  const auto hot = hot_code(compiled, guest.size());
  const auto compare = find_word(hot, [](std::uint32_t word) {
    return (word & 0xffe0'fc1fU) == 0xeb00'001fU;
  });
  const auto branch = find_word(
      hot,
      [](std::uint32_t word) {
        return (word & 0xff00'0010U) == 0x5400'0000U;
      },
      compare + 1U);
  require(compare < branch && branch < hot.size(),
          "native CMP/B.cond sequence was not emitted");
  const auto between =
      hot.subspan(compare + 1U, branch - compare - 1U);
  require(transfer_count(between) == 2,
          "conditional split must publish dirty NZCV exactly once");
  require(std::ranges::count_if(between, is_nzcv_mrs) == 1 &&
              std::ranges::count_if(between, is_nzcv_state_store) == 1,
          "conditional split reloaded NZCV instead of publishing it");
  require(std::ranges::none_of(hot, is_nzcv_state_load) &&
              std::ranges::none_of(hot, is_nzcv_msr),
          "CMP/B.cond contains a legacy reload round trip");
  constexpr std::size_t legacy_transfers = 4;
  require(legacy_transfers - transfer_count(hot) == 2,
          "CMP/B.cond host-word saving changed");
}

void compare_memory_select_keeps_flags_on_both_tlb_paths() {
  constexpr std::array guest{
      0xeb02'003fU, // cmp x1, x2
      0xf940'0003U, // ldr x3, [x0]
      0x9a86'00a4U, // csel x4, x5, x6, eq
  };
  const auto compiled = jit_test::compile_block(code_base + 0x3000U, guest);
  const auto hot = hot_code(compiled, guest.size());
  const auto compare = find_word(hot, [](std::uint32_t word) {
    return (word & 0xffe0'fc1fU) == 0xeb00'001fU;
  });
  const auto select = find_word(
      hot,
      [](std::uint32_t word) {
        return (word & 0xffe0'0c00U) == 0x9a80'0000U;
      },
      compare + 1U);
  require(compare < select && select < hot.size(),
          "native CMP/LDR/CSEL sequence was not emitted");
  const auto memory_path =
      hot.subspan(compare + 1U, select - compare - 1U);
  require(transfer_count(memory_path) == 0,
          "memory translation published or reloaded live guest NZCV");
  require(std::ranges::none_of(memory_path, is_conditional_branch),
          "memory translation still contains a flag-consuming guard");
  require(std::ranges::none_of(memory_path,
                               [](std::uint32_t word) {
                                 return word == 0xd63f'0200U;
                               }),
          "returning TLB miss call leaked into the hot memory path");
  require(std::ranges::count(memory_path, 0xca01'0042U) == 1,
          "hot TLB tag matching is not one EOR/CBNZ sequence");
  require(std::ranges::count_if(memory_path, is_tlb_tag_data_pair_load) == 1,
          "hot TLB way does not load its tag and backing delta as one pair");
  require(std::ranges::none_of(memory_path, is_old_scalar_tlb_field_load),
          "TLB hit path retained a separate tag or backing-data load");
  require(std::ranges::count_if(memory_path, is_tbnz) == 1,
          "multi-byte page guard is not one flag-neutral TBNZ");
  require(transfer_count(hot) == 2,
          "CMP/LDR/CSEL must publish NZCV only at the hot block exit");

  const auto cold =
      std::span<const std::uint32_t>{compiled.words}.subspan(
          compiled.hot_words);
  require(std::ranges::count(cold, 0xca01'0042U) == 0 &&
              std::ranges::count_if(cold, is_tlb_tag_data_pair_load) == 0,
          "the deferred path holds a TLB probe of its own");
  std::size_t returning_miss_calls = 0;
  for (std::size_t index = 0; index + 1U < cold.size(); ++index)
    if (cold[index] == 0xd63f'0200U &&
        cold[index + 1U] == 0xaa00'03e8U) // blr x16; mov x8, x0
      ++returning_miss_calls;
  require(returning_miss_calls == 1,
          "cold translation omitted or duplicated the returning miss call");
  require(transfer_count(cold) == 4,
          "load fault and slow edges did not each publish their flag snapshot");
}

void add_sp_memory_select_keeps_flags_resident() {
  constexpr std::array guest{
      0xab02'0027U, // adds x7, x1, x2
      0xf900'03e3U, // str x3, [sp]
      0x9a86'00a4U, // csel x4, x5, x6, eq
  };
  const auto compiled = jit_test::compile_block(code_base + 0x4000U, guest);
  const auto hot = hot_code(compiled, guest.size());
  const auto add = find_word(hot, [](std::uint32_t word) {
    return (word & ~((31U << 16U) | (31U << 5U) | 31U)) ==
           0xab00'0000U;
  });
  const auto select = find_word(
      hot,
      [](std::uint32_t word) {
        return (word & 0xffe0'0c00U) == 0x9a80'0000U;
      },
      add + 1U);
  require(add < select && select < hot.size(),
          "native ADDS/STR-SP/CSEL sequence was not emitted");
  const auto memory_path = hot.subspan(add + 1U, select - add - 1U);
  require(transfer_count(memory_path) == 0,
          "SP memory guards published or reloaded live guest NZCV");
  require(std::ranges::none_of(memory_path, is_conditional_branch),
          "SP memory guards still consume host flags");
  require(std::ranges::count(memory_path, 0xd340'0d00U) == 1,
          "SP alignment is not the expected UBFM/CBNZ predicate");
}

} // namespace

int main() {
  compare_to_conditional_select_has_no_round_trip();
  add_compare_select_chain_stays_native();
  compare_to_conditional_branch_publishes_once();
  compare_memory_select_keeps_flags_on_both_tlb_paths();
  add_sp_memory_select_keeps_flags_resident();
  return 0;
}
