// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_executor_internal.hpp"

#include <algorithm>

namespace uml::cpu::arm64 {

#if UML_JIT_RUNTIME_DIAGNOSTICS
namespace {

[[nodiscard]] std::size_t diagnostic_table_capacity(
    const std::optional<std::size_t> &requested,
    std::size_t production_capacity) noexcept {
  return std::min(requested.value_or(production_capacity),
                  production_capacity);
}

} // namespace
#endif

JitExecutor::Implementation::Implementation(
    JitCacheExecutorLease lease, JitExecutorOptions options
#if UML_JIT_RUNTIME_DIAGNOSTICS
    , JitExecutorDiagnosticsOptions diagnostics
#endif
)
    : cache_lease_{std::move(lease)}, geometry_{cache_lease_.geometry()},
      block_cache_{std::make_unique<BlockCache>(geometry_.cache_sets)},
      fallback_saved_guest_words_{
          std::make_shared_for_overwrite<SavedGuestWordSet[]>(
              geometry_.cache_sets)},
      empty_link_{.guest_pc = 1},
#if UML_JIT_RUNTIME_DIAGNOSTICS
      links_{diagnostic_table_capacity(diagnostics.direct_link_capacity,
                                       geometry_.block_links)},
      indirect_links_{diagnostic_table_capacity(
                          diagnostics.indirect_link_capacity,
                          geometry_.indirect_links),
                      &empty_link_},
#else
      links_{geometry_.block_links},
      indirect_links_{geometry_.indirect_links, &empty_link_},
#endif
#if UML_JIT_RUNTIME_DIAGNOSTICS
      arena_{diagnostics.code_arena_size == 0
                 ? geometry_.arena_bytes
                 : std::min(diagnostics.code_arena_size,
                            geometry_.arena_bytes)},
#else
      arena_{geometry_.arena_bytes},
#endif
      tracked_store_arena_{geometry_.tracked_store_arena_bytes},
      single_instruction_arena_{single_instruction_arena_size},
#if UML_JIT_RUNTIME_DIAGNOSTICS
      shared_publication_context_{diagnostics.shared_publication_context},
      before_shared_publication_{diagnostics.before_shared_publication},
#endif
#if UML_JIT_RUNTIME_DIAGNOSTICS
      reuse_validated_stale_blocks_{
          diagnostics.reuse_validated_stale_blocks},
#endif
      host_capabilities_{options.host_capabilities} {
  // Leave at least 16 KiB of the fixed 64 KiB executor allowance for its
  // allocation and publication-registry bookkeeping in every build flavor.
  static_assert(sizeof(Implementation) <= sizeof(Tlb) + 48U * 1024U);
  relocated_link_pointers_.reserve(maximum_block_instructions);
  tlb_.unaligned_atomics = host_capabilities_.has(ArmFeature::lse2);
#if UML_JIT_RUNTIME_DIAGNOSTICS
  cache_validation_diagnostics_.direct_link_table_capacity =
      links_.capacity();
  indirect_link_diagnostics_.table_capacity = indirect_links_.capacity();
#endif
  register_jit_store_participant(store_participant_);
  SharedTranslationCache::instance().add_executor();
}

JitExecutor::Implementation::~Implementation() {
  if (current_cpu_ != nullptr)
    arm64::restore_native_fp_environment(*current_cpu_);
  release_current_reservation();
  unregister_jit_store_participant(store_participant_);
  SharedTranslationCache::instance().remove_executor();
  darwin_uml_static_vm_execution_context_destroy(execution_context_);
}

[[nodiscard]] bool JitExecutor::Implementation::begin_execution(unsigned long generation,
                                   std::uint64_t guest_pc) {
  if (!cache_lease_.admitted())
    return false;
  prepared_memory_ = nullptr;
  auto *context = execution_context(generation);
  return context != nullptr &&
         darwin_uml_static_vm_execution_context_begin(context, guest_pc) == 0;
}

void JitExecutor::Implementation::end_execution() noexcept {
  pending_indirect_ = {};
  prepared_memory_ = nullptr;
  if (current_cpu_ != nullptr)
    arm64::restore_native_fp_environment(*current_cpu_);
  release_current_reservation();
  current_cpu_ = nullptr;
  if (execution_context_ != nullptr)
    (void)darwin_uml_static_vm_execution_context_end(execution_context_);
}

[[nodiscard]] bool JitExecutor::Implementation::preempted() const noexcept { return preempted_; }

[[nodiscard]] std::uint64_t JitExecutor::Implementation::fault_address() const noexcept {
  return tlb_.fault_address;
}

bool JitExecutor::Implementation::fault_alignment() const noexcept {
  return tlb_.fault_alignment != 0;
}

bool JitExecutor::fault_alignment() const noexcept {
  return implementation_ != nullptr && implementation_->fault_alignment();
}

[[nodiscard]] Access JitExecutor::Implementation::fault_access() const noexcept {
  return tlb_.fault_access;
}

[[nodiscard]] std::uint32_t JitExecutor::Implementation::floating_point_syndrome() const noexcept {
  return floating_point_syndrome_;
}

[[nodiscard]] bool
JitExecutor::Implementation::budget_admitted() const noexcept {
  return cache_lease_.admitted();
}

#if UML_JIT_RUNTIME_DIAGNOSTICS
[[nodiscard]] JitCacheValidationDiagnostics
JitExecutor::Implementation::cache_validation_diagnostics() const noexcept {
  return cache_validation_diagnostics_;
}

[[nodiscard]] JitIndirectLinkDiagnostics
JitExecutor::Implementation::indirect_link_diagnostics() const noexcept {
  return indirect_link_diagnostics_;
}
#endif

JitExecutor::JitExecutor() : JitExecutor{JitExecutorOptions{}} {}

JitExecutor::JitExecutor(JitExecutorOptions options)
    : implementation_{[&] {
        JitCacheExecutorLease lease;
        if (!lease.admitted())
          return std::unique_ptr<Implementation>{};
        return std::make_unique<Implementation>(
          std::move(lease), options
#if UML_JIT_RUNTIME_DIAGNOSTICS
          , JitExecutorDiagnosticsOptions{}
#endif
        );
      }()} {}

std::unique_ptr<JitExecutor>
JitExecutor::make_for_managed_pool(JitExecutorOptions options) {
  return std::unique_ptr<JitExecutor>{
      new JitExecutor{ManagedPoolConstruction{}, options}};
}

JitExecutor::JitExecutor(ManagedPoolConstruction, JitExecutorOptions options)
    : implementation_{[&] {
        JitCacheExecutorLease lease{
            JitCacheExecutorLease::Admission::managed_pool};
        if (!lease.admitted())
          return std::unique_ptr<Implementation>{};
        return std::make_unique<Implementation>(
            std::move(lease), options
#if UML_JIT_RUNTIME_DIAGNOSTICS
            , JitExecutorDiagnosticsOptions{}
#endif
        );
      }()} {}

#if UML_JIT_RUNTIME_DIAGNOSTICS
JitExecutor::JitExecutor(JitExecutorOptions options,
                         JitExecutorDiagnosticsOptions diagnostics)
    : implementation_{[&] {
        JitCacheExecutorLease lease;
        if (!lease.admitted())
          return std::unique_ptr<Implementation>{};
        return std::make_unique<Implementation>(
            std::move(lease), options, diagnostics);
      }()} {}
#endif

JitExecutor::~JitExecutor() = default;

bool JitExecutor::begin_execution(unsigned long generation,
                                  std::uint64_t guest_pc) {
  return implementation_ != nullptr &&
         implementation_->begin_execution(generation, guest_pc);
}

void JitExecutor::end_execution() noexcept {
  if (implementation_ != nullptr)
    implementation_->end_execution();
}

Event JitExecutor::run_block(CpuState &cpu, StaticGuestMemory &memory,
                             unsigned long generation, std::size_t &executed) {
  if (implementation_ == nullptr) {
    executed = 0;
    return Event::fatal;
  }
  return implementation_->run_block(cpu, memory, generation, executed);
}

bool JitExecutor::preempted() const noexcept {
  return implementation_ != nullptr && implementation_->preempted();
}

void JitExecutor::set_single_step(bool enabled) noexcept {
  if (implementation_ != nullptr)
    implementation_->set_single_step(enabled);
}

std::uint64_t JitExecutor::fault_address() const noexcept {
  return implementation_ == nullptr ? 0 : implementation_->fault_address();
}

Access JitExecutor::fault_access() const noexcept {
  return implementation_ == nullptr ? Access::read
                                    : implementation_->fault_access();
}

std::uint32_t JitExecutor::floating_point_syndrome() const noexcept {
  return implementation_ == nullptr
             ? 0
             : implementation_->floating_point_syndrome();
}

bool JitExecutor::budget_admitted() const noexcept {
  return implementation_ != nullptr;
}

#if UML_JIT_RUNTIME_DIAGNOSTICS
JitCacheValidationDiagnostics
JitExecutor::cache_validation_diagnostics() const noexcept {
  return implementation_ == nullptr
             ? JitCacheValidationDiagnostics{}
             : implementation_->cache_validation_diagnostics();
}

JitIndirectLinkDiagnostics
JitExecutor::indirect_link_diagnostics() const noexcept {
  return implementation_ == nullptr
             ? JitIndirectLinkDiagnostics{}
             : implementation_->indirect_link_diagnostics();
}
#endif

bool native_jit_available() noexcept {
  return jit::detail::ExecutableArena::available();
}

} // namespace uml::cpu::arm64
