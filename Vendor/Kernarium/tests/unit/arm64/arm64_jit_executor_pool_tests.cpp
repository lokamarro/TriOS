// SPDX-License-Identifier: GPL-2.0-or-later

#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/arm64/jit/jit_executor_pool.hpp"
#include "cpu/engine_factory.hpp"
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "memory/static_guest_memory.hpp"
#include "require.hpp"
#include "wait_until.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <thread>

namespace {

namespace arm64 = ::uml::cpu::arm64;
using ::uml::cpu::CpuEngine;
using ::uml::cpu::EngineConfiguration;
using ::uml::cpu::EngineKind;
using ::uml::cpu::ExecutionRequest;
using ::uml::cpu::ExitReason;
using ::uml::emulator::tests::JitBacking;
using ::uml::emulator::tests::JitMapping;

constexpr std::uint64_t code_base = 0x6d00'0000;
constexpr std::uint64_t second_code = code_base + 0x100;
constexpr unsigned long first_generation = 0xc801;
constexpr unsigned long second_generation = 0xc802;
constexpr std::uint64_t first_value = 0x11;
constexpr std::uint64_t second_value = 0x22;
constexpr std::uint64_t same_generation_value = 0x33;
constexpr std::uint64_t preserved_register = 0xa55a'c33c'9669'5aa5ULL;
constexpr std::uint64_t initial_x8 = 0x1234'5678'9abc'def0ULL;
constexpr std::uint64_t initial_x9 = 0x0fed'cba9'8765'4321ULL;
constexpr std::uint32_t system_call = 0xd400'0001U;

using namespace std::chrono_literals;
using uml::testing::require;
using uml::testing::waitUntil;

struct RunObservation final {
  ExitReason reason{ExitReason::fatal};
  std::size_t retired{};
  std::uint64_t x0{};
  std::uint64_t x1{};
  std::uint64_t x8{};
  std::uint64_t x9{};
};

struct HeldBoundary final {
  std::atomic<bool> entered{};
  std::atomic<bool> proceed{};
};

bool hold_boundary(arm64::CpuState &, void *opaque) noexcept {
  auto &boundary = *static_cast<HeldBoundary *>(opaque);
  boundary.entered.store(true, std::memory_order_release);
  boundary.entered.notify_all();
  boundary.proceed.wait(false, std::memory_order_acquire);
  return true;
}

RunObservation run_program_at(CpuEngine &engine, unsigned long generation,
                              std::uint64_t pc,
                              ::uml::cpu::BoundaryHook boundary = nullptr,
                              void *boundary_context = nullptr) {
  arm64::CpuState cpu{};
  cpu.pc = pc;
  cpu.registers[1] = preserved_register;
  cpu.registers[8] = initial_x8;
  cpu.registers[9] = initial_x9;
  auto memory = std::make_unique<::uml::memory::StaticGuestMemory>();
  ExecutionRequest request{
      .cpu = cpu,
      .memory = *memory,
      .translated_memory = memory.get(),
      .generation = generation,
      .instruction_budget = 8,
      .boundary = boundary,
      .boundary_context = boundary_context,
  };
  const auto result = engine.run(request);
  return {.reason = result.reason,
          .retired = result.retired_instructions,
          .x0 = cpu.registers[0],
          .x1 = cpu.registers[1],
          .x8 = cpu.registers[8],
          .x9 = cpu.registers[9]};
}

RunObservation run_program(CpuEngine &engine, unsigned long generation,
                           ::uml::cpu::BoundaryHook boundary = nullptr,
                           void *boundary_context = nullptr) {
  return run_program_at(engine, generation, code_base, boundary,
                        boundary_context);
}

void require_success(const RunObservation &result, std::uint64_t value,
                     const char *message) {
  require(result.reason == ExitReason::system_call && result.retired == 4 &&
              result.x0 == value && result.x1 == preserved_register &&
              result.x8 == initial_x8 + 1U &&
              result.x9 == initial_x9 + 2U,
          message);
}

void release_boundary(HeldBoundary &boundary) noexcept {
  boundary.proceed.store(true, std::memory_order_release);
  boundary.proceed.notify_all();
}

void sequential_task_wrappers_reuse_one_across_states_and_generations() {
  require(arm64::acquire_jit_cache_run_session(),
          "could not acquire the sequential pool session");
  const auto session = arm64::active_jit_executor_pool_session();
  require(session != 0, "the admitted session did not publish a pool token");

  constexpr std::size_t wrapper_count = 6;
  constexpr std::array<unsigned long, wrapper_count> generations{
      first_generation, first_generation, first_generation,
      second_generation, first_generation, second_generation};
  constexpr std::array<std::uint64_t, wrapper_count> pcs{
      code_base, second_code, code_base,
      code_base, code_base, code_base};
  constexpr std::array<std::uint64_t, wrapper_count> values{
      first_value, same_generation_value, first_value,
      second_value, first_value, second_value};
  std::array<RunObservation, wrapper_count> results{};
  std::array<std::thread, wrapper_count> workers;
  std::atomic<std::size_t> ready{};
  std::atomic<std::size_t> turn{};
  std::atomic<bool> start{};
  std::atomic<bool> retire{};

  for (std::size_t index = 0; index < workers.size(); ++index) {
    workers[index] = std::thread{[&, index] {
      auto engine = ::uml::cpu::make_cpu_engine(EngineKind::jit);
      require(engine != nullptr, "could not create a pooled engine wrapper");
      ready.fetch_add(1, std::memory_order_release);
      ready.notify_all();
      start.wait(false, std::memory_order_acquire);
      auto observed = turn.load(std::memory_order_acquire);
      while (observed != index) {
        turn.wait(observed, std::memory_order_acquire);
        observed = turn.load(std::memory_order_acquire);
      }
      results[index] =
          run_program_at(*engine, generations[index], pcs[index]);
      turn.fetch_add(1, std::memory_order_release);
      turn.notify_all();
      retire.wait(false, std::memory_order_acquire);
    }};
  }
  require(waitUntil([&] {
    return ready.load(std::memory_order_acquire) == wrapper_count;
  }, 5s, 0ms), "persistent wrapper threads did not become ready");
  start.store(true, std::memory_order_release);
  start.notify_all();
  require(waitUntil([&] {
    return turn.load(std::memory_order_acquire) == wrapper_count;
  }, 5s, 0ms), "sequential wrapper runs did not finish");

  for (std::size_t index = 0; index < results.size(); ++index)
    require_success(results[index], values[index],
                    "cross-state/generation executor reuse changed guest state");
  const auto diagnostics = arm64::jit_executor_pool_diagnostics();
  require(diagnostics.session == session && diagnostics.capacity == 1 &&
              diagnostics.created == 1 && diagnostics.free == 1 &&
              diagnostics.borrowed == 0 && diagnostics.constructions == 1 &&
              diagnostics.borrows == wrapper_count,
          "more than one retained executor backed sequential task wrappers");

  retire.store(true, std::memory_order_release);
  retire.notify_all();
  for (auto &worker : workers)
    worker.join();
  arm64::release_jit_cache_run_session();
  const auto drained = arm64::jit_executor_pool_diagnostics();
  require(!drained.active && !drained.draining && drained.created == 0,
          "normal session release did not synchronously drain its executor");
}

void a_queued_waiter_precedes_a_tight_preferred_reborrow() {
  require(arm64::acquire_jit_cache_run_session(),
          "could not acquire the waiter-fairness pool session");
  const auto session = arm64::active_jit_executor_pool_session();
  require(session != 0 && arm64::bind_jit_executor_pool_configuration(
                              session, arm64::JitExecutorOptions{}),
          "could not bind the waiter-fairness pool configuration");

  auto holder =
      std::make_optional(arm64::borrow_jit_executor(session));
  require(static_cast<bool>(*holder),
          "could not borrow the waiter-fairness holder slot");
  const auto preferred_slot = holder->slot();
  std::atomic<unsigned> next_order{};
  std::atomic<unsigned> waiter_order{};
  std::thread waiter{[&] {
    auto borrowed = arm64::borrow_jit_executor(session, preferred_slot);
    require(static_cast<bool>(borrowed),
            "the queued fairness borrower failed");
    waiter_order.store(next_order.fetch_add(1, std::memory_order_relaxed) + 1U,
                       std::memory_order_release);
  }};
  require(waitUntil([] {
    return arm64::jit_executor_pool_diagnostics().waiters == 1;
  }, 5s, 0ms), "the fairness borrower did not queue behind the held slot");

  holder.reset();
  unsigned reborrow_order = 0;
  {
    auto reborrowed =
        arm64::borrow_jit_executor(session, preferred_slot);
    require(static_cast<bool>(reborrowed),
            "the tight preferred-slot reborrow failed");
    reborrow_order =
        next_order.fetch_add(1, std::memory_order_relaxed) + 1U;
  }
  waiter.join();
  require(waiter_order.load(std::memory_order_acquire) == 1U &&
              reborrow_order == 2U,
          "a tight preferred reborrow bypassed an already queued waiter");
  const auto diagnostics = arm64::jit_executor_pool_diagnostics();
  require(diagnostics.created == 1 && diagnostics.free == 1 &&
              diagnostics.waiters == 0 && diagnostics.constructions == 1 &&
              diagnostics.borrows == 3,
          "fair waiter handoff changed the retained one-slot bound");
  arm64::release_jit_cache_run_session();
}

void an_n_plus_one_task_waits_and_wakes_without_a_fatal_exit() {
  require(arm64::acquire_jit_cache_run_session(),
          "could not acquire the wait/wake pool session");
  auto first = ::uml::cpu::make_cpu_engine(EngineKind::jit);
  auto second = ::uml::cpu::make_cpu_engine(EngineKind::jit);
  require(first != nullptr && second != nullptr,
          "could not create concurrent pooled wrappers");
  HeldBoundary boundary;
  RunObservation first_result;
  RunObservation second_result;
  std::atomic<bool> second_started{};
  std::atomic<bool> second_finished{};

  std::thread holder{[&] {
    first_result = run_program(*first, first_generation,
                               &hold_boundary, &boundary);
  }};
  require(waitUntil([&] {
    return boundary.entered.load(std::memory_order_acquire);
  }, 5s, 0ms), "the one-slot holder did not reach its execution boundary");
  std::thread waiter{[&] {
    second_started.store(true, std::memory_order_release);
    second_started.notify_all();
    second_result = run_program(*second, second_generation);
    second_finished.store(true, std::memory_order_release);
    second_finished.notify_all();
  }};
  require(waitUntil([&] {
    return second_started.load(std::memory_order_acquire);
  }, 5s, 0ms), "the N+1 task did not start");
  require(waitUntil([] {
    return arm64::jit_executor_pool_diagnostics().waiters == 1;
  }, 5s, 0ms), "the N+1 task did not reach the bounded pool wait");
  require(!second_finished.load(std::memory_order_acquire),
          "the N+1 task crossed a concurrently borrowed one-slot pool");

  release_boundary(boundary);
  holder.join();
  waiter.join();
  require_success(first_result, first_value,
                  "the slot holder failed after waking its waiter");
  require_success(second_result, second_value,
                  "the N+1 task received the former lease-exhaustion fatal");
  const auto diagnostics = arm64::jit_executor_pool_diagnostics();
  require(diagnostics.created == 1 && diagnostics.free == 1 &&
              diagnostics.waiters == 0 && diagnostics.constructions == 1 &&
              diagnostics.borrows == 2,
          "wait/wake changed the one-slot retained executor bound");
  arm64::release_jit_cache_run_session();
}

void misordered_drain_is_safe_stale_and_recoverable() {
  require(arm64::acquire_jit_cache_run_session(),
          "could not acquire the misordered-drain session");
  const auto stale_session = arm64::active_jit_executor_pool_session();
  auto holder_engine = ::uml::cpu::make_cpu_engine(EngineKind::jit);
  auto waiter_engine = ::uml::cpu::make_cpu_engine(EngineKind::jit);
  require(holder_engine != nullptr && waiter_engine != nullptr,
          "could not create misordered-drain wrappers");
  HeldBoundary boundary;
  RunObservation holder_result;
  RunObservation waiter_result;
  std::atomic<bool> waiter_started{};

  std::thread holder{[&] {
    holder_result = run_program(*holder_engine, first_generation,
                                &hold_boundary, &boundary);
  }};
  require(waitUntil([&] {
    return boundary.entered.load(std::memory_order_acquire);
  }, 5s, 0ms), "the drain holder did not reach its execution boundary");
  std::thread waiter{[&] {
    waiter_started.store(true, std::memory_order_release);
    waiter_started.notify_all();
    waiter_result = run_program(*waiter_engine, second_generation);
  }};
  require(waitUntil([&] {
    return waiter_started.load(std::memory_order_acquire);
  }, 5s, 0ms), "the drain waiter did not start");
  require(waitUntil([] {
    return arm64::jit_executor_pool_diagnostics().waiters == 1;
  }, 5s, 0ms), "the drain test waiter did not reach pool exhaustion");

  arm64::release_jit_cache_run_session();
  waiter.join();
  require(waiter_result.reason == ExitReason::fatal &&
              waiter_result.retired == 0,
          "a queued borrower crossed session drain");
  require(!arm64::acquire_jit_cache_run_session(),
          "a new session crossed the explicitly lingering active borrower");
  const auto draining = arm64::jit_executor_pool_diagnostics();
  require(!draining.active && draining.draining &&
              draining.created == 1 && draining.borrowed == 1,
          "misordered drain released an actively borrowed executor");

  release_boundary(boundary);
  holder.join();
  require_success(holder_result, first_value,
                  "an active borrow did not survive safe deferred drain");
  require(waitUntil([] {
    const auto state = arm64::jit_executor_pool_diagnostics();
    return !state.active && !state.draining && state.created == 0;
  }, 5s, 0ms), "the final active borrow did not complete deferred teardown");

  require(arm64::acquire_jit_cache_run_session(),
          "the pool/session gate did not recover after deferred teardown");
  const auto fresh_session = arm64::active_jit_executor_pool_session();
  require(fresh_session != 0 && fresh_session != stale_session,
          "a recovered session reused a stale wrapper token");

  const auto stale = run_program(*holder_engine, first_generation);
  require(stale.reason == ExitReason::fatal && stale.retired == 0,
          "a stale wrapper attached to a later pool session");

  auto fresh = ::uml::cpu::make_cpu_engine(EngineKind::jit);
  require(fresh != nullptr, "could not create a recovered pooled wrapper");
  require_success(run_program(*fresh, second_generation), second_value,
                  "a fresh wrapper could not use the recovered pool");
  const auto require_incompatible = [&](EngineConfiguration configuration,
                                        const char *message) {
    const auto before = arm64::jit_executor_pool_diagnostics().borrows;
    auto incompatible =
        ::uml::cpu::make_cpu_engine(EngineKind::jit, configuration);
    require(incompatible != nullptr,
            "could not create an incompatible-profile wrapper");
    const auto rejected = run_program(*incompatible, first_generation);
    require(rejected.reason == ExitReason::fatal && rejected.retired == 0 &&
                arm64::jit_executor_pool_diagnostics().borrows == before,
            message);
  };
  auto capability_mismatch = EngineConfiguration{};
  capability_mismatch.host_capabilities.cssc =
      !capability_mismatch.host_capabilities.cssc;
  require_incompatible(
      capability_mismatch,
      "a host-capability mismatch borrowed another profile's executor");

  arm64::release_jit_cache_run_session();
  const auto drained = arm64::jit_executor_pool_diagnostics();
  require(!drained.active && !drained.draining && drained.created == 0,
          "recovered session teardown left a pooled executor alive");
}

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  if (!::uml::cpu::cpu_engine_available(EngineKind::jit))
    return EXIT_SUCCESS;

  const auto budget = arm64::minimum_jit_cache_budget(1);
  require(budget != 0 && arm64::set_jit_cache_budget(budget, 1),
          "could not publish the one-slot pool budget");

  JitBacking first_backing{1};
  first_backing.put32(0, 0xd280'0220U); // movz x0, #0x11
  first_backing.put32(4, 0x9100'0508U); // add x8, x8, #1
  first_backing.put32(8, 0x9100'0929U); // add x9, x9, #2
  first_backing.put32(12, system_call);
  first_backing.put32(0x100, 0xd280'0660U); // movz x0, #0x33
  first_backing.put32(0x104, 0x9100'0508U); // add x8, x8, #1
  first_backing.put32(0x108, 0x9100'0929U); // add x9, x9, #2
  first_backing.put32(0x10c, system_call);
  JitBacking second_backing{1};
  second_backing.put32(0, 0xd280'0440U); // movz x0, #0x22
  second_backing.put32(4, 0x9100'0508U); // add x8, x8, #1
  second_backing.put32(8, 0x9100'0929U); // add x9, x9, #2
  second_backing.put32(12, system_call);
  JitMapping first_mapping{first_generation, code_base, first_backing, 1};
  JitMapping second_mapping{second_generation, code_base, second_backing, 1};

  sequential_task_wrappers_reuse_one_across_states_and_generations();
  a_queued_waiter_precedes_a_tight_preferred_reborrow();
  an_n_plus_one_task_waits_and_wakes_without_a_fatal_exit();
  misordered_drain_is_safe_stale_and_recoverable();
  return EXIT_SUCCESS;
}
