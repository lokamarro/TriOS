// SPDX-License-Identifier: GPL-2.0-or-later
#include "arm64_host_counter_test_support.hpp"
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

using namespace uml::emulator::tests;

namespace {

void direct_counter_block_executes()
{
	constexpr unsigned long generation = 716;
	constexpr std::uint64_t base = 0x8b'0000;
	JitBacking backing{1};
	backing.put32(0, 0xd53b'e005U); // MRS X5, CNTFRQ_EL0
	backing.put32(4, 0xd53b'e046U); // MRS X6, CNTVCT_EL0
	backing.put32(8, 0xd400'0001U); // SVC #0
	JitMapping mapping{generation, base, backing, 1};
	uml::cpu::arm64::JitExecutor executor;
	uml::memory::StaticGuestMemory memory;
	uml::cpu::arm64::CpuState cpu{};
	cpu.pc = base;

	const auto before =
		read_host_virtual_counter();
	require(executor.begin_execution(generation, cpu.pc));
	std::size_t executed = 0;
	const auto event =
		executor.run_block(cpu, memory, generation, executed);
	executor.end_execution();
	const auto after =
		read_host_virtual_counter();

	require(event == ::uml::cpu::EngineEvent::system_call);
	require(executed == 3);
	require(cpu.pc == base + 12);
	require(cpu.registers[5] ==
		read_host_counter_frequency());
	require(cpu.registers[6] >= before);
	require(cpu.registers[6] <= after);
}

} // namespace

int main()
{
	const uml::testing::RuntimeTestRoute runtime_route;
	require(jit_available() == 1);
	direct_counter_block_executes();
	return 0;
}
