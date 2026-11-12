// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_jit_memory_run_codegen_test_support.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <source_location>
#include <span>
#include <vector>

namespace jit_test = uml::cpu::arm64::jit::testing;

namespace {

using namespace arm64_jit_memory_run_codegen_test;

constexpr std::uint64_t code_base = 0x4f40'0000ULL;
constexpr std::uint32_t span_page_tbnz =
    0x3700'0000U | ((page_shift - 4U) << 19U);

[[nodiscard]] std::size_t count_memory_form(
    std::span<const std::uint32_t> words, std::uint32_t guest,
    const std::source_location location = std::source_location::current()) {
  return arm64_jit_memory_run_codegen_test::count_memory_form(
      words, guest, OtherBase::fail, true, location);
}

void every_scalar_variant_coalesces() {
  for (std::size_t index = 0; index < variants.size(); ++index) {
    const auto &variant = variants[index];
    const std::array guest{
        unsigned_memory(variant, 1, 31, 1),
        unsigned_memory(variant, 3, 31, 2),
    };
    const auto compiled =
        jit_test::compile_block(code_base + index * 0x100U, guest);
    const auto hot = hot_code(compiled, guest.size());
    require(count_word(hot, translation_probe) == 1,
            "allocated scalar SP pair did not share one TLB translation");
    require(count_memory_form(hot, guest[0]) == 1,
            "first scalar SP operation lost its native offset");
    require(count_memory_form(hot, guest[1]) == 1,
            "second scalar SP operation lost its native offset");
    const auto expected_mask =
        variant.load ? std::uint64_t{0} : std::uint64_t{3};
    require(compiled.memory_write_mask == expected_mask,
            "SP pair has the wrong memory-write mask");
    require(compiled.direct_memory_write_mask == expected_mask,
            "SP pair has the wrong direct-store mask");
    require(count_word(hot, monitor_clear) == 0U,
            "SP pair cleared the local monitor");

    if (!variant.load) {
      const auto tracked = jit_test::compile_tracked_block(
          code_base + 0x2000U + index * 0x100U, guest);
      const auto tracked_hot = hot_code(tracked, guest.size());
      require(count_word(tracked_hot, translation_probe) == guest.size(),
              "tracked-store translation unexpectedly coalesced");
      require(tracked.memory_write_mask == compiled.memory_write_mask &&
                  tracked.direct_memory_write_mask ==
                      compiled.direct_memory_write_mask,
              "tracked and ordinary store masks differ");
    }
  }
}

void run_lengths_are_bounded_to_four() {
  for (std::size_t length = 2; length <= 6; ++length) {
    std::vector<std::uint32_t> guest;
    guest.reserve(length);
    for (std::size_t index = 0; index < length; ++index)
      guest.push_back(unsigned_x(true, static_cast<unsigned>(index), 31,
                                 static_cast<unsigned>(index + 1U)));
    const auto compiled = jit_test::compile_block(
        code_base + 0x4000U + length * 0x100U, guest);
    const auto hot = hot_code(compiled, guest.size());
    require(count_word(hot, translation_probe) == (length + 3U) / 4U,
            "SP memory run did not split at four instructions");
  }
}

void page_limits_select_only_safe_prefixes() {
  constexpr unsigned page_units = DARWIN_UML_GUEST_PAGE_SIZE / 8U;
  const auto near = unsigned_x(true, 0, 31, 1);
  const auto exact = unsigned_x(true, page_units - 1U, 31, 2);
  const auto beyond = unsigned_x(true, page_units, 31, 3);

  const std::array exact_guest{near, exact};
  const auto exact_compiled =
      jit_test::compile_block(code_base + 0x5000U, exact_guest);
  const auto exact_hot = hot_code(exact_compiled, exact_guest.size());
  require(count_word(exact_hot, translation_probe) == 1,
          "exact-page-end SP pair was not coalesced");
  require(count_word(exact_hot, sp_span_offset_extract) == 1,
          "exact-page-end SP pair omitted its offset proof");
  constexpr auto exact_last_unit =
      static_cast<unsigned>((DARWIN_UML_GUEST_PAGE_SIZE - 1U) >> 4U);
  constexpr auto exact_add = 0x9100'0000U | (exact_last_unit << 10U);
  require(count_word(exact_hot, exact_add) == 1,
          "exact-page-end SP pair has the wrong unit limit");
  require(count_branch_shape(exact_hot, span_page_tbnz,
                             test_branch_displacement_mask) == 1,
          "exact-page-end SP pair omitted its flag-neutral page rejection");

  const std::array rejected{near, beyond};
  const auto rejected_compiled =
      jit_test::compile_block(code_base + 0x5100U, rejected);
  require(count_word(hot_code(rejected_compiled, rejected.size()),
                     translation_probe) ==
              rejected.size(),
          "SP offsets spanning more than a page were coalesced");

  const std::array far{beyond, unsigned_x(true, page_units + 1U, 31, 4)};
  const auto far_hot =
      hot_code(jit_test::compile_block(code_base + 0x5180U, far), far.size());
  require(count_word(far_hot, translation_probe) == 1,
          "SP fields beyond a page did not share one translation");
  require(count_word(far_hot, sp_alignment_extract) == 1,
          "far SP run did not check SP alignment once");
  require(count_word(far_hot, run_base_forward(page_units * 8U)) == 1 &&
              count_word(far_hot, run_base_back(page_units * 8U)) == 1,
          "SP run did not translate its first accessed byte");
  require(count_word(far_hot, page_span_eor) == 1 &&
              count_page_span_tbnz(far_hot) == 1,
          "far SP run omitted its accessed-span page proof");
  require(count_memory_form(far_hot, far[0]) == 1 &&
              count_memory_form(far_hot, far[1]) == 1,
          "far SP fields lost their native offsets");

  const std::array shrunk{near, near, beyond};
  const auto shrunk_compiled =
      jit_test::compile_block(code_base + 0x5200U, shrunk);
  require(count_word(hot_code(shrunk_compiled, shrunk.size()),
                     translation_probe) == 2,
          "unsafe tail did not shrink to a safe two-instruction prefix");
}

void span_proof_matches_each_guest_page_size() {
  const std::array small{
      unsigned_x(true, 0, 31, 1),
      unsigned_x(true, 1, 31, 2),
  };
  const auto small_compiled =
      jit_test::compile_block(code_base + 0x6000U, small);
  const auto small_hot = hot_code(small_compiled, small.size());
  require(count_word(small_hot, sp_alignment_extract) == 1 &&
              count_branch_shape(small_hot, cbnz_x0,
                                 compare_branch_displacement_mask) == 1,
          "small SP run did not check alignment once");
  require(count_word(small_hot, sp_span_offset_extract) == 0,
          "16-byte-contained SP run emitted an unnecessary span guard");

  const std::array guarded{
      unsigned_x(true, 0, 31, 1),
      unsigned_x(true, 2, 31, 2),
  };
  constexpr std::uint64_t maximum_end = 24;
  constexpr auto last_unit =
      static_cast<unsigned>((maximum_end - 1U) >> 4U);
  constexpr auto add_last_unit = 0x9100'0000U | (last_unit << 10U);
  const auto guarded_compiled =
      jit_test::compile_block(code_base + 0x6100U, guarded);
  const auto guarded_hot = hot_code(guarded_compiled, guarded.size());
  require(count_word(guarded_hot, sp_alignment_extract) == 1 &&
              count_branch_shape(guarded_hot, cbnz_x0,
                                 compare_branch_displacement_mask) == 1,
          "guarded SP run repeated its alignment check");
  require(count_word(guarded_hot, sp_span_offset_extract) == 1,
          "guarded SP run has the wrong 4K/16K offset extraction");
  require(count_word(guarded_hot, add_last_unit) == 1,
          "guarded SP run has the wrong translation-time unit limit");
  require(count_branch_shape(guarded_hot, span_page_tbnz,
                             test_branch_displacement_mask) == 1,
          "guarded SP run omitted its flag-neutral page rejection");
}

void infrastructure_guest_registers_are_remapped() {
  constexpr std::array guest_registers{8U, 9U, 16U, 17U, 18U};
  for (std::size_t index = 0; index < guest_registers.size(); ++index) {
    const auto transfer = guest_registers[index];
    for (const bool load : {false, true}) {
      const std::array guest{
          unsigned_x(load, 100, 31, transfer),
          unsigned_x(load, 101, 31, transfer),
      };
      const auto compiled = jit_test::compile_block(
          code_base + 0x6800U + index * 0x100U + (load ? 0x80U : 0U), guest);
      const auto hot = hot_code(compiled, guest.size());
      require(count_word(hot, translation_probe) == 1,
              "infrastructure guest register pair did not coalesce");
      require(count_memory_form(hot, guest[0]) == 1,
              "first infrastructure guest register was not remapped");
      require(count_memory_form(hot, guest[1]) == 1,
              "second infrastructure guest register was not remapped");
    }
  }
}

void reserved_scalar_cells_do_not_coalesce() {
  constexpr std::array reserved{
      Variant{2, 3, true},
      Variant{3, 3, true},
  };
  for (std::size_t index = 0; index < reserved.size(); ++index) {
    const std::array guest{
        unsigned_memory(reserved[index], 0, 31, 1),
        unsigned_memory(reserved[index], 1, 31, 2),
    };
    const auto compiled =
        jit_test::compile_block(code_base + 0x6e00U + index * 0x100U, guest);
    require(compiled.guest_instructions == 1,
            "reserved scalar size/opc cell was accepted as a memory run");
    const auto hot = std::span<const std::uint32_t>{compiled.words}.first(
        compiled.hot_words);
    require(count_word(hot, translation_probe) == 0,
            "reserved scalar size/opc cell performed a TLB translation");
    require(count_word(hot, sp_alignment_extract) == 0,
            "reserved scalar size/opc cell emitted an SP-run alignment check");
  }
}

void unrelated_allocations_do_not_coalesce() {
  const std::array mixed{
      unsigned_x(false, 0, 31, 1),
      unsigned_x(true, 1, 31, 2),
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0x7000U, mixed),
                       mixed.size()),
              translation_probe) == mixed.size(),
          "mixed load/store direction was coalesced");

  const std::array non_sp{
      unsigned_x(true, 0, 2, 1),
      unsigned_x(true, 1, 2, 3),
  };
  const auto non_sp_hot =
      hot_code(jit_test::compile_block(code_base + 0x7100U, non_sp),
               non_sp.size());
  require_count(count_word(non_sp_hot, translation_probe), 1,
                "non-SP unsigned memory did not use its GPR run");
  require(count_word(non_sp_hot, sp_alignment_extract) == 0,
          "GPR memory run emitted an SP alignment check");

  const std::array vector{
      std::uint32_t{0x3dc0'03e0U},
      std::uint32_t{0x3dc0'07e1U},
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0x7200U, vector),
                       vector.size()),
              translation_probe) == 1,
          "vector unsigned memory did not use its dedicated shared run");

  const std::array unscaled{
      std::uint32_t{0xf840'03e1U},
      std::uint32_t{0xf840'83e2U},
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0x7300U, unscaled),
                       unscaled.size()),
              translation_probe) == unscaled.size(),
          "non-unsigned addressing mode was coalesced");

  const std::array prefetch{
      std::uint32_t{0xf980'03e0U},
      std::uint32_t{0xf980'07e1U},
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0x7400U, prefetch),
                       prefetch.size()),
              translation_probe) == 0,
          "prefetch hints performed a memory translation");
}

} // namespace

int main() {
  every_scalar_variant_coalesces();
  run_lengths_are_bounded_to_four();
  page_limits_select_only_safe_prefixes();
  span_proof_matches_each_guest_page_size();
  infrastructure_guest_registers_are_remapped();
  reserved_scalar_cells_do_not_coalesce();
  unrelated_allocations_do_not_coalesce();
  return 0;
}
