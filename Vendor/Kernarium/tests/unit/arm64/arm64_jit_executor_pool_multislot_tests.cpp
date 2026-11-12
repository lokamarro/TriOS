// SPDX-License-Identifier: GPL-2.0-or-later

#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/arm64/jit/jit_executor_pool.hpp"
#include "require.hpp"
#include "wait_until.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <thread>

namespace {

namespace arm64 = ::uml::cpu::arm64;

constexpr unsigned pool_capacity = 4;
constexpr unsigned stress_thread_count = 8;
constexpr unsigned stress_iterations = 512;

using namespace std::chrono_literals;
using uml::testing::require;
using uml::testing::waitUntil;

using Borrow = arm64::JitExecutorPoolBorrow;
using HeldBorrow = std::optional<Borrow>;

struct ThreadPause final {
  std::atomic<bool> reached{};
  std::atomic<bool> resume{};
};

void pause_thread(void *opaque) noexcept {
  auto &pause = *static_cast<ThreadPause *>(opaque);
  pause.reached.store(true, std::memory_order_release);
  pause.reached.notify_all();
  pause.resume.wait(false, std::memory_order_acquire);
}

void construct_all_slots_lowest_first(
    arm64::JitExecutorPoolSessionToken session,
    std::array<HeldBorrow, pool_capacity> &holders,
    bool verify_creation = false) {
  for (unsigned slot = 0; slot < pool_capacity; ++slot) {
    holders[slot].emplace(arm64::borrow_jit_executor(session));
    require(static_cast<bool>(*holders[slot]),
            "could not construct every executor slot");
    require(holders[slot]->slot() == slot,
            "generic construction stopped being lowest-slot-first");
    if (verify_creation) {
      const auto diagnostics = arm64::jit_executor_pool_diagnostics();
      require(diagnostics.created == slot + 1U && diagnostics.free == 0 &&
                  diagnostics.borrowed == slot + 1U &&
                  diagnostics.constructions == slot + 1U,
              "lazy construction did not advance from zero through four");
    }
  }
}

void lowest_available_scan_remains_stable(
    arm64::JitExecutorPoolSessionToken session) {
  std::array<HeldBorrow, pool_capacity> holders;
  construct_all_slots_lowest_first(session, holders, true);
  for (auto &holder : holders)
    holder.reset();

  for (unsigned slot = 0; slot < pool_capacity; ++slot) {
    holders[slot].emplace(arm64::borrow_jit_executor(session));
    require(static_cast<bool>(*holders[slot]),
            "could not reclaim every available executor");
    require(holders[slot]->slot() == slot,
            "generic availability scan stopped being lowest-slot-first");
  }
  for (auto &holder : holders)
    holder.reset();

  const auto diagnostics = arm64::jit_executor_pool_diagnostics();
  require(diagnostics.created == pool_capacity &&
              diagnostics.free == pool_capacity &&
              diagnostics.borrowed == 0 && diagnostics.waiters == 0 &&
              diagnostics.constructions == pool_capacity,
          "four-slot construction or availability accounting changed");
}

void queued_borrowers_keep_fifo_priority_across_one_free_cell(
    arm64::JitExecutorPoolSessionToken session) {
  std::array<HeldBorrow, pool_capacity> holders;
  construct_all_slots_lowest_first(session, holders);
  constexpr unsigned released_slot = 2;
  constexpr unsigned waiter_count = 3;
  std::atomic<unsigned> next_order{};
  std::array<std::atomic<unsigned>, waiter_count> orders{};
  std::array<std::atomic<bool>, waiter_count> releases{};
  std::array<std::thread, waiter_count> waiters;

  for (unsigned index = 0; index < waiter_count; ++index) {
    waiters[index] = std::thread{[&, index] {
      auto borrowed = arm64::borrow_jit_executor(session, released_slot);
      require(static_cast<bool>(borrowed), "a FIFO borrower failed");
      require(borrowed.slot() == released_slot,
              "a FIFO borrower did not receive the only free cell");
      orders[index].store(
          next_order.fetch_add(1, std::memory_order_relaxed) + 1U,
          std::memory_order_release);
      orders[index].notify_all();
      releases[index].wait(false, std::memory_order_acquire);
    }};
    require(waitUntil([index] {
      return arm64::jit_executor_pool_diagnostics().waiters == index + 1U;
    }, 5s, 0ms), "FIFO borrowers did not queue in observed order");
  }

  std::atomic<unsigned> reborrow_order{};
  std::thread reborrow{[&] {
    auto borrowed = arm64::borrow_jit_executor(session, released_slot);
    require(static_cast<bool>(borrowed), "the tight preferred reborrow failed");
    require(borrowed.slot() == released_slot,
            "the tight reborrow did not receive the handed-off cell");
    reborrow_order.store(
        next_order.fetch_add(1, std::memory_order_relaxed) + 1U,
        std::memory_order_release);
    reborrow_order.notify_all();
  }};
  require(waitUntil([&] {
    return arm64::jit_executor_pool_diagnostics().waiters == waiter_count + 1U;
  }, 5s, 0ms), "the tight reborrow did not queue behind existing borrowers");

  holders[released_slot].reset();
  for (unsigned index = 0; index < waiter_count; ++index) {
    require(waitUntil([&, index] {
      return orders[index].load(std::memory_order_acquire) != 0;
    }, 5s, 0ms), "a FIFO borrower did not wake");
    require(orders[index].load(std::memory_order_acquire) == index + 1U &&
                reborrow_order.load(std::memory_order_acquire) == 0,
            "a queued borrower or tight reborrow bypassed FIFO order");
    releases[index].store(true, std::memory_order_release);
    releases[index].notify_all();
  }
  require(waitUntil([&] {
    return reborrow_order.load(std::memory_order_acquire) != 0;
  }, 5s, 0ms), "the tight reborrow did not receive the final handoff");
  require(reborrow_order.load(std::memory_order_acquire) == waiter_count + 1U,
          "the tight preferred reborrow bypassed a queued borrower");
  for (auto &waiter : waiters)
    waiter.join();
  reborrow.join();

  for (auto &holder : holders)
    holder.reset();
}

void concurrent_slot_transfers_never_duplicate_ownership(
    arm64::JitExecutorPoolSessionToken session) {
  std::array<std::atomic<unsigned>, pool_capacity> owners{};
  std::array<std::thread, stress_thread_count> workers;
  std::atomic<unsigned> ready{};
  std::atomic<bool> start{};

  for (unsigned worker = 0; worker < workers.size(); ++worker) {
    workers[worker] = std::thread{[&, worker] {
      auto preferred = worker % pool_capacity;
      ready.fetch_add(1, std::memory_order_release);
      ready.notify_all();
      start.wait(false, std::memory_order_acquire);
      for (unsigned iteration = 0; iteration < stress_iterations;
           ++iteration) {
        auto borrowed = arm64::borrow_jit_executor(session, preferred);
        require(static_cast<bool>(borrowed),
                "a concurrent multi-slot borrow failed");
        preferred = borrowed.slot();
        unsigned expected = 0;
        require(owners[preferred].compare_exchange_strong(
                    expected, worker + 1U, std::memory_order_acq_rel,
                    std::memory_order_acquire),
                "two borrowers simultaneously owned one executor cell");
        if ((iteration & 7U) == 0)
          std::this_thread::yield();
        owners[preferred].store(0, std::memory_order_release);
      }
    }};
  }
  require(waitUntil([&] {
    return ready.load(std::memory_order_acquire) == workers.size();
  }, 5s, 0ms), "multi-slot stress workers did not become ready");
  start.store(true, std::memory_order_release);
  start.notify_all();
  for (auto &worker : workers)
    worker.join();

  for (const auto &owner : owners)
    require(owner.load(std::memory_order_acquire) == 0,
            "multi-slot stress left a cell marked as owned");
  const auto diagnostics = arm64::jit_executor_pool_diagnostics();
  require(diagnostics.created == pool_capacity &&
              diagnostics.free == pool_capacity &&
              diagnostics.borrowed == 0 && diagnostics.waiters == 0,
          "concurrent multi-slot transfers changed pool accounting");
}

void return_cannot_notify_before_a_queued_borrower_waits(
    arm64::JitExecutorPoolSessionToken session) {
  std::array<HeldBorrow, pool_capacity> holders;
  construct_all_slots_lowest_first(session, holders);
  constexpr unsigned released_slot = 1;
  ThreadPause pause;
  std::atomic<bool> borrowed{};
  std::thread waiter{[&] {
    auto owner = arm64::borrow_jit_executor_before_wait_for_test(
        session, released_slot, pause_thread, &pause);
    require(static_cast<bool>(owner) && owner.slot() == released_slot,
            "the paused borrower did not receive the returned cell");
    borrowed.store(true, std::memory_order_release);
  }};
  require(waitUntil([&] {
    return pause.reached.load(std::memory_order_acquire);
  }, 5s, 0ms), "the borrower did not pause between its predicate and wait");

  std::atomic<bool> return_started{};
  std::atomic<bool> return_finished{};
  std::thread returning{[&] {
    return_started.store(true, std::memory_order_release);
    holders[released_slot].reset();
    return_finished.store(true, std::memory_order_release);
  }};
  require(waitUntil([&] {
    return return_started.load(std::memory_order_acquire);
  }, 5s, 0ms), "the concurrent executor return did not start");
  std::this_thread::sleep_for(std::chrono::milliseconds{20});
  const bool premature = return_finished.load(std::memory_order_acquire);
  pause.resume.store(true, std::memory_order_release);
  pause.resume.notify_all();
  require(waitUntil([&] {
    return borrowed.load(std::memory_order_acquire);
  }, 5s, 0ms), "an executor return was lost between the predicate and wait");
  waiter.join();
  returning.join();
  require(!premature,
          "a queued return notified before the borrower could begin waiting");
}

void drain_destroys_idle_slots_and_defers_active_slots(
    arm64::JitExecutorPoolSessionToken session) {
  auto first = std::make_optional(arm64::borrow_jit_executor(session, 0));
  auto second = std::make_optional(arm64::borrow_jit_executor(session, 2));
  require(static_cast<bool>(*first) && static_cast<bool>(*second) &&
              first->slot() != second->slot(),
          "could not retain two active slots for drain");

  arm64::release_jit_cache_run_session();
  const auto draining = arm64::jit_executor_pool_diagnostics();
  require(!draining.active && draining.draining &&
              draining.created == 2 && draining.free == 0 &&
              draining.borrowed == 2,
          "drain did not destroy exactly the two idle slots");
  require(!arm64::borrow_jit_executor(session),
          "a stale borrow crossed multi-slot drain");

  first.reset();
  const auto one_left = arm64::jit_executor_pool_diagnostics();
  require(one_left.draining && one_left.created == 1 &&
              one_left.borrowed == 1,
          "the first active return changed the other slot's lifetime");
  second.reset();
  require(waitUntil([] {
    const auto diagnostics = arm64::jit_executor_pool_diagnostics();
    return !diagnostics.active && !diagnostics.draining &&
           diagnostics.created == 0;
  }, 5s, 0ms), "the last active cell did not finish multi-slot drain");

  require(arm64::acquire_jit_cache_run_session(),
          "a fresh session could not follow deferred multi-slot drain");
  const auto fresh_session = arm64::active_jit_executor_pool_session();
  require(fresh_session != 0 && fresh_session != session &&
              arm64::bind_jit_executor_pool_configuration(
                  fresh_session, arm64::JitExecutorOptions{}),
          "multi-slot drain reused a stale session or configuration");
  {
    auto fresh = arm64::borrow_jit_executor(fresh_session);
    require(static_cast<bool>(fresh) && fresh.slot() == 0,
            "a fresh session could not reconstruct its first slot");
  }
  arm64::release_jit_cache_run_session();
  const auto recovered = arm64::jit_executor_pool_diagnostics();
  require(!recovered.active && !recovered.draining &&
              recovered.created == 0,
          "the recovered multi-slot session did not drain synchronously");
}

void stale_checked_borrow_cannot_claim_a_restarted_session() {
  require(arm64::acquire_jit_cache_run_session(),
          "could not acquire the stale-claim source session");
  const auto stale_session = arm64::active_jit_executor_pool_session();
  require(stale_session != 0 &&
              arm64::bind_jit_executor_pool_configuration(
                  stale_session, arm64::JitExecutorOptions{}),
          "could not bind the stale-claim source session");
  {
    auto idle = arm64::borrow_jit_executor(stale_session);
    require(static_cast<bool>(idle) && idle.slot() == 0,
            "could not publish the stale-claim source slot");
  }

  ThreadPause pause;
  std::atomic<bool> stale_succeeded{};
  std::atomic<bool> stale_finished{};
  std::thread stale{[&] {
    auto borrowed =
        arm64::borrow_jit_executor_after_session_check_for_test(
            stale_session, arm64::maximum_jit_cache_executors,
            pause_thread, &pause);
    stale_succeeded.store(static_cast<bool>(borrowed),
                          std::memory_order_release);
    stale_finished.store(true, std::memory_order_release);
    stale_finished.notify_all();
  }};
  require(waitUntil([&] {
    return pause.reached.load(std::memory_order_acquire);
  }, 5s, 0ms), "the stale borrower did not pause after its session check");

  arm64::release_jit_cache_run_session();
  const auto old_drained = arm64::jit_executor_pool_diagnostics();
  require(!old_drained.active && !old_drained.draining &&
              old_drained.created == 0,
          "the paused stale borrower prevented idle source-session drain");
  require(arm64::acquire_jit_cache_run_session(),
          "could not activate the replacement session");
  const auto replacement_session =
      arm64::active_jit_executor_pool_session();
  require(replacement_session != 0 &&
              replacement_session != stale_session &&
              arm64::bind_jit_executor_pool_configuration(
                  replacement_session, arm64::JitExecutorOptions{}),
          "could not bind a distinct replacement session");
  {
    auto replacement = arm64::borrow_jit_executor(replacement_session);
    require(static_cast<bool>(replacement) && replacement.slot() == 0,
            "could not publish the replacement session's first slot");
  }
  const auto replacement_idle = arm64::jit_executor_pool_diagnostics();
  require(replacement_idle.created == 1 && replacement_idle.free == 1 &&
              replacement_idle.borrowed == 0 &&
              replacement_idle.constructions == 1,
          "the replacement slot was not singly published before stale resume");

  pause.resume.store(true, std::memory_order_release);
  pause.resume.notify_all();
  require(waitUntil([&] {
    return stale_finished.load(std::memory_order_acquire);
  }, 5s, 0ms), "the stale borrower hung after replacement-session publication");
  stale.join();
  require(!stale_succeeded.load(std::memory_order_acquire),
          "a stale borrower claimed the replacement session's executor");

  const auto after_stale = arm64::jit_executor_pool_diagnostics();
  require(after_stale.session == replacement_session &&
              after_stale.created == 1 && after_stale.free == 1 &&
              after_stale.borrowed == 0 && after_stale.waiters == 0 &&
              after_stale.constructions == 1,
          "the stale borrower stole or leaked the replacement slot");
  {
    auto fresh = arm64::borrow_jit_executor(replacement_session);
    require(static_cast<bool>(fresh) && fresh.slot() == 0,
            "the replacement slot was unavailable after the stale claim");
  }
  arm64::release_jit_cache_run_session();
  require(arm64::acquire_jit_cache_run_session(),
          "a session could not follow the repaired stale-claim drain");
  const auto final_session = arm64::active_jit_executor_pool_session();
  require(final_session != 0 && final_session != replacement_session &&
              arm64::bind_jit_executor_pool_configuration(
                  final_session, arm64::JitExecutorOptions{}),
          "the repaired stale-claim path did not admit a fresh session");
  {
    auto final = arm64::borrow_jit_executor(final_session);
    require(static_cast<bool>(final) && final.slot() == 0,
            "the post-race session could not construct its first slot");
  }
  arm64::release_jit_cache_run_session();
}

void standalone_executor_cannot_enter_managed_activation() {
  ThreadPause pause;
  std::atomic<bool> activation_admitted{};
  std::atomic<bool> activation_finished{};
  std::thread activation{[&] {
    activation_admitted.store(
        arm64::acquire_jit_cache_run_session_after_reservation_for_test(
            pause_thread, &pause),
        std::memory_order_release);
    activation_finished.store(true, std::memory_order_release);
    activation_finished.notify_all();
  }};
  require(waitUntil([&] {
    return pause.reached.load(std::memory_order_acquire);
  }, 5s, 0ms), "managed activation did not pause after registry reservation");

  {
    arm64::JitExecutor standalone;
    require(!standalone.budget_admitted(),
            "a standalone executor entered the managed activation window");
  }
  pause.resume.store(true, std::memory_order_release);
  pause.resume.notify_all();
  require(waitUntil([&] {
    return activation_finished.load(std::memory_order_acquire);
  }, 5s, 0ms),
          "managed activation hung after standalone admission was rejected");
  activation.join();
  require(activation_admitted.load(std::memory_order_acquire),
          "managed activation failed after rejecting a standalone lease");

  const auto session = arm64::active_jit_executor_pool_session();
  require(session != 0 && arm64::bind_jit_executor_pool_configuration(
                              session, arm64::JitExecutorOptions{}),
          "the activation-window session could not bind its pool");
  {
    auto pooled = arm64::borrow_jit_executor(session);
    require(static_cast<bool>(pooled) && pooled.slot() == 0 &&
                pooled.executor().budget_admitted(),
            "the managed pool could not claim its reserved lease");
    arm64::JitExecutor standalone;
    require(!standalone.budget_admitted(),
            "a standalone executor entered a fully active managed session");
  }
  arm64::release_jit_cache_run_session();

  require(arm64::acquire_jit_cache_run_session(),
          "managed admission did not recover after activation-window drain");
  const auto recovered_session =
      arm64::active_jit_executor_pool_session();
  require(recovered_session != 0 && recovered_session != session &&
              arm64::bind_jit_executor_pool_configuration(
                  recovered_session, arm64::JitExecutorOptions{}),
          "the recovered managed session could not bind its pool");
  {
    auto pooled = arm64::borrow_jit_executor(recovered_session);
    require(static_cast<bool>(pooled) && pooled.slot() == 0,
            "the recovered managed pool could not construct an executor");
  }
  arm64::release_jit_cache_run_session();
}

} // namespace

int main() {
  if (!arm64::native_jit_available())
    return EXIT_SUCCESS;

  const auto budget = arm64::minimum_jit_cache_budget(pool_capacity);
  require(budget != 0 &&
              arm64::set_jit_cache_budget(budget, pool_capacity),
          "could not publish the four-slot pool budget");
  require(arm64::acquire_jit_cache_run_session(),
          "could not acquire the four-slot pool session");
  const auto session = arm64::active_jit_executor_pool_session();
  require(session != 0 && arm64::bind_jit_executor_pool_configuration(
                              session, arm64::JitExecutorOptions{}),
          "could not bind the four-slot pool configuration");

  lowest_available_scan_remains_stable(session);
  queued_borrowers_keep_fifo_priority_across_one_free_cell(session);
  return_cannot_notify_before_a_queued_borrower_waits(session);
  concurrent_slot_transfers_never_duplicate_ownership(session);
  drain_destroys_idle_slots_and_defers_active_slots(session);
  stale_checked_borrow_cannot_claim_a_restarted_session();
  standalone_executor_cannot_enter_managed_activation();
  return EXIT_SUCCESS;
}
