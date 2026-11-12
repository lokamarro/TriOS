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

constexpr std::uint64_t code_base = 0x5040'0000ULL;

/* TLB scaffolding can contain an unrelated load with the same size, opcode,
 * and immediate; the translated guest form fixes Rn to x8. */
[[nodiscard]] std::size_t count_memory_form(
    std::span<const std::uint32_t> words, std::uint32_t guest,
    const std::source_location location = std::source_location::current()) {
  return arm64_jit_memory_run_codegen_test::count_memory_form(
      words, guest, OtherBase::skip, true, location);
}

void every_scalar_variant_coalesces() {
  for (std::size_t index = 0; index < variants.size(); ++index) {
    const auto &variant = variants[index];
    const std::array guest{
        unsigned_memory(variant, 1, 2, 3),
        unsigned_memory(variant, 3, 2, 4),
    };
    const auto compiled =
        jit_test::compile_block(code_base + index * 0x100U, guest);
    const auto hot = hot_code(compiled, guest.size());
    require(count_word(hot, translation_probe) == 1,
            "allocated scalar GPR pair did not share one TLB translation");
    require(count_memory_form(hot, guest[0]) == 1,
            "first scalar GPR operation lost its native offset");
    require(count_memory_form(hot, guest[1]) == 1,
            "second scalar GPR operation lost its native offset");
    require(count_word(hot, sp_alignment_extract) == 0,
            "non-SP memory run emitted an SP alignment check");
    const auto expected_mask =
        variant.load ? std::uint64_t{0} : std::uint64_t{3};
    require(compiled.memory_write_mask == expected_mask,
            "GPR pair has the wrong memory-write mask");
    require(compiled.direct_memory_write_mask == expected_mask,
            "GPR pair has the wrong direct-store mask");
    require(count_word(hot, monitor_clear) == 0U,
            "GPR pair cleared the local monitor");

    if (!variant.load) {
      const auto tracked = jit_test::compile_tracked_block(
          code_base + 0x2000U + index * 0x100U, guest);
      const auto tracked_hot = hot_code(tracked, guest.size());
      require(count_word(tracked_hot, translation_probe) == guest.size(),
              "tracked-store GPR translation unexpectedly coalesced");
      require(tracked.memory_write_mask == compiled.memory_write_mask &&
                  tracked.direct_memory_write_mask ==
                      compiled.direct_memory_write_mask,
              "tracked and ordinary GPR store masks differ");
    }
  }
}

void run_lengths_are_bounded_to_four() {
  for (std::size_t length = 2; length <= 6; ++length) {
    std::vector<std::uint32_t> guest;
    guest.reserve(length);
    for (std::size_t index = 0; index < length; ++index)
      guest.push_back(unsigned_x(
          true, static_cast<unsigned>(index), 2,
          static_cast<unsigned>(index + 3U)));
    const auto compiled = jit_test::compile_block(
        code_base + 0x4000U + length * 0x100U, guest);
    const auto hot = hot_code(compiled, guest.size());
    require(count_word(hot, translation_probe) == (length + 3U) / 4U,
            "GPR memory run did not split at four instructions");
  }
}

void grouping_boundaries_are_exact() {
  const std::array mixed_direction{
      unsigned_x(false, 0, 2, 3),
      unsigned_x(true, 1, 2, 4),
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0x5000U,
                                               mixed_direction),
                       mixed_direction.size()),
              translation_probe) == mixed_direction.size(),
          "mixed GPR load/store direction was coalesced");

  const std::array mixed_base{
      unsigned_x(true, 0, 2, 3),
      unsigned_x(true, 1, 5, 4),
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0x5100U,
                                               mixed_base),
                       mixed_base.size()),
              translation_probe) == mixed_base.size(),
          "different GPR base registers were coalesced");

  const std::array mixed_sp{
      unsigned_x(true, 0, 2, 3),
      unsigned_x(true, 1, 31, 4),
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0x5200U,
                                               mixed_sp),
                       mixed_sp.size()),
              translation_probe) == mixed_sp.size(),
          "GPR and SP operations were coalesced together");
}

void base_alias_rules_are_exact() {
  const std::array first_load_alias{
      unsigned_x(true, 0, 2, 2),
      unsigned_x(true, 1, 2, 3),
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0x5800U,
                                               first_load_alias),
                       first_load_alias.size()),
              translation_probe) == first_load_alias.size(),
          "load Rt==Rn was accepted as the first operation of a run");

  const std::array second_load_alias{
      unsigned_x(true, 0, 2, 3),
      unsigned_x(true, 1, 2, 2),
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0x5900U,
                                               second_load_alias),
                       second_load_alias.size()),
              translation_probe) == second_load_alias.size(),
          "load Rt==Rn was accepted later in a run");

  const std::array alias_after_safe_prefix{
      unsigned_x(true, 0, 2, 3),
      unsigned_x(true, 1, 2, 4),
      unsigned_x(true, 2, 2, 2),
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0x5a00U,
                                               alias_after_safe_prefix),
                       alias_after_safe_prefix.size()),
              translation_probe) == 2,
          "load Rt==Rn did not terminate the preceding safe run");

  const std::array store_alias{
      unsigned_x(false, 0, 2, 2),
      unsigned_x(false, 1, 2, 3),
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0x5b00U,
                                               store_alias),
                       store_alias.size()),
              translation_probe) == 1,
          "safe store Rt==Rn pair did not coalesce");
}

void page_limits_select_only_safe_prefixes() {
  constexpr unsigned page_units = DARWIN_UML_GUEST_PAGE_SIZE / 8U;
  const auto near = unsigned_x(true, 0, 2, 3);
  const auto exact = unsigned_x(true, page_units - 1U, 2, 4);
  const auto beyond = unsigned_x(true, page_units, 2, 5);

  const std::array exact_guest{near, exact};
  const auto exact_compiled =
      jit_test::compile_block(code_base + 0x6000U, exact_guest);
  const auto exact_hot = hot_code(exact_compiled, exact_guest.size());
  require(count_word(exact_hot, translation_probe) == 1,
          "exact-page-span GPR pair was not coalesced");
  require(count_word(exact_hot, page_span_eor) == 1,
          "exact-page-span GPR pair omitted its end-address comparison");
  require(count_page_span_tbnz(exact_hot) == 1,
          "exact-page-span GPR pair omitted its page-bit rejection");
  const auto exact_cold = cold_code(exact_compiled);
  require(count_word(exact_cold, straddle_next_page) == 1 &&
              count_straddle_form(exact_cold, near) == 1 &&
              count_straddle_form(exact_cold, exact) == 1,
          "GPR run lacks a straddle path replaying each access");

  const std::array rejected{near, beyond};
  const auto rejected_compiled =
      jit_test::compile_block(code_base + 0x6100U, rejected);
  require(count_word(hot_code(rejected_compiled, rejected.size()),
                     translation_probe) ==
              rejected.size(),
          "GPR offsets spanning more than a page were coalesced");

  const std::array far{beyond, unsigned_x(true, page_units + 1U, 2, 6)};
  const auto far_hot =
      hot_code(jit_test::compile_block(code_base + 0x6180U, far), far.size());
  require(count_word(far_hot, translation_probe) == 1,
          "GPR fields beyond a page did not share one translation");
  require(count_word(far_hot, run_base_forward(page_units * 8U)) == 1 &&
              count_word(far_hot, run_base_back(page_units * 8U)) == 1,
          "GPR run did not translate its first accessed byte");
  require(count_memory_form(far_hot, far[0]) == 1 &&
              count_memory_form(far_hot, far[1]) == 1,
          "far GPR fields lost their native offsets");

  const std::array shrunk{near, near, beyond};
  const auto shrunk_compiled =
      jit_test::compile_block(code_base + 0x6200U, shrunk);
  require(count_word(hot_code(shrunk_compiled, shrunk.size()),
                     translation_probe) == 2,
          "unsafe GPR tail did not shrink to a safe two-instruction prefix");

  const std::array same_byte{
      unsigned_memory(variants[1], 0, 2, 3),
      unsigned_memory(variants[1], 0, 2, 4),
  };
  const auto same_compiled =
      jit_test::compile_block(code_base + 0x6300U, same_byte);
  const auto same_hot = hot_code(same_compiled, same_byte.size());
  require(count_word(same_hot, translation_probe) == 1,
          "same-byte GPR loads did not coalesce");
  require(count_word(same_hot, page_span_eor) == 0 &&
              count_page_span_tbnz(same_hot) == 0,
          "zero-width page span emitted a redundant runtime proof");
  require(count_word(same_compiled.words, straddle_next_page) == 0,
          "a run that cannot straddle emitted a straddle path");
}

void infrastructure_guest_registers_are_remapped() {
  for (unsigned base = 0; base < 31U; ++base) {
    const unsigned first = (base + 1U) % 31U;
    const unsigned second = (base + 2U) % 31U;
    const std::array guest{
        unsigned_x(true, 4, base, first),
        unsigned_x(true, 5, base, second),
    };
    const auto compiled = jit_test::compile_block(
        code_base + 0x6800U + base * 0x80U, guest);
    const auto hot = hot_code(compiled, guest.size());
    require(count_word(hot, translation_probe) == 1,
            "infrastructure guest base pair did not coalesce");
    require(count_memory_form(hot, guest[0]) == 1 &&
                count_memory_form(hot, guest[1]) == 1,
            "infrastructure guest base was not remapped to x8");
  }

  for (unsigned transfer = 0; transfer < 32U; ++transfer) {
    for (const bool load : {false, true}) {
      const unsigned base = transfer == 2U ? 3U : 2U;
      const std::array guest{
          unsigned_x(load, 20, base, transfer),
          unsigned_x(load, 21, base, transfer),
      };
      const auto compiled = jit_test::compile_block(
          code_base + 0x8000U + transfer * 0x100U +
              (load ? 0x80U : 0U),
          guest);
      const auto hot = hot_code(compiled, guest.size());
      require(count_word(hot, translation_probe) == 1,
              "infrastructure transfer-register pair did not coalesce");
      require(count_memory_form(hot, guest[0]) == 1 &&
                  count_memory_form(hot, guest[1]) == 1,
              "infrastructure transfer register was not remapped");
    }
  }
}

void store_masks_include_every_grouped_instruction() {
  const std::array guest{
      nop,
      unsigned_x(false, 0, 2, 3),
      unsigned_x(false, 1, 2, 4),
      unsigned_x(false, 2, 2, 5),
      unsigned_x(false, 3, 2, 6),
  };
  const auto compiled = jit_test::compile_block(code_base + 0x8000U, guest);
  const auto hot = hot_code(compiled, guest.size());
  require(compiled.memory_write_mask == 0x1eU,
          "prefixed GPR run omitted a memory-write bit");
  require(compiled.direct_memory_write_mask == 0x1eU,
          "prefixed GPR run omitted a direct-store bit");
  require(count_word(hot, monitor_clear) == 0,
          "four-store GPR run cleared the local monitor");
}

void reserved_and_unrelated_allocations_do_not_coalesce() {
  constexpr std::array reserved{
      Variant{2, 3, true},
      Variant{3, 3, true},
  };
  for (std::size_t index = 0; index < reserved.size(); ++index) {
    const std::array guest{
        unsigned_memory(reserved[index], 0, 2, 3),
        unsigned_memory(reserved[index], 1, 2, 4),
    };
    const auto compiled =
        jit_test::compile_block(code_base + 0x8800U + index * 0x100U, guest);
    require(compiled.guest_instructions == 1,
            "reserved scalar cell was accepted as a GPR memory run");
    const auto hot = std::span<const std::uint32_t>{compiled.words}.first(
        compiled.hot_words);
    require(count_word(hot, translation_probe) == 0,
            "reserved scalar cell performed a TLB translation");
  }

  const std::array vector{
      std::uint32_t{0x3dc0'0041U},
      std::uint32_t{0x3dc0'0442U},
  };
  const auto vector_hot =
      hot_code(jit_test::compile_block(code_base + 0x8a00U, vector),
               vector.size());
  require_count(
      count_word(vector_hot, translation_probe), 1,
      "vector unsigned memory did not use its dedicated shared run");
  require(count_word(vector_hot, sp_alignment_extract) == 0,
          "GPR-based vector run emitted the scalar SP alignment guard");

  const std::array unscaled{
      std::uint32_t{0xf840'0041U},
      std::uint32_t{0xf840'8042U},
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0x8b00U, unscaled),
                       unscaled.size()),
              translation_probe) == unscaled.size(),
          "non-unsigned addressing mode was accepted as a GPR run");

  const std::array prefetch{
      std::uint32_t{0xf980'0040U},
      std::uint32_t{0xf980'0441U},
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0x8c00U, prefetch),
                       prefetch.size()),
              translation_probe) == 0,
          "prefetch hints performed a memory translation");
}

} // namespace

int main() {
  every_scalar_variant_coalesces();
  run_lengths_are_bounded_to_four();
  grouping_boundaries_are_exact();
  base_alias_rules_are_exact();
  page_limits_select_only_safe_prefixes();
  infrastructure_guest_registers_are_remapped();
  store_masks_include_every_grouped_instruction();
  reserved_and_unrelated_allocations_do_not_coalesce();
  return 0;
}
