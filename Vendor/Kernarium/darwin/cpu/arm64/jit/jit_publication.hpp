// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "cpu/arm64/cpu_state.hpp"

#include <array>
#include <atomic>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace uml::cpu::arm64 {

enum class JitStoreState : unsigned {
  idle,
  checking,
  direct,
  tracked,
};

/*
 * A participant's observable store state lives in a slot rather than in the
 * participant, so that scanning it needs no lock (G132). The wait loop in
 * JitExclusiveOperationScope runs the scan on every iteration.
 *
 * The states are seq_cst on both sides, so no mutex is needed to order them;
 * the hazard is a scan dereferencing a destroyed participant. Slots remove it:
 * the scan reads stable storage that outlives every participant, a released
 * slot reads idle - which is exactly "no published store" - and no
 * participant is ever dereferenced. A participant stores only its own state,
 * so the lock-free direct-store path performs no shared read-modify-write.
 * Match the Apple arm64 cache line so adjacent vCPUs do not invalidate each
 * other's publication state on every native chain.
 */
struct alignas(128) JitStoreSlot final {
  std::atomic<JitStoreState> state{JitStoreState::idle};
  std::atomic<bool> claimed{false};
};
static_assert(alignof(JitStoreSlot) == 128 && sizeof(JitStoreSlot) == 128);

/* Slots are static so the scan never races an allocation. The bound is a
 * capacity, not an assumption: a participant that cannot claim one keeps its
 * own slot and joins an overflow list the scan consults under the mutex, so
 * exhaustion costs speed and never correctness. */
inline constexpr std::size_t jit_store_slot_capacity = 1024;
extern std::array<JitStoreSlot, jit_store_slot_capacity> jit_store_slots;
/* Only ever grows. A released slot reads idle, so scanning one costs a load
 * and cannot produce a false positive; shrinking it would race a participant
 * that is still publishing through a lower index. */
extern std::atomic<std::size_t> jit_store_slot_limit;
extern std::atomic<std::size_t> jit_store_overflow_count;

struct JitStoreParticipant final {
  JitStoreParticipant() = default;

  /* published_ points into this object until the participant claims a shared
   * slot, so a copy or a move would leave one of the two aliasing the other's
   * storage. Nothing needs to copy a participant; deleting these makes any
   * attempt a compile error rather than a dangling publication. */
  JitStoreParticipant(const JitStoreParticipant &) = delete;
  JitStoreParticipant &operator=(const JitStoreParticipant &) = delete;
  JitStoreParticipant(JitStoreParticipant &&) = delete;
  JitStoreParticipant &operator=(JitStoreParticipant &&) = delete;

  [[nodiscard]] std::atomic<JitStoreState> &state() const noexcept {
    return published_->state;
  }

  std::atomic<CpuState *> reservation_cpu{};

  /* Declared before published_ so it is constructed before its address is
   * taken. An unregistered participant publishes here and is invisible to the
   * scan. */
  JitStoreSlot local_{};
  /* Written only by the owning thread, in register/unregister, under
   * jit_store_participants_mutex. The owner reads it without the mutex on its
   * own store path. The slot scan reads the slot array, not this pointer;
   * another thread reads it only in the overflow scan, which holds the same
   * mutex, so the owner's store path needs no synchronisation. */
  JitStoreSlot *published_{&local_};
};

alignas(128) extern std::atomic<unsigned> active_jit_reservations;
extern std::mutex jit_store_participants_mutex;
extern std::vector<JitStoreParticipant *> jit_store_participants;
/*
 * Only the current reservation helper closes the store phase. Queued helpers
 * wait outside that phase, and stores parked by the current helper are allowed
 * to publish and drain before the next helper can enter. A parked store
 * therefore waits out only the current helper, never the helpers queued
 * behind it, and sleeps on the phase after a bounded spin.
 *
 * The uncontended direct-store path remains lock-free: it publishes its
 * participant state, reads the phase once, and proceeds. Stores already in
 * flight when a helper announces itself are covered by the participant scan.
 */
alignas(128) extern std::atomic<unsigned> jit_exclusive_operation_phase;
alignas(128) extern std::atomic<unsigned> jit_waiting_store_count_value;
alignas(128) extern std::atomic<bool> jit_exclusive_operation_turn;
constexpr unsigned jit_exclusive_operation_active = 1U;
constexpr unsigned jit_gate_spin_iterations = 64U;
[[nodiscard]] constexpr bool
jit_exclusive_operation_is_active(unsigned phase) noexcept {
  return phase != 0U;
}
template <typename T>
[[nodiscard]] bool jit_gate_value_changed(
    const std::atomic<T> &value, T expected) noexcept {
  for (unsigned iteration = 0; iteration < jit_gate_spin_iterations;
       ++iteration) {
    if (value.load(std::memory_order_relaxed) != expected)
      return true;
    __asm__ volatile("yield");
  }
  return value.load(std::memory_order_relaxed) != expected;
}
alignas(128) extern std::atomic<unsigned> jit_exclusive_operation_count;
extern std::mutex jit_reservations_mutex;
extern std::unordered_set<CpuState *> jit_reservations;
extern thread_local JitStoreParticipant *current_jit_store_participant;

[[nodiscard]] bool has_published_jit_store(
    const JitStoreParticipant *excluded) noexcept;

class JitExclusiveOperationScope final {
public:
  JitExclusiveOperationScope() noexcept;

  JitExclusiveOperationScope(const JitExclusiveOperationScope &) = delete;
  JitExclusiveOperationScope &
  operator=(const JitExclusiveOperationScope &) = delete;

  ~JitExclusiveOperationScope();
};

class JitBlockPublication final {
public:
  [[gnu::always_inline]] explicit JitBlockPublication(
      JitStoreParticipant &participant) noexcept
      : participant_{participant} {
    current_jit_store_participant = &participant_;
    participant_.state().store(JitStoreState::checking,
                             std::memory_order_seq_cst);
    auto phase =
        jit_exclusive_operation_phase.load(std::memory_order_seq_cst);
    if (jit_exclusive_operation_is_active(phase)) {
      participant_.state().store(JitStoreState::idle,
                               std::memory_order_seq_cst);
      waiting_ = true;
      jit_waiting_store_count_value.fetch_add(1,
                                               std::memory_order_seq_cst);
      do {
        while (jit_exclusive_operation_is_active(phase)) {
          if (!jit_gate_value_changed(jit_exclusive_operation_phase,
                                      phase))
            jit_exclusive_operation_phase.wait(
                phase, std::memory_order_seq_cst);
          phase =
              jit_exclusive_operation_phase.load(std::memory_order_seq_cst);
        }
        participant_.state().store(JitStoreState::checking,
                                 std::memory_order_seq_cst);
        phase =
            jit_exclusive_operation_phase.load(std::memory_order_seq_cst);
        if (jit_exclusive_operation_is_active(phase)) {
          participant_.state().store(JitStoreState::idle,
                                   std::memory_order_seq_cst);
        }
      } while (jit_exclusive_operation_is_active(phase));
    }
    /* Every vCPU's store tracks while any vCPU holds a reservation, the
     * holder's own included: its store to the reserved granule must fail
     * its store-exclusive, and one to any other granule must not. */
    tracked_ =
        active_jit_reservations.load(std::memory_order_seq_cst) != 0;
    participant_.state().store(tracked_ ? JitStoreState::tracked
                                      : JitStoreState::direct,
                             std::memory_order_seq_cst);
    if (waiting_) {
      jit_waiting_store_count_value.fetch_sub(1,
                                               std::memory_order_seq_cst);
      jit_waiting_store_count_value.notify_all();
    }
  }

  JitBlockPublication(const JitBlockPublication &) = delete;
  JitBlockPublication &operator=(const JitBlockPublication &) = delete;

  [[gnu::always_inline]] ~JitBlockPublication() {
    participant_.state().store(JitStoreState::idle, std::memory_order_seq_cst);
    current_jit_store_participant = nullptr;
  }

  [[nodiscard, gnu::always_inline]] bool needs_tracking() const noexcept {
    return tracked_;
  }

private:
  JitStoreParticipant &participant_;
  bool waiting_{};
  bool tracked_{};
};

void register_jit_store_participant(JitStoreParticipant &participant);
void unregister_jit_store_participant(JitStoreParticipant &participant);
[[nodiscard]] bool begin_jit_reservation(CpuState &cpu);
void end_jit_reservation(CpuState &cpu) noexcept;

} // namespace uml::cpu::arm64
