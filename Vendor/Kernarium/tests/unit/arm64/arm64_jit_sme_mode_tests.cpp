// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The guest SVCR mode joins every JIT translation key: the block cache, the
 * links and the indirect sites. These checks need no SME hardware: they
 * compile on an injected SME profile, and the runtime ones run only GPR code
 * in the SM=0/ZA=1 mode, which never puts the host in streaming mode.
 */
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_runtime_contract.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "cpu/arm64/sme_profile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace uml::emulator::tests;

namespace {

namespace arm64 = uml::cpu::arm64;
namespace jit_test = uml::cpu::arm64::jit::testing;
using arm64::JitExecutor;
using uml::memory::StaticGuestMemory;
using Event = ::uml::cpu::EngineEvent;

constexpr std::uint32_t system_call = 0xd400'0001U;
constexpr std::uint32_t add_x5_one = 0x9100'04a5U;
constexpr std::uint32_t add_x6_one = 0x9100'04c6U;

[[nodiscard]] constexpr std::uint32_t branch(std::int64_t words) noexcept {
  return 0x1400'0000U | (static_cast<std::uint32_t>(words) & 0x03ff'ffffU);
}

[[nodiscard]] constexpr std::uint32_t
branch_register(unsigned source) noexcept {
  return 0xd61f'0000U | ((source & 31U) << 5U);
}

/* The host's own profile with the SME profile tokens at SVL 64. It never
 * claims sme_hardware, so nothing reads the host's SVCR. */
[[nodiscard]] uml::cpu::HostCapabilities sme_host() {
  auto host = uml::cpu::host_capabilities();
  for (const auto feature :
       {uml::cpu::ArmFeature::sme, uml::cpu::ArmFeature::sme_f64f64,
        uml::cpu::ArmFeature::sme_i16i64})
    host.supported.set(static_cast<std::size_t>(feature));
  host.sme_svl_bytes = 64;
  return host;
}

void link_keys_carry_the_mode() {
  const auto host = sme_host();
  const uml::cpu::HostCapabilitiesScope scope{host};
  constexpr std::uint64_t pc = 0x4f00'0000U;
  const std::array direct{add_x5_one, branch(0x40)};
  const std::array indirect{add_x5_one, branch_register(7)};
  for (std::uint8_t mode = 0; mode <= arm64::svcr_mask; ++mode) {
    const auto linked = jit_test::compile_block_in_mode(
        pc, direct, mode, jit_test::LinkShape::direct);
    const auto target = arm64::translation_key(pc + 4U + 0x100U, mode);
    require(linked.direct_keys == std::vector<std::uint64_t>{target});
    require(linked.relocation_keys == std::vector<std::uint64_t>{target});
    const auto site = jit_test::compile_block_in_mode(
        pc, indirect, mode, jit_test::LinkShape::indirect);
    const auto source = arm64::translation_key(pc + 4U, mode);
    require(site.indirect_keys == std::vector<std::uint64_t>{source});
    require(site.relocation_keys == std::vector<std::uint64_t>{source});
  }
}

void mode_zero_is_the_translation_without_sme() {
  const auto host = sme_host();
  const uml::cpu::HostCapabilitiesScope scope{host};
  constexpr std::uint64_t pc = 0x4f10'0000U;
  const std::array program{add_x5_one, add_x6_one, branch_register(7)};
  require(jit_test::compile_block_in_mode(pc, program, 0).code.words ==
          jit_test::compile_block(pc, program).words);
  require(jit_test::compile_block_in_mode(pc, program, 0,
                                          jit_test::LinkShape::tracked)
              .code.words ==
          jit_test::compile_tracked_block(pc, program).words);
}

struct SmeRun final {
  JitExecutor executor;
  StaticGuestMemory memory;
  arm64::SmeRegisterFile registers{64};
  arm64::CpuState cpu{};

  SmeRun() : executor{arm64::JitExecutorOptions{.host_capabilities = sme_host()}} {
    cpu.bind_sme_registers(registers);
  }

  void set_mode(std::uint8_t mode) {
    cpu.defer_sme_load(nullptr, mode, 0);
  }

  [[nodiscard]] std::size_t run(unsigned long generation, Event expected) {
    std::size_t executed = 0;
    require(executor.run_block(cpu, memory, generation, executed) ==
            expected);
    return executed;
  }
};

void each_mode_has_its_own_translation() {
  constexpr unsigned long generation = 9301;
  constexpr std::uint64_t base = 0xd0'0000;
  JitBacking backing{1};
  backing.put32(0, add_x5_one);
  backing.put32(4, system_call);
  JitMapping mapping{generation, base, backing, 1};
  SmeRun run;
  require(run.executor.begin_execution(generation, base));
  const auto translate = [&](std::uint8_t mode) {
    run.set_mode(mode);
    run.cpu.pc = base;
    require(run.run(generation, Event::system_call) == 2);
    return run.executor.cache_validation_diagnostics().translations;
  };
  require(translate(0) == 1);
  require(translate(arm64::svcr_za) == 2);
  require(translate(0) == 2);
  require(translate(arm64::svcr_za) == 2);
  require(run.cpu.registers[5] == 4);
  run.executor.end_execution();
}

void links_stay_within_their_mode() {
  constexpr unsigned long generation = 9302;
  constexpr std::uint64_t base = 0xd1'0000;
  JitBacking backing{1};
  backing.put32(0, add_x5_one);
  backing.put32(4, branch(2));
  backing.put32(12, add_x6_one);
  backing.put32(16, system_call);
  JitMapping mapping{generation, base, backing, 1};
  SmeRun run;
  require(run.executor.begin_execution(generation, base));

  /* Build and activate both blocks in mode ZA, then chain them. */
  run.set_mode(arm64::svcr_za);
  run.cpu.pc = base;
  require(run.run(generation, Event::continued) == 2);
  require(run.cpu.pc == base + 12U);
  require(run.run(generation, Event::system_call) == 2);
  run.cpu.pc = base;
  require(run.run(generation, Event::system_call) == 4);

  /* Mode 0 must not chain into the mode-ZA successor. */
  run.set_mode(0);
  run.cpu.pc = base;
  require(run.run(generation, Event::continued) == 2);
  require(run.cpu.pc == base + 12U);
  run.executor.end_execution();
}

void indirect_fallback_restores_the_plain_target() {
  constexpr unsigned long generation = 9303;
  constexpr std::uint64_t base = 0xd2'0000;
  JitBacking backing{1};
  backing.put32(0, add_x5_one);
  backing.put32(4, branch_register(7));
  JitMapping mapping{generation, base, backing, 1};
  SmeRun run;
  require(run.executor.begin_execution(generation, base));
  run.set_mode(arm64::svcr_za);
  run.cpu.pc = base;
  run.cpu.registers[7] = base;
  /* The first run misses and records the site; the second trains it on
   * itself and loops until the fairness budget takes the fallback, which
   * must leave the target PC, not the target's key. */
  require(run.run(generation, Event::continued) == 2);
  require(run.cpu.pc == base);
  const auto executed = run.run(generation, Event::continued);
  require(executed > 2U);
  require(run.cpu.pc == base);
  require(run.executor.indirect_link_diagnostics().trains == 1);
  run.executor.end_execution();
}

void indirect_sites_train_only_their_mode() {
  constexpr unsigned long generation = 9304;
  constexpr std::uint64_t base = 0xd3'0000;
  JitBacking backing{1};
  backing.put32(0, add_x5_one);
  backing.put32(4, branch_register(7));
  backing.put32(12, add_x6_one);
  backing.put32(16, system_call);
  JitMapping mapping{generation, base, backing, 1};
  SmeRun run;
  require(run.executor.begin_execution(generation, base));
  /* A mode-ZA site misses and records its target; the mode changes
   * before the target runs, so the mode-0 block must not train it. */
  run.set_mode(arm64::svcr_za);
  run.cpu.pc = base;
  run.cpu.registers[7] = base + 12U;
  require(run.run(generation, Event::continued) == 2);
  require(run.cpu.pc == base + 12U);
  run.set_mode(0);
  require(run.run(generation, Event::system_call) == 2);
  require(run.executor.indirect_link_diagnostics().trains == 0);
  /* In its own mode the same target trains the site. */
  run.set_mode(arm64::svcr_za);
  run.cpu.pc = base;
  require(run.run(generation, Event::continued) == 2);
  require(run.run(generation, Event::system_call) == 2);
  require(run.executor.indirect_link_diagnostics().trains == 1);
  run.executor.end_execution();
}

} // namespace

int main() {
  const uml::testing::RuntimeTestRoute runtime_route;
  link_keys_carry_the_mode();
  mode_zero_is_the_translation_without_sme();
  if (jit_available() != 1)
    return 0;
  each_mode_has_its_own_translation();
  links_stay_within_their_mode();
  indirect_fallback_restores_the_plain_target();
  indirect_sites_train_only_their_mode();
  return 0;
}
