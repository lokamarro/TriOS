// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_executor_internal.hpp"

#include "cpu/arm64/aarch32/guest_key.hpp"
#include "uml_darwin_static_mmu.h"

namespace uml::cpu::arm64 {

[[nodiscard]] void *JitExecutor::Implementation::execution_context(unsigned long generation) {
  if (execution_context_ != nullptr && execution_generation_ == generation)
    return execution_context_;
  darwin_uml_static_vm_execution_context_destroy(execution_context_);
  execution_context_ =
      darwin_uml_static_vm_execution_context_create(generation);
  execution_generation_ = execution_context_ != nullptr ? generation : 0;
  const auto *sequence =
      execution_context_ != nullptr
          ? darwin_uml_static_vm_execution_context_code_sequence(
                execution_context_)
          : nullptr;
  code_sequence_ = sequence != nullptr ? sequence : &arm64::no_code_changes;
  return execution_context_;
}

[[nodiscard]] const void *
JitExecutor::Implementation::resolve_block_link(void *context, std::uint64_t pc) {
  return static_cast<Implementation *>(context)->link_for(pc);
}

[[nodiscard]] const void *
JitExecutor::Implementation::resolve_indirect_link(void *context, std::uint64_t source_pc) {
  return static_cast<Implementation *>(context)->indirect_link_for(source_pc);
}

[[nodiscard]] JitBlockLink *JitExecutor::Implementation::link_for(std::uint64_t pc) {
  (void)pc;
  /* The literal is replaced with the private table's unique target anchor before
   * the code is published. A nonnull sentinel asks the compiler for the
   * linked shape without allocating target-owned storage during lowering.
   */
  return &empty_link_;
}

[[nodiscard]] JitIndirectLink *
JitExecutor::Implementation::indirect_link_for(std::uint64_t source_pc) {
  auto *site = indirect_links_.find_or_create(source_pc, link_generation_);
#if UML_JIT_RUNTIME_DIAGNOSTICS
  indirect_link_diagnostics_.capacity_refusals += site == nullptr;
  if (indirect_links_.size() > indirect_link_diagnostics_.table_high_water)
    indirect_link_diagnostics_.table_high_water = indirect_links_.size();
#endif
  return site;
}

void JitExecutor::Implementation::train_indirect(std::uint64_t source_key,
                    const CompiledBlock &target) {
  if (a32_targets_ != nullptr && aarch32::key_is_aarch32(target.key) &&
      target.link != nullptr && target.link_epoch == link_epoch_ &&
      target.link_target_safe) {
    const auto target_pc = translation_key_pc(target.key);
    a32_targets_[jit_indirect_target_index(target_pc)] = {
        .guest_pc = target_pc | jit_indirect_target_tag(link_epoch_),
        .link = target.link};
  }
  /* consume_pending_indirect() matched the target with the key of the
   * site's own mode, so the site never links to code of another mode. */
  auto *site = indirect_links_.find(source_key, link_generation_);
  if (site == nullptr || target.link == nullptr ||
      target.link_epoch != link_epoch_ || !target.link_target_safe) {
#if UML_JIT_RUNTIME_DIAGNOSTICS
    ++indirect_link_diagnostics_.unsafe_target_refusals;
#endif
    return;
  }
  if (site->activation_epoch != link_epoch_) {
    indirect_links_.reset(*site);
    site->activation_epoch = link_epoch_;
    site->next_active = link_context_->active_indirect_links;
    link_context_->active_indirect_links = site;
  }
  /* Generated code compares a cell with the guest's branch target. */
  const auto target_pc = translation_key_pc(target.key);
  for (std::size_t index = 0; index < site->entries.size(); ++index) {
    auto &entry = site->entries[index];
    if (entry.guest_pc != target_pc)
      continue;
    entry.link = target.link;
#if UML_JIT_RUNTIME_DIAGNOSTICS
    ++indirect_link_diagnostics_.trains;
#endif
    return;
  }
  const auto replacement = site->next_replacement & 1U;
  auto &entry = site->entries[replacement];
  /*
   * Publish the host-owned entry table before the comparison key. Execution
   * is single-threaded per executor, but this order also prevents a matching
   * key from ever exposing an uninitialized pointer during diagnostics.
   */
  entry.link = target.link;
  entry.guest_pc = target_pc;
  site->next_replacement ^= 1U;
#if UML_JIT_RUNTIME_DIAGNOSTICS
  ++indirect_link_diagnostics_.trains;
#endif
}

void JitExecutor::Implementation::discard_links() noexcept {
  if (link_context_ == nullptr)
    return;
  link_context_->active_links = nullptr;
}

void JitExecutor::Implementation::deactivate_links() noexcept {
  if (link_context_ == nullptr)
    return;
  for (auto *link = link_context_->active_links; link != nullptr;
       link = link->next_active)
    arena_.deactivate_links({&link, 1});
  discard_links();
}

void JitExecutor::Implementation::deactivate_indirect_links() noexcept {
  if (link_context_ == nullptr)
    return;
  for (auto *site = link_context_->active_indirect_links; site != nullptr;
       site = site->next_active) {
#if UML_JIT_RUNTIME_DIAGNOSTICS
    ++indirect_link_diagnostics_.deactivated_sites;
#endif
    indirect_links_.reset(*site);
  }
  link_context_->active_indirect_links = nullptr;
}

[[gnu::noinline]] void JitExecutor::Implementation::refresh_link(CompiledBlock &block) {
  if (block.link == nullptr) {
    block.link_epoch = link_epoch_;
    return;
  }
  JitBlockLinkActivation activation{
      .instruction_count = block.instruction_count,
      .epoch = link_epoch_,
  };
  const auto first_activation = block.link->activation_epoch != link_epoch_;
  if (block.link_target_safe) {
    const auto requirements =
        link_state_requirements(block.accesses_fp_state, block.native_fp);
    const auto publication =
        link_capability(JitLinkCapability::store_publication);
    for (std::size_t index = 0;
         index < jit_link_selector_capabilities.size(); ++index) {
      const auto capabilities = jit_link_selector_capabilities[index];
      if ((capabilities & requirements) != requirements ||
          (block.direct_memory_write_mask != 0 &&
           (capabilities & publication) == 0U))
        continue;
      activation.allowed_selectors |= std::uint32_t{1} << index;
    }
    activation.internal_entry = block.internal_entry;
  }
  arena_.activate_link(*block.link, activation);
  if (activation.allowed_selectors != 0 && first_activation) {
    block.link->next_active = link_context_->active_links;
    link_context_->active_links = block.link;
  }
  block.link_epoch = link_epoch_;
}

void JitExecutor::Implementation::refresh_cached_link_target(
    std::uint64_t key) {
  if (link_generation_ == 0)
    return;
  auto &set = (*block_cache_)[cache_index(
      link_generation_, key, geometry_.cache_sets)];
  for (auto &block : set.ways) {
    if (block.function == nullptr || block.generation != link_generation_ ||
        block.code_changes != link_code_changes_ || block.key != key)
      continue;
    refresh_link(block);
    return;
  }
}

} // namespace uml::cpu::arm64
