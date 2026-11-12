// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

/* A fixture for the JIT's AArch32 tests: a code region and a data region
 * mapped into a static address space, a CpuState in key form, and an entry
 * through CpuEngine::run_aarch32. Programs are A32 words or T32 halfwords
 * assembled with LLVM MC at authoring time. */

#include "jit_test_support.hpp"
#include "static_mmu_test_support.hpp"

#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "cpu/engine.hpp"
#include "cpu/engine_factory.hpp"
#include "memory/static_guest_memory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <span>
#include <vector>

namespace uml::emulator::tests::a32 {

namespace arm64 = ::uml::cpu::arm64;
namespace key = ::uml::cpu::arm64::aarch32;

inline constexpr std::uint32_t code_base = 0x4f00'0000U;
inline constexpr std::uint32_t data_base = 0x5100'0000U;
inline constexpr std::size_t guest_page = DARWIN_UML_GUEST_PAGE_SIZE;

/* Condition-flag bits of CpuState::nzcv. */
inline constexpr std::uint32_t flag_n = 1U << 31;
inline constexpr std::uint32_t flag_z = 1U << 30;
inline constexpr std::uint32_t flag_c = 1U << 29;
inline constexpr std::uint32_t flag_v = 1U << 28;

class Fixture final {
public:
  explicit Fixture(unsigned long generation, std::size_t code_pages = 2,
                   std::size_t data_pages = 2)
      : generation_{generation}, code_{code_pages}, data_{data_pages},
        engine_{::uml::cpu::make_cpu_engine(::uml::cpu::EngineKind::jit)} {
    require(darwin_uml_static_vm_activate(generation_) == 0);
    require(darwin_uml_static_vm_map_generation(
                generation_, reinterpret_cast<void *>(std::uintptr_t{code_base}),
                code_.descriptor(), 0, code_pages * guest_page,
                PROT_READ | PROT_WRITE | PROT_EXEC, 1, 1) == 0);
    require(darwin_uml_static_vm_map_generation(
                generation_, reinterpret_cast<void *>(std::uintptr_t{data_base}),
                data_.descriptor(), 0, data_pages * guest_page,
                PROT_READ | PROT_WRITE, 1, 1) == 0);
    require(engine_ != nullptr);
  }

  ~Fixture() { require(darwin_uml_static_vm_destroy(generation_) == 0); }

  Fixture(const Fixture &) = delete;
  Fixture &operator=(const Fixture &) = delete;

  void code32(std::size_t offset, std::initializer_list<std::uint32_t> words) {
    const auto first = offset;
    for (const auto word : words) {
      code_.put32(offset, word);
      offset += 4;
    }
    code_written(first, offset - first);
  }
  void code16(std::size_t offset, std::initializer_list<std::uint16_t> halves) {
    const auto first = offset;
    for (const auto half : halves) {
      code_.put(offset, std::as_bytes(std::span{&half, 1}));
      offset += 2;
    }
    code_written(first, offset - first);
  }
  /* The first code page again at `address`, read-only and executable, as the
   * kernel's page of kuser helpers is. */
  void map_code_alias(std::uint32_t address) {
    require(darwin_uml_static_vm_map_generation(
                generation_, reinterpret_cast<void *>(std::uintptr_t{address}),
                code_.descriptor(), 0, guest_page, PROT_READ | PROT_EXEC, 1,
                1) == 0);
  }
  /* Page `page` of the data again at `address` (data_pages permitting), so a
   * test can put two pages next to each other in the guest that are not next
   * to each other in the host. */
  void map_data_alias(std::uint32_t address, std::size_t page) {
    require(darwin_uml_static_vm_map_generation(
                generation_, reinterpret_cast<void *>(std::uintptr_t{address}),
                data_.descriptor(), page * guest_page, guest_page,
                PROT_READ | PROT_WRITE, 1, 1) == 0);
  }
  void code_byte(std::size_t offset, std::byte value) {
    code_.put(offset, std::span{&value, 1});
    code_written(offset, 1);
  }
  void data32(std::size_t offset, std::uint32_t value) {
    data_.put32(offset, value);
  }
  void data_bytes(std::size_t offset, std::span<const std::byte> bytes) {
    data_.put(offset, bytes);
  }
  void read_code(std::size_t offset, std::span<std::byte> bytes) const {
    code_.get(offset, bytes);
  }
  [[nodiscard]] std::vector<std::byte> read_data(std::size_t offset,
                                                 std::size_t size) const {
    std::vector<std::byte> bytes(size);
    data_.get(offset, bytes);
    return bytes;
  }
  [[nodiscard]] std::uint32_t read32(std::size_t offset) const {
    std::uint32_t value;
    data_.get(offset, std::as_writable_bytes(std::span{&value, 1}));
    return value;
  }
  [[nodiscard]] std::uint8_t read8(std::size_t offset) const {
    std::uint8_t value;
    data_.get(offset, std::as_writable_bytes(std::span{&value, 1}));
    return value;
  }

  /* A task about to run the A32 or T32 code at `offset`. */
  [[nodiscard]] static arm64::CpuState task(bool thumb, std::size_t offset = 0) {
    arm64::CpuState cpu{};
    cpu.a32 = 1;
    cpu.pc = thumb ? key::t32_key(code_base + static_cast<std::uint32_t>(offset))
                   : key::a32_key(code_base + static_cast<std::uint32_t>(offset));
    cpu.registers[13] = data_base + static_cast<std::uint32_t>(guest_page);
    return cpu;
  }

  ::uml::cpu::ExecutionResult run(arm64::CpuState &cpu,
                                  std::size_t budget = 4096,
                                  bool single_step = false) {
    return run_on(*engine_, cpu, budget, single_step);
  }

  /* A further engine over the same address space, for a second thread. */
  [[nodiscard]] static std::unique_ptr<::uml::cpu::CpuEngine> new_engine() {
    return ::uml::cpu::make_cpu_engine(::uml::cpu::EngineKind::jit);
  }

  ::uml::cpu::ExecutionResult run_on(::uml::cpu::CpuEngine &engine,
                                     arm64::CpuState &cpu,
                                     std::size_t budget = 4096,
                                     bool single_step = false) {
    ::uml::memory::StaticGuestMemory memory;
    const auto sequences = darwin_uml_static_vm_sequences(generation_);
    memory.prepare(generation_, sequences.mapping_changes);
    ::uml::cpu::ExecutionRequest request{
        .cpu = cpu,
        .memory = memory,
        .translated_memory = &memory,
        .generation = generation_,
        .instruction_budget = budget,
        .single_step = single_step,
    };
    return engine.run_aarch32(request);
  }

  [[nodiscard]] unsigned long generation() const noexcept { return generation_; }

private:
  /* A guest store to code publishes this; a write through the backing does
   * not, so a test that rewrites code says so. */
  void code_written(std::size_t offset, std::size_t length) {
    require(darwin_uml_static_vm_invalidate_code_generation(
                generation_, code_base + offset, length) == 0);
  }

  unsigned long generation_;
  JitBacking code_;
  JitBacking data_;
  std::unique_ptr<::uml::cpu::CpuEngine> engine_;
};

/* Prints the host words of the translation of `bytes` at `key` as one hex
 * word per line, which llvm-mc --disassemble reads (a debugging aid). */
inline void dump_translation(std::uint64_t key,
                             std::span<const std::byte> bytes,
                             std::uint8_t itstate = 0, bool single = false) {
  const auto compiled =
      ::uml::cpu::arm64::jit::testing::compile_a32_block(key, bytes, itstate,
                                                         single);
  std::fprintf(stderr, "units=%zu instructions=%zu hot=%zu exec=%zu\n",
               compiled.guest_units, compiled.code.guest_instructions,
               compiled.code.hot_words, compiled.code.executable_words);
  for (const auto word : compiled.code.words)
    std::fprintf(stderr, "0x%02x 0x%02x 0x%02x 0x%02x\n", word & 0xffU,
                 word >> 8 & 0xffU, word >> 16 & 0xffU, word >> 24 & 0xffU);
}

} // namespace uml::emulator::tests::a32
