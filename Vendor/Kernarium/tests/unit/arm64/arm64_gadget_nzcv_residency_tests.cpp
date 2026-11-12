// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/gadgets/program.hpp"
#include "require.hpp"
#include "scratch_backing.hpp"

#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sys/mman.h>

namespace arm64 = uml::cpu::arm64;
namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using uml::testing::require;

class Mapping final {
public:
	Mapping()
	{
		backing_.put64(0, 0x0123'4567'89ab'cdefULL);
		require(darwin_uml_static_vm_map_generation(generation,
				reinterpret_cast<void *>(guest),
				backing_.descriptor(), 0,
				DARWIN_UML_GUEST_PAGE_SIZE,
				PROT_READ | PROT_WRITE, 0, 1) == 0);
	}

	Mapping(const Mapping &) = delete;
	Mapping &operator=(const Mapping &) = delete;

	~Mapping()
	{
		require(darwin_uml_static_vm_destroy(generation) == 0);
	}

	static constexpr unsigned long generation = 412;
	static constexpr std::uint64_t guest = 0x1240'000;

private:
	uml::testing::ScratchBacking backing_{
		"uml-nzcv-residency.XXXXXX", DARWIN_UML_GUEST_PAGE_SIZE};
};

template<std::size_t Size>
arm64::CpuState execute(const std::array<std::uint32_t, Size> &instructions,
	arm64::CpuState cpu)
{
	const auto built = gadgets::Builder{}.build(0x9000, instructions);
	require(built.complete);
	auto tlb = std::make_unique<::uml::memory::Tlb>();
	tlb->prepare(Mapping::generation,
		darwin_uml_static_vm_changes(Mapping::generation));
	require(gadgets::execute(built.program, cpu, *tlb) ==
		::uml::cpu::EngineEvent::system_call);
	return cpu;
}

} // namespace

int main()
{
	Mapping mapping;

	{
		const std::array instructions{
			0xb100'0420U, // adds x0,x1,#1
			0xf940'0062U, // ldr x2,[x3] (TLB miss clobbers host PSTATE)
			0x9a1f'03e4U, // adc x4,xzr,xzr
			0x9a87'00c5U, // csel x5,x6,x7,eq
			0xd400'0001U, // svc #0
		};
		arm64::CpuState initial{};
		initial.registers[1] = ~std::uint64_t{};
		initial.registers[3] = Mapping::guest;
		initial.registers[6] = 0x66;
		initial.registers[7] = 0x77;
		const auto cpu = execute(instructions, initial);
		require(cpu.registers[0] == 0);
		require(cpu.registers[2] == 0x0123'4567'89ab'cdefULL);
		require(cpu.registers[4] == 1);
		require(cpu.registers[5] == 0x66);
		require(cpu.nzcv == 0x6000'0000U);
	}

	{
		const std::array instructions{
			0xd51b'4208U, // msr nzcv,x8
			0xf940'0062U, // ldr x2,[x3]
			0xd53b'4209U, // mrs x9,nzcv
			0x9a87'40c5U, // csel x5,x6,x7,mi
			0xd400'0001U, // svc #0
		};
		arm64::CpuState initial{};
		initial.registers[3] = Mapping::guest;
		initial.registers[6] = 0x66;
		initial.registers[7] = 0x77;
		initial.registers[8] = 0xa000'0000U;
		const auto cpu = execute(instructions, initial);
		require(cpu.registers[5] == 0x66);
		require(cpu.registers[9] == 0xa000'0000U);
		require(cpu.nzcv == 0xa000'0000U);
	}

	{
		const std::array instructions{
			0x1e61'2000U, // fcmp d0,d1
			0xf940'0062U, // ldr x2,[x3]
			0x9a87'00c5U, // csel x5,x6,x7,eq
			0xd400'0001U, // svc #0
		};
		arm64::CpuState initial{};
		initial.registers[3] = Mapping::guest;
		initial.registers[6] = 0x66;
		initial.registers[7] = 0x77;
		const auto one = std::bit_cast<std::uint64_t>(1.0);
		for (unsigned index : {0U, 1U})
			for (unsigned byte = 0; byte < 8; ++byte)
				initial.vectors[index].bytes[byte] =
					static_cast<std::byte>(one >> (byte * 8U));
		const auto cpu = execute(instructions, initial);
		require(cpu.registers[5] == 0x66);
		require(cpu.nzcv == 0x6000'0000U);
	}
}
