// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/gadgets/program.hpp"
#include "platform/apple/host_features.hpp"
#include "require.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory_resource>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = arm64::gadgets;

namespace {

using uml::testing::require;

void set_single(arm64::CpuState &cpu, unsigned index, std::uint32_t bits)
{
	std::memcpy(cpu.vectors[index].bytes.data(), &bits, sizeof(bits));
}

std::uint32_t single(const arm64::CpuState &cpu, unsigned index)
{
	std::uint32_t bits = 0;
	std::memcpy(&bits, cpu.vectors[index].bytes.data(), sizeof(bits));
	return bits;
}

std::uint64_t native_fpcr()
{
	std::uint64_t value = 0;
	asm volatile("mrs %0, fpcr" : "=r"(value));
	return value;
}

template<std::size_t Size>
arm64::CpuState execute(const std::array<std::uint32_t, Size> &instructions,
	arm64::CpuState cpu)
{
	const auto built = gadgets::Builder{}.build(0x8000, instructions);
	require(built.complete);
	::uml::memory::Tlb tlb;
	require(gadgets::execute(built.program, cpu, tlb) ==
		::uml::cpu::EngineEvent::system_call);
	return cpu;
}

constexpr std::uint32_t fpsr_inexact = 1U << 4;
constexpr std::uint32_t fpsr_saturation = 1U << 27;

/* FDIV S2,S0,S1 computes 1/3 and raises IXC in the live environment, then
 * `instruction` runs a software model, then MRS X5,FPSR. The model raises
 * nothing more, so the read and the committed FPSR both keep QC and IXC. */
arm64::CpuState run_after_inexact(const ::uml::cpu::HostCapabilities &host,
	std::uint32_t instruction, arm64::CpuState cpu)
{
	const std::array instructions{
		0x1e21'1802U, // fdiv s2,s0,s1
		instruction,
		0xd53b'4425U, // mrs x5,fpsr
		0xd400'0001U, // svc #0
	};
	const auto built = gadgets::Builder{}.build(0x8000, instructions, {},
		std::pmr::get_default_resource(), {},
		gadgets::BuildOptions{.host_capabilities = host});
	require(built.complete);
	set_single(cpu, 0, 0x3f80'0000U);
	set_single(cpu, 1, 0x4040'0000U);
	cpu.fpsr = fpsr_saturation;
	::uml::memory::Tlb tlb;
	require(gadgets::execute(built.program, cpu, tlb) ==
		::uml::cpu::EngineEvent::system_call);
	require(single(cpu, 2) == 0x3eaa'aaabU);
	require(cpu.registers[5] == (fpsr_saturation | fpsr_inexact));
	require(cpu.fpsr == (fpsr_saturation | fpsr_inexact));
	return cpu;
}

} // namespace

int main()
{
	/* Activate the native environment, change FPCR while it is active, then
	 * prove both the register source and XZR write affect the very next native
	 * operation. 1.0 + half an ULP distinguishes +Infinity from nearest. */
	{
		const std::array instructions{
			0x1e22'2824U, // fadd s4,s1,s2 (activates default environment)
			0xd51b'4403U, // msr fpcr,x3
			0x1e22'2820U, // fadd s0,s1,s2
			0xd51b'441fU, // msr fpcr,xzr
			0x1e22'2825U, // fadd s5,s1,s2
			0xd400'0001U, // svc #0
		};
		arm64::CpuState initial{};
		initial.registers[3] = 1U << 22; // Round toward plus infinity.
		set_single(initial, 1, 0x3f80'0000U);
		set_single(initial, 2, 0x3380'0000U);
		const auto cpu = execute(instructions, initial);
		require(single(cpu, 4) == 0x3f80'0000U);
		require(single(cpu, 0) == 0x3f80'0001U);
		require(single(cpu, 5) == 0x3f80'0000U);
		require(cpu.fpcr == 0);
	}

	/* An active FPSR write must update native status immediately; otherwise
	 * leave would resurrect the divide-by-zero flag cleared by MSR FPSR. */
	{
		const std::array instructions{
			0x1e22'1820U, // fdiv s0,s1,s2
			0xd51b'443fU, // msr fpsr,xzr
			0x1e22'2823U, // fadd s3,s1,s2
			0xd53b'4424U, // mrs x4,fpsr
			0xd400'0001U, // svc #0
		};
		arm64::CpuState initial{};
		set_single(initial, 1, 0x3f80'0000U);
		set_single(initial, 2, 0);
		const auto cpu = execute(instructions, initial);
		require(cpu.registers[4] == 0);
		require(cpu.fpsr == 0);
	}

	/* The production executor may retain the guest FP environment between
	 * internal block exits, but an explicit dispatch-boundary restore must
	 * commit FPSR and restore the caller environment exactly once. */
	{
		const std::array instructions{
			0x1e22'2820U, // fadd s0,s1,s2
			0xd400'0001U, // svc #0
		};
		const auto built = gadgets::Builder{}.build(0xa000, instructions);
		require(built.complete);
		arm64::CpuState cpu{};
		cpu.fpcr = 1U << 22; // Round toward plus infinity.
		set_single(cpu, 1, 0x3f80'0000U);
		set_single(cpu, 2, 0x3380'0000U);
		::uml::memory::Tlb tlb;
		std::size_t executed = 0;
		const auto host_fpcr = native_fpcr();
		require(gadgets::execute_preserving_native_fp_environment(
			built.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::system_call);
		require(cpu.gadget_fp_active);
		require(single(cpu, 0) == 0x3f80'0001U);
		require(gadgets::execute_preserving_native_fp_environment(
			built.program, cpu, tlb, executed) ==
			::uml::cpu::EngineEvent::system_call);
		require(cpu.gadget_fp_active);
		require(single(cpu, 0) == 0x3f80'0001U);
		gadgets::restore_native_fp_environment(cpu);
		require(!cpu.gadget_fp_active);
		require(native_fpcr() == host_fpcr);
	}

	/* A software model reports its exceptions through CpuState.fpsr while
	 * the live FPSR is the host register; status earlier gadgets of the
	 * block raised must survive the call. The ABI baseline host has no
	 * FEAT_FP16, FEAT_JSCVT or FEAT_FHM, so it selects the fallback
	 * gadgets; FCVTL under FPCR.AHP takes the inline software path on any
	 * host without FEAT_AFP, and FJCVTZS under FPCR.FZ on one with
	 * FEAT_JSCVT. Results match native M1 execution. */
	{
		const auto &baseline = ::uml::cpu::baseline_host_capabilities();
		const std::uint64_t two = 0x4000'0000'0000'0000ULL;

		arm64::CpuState cpu{};
		std::memcpy(cpu.vectors[3].bytes.data(), &two, sizeof(two));
		cpu = run_after_inexact(baseline, 0x1e7e'0064U, cpu); // fjcvtzs w4,d3
		require(cpu.registers[4] == 2U && cpu.nzcv == 0x4000'0000U);

		cpu = {};
		cpu.vectors[3].bytes[1] = std::byte{0x40}; // 2.0 in h3
		cpu = run_after_inexact(baseline, 0x1ef8'0064U, cpu); // fcvtzs w4,h3
		require(cpu.registers[4] == 2U);

		cpu = {};
		cpu.registers[3] = 2U;
		cpu = run_after_inexact(baseline, 0x1ee2'0064U, cpu); // scvtf h4,w3
		require(single(cpu, 4) == 0x4000U);

		cpu = {};
		const std::array<std::uint32_t, 2> addends{
			0x3f80'0000U, 0x3f80'0000U};
		const std::array<std::uint16_t, 4> left{0x3c00U, 0x4000U};
		const std::array<std::uint16_t, 4> right{0x4000U, 0x3800U};
		std::memcpy(cpu.vectors[4].bytes.data(), addends.data(), 8);
		std::memcpy(cpu.vectors[5].bytes.data(), left.data(), 8);
		std::memcpy(cpu.vectors[6].bytes.data(), right.data(), 8);
		cpu = run_after_inexact(baseline, 0x0e26'eca4U, cpu); // fmlal v4.2s
		std::array<std::uint32_t, 4> fused{};
		std::memcpy(fused.data(), cpu.vectors[4].bytes.data(), 16);
		require(fused == std::array<std::uint32_t, 4>{
			0x4040'0000U, 0x4000'0000U, 0U, 0U});

		cpu = {};
		cpu.fpcr = 1U << 26; // AHP: 0x7c00 is 65536.
		const std::array<std::uint16_t, 4> halves{
			0x3c00U, 0x4000U, 0xb800U, 0x7c00U};
		std::memcpy(cpu.vectors[7].bytes.data(), halves.data(), 8);
		cpu = run_after_inexact(baseline, 0x0e21'78e6U, cpu); // fcvtl v6.4s
		std::array<std::uint32_t, 4> widened{};
		std::memcpy(widened.data(), cpu.vectors[6].bytes.data(), 16);
		require(widened == std::array<std::uint32_t, 4>{0x3f80'0000U,
			0x4000'0000U, 0xbf00'0000U, 0x4780'0000U});

		cpu = {};
		cpu.fpcr = 1U << 24; // FZ
		std::memcpy(cpu.vectors[3].bytes.data(), &two, sizeof(two));
		cpu = run_after_inexact(
			::uml::platform::apple::probed_host_capabilities(),
			0x1e7e'0064U, cpu); // fjcvtzs w4,d3
		require(cpu.registers[4] == 2U && cpu.nzcv == 0x4000'0000U);

		/* FPSR bits 31:28 are RES0: an MSR FPSR that sets them leaves
		 * nothing for the model's materialization or the execution
		 * boundary to commit. */
		const std::array convert{
			0xd51b'4426U, // msr fpsr,x6
			0x1e7e'0064U, // fjcvtzs w4,d3
			0xd400'0001U, // svc #0
		};
		const auto built = gadgets::Builder{}.build(0x8000, convert, {},
			std::pmr::get_default_resource(), {},
			gadgets::BuildOptions{.host_capabilities =
				::uml::platform::apple::probed_host_capabilities()});
		require(built.complete);
		cpu = {};
		cpu.fpcr = 1U << 24; // FZ
		cpu.registers[6] = 0xf000'0000U | fpsr_saturation;
		std::memcpy(cpu.vectors[3].bytes.data(), &two, sizeof(two));
		::uml::memory::Tlb tlb;
		require(gadgets::execute(built.program, cpu, tlb) ==
			::uml::cpu::EngineEvent::system_call);
		require(cpu.registers[4] == 2U);
		require(cpu.fpsr == fpsr_saturation);
	}
}
