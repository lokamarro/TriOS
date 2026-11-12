// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "a32_operations.hpp"

#include "cpu/arm64/aarch32/decode.hpp"

#include <cstdint>

namespace uml::cpu::arm64::jit::a32 {

/* What a memory instruction does, read from its decoded operands once for
 * the compiler (which lowers the common shapes inline) and for the slow path
 * (which executes any of them). */
struct Transfer final {
  enum class Kind : std::uint8_t {
    none,
    /* LDR, LDRB, LDRH, LDRSB, LDRSH and the stores, with the unprivileged
     * forms, which at EL0 are the same accesses, and LDA and STL. */
    single,
    /* LDRD and STRD. */
    pair,
    /* LDM, STM, PUSH and POP. */
    multiple,
    /* TBB and TBH. */
    table,
    /* LDREX, STREX, LDAEX, STLEX and their B, H and D forms. */
    exclusive,
    /* PLD, PLDW and PLI, which never fault. */
    prefetch,
  };
  enum class Mode : std::uint8_t { ia, ib, da, db };

  Kind kind{Kind::none};
  bool load{};
  /* The bytes one element moves: 1, 2 or 4 for a single transfer, 8 for a
   * pair or an exclusive pair. */
  std::uint8_t bytes{};
  bool sign_extend{};
  /* The address is the literal pool's: Align(PC, 4) plus or minus imm. */
  bool literal{};
  bool register_offset{};
  /* LDA, LDAB, LDAH, STL, STLB, STLH: the address must be aligned to the
   * size, and the access is a load-acquire or a store-release. */
  bool ordered{};
  /* A load-exclusive with acquire, a store-exclusive with release. */
  bool acquire{};
  bool release{};
  /* Offset or pre-indexed addressing: the address includes the offset. */
  bool index{true};
  bool add{true};
  bool wback{};
  std::uint8_t t{};
  std::uint8_t t2{};
  std::uint8_t n{};
  std::uint8_t m{};
  /* The STREX status register. */
  std::uint8_t d{};
  aarch32::SRType shift_type{aarch32::SRType::LSL};
  std::uint8_t shift_amount{};
  std::uint32_t imm{};
  std::uint16_t registers{};
  Mode mode{Mode::ia};
  bool halfword_table{};
};

/* The transfer a decoded memory instruction performs; kind is none for any
 * other instruction. */
[[nodiscard]] Transfer describe_transfer(
    const aarch32::Instruction &inst) noexcept;

} // namespace uml::cpu::arm64::jit::a32
