// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/aarch32/guest_key.hpp"
#include "jit_cache_geometry.hpp"
#include "jit_runtime_contract.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace uml::cpu::arm64 {

/* Translation stops at the first control-flow instruction. An unconditional
 * or indirect exit owns one executor pointer; a conditional exit is the sole
 * two-pointer case because it lowers both taken and fallthrough successors.
 */
inline constexpr std::size_t maximum_shared_translation_relocations = 2;

/*
 * Translations shared between executors.
 *
 * Every executor owns a private block cache, so without this tier guest CPUs
 * running the same guest code would each decode and emit it (G134).
 *
 * An entry is keyed by the guest PC and the exact guest instruction words it
 * was translated from, so it is a pure function of its input and can never go
 * stale. That deliberately decouples this cache from the generation and
 * code_changes invalidation the per-executor block cache uses: if the guest
 * bytes at a PC are what they were, the translation of them is still correct,
 * and if they are not, the key does not match.
 *
 * The host code still belongs to the executor that runs it. Canonical words
 * retain zero in every executor-owned pointer cell, and relocations contain
 * only neutral guest keys. A reusing executor copies that template into its
 * own arena, resolves its private direct-link records, and resolves indirect
 * sites against its own tables. No executable memory, activation state, link
 * record or arena is shared, and the compile path gains no lock that execution
 * can see. What is shared is the decode and emit work.
 */
struct SharedTranslation final {
  std::vector<std::uint32_t> words;
  std::vector<JitRelocation> relocations;
  /* Either nonzero count identifies correct executor-private code that fell
   * back to C++ because its compiling executor could not allocate every
   * requested stable link record. The shared cache rejects it without
   * occupying the key so a later full translation can become canonical.
   */
  std::size_t direct_link_refusals{};
  std::size_t indirect_link_refusals{};
  /* Shared separately from the host code so a private block-cache entry can
   * retain exactly the immutable prefix the compiler consumed. Bytes fetched
   * after that prefix cannot affect this translation and are not part of its
   * identity.
   *
   * The identity is guest_word_count units of the translation's ISA: words
   * for A64 and A32, where it equals guest_instructions, and halfwords for
   * T32, where an instruction takes one or two. The bytes lie at the front
   * of guest_words, which holds them in whole 32-bit elements, and the
   * spans find(), preflight_publication() and insert() take hold exactly
   * that many elements, any padding in the last one beyond the identity.
   */
  std::shared_ptr<const std::uint32_t[]> guest_words;
  std::size_t guest_word_count{};
  std::size_t guest_instructions{};
  /* The aarch32::GuestIsa of the key the translation is stored under
   * (insert() checks it), which gives the unit guest_word_count counts. */
  std::uint8_t isa{};
  std::size_t hot_words{};
  std::size_t internal_entry_word{};
  std::uint64_t memory_write_mask{};
  std::uint64_t direct_memory_write_mask{};
  bool accesses_fp_state{};
  bool link_target_safe{};
  bool native_fp{};
  bool guest_illegal{};
};

struct SharedTranslationStatistics final {
  /* A match means find() identified a canonical translation for the fetched
   * guest prefix. Reuse is narrower: the requesting executor resolved every
   * private link pointer, copied the code into its arena, and installed the
   * resulting block. Keeping the counters separate prevents capacity misses
   * after a successful lookup from being reported as saved compilation.
   */
  std::uint64_t matches{};
  std::uint64_t reuses{};
  std::uint64_t misses{};
  std::uint64_t insertions{};
  std::uint64_t evictions{};
  std::uint64_t rejected{};
  std::uint64_t degraded_rejected{};
  std::uint64_t malformed_rejected{};
  /* A failed insertion can conservatively retain its complete budget charge
   * after a new map node enlarged the bucket array and was then erased. These
   * are accounted reservation bytes, not a claim about measured heap usage.
   */
  std::uint64_t insertion_failures{};
  std::uint64_t failed_insertion_retained_accounted_bytes{};
  std::uint64_t materialization_invalid_metadata_failures{};
  std::uint64_t materialization_direct_link_failures{};
  std::uint64_t materialization_indirect_link_failures{};
  std::uint64_t materialization_arena_failures{};
  std::uint64_t entries{};
  std::uint64_t payload_bytes{};
  std::uint64_t accounted_bytes{};
  std::uint64_t maximum_accounted_bytes{};

  [[nodiscard]] bool
  operator==(const SharedTranslationStatistics &) const noexcept = default;
};

struct SharedTranslationPublicationSizes final {
  std::size_t host_words{};
  std::size_t relocations{};
  std::size_t guest_words{};
};

struct SharedTranslationPublicationPreflight final {
  /* A canonical owner may race the caller's local compilation. Reuse it
   * before consulting the capacity bounds, matching insert()'s ordering. */
  std::shared_ptr<const SharedTranslation> canonical;
  /* False with no canonical means publication is gated or definitely cannot
   * fit. The caller must retain its private validation words. */
  bool may_construct{};
};

enum class SharedTranslationMaterializationFailure : std::uint8_t {
  invalid_metadata,
  direct_link_capacity,
  indirect_link_capacity,
  arena_exhaustion,
};

/* Validates every field consumed while installing shared code. requested is
 * the currently fetched guest span and may contain an unconsumed tail; the
 * translation must own and exactly describe a nonempty prefix of it.
 */
[[nodiscard]] bool valid_shared_translation(
    const SharedTranslation &translation,
    std::span<const std::uint32_t> requested) noexcept;

class SharedTranslationCache final {
public:
  /* maximum_accounted_bytes bounds storage retained by this cache, including
   * its fixed shards, map/index overhead, and admitted translation ownership.
   * A caller's not-yet-admitted candidate remains transient caller storage.
   */
  explicit SharedTranslationCache(
      std::size_t shard_capacity = unlimited_shard_entries,
      std::size_t maximum_accounted_bytes =
          default_jit_cache_budget_per_executor * 4U) noexcept;
  ~SharedTranslationCache();

  SharedTranslationCache(const SharedTranslationCache &) = delete;
  SharedTranslationCache &operator=(const SharedTranslationCache &) = delete;

  /* Codegen options that change emitted code. Two executors may only share a
   * translation when these agree, so they are part of the identity of the
   * cache rather than of each entry.
   */
  struct CodegenProfile final {
    ::uml::cpu::HostCapabilities capabilities{};
    /* The mode the translation was compiled for: the guest SVCR mode and
     * whether data addresses ignore their top byte (translation_key()). */
    std::uint8_t translation_mode{};

    /* Compared field by field rather than by bytes so padding cannot make
     * two identical configurations look different.
     */
    [[nodiscard]] bool operator==(const CodegenProfile &other) const noexcept;
  };

  [[nodiscard]] std::shared_ptr<const SharedTranslation>
  find(const CodegenProfile &profile, std::uint64_t guest_pc,
       std::span<const std::uint32_t> guest_words) const;

  /* Nonallocating advisory check for the production publisher. It first
   * returns an exact canonical owner that raced local compilation, including
   * after the publishing peer retires. Otherwise a new admission requires two
   * live executors, then records exactly one degraded rejection when either
   * refusal count is nonzero, or one capacity rejection when the shard is
   * already full or an overflow-safe lower-bound charge for the supplied
   * payload sizes cannot fit after a bounded sweep of unused entries.
   * insert() remains the authority for capacity consumed after this check
   * and for actual vector capacities.
   */
  [[nodiscard]] SharedTranslationPublicationPreflight preflight_publication(
      const CodegenProfile &profile, std::uint64_t guest_pc,
      std::span<const std::uint32_t> guest_words,
      SharedTranslationPublicationSizes sizes,
      std::size_t direct_link_refusals,
      std::size_t indirect_link_refusals);

  /* Returns the canonical admitted entry, which may have been inserted by a
   * racing executor or outlive its publishing peer. A null result means new
   * publication is gated off or that the shard/byte capacity is full; the
   * caller must retain private validation words.
   */
  [[nodiscard]] std::shared_ptr<const SharedTranslation>
  insert(const CodegenProfile &profile, std::uint64_t guest_pc,
         std::span<const std::uint32_t> guest_words,
         std::shared_ptr<const SharedTranslation> translation);

  [[nodiscard]] bool accepts_publications() const noexcept;

  /* Returns true after recording exactly one refusal when a compiler result
   * lost one or more requested stable links. Publication preflight calls this
   * after its exact canonical lookup; insert() repeats the guard for direct API
   * callers and tests.
   */
  [[nodiscard]] bool
  refuse_degraded_publication(std::size_t direct_link_refusals,
                              std::size_t indirect_link_refusals) noexcept;

  /* Called only after an executor has materialized and installed a matched
   * translation, or at the exact boundary where materialization failed.
   */
  void record_reuse() noexcept;
  void record_materialization_failure(
      SharedTranslationMaterializationFailure failure) noexcept;

  /* Sharing only pays when executors run at the same time. A process with a
   * single executor would otherwise copy and store every translation it
   * compiles for a reader that never arrives, which for a workload of
   * mostly-unique blocks is pure cost. Lookups stay enabled unconditionally
   * because an empty cache makes them a hash probe.
   */
  void add_executor() noexcept;
  void remove_executor() noexcept;

  [[nodiscard]] SharedTranslationStatistics statistics() const noexcept;
  [[nodiscard]] std::size_t allocated_shards() const noexcept;

  [[nodiscard]] static SharedTranslationCache &instance() noexcept;

private:
  struct Entry final {
    CodegenProfile profile;
    std::shared_ptr<const SharedTranslation> translation;
  };

  static constexpr std::size_t unlimited_shard_entries =
      std::numeric_limits<std::size_t>::max();
  /* One mutex for every executor's misses and insertions serialised them into
   * a convoy that cost more than the compilation it saved. Shard by guest PC
   * so ten executors rarely contend, and pad each shard so their locks do not
   * share a cache line.
   */
  static constexpr std::size_t shard_count = 64;
  struct alignas(64) Shard final {
    mutable std::shared_mutex mutex;
    std::unordered_map<std::uint64_t, std::vector<Entry>> entries;
    std::size_t count{};
    std::size_t index_credit{};
    std::uint64_t next_victim{};
    bool victim_initialized{};
  };

  struct alignas(64) ShardSlot final {
    std::atomic<Shard *> shard{};
    mutable std::mutex creation_mutex;
  };

  [[nodiscard]] static std::size_t shard_of(std::uint64_t guest_pc) noexcept {
    auto hash = guest_pc * 0x9e3779b97f4a7c15ULL;
    hash ^= hash >> 29U;
    return static_cast<std::size_t>(hash) & (shard_count - 1U);
  }

  [[nodiscard]] Shard *allocated_shard(std::uint64_t guest_pc) const noexcept;
  [[nodiscard]] Shard &ensure_shard(std::uint64_t guest_pc);

  [[nodiscard]] std::size_t entry_budget_charge(
      const SharedTranslation &translation) const noexcept;
  [[nodiscard]] static std::size_t
  lower_bound_entry_budget_charge(
      SharedTranslationPublicationSizes sizes) noexcept;
  [[nodiscard]] static std::size_t
  entry_budget_charge_from_payload(std::size_t payload_bytes) noexcept;
  [[nodiscard]] static std::size_t entry_index_budget_charge() noexcept;
  [[nodiscard]] static std::size_t additional_budget_charge(
      const Shard &shard, std::size_t charge) noexcept;
  void reclaim_unused(Shard &shard, std::size_t charge) noexcept;
  [[nodiscard]] bool budget_can_fit(std::size_t bytes) const noexcept;
  [[nodiscard]] bool reserve_budget(std::size_t bytes) noexcept;
  void release_budget(std::size_t bytes) noexcept;

  /* The directory is fixed and tiny, but each mutex/map/vector backing is
   * created only when a publication actually lands in that shard. Empty and
   * lookup-only runs therefore never allocate shared-cache backing.
   */
  std::array<ShardSlot, shard_count> shards_{};
  /* Counted outside the lock, so readers holding only a shared lock do not
   * race on them.
   */
  mutable std::atomic<std::uint64_t> matches_{};
  std::atomic<std::uint64_t> reuses_{};
  mutable std::atomic<std::uint64_t> misses_{};
  std::atomic<std::uint64_t> insertions_{};
  std::atomic<std::uint64_t> evictions_{};
  std::atomic<std::uint64_t> rejected_{};
  std::atomic<std::uint64_t> degraded_rejected_{};
  std::atomic<std::uint64_t> malformed_rejected_{};
  std::atomic<std::uint64_t> insertion_failures_{};
  std::atomic<std::uint64_t>
      failed_insertion_retained_accounted_bytes_{};
  std::atomic<std::uint64_t> materialization_invalid_metadata_failures_{};
  std::atomic<std::uint64_t> materialization_direct_link_failures_{};
  std::atomic<std::uint64_t> materialization_indirect_link_failures_{};
  std::atomic<std::uint64_t> materialization_arena_failures_{};
  std::atomic<std::uint64_t> payload_bytes_{};
  std::atomic<std::size_t> accounted_bytes_{};
  std::atomic<unsigned> live_executors_{};
  std::size_t shard_capacity_;
  std::size_t maximum_accounted_bytes_;
};

} // namespace uml::cpu::arm64
