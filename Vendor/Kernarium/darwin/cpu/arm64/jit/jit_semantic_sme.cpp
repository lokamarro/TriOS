// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The JIT's C++ side of SME: guest writes of SVCR, and the slow path of the
 * ZA and ZT0 forms and of the streaming memory forms. Generated code
 * reaches both as terminal helpers; an SM=1 translation's leave sequence
 * has spilled the guest's vector state to the register file first, so both
 * work on CpuState alone.
 */
#include "jit_semantic_memory.hpp"
#include "jit_semantic_reservation.hpp"
#include "jit_sme_support.hpp"

#include "cpu/arm64/fp_environment.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <optional>
#include <span>

namespace uml::cpu::arm64 {

namespace {

using ::uml::memory::Access;

void materialize(CpuState &cpu) noexcept {
  arm64::restore_native_fp_environment(cpu);
  cpu.access_fp_state();
}

[[nodiscard]] std::uint64_t retire(CpuState &cpu) noexcept {
  cpu.pc += sizeof(std::uint32_t);
  return pack_jit_result(Event::continued, 1);
}

[[nodiscard]] std::uint64_t report_fault(Tlb &tlb, std::uint64_t address,
                                         bool load,
                                         bool alignment) noexcept {
  tlb.fault_address = address;
  tlb.fault_access = load ? Access::read : Access::write;
  tlb.fault_alignment = alignment ? 1 : 0;
  return pack_jit_result(Event::memory_fault, 0);
}

/*
 * Moves `size` bytes (at most one SVL, below a page) between guest memory
 * at `address` and `file`. Every page is translated before any byte moves,
 * so a store is never half applied, and a fault reports the first byte of
 * the first page that fails, as the M4 does (the S3b native records). A
 * store revalidates write permission at this boundary and invalidates the
 * software reservations it overlaps, as the native fast path does.
 */
[[nodiscard]] std::uint64_t transfer(CpuState &cpu, Tlb &tlb,
                                     std::uint64_t address, std::byte *file,
                                     std::size_t size, bool load) noexcept {
  std::array<std::byte *, 2> host{};
  std::array<std::size_t, 2> lengths{};
  std::size_t pages = 0;
  for (std::size_t completed = 0; completed < size;) {
    const auto current = address + completed;
    if (pages == host.size())
      return pack_jit_result(Event::fatal, 0);
    if (!load)
      tlb.discard(current);
    auto *pointer = tlb.translate(current, load ? Access::read : Access::write);
    if (pointer == nullptr)
      return report_fault(tlb, current, load, false);
    const auto length = std::min(
        size - completed,
        static_cast<std::size_t>(page_size - (current & (page_size - 1U))));
    host[pages] = pointer;
    lengths[pages] = length;
    ++pages;
    completed += length;
  }
  std::size_t completed = 0;
  std::array<JitStoreRange, 2> stored{};
  for (std::size_t page = 0; page < pages; ++page) {
    if (load) {
      std::memcpy(file + completed, host[page], lengths[page]);
    } else {
      std::memcpy(host[page], file + completed, lengths[page]);
      stored[page] = {reinterpret_cast<std::uintptr_t>(host[page]),
                      lengths[page]};
    }
    completed += lengths[page];
  }
  if (!load)
    jit::semantic::invalidate_store_ranges(
        cpu, std::span<const JitStoreRange>{stored}.first(pages));
  return retire(cpu);
}

/*
 * The guest pages a streaming memory access's active bytes lie in, each
 * translated the first time an active byte needs it; no access spans more
 * than two. A store revalidates write permission, as transfer() does, and
 * publish() invalidates the reservations of the bytes it wrote and clears
 * the local monitor.
 */
class Pages final {
public:
  Pages(Tlb &tlb, bool load) noexcept : tlb_{tlb}, load_{load} {}

  /* The first byte of [address, address + size) that does not translate,
   * or nothing when every byte does. */
  [[nodiscard]] std::optional<std::uint64_t>
  unreachable(std::uint64_t address, std::size_t size) noexcept {
    for (std::size_t done = 0; done < size;) {
      const auto current = address + done;
      const auto page = current & ~(page_size - 1U);
      if (find(page) == nullptr) {
        if (count_ == pages_.size())
          return current;
        if (!load_)
          tlb_.discard(current);
        auto *const host =
            tlb_.translate(current, load_ ? Access::read : Access::write);
        if (host == nullptr)
          return current;
        pages_[count_] = page;
        hosts_[count_] = host - (current - page);
        low_[count_] = page_size;
        high_[count_] = 0;
        ++count_;
      }
      done += std::min<std::size_t>(size - done, page_size - (current - page));
    }
    return std::nullopt;
  }

  /* Copies between the bytes at `address`, all reached, and `data`. */
  void move(std::uint64_t address, std::byte *data, std::size_t size) noexcept {
    for (std::size_t done = 0; done < size;) {
      const auto current = address + done;
      const auto page = current & ~(page_size - 1U);
      const auto offset = static_cast<std::size_t>(current - page);
      const auto length = std::min<std::size_t>(size - done, page_size - offset);
      const auto index = slot(page);
      auto *const host = hosts_[index] + offset;
      if (load_) {
        std::memcpy(data + done, host, length);
      } else {
        std::memcpy(host, data + done, length);
        low_[index] = std::min(low_[index], offset);
        high_[index] = std::max(high_[index], offset + length);
      }
      done += length;
    }
  }

  void publish(CpuState &cpu) const noexcept {
    if (load_)
      return;
    std::array<JitStoreRange, 2> stored{};
    std::size_t ranges = 0;
    for (unsigned index = 0; index < count_; ++index)
      if (low_[index] < high_[index])
        stored[ranges++] = {
            reinterpret_cast<std::uintptr_t>(hosts_[index] + low_[index]),
            high_[index] - low_[index]};
    jit::semantic::invalidate_store_ranges(
        cpu, std::span<const JitStoreRange>{stored}.first(ranges));
  }

private:
  [[nodiscard]] std::byte *find(std::uint64_t page) const noexcept {
    for (unsigned index = 0; index < count_; ++index)
      if (pages_[index] == page)
        return hosts_[index];
    return nullptr;
  }

  [[nodiscard]] unsigned slot(std::uint64_t page) const noexcept {
    unsigned index = 0;
    while (index + 1U < count_ && pages_[index] != page)
      ++index;
    return index;
  }

  Tlb &tlb_;
  bool load_;
  std::array<std::uint64_t, 2> pages_{};
  std::array<std::byte *, 2> hosts_{};
  std::array<std::size_t, 2> low_{};
  std::array<std::size_t, 2> high_{};
  unsigned count_ = 0;
};

/* A memory element of `from` bytes at `data`, extended to `to` bytes. */
void extend(std::byte *data, std::size_t from, std::size_t to,
            bool sign) noexcept {
  if (from == to)
    return;
  const bool negative =
      sign && (std::to_integer<unsigned>(data[from - 1U]) & 0x80U) != 0U;
  std::memset(data + from, negative ? 0xff : 0, to - from);
}

/* One streaming SVE or SME memory word, decoded from its row. */
struct VectorAccess final {
  const detail::SmeMemory *memory;
  std::byte *file;
  std::size_t svl;
  std::uint64_t address;
  std::size_t esize;
  std::size_t msize;
  /* The first transfer register, Z or P. */
  unsigned transfer;
  /* The governing predicate, in the register file. */
  const std::byte *predicate;
  /* A ZA slice's array vectors: horizontal slice s of tile t holds array
   * vector s * esize + t; element i of a vertical slice is element s of
   * array vector i * esize + t. */
  unsigned tile;
  std::size_t slice;
  bool vertical;

  [[nodiscard]] std::byte *z(unsigned offset) const noexcept {
    return file + sme_z_offset(svl, (transfer + offset) % 32U);
  }

  [[nodiscard]] bool active(std::size_t element) const noexcept {
    const auto bit = element * esize;
    return ((std::to_integer<unsigned>(predicate[bit / 8U]) >> (bit % 8U)) &
            1U) != 0U;
  }
};

/*
 * Visits each element the access transfers, in element order, which is
 * also address order: visit(address, bytes, register bytes, active). A load
 * writes `register bytes` from memory or, for an inactive element, zeroes
 * them; a store writes memory from them.
 */
template <typename Visit>
void elements(const VectorAccess &access, Visit &&visit) noexcept {
  const auto svl = access.svl;
  const auto esize = access.esize;
  const auto count = svl / esize;
  switch (access.memory->shape) {
  case detail::SmeMemoryShape::contiguous:
    for (std::size_t element = 0; element < count; ++element)
      visit(access.address + element * access.msize, access.msize,
            access.z(0) + element * esize, access.active(element));
    break;
  case detail::SmeMemoryShape::structure: {
    const unsigned registers = access.memory->registers;
    for (std::size_t element = 0; element < count; ++element)
      for (unsigned index = 0; index < registers; ++index)
        visit(access.address + (element * registers + index) * esize, esize,
              access.z(index) + element * esize, access.active(element));
    break;
  }
  case detail::SmeMemoryShape::za_slice:
    for (std::size_t element = 0; element < count; ++element) {
      const auto row =
          (access.vertical ? element : access.slice) * esize + access.tile;
      const auto column = access.vertical ? access.slice : element;
      visit(access.address + element * esize, esize,
            access.file + sme_za_offset(svl, static_cast<unsigned>(row)) +
                column * esize,
            access.active(element));
    }
    break;
  default:
    break;
  }
}

/*
 * Every streaming SVE and SME1 memory word whose translation cannot finish
 * it natively: an access that crosses a page, misses the TLB or faults, an
 * SP base that is not 16-byte aligned, or a tagged base. It runs on the
 * register file and touches only active elements; every page they lie in
 * is translated before any byte moves, so a fault leaves memory and the
 * registers unchanged. The fault is reported as the M4 reports it (the
 * native S3b records): a predicated access at its first faulting active
 * byte rounded down to 4 KiB, an unpredicated one (LDR and STR of a vector
 * or a predicate) at that byte. A load with no active element accesses
 * nothing and cannot fault.
 */
[[nodiscard]] std::uint64_t vector_memory(CpuState &cpu, Tlb &tlb,
                                          std::uint32_t instruction,
                                          const detail::SmeEncoding &row) noexcept {
  using Shape = detail::SmeMemoryShape;
  const auto &memory = *detail::sme_encoding_memory_of(row);
  /* A prefetch accesses nothing and cannot fault. */
  if (memory.shape == Shape::prefetch)
    return retire(cpu);
  const auto *operands = detail::sme_encoding_operands_of(row);
  const bool load = memory.direction != detail::SmeMemoryDirection::store;
  const std::size_t svl = cpu.sme_svl;
  const unsigned base =
      detail::sme_operand_number(operands[memory.base_operand], instruction);
  const auto tagged = cpu.read_register(base, true);
  /* CheckSPAlignment() comes before any access. */
  if (base == 31U && (tagged & 15U) != 0U)
    return report_fault(tlb, tagged, load, true);
  std::uint64_t index = 0;
  if (memory.index_operand != detail::sme_no_operand)
    index = cpu.read_register(
        detail::sme_operand_number(operands[memory.index_operand],
                                   instruction),
        false);

  VectorAccess access{};
  access.memory = &memory;
  access.file = cpu.sme_registers;
  access.svl = svl;
  access.address = cpu.untag_data_address(
      tagged + jit::sme_memory_offset(memory, instruction, svl, index));
  access.esize = jit::sme_memory_element_bytes(memory, instruction);
  access.msize = memory.msize / 8U;
  for (unsigned operand = 0; operand < row.operand_count; ++operand) {
    const auto &candidate = operands[operand];
    const auto number = detail::sme_operand_number(candidate, instruction);
    if (candidate.kind == detail::SmeOperandKind::z)
      access.transfer = number;
    else if (candidate.kind == detail::SmeOperandKind::p &&
             memory.shape == Shape::fill_p)
      access.transfer = number;
    else if (candidate.kind == detail::SmeOperandKind::p)
      access.predicate =
          access.file + sme_p_offset(svl, static_cast<unsigned>(number));
    else if (candidate.kind == detail::SmeOperandKind::gpr &&
             candidate.segment_count == 2U && !candidate.segments[0].field)
      /* The slice index register, W12-W15. */
      access.slice = static_cast<std::uint32_t>(cpu.registers[number]);
  }
  if (memory.shape == Shape::za_slice) {
    /* Bits 3:0 hold the tile, then the slice offset, in that order of
     * significance: log2(esize) bits of tile. */
    const unsigned tile_bits =
        static_cast<unsigned>(std::countr_zero(access.esize));
    const unsigned low = instruction & 15U;
    access.tile = low >> (4U - tile_bits);
    access.slice = (access.slice + (low & ((1U << (4U - tile_bits)) - 1U))) %
                   (svl / access.esize);
    access.vertical = (instruction & 0x8000U) != 0U;
  }

  Pages pages{tlb, load};
  std::optional<std::uint64_t> fault;
  bool predicated = true;
  switch (memory.shape) {
  case Shape::contiguous:
  case Shape::structure:
  case Shape::za_slice:
    elements(access, [&](std::uint64_t at, std::size_t size, std::byte *,
                         bool active) noexcept {
      if (active && !fault)
        fault = pages.unreachable(at, size);
    });
    if (fault)
      break;
    elements(access, [&](std::uint64_t at, std::size_t size, std::byte *data,
                         bool active) noexcept {
      if (!load) {
        if (active)
          pages.move(at, data, size);
        return;
      }
      const auto lane =
          memory.shape == Shape::contiguous ? access.esize : size;
      if (!active) {
        std::memset(data, 0, lane);
        return;
      }
      pages.move(at, data, size);
      extend(data, size, lane, memory.sign_extend);
    });
    break;
  case Shape::replicate: {
    /* LD1R: one element when any element is active, to every active
     * element. */
    const auto count = svl / access.esize;
    bool any = false;
    for (std::size_t element = 0; element < count; ++element)
      any = any || access.active(element);
    std::array<std::byte, 8> value{};
    if (any) {
      fault = pages.unreachable(access.address, access.msize);
      if (fault)
        break;
      pages.move(access.address, value.data(), access.msize);
      extend(value.data(), access.msize, access.esize, memory.sign_extend);
    }
    auto *const z = access.z(0);
    for (std::size_t element = 0; element < count; ++element) {
      if (access.active(element))
        std::memcpy(z + element * access.esize, value.data(), access.esize);
      else
        std::memset(z + element * access.esize, 0, access.esize);
    }
    break;
  }
  case Shape::replicate_quad: {
    /* LD1RQ: the active elements of the first quadword, in every
     * quadword. */
    std::array<std::byte, 16> quad{};
    const auto count = quad.size() / access.esize;
    for (std::size_t element = 0; element < count && !fault; ++element)
      if (access.active(element))
        fault = pages.unreachable(access.address + element * access.esize,
                                  access.esize);
    if (fault)
      break;
    for (std::size_t element = 0; element < count; ++element)
      if (access.active(element))
        pages.move(access.address + element * access.esize,
                   quad.data() + element * access.esize, access.esize);
    auto *const z = access.z(0);
    for (std::size_t offset = 0; offset < svl; offset += quad.size())
      std::memcpy(z + offset, quad.data(), quad.size());
    break;
  }
  case Shape::fill_z:
  case Shape::fill_p: {
    predicated = false;
    const auto size = memory.shape == Shape::fill_z ? svl : svl / 8U;
    auto *const data =
        memory.shape == Shape::fill_z
            ? access.z(0)
            : access.file + sme_p_offset(svl, access.transfer);
    fault = pages.unreachable(access.address, size);
    if (!fault)
      pages.move(access.address, data, size);
    break;
  }
  default:
    return pack_jit_result(Event::fatal, 0);
  }
  if (fault)
    return report_fault(tlb, predicated ? *fault & ~std::uint64_t{0xfff} : *fault,
                        load, false);
  pages.publish(cpu);
  return retire(cpu);
}

/*
 * A streaming SIMD&FP load or store the translation could not finish
 * natively. A load writes each register's low bytes and zeroes the rest of
 * its Z register, as the V register write does; a fault is at the first
 * byte that does not translate, and leaves memory, the registers and the
 * base unchanged.
 */
[[nodiscard]] std::uint64_t fp_memory(CpuState &cpu, Tlb &tlb,
                                      const jit::FpMemory &memory) noexcept {
  using Mode = jit::FpMemoryMode;
  const std::size_t svl = cpu.sme_svl;
  std::uint64_t base = 0;
  std::uint64_t address;
  if (memory.mode == Mode::literal) {
    address = cpu.pc + static_cast<std::uint64_t>(memory.immediate);
  } else {
    base = cpu.read_register(memory.base, true);
    if (memory.base == 31U && (base & 15U) != 0U)
      return report_fault(tlb, base, memory.load, true);
    std::uint64_t offset = 0;
    if (memory.mode == Mode::register_offset) {
      auto index = cpu.read_register(memory.index, false);
      if ((memory.option & 3U) == 2U)
        index = (memory.option & 4U) != 0U
                    ? static_cast<std::uint64_t>(std::int64_t{
                          static_cast<std::int32_t>(index)})
                    : index & 0xffff'ffffU;
      offset = index << memory.shift;
    } else if (memory.mode != Mode::post_index) {
      offset = static_cast<std::uint64_t>(memory.immediate);
    }
    address = cpu.untag_data_address(base + offset);
  }
  Pages pages{tlb, memory.load};
  if (const auto fault = pages.unreachable(
          address, std::size_t{memory.bytes} * memory.registers))
    return report_fault(tlb, *fault, memory.load, false);
  const unsigned transfers[] = {memory.transfer, memory.transfer2};
  for (unsigned index = 0; index < memory.registers; ++index) {
    auto *const z = cpu.sme_registers + sme_z_offset(svl, transfers[index]);
    if (memory.load)
      std::memset(z, 0, svl);
    pages.move(address + index * memory.bytes, z, memory.bytes);
  }
  if (memory.mode == Mode::pre_index || memory.mode == Mode::post_index)
    cpu.write_register(memory.base,
                       base + static_cast<std::uint64_t>(memory.immediate),
                       true);
  pages.publish(cpu);
  return retire(cpu);
}

} // namespace

extern "C" std::uint64_t
uml_arm64_jit_write_svcr(CpuState *cpu, Tlb *tlb,
                         std::uint32_t instruction) noexcept {
  if (cpu == nullptr || tlb == nullptr || cpu->sme_registers == nullptr)
    return pack_jit_result(Event::fatal, 0);
  std::uint8_t mask = svcr_mask;
  std::uint8_t value = 0;
  if ((instruction & 0xffff'ffe0U) == 0xd51b'4240U) {
    /* MSR SVCR, Xt: SM from bit 0, ZA from bit 1. */
    value = static_cast<std::uint8_t>(
        cpu->read_register(instruction & 31U, false) & svcr_mask);
  } else if ((instruction & 0xffff'f0ffU) == 0xd503'407fU) {
    /* MSR SVCRSM, SVCRZA or SVCRSMZA, #imm: CRm<2:1> selects, CRm<0> is
     * the value. Translation routes only those three selections here. */
    const unsigned field = (instruction >> 8) & 15U;
    mask = static_cast<std::uint8_t>(((field >> 1) & 1U ? svcr_sm : 0U) |
                                     ((field >> 2) & 1U ? svcr_za : 0U));
    if (mask == 0U || field > 7U)
      return pack_jit_result(Event::fatal, 0);
    value = (field & 1U) != 0U ? mask : std::uint8_t{0};
  } else {
    return pack_jit_result(Event::fatal, 0);
  }
  /* write_svcr() needs CpuState to hold the newest guest FPSR. */
  materialize(*cpu);
  if (!cpu->write_svcr(mask, value))
    return pack_jit_result(Event::sme_access, 0);
  return retire(*cpu);
}

extern "C" std::uint64_t
uml_arm64_jit_execute_sme(CpuState *cpu, Tlb *tlb,
                          std::uint32_t instruction) noexcept {
  if (cpu == nullptr || tlb == nullptr || cpu->sme_registers == nullptr ||
      !valid_streaming_vector_length(cpu->sme_svl))
    return pack_jit_result(Event::fatal, 0);
  if (const auto fp = jit::decode_streaming_fp_memory(instruction)) {
    materialize(*cpu);
    return fp_memory(*cpu, *tlb, *fp);
  }
  const auto *row = detail::find_sme_encoding(instruction);
  if (row == nullptr)
    return pack_jit_result(Event::fatal, 0);
  materialize(*cpu);
  const std::size_t svl = cpu->sme_svl;
  auto *const za = cpu->sme_registers + sme_za_offset(svl, 0);
  auto *const zt0 = cpu->sme_registers + sme_zt0_offset(svl);
  const auto form = row->form;
  if (row->memory != 0U &&
      jit::implements_sme_memory(*detail::sme_encoding_memory_of(*row)))
    return vector_memory(*cpu, *tlb, instruction, *row);
  if (form == jit::sme_forms::zero_za) {
    const unsigned tiles = instruction & 0xffU;
    for (std::size_t vector = 0; vector < svl; ++vector)
      if (((tiles >> (vector % 8U)) & 1U) != 0U)
        std::memset(za + vector * svl, 0, svl);
    return retire(*cpu);
  }
  if (form == jit::sme_forms::zero_zt0) {
    std::memset(zt0, 0, sme_zt0_bytes);
    return retire(*cpu);
  }
  const bool array_vector =
      form == jit::sme_forms::ldr_za || form == jit::sme_forms::str_za;
  if (!array_vector && form != jit::sme_forms::ldr_zt0 &&
      form != jit::sme_forms::str_zt0)
    return pack_jit_result(Event::fatal, 0);
  const bool load =
      form == jit::sme_forms::ldr_za || form == jit::sme_forms::ldr_zt0;
  const unsigned base = (instruction >> 5) & 31U;
  const auto tagged = cpu->read_register(base, true);
  /* CheckSPAlignment() comes before any access. */
  if (base == 31U && (tagged & 15U) != 0U)
    return report_fault(*tlb, tagged, load, true);
  auto address = cpu->untag_data_address(tagged);
  if (!array_vector)
    return transfer(*cpu, *tlb, address, zt0, sme_zt0_bytes, load);
  /* LDR and STR ZA[Wv, #off4], [Xn|SP, #off4, MUL VL]: one horizontal ZA
   * vector, (Wv + off4) MOD SVL, at Xn + off4 * SVL. */
  const unsigned offset = instruction & 15U;
  const auto index = static_cast<std::uint32_t>(
      cpu->registers[12U + ((instruction >> 13) & 3U)]);
  const auto vector = (static_cast<std::size_t>(index) + offset) % svl;
  address += offset * svl;
  return transfer(*cpu, *tlb, address, za + vector * svl, svl, load);
}

} // namespace uml::cpu::arm64
