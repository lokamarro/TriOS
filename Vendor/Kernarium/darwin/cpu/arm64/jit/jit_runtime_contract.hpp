// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/detail/instruction_primitives.hpp"
#include "cpu/engine.hpp"
#include "uml_darwin_static_mmu.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace uml::cpu::arm64 {

using Event = ::uml::cpu::EngineEvent;

inline constexpr std::size_t maximum_block_instructions = 64;
/* The block cache, link tables, and arena are one coupled resource sized from
 * the process budget. Arena exhaustion clears the block cache. See
 * jit_cache_geometry.hpp.
 */
inline constexpr std::size_t cache_ways = 4;
inline constexpr std::size_t single_instruction_arena_size = 16U * 1024U;
inline constexpr std::uint64_t page_size = DARWIN_UML_GUEST_PAGE_SIZE;
inline constexpr unsigned page_shift = DARWIN_UML_GUEST_PAGE_SHIFT;
inline constexpr std::size_t maximum_chain_instructions = 4096;

/* The mode a translation is compiled for: the guest SVCR mode (svcr_sm and
 * svcr_za) in the two low bits, and translation_mode_tbi when the guest's
 * data addresses ignore their top byte (CpuState::tbi_enabled), so the code
 * strips it inline instead of testing the flag at every access.
 */
inline constexpr std::uint8_t translation_mode_tbi = 4;
static_assert((translation_mode_tbi & svcr_mask) == 0);

/* A translation's key: its guest PC with its mode (above). The SVCR mode
 * is in the two low bits, which every PC that reaches a translation leaves
 * clear; the top-byte-ignore bit is aarch32::a64_tbi_key_bit, which no
 * user PC reaches. The block cache, the shared cache, every link record and
 * every indirect site use the key, so code compiled for one mode only ever
 * runs, links or chains in that mode. A mode-0 key is its PC.
 */
[[nodiscard]] constexpr std::uint64_t
translation_key(std::uint64_t pc, std::uint8_t mode) noexcept {
  return pc | (mode & svcr_mask) |
         ((mode & translation_mode_tbi) != 0U ? aarch32::a64_tbi_key_bit
                                              : std::uint64_t{0});
}

[[nodiscard]] constexpr std::uint64_t
translation_key_pc(std::uint64_t key) noexcept {
  return key & ~(std::uint64_t{svcr_mask} | aarch32::a64_tbi_key_bit);
}

[[nodiscard]] constexpr std::uint8_t
translation_key_mode(std::uint64_t key) noexcept {
  return static_cast<std::uint8_t>(
      (key & svcr_mask) |
      ((key & aarch32::a64_tbi_key_bit) != 0U ? translation_mode_tbi : 0U));
}

static_assert(translation_key(0x4000'1000U, 0) == 0x4000'1000U);
static_assert(translation_key_pc(translation_key(0x4000'1000U, 3)) ==
              0x4000'1000U);
static_assert(translation_key_mode(translation_key(0x4000'1000U, 2)) == 2);
static_assert(translation_key(0x4000'1000U, translation_mode_tbi) ==
              (aarch32::a64_tbi_key_bit | 0x4000'1000U));
static_assert(translation_key_pc(translation_key(
                  0x4000'1000U, translation_mode_tbi | 3U)) == 0x4000'1000U);
static_assert(translation_key_mode(translation_key(
                  0x4000'1000U, translation_mode_tbi | 1U)) ==
              (translation_mode_tbi | 1U));
static_assert(!aarch32::key_is_aarch32(
    translation_key(0x4000'1000U, translation_mode_tbi)));

[[nodiscard]] constexpr bool
floating_or_vector_primary_class(std::uint32_t instruction) noexcept {
  const auto op0 = (instruction >> 25U) & 15U;
  return op0 == 7U || op0 == 15U;
}

[[nodiscard]] constexpr bool
obvious_translated_block_terminator(std::uint32_t instruction) noexcept {
  return (instruction & 0x7c00'0000U) == 0x1400'0000U ||
         (instruction & 0xff00'0000U) == 0x5400'0000U ||
         (instruction & 0x7e00'0000U) == 0x3400'0000U ||
         (instruction & 0x7e00'0000U) == 0x3600'0000U ||
         (instruction & 0xfe00'0000U) == 0xd600'0000U ||
         (instruction & 0xff00'0000U) == 0xd400'0000U;
}

static_assert(floating_or_vector_primary_class(0x6f40'0000U));
static_assert(floating_or_vector_primary_class(0x4f00'1000U));
static_assert(!floating_or_vector_primary_class(0x8b02'0020U));
static_assert(!floating_or_vector_primary_class(0xf940'0000U));
static_assert(!floating_or_vector_primary_class(0x5400'0000U));
static_assert(!floating_or_vector_primary_class(0xd400'0001U));
static_assert(obvious_translated_block_terminator(0x1400'0000U));
static_assert(obvious_translated_block_terminator(0x9400'0000U));
static_assert(obvious_translated_block_terminator(0x5400'0000U));
static_assert(obvious_translated_block_terminator(0xb400'0000U));
static_assert(obvious_translated_block_terminator(0x3600'0000U));
static_assert(obvious_translated_block_terminator(0xd61f'0000U));
static_assert(obvious_translated_block_terminator(0xd63f'0000U));
static_assert(obvious_translated_block_terminator(0xd65f'0000U));
static_assert(obvious_translated_block_terminator(0xd400'0001U));
static_assert(!obvious_translated_block_terminator(0xd503'201fU));
static_assert(!obvious_translated_block_terminator(0x6f40'0000U));

struct JitBarrierLowering final {
  std::array<std::uint32_t, 2> words{};
  std::size_t count{};
};

[[nodiscard]] constexpr JitBarrierLowering
lower_memory_barrier(std::uint32_t instruction, bool native_sb) noexcept {
  constexpr std::uint32_t dsb_base = 0xd503'309fU;
  constexpr std::uint32_t isb_sy = 0xd503'3fdfU;
  constexpr std::uint32_t sb = 0xd503'30ffU;

  if (instruction == sb) {
    if (native_sb)
      return {{{instruction, 0}}, 1};
    return {{{dsb_base | (15U << 8U), isb_sy}}, 2};
  }
  return {{{instruction, 0}}, 1};
}

/* Callers pass the instruction field already masked to `bits`. */
using detail::sign_extend;

[[nodiscard]] constexpr std::uint64_t
pack_jit_result(Event event, std::size_t retired) noexcept {
  return static_cast<std::uint32_t>(event) |
         (static_cast<std::uint64_t>(retired) << 32U);
}

struct JitResult final {
  Event event;
  std::size_t retired;
};

[[nodiscard]] constexpr JitResult
unpack_jit_result(std::uint64_t result) noexcept {
  return {static_cast<Event>(static_cast<std::int32_t>(result & 0xffff'ffffU)),
          static_cast<std::size_t>(result >> 32U)};
}

/*
 * A translation has two executor-owned link operands: a direct successor key
 * whose mutable descriptor is regenerated beside the installed code, and an
 * indirect-link site. Canonical shared words zero both pointer cells and retain
 * only their neutral guest keys. Recording the cells lets every executor
 * recreate its private link state while reusing decode and emission work.
 */
enum class JitRelocationKind : std::uint8_t {
  direct_link,
  indirect_link,
};

struct JitRelocation final {
  /* Index of the low half of the 64-bit literal pool slot. */
  std::size_t word{};
  /* Translation key (translation_key()) the pointer is resolved from: the
   * branch target for a direct link, the branch source for an indirect one.
   */
  std::uint64_t key{};
  JitRelocationKind kind{};
};

struct JitStoreRange final {
  /* The tracked-only program records the translated backing address so
   * distinct guest virtual aliases share one reservation identity.
   */
  std::uint64_t address;
  std::uint64_t size;
};

struct JitChainState final {
  /*
   * Instructions still available before the current translated block. The
   * persistent ABI keeps the same value in x20 and publishes it on return.
   */
  std::uint64_t remaining_instructions{maximum_chain_instructions};
  JitStoreRange *stores{};
  void *indirect_miss{};
  std::uint8_t link_selector{};
  /*
   * A helper-sensitive cold path cannot run while the outer direct-store
   * publication is alive. Generated code records the translated exclusive
   * instruction here and returns without retiring it; run_block executes that
   * exact word after the publication scope has ended. Zero (UDF #0) is never
   * deferred and means that no instruction is pending.
   */
  std::uint32_t deferred_exclusive_instruction{};
  /* The translation key of the indirect site that missed. */
  std::uint64_t indirect_source_pc{};
  /* The executor's table of trained indirect targets (JitIndirectTarget),
   * for the AArch32 translations, whose indirect branches look there after
   * their site's cells; A64 code does not read it. */
  const void *indirect_targets{};
  /* The link epoch of the translation context the chain runs in, shifted to
   * the bits above an AArch32 key (jit_indirect_target_tag()): a table entry
   * trained under another context or another address space names a
   * translation of other guest memory and must not match. */
  std::uint64_t indirect_target_tag{};
};

static_assert(offsetof(JitChainState, remaining_instructions) == 0);
static_assert(offsetof(JitChainState, stores) == 8);
static_assert(offsetof(JitChainState, indirect_miss) == 16);
static_assert(offsetof(JitChainState, link_selector) == 24);
static_assert(offsetof(JitChainState, deferred_exclusive_instruction) == 28);
static_assert(offsetof(JitChainState, indirect_source_pc) == 32);
static_assert(offsetof(JitChainState, indirect_targets) == 40);
static_assert(offsetof(JitChainState, indirect_target_tag) == 48);
static_assert(sizeof(JitChainState) == 56);
static_assert(sizeof(JitStoreRange) == 16);
static_assert(sizeof(JitChainState::link_selector) == 1);

} // namespace uml::cpu::arm64
