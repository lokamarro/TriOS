// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_executor_pool.hpp"

#include "jit_cache_geometry.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace uml::cpu::arm64 {
namespace {

[[nodiscard]] bool same_configuration(
    const JitExecutorOptions &first,
    const JitExecutorOptions &second) noexcept {
  return first.host_capabilities == second.host_capabilities;
}

[[nodiscard]] constexpr std::uint64_t bit_for(unsigned slot) noexcept {
  return std::uint64_t{1} << slot;
}

[[nodiscard]] constexpr std::uint64_t mask_for(unsigned capacity) noexcept {
  return capacity == maximum_jit_cache_executors
             ? ~std::uint64_t{}
             : (std::uint64_t{1} << capacity) - 1U;
}

struct ClaimedExecutor final {
  JitExecutor *executor{};
  unsigned slot{};
};

/* Apple arm64 hosts use 128-byte cache lines. Keep independent executor
 * handoffs off one process-global ownership line. A nonzero token publishes
 * one idle executor for exactly that session; zero means uncreated, borrowed,
 * or claimed by drain. Coupling availability and identity makes a stale
 * session's CAS unable to claim a later session's executor even if its owner
 * allocation reused the same address. */
struct alignas(128) AvailableExecutor final {
  std::atomic<JitExecutorPoolSessionToken> session{};
};
static_assert(alignof(AvailableExecutor) >= 128);
static_assert(sizeof(AvailableExecutor) % 128 == 0);
static_assert(
    std::atomic<JitExecutorPoolSessionToken>::is_always_lock_free);

} // namespace

class JitExecutorPool final {
public:
  [[nodiscard]] bool activate(unsigned capacity) noexcept {
    if (capacity == 0 || capacity > maximum_jit_cache_executors)
      return false;
    std::lock_guard lock{mutex_};
    if (active_ || draining_ || waiter_head_ != nullptr ||
        waiters_.load(std::memory_order_seq_cst) != 0 ||
        created_mask_.load(std::memory_order_acquire) != 0 ||
        next_session_ ==
            std::numeric_limits<JitExecutorPoolSessionToken>::max())
      return false;
    capacity_ = capacity;
    allowed_mask_ = mask_for(capacity);
    configuration_.reset();
    for (auto &available : available_)
      available.session.store(0, std::memory_order_relaxed);
    created_mask_.store(0, std::memory_order_relaxed);
#if UML_JIT_RUNTIME_DIAGNOSTICS
    constructions_ = 0;
    borrows_.store(0, std::memory_order_relaxed);
#endif
    active_ = true;
    const auto token = ++next_session_;
    active_session_.store(token, std::memory_order_seq_cst);
    return true;
  }

  void drain() noexcept {
    std::lock_guard lock{mutex_};
    if (!active_)
      return;
    active_ = false;
    draining_ = true;
    active_session_.store(0, std::memory_order_seq_cst);
    changed_.notify_all();

    auto created = created_mask_.load(std::memory_order_acquire);
    while (created != 0) {
      const auto bit = created & (~created + 1U);
      const auto slot = static_cast<unsigned>(std::countr_zero(bit));
      if (available_[slot].session.exchange(
              0, std::memory_order_seq_cst) != 0)
        destroy_slot_locked(slot, bit);
      created &= ~bit;
    }
    finish_drain_locked();
  }

  [[nodiscard]] JitExecutorPoolSessionToken active_session() const noexcept {
    return active_session_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool bind(JitExecutorPoolSessionToken session,
                          const JitExecutorOptions &options) noexcept {
    if (session == 0)
      return false;
    std::lock_guard lock{mutex_};
    if (!active_ || draining_ ||
        active_session_.load(std::memory_order_relaxed) != session)
      return false;
    if (!configuration_) {
      configuration_ = options;
      return true;
    }
    return same_configuration(*configuration_, options);
  }

  [[nodiscard]] ClaimedExecutor borrow(
      JitExecutorPoolSessionToken session, unsigned preferred_slot
#if UML_JIT_RUNTIME_DIAGNOSTICS
      ,
      JitExecutorPoolSessionCheckedHook checked_hook = nullptr,
      void *checked_hook_context = nullptr,
      JitExecutorPoolSessionCheckedHook wait_hook = nullptr,
      void *wait_hook_context = nullptr
#endif
      ) {
#if UML_JIT_RUNTIME_DIAGNOSTICS
    bool checked_hook_pending = checked_hook != nullptr;
#endif
    for (;;) {
      if (session == 0 ||
          active_session_.load(std::memory_order_acquire) != session)
        return {};
      if (waiters_.load(std::memory_order_seq_cst) == 0) {
#if UML_JIT_RUNTIME_DIAGNOSTICS
        if (checked_hook_pending) {
          checked_hook_pending = false;
          checked_hook(checked_hook_context);
        }
#endif
        const auto claimed = claim_free(session, preferred_slot);
        if (claimed.executor != nullptr) {
          /* Drain clears the active token before claiming each available
           * executor. If it raced this claim, the zero availability token keeps
           * the owner alive until return. A waiter that queued after the first
           * count check gets priority over this in-flight fast claim. */
          if (active_session_.load(std::memory_order_acquire) == session &&
              waiters_.load(std::memory_order_seq_cst) == 0) {
#if UML_JIT_RUNTIME_DIAGNOSTICS
            borrows_.fetch_add(1, std::memory_order_relaxed);
#endif
            return claimed;
          }
          release(claimed.slot, session, claimed.executor);
          if (active_session_.load(std::memory_order_acquire) != session)
            return {};
          continue;
        }
      }

      std::unique_lock lock{mutex_};
      if (!active_ || draining_ ||
          active_session_.load(std::memory_order_relaxed) != session)
        return {};
      if (waiter_head_ == nullptr) {
        if (const auto claimed = claim_free(session, preferred_slot);
            claimed.executor != nullptr) {
#if UML_JIT_RUNTIME_DIAGNOSTICS
          borrows_.fetch_add(1, std::memory_order_relaxed);
#endif
          return claimed;
        }

        const auto created = created_mask_.load(std::memory_order_acquire);
        const auto uncreated = allowed_mask_ & ~created;
        if (uncreated != 0) {
          if (!configuration_)
            return {};
          const auto bit = uncreated & (~uncreated + 1U);
          const auto slot = static_cast<unsigned>(std::countr_zero(bit));
          auto executor =
              JitExecutor::make_for_managed_pool(*configuration_);
          if (executor == nullptr || !executor->budget_admitted())
            return {};
          slots_[slot].owner = std::move(executor);
          slots_[slot].session.store(session, std::memory_order_relaxed);
          created_mask_.fetch_or(bit, std::memory_order_release);
#if UML_JIT_RUNTIME_DIAGNOSTICS
          ++constructions_;
          borrows_.fetch_add(1, std::memory_order_relaxed);
#endif
          return {slots_[slot].owner.get(), slot};
        }
      }

      Waiter waiter;
      enqueue_waiter_locked(waiter);
      try {
        for (;;) {
          changed_.wait(lock, [&] {
            const bool ready =
                active_session_.load(std::memory_order_acquire) != session ||
                (&waiter == waiter_head_ &&
                 has_free(std::memory_order_seq_cst));
#if UML_JIT_RUNTIME_DIAGNOSTICS
            if (!ready && wait_hook != nullptr) {
              const auto hook = std::exchange(wait_hook, nullptr);
              hook(wait_hook_context);
            }
#endif
            return ready;
          });
          if (active_session_.load(std::memory_order_acquire) != session) {
            remove_waiter_locked(waiter);
            finish_drain_locked();
            return {};
          }
          if (const auto claimed = claim_free(session, preferred_slot);
              claimed.executor != nullptr) {
            remove_waiter_locked(waiter);
            if (waiter_head_ != nullptr &&
                has_free(std::memory_order_acquire))
              changed_.notify_all();
#if UML_JIT_RUNTIME_DIAGNOSTICS
            borrows_.fetch_add(1, std::memory_order_relaxed);
#endif
            return claimed;
          }
        }
      } catch (...) {
        remove_waiter_locked(waiter);
        finish_drain_locked();
        changed_.notify_all();
        throw;
      }
    }
  }

  void release(unsigned slot, JitExecutorPoolSessionToken session,
               JitExecutor *executor) noexcept {
    if (slot >= maximum_jit_cache_executors || session == 0 ||
        executor == nullptr ||
        slots_[slot].session.load(std::memory_order_acquire) != session)
      return;
    const auto bit = bit_for(slot);
    /* Sequential consistency pairs this publication with enqueue's waiter
     * count: either return observes the waiter or a newly queued waiter sees
     * this session token. It also orders publication against drain's session
     * revocation and per-slot exchange. */
    available_[slot].session.store(session, std::memory_order_seq_cst);
    if (active_session_.load(std::memory_order_seq_cst) == session) {
      if (waiters_.load(std::memory_order_seq_cst) != 0) {
        /* Pair notification with the queue mutex so it cannot fall between
         * a false predicate and the condition variable's atomic unlock/wait.
         * Uncontended returns retain their lock-free publication path. */
        std::lock_guard lock{mutex_};
        changed_.notify_all();
      }
      return;
    }

    /* A drain may already have consumed this publication, or it may be
     * waiting for this return. A successor may also have claimed the same
     * executor before revocation became visible here. Reclaim only this
     * session's still-published token after revalidating its owner; a zero cell
     * belongs to that successor and must survive until its own return. */
    std::lock_guard lock{mutex_};
    if (slots_[slot].session.load(std::memory_order_relaxed) != session ||
        slots_[slot].owner.get() != executor)
      return;
    auto expected = session;
    if (!available_[slot].session.compare_exchange_strong(
            expected, 0, std::memory_order_seq_cst,
            std::memory_order_acquire)) {
      finish_drain_locked();
      return;
    }
    destroy_slot_locked(slot, bit);
    finish_drain_locked();
  }

#if UML_JIT_RUNTIME_DIAGNOSTICS
  [[nodiscard]] JitExecutorPoolDiagnostics diagnostics() noexcept {
    std::lock_guard lock{mutex_};
    const auto created = created_mask_.load(std::memory_order_acquire);
    const auto free = count_free(created, std::memory_order_acquire);
    return {
        .session = active_session_.load(std::memory_order_acquire),
        .capacity = capacity_,
        .created = static_cast<std::size_t>(std::popcount(created)),
        .free = free,
        .borrowed = static_cast<std::size_t>(std::popcount(created)) - free,
        .waiters = waiters_.load(std::memory_order_acquire),
        .constructions = constructions_,
        .borrows = borrows_.load(std::memory_order_acquire),
        .active = active_,
        .draining = draining_,
    };
  }
#endif

private:
  struct Waiter final {
    Waiter *previous{};
    Waiter *next{};
    bool queued{};
  };

  struct Slot final {
    std::atomic<JitExecutorPoolSessionToken> session{};
    std::unique_ptr<JitExecutor> owner;
  };

  [[nodiscard]] ClaimedExecutor claim_slot(
      unsigned slot, JitExecutorPoolSessionToken session) noexcept {
    auto expected = session;
    const bool claimed = available_[slot].session.compare_exchange_strong(
        expected, 0, std::memory_order_acq_rel,
        std::memory_order_acquire);
    if (!claimed)
      return {};
    return {slots_[slot].owner.get(), slot};
  }

  [[nodiscard]] ClaimedExecutor claim_free(
      JitExecutorPoolSessionToken session, unsigned preferred_slot) noexcept {
    if (preferred_slot < maximum_jit_cache_executors) {
      if (const auto claimed = claim_slot(preferred_slot, session);
          claimed.executor != nullptr)
        return claimed;
    }
    auto created = created_mask_.load(std::memory_order_acquire);
    while (created != 0) {
      const auto bit = created & (~created + 1U);
      const auto slot = static_cast<unsigned>(std::countr_zero(bit));
      if (const auto claimed = claim_slot(slot, session);
          claimed.executor != nullptr)
        return claimed;
      created &= ~bit;
    }
    return {};
  }

  [[nodiscard]] bool has_free(std::memory_order order) const noexcept {
    const auto created = created_mask_.load(std::memory_order_acquire);
    return count_free(created, order) != 0;
  }

  [[nodiscard]] std::size_t count_free(
      std::uint64_t created, std::memory_order order) const noexcept {
    std::size_t free = 0;
    while (created != 0) {
      const auto bit = created & (~created + 1U);
      const auto slot = static_cast<unsigned>(std::countr_zero(bit));
      free += static_cast<std::size_t>(
          available_[slot].session.load(order) != 0);
      created &= ~bit;
    }
    return free;
  }

  void destroy_slot_locked(unsigned slot, std::uint64_t bit) noexcept {
    slots_[slot].session.store(0, std::memory_order_release);
    slots_[slot].owner.reset();
    created_mask_.fetch_and(~bit, std::memory_order_release);
  }

  void enqueue_waiter_locked(Waiter &waiter) noexcept {
    /* Publish the admission gate before linking the stack-resident node, so
     * an in-flight fast borrower must yield even during this short mutation. */
    waiters_.fetch_add(1, std::memory_order_seq_cst);
    waiter.previous = waiter_tail_;
    waiter.next = nullptr;
    waiter.queued = true;
    if (waiter_tail_ != nullptr)
      waiter_tail_->next = &waiter;
    else
      waiter_head_ = &waiter;
    waiter_tail_ = &waiter;
  }

  void remove_waiter_locked(Waiter &waiter) noexcept {
    if (!waiter.queued)
      return;
    if (waiter.previous != nullptr)
      waiter.previous->next = waiter.next;
    else
      waiter_head_ = waiter.next;
    if (waiter.next != nullptr)
      waiter.next->previous = waiter.previous;
    else
      waiter_tail_ = waiter.previous;
    waiter.previous = nullptr;
    waiter.next = nullptr;
    waiter.queued = false;
    waiters_.fetch_sub(1, std::memory_order_seq_cst);
  }

  void finish_drain_locked() noexcept {
    if (!draining_ ||
        created_mask_.load(std::memory_order_acquire) != 0 ||
        waiter_head_ != nullptr)
      return;
    configuration_.reset();
    capacity_ = 0;
    allowed_mask_ = 0;
    draining_ = false;
    changed_.notify_all();
  }

  std::array<AvailableExecutor, maximum_jit_cache_executors> available_{};
  std::array<Slot, maximum_jit_cache_executors> slots_{};
  std::atomic<std::uint64_t> created_mask_{};
  std::atomic<JitExecutorPoolSessionToken> active_session_{};
  std::atomic<unsigned> waiters_{};
  std::mutex mutex_;
  std::condition_variable changed_;
  Waiter *waiter_head_{};
  Waiter *waiter_tail_{};
  std::optional<JitExecutorOptions> configuration_;
  std::uint64_t allowed_mask_{};
  JitExecutorPoolSessionToken next_session_{};
  unsigned capacity_{};
  bool active_{};
  bool draining_{};
#if UML_JIT_RUNTIME_DIAGNOSTICS
  std::uint64_t constructions_{};
  std::atomic<std::uint64_t> borrows_{};
#endif
};

namespace {

[[nodiscard]] JitExecutorPool &executor_pool() noexcept {
  static JitExecutorPool pool;
  return pool;
}

} // namespace

JitExecutorPoolBorrow::~JitExecutorPoolBorrow() {
  if (executor_ != nullptr)
    executor_pool().release(slot_, session_, executor_);
}

JitExecutorPoolBorrow::JitExecutorPoolBorrow(
    JitExecutorPoolBorrow &&other) noexcept
    : executor_{other.executor_}, slot_{other.slot_}, session_{other.session_} {
  other.executor_ = nullptr;
  other.slot_ = 0;
  other.session_ = 0;
}

bool activate_jit_executor_pool(unsigned capacity) noexcept {
  return executor_pool().activate(capacity);
}

void drain_jit_executor_pool() noexcept { executor_pool().drain(); }

JitExecutorPoolSessionToken active_jit_executor_pool_session() noexcept {
  return executor_pool().active_session();
}

bool bind_jit_executor_pool_configuration(
    JitExecutorPoolSessionToken session,
    const JitExecutorOptions &options) noexcept {
  return executor_pool().bind(session, options);
}

JitExecutorPoolBorrow borrow_jit_executor(
    JitExecutorPoolSessionToken session, unsigned preferred_slot) {
  const auto claimed = executor_pool().borrow(session, preferred_slot);
  if (claimed.executor == nullptr)
    return {};
  return {claimed.executor, claimed.slot, session};
}

#if UML_JIT_RUNTIME_DIAGNOSTICS
JitExecutorPoolDiagnostics jit_executor_pool_diagnostics() noexcept {
  return executor_pool().diagnostics();
}

JitExecutorPoolBorrow borrow_jit_executor_after_session_check_for_test(
    JitExecutorPoolSessionToken session, unsigned preferred_slot,
    JitExecutorPoolSessionCheckedHook hook, void *context) {
  const auto claimed =
      executor_pool().borrow(session, preferred_slot, hook, context);
  if (claimed.executor == nullptr)
    return {};
  return {claimed.executor, claimed.slot, session};
}

JitExecutorPoolBorrow borrow_jit_executor_before_wait_for_test(
    JitExecutorPoolSessionToken session, unsigned preferred_slot,
    JitExecutorPoolSessionCheckedHook hook, void *context) {
  const auto claimed = executor_pool().borrow(
      session, preferred_slot, nullptr, nullptr, hook, context);
  if (claimed.executor == nullptr)
    return {};
  return {claimed.executor, claimed.slot, session};
}
#endif

} // namespace uml::cpu::arm64
