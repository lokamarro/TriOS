// SPDX-License-Identifier: GPL-2.0-or-later
//
// The terminal slow path of the AArch32 translation (a32_helpers.hpp): one
// instruction the inline lowering declines, executed on the register file the
// generated code spilled. Every instruction runs in the AArch32 semantic
// library (cpu/arm64/aarch32/semantics.hpp) over the Tlb, with the library's
// fault ordering: a page crossing, a misalignment the architecture faults on,
// a faulting access and the forms the compiler leaves to it.
//
// The library's exclusive-monitor hooks are overridden here. The guest
// monitor is the JIT's versioned software reservation table, which its tracked
// stores and its fused host exclusives share (jit_semantic_reservation.hpp),
// so LibraryMemory records a load-exclusive in CpuState and checks a
// store-exclusive against that table instead of the library's
// compare-and-swap.

#include "a32_helpers.hpp"

#include "a32_operations.hpp"

#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/aarch32/semantics.hpp"
#include "cpu/arm64/fp_environment.hpp"
#include "cpu/arm64/jit/jit_publication.hpp"
#include "cpu/arm64/jit/jit_runtime_contract.hpp"
#include "cpu/arm64/jit/jit_semantic_reservation.hpp"
#include "cpu/arm64/memory.hpp"

#include <bit>
#include <mutex>
#include <span>

namespace uml::cpu::arm64 {

namespace {

using ::uml::memory::Access;
using ::uml::memory::Tlb;
namespace a32 = jit::a32;
namespace sem = jit::semantic;
using aarch32::Instruction;

/* The exclusive monitor's view of guest memory, for sem::atomic_load_value
 * and sem::atomic_store_exclusive_value: the translated page of one access
 * the library has already checked for natural alignment (1, 2, 4 or 8
 * bytes), which the monitor performs as one host access. has_fault() is
 * true, so a failed translation is the access's fault, which the library
 * reports, and neither helper falls back to read or write. */
class TlbMemory final : public GuestMemory {
public:
  explicit TlbMemory(Tlb &tlb) noexcept : tlb_{tlb} {}

  bool read(std::uint64_t, std::span<std::byte>) override { return false; }

  bool write(std::uint64_t, std::span<const std::byte>) override {
    return false;
  }

  /* The dynamic loader can change COW permissions between accesses, so a
   * write translation is revalidated here. */
  std::byte *atomic_pointer(std::uint64_t address, std::size_t size,
                            ::uml::memory::MemoryAccess access) override {
    if (size == 0 || size > 8U || (size & (size - 1U)) != 0 ||
        (address & (size - 1U)) != 0)
      return nullptr;
    const auto direction = ::uml::memory::translated_access(access);
    if (direction == Access::write)
      tlb_.discard(address);
    return tlb_.translate(address, direction);
  }

  [[nodiscard]] bool has_fault() const noexcept override { return true; }

private:
  Tlb &tlb_;
};


/* The library's view of guest memory: the Tlb's translations, with a write
 * translation revalidated (the dynamic loader can change COW permissions
 * between accesses). The failing page is the library's to report. The
 * exclusive monitor is the JIT's software reservation. */
class LibraryMemory final : public aarch32::A32Memory {
public:
  LibraryMemory(CpuState &cpu, Tlb &tlb) noexcept
      : cpu_{cpu}, tlb_{tlb}, memory_{tlb} {}

  std::byte *translate(std::uint64_t address,
                       ::uml::memory::MemoryAccess access) noexcept override {
    const auto direction = ::uml::memory::translated_access(access);
    if (direction == Access::write)
      tlb_.discard(address);
    return tlb_.translate(address, direction);
  }

  /* An ordinary store invalidates the reservation granules it touched, as
   * the native store blocks do when their publication ends. */
  void stored(std::byte *host, std::size_t size) noexcept override {
    const JitStoreRange range{reinterpret_cast<std::uint64_t>(host),
                                   size};
    sem::invalidate_store_ranges(cpu_, std::span{&range, 1});
  }

  /* LDREX: the value and the reservation (the granule's key and version)
   * recorded together under the lock the store-exclusive and the stores take.
   * The library has just translated this access, so the load fails only if
   * the mapping changed in between; the monitor is then left clear. */
  std::uint64_t load_exclusive(CpuState &cpu, std::uint32_t address,
                               std::byte *, unsigned size,
                               bool acquire) noexcept override {
    std::scoped_lock lock{sem::exclusive_memory_mutex};
    std::uint64_t value = 0;
    std::uintptr_t key = 0;
    if (!sem::atomic_load_value(memory_, address, size,
                                acquire ? std::memory_order_acquire
                                        : std::memory_order_relaxed,
                                value, key)) {
      cpu.clear_exclusive_monitor();
      return 0;
    }
    cpu.exclusive_address = address;
    cpu.exclusive_key = key;
    cpu.exclusive_value = value;
    cpu.exclusive_value2 = 0;
    cpu.exclusive_version = sem::reservation_version(key);
    cpu.exclusive_size = static_cast<std::uint8_t>(size);
    return value;
  }

  /* STREX passes when the monitor still names this location, no store has
   * invalidated its granule and the location holds the value the load read;
   * the monitor is cleared either way. The store itself is a compare-and-swap
   * from that value, so a write that does not take the exclusive lock (the
   * kernel's futex and SWP emulation) between the check and the store fails
   * the STREX instead of being overwritten. */
  bool store_exclusive(CpuState &cpu, std::uint32_t address, std::byte *,
                       unsigned size, std::uint64_t value,
                       bool release) noexcept override {
    std::scoped_lock lock{sem::exclusive_memory_mutex};
    std::uint64_t current = 0;
    std::uintptr_t key = 0;
    bool success = sem::atomic_load_value(memory_, address, size,
                                          std::memory_order_relaxed, current,
                                          key) &&
                   cpu.exclusive_address == address &&
                   cpu.exclusive_size == size && cpu.exclusive_key == key &&
                   cpu.exclusive_version == sem::reservation_version(key) &&
                   cpu.exclusive_value == current;
    const sem::MonitoredValue monitored{cpu.exclusive_key, cpu.exclusive_value,
                                        0};
    cpu.clear_exclusive_monitor();
    if (!success)
      return false;
    std::uintptr_t stored_key = 0;
    const auto result = sem::atomic_store_exclusive_value(
        memory_, address, size, monitored, value,
        release ? std::memory_order_release : std::memory_order_relaxed,
        stored_key);
    if (result != sem::AtomicStoreResult::stored)
      return false;
    sem::reservation_store(cpu, stored_key);
    return true;
  }

private:
  CpuState &cpu_;
  Tlb &tlb_;
  TlbMemory memory_;
};

/* Whether the library runs the instruction with the host's own FPCR and FPSR
 * and the guest's committed in CpuState: everything but the memory transfers
 * and the integer instructions of the parallel family. */
[[nodiscard]] bool needs_host_fp_environment(const Instruction &inst) noexcept {
  const auto traits = inst.info().traits;
  if ((traits & (aarch32::trait::loads | aarch32::trait::stores)) != 0U)
    return false;
  switch (a32::op_info(inst.form).op) {
  case a32::Op::parallel: case a32::Op::ssat16: case a32::Op::usat16:
  case a32::Op::usad8: case a32::Op::usada8:
    return false;
  default:
    return true;
  }
}

/* The library's answer, as the JIT result the generated code returns. */
[[nodiscard]] std::uint64_t execute_in_library(CpuState &cpu, Tlb &tlb,
                                               const Instruction &inst,
                                               std::uint64_t key,
                                               std::uint32_t itstate) noexcept {
  cpu.pc = key;
  cpu.a32_itstate = static_cast<std::uint8_t>(itstate);
  LibraryMemory memory{cpu, tlb};
  aarch32::StepResult result;
  const auto traits = inst.info().traits;
  if ((traits & aarch32::trait::stores) != 0U &&
      (traits & aarch32::trait::exclusive) == 0U) {
    /* A store serializes with the exclusive operations, which check the
     * monitor and store under the same lock (the store-exclusive takes it
     * itself). */
    std::scoped_lock lock{sem::exclusive_memory_mutex};
    result = aarch32::execute_a32(cpu, inst, memory);
  } else {
    result = aarch32::execute_a32(cpu, inst, memory);
  }
  switch (result.event) {
  case aarch32::StepEvent::retired:
    return pack_jit_result(Event::continued, 1);
  case aarch32::StepEvent::memory_fault:
    tlb.fault_address = result.fault_address;
    tlb.fault_access = ::uml::memory::translated_access(result.access);
    tlb.fault_alignment = result.alignment ? 1U : 0U;
    return pack_jit_result(Event::memory_fault, 0);
  case aarch32::StepEvent::undefined:
    return pack_jit_result(Event::illegal_instruction, 0);
  default:
    break;
  }
  /* The compiler routes only what the library implements here. */
  return pack_jit_result(Event::fatal, 0);
}

} // namespace

extern "C" std::uint64_t uml_arm64_jit_a32_execute(
    CpuState *cpu, Tlb *tlb, std::uint32_t word, std::uint64_t key,
    std::uint32_t itstate) noexcept {
  if (cpu == nullptr || tlb == nullptr || !aarch32::key_is_aarch32(key))
    return pack_jit_result(Event::fatal, 0);
  const auto profile = a32::decode_profile();
  const auto inst =
      aarch32::key_is_thumb(key)
          ? aarch32::decode_thumb(
                word, aarch32::ItState{static_cast<std::uint8_t>(itstate)},
                profile)
          : aarch32::decode_a32(word, profile);
  /* A terminal helper runs outside the native register environment, unless
   * the instruction has nothing to do with floating point (a memory transfer
   * or integer arithmetic): the guest's FPCR and FPSR then stay installed,
   * the next block that needs them finds the environment active, and the end
   * of the run restores the host's. */
  if (needs_host_fp_environment(inst))
    arm64::restore_native_fp_environment(*cpu);
  const auto traits = inst.info().traits;
  if ((traits & aarch32::trait::exclusive) == 0U)
    return execute_in_library(*cpu, *tlb, inst, key, itstate);

  /* The exclusives: serialized with the other vCPUs' stores while a software
   * reservation exists, and creating one registers it (the A64 exclusive
   * helper's rules). */
  const bool establishes = (traits & aarch32::trait::loads) != 0U;
  const bool uses_software_reservation =
      establishes || cpu->exclusive_size != 0U;
  const auto run = [&]() noexcept {
    const bool registered = establishes && begin_jit_reservation(*cpu);
    const auto result = execute_in_library(*cpu, *tlb, inst, key, itstate);
    if (establishes && cpu->exclusive_size != 0U &&
        current_jit_store_participant != nullptr)
      current_jit_store_participant->reservation_cpu.store(
          cpu, std::memory_order_seq_cst);
    if (registered && cpu->exclusive_size == 0) {
      if (current_jit_store_participant != nullptr &&
          current_jit_store_participant->reservation_cpu.load(
              std::memory_order_seq_cst) == cpu)
        current_jit_store_participant->reservation_cpu.store(
            nullptr, std::memory_order_seq_cst);
      end_jit_reservation(*cpu);
    }
    return result;
  };
  if (uses_software_reservation) {
    JitExclusiveOperationScope operation;
    return run();
  }
  return run();
}

} // namespace uml::cpu::arm64
