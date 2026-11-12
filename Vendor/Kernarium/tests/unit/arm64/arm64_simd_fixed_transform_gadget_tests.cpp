// SPDX-License-Identifier: GPL-2.0-or-later
#include "a64_body_inspection.hpp"
#include "arm64_engine_test_support.hpp"
#include "arm64_gadget_test_support.hpp"
#include "require.hpp"

#include "cpu/arm64/gadgets/gadget_operand_encoding.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <set>
#include <span>
#include <string_view>

namespace arm64_simd_fixed_transform_test {

namespace arm64 = uml::cpu::arm64;
namespace engine = uml::emulator::tests;

struct Registers2 final {
  unsigned destination;
  unsigned source;
};

struct Registers3 final {
  unsigned destination;
  unsigned first;
  unsigned second;
};

using uml::testing::fail;
using uml::testing::require;

[[nodiscard]] inline engine::NativeRequirements feature1() {
  return engine::NativeRequirements::features(::uml::cpu::ArmFeature::sha1);
}

[[nodiscard]] inline engine::NativeRequirements feature2() {
  return engine::NativeRequirements::features(::uml::cpu::ArmFeature::sha256);
}

[[nodiscard]] inline engine::NativeRequirements feature64() {
  return engine::NativeRequirements::features(::uml::cpu::ArmFeature::sha512);
}

[[nodiscard]] constexpr std::uint32_t transform32_three(unsigned operation,
                                                        Registers3 registers) {
  return 0x5e00'0000U | ((registers.second & 31U) << 16) |
         ((operation & 7U) << 12) | ((registers.first & 31U) << 5) |
         (registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t transform32_two(unsigned operation,
                                                      Registers2 registers) {
  return 0x5e28'0800U | ((operation & 3U) << 12) |
         ((registers.source & 31U) << 5) | (registers.destination & 31U);
}

inline constexpr std::array transform64_three_bases{
    0xce60'8000U,
    0xce60'8400U,
    0xce60'8800U,
};

[[nodiscard]] constexpr std::uint32_t transform64_three(unsigned operation,
                                                        Registers3 registers) {
  return transform64_three_bases[operation] | ((registers.second & 31U) << 16) |
         ((registers.first & 31U) << 5) | (registers.destination & 31U);
}

[[nodiscard]] constexpr std::uint32_t transform64_two(Registers2 registers) {
  return 0xcec0'8000U | ((registers.source & 31U) << 5) |
         (registers.destination & 31U);
}

} // namespace arm64_simd_fixed_transform_test

namespace a64 = uml::testing::a64;

extern "C" {
#define UML_DECLARE_FIXED_GADGET(name)                                         \
  void uml_arm64_gadget_simd_##name();                                         \
  void uml_arm64_gadget_simd_##name##_soft();
UML_DECLARE_FIXED_GADGET(sha1c)
UML_DECLARE_FIXED_GADGET(sha1p)
UML_DECLARE_FIXED_GADGET(sha1m)
UML_DECLARE_FIXED_GADGET(sha1su0)
UML_DECLARE_FIXED_GADGET(sha256h)
UML_DECLARE_FIXED_GADGET(sha256h2)
UML_DECLARE_FIXED_GADGET(sha256su1)
UML_DECLARE_FIXED_GADGET(sha1h)
UML_DECLARE_FIXED_GADGET(sha1su1)
UML_DECLARE_FIXED_GADGET(sha256su0)
UML_DECLARE_FIXED_GADGET(sha512h)
UML_DECLARE_FIXED_GADGET(sha512h2)
UML_DECLARE_FIXED_GADGET(sha512su1)
UML_DECLARE_FIXED_GADGET(sha512su0)
#undef UML_DECLARE_FIXED_GADGET
}

namespace test = arm64_simd_fixed_transform_test;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using uml::emulator::tests::Gadget;

constexpr std::array<Gadget, 7> native32_three{
    uml_arm64_gadget_simd_sha1c,     uml_arm64_gadget_simd_sha1p,
    uml_arm64_gadget_simd_sha1m,     uml_arm64_gadget_simd_sha1su0,
    uml_arm64_gadget_simd_sha256h,   uml_arm64_gadget_simd_sha256h2,
    uml_arm64_gadget_simd_sha256su1,
};
constexpr std::array<Gadget, 7> software32_three{
    uml_arm64_gadget_simd_sha1c_soft,     uml_arm64_gadget_simd_sha1p_soft,
    uml_arm64_gadget_simd_sha1m_soft,     uml_arm64_gadget_simd_sha1su0_soft,
    uml_arm64_gadget_simd_sha256h_soft,   uml_arm64_gadget_simd_sha256h2_soft,
    uml_arm64_gadget_simd_sha256su1_soft,
};
constexpr std::array<Gadget, 3> native32_two{
    uml_arm64_gadget_simd_sha1h,
    uml_arm64_gadget_simd_sha1su1,
    uml_arm64_gadget_simd_sha256su0,
};
constexpr std::array<Gadget, 3> software32_two{
    uml_arm64_gadget_simd_sha1h_soft,
    uml_arm64_gadget_simd_sha1su1_soft,
    uml_arm64_gadget_simd_sha256su0_soft,
};
constexpr std::array<Gadget, 3> native64_three{
    uml_arm64_gadget_simd_sha512h,
    uml_arm64_gadget_simd_sha512h2,
    uml_arm64_gadget_simd_sha512su1,
};
constexpr std::array<Gadget, 3> software64_three{
    uml_arm64_gadget_simd_sha512h_soft,
    uml_arm64_gadget_simd_sha512h2_soft,
    uml_arm64_gadget_simd_sha512su1_soft,
};
using uml::emulator::tests::address;
using uml::emulator::tests::vector_offset;

[[nodiscard]] gadgets::BuildResult build(std::uint32_t instruction,
                                         bool force_software) {
  gadgets::BuildOptions options{};
  options.force_software_fixed_transforms = force_software;
  return gadgets::Builder{}.build(
      test::engine::engine_test_code_base, std::array{instruction}, {},
      std::pmr::get_default_resource(), {}, options);
}

template <std::size_t count>
void check_stream(std::uint32_t instruction, Gadget native, Gadget software,
                  bool host_native,
                  const std::array<unsigned, count> &registers,
                  std::set<std::uintptr_t> &selected,
                  std::set<std::uintptr_t> &portable) {
  std::array<std::uintptr_t, 4> expected_offsets{};
  for (unsigned index = 0; index < count; ++index)
    expected_offsets[index] = vector_offset(registers[index]);
  for (const bool forced : {false, true}) {
    const auto built = build(instruction, forced);
    const auto words = built.program.words();
    test::require(built.complete && words.size() == 4U,
                  "fixed transform stream shape is not exact");
    const auto expected_gadget = !forced && host_native ? native : software;
    test::require(words[0] == address(expected_gadget),
                  "fixed transform host path remained at runtime");
    test::require(words[1] == gadgets::selection::packed_u16_operands(
                                  expected_offsets[0], expected_offsets[1],
                                  expected_offsets[2], expected_offsets[3]),
                  "fixed transform offsets were not packed");
    (forced ? portable : selected).insert(words[0]);
  }
}

void translation_selects_every_operation_and_control(
    std::set<std::uintptr_t> &selected, std::set<std::uintptr_t> &portable) {
  constexpr test::Registers3 three{31, 30, 29};
  constexpr test::Registers2 two{31, 30};
  const bool first_native = test::engine::host_supports(test::feature1());
  const bool second_native = test::engine::host_supports(test::feature2());
  const bool wide_native = test::engine::host_supports(test::feature64());

  for (unsigned operation = 0; operation < 7U; ++operation)
    check_stream(test::transform32_three(operation, three),
                 native32_three[operation], software32_three[operation],
                 operation <= 3U ? first_native : second_native,
                 std::array{three.destination, three.first, three.second},
                 selected, portable);
  for (unsigned operation = 0; operation < 3U; ++operation)
    check_stream(test::transform32_two(operation, two), native32_two[operation],
                 software32_two[operation],
                 operation <= 1U ? first_native : second_native,
                 std::array{two.destination, two.source}, selected, portable);
  for (unsigned operation = 0; operation < 3U; ++operation)
    check_stream(test::transform64_three(operation, three),
                 native64_three[operation], software64_three[operation],
                 wide_native,
                 std::array{three.destination, three.first, three.second},
                 selected, portable);
  check_stream(test::transform64_two(two), uml_arm64_gadget_simd_sha512su0,
               uml_arm64_gadget_simd_sha512su0_soft, wide_native,
               std::array{two.destination, two.source}, selected, portable);

  test::require(selected.size() == 14U,
                "fixed transform current-host selection is incomplete");
  test::require(portable.size() == 14U,
                "fixed transform portable selection is incomplete");
}

void baseline_regression_stream_consumes_every_operand() {
  constexpr std::uint32_t instruction = 0x5e02'0020U;
  const auto built = build(instruction, false);
  const auto words = built.program.words();
  test::require(
      built.complete && words.size() == 4U,
      "baseline regression stream has mismatched producer/consumer shape");
  test::require(words[1] == gadgets::selection::packed_u16_operands(
                                  vector_offset(0), vector_offset(1),
                                  vector_offset(2)),
                "baseline regression offsets were not packed");
}

struct GadgetShape final {
  unsigned instructions;
  unsigned conditional_branches;
};

[[nodiscard]] GadgetShape shape(Gadget gadget) {
  const auto body = a64::body_shape(reinterpret_cast<std::uintptr_t>(gadget),
                                    {.window = 192U});
  if (!body.reached_dispatch)
    test::fail("fixed transform gadget did not reach threaded dispatch");
  return {body.instructions + 1U, body.conditional_branches};
}

void every_definition_is_unique_branch_free_and_bounded() {
  std::set<std::uintptr_t> definitions;
  unsigned maximum_native = 0;
  unsigned maximum_helper_software = 0;
  const auto inspect = [&](Gadget gadget, unsigned &maximum) {
    const auto properties = shape(gadget);
    test::require(properties.conditional_branches == 0U,
                  "fixed transform gadget contains conditional branch");
    maximum = std::max(maximum, properties.instructions);
    definitions.insert(address(gadget));
  };
  for (unsigned operation = 0; operation < native32_three.size(); ++operation) {
    inspect(native32_three[operation], maximum_native);
    inspect(software32_three[operation], maximum_helper_software);
  }
  for (unsigned operation = 0; operation < native32_two.size(); ++operation) {
    inspect(native32_two[operation], maximum_native);
    inspect(software32_two[operation], maximum_helper_software);
  }
  for (unsigned operation = 0; operation < native64_three.size(); ++operation) {
    inspect(native64_three[operation], maximum_native);
    inspect(software64_three[operation], maximum_helper_software);
  }
  inspect(uml_arm64_gadget_simd_sha512su0, maximum_native);
  inspect(uml_arm64_gadget_simd_sha512su0_soft, maximum_helper_software);

  test::require(
      definitions.size() == 28U,
      "fixed transform gadget definition is duplicate or unreachable");
  test::require(maximum_native <= 12U,
                "native fixed transform gadget exceeds instruction budget");
  test::require(maximum_helper_software <= 13U,
                "portable helper gadget exceeds instruction budget");
}

} // namespace

int main() {
  std::set<std::uintptr_t> selected;
  std::set<std::uintptr_t> portable;
  translation_selects_every_operation_and_control(selected, portable);
  baseline_regression_stream_consumes_every_operand();
  every_definition_is_unique_branch_free_and_bounded();
  return 0;
}
