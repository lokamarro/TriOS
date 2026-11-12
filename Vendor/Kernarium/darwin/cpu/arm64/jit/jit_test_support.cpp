// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_arena.hpp"
#include "jit_cache.hpp"
#include "jit_publication.hpp"
#include "jit_semantic_reservation.hpp"
#include "jit_test_api.hpp"
#include "a32/a32_compiler.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <semaphore>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace uml::cpu::arm64 {

namespace jit::testing {

namespace {

[[nodiscard]] CompiledCode copied_code(const JitCompiler::Result &compiled) {
  CompiledCode result{
      .words = std::vector<std::uint32_t>{compiled.words.begin(),
                                         compiled.words.end()},
      .executable_words = compiled.executable_words,
      .hot_words = compiled.hot_words,
      .direct_link_refusals = compiled.direct_link_refusals,
      .indirect_link_refusals = compiled.indirect_link_refusals,
      .guest_instructions = compiled.guest_instructions,
      .internal_entry_word = compiled.internal_entry_word,
      .memory_write_mask = compiled.memory_write_mask,
      .direct_memory_write_mask = compiled.direct_memory_write_mask,
      .accesses_fp_state = compiled.accesses_fp_state,
      .link_target_safe = compiled.link_target_safe,
      .native_fp = compiled.native_fp,
      .guest_illegal = compiled.guest_illegal,
  };
  return result;
}

} // namespace

namespace {

[[nodiscard]] unsigned waiting_stores() noexcept {
  return jit_waiting_store_count_value.load(std::memory_order_seq_cst);
}

[[nodiscard]] bool jit_exclusive_gate_is_quiescent_for_testing() {
  return jit_exclusive_operation_phase.load(std::memory_order_seq_cst) == 0U &&
         waiting_stores() == 0U &&
         jit_exclusive_operation_count.load(std::memory_order_seq_cst) == 0U &&
         !jit_exclusive_operation_turn.load(std::memory_order_seq_cst);
}

void wait_for_jit_exclusive_turn_waiter() {
  while (jit_exclusive_operation_count.load(
             std::memory_order_seq_cst) < 2U)
    std::this_thread::yield();
}

[[maybe_unused, nodiscard]] bool
waiting_store_precedes_queued_helper_for_testing(bool queue_helper_first) {
  if (!jit_exclusive_gate_is_quiescent_for_testing())
    return false;

  JitStoreParticipant participant;
  register_jit_store_participant(participant);

  std::binary_semaphore first_helper_entered{0};
  std::binary_semaphore release_first_helper{0};
  std::binary_semaphore store_started{0};
  std::binary_semaphore store_entered{0};
  std::binary_semaphore release_store{0};
  std::atomic<unsigned> entry_order{};
  std::atomic<unsigned> store_order{};
  std::atomic<unsigned> second_helper_order{};

  std::thread first_helper{[&] {
    JitExclusiveOperationScope scope;
    first_helper_entered.release();
    release_first_helper.acquire();
  }};
  first_helper_entered.acquire();

  std::thread store;
  const auto start_store = [&] {
    store = std::thread{[&] {
      store_started.release();
      JitBlockPublication publication{participant};
      store_order.store(
          entry_order.fetch_add(1, std::memory_order_relaxed) + 1U,
          std::memory_order_relaxed);
      store_entered.release();
      release_store.acquire();
    }};
    store_started.acquire();
    while (waiting_stores() == 0U ||
           participant.state().load(std::memory_order_seq_cst) !=
               JitStoreState::idle)
      std::this_thread::yield();
  };

  std::thread second_helper;
  const auto start_second_helper = [&] {
    second_helper = std::thread{[&] {
      JitExclusiveOperationScope scope;
      second_helper_order.store(
          entry_order.fetch_add(1, std::memory_order_relaxed) + 1U,
          std::memory_order_relaxed);
    }};
    wait_for_jit_exclusive_turn_waiter();
  };

  if (queue_helper_first) {
    start_second_helper();
    start_store();
  } else {
    start_store();
    start_second_helper();
  }

  const auto phase_before_release =
      jit_exclusive_operation_phase.load(std::memory_order_seq_cst);
  const bool both_waiting_before_release =
      jit_exclusive_operation_is_active(phase_before_release) &&
      waiting_stores() == 1U &&
      store_order.load(std::memory_order_relaxed) == 0U &&
      second_helper_order.load(std::memory_order_relaxed) == 0U &&
      participant.state().load(std::memory_order_seq_cst) ==
          JitStoreState::idle;

  release_first_helper.release();
  first_helper.join();
  store_entered.acquire();
  while (!jit_exclusive_operation_is_active(
      jit_exclusive_operation_phase.load(std::memory_order_seq_cst)))
    std::this_thread::yield();
  const auto phase_while_store_runs =
      jit_exclusive_operation_phase.load(std::memory_order_seq_cst);
  const bool store_won_phase =
      store_order.load(std::memory_order_relaxed) == 1U &&
      second_helper_order.load(std::memory_order_relaxed) == 0U &&
      jit_exclusive_operation_is_active(phase_while_store_runs) &&
      waiting_stores() == 0U &&
      participant.state().load(std::memory_order_seq_cst) ==
          JitStoreState::direct;

  release_store.release();
  store.join();
  second_helper.join();

  const bool completed_in_order =
      second_helper_order.load(std::memory_order_relaxed) == 2U &&
      participant.state().load(std::memory_order_seq_cst) ==
          JitStoreState::idle &&
      jit_exclusive_gate_is_quiescent_for_testing();
  unregister_jit_store_participant(participant);
  return both_waiting_before_release && store_won_phase &&
         completed_in_order;
}

} // namespace

bool exclusive_operations_are_serialized_for_testing() {
  if (!jit_exclusive_gate_is_quiescent_for_testing())
    return false;

  std::binary_semaphore first_entered{0};
  std::binary_semaphore release_first{0};
  std::binary_semaphore second_entered{0};

  std::thread first{[&] {
    JitExclusiveOperationScope scope;
    first_entered.release();
    release_first.acquire();
  }};
  first_entered.acquire();

  std::thread second{[&] {
    JitExclusiveOperationScope scope;
    second_entered.release();
  }};
  wait_for_jit_exclusive_turn_waiter();

  const auto queued_phase =
      jit_exclusive_operation_phase.load(std::memory_order_seq_cst);
  const bool follower_parked =
      !second_entered.try_acquire() &&
      jit_exclusive_operation_is_active(queued_phase) &&
      waiting_stores() == 0U;

  release_first.release();
  first.join();
  if (follower_parked)
    second_entered.acquire();
  second.join();
  return follower_parked && jit_exclusive_gate_is_quiescent_for_testing();
}

bool normal_store_leaves_exclusive_gate_unchanged_for_testing() {
  if (!jit_exclusive_gate_is_quiescent_for_testing())
    return false;

  JitStoreParticipant participant;
  register_jit_store_participant(participant);
  bool direct_without_mutation = false;
  {
    JitBlockPublication publication{participant};
    direct_without_mutation =
        !publication.needs_tracking() &&
        participant.state().load(std::memory_order_seq_cst) ==
            JitStoreState::direct &&
        jit_exclusive_operation_phase.load(std::memory_order_seq_cst) == 0U;
  }
  const bool unchanged_after =
      participant.state().load(std::memory_order_seq_cst) ==
          JitStoreState::idle &&
      jit_exclusive_operation_phase.load(std::memory_order_seq_cst) == 0U;
  unregister_jit_store_participant(participant);
  return direct_without_mutation && unchanged_after;
}

bool store_first_publication_handshake_for_testing() {
  if (!jit_exclusive_gate_is_quiescent_for_testing())
    return false;

  JitStoreParticipant participant;
  register_jit_store_participant(participant);

  std::binary_semaphore store_entered{0};
  std::binary_semaphore release_store{0};
  std::binary_semaphore helper_entered{0};
  std::binary_semaphore release_helper{0};
  std::thread store{[&] {
    JitBlockPublication publication{participant};
    store_entered.release();
    release_store.acquire();
  }};
  store_entered.acquire();

  std::thread helper{[&] {
    JitExclusiveOperationScope scope;
    helper_entered.release();
    release_helper.acquire();
  }};
  while (!jit_exclusive_operation_is_active(
      jit_exclusive_operation_phase.load(std::memory_order_seq_cst)))
    std::this_thread::yield();

  const auto phase_while_helper_waits =
      jit_exclusive_operation_phase.load(std::memory_order_seq_cst);
  const bool helper_crossed_published_store = helper_entered.try_acquire();
  release_store.release();
  store.join();
  if (!helper_crossed_published_store)
    helper_entered.acquire();
  release_helper.release();
  helper.join();

  const bool idle =
      participant.state().load(std::memory_order_seq_cst) ==
          JitStoreState::idle &&
      jit_exclusive_gate_is_quiescent_for_testing();
  unregister_jit_store_participant(participant);
  return !helper_crossed_published_store &&
         jit_exclusive_operation_is_active(phase_while_helper_waits) &&
         waiting_stores() == 0U && idle;
}

bool waiting_store_precedes_later_helper_for_testing() {
  return waiting_store_precedes_queued_helper_for_testing(false);
}

bool waiting_store_precedes_already_queued_helper_for_testing() {
  return waiting_store_precedes_queued_helper_for_testing(true);
}

bool all_waiting_stores_precede_queued_helper_for_testing() {
  if (!jit_exclusive_gate_is_quiescent_for_testing())
    return false;

  JitStoreParticipant first_participant;
  JitStoreParticipant second_participant;
  register_jit_store_participant(first_participant);
  register_jit_store_participant(second_participant);

  std::binary_semaphore first_helper_entered{0};
  std::binary_semaphore release_first_helper{0};
  std::binary_semaphore first_store_started{0};
  std::binary_semaphore second_store_started{0};
  std::binary_semaphore first_store_entered{0};
  std::binary_semaphore second_store_entered{0};
  std::binary_semaphore release_first_store{0};
  std::binary_semaphore release_second_store{0};
  std::binary_semaphore queued_helper_entered{0};
  std::atomic<unsigned> released_stores{};
  std::atomic<unsigned> helper_observed_releases{};

  std::thread first_helper{[&] {
    JitExclusiveOperationScope scope;
    first_helper_entered.release();
    release_first_helper.acquire();
  }};
  first_helper_entered.acquire();

  std::thread first_store{[&] {
    first_store_started.release();
    JitBlockPublication publication{first_participant};
    first_store_entered.release();
    release_first_store.acquire();
  }};
  std::thread second_store{[&] {
    second_store_started.release();
    JitBlockPublication publication{second_participant};
    second_store_entered.release();
    release_second_store.acquire();
  }};
  first_store_started.acquire();
  second_store_started.acquire();
  while (waiting_stores() != 2U ||
         first_participant.state().load(std::memory_order_seq_cst) !=
             JitStoreState::idle ||
         second_participant.state().load(std::memory_order_seq_cst) !=
             JitStoreState::idle)
    std::this_thread::yield();

  std::thread queued_helper{[&] {
    JitExclusiveOperationScope scope;
    helper_observed_releases.store(
        released_stores.load(std::memory_order_seq_cst),
        std::memory_order_seq_cst);
    queued_helper_entered.release();
  }};
  wait_for_jit_exclusive_turn_waiter();

  const auto phase_before_release =
      jit_exclusive_operation_phase.load(std::memory_order_seq_cst);
  const auto stores_waiting_before_release =
      waiting_stores();
  release_first_helper.release();
  first_helper.join();
  first_store_entered.acquire();
  second_store_entered.acquire();
  while (!jit_exclusive_operation_is_active(
      jit_exclusive_operation_phase.load(std::memory_order_seq_cst)))
    std::this_thread::yield();

  const bool helper_crossed_both_stores =
      queued_helper_entered.try_acquire();
  const bool both_stores_won_phase =
      jit_exclusive_operation_is_active(phase_before_release) &&
      stores_waiting_before_release == 2U &&
      first_participant.state().load(std::memory_order_seq_cst) ==
          JitStoreState::direct &&
      second_participant.state().load(std::memory_order_seq_cst) ==
          JitStoreState::direct &&
      !helper_crossed_both_stores;

  released_stores.store(1U, std::memory_order_seq_cst);
  release_first_store.release();
  first_store.join();
  const bool helper_crossed_second_store =
      queued_helper_entered.try_acquire();
  const bool helper_waited_for_second =
      second_participant.state().load(std::memory_order_seq_cst) ==
          JitStoreState::direct &&
      !helper_crossed_second_store;

  released_stores.store(2U, std::memory_order_seq_cst);
  release_second_store.release();
  second_store.join();
  if (!helper_crossed_both_stores && !helper_crossed_second_store)
    queued_helper_entered.acquire();
  queued_helper.join();

  const bool completed =
      helper_observed_releases.load(std::memory_order_seq_cst) == 2U &&
      first_participant.state().load(std::memory_order_seq_cst) ==
          JitStoreState::idle &&
      second_participant.state().load(std::memory_order_seq_cst) ==
          JitStoreState::idle &&
      jit_exclusive_gate_is_quiescent_for_testing();
  unregister_jit_store_participant(second_participant);
  unregister_jit_store_participant(first_participant);
  return both_stores_won_phase && helper_waited_for_second && completed;
}

bool late_store_waits_for_current_helper_for_testing() {
  if (!jit_exclusive_gate_is_quiescent_for_testing())
    return false;

  JitStoreParticipant participant;
  register_jit_store_participant(participant);
  std::binary_semaphore helper_entered{0};
  std::binary_semaphore release_helper{0};
  std::binary_semaphore store_entered{0};

  std::thread helper{[&] {
    JitExclusiveOperationScope scope;
    helper_entered.release();
    release_helper.acquire();
  }};
  helper_entered.acquire();
  std::thread store{[&] {
    JitBlockPublication publication{participant};
    store_entered.release();
  }};
  while (participant.state().load(std::memory_order_seq_cst) !=
         JitStoreState::idle)
    std::this_thread::yield();
  const bool parked = !store_entered.try_acquire();
  release_helper.release();
  helper.join();
  if (parked)
    store_entered.acquire();
  store.join();
  const bool completed =
      parked &&
      participant.state().load(std::memory_order_seq_cst) ==
          JitStoreState::idle &&
      jit_exclusive_gate_is_quiescent_for_testing();
  unregister_jit_store_participant(participant);
  return completed;
}

bool tracked_store_invalidation_does_not_deadlock_for_testing() {
  if (!jit_exclusive_gate_is_quiescent_for_testing())
    return false;

  struct Shared final {
    JitStoreParticipant participant;
    CpuState reservation_cpu{};
    CpuState store_cpu{};
    alignas(16) std::array<std::byte, 16> backing{};
    JitStoreRange range{};
    std::binary_semaphore store_published{0};
    std::binary_semaphore begin_invalidation{0};
    std::binary_semaphore invalidation_finished{0};
    std::binary_semaphore helper_entered{0};
    std::atomic<bool> tracked{};
  };
  auto shared = std::make_shared<Shared>();
  shared->range = {
      .address = reinterpret_cast<std::uint64_t>(shared->backing.data()),
      .size = shared->backing.size(),
  };
  register_jit_store_participant(shared->participant);
  if (!begin_jit_reservation(shared->reservation_cpu)) {
    unregister_jit_store_participant(shared->participant);
    return false;
  }

  std::thread store{[shared] {
    {
      JitBlockPublication publication{shared->participant};
      shared->tracked.store(publication.needs_tracking(),
                            std::memory_order_seq_cst);
      shared->store_published.release();
      shared->begin_invalidation.acquire();
      semantic::invalidate_store_ranges(shared->store_cpu,
                                        {&shared->range, 1U});
    }
    shared->invalidation_finished.release();
  }};
  shared->store_published.acquire();

  std::thread helper{[shared] {
    JitExclusiveOperationScope scope;
    shared->helper_entered.release();
  }};
  while (!jit_exclusive_operation_is_active(
      jit_exclusive_operation_phase.load(std::memory_order_seq_cst)))
    std::this_thread::yield();

  const bool helper_crossed_store = shared->helper_entered.try_acquire();
  const bool tracked_before_invalidation =
      shared->tracked.load(std::memory_order_seq_cst) &&
      shared->participant.state().load(std::memory_order_seq_cst) ==
          JitStoreState::tracked &&
      !helper_crossed_store;
  shared->begin_invalidation.release();

  constexpr auto timeout = std::chrono::seconds{5};
  if (!shared->invalidation_finished.try_acquire_for(timeout)) {
    /*
     * A semantic-lock-before-publication-scan regression deadlocks these two
     * threads. Keep their shared state alive until the test process reports
     * failure instead of blocking forever in thread destructors.
     */
    store.detach();
    helper.detach();
    return false;
  }
  store.join();
  const bool helper_completed =
      helper_crossed_store ||
      shared->helper_entered.try_acquire_for(timeout);
  if (!helper_completed) {
    helper.detach();
    return false;
  }
  helper.join();

  const bool completed =
      tracked_before_invalidation && !helper_crossed_store &&
      shared->participant.state().load(std::memory_order_seq_cst) ==
          JitStoreState::idle &&
      jit_exclusive_gate_is_quiescent_for_testing();
  end_jit_reservation(shared->reservation_cpu);
  unregister_jit_store_participant(shared->participant);
  return completed;
}

bool publication_gate_randomized_stress_for_testing() {
  if (!jit_exclusive_gate_is_quiescent_for_testing())
    return false;

  constexpr std::size_t helper_threads = 3;
  constexpr std::size_t store_threads = 4;
  constexpr unsigned helper_iterations = 400;
  constexpr unsigned store_iterations = 600;
  std::array<JitStoreParticipant, store_threads> participants;
  for (auto &participant : participants)
    register_jit_store_participant(participant);

  std::atomic<bool> start{};
  std::atomic<bool> failed{};
  std::atomic<unsigned> active_helpers{};
  std::atomic<unsigned> active_stores{};
  const auto jitter = [](std::uint64_t &state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    if ((state & 7U) == 0U)
      std::this_thread::yield();
  };

  std::array<std::thread, helper_threads> helpers;
  for (std::size_t index = 0; index < helpers.size(); ++index) {
    helpers[index] = std::thread{[&, index] {
      std::uint64_t random = 0x9e37'79b9'7f4a'7c15ULL ^
                             (index * 0x100'0000'01b3ULL);
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();
      for (unsigned iteration = 0; iteration < helper_iterations;
           ++iteration) {
        jitter(random);
        JitExclusiveOperationScope scope;
        if (active_helpers.fetch_add(1, std::memory_order_seq_cst) != 0U ||
            active_stores.load(std::memory_order_seq_cst) != 0U)
          failed.store(true, std::memory_order_seq_cst);
        jitter(random);
        if (active_stores.load(std::memory_order_seq_cst) != 0U)
          failed.store(true, std::memory_order_seq_cst);
        active_helpers.fetch_sub(1, std::memory_order_seq_cst);
      }
    }};
  }

  std::array<std::thread, store_threads> stores;
  for (std::size_t index = 0; index < stores.size(); ++index) {
    stores[index] = std::thread{[&, index] {
      std::uint64_t random = 0xd1b5'4a32'd192'ed03ULL ^
                             (index * 0x9e37'79b9ULL);
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();
      for (unsigned iteration = 0; iteration < store_iterations;
           ++iteration) {
        jitter(random);
        JitBlockPublication publication{participants[index]};
        active_stores.fetch_add(1, std::memory_order_seq_cst);
        if (active_helpers.load(std::memory_order_seq_cst) != 0U)
          failed.store(true, std::memory_order_seq_cst);
        jitter(random);
        if (active_helpers.load(std::memory_order_seq_cst) != 0U)
          failed.store(true, std::memory_order_seq_cst);
        active_stores.fetch_sub(1, std::memory_order_seq_cst);
      }
    }};
  }

  start.store(true, std::memory_order_release);
  for (auto &helper : helpers)
    helper.join();
  for (auto &store : stores)
    store.join();

  const bool all_idle = std::ranges::all_of(
      participants, [](const auto &participant) {
        return participant.state().load(std::memory_order_seq_cst) ==
               JitStoreState::idle;
      });
  const bool completed =
      !failed.load(std::memory_order_seq_cst) &&
      active_helpers.load(std::memory_order_seq_cst) == 0U &&
      active_stores.load(std::memory_order_seq_cst) == 0U && all_idle &&
      jit_exclusive_gate_is_quiescent_for_testing();
  for (auto &participant : participants)
    unregister_jit_store_participant(participant);
  return completed;
}

bool publication_participant_lifetime_is_idempotent_for_testing() {
  if (!jit_exclusive_gate_is_quiescent_for_testing())
    return false;

  JitStoreParticipant participant;
  std::size_t initial_size{};
  {
    std::scoped_lock lock{jit_store_participants_mutex};
    initial_size = jit_store_participants.size();
    if (std::ranges::find(jit_store_participants, &participant) !=
        jit_store_participants.end())
      return false;
  }

  register_jit_store_participant(participant);
  register_jit_store_participant(participant);
  bool exactly_once{};
  {
    std::scoped_lock lock{jit_store_participants_mutex};
    exactly_once = jit_store_participants.size() == initial_size + 1U &&
                   std::ranges::count(jit_store_participants, &participant) ==
                       1U;
  }

  participant.state().store(JitStoreState::direct,
                          std::memory_order_seq_cst);
  const bool visible_while_registered =
      has_published_jit_store(nullptr);
  participant.state().store(JitStoreState::idle, std::memory_order_seq_cst);
  unregister_jit_store_participant(participant);
  unregister_jit_store_participant(participant);

  bool fully_removed{};
  {
    std::scoped_lock lock{jit_store_participants_mutex};
    fully_removed = jit_store_participants.size() == initial_size &&
                    std::ranges::find(jit_store_participants, &participant) ==
                        jit_store_participants.end();
  }
  return exactly_once && visible_while_registered && fully_removed;
}

CompiledCode compile_block(
    std::uint64_t pc,
    std::span<const std::uint32_t> instructions) {
  JitCompiler compiler{};
  return copied_code(compiler.compile(pc, instructions));
}

namespace {

[[nodiscard]] ::uml::cpu::EngineEvent
run_translation_on_tlb(const JitCompiler::Result &compiled, CpuState &cpu,
                       ::uml::memory::Tlb &tlb) {
  if (compiled.words.empty() || !compiled.relocations.empty())
    return ::uml::cpu::EngineEvent::fatal;
  detail::ExecutableArena arena{256U * 1024U};
  auto *const code = arena.valid() ? arena.store(compiled.words) : nullptr;
  if (code == nullptr)
    return ::uml::cpu::EngineEvent::fatal;
  JitChainState chain{};
  const auto result =
      sign_jit_function<JitFunction>(code)(&cpu, &tlb, &chain);
  return unpack_jit_result(result).event;
}

} // namespace

::uml::cpu::EngineEvent
run_block_on_tlb_for_testing(std::uint64_t pc,
                             std::span<const std::uint32_t> instructions,
                             CpuState &cpu, ::uml::memory::Tlb &tlb,
                             std::uint8_t mode) {
  JitCompiler compiler{};
  compiler.set_translation_mode(mode);
  return run_translation_on_tlb(compiler.compile(pc, instructions), cpu, tlb);
}

::uml::cpu::EngineEvent
run_a32_block_on_tlb_for_testing(std::uint64_t key,
                                 std::span<const std::byte> bytes,
                                 CpuState &cpu, ::uml::memory::Tlb &tlb) {
  a32::A32Compiler compiler{};
  const auto compiled = compiler.compile({.key = key, .bytes = bytes});
  return run_translation_on_tlb(compiled.translation, cpu, tlb);
}

A32Compilation compile_a32_block(std::uint64_t key,
                                 std::span<const std::byte> bytes,
                                 std::uint8_t itstate, bool single) {
  a32::A32Compiler compiler{};
  const auto compiled = compiler.compile({.key = key,
                                          .bytes = bytes,
                                          .itstate = aarch32::ItState{itstate},
                                          .single = single});
  return {.code = copied_code(compiled.translation),
          .guest_units = compiled.guest_units};
}

A32Compilation compile_a32_linked_block(std::uint64_t key,
                                        std::span<const std::byte> bytes) {
  struct Context final {
    JitBlockLink direct{};
    JitIndirectLink site{};
  } context;
  const auto direct = +[](void *opaque, std::uint64_t key) -> const void * {
    auto &resolved = *static_cast<Context *>(opaque);
    resolved.direct.guest_pc = key;
    return &resolved.direct;
  };
  const auto indirect =
      +[](void *opaque, std::uint64_t key) -> const void * {
    auto &resolved = *static_cast<Context *>(opaque);
    resolved.site.source_pc = key;
    return &resolved.site;
  };
  a32::A32Compiler compiler{&context, direct, false, indirect};
  const auto compiled = compiler.compile({.key = key, .bytes = bytes});
  return {.code = copied_code(compiled.translation),
          .guest_units = compiled.guest_units};
}

ModeCompilation compile_block_in_mode(
    std::uint64_t pc, std::span<const std::uint32_t> instructions,
    std::uint8_t mode, LinkShape shape) {
  struct Context final {
    JitBlockLink empty{.guest_pc = 1};
    JitBlockLink direct{};
    JitIndirectLink site{};
    ModeCompilation result;
  } context;
  for (auto &entry : context.site.entries) {
    entry.guest_pc = 1;
    entry.link = &context.empty;
  }
  const auto direct = +[](void *opaque, std::uint64_t key) -> const void * {
    auto &resolved = *static_cast<Context *>(opaque);
    resolved.result.direct_keys.push_back(key);
    resolved.direct.guest_pc = key;
    return &resolved.direct;
  };
  const auto indirect =
      +[](void *opaque, std::uint64_t key) -> const void * {
    auto &resolved = *static_cast<Context *>(opaque);
    resolved.result.indirect_keys.push_back(key);
    resolved.site.source_pc = key;
    return &resolved.site;
  };
  JitCompiler compiler{
      shape == LinkShape::tracked ? nullptr : &context,
      shape == LinkShape::direct ? direct : nullptr,
      shape == LinkShape::tracked,
      shape == LinkShape::indirect ? indirect : nullptr};
  compiler.set_translation_mode(mode);
  const auto compiled = compiler.compile(pc, instructions);
  context.result.code = copied_code(compiled);
  for (const auto &relocation : compiled.relocations)
    context.result.relocation_keys.push_back(relocation.key);
  return std::move(context.result);
}

CompiledCode compile_block_without_table_list_planning(
    std::uint64_t pc,
    std::span<const std::uint32_t> instructions) {
  JitCompiler compiler{};
  compiler.set_table_list_planning_enabled(false);
  return copied_code(compiler.compile(pc, instructions));
}

CompiledCode compile_block_with_feature_override(
    std::uint64_t pc, std::span<const std::uint32_t> instructions,
    ArmFeature feature, bool available) {
  auto features = host_capabilities().supported;
  features.set(static_cast<std::size_t>(feature), available);
  JitCompiler compiler{features};
  return copied_code(compiler.compile(pc, instructions));
}

namespace {

class PopulationCountProbe final : private JitCompilerFamilies {
public:
  [[nodiscard]] std::vector<std::uint32_t> build(bool wide) {
    Arm64Emitter emitter;
    bool native_fp = false;
    attach_family_emitter(&emitter, &native_fp, false);
    emit_gpr_population_count(wide, 0, 0);
    emitter.emit(0xd65f'03c0U); // ret
    const auto words = emitter.finish();
    return {words.begin(), words.end()};
  }
};

} // namespace

std::uint64_t gpr_population_count_for_testing(bool wide,
                                               std::uint64_t value) {
  static detail::ExecutableArena arena{16U * 1024U};
  static void *const code[2] = {
      arena.store(PopulationCountProbe{}.build(false)),
      arena.store(PopulationCountProbe{}.build(true)),
  };
  using Function = std::uint64_t (*)(std::uint64_t);
  return sign_jit_function<Function>(code[wide ? 1 : 0])(value);
}

std::vector<std::uint32_t>
lower_memory_barrier_for_testing(std::uint32_t instruction, bool native_sb) {
  const auto lowered = lower_memory_barrier(instruction, native_sb);
  return {lowered.words.begin(), lowered.words.begin() + lowered.count};
}

CompiledCode compile_tracked_block(
    std::uint64_t pc,
    std::span<const std::uint32_t> instructions) {
  JitCompiler compiler{/*link_context=*/nullptr,
                       /*link_resolver=*/nullptr,
                       /*track_direct_stores=*/true};
  return copied_code(compiler.compile(pc, instructions));
}

CompiledCode compile_linked_block(
    std::uint64_t pc,
    std::span<const std::uint32_t> instructions) {
  JitBlockLink link{};
  const auto resolver = +[](void *context, std::uint64_t target) -> const void * {
    auto &resolved = *static_cast<JitBlockLink *>(context);
    resolved.guest_pc = target;
    return &resolved;
  };
  JitCompiler compiler{&link, resolver};
  return copied_code(compiler.compile(pc, instructions));
}

CompiledCode compile_linked_block_with_capacity(
    std::uint64_t pc, std::span<const std::uint32_t> instructions,
    std::size_t capacity) {
  /* Resolves each distinct target to its own record until the table is
   * full, after which the compiler must fall back to an unlinked exit. */
  struct Links final {
    std::vector<JitBlockLink> records;
    std::size_t capacity{};
  } links;
  links.records.reserve(capacity);
  links.capacity = capacity;
  const auto resolver = +[](void *context,
                            std::uint64_t target) -> const void * {
    auto &table = *static_cast<Links *>(context);
    for (auto &record : table.records)
      if (record.guest_pc == target)
        return &record;
    if (table.records.size() >= table.capacity)
      return nullptr;
    auto &record = table.records.emplace_back();
    record.guest_pc = target;
    return &record;
  };
  JitCompiler compiler{&links, resolver};
  return copied_code(compiler.compile(pc, instructions));
}

CompiledCode compile_indirect_linked_block(
    std::uint64_t pc,
    std::span<const std::uint32_t> instructions) {
  struct Context final {
    JitBlockLink empty{.guest_pc = 1};
    JitIndirectLink site{};
  } context;
  context.site.source_pc = pc;
  for (auto &entry : context.site.entries) {
    entry.guest_pc = 1;
    entry.link = &context.empty;
  }
  const auto resolver =
      +[](void *opaque, std::uint64_t) -> const void * {
    return &static_cast<Context *>(opaque)->site;
  };
  JitCompiler compiler{&context, nullptr, false, resolver};
  return copied_code(compiler.compile(pc, instructions));
}

CompiledCode compile_indirect_linked_block_with_capacity(
    std::uint64_t pc, std::span<const std::uint32_t> instructions,
    std::size_t capacity) {
  JitBlockLink empty{.guest_pc = 1};
  JitIndirectLinkTable links{capacity, &empty};
  const auto resolver = +[](void *context,
                            std::uint64_t source_pc) -> const void * {
    return static_cast<JitIndirectLinkTable *>(context)->find_or_create(
        source_pc);
  };
  JitCompiler compiler{&links,
                       /*link_resolver=*/nullptr,
                       /*track_direct_stores=*/false, resolver};
  return copied_code(compiler.compile(pc, instructions));
}

std::vector<std::uint32_t>
compile_stable_pointer_load_for_testing(std::uintptr_t value,
                                        std::size_t literal_range_words) {
  Arm64Emitter emitter{literal_range_words};
  emitter.load_stable_pointer(12, value);
  emitter.emit(0xd65f'03c0U); // ret
  const auto words = emitter.finish();
  return {words.begin(), words.end()};
}

std::vector<std::uint32_t> compile_two_stable_pointer_loads_for_testing(
    std::uintptr_t first, std::uintptr_t second,
    std::size_t literal_range_words) {
  Arm64Emitter emitter{literal_range_words};
  emitter.load_stable_pointer(12, first);
  emitter.load_stable_pointer(13, second);
  emitter.emit(0xd65f'03c0U); // ret
  const auto words = emitter.finish();
  return {words.begin(), words.end()};
}

std::vector<std::uint32_t>
materialize_constant_for_testing(unsigned destination, std::uint64_t value) {
  Arm64Emitter emitter;
  emitter.load_constant(destination, value);
  const auto words = emitter.finish();
  return {words.begin(), words.end()};
}

std::size_t reservation_slot_for_testing(const void *host) noexcept {
  return semantic::reservation_slot_index(static_cast<const std::byte *>(host));
}

std::size_t successful_link_host_word_savings() noexcept {
  /*
   * The common basic-frame transition would reload x0/x1/x2 and unwind
   * before its indirect branch (five words including BR), then execute the
   * successor's external prologue. The persistent internal ABI needs only the
   * existing one-word BR.
   */
  constexpr std::size_t legacy_tail_words = 5;
  constexpr std::size_t external_prologue_words = 10;
  constexpr std::size_t internal_tail_words = 1;
  return legacy_tail_words + external_prologue_words - internal_tail_words;
}

std::size_t link_chain_instruction_limit() noexcept {
  return maximum_chain_instructions;
}

std::size_t link_capability_selector_count() noexcept {
  return jit_link_selector_count;
}

std::size_t block_link_record_size() noexcept {
  return sizeof(JitBlockLink);
}

std::size_t block_link_record_alignment() noexcept {
  return alignof(JitBlockLink);
}

std::size_t block_link_internal_entry_offset() noexcept {
  return offsetof(JitBlockLink, internal_entry);
}

std::size_t indirect_link_entry_size() noexcept {
  return sizeof(JitIndirectLinkEntry);
}

std::uint8_t
link_selector_for_capability_mask(std::uint8_t capabilities) noexcept {
  return link_selector_for_capabilities(capabilities);
}

std::uint8_t
checked_link_selector_for_capability_mask(std::uint8_t capabilities) noexcept {
  return checked_link_selector(capabilities);
}

std::uint8_t
link_capability_mask_for_selector(std::size_t selector) noexcept {
  if (selector >= jit_link_selector_capabilities.size())
    return invalid_link_selector;
  return jit_link_selector_capabilities[selector];
}

bool invalid_emitter_metadata_is_rejected_for_testing() {
  Arm64Emitter invalid_pair;
  invalid_pair.prologue();
  invalid_pair.load_pair_x(0, 1, 2, 3);
  invalid_pair.return_constant(Event::illegal_instruction, 0);
  if (!invalid_pair.finish().empty())
    return false;

  /* Zero-count terminal and dynamic returns are valid defensive
   * translation shapes and must remain representable without terminating
   * the translator. */
  Arm64Emitter terminal;
  terminal.prologue();
  terminal.return_constant(Event::illegal_instruction, 0);
  if (terminal.finish().empty())
    return false;
  Arm64Emitter dynamic;
  dynamic.prologue();
  dynamic.return_dynamic(0);
  return !dynamic.finish().empty();
}

bool block_link_table_is_bounded(std::size_t capacity) {
  JitBlockLinkTable table{capacity};
  if (capacity == 0)
    return table.find_or_create(1) == nullptr && table.size() == 0;
  auto *first = table.find_or_create(0x1000);
  if (first == nullptr || first->guest_pc != 0x1000)
    return false;
  first->internal_entry = 0x1234;
  first->allowed_selectors = 1U << 1U;
  for (std::size_t index = 1; index < capacity; ++index)
    if (table.find_or_create(0x1000 + index * 4U) == nullptr)
      return false;
  return table.size() == capacity && table.find(0x1000) == first &&
         table.find_or_create(0x1000) == first &&
         first->internal_entry == 0x1234 &&
         first->allowed_selectors == (1U << 1U) &&
         table.find_or_create(0x8000'0000) == nullptr &&
         table.size() == capacity;
}

bool indirect_link_table_is_bounded(std::size_t capacity) {
  JitBlockLink empty{.guest_pc = 1};
  JitIndirectLinkTable table{capacity, &empty};
  if (capacity == 0)
    return table.find_or_create(0x1000) == nullptr && table.size() == 0;
  auto *first = table.find_or_create(0x1000);
  if (first == nullptr || first->source_pc != 0x1000)
    return false;
  first->entries[0].guest_pc = 0x2000;
  first->entries[0].link = &empty;
  for (std::size_t index = 1; index < capacity; ++index)
    if (table.find_or_create(0x1000 + index * 4U) == nullptr)
      return false;
  return table.size() == capacity &&
         table.find_or_create(0x1000) == first &&
         first->entries[0].guest_pc == 0x2000 &&
         first->entries[0].link == &empty &&
         table.find_or_create(0x8000'0000) == nullptr &&
         table.size() == capacity;
}

ExclusiveMonitorState exclusive_monitor_state_for_testing() {
  ExclusiveMonitorState state;
  state.active_reservations =
      active_jit_reservations.load(std::memory_order_seq_cst);
  {
    std::scoped_lock lock{jit_reservations_mutex};
    state.reservations = jit_reservations.size();
  }
  {
    std::scoped_lock lock{jit_store_participants_mutex};
    state.store_participants = jit_store_participants.size();
  }
  state.gate_quiescent = jit_exclusive_gate_is_quiescent_for_testing();
  return state;
}

} // namespace jit::testing

} // namespace uml::cpu::arm64
