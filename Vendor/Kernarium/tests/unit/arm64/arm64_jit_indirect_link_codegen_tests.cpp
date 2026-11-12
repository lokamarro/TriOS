// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "require.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

using uml::testing::a64::indirect_branch;

namespace jit_test = uml::cpu::arm64::jit::testing;

namespace {

using uml::testing::require;

[[nodiscard]] constexpr std::uint32_t
branch_register(unsigned source) noexcept {
  return 0xd61f'0000U | ((source & 31U) << 5U);
}

[[nodiscard]] constexpr std::uint32_t
branch_link_register(unsigned source) noexcept {
  return 0xd63f'0000U | ((source & 31U) << 5U);
}

[[nodiscard]] constexpr std::uint32_t
return_register(unsigned source) noexcept {
  return 0xd65f'0000U | ((source & 31U) << 5U);
}

[[nodiscard]] std::span<const std::uint32_t>
hot_code(const jit_test::CompiledCode &compiled) {
  require(compiled.guest_instructions != 0,
          "indirect branch did not compile");
  require(compiled.hot_words <= compiled.words.size(),
          "hot boundary exceeds generated code");
  return std::span<const std::uint32_t>{compiled.words}.first(
      compiled.hot_words);
}

constexpr std::array persistent_capability_selection{
    0xb940'298dU, // ldr w13, [x12, #40]: allowed selector mask.
    0x9ad5'25adU, // lsrv x13, x13, x21: select the capability bit.
};
constexpr std::uint32_t paired_link_metadata =
    0xa940'2590U; // ldp x16, x9, [x12]: guest PC and instruction count.
constexpr std::uint32_t internal_entry_load =
    0xf940'098dU; // ldr x13, [x12, #16]

[[nodiscard]] constexpr bool
is_immediate_link_entry_load(std::uint32_t word) noexcept {
  /* Old selector: LDR x10/x13, [x12, #0/#8]. */
  const auto without_destination = word & ~std::uint32_t{31};
  return without_destination ==
             (0xf940'0000U | (12U << 5U)) ||
         without_destination ==
             (0xf940'0000U | (1U << 10U) | (12U << 5U));
}

void require_compact_capability_selection(
    std::span<const std::uint32_t> hot) {
  const auto selection =
      std::search(hot.begin(), hot.end(),
                  persistent_capability_selection.begin(),
                  persistent_capability_selection.end());
  require(selection != hot.end(),
          "linked exit does not test the compact capability mask");
  require(selection != hot.begin() && selection + 3 < hot.end(),
          "compact capability selection is truncated");
  require(selection[-1] == paired_link_metadata,
          "link metadata is not loaded before capability fallback");
  require((selection[2] & ~(0x3fffU << 5U)) == 0x3600'000dU,
          "linked exit does not reject an unavailable capability bit");
  require(selection[3] == internal_entry_load,
          "internal entry is not loaded from the compact descriptor");

  std::size_t mask_loads = 0;
  std::size_t variable_shifts = 0;
  std::size_t capability_tests = 0;
  std::size_t paired_metadata_loads = 0;
  std::size_t internal_entry_loads = 0;
  std::size_t outer_selector_loads = 0;
  for (const auto word : hot) {
    mask_loads += word == persistent_capability_selection.front();
    variable_shifts += word == persistent_capability_selection.back();
    capability_tests +=
        (word & ~(0x3fffU << 5U)) == 0x3600'000dU;
    paired_metadata_loads += word == paired_link_metadata;
    internal_entry_loads += word == internal_entry_load;
    outer_selector_loads += word == 0x3940'6275U;
    require(!is_immediate_link_entry_load(word),
            "linked exit still loads a link entry by immediate offset");
    require(word != 0xf875'798dU,
            "linked exit still indexes a capability pointer array");
    require(word != 0xf940'13ebU && word != 0x3940'6169U &&
                word != 0xf869'798dU,
            "linked exit reloads memory-backed chain selection state");
    require(word != 0xf100'013fU,
            "linked exit compares the capability selector");
    require(word != 0x9a8d'014dU,
            "linked exit conditionally selects a capability entry");
    require(word != 0xf940'4189U && word != 0xf940'4589U,
            "linked exit separately loads displaced link metadata");
  }
  require(mask_loads == 1 && variable_shifts == 1 &&
              capability_tests == 1,
          "linked exit must test one compact capability mask");
  require(paired_metadata_loads == 1,
          "linked exit must load link metadata with one paired load");
  require(internal_entry_loads == 1,
          "linked exit must load one internal entry from the descriptor");
  require(outer_selector_loads == 1,
          "outer generated invocation must load its selector exactly once");
}

void two_way_shape_uses_only_host_owned_branch_target(
    std::uint32_t instruction, std::uint64_t pc) {
  const std::array guest{instruction};
  const auto compiled =
      jit_test::compile_indirect_linked_block(pc, guest);
  const auto hot = hot_code(compiled);

  std::size_t target_compares = 0;
  std::size_t host_branches = 0;
  std::size_t host_calls = 0;
  std::size_t site_entry_loads = 0;
  bool records_miss_cell = false;
  bool records_miss_source = false;
  for (const auto word : hot) {
    target_compares += word == 0xeb09'011fU; // cmp x8, x9
    if (indirect_branch(word)) {
      ++host_branches;
      require(((word >> 5U) & 31U) == 13U,
              "guest target register became a native BR operand");
    }
    host_calls += (word & 0xffff'fc1fU) == 0xd63f'0000U;
    if ((word & ~(0x7fU << 15U)) ==
        (0xa940'0000U | (12U << 10U) | (13U << 5U) | 9U))
      ++site_entry_loads;
    records_miss_cell |=
        word == (0xf900'0000U | (2U << 10U) | (19U << 5U) | 13U);
    records_miss_source |=
        word == (0xf900'0000U | (4U << 10U) | (19U << 5U) | 9U);
  }

  require(target_compares == 2,
          "indirect PIC does not compare both target ways");
  require(site_entry_loads == 2,
          "indirect PIC does not load two host-owned entries");
  require(host_branches == 1,
          "indirect PIC must have exactly one native BR");
  require(host_calls == 0,
          "indirect PIC unexpectedly calls through a register");
  require(records_miss_cell,
          "indirect PIC miss does not identify its stable site");
  require(records_miss_source,
          "indirect PIC miss does not record its translation-owned source");
  require_compact_capability_selection(hot);
}

void br_blr_and_ret_are_translation_specialized() {
  constexpr std::uint64_t base = 0x4f80'0000;
  two_way_shape_uses_only_host_owned_branch_target(
      branch_register(5), base);
  two_way_shape_uses_only_host_owned_branch_target(
      branch_link_register(30), base + 0x1000);
  two_way_shape_uses_only_host_owned_branch_target(
      return_register(12), base + 0x2000);
}

void unlinked_compilation_has_no_native_guest_target_branch() {
  constexpr std::uint64_t pc = 0x4f84'0000;
  const std::array guest{branch_register(9)};
  const auto compiled = jit_test::compile_block(pc, guest);
  const auto hot = hot_code(compiled);
  for (const auto word : hot)
    require(!indirect_branch(word),
            "unlinked indirect exit branched to a guest register");
}

void direct_link_uses_compact_capability_selection() {
  constexpr std::uint64_t pc = 0x4f86'0000;
  const std::array guest{0x1400'0001U}; // b pc + 4
  const auto compiled = jit_test::compile_linked_block(pc, guest);
  const auto hot = hot_code(compiled);

  require_compact_capability_selection(hot);
  std::size_t host_branches = 0;
  for (const auto word : hot) {
    if (!indirect_branch(word))
      continue;
    ++host_branches;
    require(((word >> 5U) & 31U) == 13U,
            "direct link BR does not consume the host-selected x13");
  }
  require(host_branches == 1,
          "direct linked exit must have exactly one native BR");
}

void stable_site_storage_is_bounded() {
  require(jit_test::indirect_link_table_is_bounded(0),
          "zero-capacity indirect table accepted a site");
  require(jit_test::indirect_link_table_is_bounded(1),
          "one-entry indirect table is not stable and bounded");
  require(jit_test::indirect_link_table_is_bounded(7),
          "multi-entry indirect table is not stable and bounded");
  require(jit_test::link_chain_instruction_limit() == 4096,
          "JIT link fairness limit diverges from the executor checkpoint");
  require(jit_test::indirect_link_entry_size() == 16,
          "production PIC way no longer contains one key and one pointer");
}

} // namespace

int main() {
  br_blr_and_ret_are_translation_specialized();
  unlinked_compilation_has_no_native_guest_target_branch();
  direct_link_uses_compact_capability_selection();
  stable_site_storage_is_bounded();
  return 0;
}
