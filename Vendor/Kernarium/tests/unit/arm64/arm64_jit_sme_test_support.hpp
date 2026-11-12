// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/*
 * A vCPU with SME task storage for the JIT's SME run-time tests
 * (arm64-jit-sme-control and arm64-jit-sme-register), with the encoders,
 * patterns and mode lists both use.
 */
#include "jit_test_support.hpp"
#include "cpu/arm64/fp_environment.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/sme_profile.hpp"
#include "cpu/host_capabilities.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <span>
#include <vector>

namespace uml::emulator::tests::jit_sme {

namespace arm64 = uml::cpu::arm64;
using arm64::JitExecutor;
using uml::memory::StaticGuestMemory;
using Event = ::uml::cpu::EngineEvent;

inline constexpr std::size_t page = DARWIN_UML_GUEST_PAGE_SIZE;
inline constexpr std::uint32_t system_call = 0xd400'0001U;
inline constexpr std::uint32_t smstart = 0xd503'477fU;
inline constexpr std::uint32_t smstart_sm = 0xd503'437fU;
inline constexpr std::uint32_t smstart_za = 0xd503'457fU;
inline constexpr std::uint32_t smstop = 0xd503'467fU;
inline constexpr std::uint32_t smstop_sm = 0xd503'427fU;
inline constexpr std::uint32_t smstop_za = 0xd503'447fU;

[[nodiscard]] constexpr std::uint32_t mrs_svcr(unsigned t) noexcept {
  return 0xd53b'4240U | t;
}
[[nodiscard]] constexpr std::uint32_t msr_svcr(unsigned t) noexcept {
  return 0xd51b'4240U | t;
}
[[nodiscard]] constexpr std::uint32_t mrs_tpidr2(unsigned t) noexcept {
  return 0xd53b'd0a0U | t;
}
[[nodiscard]] constexpr std::uint32_t msr_tpidr2(unsigned t) noexcept {
  return 0xd51b'd0a0U | t;
}
[[nodiscard]] constexpr std::uint32_t rdsvl(unsigned d, int imm) noexcept {
  return 0x04bf'5800U | ((static_cast<unsigned>(imm) & 63U) << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t addsvl(unsigned d, unsigned n,
                                             int imm) noexcept {
  return 0x0420'5800U | (n << 16) |
         ((static_cast<unsigned>(imm) & 63U) << 5) | d;
}
[[nodiscard]] constexpr std::uint32_t addspl(unsigned d, unsigned n,
                                             int imm) noexcept {
  return 0x0460'5800U | (n << 16) |
         ((static_cast<unsigned>(imm) & 63U) << 5) | d;
}
/* LDR or STR ZA[W(12 + v), #off], [Xn|SP, #off, MUL VL]. */
[[nodiscard]] constexpr std::uint32_t za_vector(bool load, unsigned v,
                                                unsigned n,
                                                unsigned off) noexcept {
  return (load ? 0xe100'0000U : 0xe120'0000U) | (v << 13) | (n << 5) | off;
}
[[nodiscard]] constexpr std::uint32_t zt0_vector(bool load,
                                                 unsigned n) noexcept {
  return (load ? 0xe11f'8000U : 0xe13f'8000U) | (n << 5);
}
[[nodiscard]] constexpr std::uint32_t zero_za(unsigned mask) noexcept {
  return 0xc008'0000U | (mask & 0xffU);
}
inline constexpr std::uint32_t zero_zt0 = 0xc048'0001U;
[[nodiscard]] constexpr std::uint32_t load_x(unsigned t, unsigned n) noexcept {
  return 0xf940'0000U | (n << 5) | t;
}
[[nodiscard]] constexpr std::uint32_t branch(int words) noexcept {
  return 0x1400'0000U | (static_cast<std::uint32_t>(words) & 0x03ff'ffffU);
}
/* CPYFP, CPYFM and CPYFE [X0]!, [X1]!, X2!. */
inline constexpr std::array<std::uint32_t, 3> copy_forward{
    0x1901'0440U, 0x1941'0440U, 0x1981'0440U};
inline constexpr std::uint32_t initial_fpcr = 0x0040'0000U; // RMode +inf
inline constexpr std::uint32_t initial_fpsr = 0x0800'0000U; // QC
inline constexpr std::uint32_t initial_nzcv = 0x6000'0000U; // Z, C
inline constexpr unsigned stack_pointer_index = 31;

[[nodiscard]] inline std::byte pattern(std::size_t index,
                                      unsigned seed) noexcept {
  return static_cast<std::byte>((index * 7U + seed * 31U + 1U) & 0xffU);
}

/*
 * A vCPU with SME task storage, guest code on page 0 and data on the pages
 * after it, run through one executor, on the host profile current at
 * construction. Every GPR, SP, NZCV and FPCR starts with a pattern a test
 * may override before its first run; require_preserved() checks the rest.
 * The register file is the state to check after a run: every translation
 * leaves streaming mode on exit.
 */
class Machine final {
public:
  Machine(unsigned long generation, std::uint64_t base, std::uint8_t svcr,
          bool storage = true, std::size_t pages = 4,
          std::size_t mapped_pages = 4)
      : generation_{generation}, base_{base}, backing_{pages},
        mapping_{generation, base, backing_, mapped_pages},
        file_{arm64::SmeRegisterFile::for_host(
            uml::cpu::host_capabilities())},
        svl_{file_.svl()}, fp_image_(32U * 16U) {
    require(svl_ != 0U);
    if (storage) {
      task_.resize(arm64::sme_storage_size(svl_));
      for (std::size_t index = 0; index < task_.size(); ++index)
        task_[index] = pattern(index, 1);
    }
    for (std::size_t index = 0; index < fp_image_.size(); ++index)
      fp_image_[index] = pattern(index, 2);
    for (unsigned index = 0; index < cpu.registers.size(); ++index)
      cpu.registers[index] =
          0x5a00'0000'0000'0000ULL | (std::uint64_t{index} << 32) | index;
    cpu.sp = 0x7700'1230;
    cpu.nzcv = initial_nzcv;
    cpu.pc = base;
    cpu.bind_sme_registers(file_);
    cpu.defer_fp_load(fp_image_.data(), initial_fpcr, initial_fpsr, ~0U);
    cpu.defer_sme_load(storage ? task_.data() : nullptr, svcr, 0);
    require(executor_.begin_execution(generation, base));
  }

  ~Machine() { executor_.end_execution(); }

  Machine(const Machine &) = delete;
  Machine &operator=(const Machine &) = delete;

  [[nodiscard]] std::uint64_t data(std::size_t offset = 0) const noexcept {
    return base_ + page + offset;
  }

  void code(std::size_t offset, std::initializer_list<std::uint32_t> words) {
    for (const auto word : words) {
      backing_.put32(offset, word);
      offset += 4U;
    }
  }

  void code(std::size_t offset, std::span<const std::uint32_t> words) {
    for (const auto word : words) {
      backing_.put32(offset, word);
      offset += 4U;
    }
  }

  void put(std::uint64_t address, std::span<const std::byte> bytes) {
    backing_.put(address - base_, bytes);
  }

  [[nodiscard]] std::vector<std::byte> get(std::uint64_t address,
                                           std::size_t size) const {
    std::vector<std::byte> bytes(size);
    backing_.get(address - base_, bytes);
    return bytes;
  }

  /* The register file as the first translation of this mode will see it. */
  [[nodiscard]] std::vector<std::byte> initial_file() {
    cpu.access_sme_state();
    return file();
  }

  [[nodiscard]] std::vector<std::byte> file() const {
    return {file_.data(), file_.data() + arm64::sme_storage_size(svl_)};
  }

  [[nodiscard]] Event run(std::size_t &retired, std::size_t blocks = 64) {
    if (!started_) {
      started_ = true;
      registers_ = cpu.registers;
      sp_ = cpu.sp;
    }
    retired = 0;
    for (std::size_t round = 0; round < blocks; ++round) {
      std::size_t executed = 0;
      const auto event =
          executor_.run_block(cpu, memory_, generation_, executed);
      /* Release builds compile out the engine's own check. */
      require(arm64::host_streaming_state_clear(
          uml::cpu::host_capabilities()));
      retired += executed;
      if (event != Event::continued)
        return event;
    }
    return Event::continued;
  }

  /* Every GPR and SP but `written` (31 for SP) as the first run found them,
   * FPCR and, unless the code sets flags, NZCV as constructed. */
  void require_preserved(std::initializer_list<unsigned> written = {},
                         bool sets_flags = false) {
    const auto is_written = [&](unsigned index) {
      return std::ranges::find(written, index) != written.end();
    };
    for (unsigned index = 0; index < cpu.registers.size(); ++index)
      if (!is_written(index) && cpu.registers[index] != registers_[index]) {
        std::fprintf(stderr, "x%u changed: %#llx\n", index,
                     static_cast<unsigned long long>(cpu.registers[index]));
        require(false);
      }
    require(is_written(stack_pointer_index) || cpu.sp == sp_);
    arm64::restore_native_fp_environment(cpu);
    if ((!sets_flags && cpu.nzcv != initial_nzcv) ||
        cpu.effective_fpcr() != initial_fpcr) {
      std::fprintf(stderr, "pc %#llx svcr %u: nzcv %#x fpcr %#x\n",
                   static_cast<unsigned long long>(cpu.pc), cpu.svcr,
                   cpu.nzcv, cpu.effective_fpcr());
      require(false);
    }
  }

  [[nodiscard]] std::uint32_t guest_fpsr() {
    arm64::restore_native_fp_environment(cpu);
    return cpu.effective_fpsr();
  }

  /* V0-V31 as the FP image held them. */
  [[nodiscard]] bool vectors_unchanged() {
    arm64::restore_native_fp_environment(cpu);
    cpu.access_fp_state();
    return std::memcmp(cpu.vectors.data(), fp_image_.data(),
                       fp_image_.size()) == 0;
  }

  [[nodiscard]] JitExecutor &executor() noexcept { return executor_; }
  [[nodiscard]] unsigned svl() const noexcept { return svl_; }

  arm64::CpuState cpu{};

private:
  unsigned long generation_;
  std::uint64_t base_;
  JitBacking backing_;
  JitMapping mapping_;
  JitExecutor executor_;
  StaticGuestMemory memory_;
  arm64::SmeRegisterFile file_;
  unsigned svl_;
  std::vector<std::byte> task_;
  std::vector<std::byte> fp_image_;
  bool started_{};
  std::array<std::uint64_t, 31> registers_{};
  std::uint64_t sp_{};
};

[[nodiscard]] inline std::vector<std::byte> bytes_of(std::size_t size,
                                              unsigned seed) {
  std::vector<std::byte> bytes(size);
  for (std::size_t index = 0; index < size; ++index)
    bytes[index] = pattern(index, seed);
  return bytes;
}

[[nodiscard]] inline bool all_zero(std::span<const std::byte> bytes) {
  return std::ranges::all_of(bytes, [](std::byte b) { return b == std::byte{}; });
}

/* The modes a run tests: SM=1 needs the hardware. */
[[nodiscard]] inline std::vector<std::uint8_t> all_modes(bool hardware) {
  if (hardware)
    return {0, arm64::svcr_sm, arm64::svcr_za, arm64::svcr_mask};
  return {0, arm64::svcr_za};
}

[[nodiscard]] inline std::vector<std::uint8_t> za_modes(bool hardware) {
  if (hardware)
    return {arm64::svcr_za, arm64::svcr_mask};
  return {arm64::svcr_za};
}

/* The host's own profile with the SME2 profile at SVL 64. It never claims
 * sme_hardware, so nothing reads the host's SVCR. */
[[nodiscard]] inline uml::cpu::HostCapabilities injected_sme_host() {
  auto host = uml::cpu::host_capabilities();
  for (const auto feature :
       {uml::cpu::ArmFeature::sme, uml::cpu::ArmFeature::sme2,
        uml::cpu::ArmFeature::sme_f64f64, uml::cpu::ArmFeature::sme_i16i64})
    host.supported.set(static_cast<std::size_t>(feature));
  host.sme_svl_bytes = 64;
  host.sme_hardware = false;
  return host;
}

} // namespace uml::emulator::tests::jit_sme
