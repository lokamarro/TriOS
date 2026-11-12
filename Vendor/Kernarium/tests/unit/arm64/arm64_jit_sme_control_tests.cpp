// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The JIT's SME control core at run time, from SmeSupport::control on.
 * On a host whose profile implements SME, an SME host
 * unless DARWIN_UML_GUEST_SME=0, every test runs in every mode. Elsewhere the SM=0
 * tests run on an injected SME2 profile at SVL 64: SM=0 translations hold
 * no SVE or SME instruction (arm64-jit-sme-codegen), so they run on any
 * host, and only SM=1 code needs the hardware.
 */
#include "arm64_jit_sme_test_support.hpp"
#include "arm64_kernel_stops.hpp"
#include "arm64_sm0_vector_length_support.hpp"
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "cpu/arm64/fp_environment.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_sme_support.hpp"
#include "cpu/arm64/sme_profile.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <span>
#include <vector>

using namespace uml::emulator::tests;
using namespace uml::emulator::tests::jit_sme;

namespace {

namespace arm64 = uml::cpu::arm64;
using arm64::JitExecutor;
using uml::memory::StaticGuestMemory;
using Event = ::uml::cpu::EngineEvent;

/* Each SVCR write ends its block; leaving streaming mode zeroes V and
 * resets FPSR. */
void streaming_mode_switches() {
  {
    Machine machine{9401, 0xe0'0000, 0};
    machine.code(0, {smstart, mrs_svcr(0), smstop_sm, mrs_svcr(1),
                     smstart_sm, smstop_za, mrs_svcr(2), smstop, mrs_svcr(3),
                     system_call});
    std::size_t retired = 0;
    require(machine.run(retired) == Event::system_call);
    require(retired == 10U);
    require(machine.cpu.registers[0] == 3U && machine.cpu.registers[1] == 2U &&
            machine.cpu.registers[2] == 1U && machine.cpu.registers[3] == 0U);
    require(machine.cpu.svcr == 0U);
    require(all_zero(std::as_bytes(std::span{machine.cpu.vectors})));
    require(machine.guest_fpsr() == arm64::streaming_mode_change_fpsr);
    machine.require_preserved({0, 1, 2, 3});
  }
  /* MSR SVCR writes both bits from Xt. */
  Machine machine{9402, 0xe1'0000, 0};
  machine.code(0, {msr_svcr(4), mrs_svcr(5), msr_svcr(31), mrs_svcr(6),
                   system_call});
  machine.cpu.registers[4] = 0xffff'fffe'0000'0003ULL;
  std::size_t retired = 0;
  require(machine.run(retired) == Event::system_call);
  require(machine.cpu.registers[5] == 3U && machine.cpu.registers[6] == 0U);
  machine.require_preserved({5, 6});
}

/* SMSTART ZA zeroes ZA and ZT0; ZA writes leave V and FPSR alone. */
void za_mode_switches() {
  Machine machine{9403, 0xe2'0000, 0};
  machine.code(0, {smstart_za, mrs_svcr(0), smstop_za, mrs_svcr(1),
                   msr_svcr(4), mrs_svcr(5), msr_svcr(31), mrs_svcr(6),
                   smstart_za, system_call});
  machine.cpu.registers[4] = 0xffff'fffe'0000'0002ULL;
  const auto before = machine.initial_file();
  std::size_t retired = 0;
  require(machine.run(retired) == Event::system_call);
  require(retired == 10U);
  require(machine.cpu.registers[0] == 2U && machine.cpu.registers[1] == 0U &&
          machine.cpu.registers[5] == 2U && machine.cpu.registers[6] == 0U);
  require(machine.cpu.svcr == arm64::svcr_za);
  const auto after = machine.file();
  const auto za = arm64::sme_za_offset(machine.svl(), 0);
  require(std::equal(after.begin(), after.begin() + static_cast<std::ptrdiff_t>(za),
                     before.begin()));
  require(all_zero(std::span{after}.subspan(za)));
  require(machine.vectors_unchanged());
  require(machine.guest_fpsr() == initial_fpsr);
  machine.require_preserved({0, 1, 5, 6});
}

void enabling_without_storage_asks_for_it() {
  Machine machine{9404, 0xe3'0000, 0, false};
  machine.code(0, {smstop, smstart_za, system_call});
  std::size_t retired = 0;
  require(machine.run(retired) == Event::sme_access);
  require(retired == 1U);
  require(machine.cpu.pc == 0xe3'0004U);
  require(machine.cpu.svcr == 0U);
  machine.require_preserved();
}

void tpidr2_is_the_guest_register(bool hardware) {
  unsigned long generation = 9410;
  for (const auto mode : all_modes(hardware)) {
    Machine machine{generation, 0xe4'0000U + (generation - 9410U) * 0x1'0000U,
                    mode};
    ++generation;
    machine.code(0, {msr_tpidr2(1), mrs_tpidr2(2), system_call});
    machine.cpu.registers[1] = 0x1234'5678'9abc'def0ULL;
    std::size_t retired = 0;
    require(machine.run(retired) == Event::system_call);
    require(machine.cpu.tpidr2 == 0x1234'5678'9abc'def0ULL);
    require(machine.cpu.registers[2] == machine.cpu.tpidr2);
    machine.require_preserved({2});
  }
}

void vector_lengths_come_from_the_svl(bool hardware) {
  unsigned long generation = 9420;
  for (const auto mode : all_modes(hardware)) {
    Machine machine{generation, 0xe8'0000U + (generation - 9420U) * 0x1'0000U,
                    mode};
    ++generation;
    machine.code(0, {rdsvl(0, 1), rdsvl(1, -2), addsvl(2, 3, 3),
                     addspl(4, 3, -5), addsvl(31, 31, 1), addspl(5, 5, 31),
                     rdsvl(31, 4), system_call});
    machine.cpu.registers[3] = 0x1000;
    machine.cpu.registers[5] = 0x40;
    machine.cpu.sp = 0x8000;
    std::size_t retired = 0;
    require(machine.run(retired) == Event::system_call);
    const std::uint64_t svl = machine.svl();
    require(machine.cpu.registers[0] == svl);
    require(machine.cpu.registers[1] == ~(2U * svl) + 1U);
    require(machine.cpu.registers[2] == 0x1000U + 3U * svl);
    require(machine.cpu.registers[4] == 0x1000U - 5U * (svl / 8U));
    require(machine.cpu.registers[5] == 0x40U + 31U * (svl / 8U));
    require(machine.cpu.sp == 0x8000U + svl);
    machine.require_preserved({0, 1, 2, 4, 5, stack_pointer_index});
  }
}

/* A virtual CPU with SME and without SVE counts and scales a vector length
 * of 16 bytes outside streaming mode (detail/sm0_vector_length.hpp): every
 * word of the family runs, with and without ZA, as the model of the
 * pseudocode says, and touches no vector or SVE state. */
void vector_length_family_runs_outside_streaming_mode() {
  namespace family = uml::emulator::tests::sm0_vector_length;
  const auto host = ::uml::platform::without_sve(uml::cpu::host_capabilities());
  const uml::cpu::HostCapabilitiesScope scope{host};
  const auto words = family::words();
  constexpr std::size_t chunk = 200;
  unsigned long generation = 9460;
  for (const std::uint8_t mode : {std::uint8_t{0}, arm64::svcr_za})
    for (std::size_t start = 0; start < words.size(); start += chunk) {
      const auto count = std::min(chunk, words.size() - start);
      Machine machine{generation,
                      0x300'0000U + (generation - 9460U) * 0x10'0000U, mode};
      ++generation;
      std::vector<std::uint32_t> code(words.begin() + start,
                                      words.begin() + start + count);
      code.push_back(system_call);
      machine.code(0, code);
      family::Registers expected;
      expected.x = machine.cpu.registers;
      expected.sp = machine.cpu.sp;
      for (std::size_t index = 0; index < count; ++index)
        require(family::model(words[start + index], expected));
      const auto before = machine.initial_file();
      std::size_t retired = 0;
      require(machine.run(retired) == Event::system_call);
      require(retired == code.size());
      require(machine.cpu.registers == expected.x);
      require(machine.cpu.sp == expected.sp);
      require(machine.cpu.nzcv == initial_nzcv);
      require(machine.cpu.svcr == mode);
      require(machine.vectors_unchanged());
      require(machine.file() == before);
    }
}

/* The spill words (LDR and STR of Z and P) run outside streaming mode on a
 * virtual CPU with SME and without SVE: Z is the V register, a predicate
 * store writes zeros and a predicate load only reads, and nothing else
 * changes. Placement 0 is inside a page (the fast path); 1 crosses a page
 * and 2 goes through a tagged pointer, which take the semantic library. */
void spill_words_run_outside_streaming_mode() {
  namespace family = uml::emulator::tests::sm0_vector_length;
  const auto host = ::uml::platform::without_sve(uml::cpu::host_capabilities());
  const uml::cpu::HostCapabilitiesScope scope{host};
  constexpr std::size_t window = 3U * page;
  unsigned long generation = 20000;
  std::size_t checked = 0;
  for (const auto &spill : family::spills())
    for (int placement = 0; placement < 3; ++placement) {
      const bool selected = placement == 0 || (spill.base != 31U &&
                                               spill.transfer == 5U &&
                                               (spill.immediate == -19 ||
                                                spill.immediate == 0 ||
                                                spill.immediate == 15));
      if (!selected)
        continue;
      Machine machine{generation,
                      0x1000'0000U + (generation - 20000U) * 0x2'0000U, 0};
      ++generation;
      const auto memory = bytes_of(window, 9);
      machine.put(machine.data(0), memory);
      std::uint64_t address;
      if (spill.base == 31U) {
        const auto anchor =
            spill.offset() < 0 ? machine.data(window - 64U) : machine.data(64U);
        machine.cpu.sp = anchor;
        address = anchor + spill.offset();
      } else {
        address = placement == 1 ? machine.data(page - spill.bytes() / 2U)
                  : spill.offset() < 0 ? machine.data(window - 58U)
                                       : machine.data(70U);
        machine.cpu.registers[spill.base] = address - spill.offset();
        if (placement == 2) {
          machine.cpu.tbi_enabled = true;
          machine.cpu.registers[spill.base] |= 0x5a00'0000'0000'0000ULL;
        }
      }
      machine.code(0, {spill.word(), system_call});
      /* V0-V31 as the FP image holds them. */
      machine.cpu.access_fp_state();
      family::SpillImage expected;
      for (unsigned reg = 0; reg < 32U; ++reg)
        for (unsigned byte = 0; byte < 16U; ++byte)
          expected.v[reg][byte] = static_cast<std::uint8_t>(
              machine.cpu.vectors[reg].bytes[byte]);
      for (const auto byte : memory)
        expected.memory.push_back(static_cast<std::uint8_t>(byte));
      family::apply_spill(spill, address - machine.data(0), expected);
      std::size_t retired = 0;
      const auto event = machine.run(retired);
      if (event != Event::system_call) {
        std::fprintf(stderr, "%08x placement %d: event %d\n", spill.word(),
                     placement, static_cast<int>(event));
        require(false);
      }
      require(retired == 2U);
      require(machine.cpu.svcr == 0U);
      machine.require_preserved();
      arm64::restore_native_fp_environment(machine.cpu);
      machine.cpu.access_fp_state();
      for (unsigned reg = 0; reg < 32U; ++reg)
        for (unsigned byte = 0; byte < 16U; ++byte)
          if (static_cast<std::uint8_t>(machine.cpu.vectors[reg].bytes[byte]) !=
              expected.v[reg][byte]) {
            std::fprintf(stderr, "%08x placement %d: V%u[%u] differs\n",
                         spill.word(), placement, reg, byte);
            require(false);
          }
      const auto actual = machine.get(machine.data(0), window);
      for (std::size_t index = 0; index < window; ++index)
        if (static_cast<std::uint8_t>(actual[index]) != expected.memory[index]) {
          std::fprintf(stderr, "%08x placement %d: memory +%zu differs\n",
                       spill.word(), placement, index);
          require(false);
        }
      ++checked;
    }
  require(checked > 500U);
}

/* ... and fault like the access each is: an unmapped page, with nothing of a
 * store reaching the mapped one, and a misaligned SP, each leaving the PC at
 * the word. */
void spill_words_fault_like_their_accesses() {
  namespace family = uml::emulator::tests::sm0_vector_length;
  const auto host = ::uml::platform::without_sve(uml::cpu::host_capabilities());
  const uml::cpu::HostCapabilitiesScope scope{host};
  unsigned long generation = 19000;
  for (const bool predicate : {false, true})
    for (const bool store : {false, true}) {
      const family::Spill spill{store, predicate, 3, 0, 0};
      {
        const std::uint64_t base =
            0xf00'0000U + (generation - 19000U) * 0x2'0000U;
        Machine machine{generation, base, 0, true, 4, 2};
        ++generation;
        machine.code(0, {spill.word(), system_call});
        machine.cpu.registers[0] = machine.data(page - spill.bytes() + 1U);
        const auto before = machine.get(machine.data(page - 16U), 16U);
        std::size_t retired = 0;
        require(machine.run(retired) == Event::memory_fault);
        require(retired == 0U);
        require(machine.cpu.pc == base);
        require(machine.executor().fault_address() == machine.data(page));
        require(machine.executor().fault_access() ==
                (store ? uml::memory::Access::write : uml::memory::Access::read));
        require(!machine.executor().fault_alignment());
        require(machine.get(machine.data(page - 16U), 16U) == before);
        machine.require_preserved();
      }
      {
        const std::uint64_t base =
            0xf00'0000U + (generation - 19000U) * 0x2'0000U;
        Machine machine{generation, base, 0};
        ++generation;
        machine.code(0, {family::Spill{store, predicate, 3, 31, 1}.word(),
                         system_call});
        machine.cpu.sp = machine.data(8);
        std::size_t retired = 0;
        require(machine.run(retired) == Event::memory_fault);
        require(retired == 0U);
        require(machine.executor().fault_alignment());
        require(machine.executor().fault_address() == machine.data(8));
        machine.require_preserved();
      }
    }
}

/* LDR ZA, then STR ZA of the same vector through another index register,
 * at each placement the fast and slow paths divide. */
void za_vectors_round_trip(bool hardware) {
  unsigned long generation = 9430;
  for (const auto mode : za_modes(hardware))
    for (int placement = 0; placement < 3; ++placement) {
      Machine machine{generation,
                      0xf0'0000U + (generation - 9430U) * 0x10'0000U, mode};
      ++generation;
      const std::size_t svl = machine.svl();
      /* Aligned, crossing into the next page, and through a tagged
       * pointer. */
      const auto source =
          placement == 1 ? machine.data(page - svl / 2U) : machine.data(64);
      const auto destination = machine.data(2U * page + 3U);
      const auto bytes = bytes_of(svl, 5);
      machine.put(source, bytes);
      machine.code(0, {za_vector(true, 0, 2, 3), za_vector(false, 1, 3, 1),
                       system_call});
      machine.cpu.registers[12] = 5;
      machine.cpu.registers[13] = 7;
      machine.cpu.registers[2] = source - 3U * svl;
      machine.cpu.registers[3] = destination - svl;
      if (placement == 2) {
        machine.cpu.tbi_enabled = true;
        machine.cpu.registers[2] |= 0x5a00'0000'0000'0000ULL;
      }
      const auto before = machine.initial_file();
      std::size_t retired = 0;
      require(machine.run(retired) == Event::system_call);
      require(retired == 3U);
      require(machine.get(destination, svl) == bytes);
      const auto after = machine.file();
      const auto row = arm64::sme_za_offset(svl, 8);
      require(std::equal(bytes.begin(), bytes.end(),
                         after.begin() + static_cast<std::ptrdiff_t>(row)));
      /* Nothing else in the file changed: not Z or P across the enter
       * and leave sequences, nor another ZA vector. */
      for (std::size_t index = 0; index < after.size(); ++index)
        if (index < row || index >= row + svl)
          require(after[index] == before[index]);
      machine.require_preserved();
    }
}

void za_faults_match_the_m4(bool hardware) {
  for (const auto mode : za_modes(hardware)) {
    const std::uint64_t base = 0x1f0'0000U + mode * 0x10'0000U;
    /* Only the code page and the first data page are mapped. */
    {
      Machine machine{9440U + mode, base, mode, true, 4, 2};
      const std::size_t svl = machine.svl();
      machine.code(0, {za_vector(false, 0, 2, 0), system_call});
      machine.cpu.registers[2] = machine.data(page - svl / 2U);
      const auto before = machine.get(machine.data(page - svl / 2U), svl / 2U);
      std::size_t retired = 0;
      require(machine.run(retired) == Event::memory_fault);
      require(retired == 0U);
      require(machine.cpu.pc == base);
      require(machine.executor().fault_address() == machine.data(page));
      require(machine.executor().fault_access() == uml::memory::Access::write);
      require(!machine.executor().fault_alignment());
      /* Nothing of the vector reached the mapped page. */
      require(machine.get(machine.data(page - svl / 2U), svl / 2U) == before);
      machine.require_preserved();
    }
    {
      Machine machine{9450U + mode, base + 0x8'0000U, mode};
      machine.code(0, {za_vector(true, 0, 31, 0), system_call});
      machine.cpu.sp = machine.data(8);
      std::size_t retired = 0;
      require(machine.run(retired) == Event::memory_fault);
      require(machine.executor().fault_alignment());
      require(machine.executor().fault_address() == machine.data(8));
      require(machine.executor().fault_access() == uml::memory::Access::read);
      machine.require_preserved();
    }
  }
}

void zero_za_clears_the_named_tiles(bool hardware) {
  for (const auto mode : za_modes(hardware)) {
    Machine machine{9460U + mode, 0x300'0000U + mode * 0x10'0000U, mode};
    machine.code(0, {zero_za(0x09), system_call});
    const auto before = machine.initial_file();
    std::size_t retired = 0;
    require(machine.run(retired) == Event::system_call);
    const std::size_t svl = machine.svl();
    const auto after = machine.file();
    for (std::size_t vector = 0; vector < svl; ++vector) {
      const auto offset = arm64::sme_za_offset(svl, vector);
      const bool cleared = vector % 8U == 0U || vector % 8U == 3U;
      for (std::size_t index = 0; index < svl; ++index)
        require(after[offset + index] ==
                (cleared ? std::byte{} : before[offset + index]));
    }
    machine.require_preserved();
  }
}

void zt0_moves_and_clears(bool hardware) {
  if (arm64::sme_profile(uml::cpu::host_capabilities()) !=
      arm64::SmeProfile::sme2)
    return;
  for (const auto mode : za_modes(hardware)) {
    Machine machine{9470U + mode, 0x400'0000U + mode * 0x10'0000U, mode};
    const auto bytes = bytes_of(arm64::sme_zt0_bytes, 9);
    machine.put(machine.data(128), bytes);
    machine.code(0, {zt0_vector(true, 2), zt0_vector(false, 3), zero_zt0,
                     system_call});
    machine.cpu.registers[2] = machine.data(128);
    machine.cpu.registers[3] = machine.data(page + 48);
    std::size_t retired = 0;
    require(machine.run(retired) == Event::system_call);
    require(machine.get(machine.data(page + 48), bytes.size()) == bytes);
    const auto after = machine.file();
    require(all_zero(
        std::span{after}.subspan(arm64::sme_zt0_offset(machine.svl()))));
    machine.require_preserved();
  }
}

/*
 * A TLB miss in an SM=1 block takes the streaming bridge, a linked SM=1
 * successor branches past the enter, and the guest's state and FPSR come
 * through both, on the first run and on the linked second one.
 */
void streaming_state_survives_misses_and_links() {
  Machine machine{9480, 0x500'0000, arm64::svcr_mask};
  const std::size_t svl = machine.svl();
  machine.code(0, {load_x(5, 6), za_vector(false, 0, 3, 0), branch(1),
                   za_vector(true, 1, 2, 0), za_vector(false, 1, 4, 0),
                   system_call});
  machine.cpu.registers[6] = machine.data(2U * page);
  machine.cpu.registers[3] = machine.data(0);
  machine.cpu.registers[2] = machine.data(0);
  machine.cpu.registers[4] = machine.data(page);
  machine.cpu.registers[12] = 4;
  machine.cpu.registers[13] = 9;
  const auto before = machine.initial_file();
  for (int pass = 0; pass < 2; ++pass) {
    machine.cpu.pc = 0x500'0000U;
    std::size_t retired = 0;
    require(machine.run(retired) == Event::system_call);
    require(retired == 6U);
    /* ZA vector 4 went out, came back as vector 9, and went out again. */
    const auto row =
        std::span{before}.subspan(arm64::sme_za_offset(svl, 4), svl);
    const auto out = machine.get(machine.data(page), svl);
    require(std::equal(row.begin(), row.end(), out.begin()));
    const auto after = machine.file();
    const auto nine = arm64::sme_za_offset(svl, 9);
    for (std::size_t index = 0; index < after.size(); ++index)
      require(after[index] ==
              (index >= nine && index < nine + svl ? row[index - nine]
                                                   : before[index]));
    require(machine.guest_fpsr() == initial_fpsr);
    machine.require_preserved({5});
  }
}

/* A host helper in an SM=1 block (the FEAT_MOPS copy) is terminal: the
 * leave before it spills the guest's state and the next block refills it. */
void streaming_state_survives_helpers() {
  Machine machine{9490, 0x600'0000, arm64::svcr_mask};
  const std::size_t svl = machine.svl();
  const auto copied = bytes_of(64, 3);
  const auto row = bytes_of(svl, 4);
  machine.put(machine.data(0), copied);
  machine.put(machine.data(2U * page), row);
  machine.code(0, {za_vector(true, 0, 3, 0), copy_forward[0], copy_forward[1],
                   copy_forward[2], za_vector(false, 0, 4, 0), system_call});
  machine.cpu.registers[0] = machine.data(page);
  machine.cpu.registers[1] = machine.data(0);
  machine.cpu.registers[2] = 64;
  machine.cpu.registers[3] = machine.data(2U * page);
  machine.cpu.registers[4] = machine.data(2U * page + 512U);
  machine.cpu.registers[12] = 5;
  const auto before = machine.initial_file();
  std::size_t retired = 0;
  require(machine.run(retired) == Event::system_call);
  require(retired == 6U);
  require(machine.cpu.registers[2] == 0U);
  require(machine.get(machine.data(page), copied.size()) == copied);
  require(machine.get(machine.data(2U * page + 512U), svl) == row);
  const auto after = machine.file();
  const auto five = arm64::sme_za_offset(svl, 5);
  for (std::size_t index = 0; index < after.size(); ++index)
    require(after[index] ==
            (index >= five && index < five + svl ? row[index - five]
                                                 : before[index]));
  require(machine.guest_fpsr() == initial_fpsr);
  /* CPYFP reports the copy's algorithm option in NZCV. */
  machine.require_preserved({0, 1, 2}, true);
}

/*
 * XNU resets FPSR to 0x0800009F on return from any kernel entry while a
 * thread is in streaming mode. An SM=1 translation therefore keeps the
 * guest's FPCR and FPSR in CpuState: a status written there survives a
 * long SM=1 loop that another thread repeatedly stops in the kernel, and
 * reads back unchanged, in streaming mode and after the run.
 */
void fp_controls_survive_kernel_entries_in_streaming_mode() {
  constexpr std::uint32_t written_fpsr = 0x0000'0011U; // IXC, IOC
  constexpr std::uint32_t written_fpcr = 0x0080'0000U; // RMode -inf
  Machine machine{9495, 0x700'0000, arm64::svcr_sm};
  machine.code(0, {0xd51b'4421U,   // msr fpsr, x1
                   0xd51b'4404U,   // msr fpcr, x4
                   0xf100'0442U,   // subs x2, x2, #1
                   0x54ff'ffe1U,   // b.ne .-4
                   0xd53b'4423U,   // mrs x3, fpsr
                   0xd53b'4405U,   // mrs x5, fpcr
                   system_call});
  machine.cpu.registers[1] = written_fpsr;
  machine.cpu.registers[2] = 40'000'000;
  machine.cpu.registers[4] = written_fpcr;
  KernelStops interrupter;
  std::size_t retired = 0;
  const auto event = machine.run(retired, 1'000'000);
  const auto stops = interrupter.stop();
  require(event == Event::system_call);
  std::fprintf(stderr, "streaming loop stopped in the kernel %u times\n",
               stops);
  require(stops >= 10U);
  require(machine.cpu.svcr == arm64::svcr_sm);
  require(machine.cpu.registers[3] == written_fpsr);
  require(machine.cpu.registers[5] == written_fpcr);
  require(machine.guest_fpsr() == written_fpsr);
  arm64::restore_native_fp_environment(machine.cpu);
  require(machine.cpu.effective_fpcr() == written_fpcr);
}

void run_tests(bool hardware) {
  if (hardware)
    streaming_mode_switches();
  za_mode_switches();
  enabling_without_storage_asks_for_it();
  tpidr2_is_the_guest_register(hardware);
  vector_lengths_come_from_the_svl(hardware);
  vector_length_family_runs_outside_streaming_mode();
  spill_words_run_outside_streaming_mode();
  spill_words_fault_like_their_accesses();
  za_vectors_round_trip(hardware);
  za_faults_match_the_m4(hardware);
  zero_za_clears_the_named_tiles(hardware);
  zt0_moves_and_clears(hardware);
  if (hardware) {
    streaming_state_survives_misses_and_links();
    streaming_state_survives_helpers();
    fp_controls_survive_kernel_entries_in_streaming_mode();
  }
}

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  if (arm64::jit::sme_support == arm64::SmeSupport::none ||
      !jit_available()) {
    std::printf("skipped: the JIT does not run here\n");
    return 0;
  }
  if (arm64::implements_sme(uml::cpu::host_capabilities())) {
    run_tests(true);
    return 0;
  }
  const auto host = injected_sme_host();
  const uml::cpu::HostCapabilitiesScope scope{host};
  run_tests(false);
  std::printf("SM=1 skipped: the host profile does not implement SME\n");
  return 0;
}
