// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_executor_internal.hpp"

#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/aarch32/guest_validation.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <new>
#include <span>

namespace uml::cpu::arm64 {

[[nodiscard]] JitCacheValidationResult
JitExecutor::Implementation::validate_stale_block(
    const CompiledBlock &block, std::uint64_t changes,
    StaticGuestMemory &memory) {
  /* The saved units are the block's guest words, or for T32 its halfwords,
   * read where the key says they lie (aarch32/guest_key.hpp). */
  const auto isa = aarch32::key_isa(block.key);
  const std::size_t unit_limit =
      isa == aarch32::GuestIsa::t32 ? 2 * maximum_block_instructions
                                    : maximum_block_instructions;
  if (block.instruction_count == 0 ||
      block.instruction_count > maximum_block_instructions ||
      block.saved_units == 0 || block.saved_units > unit_limit ||
      block.saved_guest_words == nullptr)
    return JitCacheValidationResult::mismatch;

  /*
   * This runs on every stale hit in production builds, so it fetches
   * page-bounded runs and compares each run once, with the page-splitting
   * loop fetch_guest_units() (aarch32/guest_fetch.hpp) uses for a window.
   *
   * Validation reports mismatch at the first differing word, and fault only
   * for an unreadable word it reaches before any mismatch. A failed bulk fetch
   * cannot tell which of those applies, so a failed run falls back to the
   * word-at-a-time path over that run rather than reporting either outcome
   * for the whole span.
   */
  constexpr std::uint64_t guest_page_size = DARWIN_UML_GUEST_PAGE_SIZE;
  GuestWordBuffer fetched{};
  switch (aarch32::validate_guest_units(
      block.key, block.saved_units,
      reinterpret_cast<const std::byte *>(block.saved_guest_words.get()),
      std::as_writable_bytes(std::span{fetched}), guest_page_size,
      [&memory](std::uint64_t address, std::span<std::byte> bytes) {
        return memory.fetch(address, bytes);
      },
      true)) {
  case aarch32::GuestValidation::match:
    break;
  case aarch32::GuestValidation::mismatch:
    return JitCacheValidationResult::mismatch;
  case aarch32::GuestValidation::fault:
    return JitCacheValidationResult::fault;
  }

  /*
   * The exact-byte proof is useful only for the sequence whose transition
   * caused this lookup. A concurrent code publication after the first
   * sequence snapshot must leave the old link inactive and retry through
   * the normal miss path.
   */
  const auto current =
      darwin_uml_static_vm_execution_context_sequences(execution_context_);
  if (current.execution_interrupt != 0 ||
      current.mapping_changes != mapping_changes_ ||
      current.code_changes != changes)
    return JitCacheValidationResult::sequence_race;
  return JitCacheValidationResult::hit;
}

[[nodiscard]] CompiledBlock *JitExecutor::Implementation::find(unsigned long generation,
                                  std::uint64_t changes, std::uint64_t key,
                                  StaticGuestMemory &memory) {
  const auto set_index = cache_index(generation, key, geometry_.cache_sets);
  auto &set = (*block_cache_)[set_index];
  CompiledBlock *stale{};
  for (std::size_t way = 0; way < set.ways.size(); ++way) {
    auto &block = set.ways[way];
    if (block.function != nullptr && block.generation == generation &&
        block.code_changes == changes && block.key == key) {
      block.stamp = ++stamp_;
      activate_link(block);
      return &block;
    }
    if (block.function != nullptr && block.generation == generation &&
        block.key == key &&
        (stale == nullptr || block.stamp > stale->stamp)) {
      stale = &block;
    }
  }
#if UML_JIT_RUNTIME_DIAGNOSTICS
  if (!reuse_validated_stale_blocks_ || stale == nullptr)
#else
  if (stale == nullptr)
#endif
    return nullptr;

#if UML_JIT_RUNTIME_DIAGNOSTICS
  ++cache_validation_diagnostics_.validation_attempts;
#endif
  switch (validate_stale_block(*stale, changes, memory)) {
  case JitCacheValidationResult::hit:
#if UML_JIT_RUNTIME_DIAGNOSTICS
    ++cache_validation_diagnostics_.validation_hits;
#endif
    stale->code_changes = changes;
    stale->stamp = ++stamp_;
    ++compiled_in_link_sequence_;
    activate_link(*stale);
    return stale;
  case JitCacheValidationResult::mismatch:
#if UML_JIT_RUNTIME_DIAGNOSTICS
    ++cache_validation_diagnostics_.validation_mismatches;
#endif
    *stale = {};
    break;
  case JitCacheValidationResult::fault:
#if UML_JIT_RUNTIME_DIAGNOSTICS
    ++cache_validation_diagnostics_.validation_faults;
#endif
    *stale = {};
    break;
  case JitCacheValidationResult::sequence_race:
#if UML_JIT_RUNTIME_DIAGNOSTICS
    ++cache_validation_diagnostics_.validation_sequence_races;
#endif
    break;
  }
  return nullptr;
}

[[nodiscard]] CacheSlot JitExecutor::Implementation::replacement(
    unsigned long generation, std::uint64_t changes, std::uint64_t key) {
  const auto set_index = cache_index(generation, key, geometry_.cache_sets);
  auto &set = (*block_cache_)[set_index];
  std::size_t selected_way = 0;
  for (std::size_t way = 0; way < set.ways.size(); ++way)
    if (set.ways[way].function == nullptr ||
        set.ways[way].stamp < set.ways[selected_way].stamp)
      selected_way = way;
  auto *selected = &set.ways[selected_way];
#if UML_JIT_RUNTIME_DIAGNOSTICS
  cache_validation_diagnostics_.cache_replacements +=
      selected->function != nullptr;
#endif
  *selected = {};
  selected->generation = generation;
  selected->code_changes = changes;
  selected->key = key;
  selected->stamp = ++stamp_;
  return {
      .block = selected,
      .fallback_guest_words =
          &fallback_saved_guest_words_[set_index].ways[selected_way],
  };
}

CompiledBlock &JitExecutor::Implementation::store_native(unsigned long generation, std::uint64_t changes,
                            std::uint64_t key, JitFunction function,
                            std::uintptr_t internal_entry,
                            JitBlockLink *target_link,
                            std::span<const JitRelocation> relocations,
                            std::size_t direct_link_refusals,
                            std::span<const std::uint32_t> guest_words,
                            std::shared_ptr<const std::uint32_t[]>
                                saved_guest_words,
                            std::size_t instruction_count, std::size_t units,
                            std::uint64_t memory_write_mask,
                            std::uint64_t direct_memory_write_mask,
                            bool accesses_fp_state, bool link_target_safe,
                            bool native_fp,
                            bool guest_illegal
) {
#if !UML_JIT_RUNTIME_DIAGNOSTICS
  (void)direct_link_refusals;
#endif
  const auto selected = replacement(generation, changes, key);
  auto &block = *selected.block;
#if UML_JIT_RUNTIME_DIAGNOSTICS
  const bool shared_validation_words = saved_guest_words != nullptr;
#endif
  if (saved_guest_words == nullptr) {
    std::copy(guest_words.begin(), guest_words.end(),
              selected.fallback_guest_words->begin());
    saved_guest_words = std::shared_ptr<const std::uint32_t[]>{
        fallback_saved_guest_words_, selected.fallback_guest_words->data()};
  }
  block.function = function;
  block.internal_entry = internal_entry;
  block.link = target_link;
#if UML_JIT_RUNTIME_DIAGNOSTICS
  cache_validation_diagnostics_.target_link_refusals +=
      block.link == nullptr;
  cache_validation_diagnostics_.direct_link_refusals +=
      direct_link_refusals;
  if (links_.size() >
      cache_validation_diagnostics_.direct_link_table_high_water)
    cache_validation_diagnostics_.direct_link_table_high_water =
        links_.size();
#endif
  block.instruction_count = instruction_count;
  block.saved_units = static_cast<std::uint8_t>(units);
  block.saved_guest_words = std::move(saved_guest_words);
  block.memory_write_mask = memory_write_mask;
  block.direct_memory_write_mask = direct_memory_write_mask;
  block.accesses_fp_state = accesses_fp_state;
  block.link_target_safe = link_target_safe;
  block.native_fp = native_fp;
  block.guest_illegal = guest_illegal;
#if UML_JIT_RUNTIME_DIAGNOSTICS
  ++cache_validation_diagnostics_.translations;
  if (shared_validation_words)
    ++cache_validation_diagnostics_.shared_validation_word_installs;
  else
    ++cache_validation_diagnostics_.private_validation_word_installs;
#endif
  ++compiled_in_link_sequence_;
  for (const auto &relocation : relocations)
    if (relocation.kind == JitRelocationKind::direct_link)
      refresh_cached_link_target(relocation.key);
  activate_link(block);
  return block;
}

void JitExecutor::Implementation::clear_cache() noexcept {
  for (auto &set : *block_cache_)
    set = {};
  const auto generation = link_generation_;
  const auto changes = link_code_changes_;
  const auto initialized = link_sequence_initialized_;
  for (auto &context : link_contexts_) {
    link_context_ = &context;
    deactivate_indirect_links();
    discard_links();
  }
  link_contexts_ = {};
  link_context_ = nullptr;
  links_.clear();
  indirect_links_.clear();
  if (a32_targets_ != nullptr)
    std::fill_n(a32_targets_.get(), jit_indirect_target_count,
                JitIndirectTarget{});
  pending_indirect_ = {};
  tracked_store_arena_.reset();
  link_generation_ = 0;
  link_code_changes_ = 0;
  compiled_in_link_sequence_ = 0;
  link_sequence_initialized_ = false;
  if (initialized)
    prepare_links(generation, changes);
}


[[nodiscard]] SharedTranslationCache::CodegenProfile
JitExecutor::Implementation::shared_codegen_profile(
    std::uint8_t mode) const noexcept {
  return {.capabilities = host_capabilities_, .translation_mode = mode};
}

[[nodiscard]] std::optional<SharedTranslationMaterializationFailure>
JitExecutor::Implementation::materialize_shared_translation(
    const SharedTranslation &translation) {
  /* install_shared_translation has just passed translation through
   * valid_shared_translation, which bounds and types its relocations. */
  relocated_link_pointers_.clear();
  for (const auto &relocation : translation.relocations) {
    const void *pointer = nullptr;
    switch (relocation.kind) {
    case JitRelocationKind::direct_link: {
      /* The arena resolves the target in the private writable link table
       * before patching the literal and publishing executable code.
       */
      pointer = nullptr;
      break;
    }
    case JitRelocationKind::indirect_link:
      pointer = indirect_link_for(relocation.key);
      if (pointer == nullptr)
        return SharedTranslationMaterializationFailure::indirect_link_capacity;
      break;
    }
    /* Only indirect sites require preallocated executor state here. Direct
     * state is resolved during code publication. A table that is full fails
     * the materialization, and the local compile that follows recycles it.
     */
    relocated_link_pointers_.push_back(
        reinterpret_cast<std::uintptr_t>(pointer));
  }
  return std::nullopt;
}

[[nodiscard]] CompiledBlock *
JitExecutor::Implementation::install_shared_translation(
    unsigned long generation, std::uint64_t changes, std::uint64_t pc,
    std::uint8_t mode, std::span<const std::uint32_t> guest_words) {
  auto &cache = SharedTranslationCache::instance();
  const auto translation =
      cache.find(shared_codegen_profile(mode), pc, guest_words);
  if (translation == nullptr)
    return nullptr;
  if (!valid_shared_translation(*translation, guest_words)) {
    cache.record_materialization_failure(
        SharedTranslationMaterializationFailure::invalid_metadata);
    return nullptr;
  }
  if (const auto failure = materialize_shared_translation(*translation)) {
    cache.record_materialization_failure(*failure);
    return nullptr;
  }
  const auto stored = arena_.store_block(
      translation_key(pc, mode), translation->words,
      translation->relocations, relocated_link_pointers_, links_,
      empty_link_, generation);
  if (!stored) {
    /* The local compile that follows recycles the cache. */
    cache.record_materialization_failure(
        stored.links_exhausted
            ? SharedTranslationMaterializationFailure::direct_link_capacity
            : SharedTranslationMaterializationFailure::arena_exhaustion);
    return nullptr;
  }
  const auto internal_entry =
      reinterpret_cast<std::uintptr_t>(stored.code) +
      translation->internal_entry_word * sizeof(std::uint32_t);
  auto *block = &store_native(
      generation, changes, translation_key(pc, mode),
      sign_jit_function<JitFunction>(stored.code), internal_entry,
      stored.target_link, translation->relocations,
      stored.direct_link_refusals, guest_words, translation->guest_words,
      translation->guest_instructions, translation->guest_word_count,
      translation->memory_write_mask, translation->direct_memory_write_mask,
      translation->accesses_fp_state, translation->link_target_safe,
      translation->native_fp,
      translation->guest_illegal
  );
  cache.record_reuse();
  return block;
}

std::shared_ptr<const SharedTranslation>
JitExecutor::Implementation::publish_shared_translation(
    std::uint64_t pc, std::uint8_t mode,
    std::span<const std::uint32_t> guest_words,
    const JitCompiler::Result &compiled) {
  return publish_shared_translation(
      pc, mode, guest_words.first(compiled.guest_instructions),
      compiled.guest_instructions, compiled);
}

std::shared_ptr<const SharedTranslation>
JitExecutor::Implementation::publish_shared_translation(
    std::uint64_t pc, std::uint8_t mode,
    std::span<const std::uint32_t> validation_words,
    std::size_t guest_word_count, const JitCompiler::Result &compiled) {
  auto &cache = SharedTranslationCache::instance();
  if (!compiled.relocatable)
    return nullptr;
  const auto profile = shared_codegen_profile(mode);
  const auto preflight = cache.preflight_publication(
      profile, pc, validation_words,
      {.host_words = compiled.words.size(),
       .relocations = compiled.relocations.size(),
       .guest_words = validation_words.size()},
      compiled.direct_link_refusals,
      compiled.indirect_link_refusals);
  if (preflight.canonical != nullptr)
    return preflight.canonical;
  if (!preflight.may_construct)
    return nullptr;
#if UML_JIT_RUNTIME_DIAGNOSTICS
  if (before_shared_publication_ != nullptr)
    before_shared_publication_(shared_publication_context_);
#endif
  try {
    auto translation = std::make_shared<SharedTranslation>();
    auto saved_guest_words = std::make_shared_for_overwrite<std::uint32_t[]>(
        validation_words.size());
    std::copy(validation_words.begin(), validation_words.end(),
              saved_guest_words.get());
    translation->words.assign(compiled.words.begin(), compiled.words.end());
    translation->relocations.assign(compiled.relocations.begin(),
                                    compiled.relocations.end());
    /* The compiler's literal cells name its executor-local link records.
     * They are never canonical state: every installer resolves the neutral
     * relocation key and resolves its own mutable link records. Keep
     * the shared template free of publisher addresses so identical source and
     * codegen inputs retain identical canonical bytes.
     */
    for (const auto &relocation : translation->relocations) {
      if (relocation.word >= translation->words.size() ||
          translation->words.size() - relocation.word < 2U)
        return nullptr;
      translation->words[relocation.word] = 0;
      translation->words[relocation.word + 1U] = 0;
    }
    translation->direct_link_refusals = compiled.direct_link_refusals;
    translation->indirect_link_refusals = compiled.indirect_link_refusals;
    translation->isa = static_cast<std::uint8_t>(aarch32::key_isa(pc));
    translation->guest_words = std::move(saved_guest_words);
    translation->guest_word_count = guest_word_count;
    translation->guest_instructions = compiled.guest_instructions;
    translation->hot_words = compiled.hot_words;
    translation->internal_entry_word = compiled.internal_entry_word;
    translation->memory_write_mask = compiled.memory_write_mask;
    translation->direct_memory_write_mask = compiled.direct_memory_write_mask;
    translation->accesses_fp_state = compiled.accesses_fp_state;
    translation->link_target_safe = compiled.link_target_safe;
    translation->native_fp = compiled.native_fp;
    translation->guest_illegal = compiled.guest_illegal;
    return cache.insert(profile, pc, validation_words, std::move(translation));
  } catch (const std::bad_alloc &) {
    /* Cross-executor publication is opportunistic. The already-stored native
     * code remains valid and store_native() owns a preallocated private
     * validation-word fallback, so allocation pressure must not become a
     * guest-fatal engine exception.
     */
    return nullptr;
  }
}

} // namespace uml::cpu::arm64
