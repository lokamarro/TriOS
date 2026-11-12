// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "jit_executor.hpp"

#include <cstddef>
#include <cstdint>

#ifndef UML_JIT_RUNTIME_DIAGNOSTICS
#define UML_JIT_RUNTIME_DIAGNOSTICS 0
#endif

namespace uml::cpu::arm64 {

using JitExecutorPoolSessionToken = std::uint64_t;

#if UML_JIT_RUNTIME_DIAGNOSTICS
using JitExecutorPoolSessionCheckedHook = void (*)(void *) noexcept;
#endif

/* A guest run retains at most one executor per configured vCPU, while the
 * runtime may retain lightweight engine wrappers for many more task-carrier
 * host threads. One managed run session belongs to one Context memory domain:
 * executors may follow tasks across CPU states, generations and backings
 * inside it, but are drained before that domain retires and never cross into a
 * later session/domain. A borrow owns one executor exclusively until this
 * object is destroyed. */
class JitExecutorPoolBorrow final {
public:
  JitExecutorPoolBorrow() noexcept = default;
  ~JitExecutorPoolBorrow();
  JitExecutorPoolBorrow(JitExecutorPoolBorrow &&other) noexcept;

  JitExecutorPoolBorrow(const JitExecutorPoolBorrow &) = delete;
  JitExecutorPoolBorrow &operator=(const JitExecutorPoolBorrow &) = delete;
  JitExecutorPoolBorrow &operator=(JitExecutorPoolBorrow &&) = delete;

  [[nodiscard]] explicit operator bool() const noexcept {
    return executor_ != nullptr;
  }
  [[nodiscard]] JitExecutor &executor() const noexcept { return *executor_; }
  [[nodiscard]] unsigned slot() const noexcept { return slot_; }

private:
  friend JitExecutorPoolBorrow borrow_jit_executor(
      JitExecutorPoolSessionToken, unsigned);
#if UML_JIT_RUNTIME_DIAGNOSTICS
  friend JitExecutorPoolBorrow
  borrow_jit_executor_after_session_check_for_test(
      JitExecutorPoolSessionToken, unsigned,
      JitExecutorPoolSessionCheckedHook, void *);
  friend JitExecutorPoolBorrow borrow_jit_executor_before_wait_for_test(
      JitExecutorPoolSessionToken, unsigned,
      JitExecutorPoolSessionCheckedHook, void *);
#endif

  JitExecutorPoolBorrow(JitExecutor *executor, unsigned slot,
                        JitExecutorPoolSessionToken session) noexcept
      : executor_{executor}, slot_{slot}, session_{session} {}

  JitExecutor *executor_{};
  unsigned slot_{};
  JitExecutorPoolSessionToken session_{};
};

/* These are called by the process-wide cache run-session owner. Activation
 * allocates no executor. Drain synchronously destroys every idle entry and
 * leaves only explicitly misordered active borrows to retire themselves. */
[[nodiscard]] bool activate_jit_executor_pool(unsigned capacity) noexcept;
void drain_jit_executor_pool() noexcept;

/* Wrappers capture this token once. A token never identifies a later session,
 * so a wrapper surviving a misordered teardown fails closed. */
[[nodiscard]] JitExecutorPoolSessionToken
active_jit_executor_pool_session() noexcept;

/* Configuration binding is a wrapper-construction operation, not a guest-entry
 * operation. The first wrapper fixes the session's exact code-generation
 * profile; incompatible wrappers are rejected before they can borrow code. */
[[nodiscard]] bool bind_jit_executor_pool_configuration(
    JitExecutorPoolSessionToken session,
    const JitExecutorOptions &options) noexcept;

/* preferred_slot is a wrapper's previous executor. With no queued borrower it
 * is attempted before the lowest-slot-first availability scan, preserving
 * cache affinity without making it ownership. Exhausted borrowers form a FIFO
 * handoff queue and take priority over a tight preferred-slot reborrow. */
[[nodiscard]] JitExecutorPoolBorrow borrow_jit_executor(
    JitExecutorPoolSessionToken session,
    unsigned preferred_slot = 64U);

#if UML_JIT_RUNTIME_DIAGNOSTICS
struct JitExecutorPoolDiagnostics final {
  JitExecutorPoolSessionToken session{};
  std::size_t capacity{};
  std::size_t created{};
  std::size_t free{};
  std::size_t borrowed{};
  std::size_t waiters{};
  std::uint64_t constructions{};
  std::uint64_t borrows{};
  bool active{};
  bool draining{};
};

[[nodiscard]] JitExecutorPoolDiagnostics
jit_executor_pool_diagnostics() noexcept;

/* Deterministically pauses a diagnostic borrow after its initial session and
 * waiter admission checks but before it touches an availability cell. */
[[nodiscard]] JitExecutorPoolBorrow
borrow_jit_executor_after_session_check_for_test(
    JitExecutorPoolSessionToken session, unsigned preferred_slot,
    JitExecutorPoolSessionCheckedHook hook, void *context);

/* Pauses once after a false wait predicate, while holding the queue mutex. */
[[nodiscard]] JitExecutorPoolBorrow borrow_jit_executor_before_wait_for_test(
    JitExecutorPoolSessionToken session, unsigned preferred_slot,
    JitExecutorPoolSessionCheckedHook hook, void *context);
#endif

} // namespace uml::cpu::arm64
