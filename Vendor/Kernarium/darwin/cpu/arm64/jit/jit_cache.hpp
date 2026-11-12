// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "jit_compiler.hpp"
#include "jit_linking.hpp"
#include "jit_runtime_contract.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace uml::cpu::arm64 {

struct CompiledBlock final {
  unsigned long generation{};
  std::uint64_t code_changes{};
  /* translation_key(): the guest PC with the mode (SVCR mode and
   * top-byte ignore) the block was compiled for, which also selects its
   * cache set. */
  std::uint64_t key{};
  JitFunction function{};
  JitFunction tracked_function{};
  std::uintptr_t internal_entry{};
  JitBlockLink *link{};
  std::uint64_t stamp{};
  std::uint64_t link_epoch{};
  std::uint64_t memory_write_mask{};
  std::uint64_t direct_memory_write_mask{};
  std::size_t instruction_count{};
  /* Exact guest bytes used to validate this block after an unrelated global
   * code-sequence change. Shared translations and every executor that
   * materializes one retain the same immutable allocation; a translation that
   * is not admitted to the shared cache aliases its executor's bounded
   * fallback slab.
   */
  std::shared_ptr<const std::uint32_t[]> saved_guest_words;
  bool accesses_fp_state{};
  bool link_target_safe{};
  bool native_fp{};
  bool guest_illegal{};
  /* The guest units saved_guest_words holds, in what padding left: words for
   * A64 and A32, halfwords for T32 (aarch32::isa_unit_bytes of the key's
   * ISA), at most twice maximum_block_instructions. For A64 and A32 it is
   * instruction_count. */
  std::uint8_t saved_units{};
};

struct CacheSet final {
  std::array<CompiledBlock, cache_ways> ways{};
};

/* Sized at construction from the JIT cache budget; see
 * jit_cache_geometry.hpp. cache_index() must be given the same set count.
 */
using BlockCache = std::vector<CacheSet>;
using GuestWordBuffer =
    std::array<std::uint32_t, maximum_block_instructions>;
struct SavedGuestWordSet final {
  std::array<GuestWordBuffer, cache_ways> ways;
};

struct CacheSlot final {
  CompiledBlock *block{};
  GuestWordBuffer *fallback_guest_words{};
};

/* Geometry reserves this worst-case fallback payload for every cached block
 * even though shared translations leave their corresponding slab pages
 * untouched. Keeping the conservative charge makes rejected publications
 * and single-executor fallbacks stay inside the whole-process budget.
 */
inline constexpr std::size_t maximum_saved_guest_word_bytes =
    maximum_block_instructions * sizeof(std::uint32_t);
/* Conservative room for the single allocation and shared-ownership metadata
 * of an executor's fallback slab. The shared_ptr in each CompiledBlock is
 * already part of sizeof(CompiledBlock).
 */
inline constexpr std::size_t saved_guest_word_slab_allocation_allowance = 64U;

[[nodiscard]] constexpr std::size_t
cache_index(unsigned long generation, std::uint64_t pc,
            std::size_t sets) noexcept {
  const auto mixed = pc ^ (pc >> 17U) ^
                     (static_cast<std::uint64_t>(generation) * 0x9e37'79b9U);
  return static_cast<std::size_t>(mixed) & (sets - 1);
}

} // namespace uml::cpu::arm64
