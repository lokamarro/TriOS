// SPDX-License-Identifier: GPL-2.0-or-later

#include "jit_semantic_memory.hpp"
#include "jit_publication.hpp"
#include "jit_semantic_instructions.hpp"
#include "jit_semantic_reservation.hpp"

#include "cpu/arm64/detail/floating_exception.hpp"
#include "cpu/arm64/detail/memory_copy_set.hpp"
#include "cpu/arm64/detail/sm0_spill.hpp"
#include "cpu/arm64/detail/single_copy_alignment.hpp"
#include "cpu/arm64/fp_environment.hpp"
#include "cpu/arm64/memory_transfer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <span>

namespace uml::cpu::arm64 {

using ::uml::memory::Access;

void
activate_native_fp_environment(CpuState &cpu) noexcept {
  if (cpu.gadget_fp_active)
    return;
  __asm__ volatile("mrs %0, fpcr" : "=r"(cpu.gadget_host_fpcr) : : "memory");
  __asm__ volatile("mrs %0, fpsr" : "=r"(cpu.gadget_host_fpsr) : : "memory");
  cpu.gadget_fp_active = true;
  /* The environment starts under the guest's FPCR (see a32_fp_standard). */
  cpu.a32_fp_standard = 0;
  const std::uint64_t fpcr = arm64::detail::host_fpcr(cpu.fpcr);
  const std::uint64_t fpsr = cpu.fpsr;
  __asm__ volatile("msr fpcr, %0" : : "r"(fpcr) : "memory");
  __asm__ volatile("msr fpsr, %0" : : "r"(fpsr) : "memory");
}

namespace {

void materialize_semantic_cpu_state(CpuState &cpu) noexcept {
  /*
   * A terminal generated-code helper runs outside the native register
   * environment. Commit any retained guest status and restore the host
   * environment before the semantic slow path can inspect CpuState.
   */
  arm64::restore_native_fp_environment(cpu);
  cpu.access_fp_state();
}

extern "C" std::uint64_t
uml_arm64_jit_alignment_fault(CpuState *cpu, Tlb *tlb,
                              std::uint32_t instruction) noexcept {
  if (cpu == nullptr || tlb == nullptr)
    return pack_jit_result(Event::fatal, 0);
  materialize_semantic_cpu_state(*cpu);
  const unsigned base = (instruction >> 5) & 31U;
  tlb->fault_alignment = 1;
  tlb->fault_address = cpu->read_register(base, true);
  const unsigned operation = (instruction >> 22) & 3U;
  const bool vector = (instruction & 0x0400'0000U) != 0;
  const bool authenticated_load =
      (instruction & 0xff20'0400U) == 0xf820'0400U;
  const bool load = authenticated_load ||
                    (vector ? (operation & 1U) != 0U : operation != 0U);
  tlb->fault_access = load ? Access::read : Access::write;
  return pack_jit_result(Event::memory_fault, 0);
}

extern "C" void *uml_arm64_jit_tlb_translate(Tlb *tlb, std::uint64_t address,
                                             unsigned access) noexcept {
  if (tlb == nullptr || access > 1)
    return nullptr;
  return tlb->translate(address, access == 0 ? Access::read : Access::write);
}

[[nodiscard]] bool jit_pair_read(Tlb &tlb, std::uint64_t address,
                                 std::span<std::byte> bytes) noexcept {
  while (!bytes.empty()) {
    auto *source = tlb.translate(address, Access::read);
    if (source == nullptr)
      return false;
    const auto count = std::min(
        bytes.size(),
        static_cast<std::size_t>(page_size - (address & (page_size - 1U))));
    std::memcpy(bytes.data(), source, count);
    address += count;
    bytes = bytes.subspan(count);
  }
  return true;
}

[[nodiscard]] bool jit_pair_write(Tlb &tlb, std::uint64_t address,
                                  std::span<const std::byte> bytes,
                                  CpuState *writer = nullptr) noexcept {
  /* Pair stores are architecturally one memory access. Translate every
   * page before the first host store so a second-page fault cannot expose a
   * partial guest update. A pair spans at most two supported guest pages.
   *
   * A store invalidates every software reservation granule it writes, even
   * when the stored bits are unchanged. The ordinary semantic store path
   * does that itself; a caller that writes here directly asks for it.
   */
  std::array<std::byte *, 2> destinations{};
  std::array<std::size_t, 2> lengths{};
  std::size_t pages = 0;
  std::size_t completed = 0;
  while (completed != bytes.size()) {
    if (pages == destinations.size())
      return false;
    const auto current = address + completed;
    auto *destination = tlb.translate(current, Access::write);
    if (destination == nullptr)
      return false;
    const auto count = std::min(
        bytes.size() - completed,
        static_cast<std::size_t>(page_size - (current & (page_size - 1U))));
    destinations[pages] = destination;
    lengths[pages] = count;
    ++pages;
    completed += count;
  }
  completed = 0;
  std::array<JitStoreRange, 2> stored{};
  for (std::size_t page = 0; page < pages; ++page) {
    std::memcpy(destinations[page], bytes.data() + completed, lengths[page]);
    completed += lengths[page];
    stored[page] = {reinterpret_cast<std::uintptr_t>(destinations[page]),
                    lengths[page]};
  }
  if (writer != nullptr)
    jit::semantic::invalidate_store_ranges(
        *writer, std::span<const JitStoreRange>{stored}.first(pages));
  return true;
}

class JitSemanticMemory final : public ::uml::memory::GuestAddressSpace {
public:
  explicit JitSemanticMemory(Tlb &tlb) noexcept : tlb_{tlb} {}

  bool read(std::uint64_t address, std::span<std::byte> destination) override {
    if (jit_pair_read(tlb_, address, destination))
      return true;
    record_fault();
    return false;
  }

  bool write(std::uint64_t address,
             std::span<const std::byte> source) override {
    std::size_t completed = 0;
    while (completed < source.size()) {
      const auto current = address + completed;
      tlb_.discard(current);
      completed += std::min(
          source.size() - completed,
          static_cast<std::size_t>(page_size - (current & (page_size - 1U))));
    }
    if (jit_pair_write(tlb_, address, source))
      return true;
    record_fault();
    return false;
  }

  bool validate_access(std::uint64_t address, std::size_t size,
                       ::uml::memory::MemoryAccess access) override {
    const auto direction = access == ::uml::memory::MemoryAccess::write
                               ? Access::write
                               : Access::read;
    std::size_t completed = 0;
    while (completed < size) {
      const auto current = address + completed;
      if (tlb_.translate(current, direction) == nullptr) {
        record_fault();
        return false;
      }
      completed += std::min(
          size - completed,
          static_cast<std::size_t>(
              page_size - (current & (page_size - 1U))));
    }
    return true;
  }

  /*
   * Every atomic, exclusive and ordered access of the semantic path is one
   * host access through this pointer: an LSE atomic or CASP for a
   * read-modify-write, LDAR/STLR or a plain load or store for the ordered
   * and relaxed forms, and LDXP for a 128-bit snapshot. Each is therefore
   * single-copy atomic against native JIT code on other vCPUs and against a
   * host thread that shares the page, such as the Venus command ring
   * reached through the vhost-user shared-memory aperture. A byte-wise copy
   * would let either observe or overwrite a half-updated value that was
   * never architecturally visible.
   *
   * The pointer is offered only for an access the single-copy alignment rule
   * admits as a whole: naturally aligned, or, when the host has FEAT_LSE2,
   * inside one 16-byte block. Such an access never crosses a page. A failed
   * translation is the instruction's fault; callers report it instead of
   * completing the access byte by byte.
   *
   * The dynamic loader can change COW permissions within the same page
   * between two such accesses, so a write translation is revalidated at this
   * cold helper boundary instead of reusing a pre-transition host pointer,
   * exactly as the byte-wise path does before it copies.
   */
  std::byte *atomic_pointer(std::uint64_t address, std::size_t size,
                            ::uml::memory::MemoryAccess access) override {
    if (size == 0 || size > 16U || (size & (size - 1U)) != 0 ||
        arm64::detail::single_copy_alignment_faults(address, size,
                                                    tlb_.unaligned_atomics))
      return nullptr;
    const auto direction = ::uml::memory::translated_access(access);
    if (direction == Access::write)
      tlb_.discard(address);
    auto *pointer = tlb_.translate(address, direction);
    if (pointer == nullptr)
      record_fault();
    return pointer;
  }

  std::byte *reservation_pointer(
      std::uint64_t address, std::size_t size,
      ::uml::memory::MemoryAccess access) override {
    if (size == 0 ||
        size > page_size - (address & (page_size - 1U)))
      return nullptr;
    return tlb_.translate(
        address, ::uml::memory::translated_access(access));
  }

  ::uml::memory::MemoryFault last_fault() const noexcept override {
    return fault_;
  }

  [[nodiscard]] bool has_fault() const noexcept override {
    return fault_recorded_;
  }

private:
  void record_fault() noexcept {
    fault_ = {tlb_.fault_address,
              ::uml::memory::guest_access(tlb_.fault_access)};
    fault_recorded_ = true;
  }

  Tlb &tlb_;
  ::uml::memory::MemoryFault fault_{};
  bool fault_recorded_{};
};

void record_semantic_fault(Tlb &tlb, const JitSemanticMemory &memory,
                           const jit::semantic::RunResult &result) noexcept {
  tlb.fault_address =
      !result.alignment && memory.has_fault() ? memory.last_fault().address
                                             : result.address;
  tlb.fault_access = result.write ? Access::write : Access::read;
  tlb.fault_alignment = result.alignment;
}

[[nodiscard]] std::uint64_t
semantic_result(Tlb &tlb, const JitSemanticMemory &memory,
                const jit::semantic::RunResult &result) noexcept {
  using jit::semantic::StopReason;
  switch (result.reason) {
  case StopReason::continued:
    return pack_jit_result(Event::continued, result.instructions);
  case StopReason::memory_fault:
    record_semantic_fault(tlb, memory, result);
    return pack_jit_result(Event::memory_fault, result.instructions);
  case StopReason::illegal_instruction:
  case StopReason::unsupported_instruction:
    return pack_jit_result(Event::illegal_instruction, result.instructions);
  }
  return pack_jit_result(Event::fatal, 0);
}

extern "C" std::uint64_t
uml_arm64_jit_execute_exclusive(CpuState *cpu, Tlb *tlb,
                                std::uint32_t instruction) noexcept {
  if (cpu == nullptr || tlb == nullptr)
    return pack_jit_result(Event::fatal, 0);
  materialize_semantic_cpu_state(*cpu);
  /* Generated code routes only the selected classes here. Any other word is
   * an internal error and is never interpreted. */
  const auto handler = jit::semantic::exclusive_handler(instruction);
  if (handler == nullptr)
    return pack_jit_result(Event::fatal, 0);
  /* An exclusive load: o2 = 0 and L = 1 in the exclusive class. */
  const bool establishes_reservation =
      handler == jit::semantic::load_store_exclusive &&
      (instruction & 0x00c0'0000U) == 0x0040'0000U;
  const bool uses_software_reservation =
      establishes_reservation || cpu->exclusive_size != 0U;
  const auto execute = [&]() noexcept {
    const bool registered =
        establishes_reservation && begin_jit_reservation(*cpu);
    JitSemanticMemory memory{*tlb};
    const auto result = handler(*cpu, memory, instruction);
    if (establishes_reservation && cpu->exclusive_size != 0U &&
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
    return semantic_result(*tlb, memory, result);
  };
  /*
   * Compare-and-swap and the LSE atomics perform their memory transaction
   * as one host atomic on the translated page, so they are single-copy
   * atomic against every other writer without the publication gate. Only
   * an exclusive load, which creates CpuState's software reservation, and
   * any instruction run while that reservation is held, which may consume
   * it, wait out other vCPUs' native store blocks.
   */
  if (uses_software_reservation) {
    JitExclusiveOperationScope operation;
    return execute();
  }
  return execute();
}

[[nodiscard]] std::uint64_t
execute_semantic_instruction(CpuState &cpu, Tlb &tlb,
                             std::uint32_t instruction,
                             jit::semantic::Handler handler) noexcept {
  materialize_semantic_cpu_state(cpu);
  /* Generated code routes only the selected classes here. Any other word is
   * an internal error and is never interpreted. */
  if (handler == nullptr)
    return pack_jit_result(Event::fatal, 0);
  JitSemanticMemory memory{tlb};
  return semantic_result(tlb, memory, handler(cpu, memory, instruction));
}

extern "C" std::uint64_t
uml_arm64_jit_execute_memory(CpuState *cpu, Tlb *tlb,
                             std::uint32_t instruction) noexcept {
  if (cpu == nullptr || tlb == nullptr)
    return pack_jit_result(Event::fatal, 0);
  return execute_semantic_instruction(
      *cpu, *tlb, instruction, jit::semantic::memory_handler(instruction));
}

/*
 * A MOPS chunk is stored as an ordinary semantic store is: serialized with
 * the exclusive operations by exclusive_memory_mutex. It invalidates the
 * reservation granules it writes only while a software reservation exists,
 * as a native store block is tracked only then. A reservation established
 * later takes its snapshot under the same mutex, after this store.
 */
struct JitMopsStore final {
  CpuState &writer;

  template <typename Transfer>
  [[nodiscard]] bool operator()(Tlb &tlb, std::uint64_t address,
                                Transfer &&transfer) const noexcept {
    std::scoped_lock lock{jit::semantic::exclusive_memory_mutex};
    auto *pointer = tlb.translate(address, Access::write);
    if (pointer == nullptr)
      return false;
    const arm64::detail::MopsStoredRange stored = transfer(pointer);
    if (active_jit_reservations.load(std::memory_order_seq_cst) != 0U) {
      const JitStoreRange range{
          reinterpret_cast<std::uintptr_t>(stored.pointer), stored.size};
      jit::semantic::invalidate_store_ranges(writer, std::span{&range, 1});
    }
    return true;
  }
};

template <arm64::detail::MopsFamily family>
[[nodiscard]] std::uint64_t
execute_mops(CpuState &cpu, Tlb &tlb,
             const arm64::detail::MopsInstruction &mops,
             std::uint32_t stages) noexcept {
  std::uint64_t zero{};
  auto &destination = cpu.registers[mops.destination];
  auto &count = cpu.registers[mops.count];
  auto &source = mops.source == 31U ? zero : cpu.registers[mops.source];
  if (mops.stage == 0U) {
    arm64::detail::mops_prologue<family>(cpu, destination, count, source);
    if (stages == 1U) {
      cpu.pc += 4;
      return pack_jit_result(Event::continued, 1);
    }
  } else if (mops.stage == 1U) {
    cpu.pc += 4;
    return pack_jit_result(Event::continued, 1);
  }
  /* A fused sequence has retired its prologue and main stage before the
   * epilogue, which restarts alone after a fault. */
  const std::size_t retired = stages == 3U ? 2U : 0U;
  cpu.pc += retired * 4U;
  switch (arm64::detail::mops_epilogue<family>(cpu, tlb, destination, count,
                                               source, JitMopsStore{cpu})) {
  case arm64::detail::MopsOutcome::complete:
    cpu.pc += 4;
    return pack_jit_result(Event::continued, retired + 1U);
  case arm64::detail::MopsOutcome::fault:
    tlb.fault_alignment = 0;
    return pack_jit_result(Event::memory_fault, retired);
  case arm64::detail::MopsOutcome::illegal:
    break;
  }
  return pack_jit_result(Event::illegal_instruction, retired);
}

extern "C" std::uint64_t
uml_arm64_jit_execute_mops(CpuState *cpu, Tlb *tlb, std::uint32_t instruction,
                           std::uint32_t stages) noexcept {
  const auto mops = arm64::detail::decode_mops(instruction);
  /* Translation routes only valid words, and fuses only from a prologue. */
  if (cpu == nullptr || tlb == nullptr || !mops.valid ||
      (stages != 1U && (stages != 3U || mops.stage != 0U)))
    return pack_jit_result(Event::fatal, 0);
  materialize_semantic_cpu_state(*cpu);
  switch (mops.family) {
  case arm64::detail::MopsFamily::copy_forward:
    return execute_mops<arm64::detail::MopsFamily::copy_forward>(
        *cpu, *tlb, mops, stages);
  case arm64::detail::MopsFamily::copy_general:
    return execute_mops<arm64::detail::MopsFamily::copy_general>(
        *cpu, *tlb, mops, stages);
  case arm64::detail::MopsFamily::set:
    break;
  }
  return execute_mops<arm64::detail::MopsFamily::set>(*cpu, *tlb, mops,
                                                      stages);
}

extern "C" std::uint64_t
uml_arm64_jit_execute_pair(CpuState *cpu, Tlb *tlb,
                           std::uint32_t instruction) noexcept {
  if (cpu == nullptr || tlb == nullptr)
    return pack_jit_result(Event::fatal, 0);
  materialize_semantic_cpu_state(*cpu);

  const bool vector = (instruction & 0x0400'0000U) != 0;
  const bool load = (instruction & 0x0040'0000U) != 0;
  const unsigned operation = instruction >> 30;
  const unsigned mode = (instruction >> 23) & 3U;
  const std::size_t size =
      vector            ? (operation >= 2U ? 16U : std::size_t{4} << operation)
      : operation >= 2U ? 8U
                        : 4U;
  const auto displacement = sign_extend((instruction >> 15) & 0x7fU, 7) *
                            static_cast<std::int64_t>(size);
  const unsigned base_register = (instruction >> 5) & 31U;
  const auto tagged_base = cpu->read_register(base_register, true);
  if (base_register == 31U && (tagged_base & 15U) != 0) {
    tlb->fault_alignment = 1;
    tlb->fault_address = tagged_base;
    tlb->fault_access = load ? Access::read : Access::write;
    return pack_jit_result(Event::memory_fault, 0);
  }
  const auto adjusted =
      tagged_base + static_cast<std::uint64_t>(displacement);
  const auto tagged_address = mode == 1U ? tagged_base : adjusted;
  const auto address =
      cpu->untag_data_address(tagged_address);
  const unsigned first = instruction & 31U;
  const unsigned second = (instruction >> 10) & 31U;
  std::array<std::byte, 32> bytes{};
  const auto transfer = std::span{bytes}.first(size * 2U);
  if (load) {
    if (!jit_pair_read(*tlb, address, transfer))
      return pack_jit_result(Event::memory_fault, 0);
    if (vector) {
      cpu->vectors[first].bytes.fill(std::byte{});
      cpu->vectors[second].bytes.fill(std::byte{});
      std::memcpy(cpu->vectors[first].bytes.data(), bytes.data(), size);
      std::memcpy(cpu->vectors[second].bytes.data(), bytes.data() + size, size);
    } else {
      std::uint64_t first_value{};
      std::uint64_t second_value{};
      std::memcpy(&first_value, bytes.data(), size);
      std::memcpy(&second_value, bytes.data() + size, size);
      if (operation == 1U) {
        first_value = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(static_cast<std::int32_t>(first_value)));
        second_value = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(static_cast<std::int32_t>(second_value)));
      }
      cpu->write_register(first, first_value, false);
      cpu->write_register(second, second_value, false);
    }
  } else {
    if (vector) {
      std::memcpy(bytes.data(), cpu->vectors[first].bytes.data(), size);
      std::memcpy(bytes.data() + size, cpu->vectors[second].bytes.data(), size);
    } else {
      const auto first_value = cpu->read_register(first, false);
      const auto second_value = cpu->read_register(second, false);
      std::memcpy(bytes.data(), &first_value, size);
      std::memcpy(bytes.data() + size, &second_value, size);
    }
    /* This helper runs inside the block's store publication, so an
     * exclusive store waiting in its operation scope sees the invalidation. */
    if (!jit_pair_write(*tlb, address, transfer, cpu))
      return pack_jit_result(Event::memory_fault, 0);
  }

  bool writeback = mode == 1U || mode == 3U;
  if (writeback && load && !vector && base_register != 31U &&
      (base_register == first || base_register == second))
    writeback = false;
  if (writeback)
    cpu->write_register(base_register, adjusted, true);
  cpu->pc += sizeof(instruction);
  return pack_jit_result(Event::continued, 1);
}

/*
 * Scalar ordered accesses are single-copy atomic, with acquire or release
 * ordering where selected by the instruction. Guest memory shared with a
 * native host thread - the Venus command ring is reached over the vhost-user
 * shared-memory aperture - is observed by that thread with real atomics, so a
 * byte-wise copy here races: the renderer can load a tail word mid-store and
 * see a value that was never architecturally visible. Perform the access as
 * one host atomic whenever the translation is suitably aligned. The stronger
 * sequentially-consistent host order also covers every scalar ordered form
 * routed through this helper.
 */
template <typename Integer>
[[nodiscard]] std::uint64_t ordered_load_at(void *pointer) noexcept {
  return std::atomic_ref<Integer>{*static_cast<Integer *>(pointer)}.load(
      std::memory_order_seq_cst);
}

[[nodiscard]] bool ordered_access_is_atomic(const void *pointer,
                                            std::size_t size) noexcept {
  return size <= sizeof(std::uint64_t) &&
         (reinterpret_cast<std::uintptr_t>(pointer) & (size - 1U)) == 0;
}

extern "C" std::uint64_t
uml_arm64_jit_execute_ordered(CpuState *cpu, Tlb *tlb,
                              std::uint32_t instruction) noexcept {
  if (cpu == nullptr || tlb == nullptr)
    return pack_jit_result(Event::fatal, 0);
  const bool load = (instruction & 0x0040'0000U) != 0;
  /*
   * Ordered stores must invalidate software reservations even when an alias
   * writes the same bits, and must revalidate write access. The semantic
   * slow path owns both rules and stores the value with one host release
   * store, so route the store there.
   */
  if (!load)
    return execute_semantic_instruction(
        *cpu, *tlb, instruction,
        jit::semantic::ordered_store_handler(instruction));
  materialize_semantic_cpu_state(*cpu);
  const unsigned size_shift = instruction >> 30;
  const std::size_t size = std::size_t{1} << size_shift;
  const unsigned base = (instruction >> 5) & 31U;
  const unsigned transfer = instruction & 31U;
  const auto tagged_address = cpu->read_register(base, true);
  const auto address = cpu->untag_data_address(tagged_address);
  if (arm64::detail::single_copy_alignment_faults(address, size,
                                                  tlb->unaligned_atomics)) {
    tlb->fault_alignment = 1;
    tlb->fault_address = address;
    tlb->fault_access = Access::read;
    return pack_jit_result(Event::memory_fault, 0);
  }
  auto *source = tlb->translate(address, Access::read);
  if (source == nullptr)
    return pack_jit_result(Event::memory_fault, 0);
  std::uint64_t value = 0;
  if (ordered_access_is_atomic(source, size)) {
    switch (size) {
    case 1: value = ordered_load_at<std::uint8_t>(source); break;
    case 2: value = ordered_load_at<std::uint16_t>(source); break;
    case 4: value = ordered_load_at<std::uint32_t>(source); break;
    default: value = ordered_load_at<std::uint64_t>(source); break;
    }
  } else if (tlb->unaligned_atomics) {
    /* Admitted misaligned inside one 16-byte block: the host has LSE2. */
    value = arm64::detail::lse2_acquire_load(source, size);
  } else {
    std::memcpy(&value, source, size);
    std::atomic_thread_fence(std::memory_order_acquire);
  }
  cpu->write_register(transfer, value, false);
  cpu->pc += sizeof(instruction);
  return pack_jit_result(Event::continued, 1);
}

/*
 * The shared memory transfer, which the gadget engine also uses, stores
 * without touching the JIT's software reservations. Invalidate every granule
 * it wrote, even when the stored bits are unchanged, from the same untagged
 * address it translated. The helper runs inside the block's store
 * publication, so an exclusive store waiting in its operation scope sees it.
 */
void invalidate_structure_store(CpuState &cpu, Tlb &tlb,
                                std::uint64_t tagged_address,
                                std::size_t size) noexcept {
  const auto address = cpu.untag_data_address(tagged_address);
  std::array<JitStoreRange, 2> stored{};
  std::size_t pages = 0;
  for (std::size_t completed = 0;
       completed < size && pages < stored.size();) {
    const auto current = address + completed;
    const auto length = std::min(
        size - completed,
        static_cast<std::size_t>(page_size - (current & (page_size - 1U))));
    auto *destination = tlb.translate_tagged(current, Access::write);
    if (destination != nullptr)
      stored[pages++] = {reinterpret_cast<std::uintptr_t>(destination),
                         length};
    completed += length;
  }
  jit::semantic::invalidate_store_ranges(
      cpu, std::span<const JitStoreRange>{stored}.first(pages));
}

extern "C" std::uint64_t
uml_arm64_jit_execute_simd_structure(CpuState *cpu, Tlb *tlb,
                                     std::uint32_t instruction) noexcept {
  if (cpu == nullptr || tlb == nullptr)
    return pack_jit_result(Event::fatal, 0);
  materialize_semantic_cpu_state(*cpu);
  const bool single = (instruction & 0xbf80'0000U) == 0x0d00'0000U ||
                      (instruction & 0xbf80'0000U) == 0x0d80'0000U;
  const bool load = (instruction & 0x0040'0000U) != 0;
  const bool post_index = (instruction & 0x0080'0000U) != 0;
  const unsigned offset_register = (instruction >> 16) & 31U;
  const unsigned base_register = (instruction >> 5) & 31U;
  const unsigned first_register = instruction & 31U;
  const auto tagged_address = cpu->read_register(base_register, true);
  if (base_register == 31U && (tagged_address & 15U) != 0) {
    tlb->fault_alignment = 1;
    tlb->fault_address = tagged_address;
    tlb->fault_access = load ? Access::read : Access::write;
    return pack_jit_result(Event::memory_fault, 0);
  }

  unsigned register_count = 0;
  std::size_t element_size = 0;
  std::size_t vector_size = (instruction & 0x4000'0000U) != 0 ? 16U : 8U;
  std::size_t lane_offset = 0;
  bool interleaved = false;
  bool replicate = false;
  if (!single) {
    const unsigned opcode = (instruction >> 12) & 15U;
    interleaved = opcode == 8U || opcode == 4U || opcode == 0U;
    register_count = opcode == 7U    ? 1U
                     : opcode == 10U ? 2U
                     : opcode == 6U  ? 3U
                     : opcode == 2U  ? 4U
                     : opcode == 8U  ? 2U
                     : opcode == 4U  ? 3U
                     : opcode == 0U  ? 4U
                                     : 0U;
    const unsigned size = (instruction >> 10) & 3U;
    if (register_count == 0U || (!post_index && offset_register != 0U) ||
        (interleaved && vector_size == 8U && size == 3U))
      return pack_jit_result(Event::illegal_instruction, 0);
    element_size = std::size_t{1} << size;
  } else {
    const unsigned extra_register = (instruction >> 21) & 1U;
    const unsigned opcode = (instruction >> 13) & 7U;
    const unsigned scale_bit = (instruction >> 12) & 1U;
    const unsigned size = (instruction >> 10) & 3U;
    const unsigned scale = opcode >> 1;
    if (!post_index && offset_register != 0U)
      return pack_jit_result(Event::illegal_instruction, 0);
    if (scale == 3U) {
      if (!load || scale_bit != 0U)
        return pack_jit_result(Event::illegal_instruction, 0);
      replicate = true;
      register_count = opcode == 6U ? 1U + extra_register : 3U + extra_register;
      element_size = std::size_t{1} << size;
    } else {
      register_count =
          (opcode & 1U) != 0U ? 3U + extra_register : 1U + extra_register;
      std::size_t lane = 0;
      if (scale == 0U) {
        element_size = 1U;
        lane = (static_cast<std::size_t>((instruction >> 30) & 1U) << 3) |
               (scale_bit << 2) | size;
      } else if (scale == 1U) {
        if ((size & 1U) != 0U)
          return pack_jit_result(Event::illegal_instruction, 0);
        element_size = 2U;
        lane = (static_cast<std::size_t>((instruction >> 30) & 1U) << 2) |
               (scale_bit << 1) | (size >> 1);
      } else {
        if ((size & 2U) != 0U || ((size & 1U) != 0U && scale_bit != 0U))
          return pack_jit_result(Event::illegal_instruction, 0);
        element_size = (size & 1U) != 0U ? 8U : 4U;
        lane = (size & 1U) != 0U
                   ? static_cast<std::size_t>((instruction >> 30) & 1U)
                   : (static_cast<std::size_t>((instruction >> 30) & 1U) << 1) |
                         scale_bit;
      }
      lane_offset = lane * element_size;
    }
  }

  const auto transfer_size =
      single ? element_size * register_count : vector_size * register_count;
  std::array<std::byte, 64> bytes{};
  auto transfer = std::span{bytes}.first(transfer_size);
  /* Not the structure wrapper uml_arm64_simd_structure_memory_transfer,
   * which also drops the exclusive monitor for the gadget engine: a vector
   * store of the guest's own between a pair leaves it standing. */
  const auto untagged_address = cpu->untag_data_address(tagged_address);
  if (load) {
    if (uml_arm64_memory_transfer(tlb, untagged_address, transfer.data(),
                                  transfer.size(), 0) == 0U)
      return pack_jit_result(Event::memory_fault, 0);
    for (unsigned index = 0; index < register_count; ++index) {
      auto &destination = cpu->vectors[(first_register + index) & 31U].bytes;
      if (!single) {
        destination.fill(std::byte{});
        if (interleaved) {
          for (std::size_t lane = 0; lane < vector_size / element_size; ++lane)
            std::memcpy(destination.data() + lane * element_size,
                        bytes.data() +
                            (lane * register_count + index) * element_size,
                        element_size);
        } else {
          std::memcpy(destination.data(), bytes.data() + index * vector_size,
                      vector_size);
        }
      } else if (replicate) {
        destination.fill(std::byte{});
        for (std::size_t offset = 0; offset < vector_size;
             offset += element_size)
          std::memcpy(destination.data() + offset,
                      bytes.data() + index * element_size, element_size);
      } else {
        std::memcpy(destination.data() + lane_offset,
                    bytes.data() + index * element_size, element_size);
      }
    }
  } else {
    for (unsigned index = 0; index < register_count; ++index) {
      const auto &source = cpu->vectors[(first_register + index) & 31U].bytes;
      if (!single) {
        if (interleaved) {
          for (std::size_t lane = 0; lane < vector_size / element_size; ++lane)
            std::memcpy(bytes.data() +
                            (lane * register_count + index) * element_size,
                        source.data() + lane * element_size, element_size);
        } else {
          std::memcpy(bytes.data() + index * vector_size, source.data(),
                      vector_size);
        }
      } else {
        std::memcpy(bytes.data() + index * element_size,
                    source.data() + lane_offset, element_size);
      }
    }
    if (uml_arm64_memory_transfer(tlb, untagged_address, transfer.data(),
                                  transfer.size(), 1) == 0U)
      return pack_jit_result(Event::memory_fault, 0);
    invalidate_structure_store(*cpu, *tlb, tagged_address, transfer.size());
  }
  if (post_index) {
    const auto offset = offset_register == 31U
                            ? transfer_size
                            : cpu->read_register(offset_register, false);
    cpu->write_register(base_register, tagged_address + offset, true);
  }
  cpu->pc += sizeof(instruction);
  return pack_jit_result(Event::continued, 1);
}

/*
 * Guest memory for the spill access of the SM = 0 vector-length family
 * (detail/sm0_spill.hpp) through the executor's TLB. The access forms its
 * address untagged, so the plain translation applies. A store invalidates
 * the software reservations of the host bytes it wrote as it writes them.
 */
class JitSpillMemory final : public arm64::detail::Sm0Memory {
public:
  JitSpillMemory(CpuState &cpu, Tlb &tlb) noexcept : cpu_{cpu}, tlb_{tlb} {}

  std::byte *translate(std::uint64_t address,
                       ::uml::memory::MemoryAccess access) noexcept override {
    return tlb_.translate(address, ::uml::memory::translated_access(access));
  }

  void stored(std::byte *host, std::size_t size) noexcept override {
    const JitStoreRange range{reinterpret_cast<std::uintptr_t>(host), size};
    jit::semantic::invalidate_store_ranges(cpu_, std::span{&range, 1});
  }

private:
  CpuState &cpu_;
  Tlb &tlb_;
};

/* The slow path of the SM = 0 vector-length family's spill words
 * (detail/sm0_vector_length.hpp): a terminal helper, so the block's spill
 * has filled CpuState. */
extern "C" std::uint64_t
uml_arm64_jit_execute_sm0_spill(CpuState *cpu, Tlb *tlb,
                                std::uint32_t instruction) noexcept {
  if (cpu == nullptr || tlb == nullptr)
    return pack_jit_result(Event::fatal, 0);
  materialize_semantic_cpu_state(*cpu);
  JitSpillMemory memory{*cpu, *tlb};
  const auto result =
      arm64::detail::execute_sm0_spill(*cpu, instruction, memory);
  switch (result.event) {
  case arm64::detail::Sm0Event::retired:
    cpu->pc += sizeof(std::uint32_t);
    return pack_jit_result(Event::continued, 1);
  case arm64::detail::Sm0Event::memory_fault:
    tlb->fault_address = result.fault_address;
    tlb->fault_access = ::uml::memory::translated_access(result.access);
    tlb->fault_alignment = result.alignment ? 1U : 0U;
    return pack_jit_result(Event::memory_fault, 0);
  case arm64::detail::Sm0Event::fatal:
    break;
  }
  return pack_jit_result(Event::fatal, 0);
}

} // namespace
} // namespace uml::cpu::arm64
