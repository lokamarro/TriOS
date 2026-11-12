// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"
#include "arm64_gadget_test_support.hpp"
#include "require.hpp"

#include "cpu/arm64/gadgets/gadget_operand_encoding.hpp"
#include "cpu/arm64/gadgets/program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace arm64_simd_bitwise_test {

enum class Operation : unsigned {
  bit_and,
  bit_clear,
  bit_or,
  or_not,
  bit_xor,
  select,
  insert_true,
  insert_false,
};

struct Registers final {
  unsigned destination;
  unsigned left;
  unsigned right;
};

[[nodiscard]] constexpr std::uint32_t
instruction(bool q, Operation operation, Registers registers) noexcept {
  const auto encoded = static_cast<unsigned>(operation);
  return 0x0e20'1c00U | (q ? 0x4000'0000U : 0U) |
         ((encoded & 3U) << 22) | ((encoded & 4U) << 27) |
         ((registers.right & 31U) << 16) |
         ((registers.left & 31U) << 5) | (registers.destination & 31U);
}

} // namespace arm64_simd_bitwise_test

extern "C" {
void uml_arm64_gadget_nop();
void uml_arm64_gadget_simd_and_8b();
void uml_arm64_gadget_simd_and_16b();
void uml_arm64_gadget_simd_bic_8b();
void uml_arm64_gadget_simd_bic_16b();
void uml_arm64_gadget_simd_orr_8b();
void uml_arm64_gadget_simd_orr_16b();
void uml_arm64_gadget_simd_orn_8b();
void uml_arm64_gadget_simd_orn_16b();
void uml_arm64_gadget_simd_eor_8b();
void uml_arm64_gadget_simd_eor_16b();
void uml_arm64_gadget_simd_bsl_8b();
void uml_arm64_gadget_simd_bsl_16b();
void uml_arm64_gadget_simd_bit_8b();
void uml_arm64_gadget_simd_bit_16b();
void uml_arm64_gadget_simd_bif_8b();
void uml_arm64_gadget_simd_bif_16b();
void uml_arm64_gadget_simd_mov_8b();
void uml_arm64_gadget_simd_mov_16b();
}

namespace bitwise = arm64_simd_bitwise_test;
namespace engine_tests = uml::emulator::tests;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using uml::emulator::tests::Gadget;

constexpr std::array<std::array<Gadget, 2>, 8> expected_gadgets{{
    {uml_arm64_gadget_simd_and_8b, uml_arm64_gadget_simd_and_16b},
    {uml_arm64_gadget_simd_bic_8b, uml_arm64_gadget_simd_bic_16b},
    {uml_arm64_gadget_simd_orr_8b, uml_arm64_gadget_simd_orr_16b},
    {uml_arm64_gadget_simd_orn_8b, uml_arm64_gadget_simd_orn_16b},
    {uml_arm64_gadget_simd_eor_8b, uml_arm64_gadget_simd_eor_16b},
    {uml_arm64_gadget_simd_bsl_8b, uml_arm64_gadget_simd_bsl_16b},
    {uml_arm64_gadget_simd_bit_8b, uml_arm64_gadget_simd_bit_16b},
    {uml_arm64_gadget_simd_bif_8b, uml_arm64_gadget_simd_bif_16b},
}};

using uml::testing::require;
using uml::emulator::tests::address;
using uml::emulator::tests::vector_offset;

void encoding_examples() {
  require(bitwise::instruction(true, bitwise::Operation::bit_and, {0, 1, 2}) ==
              0x4e22'1c20U,
          "AND vector encoding mismatch");
  require(
      bitwise::instruction(false, bitwise::Operation::bit_clear, {3, 4, 5}) ==
          0x0e65'1c83U,
      "BIC vector encoding mismatch");
  require(bitwise::instruction(true, bitwise::Operation::insert_true,
                               {18, 19, 20}) == 0x6eb4'1e72U,
          "BIT vector encoding mismatch");
}

void production_specialization() {
  constexpr auto base = engine_tests::engine_test_code_base;
  constexpr bitwise::Registers registers{7, 11, 19};
  for (unsigned operation = 0; operation < 8; ++operation)
    for (const bool q : {false, true}) {
      const auto built = gadgets::Builder{}.build(
          base, std::array{bitwise::instruction(
                    q, static_cast<bitwise::Operation>(operation), registers)});
      require(built.complete && built.program.words().size() == 4U,
              "ordinary bitwise instruction has an unexpected stream shape");
      require(built.program.words()[0] ==
                  address(expected_gadgets[operation][q ? 1U : 0U]),
              "operation or arrangement was not specialized in translation");
      require(built.program.words()[1] ==
                  gadgets::selection::packed_u16_operands(
                      vector_offset(registers.destination),
                      vector_offset(registers.left),
                      vector_offset(registers.right)),
              "register offsets were not packed during translation");
    }

  for (const bool q : {false, true}) {
    const auto moved = gadgets::Builder{}.build(
        base, std::array{bitwise::instruction(
                  q, bitwise::Operation::bit_or, {5, 9, 9})});
    const auto expected =
        q ? uml_arm64_gadget_simd_mov_16b : uml_arm64_gadget_simd_mov_8b;
    require(moved.complete && moved.program.words().size() == 4U,
            "MOV alias has an unexpected stream shape");
    require(moved.program.words()[0] == address(expected),
            "MOV alias retained the generic two-source operation");
    require(moved.program.words()[1] ==
                gadgets::selection::packed_u16_operands(
                    vector_offset(5), vector_offset(9)),
            "MOV alias offsets were not packed during translation");
  }

  const auto ordinary_nop =
      gadgets::Builder{}.build(base, std::array{0xd503'201fU});
  const auto self_move = gadgets::Builder{}.build(
      base, std::array{bitwise::instruction(
                true, bitwise::Operation::bit_or, {9, 9, 9})});
  require(ordinary_nop.complete && self_move.complete &&
              ordinary_nop.program.words().front() ==
                  address(uml_arm64_gadget_nop) &&
              self_move.program.words().size() == 3U &&
              self_move.program.words().front() ==
                  ordinary_nop.program.words().front(),
          "full-width self MOV was not folded to NOP");

  const auto narrow_self_move = gadgets::Builder{}.build(
      base, std::array{bitwise::instruction(
                false, bitwise::Operation::bit_or, {9, 9, 9})});
  require(narrow_self_move.complete &&
              narrow_self_move.program.words().size() == 4U &&
              narrow_self_move.program.words().front() ==
                  address(uml_arm64_gadget_simd_mov_8b) &&
              narrow_self_move.program.words()[1] ==
                  gadgets::selection::packed_u16_operands(
                      vector_offset(9), vector_offset(9)),
          "narrow self MOV lost its required upper-half clear");
}

} // namespace

int main() {
  encoding_examples();
  production_specialization();
  return 0;
}
