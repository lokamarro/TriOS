// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_publication.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

namespace uml::cpu::arm64 {

alignas(128) std::atomic<unsigned> active_jit_reservations{};
std::array<JitStoreSlot, jit_store_slot_capacity> jit_store_slots{};
std::atomic<std::size_t> jit_store_slot_limit{};
std::atomic<std::size_t> jit_store_overflow_count{};
std::mutex jit_store_participants_mutex;
std::vector<JitStoreParticipant *> jit_store_participants;
alignas(128) std::atomic<unsigned> jit_exclusive_operation_phase{};
alignas(128) std::atomic<unsigned> jit_waiting_store_count_value{};
alignas(128) std::atomic<bool> jit_exclusive_operation_turn{};
alignas(128) std::atomic<unsigned> jit_exclusive_operation_count{};
std::mutex jit_reservations_mutex;
std::unordered_set<CpuState *> jit_reservations;
thread_local JitStoreParticipant *current_jit_store_participant{};

bool has_published_jit_store(
    const JitStoreParticipant *excluded) noexcept {
  /* The hot path: a bounded scan of stable storage with no lock taken. The
   * states are seq_cst on both sides, so they order against
   * jit_exclusive_operation_phase without help from a mutex, and a slot
   * outlives its participant, so the scan never dereferences a destroyed
   * one. */
  const auto *excluded_slot =
      excluded != nullptr ? excluded->published_ : nullptr;
  const auto limit = jit_store_slot_limit.load(std::memory_order_acquire);
  for (std::size_t index = 0; index < limit; ++index) {
    const auto *slot = &jit_store_slots[index];
    if (slot == excluded_slot)
      continue;
    if (slot->state.load(std::memory_order_seq_cst) != JitStoreState::idle)
      return true;
  }
  /* Only reached if the slot array was exhausted, which needs more than
   * jit_store_slot_capacity live participants. Those keep their own slot and
   * are scanned here under the mutex, so exhaustion degrades speed rather than
   * correctness. */
  if (jit_store_overflow_count.load(std::memory_order_acquire) == 0)
    return false;
  std::scoped_lock lock{jit_store_participants_mutex};
  return std::ranges::any_of(
      jit_store_participants,
      [excluded](const JitStoreParticipant *participant) {
        return participant != excluded &&
               participant->published_ == &participant->local_ &&
               participant->state().load(std::memory_order_seq_cst) !=
                   JitStoreState::idle;
      });
}

JitExclusiveOperationScope::JitExclusiveOperationScope() noexcept {
  jit_exclusive_operation_count.fetch_add(1, std::memory_order_seq_cst);

  bool expected = false;
  while (!jit_exclusive_operation_turn.compare_exchange_weak(
      expected, true, std::memory_order_acquire,
      std::memory_order_relaxed)) {
    if (!jit_gate_value_changed(jit_exclusive_operation_turn, true))
      jit_exclusive_operation_turn.wait(true, std::memory_order_relaxed);
    expected = false;
  }

  auto waiting =
      jit_waiting_store_count_value.load(std::memory_order_seq_cst);
  while (waiting != 0U) {
    if (!jit_gate_value_changed(jit_waiting_store_count_value, waiting))
      jit_waiting_store_count_value.wait(waiting,
                                         std::memory_order_seq_cst);
    waiting =
        jit_waiting_store_count_value.load(std::memory_order_seq_cst);
  }
  jit_exclusive_operation_phase.store(jit_exclusive_operation_active,
                                      std::memory_order_seq_cst);
  while (has_published_jit_store(current_jit_store_participant))
    __asm__ volatile("yield");
}

JitExclusiveOperationScope::~JitExclusiveOperationScope() {
  jit_exclusive_operation_phase.store(0U, std::memory_order_seq_cst);
  jit_exclusive_operation_phase.notify_all();
  jit_exclusive_operation_turn.store(false, std::memory_order_release);
  jit_exclusive_operation_turn.notify_one();
  jit_exclusive_operation_count.fetch_sub(1, std::memory_order_seq_cst);
}

void register_jit_store_participant(JitStoreParticipant &participant) {
  std::scoped_lock lock{jit_store_participants_mutex};
  /* Implementations register exactly once in their constructor. Treat a
   * repeated registration as the same lifetime instead of inserting a second
   * raw pointer which a single destructor could leave stale. */
  if (std::ranges::find(jit_store_participants, &participant) !=
      jit_store_participants.end())
    return;
  jit_store_participants.push_back(&participant);

  /* Claim the lowest free slot so the scan bound stays tight across a run that
   * registers and releases many participants over time. Registration is cold -
   * once per executor - so a linear search costs nothing that matters. */
  for (std::size_t index = 0; index < jit_store_slot_capacity; ++index) {
    bool expected = false;
    if (!jit_store_slots[index].claimed.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel,
            std::memory_order_relaxed))
      continue;
    jit_store_slots[index].state.store(JitStoreState::idle,
                                       std::memory_order_seq_cst);
    participant.published_ = &jit_store_slots[index];
    /* Publish the bound after the slot reads idle, so a scan that first
     * observes this index cannot see uninitialised state. The participant
     * cannot have published a store yet: it registers before it runs. */
    std::size_t limit = jit_store_slot_limit.load(std::memory_order_relaxed);
    while (limit <= index &&
           !jit_store_slot_limit.compare_exchange_weak(
               limit, index + 1, std::memory_order_release,
               std::memory_order_relaxed))
      ;
    return;
  }
  /* No slot left. The participant keeps publishing into its own storage and
   * the scan finds it through the overflow path. */
  jit_store_overflow_count.fetch_add(1, std::memory_order_release);
}

void unregister_jit_store_participant(JitStoreParticipant &participant) {
  std::scoped_lock lock{jit_store_participants_mutex};
  const auto found =
      std::ranges::find(jit_store_participants, &participant);
  if (found == jit_store_participants.end())
    return;
  jit_store_participants.erase(found);

  if (participant.published_ == &participant.local_) {
    jit_store_overflow_count.fetch_sub(1, std::memory_order_release);
    return;
  }
  /* Release the slot idle-first. A scan racing this sees either the
   * participant's final state or idle, and both mean the same thing once the
   * participant is gone. The slot itself is static, so an in-flight scan that
   * is already reading it stays inside live storage. */
  auto *slot = participant.published_;
  participant.published_ = &participant.local_;
  slot->state.store(JitStoreState::idle, std::memory_order_seq_cst);
  slot->claimed.store(false, std::memory_order_release);
}

bool begin_jit_reservation(CpuState &cpu) {
  bool inserted = false;
  {
    std::scoped_lock lock{jit_reservations_mutex};
    inserted = jit_reservations.insert(&cpu).second;
    if (inserted) {
      active_jit_reservations.fetch_add(1, std::memory_order_seq_cst);
      if (current_jit_store_participant != nullptr)
        current_jit_store_participant->reservation_cpu.store(
            &cpu, std::memory_order_seq_cst);
    }
  }
  return inserted;
}

void end_jit_reservation(CpuState &cpu) noexcept {
  std::scoped_lock lock{jit_reservations_mutex};
  if (jit_reservations.erase(&cpu) != 0)
    active_jit_reservations.fetch_sub(1, std::memory_order_seq_cst);
}

} // namespace uml::cpu::arm64
