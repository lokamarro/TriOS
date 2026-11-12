// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_semantic_memory.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "platform/apple/host_features.hpp"

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sys/mman.h>
#include <thread>
#include <vector>

using namespace uml::emulator::tests;
using uml::testing::fail;

namespace {

constexpr std::uint32_t load_exclusive = 0xc85f'7c23U;  // ldxr x3, [x1]
constexpr std::uint32_t alias_store = 0xf900'00c2U;     // str x2, [x6]
constexpr std::uint32_t store_exclusive = 0xc805'7c24U; // stxr w5, x4, [x1]
constexpr std::uint32_t system_call = 0xd400'0001U;     // svc #0

void native_single_block_case() {
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  constexpr unsigned long generation = 10'099U;
  constexpr std::uint64_t base = 0x10f'0000U;
  constexpr std::uint64_t data = base + page;
  constexpr std::uint32_t add_one = 0x9100'0463U; // add x3, x3, #1
  constexpr std::uint32_t store_loaded =
      0xc805'7c23U; // stxr w5, x3, [x1]
  constexpr std::uint32_t retry = 0x35ff'ffa5U;   // cbnz w5, pc - 12

  JitBacking backing{2};
  backing.put32(0, load_exclusive);
  backing.put32(4, add_one);
  backing.put32(8, store_loaded);
  backing.put32(12, retry);
  backing.put64(page, 41);
  JitMapping mapping{generation, base, backing, 2};

  uml::cpu::arm64::JitExecutor executor;
  uml::memory::StaticGuestMemory memory;
  uml::cpu::arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[1] = data;
  require(executor.begin_execution(generation, cpu.pc));
  std::size_t executed = 0;
  require(executor.run_block(cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::continued);
  require(executed >= 4);
  require(cpu.registers[5] == 0);
  require(backing.get64(page) == 42);
  executor.end_execution();
}

void native_linked_loop_case() {
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  constexpr unsigned long generation = 10'098U;
  constexpr std::uint64_t base = 0x10e'0000U;
  constexpr std::uint64_t data = base + page;
  constexpr std::uint32_t add_one = 0x9100'0463U;
  constexpr std::uint32_t store_loaded = 0xc805'7c23U;
  constexpr std::uint32_t retry = 0x35ff'ffa5U;
  constexpr std::uint32_t decrement = 0xf100'0673U;
  constexpr std::uint32_t loop = 0x54ff'ff61U;

  JitBacking backing{2};
  backing.put32(0, load_exclusive);
  backing.put32(4, add_one);
  backing.put32(8, store_loaded);
  backing.put32(12, retry);
  backing.put32(16, decrement);
  backing.put32(20, loop);
  backing.put32(24, system_call);
  backing.put64(page, 0);
  JitMapping mapping{generation, base, backing, 2};

  uml::cpu::arm64::JitExecutor executor;
  uml::memory::StaticGuestMemory memory;
  uml::cpu::arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[1] = data;
  cpu.registers[19] = 500;
  require(executor.begin_execution(generation, cpu.pc));
  std::size_t executed = 0;
  auto event = ::uml::cpu::EngineEvent::continued;
  for (unsigned attempt = 0;
       attempt < 10U &&
       event == ::uml::cpu::EngineEvent::continued;
       ++attempt)
    event = executor.run_block(cpu, memory, generation, executed);
  require(event == ::uml::cpu::EngineEvent::system_call);
  require(cpu.registers[19] == 0);
  require(backing.get64(page) == 500);
  executor.end_execution();
}

void native_compare_retry_case(unsigned size, bool acquire_release,
                               bool matches, bool read_only = false) {
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  const auto generation =
      static_cast<unsigned long>(read_only ? 10'106U :
                                matches ? 10'096U : 10'097U) +
      32U * size + (acquire_release ? 16U : 0U);
  const auto base =
      std::uint64_t{0x10c'0000U} + (matches ? 0U : 4U * page);
  const auto data = base + page;
  const std::uint32_t compare =
      size == 3U ? 0xeb04'007fU : 0x6b04'007fU; // cmp x3/w3, x4/w4
  constexpr std::uint32_t skip = 0x5400'0061U;    // b.ne pc + 12
  const auto order = acquire_release ? 0x8000U : 0U;
  const auto load = 0x085f'7c23U | (size << 30U) | order;
  const auto store = 0x0805'7c22U | (size << 30U) | order;
  constexpr std::uint32_t retry = 0x35ff'ff85U; // cbnz w5, pc - 16
  constexpr std::uint64_t initial = 0x1020'3040'5060'7011ULL;
  constexpr std::uint64_t replacement = 0x8877'6655'4433'2222ULL;
  const auto mask = size == 3U ? ~std::uint64_t{} :
      (std::uint64_t{1} << (8U << size)) - 1U;
  const auto loaded = initial & mask;

  JitBacking backing{2};
  backing.put32(0, load);
  backing.put32(4, compare);
  backing.put32(8, skip);
  backing.put32(12, store);
  backing.put32(16, retry);
  backing.put32(20, system_call);
  backing.put64(page, initial);
  JitMapping mapping{generation, base, backing, 2};
  if (read_only)
    require(darwin_uml_static_vm_protect(
        reinterpret_cast<void *>(data), page, PROT_READ) == 0);

  uml::cpu::arm64::JitExecutor executor;
  uml::memory::StaticGuestMemory memory;
  uml::cpu::arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[1] = data;
  cpu.registers[2] = replacement;
  cpu.registers[4] = matches ? loaded : loaded + 1U;
  cpu.registers[5] = 7;
  require(executor.begin_execution(generation, cpu.pc));
  std::size_t total_executed = 0;
  auto event = ::uml::cpu::EngineEvent::continued;
  for (std::size_t attempt = 0;
       attempt < 10U &&
       event == ::uml::cpu::EngineEvent::continued;
       ++attempt) {
    std::size_t executed = 0;
    event = executor.run_block(cpu, memory, generation, executed);
    total_executed += executed;
  }
  require(event == ::uml::cpu::EngineEvent::system_call);
  if (matches) {
    // A host STXR may fail spuriously, including on its first writable-page
    // fault. Each complete retry still retires exactly five instructions.
    require(total_executed >= 6U && (total_executed - 6U) % 5U == 0U);
  } else {
    require(total_executed == 4U);
  }
  require(cpu.registers[3] == loaded);
  require(cpu.registers[5] == (matches ? 0U : 7U));
  require(backing.get64(page) ==
          (matches ? (initial & ~mask) | (replacement & mask) : initial));
  require(cpu.nzcv == (matches ? 0x6000'0000U : 0x8000'0000U));
  executor.end_execution();
}

void compare_lookahead_preserves_fetch_faults(bool matches) {
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  const unsigned long generation = matches ? 10'108U : 10'107U;
  constexpr std::uint64_t base = 0x130'0000U;
  JitBacking backing{1};
  backing.put64(0, 41);
  backing.put32(page - 64U, system_call);
  backing.put32(page - 12U, load_exclusive);
  backing.put32(page - 8U, 0xeb04'007fU); // cmp x3, x4
  backing.put32(page - 4U, 0x54ff'fe21U); // b.ne pc - 60
  JitMapping mapping{generation, base, backing, 1};
  uml::cpu::arm64::JitExecutor executor;
  uml::memory::StaticGuestMemory memory;
  uml::cpu::arm64::CpuState cpu{};
  cpu.pc = base + page - 12U;
  cpu.registers[1] = base;
  cpu.registers[4] = matches ? 41U : 42U;
  require(executor.begin_execution(generation, cpu.pc));
  auto event = ::uml::cpu::EngineEvent::continued;
  std::size_t retired = 0;
  for (unsigned attempt = 0; attempt < 10U &&
       event == ::uml::cpu::EngineEvent::continued; ++attempt) {
    std::size_t executed = 0;
    event = executor.run_block(cpu, memory, generation, executed);
    retired += executed;
  }
  require(cpu.registers[3] == 41U);
  if (matches) {
    require(event == ::uml::cpu::EngineEvent::memory_fault);
    require(retired == 3U && cpu.pc == base + page);
    require(executor.fault_address() == base + page);
    require(executor.fault_access() == uml::memory::Access::execute);
  } else {
    require(event == ::uml::cpu::EngineEvent::system_call);
    require(retired == 4U);
  }
  executor.end_execution();
}

/*
 * Four vCPUs raise one counter with a compare-and-swap loop. The loop at the
 * page's start is the fused sequence the JIT lowers to the host's own
 * monitor. With `mixed_semantic`, vCPU 0 runs a copy that begins one NOP
 * earlier and retries to it: a load-exclusive that does not begin a block is
 * left to the checked semantic helpers and their software reservation, so
 * the two kinds of vCPU meet on one location.
 */
void concurrent_compare_retry_case(bool mixed_semantic) {
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  const unsigned long generation = mixed_semantic ? 10'105U : 10'104U;
  constexpr std::uint64_t base = 0x124'0000U;
  constexpr auto data = base + page;
  constexpr std::uint64_t semantic_entry = base + 0x100U;
  constexpr std::size_t workers = 4;
  constexpr unsigned iterations = 1000;
  constexpr std::uint32_t nop = 0xd503'201fU;
  JitBacking backing{2};
  backing.put32(0, load_exclusive);
  backing.put32(4, 0xeb04'007fU); // cmp x3, x4
  backing.put32(8, 0x5400'0061U); // b.ne end
  backing.put32(12, 0xc805'7c22U); // stxr w5, x2, [x1]
  backing.put32(16, 0x35ff'ff85U); // cbnz w5, start
  backing.put32(20, system_call);
  backing.put32(0x100, nop);
  backing.put32(0x104, load_exclusive);
  backing.put32(0x108, 0xeb04'007fU); // cmp x3, x4
  backing.put32(0x10c, 0x5400'0061U); // b.ne end
  backing.put32(0x110, 0xc805'7c22U); // stxr w5, x2, [x1]
  backing.put32(0x114, 0x35ff'ff65U); // cbnz w5, the NOP
  backing.put32(0x118, system_call);
  backing.put64(page, 0);
  JitMapping mapping{generation, base, backing, 2};
  auto executors =
      std::make_unique<std::array<uml::cpu::arm64::JitExecutor, workers>>();
  auto memories =
      std::make_unique<std::array<uml::memory::StaticGuestMemory, workers>>();
  auto cpus = std::make_unique<
      std::array<uml::cpu::arm64::CpuState, workers>>();
  std::barrier start{workers};
  std::vector<std::thread> threads;
  for (std::size_t index = 0; index < workers; ++index) {
    threads.emplace_back([&, index] {
      auto &cpu = (*cpus)[index];
      auto &executor = (*executors)[index];
      const bool semantic = mixed_semantic && index == 0;
      // Instructions one pass of the loop retires; the run ends in a
      // system call after the last pass, or after a mismatch that skips the
      // store and the retry.
      const std::size_t pass = semantic ? 6U : 5U;
      cpu.registers[1] = data;
      require(executor.begin_execution(generation, base));
      start.arrive_and_wait();
      std::uint64_t expected = 0;
      unsigned successes = 0;
      while (successes < iterations) {
        cpu.pc = semantic ? semantic_entry : base;
        cpu.registers[2] = expected + 1;
        cpu.registers[4] = expected;
        cpu.registers[5] = 7;
        auto event = ::uml::cpu::EngineEvent::continued;
        std::size_t retired = 0;
        for (unsigned attempt = 0; attempt < 10000 &&
             event == ::uml::cpu::EngineEvent::continued; ++attempt) {
          std::size_t executed = 0;
          event = executor.run_block(cpu, (*memories)[index],
                                     generation, executed);
          retired += executed;
        }
        require(event == ::uml::cpu::EngineEvent::system_call);
        if (cpu.registers[5] == 0) {
          require(cpu.registers[3] == expected);
          require((cpu.nzcv & 0x4000'0000U) != 0);
          require(retired >= pass + 1 && (retired - pass - 1) % pass == 0);
          ++successes;
          expected = cpu.registers[2];
        } else {
          // A mismatch after a failed STXR retains its status of one;
          // a mismatch on the first load retains the sentinel of seven.
          // Both outcomes also occur in the same loop on native ARM64.
          require(cpu.registers[5] == 1 || cpu.registers[5] == 7);
          require(cpu.registers[3] > expected);
          require((cpu.nzcv & 0x4000'0000U) == 0);
          require(retired >= pass - 1 && (retired - (pass - 1)) % pass == 0);
          expected = cpu.registers[3];
        }
      }
      executor.end_execution();
    });
  }
  for (auto &thread : threads)
    thread.join();
  require(backing.get64(page) == workers * iterations);
}

enum class AliasStore : unsigned { plain, release };

/* Runs the store at `cpu`'s PC through the semantic memory helper, as the
 * translated store's slow edge does, and checks it retired alone. */
void semantic_store_through_helper(uml::cpu::arm64::CpuState &cpu,
                                   unsigned long generation,
                                   std::uint32_t instruction) {
  uml::memory::Tlb tlb;
  tlb.prepare(generation, darwin_uml_static_vm_changes(generation));
  const auto result =
      uml::cpu::arm64::uml_arm64_jit_execute_memory(&cpu, &tlb, instruction);
  require((result >> 32U) == 1U);
  require(static_cast<std::uint32_t>(result) ==
          static_cast<std::uint32_t>(::uml::cpu::EngineEvent::continued));
}

void same_backing_alias_case(bool overlaps, bool semantic_store = false,
                             AliasStore store = AliasStore::plain) {
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  const auto generation =
      static_cast<unsigned long>((overlaps ? 10'100U : 10'101U) +
                                 (semantic_store ? 20U : 0U) +
                                 40U * static_cast<unsigned>(store));
  const auto base =
      std::uint64_t{0x110'0000} + (overlaps ? 0U : 8U * page);
  const auto data = base + page;
  const auto alias = base + 4U * page;
  const auto store_offset = overlaps ? std::size_t{0} : std::size_t{32};
  constexpr std::uint64_t observed = 0x1122'3344'5566'7788ULL;
  constexpr std::uint64_t replacement = 0x8877'6655'4433'2211ULL;

  JitBacking backing{2};
  backing.put32(0, load_exclusive);
  backing.put32(4, system_call);
  backing.put32(8, alias_store);
  backing.put32(12, system_call);
  backing.put32(16, store_exclusive);
  backing.put32(20, system_call);
  backing.put64(page, observed);
  backing.put64(page + store_offset, observed);
  JitMapping mapping{generation, base, backing, 2};
  require(darwin_uml_static_vm_map(
              reinterpret_cast<void *>(alias), backing.descriptor(), page,
              page, PROT_READ | PROT_WRITE, 0, 1) == 0);

  uml::cpu::arm64::JitExecutor first;
  uml::cpu::arm64::JitExecutor second;
  uml::memory::StaticGuestMemory shared_memory;
  uml::cpu::arm64::CpuState first_cpu{};
  uml::cpu::arm64::CpuState second_cpu{};
  first_cpu.pc = base;
  first_cpu.registers[1] = data;
  first_cpu.registers[4] = replacement;
  second_cpu.pc = base + 8U;
  second_cpu.registers[2] = observed;
  second_cpu.registers[6] = alias + store_offset;

  require(first.begin_execution(generation, first_cpu.pc));
  std::size_t executed = 0;
  require(first.run_block(first_cpu, shared_memory, generation, executed) ==
          ::uml::cpu::EngineEvent::continued);
  require(executed == 1);
  require(first_cpu.exclusive_size == 8);
  require(first_cpu.registers[3] == observed);

  if (store == AliasStore::plain) {
    require(second.begin_execution(generation, second_cpu.pc));
    auto event = ::uml::cpu::EngineEvent::continued;
    std::size_t retired = 0;
    if (semantic_store) {
      // The helper the store's slow edge runs, which a tracked native store
      // does not reach; the system call then runs as its own block.
      semantic_store_through_helper(second_cpu, generation, alias_store);
      require(second_cpu.pc == base + 12U);
      retired = 1;
    }
    unsigned blocks = 0;
    for (; blocks < 4U && event == ::uml::cpu::EngineEvent::continued;
         ++blocks) {
      event = second.run_block(second_cpu, shared_memory, generation,
                               executed);
      retired += executed;
    }
    require(event == ::uml::cpu::EngineEvent::system_call);
    require(retired == 2);
    // The native store and system call share one translated block.
    require(blocks == 1);
    second.end_execution();
  } else {
    // Exercise the helper explicitly so native lowering cannot hide a stale
    // software reservation on an ordered instruction's cold path.
    uml::memory::Tlb tlb;
    tlb.prepare(generation, darwin_uml_static_vm_changes(generation));
    constexpr std::uint32_t instruction = 0xc89f'fcc2U; // stlr x2, [x6]
    const auto result = uml::cpu::arm64::uml_arm64_jit_execute_ordered(
        &second_cpu, &tlb, instruction);
    require((result >> 32U) == 1U);
    require(static_cast<std::uint32_t>(result) ==
            static_cast<std::uint32_t>(::uml::cpu::EngineEvent::continued));
    require(second_cpu.pc == base + 12U);
  }

  first_cpu.pc = base + 16U;
  require(first.run_block(first_cpu, shared_memory, generation, executed) ==
          ::uml::cpu::EngineEvent::continued);
  require(executed == 1);
  require(first_cpu.registers[5] == (overlaps ? 1U : 0U));
  require(first_cpu.exclusive_size == 0);
  require(backing.get64(page) == (overlaps ? observed : replacement));
  require(backing.get64(page + store_offset) == observed);
  first.end_execution();
}

void native_store_invalidates_semantic_reservation_case() {
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  constexpr unsigned long generation = 10'102U;
  constexpr std::uint64_t base = 0x118'0000U;
  constexpr std::uint64_t data = base + page;
  constexpr std::uint32_t add_one = 0x9100'0463U;
  constexpr std::uint32_t native_store = 0xc805'7c23U;
  constexpr std::uint64_t initial = 41U;

  JitBacking backing{2};
  backing.put32(0, load_exclusive);
  backing.put32(4, system_call);
  backing.put32(8, load_exclusive);
  backing.put32(12, add_one);
  backing.put32(16, native_store);
  backing.put32(20, system_call);
  backing.put32(24, store_exclusive);
  backing.put32(28, system_call);
  backing.put64(page, initial);
  JitMapping mapping{generation, base, backing, 2};

  uml::cpu::arm64::JitExecutor semantic_executor;
  uml::cpu::arm64::JitExecutor native_executor;
  uml::memory::StaticGuestMemory memory;
  uml::cpu::arm64::CpuState semantic_cpu{};
  uml::cpu::arm64::CpuState native_cpu{};
  semantic_cpu.pc = base;
  semantic_cpu.registers[1] = data;
  semantic_cpu.registers[4] = 99U;
  native_cpu.pc = base + 8U;
  native_cpu.registers[1] = data;

  require(semantic_executor.begin_execution(generation, semantic_cpu.pc));
  std::size_t executed = 0;
  require(semantic_executor.run_block(
              semantic_cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::continued);
  require(executed == 1U);
  require(semantic_cpu.exclusive_size == 8U);

  require(native_executor.begin_execution(generation, native_cpu.pc));
  auto native_event = ::uml::cpu::EngineEvent::continued;
  for (unsigned attempt = 0;
       attempt < 4U &&
       native_event == ::uml::cpu::EngineEvent::continued;
       ++attempt)
    native_event = native_executor.run_block(
        native_cpu, memory, generation, executed);
  require(native_event ==
          ::uml::cpu::EngineEvent::system_call);
  require(native_cpu.registers[5] == 0U);
  require(backing.get64(page) == initial + 1U);
  native_executor.end_execution();

  semantic_cpu.pc = base + 24U;
  require(semantic_executor.run_block(
              semantic_cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::continued);
  require(executed == 1U);
  require(semantic_cpu.registers[5] == 1U);
  require(semantic_cpu.exclusive_size == 0U);
  require(backing.get64(page) == initial + 1U);
  semantic_executor.end_execution();
}

/*
 * A pair or structure store that the JIT leaves to its slow helper, here
 * because it crosses a page, must invalidate another vCPU's software
 * reservation on the bytes it writes even when it stores the value already
 * there. Otherwise that vCPU's STXR succeeds over the intervening store.
 */
void slow_store_invalidates_semantic_reservation_case(bool structure) {
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  const unsigned long generation = structure ? 10'123U : 10'122U;
  const std::uint64_t base = structure ? 0x158'0000U : 0x150'0000U;
  const auto data = base + 2U * page;
  constexpr std::uint64_t observed = 0x1122'3344'5566'7788ULL;
  constexpr std::uint64_t replacement = 0x8877'6655'4433'2211ULL;
  constexpr std::uint32_t crossing_pair = 0xa900'08c2U; // stp x2, x2, [x6]
  constexpr std::uint32_t crossing_structure =
      0x4c00'70c0U; // st1 {v0.16b}, [x6]

  JitBacking backing{3};
  backing.put32(0, load_exclusive);
  backing.put32(4, system_call);
  backing.put32(8, structure ? crossing_structure : crossing_pair);
  backing.put32(12, system_call);
  backing.put32(16, store_exclusive);
  backing.put32(20, system_call);
  backing.put64(2U * page - 8U, observed);
  backing.put64(2U * page, observed);
  JitMapping mapping{generation, base, backing, 3};

  uml::cpu::arm64::JitExecutor semantic_executor;
  uml::cpu::arm64::JitExecutor store_executor;
  uml::memory::StaticGuestMemory memory;
  auto cpus = std::make_unique<
      std::array<uml::cpu::arm64::CpuState, 2>>();
  auto &semantic_cpu = (*cpus)[0];
  auto &store_cpu = (*cpus)[1];
  semantic_cpu.pc = base;
  semantic_cpu.registers[1] = data;
  semantic_cpu.registers[4] = replacement;
  store_cpu.pc = base + 8U;
  store_cpu.registers[2] = observed;
  store_cpu.registers[6] = data - 8U;
  std::memcpy(store_cpu.vectors[0].bytes.data(), &observed, 8U);
  std::memcpy(store_cpu.vectors[0].bytes.data() + 8U, &observed, 8U);

  require(semantic_executor.begin_execution(generation, semantic_cpu.pc));
  std::size_t executed = 0;
  require(semantic_executor.run_block(
              semantic_cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::continued);
  require(executed == 1U);
  require(semantic_cpu.exclusive_size == 8U);

  require(store_executor.begin_execution(generation, store_cpu.pc));
  auto store_event = ::uml::cpu::EngineEvent::continued;
  std::size_t retired = 0;
  for (unsigned attempt = 0;
       attempt < 4U &&
       store_event == ::uml::cpu::EngineEvent::continued;
       ++attempt) {
    store_event = store_executor.run_block(
        store_cpu, memory, generation, executed);
    retired += executed;
  }
  require(store_event == ::uml::cpu::EngineEvent::system_call);
  require(retired == 2U);
  require(backing.get64(2U * page - 8U) == observed);
  require(backing.get64(2U * page) == observed);
  store_executor.end_execution();

  semantic_cpu.pc = base + 16U;
  require(semantic_executor.run_block(
              semantic_cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::continued);
  require(executed == 1U);
  require(semantic_cpu.registers[5] == 1U);
  require(semantic_cpu.exclusive_size == 0U);
  require(backing.get64(2U * page) == observed);
  semantic_executor.end_execution();
}

void concurrent_native_cold_paths_leave_publication_before_helper() {
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  constexpr unsigned long generation = 10'103U;
  constexpr std::uint64_t base = 0x11c'0000U;
  constexpr std::uint64_t data = base + page;
  constexpr std::size_t workers = 10U;
  constexpr std::uint32_t add_one = 0x9100'0463U;
  constexpr std::uint32_t store_loaded = 0xc805'7c23U;
  constexpr std::uint32_t retry = 0x35ff'ffa5U;

  JitBacking backing{2};
  backing.put32(0, load_exclusive);
  backing.put32(4, add_one);
  backing.put32(8, store_loaded);
  backing.put32(12, retry);
  for (std::size_t index = 0; index < workers; ++index)
    backing.put64(page + index * sizeof(std::uint64_t), index);
  JitMapping mapping{generation, base, backing, 2};
  // A page the guest can read and not write, which the native sequence's
  // write translation refuses.
  constexpr std::uint64_t read_only_page = base + 2U * page;
  JitBacking read_only{1};
  require(darwin_uml_static_vm_map(
              reinterpret_cast<void *>(read_only_page), read_only.descriptor(),
              0, page, PROT_READ, 0, 0) == 0);

  auto executors =
      std::make_unique<std::array<uml::cpu::arm64::JitExecutor, workers>>();
  auto memories =
      std::make_unique<std::array<uml::memory::StaticGuestMemory, workers>>();
  auto cpus = std::make_unique<
      std::array<uml::cpu::arm64::CpuState, workers>>();
  /*
   * Compile and warm each native sequence before the synchronized run. A
   * pointer into the read-only page then forces every cached sequence down
   * the helper cold edge, where its load-exclusive needs only read access,
   * without allowing translation latency to serialize the test.
   */
  for (std::size_t index = 0; index < workers; ++index) {
    auto &cpu = (*cpus)[index];
    cpu.pc = base;
    cpu.registers[1] = data + index * sizeof(std::uint64_t);
    require((*executors)[index].begin_execution(generation, cpu.pc));
    std::size_t executed = 0;
    require((*executors)[index].run_block(
                cpu, (*memories)[index], generation, executed) ==
            ::uml::cpu::EngineEvent::continued);
    require(executed >= 4U);
    (*executors)[index].end_execution();
    cpu.pc = base;
    cpu.registers[1] = read_only_page + index * sizeof(std::uint64_t);
  }

  std::barrier start{workers};
  std::atomic<bool> valid{true};
  std::vector<std::thread> threads;
  threads.reserve(workers);
  for (std::size_t index = 0; index < workers; ++index) {
    threads.emplace_back([&, index] {
      auto &cpu = (*cpus)[index];
      auto &executor = (*executors)[index];
      if (!executor.begin_execution(generation, cpu.pc)) {
        valid.store(false, std::memory_order_relaxed);
        start.arrive_and_drop();
        return;
      }
      start.arrive_and_wait();
      std::size_t executed = 0;
      const auto event =
          executor.run_block(
              cpu, (*memories)[index], generation, executed);
      if (event != ::uml::cpu::EngineEvent::continued ||
          executed != 1U || cpu.pc != base + sizeof(std::uint32_t))
        valid.store(false, std::memory_order_relaxed);
      executor.end_execution();
    });
  }
  for (auto &thread : threads)
    thread.join();
  require(valid.load(std::memory_order_relaxed));
}

/*
 * A compare-and-swap the JIT leaves to its semantic helper must be one host
 * atomic. vCPU A runs a helper-routed compare-and-swap loop that increments
 * one field; vCPU B runs a native LDADD loop on bytes the swap also covers,
 * concurrently. A read-compare-write that is not atomic writes back B's old
 * bytes and loses its increments; each field must end at its own count.
 *
 * Only an overlap proves that: while B is starved no swap meets an add, and
 * both fields match trivially. The host scheduler promises B no time inside
 * A's loop, so a trial measures its overlap instead of hoping for it. B
 * publishes its add count after every block it retires, and A counts the
 * blocks it ran across which that count advanced; in each, both vCPUs were
 * executing. A trial checks every invariant, but it counts only with at least
 * `required_overlaps` such blocks. One that falls short is repeated on fresh
 * state until one counts, and a deadline that only a wedged host reaches
 * fails the test.
 */
struct HelperSwapRace final {
  unsigned long generation;
  std::uint64_t base;
  std::array<std::uint32_t, 12> swap_loop;
  std::size_t swap_words;
  std::size_t swap_offset;
  std::array<std::uint32_t, 4> add_loop;
  std::size_t add_offset;
  std::uint64_t swap_mask;
  unsigned add_shift;
  std::uint64_t add_mask;
};

void helper_swap_keeps_native_updates(const HelperSwapRace &race) {
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  const auto data = race.base + page;
  constexpr std::uint64_t add_code = 0x100U;
  constexpr unsigned swaps = 20000;
  constexpr std::uint64_t unbounded = std::uint64_t{1} << 40;
  constexpr std::uint64_t required_overlaps = 4096;
  constexpr std::uint64_t deadline_poll = 4096;
  constexpr std::chrono::seconds deadline_span{180};

  JitBacking backing{2};
  for (std::size_t index = 0; index < race.swap_words; ++index)
    backing.put32(index * 4U, race.swap_loop[index]);
  for (std::size_t index = 0; index < race.add_loop.size(); ++index)
    backing.put32(add_code + index * 4U, race.add_loop[index]);
  JitMapping mapping{race.generation, race.base, backing, 2};

  uml::cpu::arm64::JitExecutor swap_executor;
  uml::cpu::arm64::JitExecutor add_executor;
  uml::memory::StaticGuestMemory swap_memory;
  uml::memory::StaticGuestMemory add_memory;
  auto cpus = std::make_unique<
      std::array<uml::cpu::arm64::CpuState, 2>>();
  auto &swap_cpu = (*cpus)[0];
  auto &add_cpu = (*cpus)[1];

  const auto deadline =
      std::chrono::steady_clock::now() + deadline_span;
  const auto expired = [&] {
    return std::chrono::steady_clock::now() >= deadline;
  };

  /* Runs A's loop against B once from fresh state and returns the number of
   * A's blocks that overlapped a block B retired. */
  const auto trial = [&]() -> std::uint64_t {
    backing.put64(page, 0);
    backing.put64(page + 8U, 0);
    swap_cpu = {};
    add_cpu = {};
    swap_cpu.pc = race.base;
    swap_cpu.registers[1] = data + race.swap_offset;
    swap_cpu.registers[19] = swaps;
    add_cpu.pc = race.base + add_code;
    add_cpu.registers[8] = data + race.add_offset;
    add_cpu.registers[9] = 1;
    add_cpu.registers[19] = unbounded;

    std::atomic<std::uint64_t> progress{0};
    std::atomic<bool> swapping{true};
    std::atomic<bool> valid{true};
    std::barrier start{2};
    std::thread adder{[&] {
      if (!add_executor.begin_execution(race.generation, add_cpu.pc))
        valid.store(false, std::memory_order_relaxed);
      start.arrive_and_wait();
      std::size_t executed = 0;
      while (swapping.load(std::memory_order_acquire) &&
             valid.load(std::memory_order_relaxed)) {
        if (add_executor.run_block(add_cpu, add_memory, race.generation,
                                   executed) !=
            ::uml::cpu::EngineEvent::continued)
          valid.store(false, std::memory_order_relaxed);
        progress.store(unbounded - add_cpu.registers[19],
                       std::memory_order_relaxed);
      }
      add_executor.end_execution();
    }};
    if (!swap_executor.begin_execution(race.generation, swap_cpu.pc))
      valid.store(false, std::memory_order_relaxed);
    start.arrive_and_wait();
    /* Do not start swapping against a B that has not run yet. */
    while (progress.load(std::memory_order_relaxed) == 0 &&
           valid.load(std::memory_order_relaxed)) {
      if (expired())
        fail("the adder vCPU never ran");
      std::this_thread::yield();
    }

    auto event = ::uml::cpu::EngineEvent::continued;
    std::size_t executed = 0;
    std::uint64_t overlaps = 0;
    auto seen = progress.load(std::memory_order_relaxed);
    for (std::uint64_t attempt = 0;
         event == ::uml::cpu::EngineEvent::continued; ++attempt) {
      event = swap_executor.run_block(swap_cpu, swap_memory,
                                      race.generation, executed);
      const auto now = progress.load(std::memory_order_relaxed);
      overlaps += now != seen;
      seen = now;
      if (attempt % deadline_poll == 0 && expired())
        fail("the swap loop did not finish before the deadline");
    }
    swapping.store(false, std::memory_order_release);
    adder.join();
    swap_executor.end_execution();
    require(valid.load(std::memory_order_relaxed));
    require(event == ::uml::cpu::EngineEvent::system_call);
    require(swap_cpu.registers[19] == 0);

    const auto adds = unbounded - add_cpu.registers[19];
    const auto low = backing.get64(page);
    const auto high = backing.get64(page + 8U);
    const unsigned __int128 memory =
        (static_cast<unsigned __int128>(high) << 64U) | low;
    const auto swapped =
        static_cast<std::uint64_t>(memory >> (race.swap_offset * 8U)) &
        race.swap_mask;
    const auto added =
        static_cast<std::uint64_t>(memory >> race.add_shift) & race.add_mask;
    require(swapped == (swaps & race.swap_mask));
    require(added == (adds & race.add_mask));
    return overlaps;
  };

  while (trial() < required_overlaps)
    if (expired())
      fail("the swap and add vCPUs never overlapped before the deadline");
}

void helper_pair_compare_swap_keeps_native_updates() {
  /* CASP always takes the helper. A increments the low doubleword with
   * CASP; B's native LDADD increments the high one. */
  helper_swap_keeps_native_updates({
      .generation = 10'120U,
      .base = 0x140'0000U,
      .swap_loop = {0xa940'0c22U,   // ldp x2, x3, [x1]
                    0x9100'0444U,   // add x4, x2, #1
                    0xaa03'03e5U,   // mov x5, x3
                    0xaa02'03e6U,   // mov x6, x2
                    0xaa03'03e7U,   // mov x7, x3
                    0x4822'7c24U,   // casp x2, x3, x4, x5, [x1]
                    0xeb06'005fU,   // cmp x2, x6
                    0xfa47'0060U,   // ccmp x3, x7, #0, eq
                    0x54ff'ff01U,   // b.ne start
                    0xf100'0673U,   // subs x19, x19, #1
                    0x54ff'fec1U,   // b.ne start
                    system_call},
      .swap_words = 12,
      .swap_offset = 0,
      .add_loop = {0xf829'010aU,    // ldadd x9, x10, [x8]
                   0xf100'0673U,    // subs x19, x19, #1
                   0x54ff'ffc1U,    // b.ne start
                   system_call},
      .add_offset = 8,
      .swap_mask = ~std::uint64_t{},
      .add_shift = 64,
      .add_mask = ~std::uint64_t{},
  });
}

void helper_misaligned_compare_swap_keeps_native_updates() {
  /* With FEAT_LSE2 a CAS misaligned inside one 16-byte block completes, and
   * the JIT leaves it to the helper. A's word covers bytes 2-5; B's native
   * LDADDH increments bytes 4-5, the upper half of that word. A's own
   * increments stay in bytes 2-3 because it swaps fewer than 65536 times. */
  if (!::uml::platform::apple::probed_host_capabilities().has(
          ::uml::cpu::ArmFeature::lse2))
    return;
  helper_swap_keeps_native_updates({
      .generation = 10'121U,
      .base = 0x148'0000U,
      .swap_loop = {0xb940'0022U,   // ldr w2, [x1]
                    0x1100'0444U,   // add w4, w2, #1
                    0x2a02'03e6U,   // mov w6, w2
                    0x88a2'7c24U,   // cas w2, w4, [x1]
                    0x6b06'005fU,   // cmp w2, w6
                    0x54ff'ff61U,   // b.ne start
                    0xf100'0673U,   // subs x19, x19, #1
                    0x54ff'ff21U,   // b.ne start
                    system_call},
      .swap_words = 9,
      .swap_offset = 2,
      .add_loop = {0x7829'010aU,    // ldaddh w9, w10, [x8]
                   0xf100'0673U,    // subs x19, x19, #1
                   0x54ff'ffc1U,    // b.ne start
                   system_call},
      .add_offset = 4,
      .swap_mask = 0xffffU,
      .add_shift = 32,
      .add_mask = 0xffffU,
  });
}


/*
 * A store of the reserving vCPU's own, between its LDXR and STXR, keeps the
 * reservation unless it writes the reserved granule. Reservation versions
 * live in hashed slots, so the store may land on another granule of the
 * reserved granule's slot; that must keep the reservation too, or a loop
 * whose spill and lock collide would fail its STXR on every retry.
 *
 * The other granule is found with the production slot function, in a mapping
 * large enough that one exists whatever the host addresses are: any three
 * aligned runs of 32768 granules hold two, and the reserved one is not both.
 */
enum class OwnStore { native, semantic };

void own_store_keeps_reservation_case(OwnStore path, bool same_granule,
                                      bool colliding) {
  constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
  const unsigned long generation = 10'130U +
      (path == OwnStore::semantic ? 4U : 0U) + (same_granule ? 2U : 0U) +
      (colliding ? 1U : 0U);
  constexpr std::uint64_t base = 0x400'0000U;
  constexpr std::size_t pages = 400;
  const auto data = base + page;
  constexpr std::uint64_t observed = 0x1122'3344'5566'7788ULL;
  constexpr std::uint64_t replacement = 0x8877'6655'4433'2211ULL;

  JitBacking backing{pages};
  backing.put32(0, load_exclusive);
  backing.put32(4, system_call);
  backing.put32(8, alias_store);
  backing.put32(12, system_call);
  backing.put32(16, store_exclusive);
  backing.put32(20, system_call);
  backing.put64(page, observed);
  JitMapping mapping{generation, base, backing, pages};

  uml::memory::Tlb tlb;
  tlb.prepare(generation, darwin_uml_static_vm_changes(generation));
  const auto host = [&](std::uint64_t address) {
    auto *pointer = tlb.translate(address, ::uml::memory::Access::write);
    require(pointer != nullptr);
    return static_cast<std::byte *>(pointer);
  };
  const auto slot = [&](std::uint64_t address) {
    return uml::cpu::arm64::jit::testing::reservation_slot_for_testing(
        host(address));
  };
  const auto reserved_slot = slot(data);
  std::uint64_t other = 0;
  if (same_granule) {
    other = data + 8U;
  } else {
    for (std::uint64_t offset = 2U * page;
         other == 0 && offset < pages * page; offset += 16U) {
      if ((slot(base + offset) == reserved_slot) == colliding)
        other = base + offset;
    }
  }
  require(other != 0);

  uml::cpu::arm64::JitExecutor executor;
  uml::memory::StaticGuestMemory memory;
  uml::cpu::arm64::CpuState cpu{};
  cpu.pc = base;
  cpu.registers[1] = data;
  cpu.registers[2] = 5U;
  cpu.registers[4] = replacement;
  cpu.registers[6] = other;
  // The load-exclusive takes the helper, which keeps a software
  // reservation; the store then runs natively or through the helper.
  require(executor.begin_execution(generation, cpu.pc));
  std::size_t executed = 0;
  require(executor.run_block(cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::continued);
  require(cpu.exclusive_size == 8U);
  cpu.pc = base + 8U;
  if (path == OwnStore::semantic) {
    semantic_store_through_helper(cpu, generation, alias_store);
  } else {
    auto event = ::uml::cpu::EngineEvent::continued;
    for (unsigned blocks = 0;
         blocks < 4U && event == ::uml::cpu::EngineEvent::continued; ++blocks)
      event = executor.run_block(cpu, memory, generation, executed);
    require(event == ::uml::cpu::EngineEvent::system_call);
  }
  cpu.pc = base + 16U;
  require(executor.run_block(cpu, memory, generation, executed) ==
          ::uml::cpu::EngineEvent::continued);
  require(cpu.registers[5] == (same_granule ? 1U : 0U));
  require(backing.get64(page) == (same_granule ? observed : replacement));
  executor.end_execution();
}

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  require(uml::cpu::arm64::set_jit_cache_budget(
      256U * 1024U * 1024U, 10));
  native_single_block_case();
  native_linked_loop_case();
  for (unsigned size = 0; size < 4U; ++size) {
    for (bool acquire_release : {false, true}) {
      native_compare_retry_case(size, acquire_release, true);
      native_compare_retry_case(size, acquire_release, false);
      native_compare_retry_case(size, acquire_release, false, true);
    }
  }
  compare_lookahead_preserves_fetch_faults(false);
  compare_lookahead_preserves_fetch_faults(true);
  concurrent_compare_retry_case(false);
  concurrent_compare_retry_case(true);
  same_backing_alias_case(true);
  same_backing_alias_case(false);
  same_backing_alias_case(true, true);
  same_backing_alias_case(false, true);
  same_backing_alias_case(true, false, AliasStore::release);
  same_backing_alias_case(false, false, AliasStore::release);
  native_store_invalidates_semantic_reservation_case();
  slow_store_invalidates_semantic_reservation_case(false);
  slow_store_invalidates_semantic_reservation_case(true);
  concurrent_native_cold_paths_leave_publication_before_helper();
  helper_pair_compare_swap_keeps_native_updates();
  helper_misaligned_compare_swap_keeps_native_updates();
  for (const auto path : {OwnStore::native, OwnStore::semantic}) {
    own_store_keeps_reservation_case(path, false, false);
    own_store_keeps_reservation_case(path, false, true);
    own_store_keeps_reservation_case(path, true, false);
  }
  return 0;
}
