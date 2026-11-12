// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* What the GPR, SP and SIMD memory-run codegen tests find in a block's hot
 * code, and the scalar unsigned-offset cells the GPR and SP runs coalesce. */

#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <span>
#include <vector>

namespace arm64_jit_memory_run_codegen_test {

using uml::testing::require;

inline constexpr std::uint32_t memory_register_mask = (31U << 5U) | 31U;
// One flag-neutral tag comparison is emitted per TLB translation.
inline constexpr std::uint32_t translation_probe = 0xca01'0042U;
inline constexpr unsigned page_shift = DARWIN_UML_GUEST_PAGE_SHIFT;
inline constexpr std::uint32_t compare_branch_displacement_mask =
    0x00ff'ffe0U;
inline constexpr std::uint32_t test_branch_displacement_mask = 0x0007'ffe0U;
// A GPR run's page proof: eor x0, x0, x8, then tbnz x0, #page_shift.
inline constexpr std::uint32_t page_span_eor = 0xca08'0000U;
inline constexpr std::uint32_t page_span_tbnz =
    0x3700'0000U | ((page_shift & 31U) << 19U);
// An SP run's alignment check: ubfx x0, x8, #0, #4, then cbnz x0.
inline constexpr std::uint32_t sp_alignment_extract =
    0xd340'0000U | (3U << 10U) | (8U << 5U);
inline constexpr std::uint32_t cbnz_x0 = 0xb500'0000U;
// An SP run's page proof: ubfx x0, x8, #4, #(page_shift - 4).
inline constexpr std::uint32_t sp_span_offset_extract =
    0xd340'0000U | (4U << 16U) | ((page_shift - 1U) << 10U) |
    (8U << 5U);
// The store that drops the software monitor (strb wzr, [x15, #exclusive_size]).
// Only a fused exclusive pair and CLREX emit it; an ordinary store emits none.
inline constexpr std::uint32_t monitor_clear =
    0x3900'0000U |
    (static_cast<std::uint32_t>(
         offsetof(uml::cpu::arm64::CpuState, exclusive_size))
     << 10U) |
    (15U << 5U) | 31U;
inline constexpr std::uint32_t nop = 0xd503'201fU;
// A run's straddle path finds the next page: orr x11, x8, #(page_size - 1).
inline constexpr std::uint32_t straddle_next_page =
    0xb240'0000U | ((page_shift - 1U) << 10U) | (8U << 5U) | 11U;

/* A run whose first accessed byte is `offset` past its base, a multiple of
 * 4 KiB, translates that byte: add x8, x8, #(offset >> 12), lsl #12 before
 * the translation and the matching sub after it. */
[[nodiscard]] constexpr std::uint32_t
run_base_forward(std::uint64_t offset) noexcept {
  return 0x9140'0000U | (static_cast<std::uint32_t>(offset >> 12U) << 10U) |
         (8U << 5U) | 8U;
}

[[nodiscard]] constexpr std::uint32_t
run_base_back(std::uint64_t offset) noexcept {
  return 0xd140'0000U | (static_cast<std::uint32_t>(offset >> 12U) << 10U) |
         (8U << 5U) | 8U;
}

struct Variant final {
  unsigned size_shift;
  unsigned opc;
  bool load;
};

inline constexpr std::array variants{
    Variant{0, 0, false}, Variant{0, 1, true},
    Variant{0, 2, true},  Variant{0, 3, true},
    Variant{1, 0, false}, Variant{1, 1, true},
    Variant{1, 2, true},  Variant{1, 3, true},
    Variant{2, 0, false}, Variant{2, 1, true},
    Variant{2, 2, true},  Variant{3, 0, false},
    Variant{3, 1, true},
};

inline void require_count(std::size_t actual, std::size_t expected,
                          const char *message,
                          const std::source_location location =
                              std::source_location::current()) {
  if (actual == expected)
    return;
  std::fprintf(stderr, "%s: expected=%zu actual=%zu at %s:%u\n", message,
               expected, actual, location.file_name(), location.line());
  std::abort();
}

[[nodiscard]] constexpr std::uint32_t
unsigned_memory(const Variant &variant, unsigned immediate, unsigned base,
                unsigned transfer) noexcept {
  return 0x3900'0000U | (variant.size_shift << 30U) |
         (variant.opc << 22U) | ((immediate & 0xfffU) << 10U) |
         ((base & 31U) << 5U) | (transfer & 31U);
}

[[nodiscard]] constexpr std::uint32_t
unsigned_x(bool load, unsigned immediate, unsigned base,
           unsigned transfer) noexcept {
  return unsigned_memory(Variant{3, load ? 1U : 0U, load}, immediate, base,
                         transfer);
}

[[nodiscard]] inline std::vector<std::uint32_t>
hot_code(const uml::cpu::arm64::jit::testing::CompiledCode &compiled,
         std::size_t expected_guest,
         const std::source_location location =
             std::source_location::current()) {
  require(compiled.guest_instructions == expected_guest,
          "JIT did not compile the complete memory-run program", location);
  require(compiled.hot_words <= compiled.words.size(),
          "JIT hot-word boundary exceeds generated code", location);
  return {compiled.words.begin(),
          compiled.words.begin() +
              static_cast<std::ptrdiff_t>(compiled.hot_words)};
}

/* The cold code after a block's hot words. */
[[nodiscard]] inline std::vector<std::uint32_t>
cold_code(const uml::cpu::arm64::jit::testing::CompiledCode &compiled) {
  return {compiled.words.begin() +
              static_cast<std::ptrdiff_t>(compiled.hot_words),
          compiled.words.end()};
}

/* Counts the straddle path's copies of `guest`, which address through the
 * per-access base it selects into x0. */
[[nodiscard]] inline std::size_t
count_straddle_form(std::span<const std::uint32_t> words,
                    std::uint32_t guest) {
  return static_cast<std::size_t>(
      std::ranges::count_if(words, [&](std::uint32_t current) {
        return (current & ~memory_register_mask) ==
                   (guest & ~memory_register_mask) &&
               ((current >> 5U) & 31U) == 0U;
      }));
}

[[nodiscard]] inline std::size_t
count_word(std::span<const std::uint32_t> words, std::uint32_t word) {
  return static_cast<std::size_t>(std::ranges::count(words, word));
}

[[nodiscard]] inline std::size_t count_branch_shape(
    std::span<const std::uint32_t> words, std::uint32_t shape,
    std::uint32_t displacement_mask = compare_branch_displacement_mask) {
  return static_cast<std::size_t>(
      std::ranges::count_if(words, [&](std::uint32_t current) {
        return (current & ~displacement_mask) == shape;
      }));
}

[[nodiscard]] inline std::size_t
count_page_span_tbnz(std::span<const std::uint32_t> words) {
  return count_branch_shape(words, page_span_tbnz,
                            test_branch_displacement_mask);
}

/* A same-shaped word whose base is not x8, the checked host address. */
enum class OtherBase {
  skip, // TLB scaffolding may hold such a load; it is not the guest form.
  fail, // Every such word must be the translated guest form.
};

/* Counts the translated copies of `guest` in `words`. With
 * `scalar_transfer`, each copy must transfer through a register the
 * executor does not own. */
[[nodiscard]] inline std::size_t count_memory_form(
    std::span<const std::uint32_t> words, std::uint32_t guest,
    OtherBase other_base, bool scalar_transfer,
    const std::source_location location = std::source_location::current()) {
  std::size_t count = 0;
  for (const auto current : words) {
    if ((current & ~memory_register_mask) !=
        (guest & ~memory_register_mask))
      continue;
    if (((current >> 5U) & 31U) != 8U) {
      if (other_base == OtherBase::skip)
        continue;
      uml::testing::fail(
          "coalesced memory operation bypassed checked host base x8",
          location);
    }
    if (scalar_transfer) {
      const auto transfer = current & 31U;
      const bool zero_register = (guest & 31U) == 31U;
      const bool safe_transfer =
          transfer == 3U || transfer == 4U || transfer == 5U ||
          transfer == 6U || transfer == 7U || transfer == 9U ||
          transfer == 12U || transfer == 13U || transfer == 17U ||
          (zero_register && transfer == 31U);
      require(safe_transfer,
              "coalesced memory operation used an executor-owned register",
              location);
    }
    ++count;
  }
  return count;
}

} // namespace arm64_jit_memory_run_codegen_test
