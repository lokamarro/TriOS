// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_engine_test_support.hpp"

#include "cpu/arm64/floating_controls.hpp"
#include "cpu/floating_point_contract.hpp"
#include "uml_darwin_guest.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>

namespace arm64 = uml::cpu::arm64;
namespace engine = uml::emulator::tests;

namespace {

constexpr std::size_t transfer_offset = 64;
// The FPCR fields MSR FPCR and a context restore can set on this host.
[[nodiscard]] std::uint32_t fpcr_mask() noexcept {
  return arm64::fpcr_writable_mask_for(::uml::cpu::host_capabilities());
}
constexpr std::uint32_t fpsr_mask = arm64::fpsr_writable_mask;

[[nodiscard]] engine::NativeRequirements native_requirements() {
  return engine::NativeRequirements::features(
      ::uml::cpu::ArmFeature::floating_point);
}

[[nodiscard]] std::array<arm64::VectorRegister, 32> deferred_vectors() {
  std::array<arm64::VectorRegister, 32> vectors{};
  for (std::size_t reg = 0; reg < vectors.size(); ++reg)
    for (std::size_t byte = 0; byte < vectors[reg].bytes.size(); ++byte)
      vectors[reg].bytes[byte] =
          static_cast<std::byte>((reg * 41U + byte * 17U + 3U) & 0xffU);
  return vectors;
}

[[nodiscard]] arm64::CpuState
deferred_state(const std::array<arm64::VectorRegister, 32> &vectors,
               std::uint32_t fpcr, std::uint32_t fpsr) {
  arm64::CpuState cpu{};
  for (std::size_t index = 0; index < cpu.registers.size(); ++index)
    cpu.registers[index] =
        0x1234'5000'0000'0000ULL | static_cast<std::uint64_t>(index);
  cpu.defer_fp_load(vectors.data(), fpcr, fpsr, fpcr_mask());
  return cpu;
}

void deferred_vector_store(engine::EngineHarness &harness) {
  const auto vectors = deferred_vectors();
  auto initial = deferred_state(vectors, 0x0040'0000U, 0x0800'0001U);
  initial.registers[1] = engine::engine_test_data_base + transfer_offset;
  auto expected = initial;
  expected.access_fp_state();

  std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> initial_page{};
  auto expected_page = initial_page;
  std::memcpy(expected_page.data() + transfer_offset, vectors[0].bytes.data(),
              vectors[0].bytes.size());
  const engine::MemoryRegion memory{
      .address = engine::engine_test_data_base,
      .initial = std::span<const std::byte>{initial_page},
      .expected = std::span<const std::byte>{expected_page},
  };

  harness.check({
      .name = "deferred-vector-store",
      .instruction = 0x3d80'0020U, // STR Q0, [X1]
      .initial = initial,
      .expected = expected,
      .native = native_requirements(),
      .memory = std::span<const engine::MemoryRegion>{&memory, 1},
  });
}

void deferred_vector_load(engine::EngineHarness &harness) {
  const auto vectors = deferred_vectors();
  auto initial = deferred_state(vectors, 0x0080'0000U, 0x0800'0002U);
  initial.registers[1] = engine::engine_test_data_base + transfer_offset;

  std::array<std::byte, DARWIN_UML_GUEST_PAGE_SIZE> page{};
  std::array<std::byte, 16> loaded{};
  for (std::size_t index = 0; index < loaded.size(); ++index)
    loaded[index] = static_cast<std::byte>(0xf0U - index * 7U);
  std::memcpy(page.data() + transfer_offset, loaded.data(), loaded.size());
  const engine::MemoryRegion memory{
      .address = engine::engine_test_data_base,
      .initial = std::span<const std::byte>{page},
  };

  auto expected = initial;
  expected.access_fp_state();
  expected.vectors[2].bytes = loaded;
  harness.check({
      .name = "deferred-vector-load",
      .instruction = 0x3dc0'0022U, // LDR Q2, [X1]
      .initial = initial,
      .expected = expected,
      .native = native_requirements(),
      .memory = std::span<const engine::MemoryRegion>{&memory, 1},
  });
}

void deferred_control_reads(engine::EngineHarness &harness) {
  const auto vectors = deferred_vectors();
  constexpr std::uint32_t deferred_fpcr = 0x0540'0000U;
  constexpr std::uint32_t deferred_fpsr = 0x8800'001fU;
  const auto initial =
      deferred_state(vectors, deferred_fpcr, deferred_fpsr);

  auto expected = initial;
  expected.access_fp_state();
  expected.registers[3] = deferred_fpcr & fpcr_mask();
  harness.check({
      .name = "deferred-control-read",
      .instruction = 0xd53b'4403U, // MRS X3, FPCR
      .initial = initial,
      .expected = expected,
      .native = native_requirements(),
  });

  expected = initial;
  expected.access_fp_state();
  expected.registers[4] = deferred_fpsr & fpsr_mask;
  harness.check({
      .name = "deferred-status-read",
      .instruction = 0xd53b'4424U, // MRS X4, FPSR
      .initial = initial,
      .expected = expected,
      .native = native_requirements(),
  });
}

void deferred_restore_masks_control(engine::EngineHarness &harness) {
  // A signal-frame or ptrace restore reaches the engines through
  // defer_fp_load. It must mask the saved FPCR as MSR FPCR does: Len and
  // Stride (no AArch32), EBF (no FEAT_EBF16) and the RES0 bits read as zero,
  // and so do FIZ, AH and NEP unless the virtual CPU implements FEAT_AFP on
  // this host, while FZ16, RMode, FZ, DN and AHP stay set. The trap enables
  // stay clear here; deferred-trap-enable covers them.
  constexpr std::uint32_t restored_fpcr = 0xffff'60ffU;
  const std::uint32_t architectural_fpcr =
      0x07c8'0000U | (restored_fpcr & fpcr_mask() & arm64::fpcr_afp_controls);
  const auto vectors = deferred_vectors();
  const auto initial = deferred_state(vectors, restored_fpcr, 0U);
  if (initial.deferred_fpcr != architectural_fpcr ||
      initial.effective_fpcr() != architectural_fpcr)
    std::abort();

  auto expected = initial;
  expected.access_fp_state();
  expected.fpcr = architectural_fpcr;
  expected.registers[3] = architectural_fpcr;
  harness.check({
      .name = "deferred-restore-masks-control",
      .instruction = 0xd53b'4403U, // MRS X3, FPCR
      .initial = initial,
      .expected = expected,
      .native = native_requirements(),
  });
}

void deferred_control_writes_persist(engine::EngineHarness &harness) {
  const auto vectors = deferred_vectors();
  auto initial = deferred_state(vectors, 0x0040'0000U, 0x8000'0001U);
  initial.registers[5] = 0x0440'0000U;
  initial.registers[6] = 0x0800'001fU;

  auto expected = initial;
  expected.access_fp_state();
  expected.fpcr = static_cast<std::uint32_t>(initial.registers[5]) & fpcr_mask();
  expected.fpsr = static_cast<std::uint32_t>(initial.registers[6]) & fpsr_mask;
  expected.registers[7] = expected.fpcr;
  expected.registers[8] = expected.fpsr;
  expected.pc = engine::engine_test_code_base + 5 * sizeof(std::uint32_t);

  const std::array code{
      engine::ProgramWord{.offset = 0, .instruction = 0xd51b'4405U},
      engine::ProgramWord{.offset = 4, .instruction = 0xd51b'4426U},
      engine::ProgramWord{.offset = 8, .instruction = 0xd53b'4407U},
      engine::ProgramWord{.offset = 12, .instruction = 0xd53b'4428U},
      engine::ProgramWord{.offset = 16, .instruction = 0xd400'0001U},
  };
  harness.check_program({
      .name = "deferred-control-write-persistence",
      .code = std::span<const engine::ProgramWord>{code},
      .initial = initial,
      .expected = expected,
      .native = native_requirements(),
      .expected_instructions = 5,
  });
}

void deferred_trap_enable(engine::EngineHarness &harness) {
  // FDIV S0, S1, S2 divides one by zero. Only the deferred FPCR enables the
  // divide-by-zero trap, so the engine must load the deferred state before it
  // decides that the instruction traps, and must then commit nothing.
  constexpr std::uint32_t divide_by_zero = 1U << 1U;
  constexpr std::uint32_t deferred_fpcr = divide_by_zero << 8U;
  constexpr std::uint32_t deferred_fpsr = (1U << 27U) | (1U << 7U);
  auto vectors = deferred_vectors();
  constexpr std::uint32_t one = 0x3f80'0000U;
  constexpr std::uint32_t zero = 0U;
  std::memcpy(vectors[1].bytes.data(), &one, sizeof(one));
  std::memcpy(vectors[2].bytes.data(), &zero, sizeof(zero));
  const auto initial = deferred_state(vectors, deferred_fpcr, deferred_fpsr);
  auto expected = initial;
  expected.access_fp_state();

  harness.check({
      .name = "deferred-trap-enable",
      .instruction = 0x1e22'1820U, // FDIV S0, S1, S2
      .initial = initial,
      .expected = expected,
      .event = engine::ExpectedEvent::floating_point_trap,
      .native = native_requirements(),
      .syndrome = ::uml::cpu::floating_point_syndrome(divide_by_zero),
  });
}

void deferred_state_survives_integer_block(engine::EngineHarness &harness) {
  const auto vectors = deferred_vectors();
  const auto initial = deferred_state(vectors, 0x0440'0000U, 0x8800'001fU);
  auto expected = initial;
  expected.registers[9] = 0x1234U;

  harness.check({
      .name = "deferred-state-integer-block",
      .instruction = 0xd282'4689U, // MOVZ X9, #0x1234
      .initial = initial,
      .expected = expected,
      .native = engine::NativeRequirements::baseline(),
  });
}

} // namespace

int main() {
  engine::EngineHarness harness;
  deferred_vector_store(harness);
  deferred_vector_load(harness);
  deferred_control_reads(harness);
  deferred_restore_masks_control(harness);
  deferred_control_writes_persist(harness);
  deferred_trap_enable(harness);
  deferred_state_survives_integer_block(harness);
  harness.print_summary("arm64-deferred-fp-state");
  const auto &counts = harness.counts();
  if (counts.gadgets != 8 ||
      counts.jit + counts.jit_skipped != 8)
    return 1;
  return 0;
}
