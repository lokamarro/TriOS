// SPDX-License-Identifier: GPL-2.0-or-later
//
// The AArch32 side of the JIT executor: the block entry for an A32 or T32
// translation (run_a32_block), which finds or compiles the block for the key
// the task's PC holds. The cache, arena, link tables and shared tier are the
// A64 ones.

#include "jit_executor_internal.hpp"

#include "a32/a32_compiler.hpp"
#include "a32/a32_helpers.hpp"
#include "jit_semantic_reservation.hpp"

#include "cpu/arm64/aarch32/guest_fetch.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>

namespace uml::cpu::arm64 {

namespace {

/* The most guest bytes one block reads: maximum_block_instructions T32
 * instructions of two halfwords. */
constexpr std::size_t maximum_block_bytes =
    maximum_block_instructions * sizeof(std::uint32_t);
constexpr std::size_t maximum_block_elements = maximum_block_instructions;

/* Up to bytes.size() guest bytes where the AArch32 key's code lies, the
 * count read: a page at a time, ending at an unreadable page and at the end
 * of the 4 GiB address space. Out of line, as the block entry is hot. */
[[nodiscard, gnu::noinline]] std::size_t
fetch_a32_bytes(StaticGuestMemory &memory, std::uint64_t key,
                std::span<std::byte> bytes) {
  const std::uint64_t base = aarch32::key_fetch_address(key);
  const std::uint64_t limit = std::uint64_t{1} << 32;
  const std::size_t wanted = static_cast<std::size_t>(
      std::min<std::uint64_t>(bytes.size(), limit - base));
  return aarch32::fetch_guest_units(
      base, 1, bytes.first(wanted), page_size,
      [&memory](std::uint64_t address, std::span<std::byte> out) {
        return memory.fetch(address, out);
      });
}

/* The code read at an AArch32 key. */
struct A32Code {
  std::size_t available;  // bytes read, up to the buffer's size
  std::size_t length;     // bytes of the first instruction; 0: not all read
};

/* Reads up to bytes.size() bytes of code at the key and works out the length
 * of its first instruction. Inline in the block entry, which is hot: an
 * out-of-line copy changes the registers its hit path uses. */
[[nodiscard, gnu::always_inline]] inline A32Code
fetch_a32_code(StaticGuestMemory &memory, std::uint64_t key,
               std::span<std::byte> bytes) {
  const auto available = fetch_a32_bytes(memory, key, bytes);
  std::size_t length = 4;
  if (aarch32::key_isa(key) == aarch32::GuestIsa::t32) {
    length = 2;
    if (available >= 2) {
      std::uint16_t first;
      std::memcpy(&first, bytes.data(), sizeof first);
      length = aarch32::thumb_length(first);
    }
  }
  return {.available = available, .length = available < length ? 0 : length};
}

} // namespace

/* The tracked-store variant of a block (jit_executor_execution.cpp's
 * tracked_store_function) for an AArch32 translation. */
[[nodiscard]] JitFunction JitExecutor::Implementation::tracked_store_function_a32(
    CompiledBlock &block, StaticGuestMemory &memory) {
  if (block.tracked_function != nullptr)
    return block.tracked_function;
  alignas(8) std::array<std::byte, maximum_block_bytes> bytes{};
  const auto isa = aarch32::key_isa(block.key);
  const auto length = block.saved_units * aarch32::isa_unit_bytes(isa);
  if (fetch_a32_bytes(memory, block.key, std::span{bytes}.first(length)) !=
      length)
    return nullptr;
  jit::a32::A32Compiler compiler{nullptr, nullptr, true};
  const auto compiled =
      compiler.compile({.key = block.key, .bytes = std::span{bytes}.first(length)});
  return store_tracked_variant(block, compiled.translation,
                               compiled.guest_units);
}

/* A private translation of exactly the first instruction, with the ITSTATE
 * the entry holds. */
[[nodiscard]] std::optional<JitExecutor::Implementation::SingleInstructionTranslation>
JitExecutor::Implementation::translate_a32_single(
    std::uint64_t key, std::span<const std::byte> bytes, std::uint8_t itstate,
    bool track_direct_stores) {
  jit::a32::A32Compiler compiler{nullptr, nullptr, track_direct_stores};
  const auto compiled =
      compiler.compile({.key = key,
                        .bytes = bytes,
                        .itstate = aarch32::ItState{itstate},
                        .single = true});
  return store_single_translation(compiled.translation);
}

/* The exclusive a translation deferred (its cold path returned with the
 * instruction word in the chain state and its key in cpu.pc): run it now,
 * after the block's store publication has ended. */
[[nodiscard]] Event JitExecutor::Implementation::run_deferred_a32_exclusive(
    CpuState &cpu, std::uint32_t word, std::size_t &executed) {
  const auto prefix = executed;
  const auto result =
      uml_arm64_jit_a32_execute(&cpu, &tlb_, word, cpu.pc, cpu.a32_itstate);
  const auto [event, retired] = unpack_jit_result(result);
  executed = prefix + retired;
  if (event == Event::memory_fault)
    tlb_.discard(tlb_.fault_address);
  return event;
}

/* One instruction, uncached and unlinked: an entry in the middle of an IT
 * block, a single step, or a block that could not be translated whole. */
[[nodiscard]] Event JitExecutor::Implementation::run_a32_single(
    CpuState &cpu, StaticGuestMemory &memory, std::size_t &executed) {
  executed = 0;
  alignas(8) std::array<std::byte, 4> bytes{};
  const auto key = cpu.pc;
  const auto code = fetch_a32_code(memory, key, bytes);
  if (code.length == 0) {
    record_fetch_fault(memory);
    return Event::memory_fault;
  }
  const auto bytes_view = std::span<const std::byte>{bytes}.first(code.length);
  const auto translation =
      translate_a32_single(key, bytes_view, cpu.a32_itstate, false);
  if (!translation)
    return Event::fatal;
  std::uint32_t deferred_exclusive = 0;
  const auto published = invoke_single_instruction_published(
      cpu, *translation,
      [&] {
        return translate_a32_single(key, bytes_view, cpu.a32_itstate, true);
      },
      executed, deferred_exclusive);
  if (!published)
    return Event::fatal;
  auto event = *published;
  if (deferred_exclusive != 0U)
    event = run_deferred_a32_exclusive(cpu, deferred_exclusive, executed);
  reconcile_reservation(cpu);
  return event;
}

[[nodiscard]] Event JitExecutor::Implementation::run_a32_block(
    CpuState &cpu, StaticGuestMemory &memory, unsigned long generation,
    std::size_t &executed) {
  if (!cache_lease_.admitted()) {
    executed = 0;
    return Event::fatal;
  }
  const ::uml::cpu::HostCapabilitiesScope capabilities_scope{
      host_capabilities_};
  executed = 0;
  floating_point_syndrome_ = 0;
  tlb_.fault_alignment = 0;
  select_cpu(cpu);
  const auto sequences =
      darwin_uml_static_vm_execution_context_sequences(execution_context_);
  preempted_ = sequences.execution_interrupt != 0;
  if (preempted_)
    return Event::interrupt;
  /* What the translations this run reaches are validated at, below: an ISB
   * leaves when the address space's code has changed since. */
  cpu.code_sequence = code_sequence_;
  cpu.code_sequence_seen = sequences.code_changes;
  const bool mapping_changed = mapping_generation_ != generation ||
                               mapping_changes_ != sequences.mapping_changes;
  if (prepared_memory_ != &memory || mapping_changed) {
    memory.prepare(generation, sequences.mapping_changes);
    prepared_memory_ = &memory;
  }
  if (mapping_changed) {
    tlb_.prepare(generation, sequences.mapping_changes);
    mapping_generation_ = generation;
    mapping_changes_ = sequences.mapping_changes;
  }
  prepare_links(generation, sequences.code_changes);
  if (a32_targets_ == nullptr)
    a32_targets_ = std::make_unique<JitIndirectTarget[]>(
        jit_indirect_target_count);

  const auto key = cpu.pc;
  if (!aarch32::key_is_canonical(key) || !aarch32::key_is_aarch32(key))
    return Event::pc_alignment;
  const auto pending_source =
      consume_pending_indirect(cpu, generation, sequences.code_changes, key);

  /* A single step, and an entry in the middle of an IT block, run one
   * instruction at a time and never link. */
  if (single_step_ || cpu.a32_itstate != 0U)
    return run_a32_single(cpu, memory, executed);

  auto *block = find(generation, sequences.code_changes, key, memory);
  if (block == nullptr) {
    alignas(8) std::array<std::byte, maximum_block_bytes> bytes{};
    const auto isa = aarch32::key_isa(key);
    const auto code = fetch_a32_code(memory, key, bytes);
    if (code.length == 0) {
      record_fetch_fault(memory);
      return Event::memory_fault;
    }
    const auto length = code.available;
    std::array<std::uint32_t, maximum_block_elements> elements{};
    std::memcpy(elements.data(), bytes.data(), length);
    const auto element_count = (length + sizeof(std::uint32_t) - 1U) /
                               sizeof(std::uint32_t);
    const auto fetched_elements =
        std::span<const std::uint32_t>{elements}.first(element_count);
    auto *shared = install_shared_translation(
        generation, sequences.code_changes, key, 0, fetched_elements);
    if (shared != nullptr) {
      block = shared;
    } else {
      /* A compilation's words live in its compiler, which therefore outlives
       * the store. */
      const auto make_compiler = [&] {
        return jit::a32::A32Compiler{
            this, &Implementation::resolve_block_link, false,
            &Implementation::resolve_indirect_link};
      };
      const jit::a32::Request request{
          .key = key, .bytes = std::span<const std::byte>{bytes}.first(length)};
      auto compiler = make_compiler();
      auto compiled = compiler.compile(request);
      const auto valid_compilation = [&](const jit::a32::Result &candidate) {
        const auto &translation = candidate.translation;
        return !translation.words.empty() &&
               translation.guest_instructions != 0 &&
               translation.guest_instructions <= maximum_block_instructions &&
               valid_link_state_metadata(translation.accesses_fp_state,
                                         translation.native_fp);
      };
      if (!valid_compilation(compiled))
        return run_a32_single(cpu, memory, executed);
      const auto elements_of = [&](const jit::a32::Result &candidate) {
        const auto bytes_used =
            candidate.guest_units * aarch32::isa_unit_bytes(isa);
        return fetched_elements.first(
            (bytes_used + sizeof(std::uint32_t) - 1U) / sizeof(std::uint32_t));
      };
      const auto install = [&](const jit::a32::Result &candidate,
                               const jit::detail::ExecutableArena::StoredBlock
                                   &stored,
                               const std::shared_ptr<const SharedTranslation>
                                   &translation) {
        const auto &compiled_translation = candidate.translation;
        const auto internal_entry =
            reinterpret_cast<std::uintptr_t>(stored.code) +
            compiled_translation.internal_entry_word * sizeof(std::uint32_t);
        auto saved = translation == nullptr
                         ? std::shared_ptr<const std::uint32_t[]>{}
                         : translation->guest_words;
        return &store_native(
            generation, sequences.code_changes, key,
            sign_jit_function<JitFunction>(stored.code), internal_entry,
            stored.target_link, compiled_translation.relocations,
            stored.direct_link_refusals, elements_of(candidate),
            std::move(saved), compiled_translation.guest_instructions,
            candidate.guest_units,
            compiled_translation.memory_write_mask,
            compiled_translation.direct_memory_write_mask,
            compiled_translation.accesses_fp_state,
            compiled_translation.link_target_safe, compiled_translation.native_fp,
            compiled_translation.guest_illegal);
      };
      jit::detail::ExecutableArena::StoredBlock stored{.links_exhausted = true};
      if (compiled.translation.indirect_link_refusals == 0 ||
          indirect_links_.size() == 0)
        stored = arena_.store_block(key, compiled.translation.words,
                                    compiled.translation.relocations, {},
                                    links_, empty_link_, generation);
      if (stored) {
        auto published = publish_shared_translation(
            key, 0, elements_of(compiled), compiled.guest_units,
            compiled.translation);
        block = install(compiled, stored, published);
      } else {
        clear_cache();
        arena_.reset();
        auto retry_compiler = make_compiler();
        auto retry = retry_compiler.compile(request);
        if (!valid_compilation(retry))
          return run_a32_single(cpu, memory, executed);
        stored = arena_.store_block(key, retry.translation.words,
                                    retry.translation.relocations, {}, links_,
                                    empty_link_, generation);
        if (!stored)
          return run_a32_single(cpu, memory, executed);
        auto published = publish_shared_translation(
            key, 0, elements_of(retry), retry.guest_units, retry.translation);
        block = install(retry, stored, published);
      }
    }
  }
  if (pending_source)
    train_indirect(*pending_source, *block);
  std::uint32_t deferred_exclusive = 0;
  const auto execute_native = [&](JitFunction function,
                                  bool tracked_stores) -> Event {
    if (tracked_stores)
      tracked_store_ranges_.fill({});
    std::uint8_t link_capabilities =
        block->direct_memory_write_mask != 0
            ? link_capability(JitLinkCapability::store_publication)
            : std::uint8_t{0};
    JitChainState chain{
        .stores = tracked_stores ? tracked_store_ranges_.data() : nullptr,
        .indirect_targets = a32_targets_.get(),
        .indirect_target_tag = jit_indirect_target_tag(link_epoch_),
    };
    /* The chain states what is set up, not what its first block needs: the
     * floating-point state is loaded for the whole run (and says so at no
     * cost), and the native environment, once a block has switched it on,
     * stays on until a helper or the end of the run restores it. A chain
     * that began at a block without floating-point code can then link into
     * one with it, instead of returning here for a chain that does. */
    cpu.access_fp_state();
    link_capabilities |=
        link_capability(JitLinkCapability::fp_state_materialized);
    if (block->native_fp)
      activate_native_fp_environment(cpu);
    if (cpu.gadget_fp_active)
      link_capabilities |=
          link_capability(JitLinkCapability::native_fp_environment);
    chain.link_selector = checked_link_selector(link_capabilities);
    const auto result = function(&cpu, &tlb_, &chain);
    if (tracked_stores)
      jit::semantic::invalidate_store_ranges(
          cpu, std::span<const JitStoreRange>{tracked_store_ranges_}.first(
                   block->instruction_count));
    const auto [event, current_executed] = unpack_jit_result(result);
    deferred_exclusive = chain.deferred_exclusive_instruction;
    executed = maximum_chain_instructions -
                   static_cast<std::size_t>(chain.remaining_instructions) +
               current_executed;
    if (event == Event::memory_fault)
      tlb_.discard(tlb_.fault_address);
    if (event == Event::continued && chain.indirect_miss != nullptr)
      record_pending_indirect(chain.indirect_miss, chain.indirect_source_pc,
                              cpu, generation, sequences.code_changes);
    if (deferred_exclusive == 0U)
      reconcile_reservation(cpu);
    return event;
  };

  if (block->direct_memory_write_mask == 0)
    return execute_native(block->function, false);
  Event event{};
  bool tracked_unavailable = false;
  {
    JitBlockPublication publication{store_participant_};
    if (!publication.needs_tracking()) {
      event = execute_native(block->function, false);
    } else {
      const auto tracked = tracked_store_function_a32(*block, memory);
      if (tracked == nullptr)
        tracked_unavailable = true;
      else
        event = execute_native(tracked, true);
    }
  }
  if (tracked_unavailable) {
    /* Nothing has executed: make progress with the first instruction as its
     * own translation. */
    return run_a32_single(cpu, memory, executed);
  }
  if (deferred_exclusive != 0U)
    event = run_deferred_a32_exclusive(cpu, deferred_exclusive, executed);
  reconcile_reservation(cpu);
  return event;
}

Event JitExecutor::run_a32_block(CpuState &cpu, StaticGuestMemory &memory,
                                 unsigned long generation,
                                 std::size_t &executed) {
  if (implementation_ == nullptr) {
    executed = 0;
    return Event::fatal;
  }
  return implementation_->run_a32_block(cpu, memory, generation, executed);
}

} // namespace uml::cpu::arm64
