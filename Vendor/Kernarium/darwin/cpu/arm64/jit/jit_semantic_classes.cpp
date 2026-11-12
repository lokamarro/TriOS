// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_semantic_instructions.hpp"

#include <cstdint>

/*
 * Class selection for the semantic slow path. Each predicate names an
 * allocation that the compiler routes to the corresponding helper; the
 * compiler has already resolved register, feature, and reserved-encoding
 * legality before emitting the call. Where two allocations overlap, the more
 * specific one is tested first.
 */

namespace uml::cpu::arm64::jit::semantic {
namespace {

[[nodiscard]] constexpr bool matches(std::uint32_t instruction,
                                     std::uint32_t mask,
                                     std::uint32_t value) noexcept {
  return (instruction & mask) == value;
}

} // namespace

Handler memory_handler(std::uint32_t instruction) noexcept {
  /* LDAPR; LDAPUR and STLUR to general registers. */
  if (matches(instruction, 0x3fff'fc00U, 0x38bf'c000U) ||
      matches(instruction, 0x3f20'0c00U, 0x1900'0000U))
    return load_store_rcpc;

  /* Literal loads, except the non-faulting literal PRFM hint. */
  if (matches(instruction, 0x3b00'0000U, 0x1800'0000U) &&
      !matches(instruction, 0xff00'0000U, 0xd800'0000U))
    return load_literal;
  if (matches(instruction, 0x3b00'0000U, 0x3900'0000U))
    return load_store_unsigned;
  /* Unscaled, post-index, unprivileged, and pre-index forms, and the
   * register-offset form. The other register-offset cells hold atomics,
   * LDAPR, and the undefined LS64 and LDRAA/LDRAB. */
  if (matches(instruction, 0x3b00'0000U, 0x3800'0000U) &&
      ((instruction & 0x0020'0000U) == 0U ||
       ((instruction >> 10) & 3U) == 2U))
    return load_store_other;
  return nullptr;
}

Handler exclusive_handler(std::uint32_t instruction) noexcept {
  /* LSE atomic memory operations. LDAPR and the undefined LS64 and
   * read-check-write cells share the allocation; none of them reaches this
   * helper. */
  if (matches(instruction, 0x3f20'0c00U, 0x3820'0000U) &&
      !matches(instruction, 0x3fff'fc00U, 0x38bf'c000U))
    return load_store_atomic;
  if (matches(instruction, 0xbfa0'0000U, 0x0820'0000U)) // CASP
    return load_store_compare_swap_pair;
  if (matches(instruction, 0x3fa0'0000U, 0x08a0'0000U)) // CAS
    return load_store_compare_swap;
  /* LDXR and STXR families, LDXP and STXP. */
  if (matches(instruction, 0x3fa0'0000U, 0x0800'0000U) ||
      matches(instruction, 0xbfa0'0000U, 0x8820'0000U))
    return load_store_exclusive;
  return nullptr;
}

Handler ordered_store_handler(std::uint32_t instruction) noexcept {
  /* STLR: the non-exclusive, non-pair ordered store cell. */
  if (matches(instruction, 0x3fe0'0000U, 0x0880'0000U))
    return load_store_exclusive;
  return nullptr;
}

} // namespace uml::cpu::arm64::jit::semantic
