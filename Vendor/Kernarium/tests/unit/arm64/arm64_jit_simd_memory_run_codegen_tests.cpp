// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_jit_memory_run_codegen_test_support.hpp"
#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/jit/jit_runtime_contract.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "require.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <source_location>
#include <span>
#include <vector>

namespace arm64 = uml::cpu::arm64;
namespace jit_test = uml::cpu::arm64::jit::testing;

namespace {

using arm64_jit_memory_run_codegen_test::cbnz_x0;
using arm64_jit_memory_run_codegen_test::cold_code;
using arm64_jit_memory_run_codegen_test::count_straddle_form;
using arm64_jit_memory_run_codegen_test::count_branch_shape;
using arm64_jit_memory_run_codegen_test::count_page_span_tbnz;
using arm64_jit_memory_run_codegen_test::count_word;
using arm64_jit_memory_run_codegen_test::hot_code;
using arm64_jit_memory_run_codegen_test::monitor_clear;
using arm64_jit_memory_run_codegen_test::nop;
using arm64_jit_memory_run_codegen_test::page_span_eor;
using arm64_jit_memory_run_codegen_test::require_count;
using arm64_jit_memory_run_codegen_test::run_base_back;
using arm64_jit_memory_run_codegen_test::run_base_forward;
using arm64_jit_memory_run_codegen_test::sp_alignment_extract;
using arm64_jit_memory_run_codegen_test::sp_span_offset_extract;
using arm64_jit_memory_run_codegen_test::straddle_next_page;
using arm64_jit_memory_run_codegen_test::translation_probe;
using uml::testing::require;

constexpr std::uint64_t code_base = 0x50c0'0000ULL;
/* ldrb w0, [x15, #tbi_enabled]: no translation tests the flag, which its
 * key carries instead. */
constexpr std::uint32_t tagged_state_load =
    0x3940'0000U |
    (static_cast<std::uint32_t>(offsetof(arm64::CpuState, tbi_enabled))
     << 10U) |
    (15U << 5U);
/* ubfx x8, x8, #0, #56: the top-byte strip of a translation compiled for
 * top-byte ignore, once per run off a non-SP base. */
constexpr std::uint32_t tag_strip = 0xd340'dd08U;

[[nodiscard]] jit_test::CompiledCode
compile_tagged(std::uint64_t pc, std::span<const std::uint32_t> guest) {
  return jit_test::compile_block_in_mode(pc, guest,
                                         arm64::translation_mode_tbi)
      .code;
}

struct Variant final {
  unsigned size;
  unsigned opc;
  std::size_t bytes;
  bool load;
};

constexpr std::array variants{
    Variant{0, 0, 1, false}, Variant{0, 1, 1, true},
    Variant{1, 0, 2, false}, Variant{1, 1, 2, true},
    Variant{2, 0, 4, false}, Variant{2, 1, 4, true},
    Variant{3, 0, 8, false}, Variant{3, 1, 8, true},
    Variant{0, 2, 16, false}, Variant{0, 3, 16, true},
};

[[nodiscard]] constexpr std::uint32_t
unsigned_memory(const Variant &variant, unsigned immediate, unsigned base,
                unsigned transfer) noexcept {
  return 0x3d00'0000U | (variant.size << 30U) |
         (variant.opc << 22U) | ((immediate & 0xfffU) << 10U) |
         ((base & 31U) << 5U) | (transfer & 31U);
}

[[nodiscard]] std::size_t count_memory_form(
    std::span<const std::uint32_t> words, std::uint32_t guest,
    const std::source_location location = std::source_location::current()) {
  return arm64_jit_memory_run_codegen_test::count_memory_form(
      words, guest, arm64_jit_memory_run_codegen_test::OtherBase::fail,
      false, location);
}

void every_valid_variant_coalesces_for_both_base_kinds() {
  for (const unsigned base : {31U, 2U}) {
    for (std::size_t index = 0; index < variants.size(); ++index) {
      const auto &variant = variants[index];
      const std::array guest{
          unsigned_memory(variant, 1, base,
                          static_cast<unsigned>(index)),
          unsigned_memory(variant, 3, base,
                          static_cast<unsigned>(31U - index)),
      };
      const auto compiled = jit_test::compile_block(
          code_base + base * 0x1000U + index * 0x80U, guest);
      const auto hot = hot_code(compiled, guest.size());
      require(count_word(hot, translation_probe) == 1,
              "valid SIMD pair did not share one permission translation");
      require(count_memory_form(hot, guest[0]) == 1 &&
                  count_memory_form(hot, guest[1]) == 1,
              "SIMD run did not preserve both native memory forms");
      require(count_word(hot, sp_alignment_extract) ==
                  (base == 31U ? 1U : 0U) &&
                  count_branch_shape(hot, cbnz_x0) ==
                      (base == 31U ? 1U : 0U),
              "SIMD run has the wrong SP alignment-check count");
      require(count_word(hot, tagged_state_load) == 0U &&
                  count_word(hot, tag_strip) == 0U,
              "SIMD run without top-byte ignore does tag work");
      const auto tagged = compile_tagged(
          code_base + 0x100'000U + base * 0x1000U + index * 0x80U, guest);
      const auto tagged_hot = hot_code(tagged, guest.size());
      require(count_word(tagged_hot, tagged_state_load) == 0U &&
                  count_word(tagged_hot, tag_strip) == 1U,
              "SIMD run has the wrong top-byte strip count");
      require(count_word(tagged_hot, translation_probe) == 1 &&
                  count_memory_form(tagged_hot, guest[0]) == 1 &&
                  count_memory_form(tagged_hot, guest[1]) == 1,
              "SIMD run lost its shape under top-byte ignore");
      const auto expected_mask =
          variant.load ? std::uint64_t{0} : std::uint64_t{3};
      require(compiled.memory_write_mask == expected_mask &&
                  compiled.direct_memory_write_mask == expected_mask,
              "SIMD pair has the wrong store masks");
      require(count_word(hot, monitor_clear) == 0U,
              "SIMD pair cleared the local monitor");

      if (!variant.load) {
        const auto tracked = jit_test::compile_tracked_block(
            code_base + 0x40'000U + base * 0x1000U + index * 0x80U,
            guest);
        const auto tracked_hot = hot_code(tracked, guest.size());
        require(count_word(tracked_hot, translation_probe) == guest.size(),
                "tracked SIMD stores were coalesced");
        require(tracked.memory_write_mask == expected_mask &&
                    tracked.direct_memory_write_mask == expected_mask,
                "tracked SIMD stores have the wrong masks");
      }
    }
  }
}

void mixed_sizes_coalesce_by_direction() {
  constexpr std::array store_indices{0U, 2U, 4U, 6U, 8U};
  constexpr std::array load_indices{1U, 3U, 5U, 7U, 9U};
  for (const unsigned base : {31U, 4U}) {
    for (const auto &indices : {store_indices, load_indices}) {
      std::array<std::uint32_t, store_indices.size()> guest{};
      for (std::size_t index = 0; index < guest.size(); ++index)
        guest[index] = unsigned_memory(
            variants[indices[index]], static_cast<unsigned>(index + 1U),
            base, static_cast<unsigned>(index * 7U) & 31U);
      const auto compiled = jit_test::compile_block(
          code_base + 0x80'000U + base * 0x100U +
              (variants[indices[0]].load ? 0x80U : 0U),
          guest);
      const auto hot = hot_code(compiled, guest.size());
      require(count_word(hot, translation_probe) == 1,
              "mixed-size same-direction SIMD run did not coalesce");
      for (const auto instruction : guest)
        require(count_memory_form(hot, instruction) == 1,
                "mixed-size SIMD run lost an original memory form");
    }
  }
}

void run_lengths_are_bounded_to_eight() {
  for (const unsigned base : {31U, 6U}) {
    for (std::size_t length = 2; length <= 10; ++length) {
      std::vector<std::uint32_t> guest;
      guest.reserve(length);
      for (std::size_t index = 0; index < length; ++index)
        guest.push_back(unsigned_memory(
            variants[9], static_cast<unsigned>(index), base,
            static_cast<unsigned>(index) & 31U));
      const auto compiled = jit_test::compile_block(
          code_base + 0x90'000U + base * 0x1000U + length * 0x40U,
          guest);
      const auto hot = hot_code(compiled, guest.size());
      const auto groups = (length + 7U) / 8U;
      require(count_word(hot, translation_probe) == groups,
              "SIMD memory run did not split at eight instructions");
      require(count_word(hot, sp_alignment_extract) ==
                  (base == 31U ? groups : 0U) &&
                  count_branch_shape(hot, cbnz_x0) ==
                      (base == 31U ? groups : 0U),
              "split SIMD run has the wrong alignment-check count");
      require(count_word(hot, tagged_state_load) == 0U &&
                  count_word(hot, tag_strip) == 0U,
              "split SIMD run without top-byte ignore does tag work");
      const auto tagged = compile_tagged(
          code_base + 0x98'000U + base * 0x1000U + length * 0x40U, guest);
      require(count_word(hot_code(tagged, guest.size()), tag_strip) == groups,
              "split SIMD run has the wrong top-byte strip count");
    }
  }
}

void every_vector_register_is_remapped() {
  for (unsigned transfer = 0; transfer < 32U; ++transfer) {
    for (const bool load : {false, true}) {
      const auto &variant = variants[load ? 9U : 8U];
      for (const unsigned base : {31U, 7U}) {
        const std::array guest{
            unsigned_memory(variant, 4, base, transfer),
            unsigned_memory(variant, 5, base, transfer),
        };
        const auto compiled = jit_test::compile_block(
            code_base + 0xa0'000U + transfer * 0x200U +
                (load ? 0x100U : 0U) + base * 4U,
            guest);
        const auto hot = hot_code(compiled, guest.size());
        require(count_word(hot, translation_probe) == 1,
                "V0-V31 SIMD pair did not coalesce");
        require(count_memory_form(hot, guest[0]) == 1 &&
                    count_memory_form(hot, guest[1]) == 1,
                "V0-V31 SIMD pair was not remapped to native transfers");
      }
    }
  }
}

void page_proofs_cover_both_guest_page_sizes() {
  constexpr unsigned page_qwords = DARWIN_UML_GUEST_PAGE_SIZE / 16U;
  const auto near = unsigned_memory(variants[9], 0, 31, 0);
  const auto exact =
      unsigned_memory(variants[9], page_qwords - 1U, 31, 1);
  const auto beyond = unsigned_memory(variants[9], page_qwords, 31, 2);

  const std::array exact_sp{near, exact};
  const auto exact_sp_compiled =
      jit_test::compile_block(code_base + 0xb0'000U, exact_sp);
  const auto exact_sp_hot = hot_code(exact_sp_compiled, exact_sp.size());
  require_count(count_word(exact_sp_hot, translation_probe), 1,
                "exact-page-end SIMD SP pair did not coalesce");
  require(count_word(exact_sp_hot, sp_span_offset_extract) == 1,
          "SIMD SP pair omitted its 4K/16K page-offset proof");
  const auto exact_sp_cold = cold_code(exact_sp_compiled);
  require(count_word(exact_sp_cold, straddle_next_page) == 1 &&
              count_straddle_form(exact_sp_cold, near) == 1 &&
              count_straddle_form(exact_sp_cold, exact) == 1,
          "SIMD SP run lacks a straddle path replaying each access");

  const std::array rejected_sp{near, beyond};
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0xb0'100U,
                                               rejected_sp),
                       rejected_sp.size()),
              translation_probe) == rejected_sp.size(),
          "SIMD SP offsets spanning more than a page were coalesced");

  for (const unsigned base : {31U, 9U}) {
    const std::array far{
        unsigned_memory(variants[9], page_qwords, base, 2),
        unsigned_memory(variants[9], page_qwords + 1U, base, 3),
    };
    const auto far_hot = hot_code(
        jit_test::compile_block(code_base + 0xb0'180U + base * 0x10U, far),
        far.size());
    require_count(count_word(far_hot, translation_probe), 1,
                  "SIMD fields beyond a page did not share one translation");
    require(count_word(far_hot, run_base_forward(page_qwords * 16U)) == 1 &&
                count_word(far_hot, run_base_back(page_qwords * 16U)) == 1,
            "SIMD run did not translate its first accessed byte");
    require(count_word(far_hot, page_span_eor) == 1 &&
                count_page_span_tbnz(far_hot) == 1,
            "far SIMD run omitted its accessed-span page proof");
  }

  const std::array shrunk_sp{near, near, beyond};
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0xb0'200U,
                                               shrunk_sp),
                       shrunk_sp.size()),
              translation_probe) == 2,
          "unsafe SIMD SP tail did not shrink to a safe prefix");

  const std::array exact_gpr{
      unsigned_memory(variants[9], 0, 9, 0),
      unsigned_memory(variants[9], page_qwords - 1U, 9, 1),
  };
  const auto exact_gpr_hot =
      hot_code(jit_test::compile_block(code_base + 0xb0'300U, exact_gpr),
               exact_gpr.size());
  require(count_word(exact_gpr_hot, translation_probe) == 1,
          "exact-page-span SIMD GPR pair did not coalesce");
  require(count_word(exact_gpr_hot, page_span_eor) == 1 &&
              count_page_span_tbnz(exact_gpr_hot) == 1,
          "SIMD GPR pair omitted its 4K/16K same-page proof");

  const std::array same_byte{
      unsigned_memory(variants[1], 0, 9, 0),
      unsigned_memory(variants[1], 0, 9, 1),
  };
  const auto same_byte_hot =
      hot_code(jit_test::compile_block(code_base + 0xb0'400U, same_byte),
               same_byte.size());
  require(count_word(same_byte_hot, page_span_eor) == 0 &&
              count_page_span_tbnz(same_byte_hot) == 0,
          "same-byte SIMD GPR loads emitted a redundant page proof");
}

void grouping_boundaries_and_reserved_cells_are_exact() {
  const std::array mixed_direction{
      unsigned_memory(variants[8], 0, 2, 0),
      unsigned_memory(variants[9], 1, 2, 1),
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0xc0'000U,
                                               mixed_direction),
                       mixed_direction.size()),
              translation_probe) == mixed_direction.size(),
          "mixed SIMD load/store direction was coalesced");

  const std::array mixed_base{
      unsigned_memory(variants[9], 0, 2, 0),
      unsigned_memory(variants[9], 1, 3, 1),
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0xc0'100U,
                                               mixed_base),
                       mixed_base.size()),
              translation_probe) == mixed_base.size(),
          "different SIMD base registers were coalesced");

  const std::array mixed_sp{
      unsigned_memory(variants[9], 0, 2, 0),
      unsigned_memory(variants[9], 1, 31, 1),
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0xc0'200U,
                                               mixed_sp),
                       mixed_sp.size()),
              translation_probe) == mixed_sp.size(),
          "SIMD GPR and SP bases were coalesced together");

  constexpr Variant scalar_x{3, 1, 8, true};
  const std::array mixed_class{
      unsigned_memory(variants[9], 0, 2, 0),
      (unsigned_memory(scalar_x, 1, 2, 1) & ~0x0400'0000U),
  };
  require(count_word(
              hot_code(jit_test::compile_block(code_base + 0xc0'300U,
                                               mixed_class),
                       mixed_class.size()),
              translation_probe) == mixed_class.size(),
          "SIMD and integer memory classes were coalesced together");

  for (const unsigned size : {1U, 2U, 3U}) {
    for (const unsigned opc : {2U, 3U}) {
      const Variant reserved{size, opc, 16, (opc & 1U) != 0U};
      const std::array guest{
          unsigned_memory(reserved, 0, 2, 0),
          unsigned_memory(reserved, 1, 2, 1),
      };
      const auto compiled = jit_test::compile_block(
          code_base + 0xc1'000U + size * 0x100U + opc * 0x20U, guest);
      require(compiled.guest_instructions == 1,
              "reserved SIMD size/opc cell was accepted as a run");
      require(count_word(
                  std::span<const std::uint32_t>{compiled.words}.first(
                      compiled.hot_words),
                  translation_probe) == 0,
              "reserved SIMD size/opc cell performed a translation");
    }
  }
}

void store_masks_cover_dirty_prefix_and_complete_run() {
  std::array<std::uint32_t, 9> guest{};
  guest[0] = nop;
  for (std::size_t index = 1; index < guest.size(); ++index)
    guest[index] = unsigned_memory(
        variants[8], static_cast<unsigned>(index - 1U), 10,
        static_cast<unsigned>(index - 1U));
  const auto compiled =
      jit_test::compile_block(code_base + 0xd0'000U, guest);
  const auto hot = hot_code(compiled, guest.size());
  require(compiled.memory_write_mask == 0x1feU &&
              compiled.direct_memory_write_mask == 0x1feU,
          "eight-store SIMD run omitted prefixed store-mask bits");
  require(count_word(hot, translation_probe) == 1,
          "eight-store SIMD run repeated its permission translation");
  require(count_word(hot, monitor_clear) == 0,
          "eight-store SIMD run cleared the local monitor");
}

} // namespace

int main() {
  every_valid_variant_coalesces_for_both_base_kinds();
  mixed_sizes_coalesce_by_direction();
  run_lengths_are_bounded_to_eight();
  every_vector_register_is_remapped();
  page_proofs_cover_both_guest_page_sizes();
  grouping_boundaries_and_reserved_cells_are_exact();
  store_masks_cover_dirty_prefix_and_complete_run();
  return 0;
}
