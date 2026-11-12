// SPDX-License-Identifier: GPL-2.0-or-later
#include "cpu/arm64/gadgets/program.hpp"
#include "cpu/arm64/id_registers.hpp"
#include "require.hpp"

#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"
#include "static_mmu_test_support.hpp"
#include "mach_vm_region_test_support.hpp"
#include "scratch_backing.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <mach/mach.h>
#include <memory>
#include <sys/mman.h>
#include <unistd.h>

namespace gadgets = uml::cpu::arm64::gadgets;

namespace {

using uml::testing::require;

void require_data_only(const gadgets::Program &program)
{
	require((uml::testing::current_vm_protection(program.words().data()) &
		VM_PROT_EXECUTE) == 0);
}

void data_only_stream_and_execution_boundary()
{
	constexpr std::uint64_t base = 0x4000;
	constexpr std::array instructions{
		0xd503'201fU, // nop
		0xd400'0001U, // svc #0
	};
	const auto built = gadgets::Builder{}.build(base, instructions);
	require(built.complete);
	require(built.program.instruction_count() == instructions.size());
	require(!built.program.words().empty());
	require_data_only(built.program);

	uml::cpu::arm64::CpuState cpu{};
	cpu.pc = base;
	auto tlb = std::make_unique<::uml::memory::Tlb>();
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(built.program, cpu, *tlb) ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.pc == base + instructions.size() * sizeof(std::uint32_t));
	require(!uml::testing::process_has_writable_executable_region());
}

void coherent_data_cache_maintenance_retires()
{
	constexpr std::uint64_t base = 0x6000;
	constexpr std::array instructions{
		0xd50b'7e2cU, // dc civac, x12
		0x9100'0400U, // add x0, x0, #1
		0xd400'0001U, // svc #0
	};
	const auto built = gadgets::Builder{}.build(base, instructions);
	require(built.complete);
	require(built.program.instruction_count() == instructions.size());
	require_data_only(built.program);

	uml::cpu::arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[0] = 6;
	cpu.registers[12] = 0x1234'5678;
	auto tlb = std::make_unique<::uml::memory::Tlb>();
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(built.program, cpu, *tlb) ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.pc == base + instructions.size() * sizeof(std::uint32_t));
	require(cpu.registers[0] == 7);
}

void emulated_identification_registers_read_sanitized_values()
{
	namespace detail = uml::cpu::arm64::detail;
	constexpr std::uint64_t base = 0x6100;
	constexpr std::uint64_t untouched = 0x5555'5555'5555'5555;
	/* Only the fields AT_HWCAP also advertises may be set in
	 * ID_AA64PFR0_EL1: FP, AdvSIMD and DIT. The virtual CPU has no SVE, so
	 * that field reads zero. */
	constexpr std::uint64_t advertised_processor_feature_fields =
		0x000f'0000'00ff'0000ULL;
	constexpr std::array instructions{
		0xd538'0001U, // mrs x1, midr_el1
		0xd538'00a2U, // mrs x2, mpidr_el1
		0xd538'0400U, // mrs x0, id_aa64pfr0_el1
		0xd538'061fU, // mrs xzr, id_aa64isar0_el1
		0xd400'0001U, // svc #0
	};
	const auto built = gadgets::Builder{}.build(base, instructions);
	require(built.complete);
	require(built.program.instruction_count() == instructions.size());
	require_data_only(built.program);

	uml::cpu::arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[3] = untouched;
	auto tlb = std::make_unique<::uml::memory::Tlb>();
	tlb->prepare(0, darwin_uml_static_vm_changes(0));
	require(gadgets::execute(built.program, cpu, *tlb) ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.pc == base + instructions.size() * sizeof(std::uint32_t));
	require(cpu.registers[1] == detail::emulated_midr_el1);
	require(cpu.registers[2] == detail::emulated_mpidr_el1);
	require(cpu.registers[0] ==
		detail::emulated_id_register_value(0xd538'0400U));
	require((cpu.registers[0] &
		~advertised_processor_feature_fields) == 0);
	require(cpu.registers[3] == untouched);

	/* The virtual CPU implements no FEAT_PAuth: APA, API, GPA, and GPI in
	 * ID_AA64ISAR1_EL1 and APA3, GPA3, and PAC_frac in ID_AA64ISAR2_EL1
	 * stay zero. */
	require((detail::emulated_id_register_value(0xd538'0620U) &
		 0xff00'0ff0U) == 0);
	require((detail::emulated_id_register_value(0xd538'0640U) &
		 0x0f00'ff00U) == 0);

	/* The AArch32 identification group is in the same encoding space and is
	 * one Linux does not emulate, so it stays illegal at the block start. */
	constexpr std::array rejected{
		0xd538'0104U, // mrs x4, id_pfr0_el1
		0xd400'0001U, // svc #0
	};
	const auto refused = gadgets::Builder{}.build(base, rejected);
	uml::cpu::arm64::CpuState refused_cpu{};
	refused_cpu.pc = base;
	require(gadgets::execute(refused.program, refused_cpu, *tlb) ==
		::uml::cpu::EngineEvent::illegal_instruction);
	require(refused_cpu.pc == base);
	require(refused_cpu.registers[4] == 0);
}

void code_synchronization_is_a_precise_execution_boundary()
{
	const uml::testing::ScratchBacking backing{
		"uml-gadget-code-sync-test.XXXXXX", DARWIN_UML_GUEST_PAGE_SIZE};

	constexpr unsigned long generation = 18;
	constexpr std::uint64_t guest_page = 0xf'0000;
	constexpr std::uint64_t code_pc = 0x8200;
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(guest_page), backing.descriptor(), 0,
		DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE | PROT_EXEC, 0, 1) == 0);

	constexpr std::array instructions{
		0xd50b'7522U, // ic ivau, x2
		0x9100'0463U, // add x3, x3, #1
	};
	const auto built = gadgets::Builder{}.build(code_pc, instructions);
	require(built.complete);
	require(built.program.instruction_count() == 1);
	require_data_only(built.program);

	uml::cpu::arm64::CpuState cpu{};
	cpu.pc = code_pc;
	cpu.registers[2] = guest_page;
	cpu.registers[3] = 7;
	auto tlb = std::make_unique<::uml::memory::Tlb>();
	tlb->prepare(generation, darwin_uml_static_vm_changes(generation));
	const auto before = darwin_uml_static_vm_code_changes(generation);
	std::size_t executed = 0;
	require(gadgets::execute(built.program, cpu, *tlb, executed) ==
		::uml::cpu::EngineEvent::continued);
	require(executed == 1);
	require(cpu.pc == code_pc + sizeof(std::uint32_t));
	require(cpu.registers[3] == 7);
	require(darwin_uml_static_vm_code_changes(generation) == before + 1);

	require(darwin_uml_static_vm_destroy(generation) == 0);
}

void mapped_memory_runtime_boundaries()
{
	uml::testing::ScratchBacking backing{
		"uml-gadget-runtime-test.XXXXXX", 2 * DARWIN_UML_GUEST_PAGE_SIZE};
	const int fd = backing.descriptor();

	constexpr std::uint64_t guest_page = 0x1'0000;
	constexpr unsigned long generation = 19;
	require(darwin_uml_static_vm_activate(generation) == 0);
	require(darwin_uml_static_vm_map(reinterpret_cast<void *>(guest_page), fd,
		0, DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE, 0, 1) == 0);

	auto tlb = std::make_unique<::uml::memory::Tlb>();

	/*
	 * A direct 64-bit entry must not access beyond its translated page.
	 * The cross-page slow path validates the second page before changing
	 * either page, and resumes the same data-only program once it is mapped.
	 */
	constexpr std::array cross_page_instructions{
		0xf900'0040U, // str x0, [x2]
		0xf940'0045U, // ldr x5, [x2]
		0xd400'0001U, // svc #0
	};
	const auto cross_page_program = gadgets::Builder{}.build(
		0x8400, cross_page_instructions);
	require(cross_page_program.complete);
	constexpr auto crossing_offset = DARWIN_UML_GUEST_PAGE_SIZE - 5;
	constexpr std::uint64_t crossing_address = guest_page + crossing_offset;
	constexpr std::uint64_t crossing_value =
		0x8877'6655'4433'2211ULL;
	std::array<std::byte, 8> zero_bytes{};
	backing.put(crossing_offset, zero_bytes);
	uml::cpu::arm64::CpuState cpu{};
	cpu.registers[0] = crossing_value;
	cpu.registers[2] = crossing_address;
	tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	require(gadgets::execute(cross_page_program.program, cpu, *tlb) ==
		::uml::cpu::EngineEvent::memory_fault);
	require(cpu.pc == 0x8400);
	std::array<std::byte, 8> crossing_bytes{};
	backing.get(crossing_offset, crossing_bytes);
	require(crossing_bytes == zero_bytes);

	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(
			guest_page + DARWIN_UML_GUEST_PAGE_SIZE),
		fd, DARWIN_UML_GUEST_PAGE_SIZE, DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE, 0, 1) == 0);
	cpu = {};
	cpu.registers[0] = crossing_value;
	cpu.registers[2] = crossing_address;
	tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	require(gadgets::execute(cross_page_program.program, cpu, *tlb) ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[5] == crossing_value);
	require(backing.get64(crossing_offset) == crossing_value);

	/*
	 * Select two guest pages in the same TLB entry. Loading A, B,
	 * then A again exercises replacement without confusing their host bases.
	 */
	constexpr std::uint64_t collision_page =
		guest_page + ::uml::memory::tlb_entry_count *
			DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr std::uint64_t first_value = 0x1111'2222'3333'4444ULL;
	constexpr std::uint64_t collision_value =
		0xaaaa'bbbb'cccc'ddddULL;
	backing.put64(0, first_value);
	backing.put64(DARWIN_UML_GUEST_PAGE_SIZE, collision_value);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(collision_page), fd,
		DARWIN_UML_GUEST_PAGE_SIZE, DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE, 0, 1) == 0);
	constexpr std::array collision_instructions{
		0xf940'0040U, // ldr x0, [x2]
		0xf940'0061U, // ldr x1, [x3]
		0xf940'0044U, // ldr x4, [x2]
		0xd400'0001U, // svc #0
	};
	const auto collision_program = gadgets::Builder{}.build(
		0x9000, collision_instructions);
	require(collision_program.complete);
	cpu = {};
	cpu.registers[2] = guest_page;
	cpu.registers[3] = collision_page;
	tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	require(gadgets::execute(collision_program.program, cpu, *tlb) ==
		::uml::cpu::EngineEvent::system_call);
	require(cpu.registers[0] == first_value);
	require(cpu.registers[1] == collision_value);
	require(cpu.registers[4] == first_value);

	/*
	 * Read and write translations retain independent host bases even when
	 * their guest pages collide in the address cache.
	 */
	auto independent_tlb = std::make_unique<::uml::memory::Tlb>();
	independent_tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	auto *read_translation = independent_tlb->translate(
		guest_page, ::uml::memory::Access::read);
	auto *write_translation = independent_tlb->translate(
		collision_page, ::uml::memory::Access::write);
	require(read_translation != nullptr);
	require(write_translation != nullptr);
	constexpr auto guest_page_number =
		guest_page >> DARWIN_UML_GUEST_PAGE_SHIFT;
	constexpr std::size_t collision_slot =
		guest_page_number & (::uml::memory::tlb_entry_count - 1);
	require(independent_tlb->entries[collision_slot].read_page ==
		guest_page_number);
	require(independent_tlb->entries[collision_slot].write_page ==
		collision_page >> DARWIN_UML_GUEST_PAGE_SHIFT);
	require(independent_tlb->translate(guest_page, ::uml::memory::Access::read) ==
		read_translation);
	require(independent_tlb->translate(
		collision_page, ::uml::memory::Access::write) == write_translation);

	/*
	 * A populated slow-path translation remains stable until prepare()
	 * observes the mapping change, then resolves the replacement host page.
	 */
	constexpr std::uint64_t cached_page = 0x3000'000;
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(cached_page), fd, 0,
		DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE, 0, 1) == 0);
	tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	auto *cached_first = tlb->translate(cached_page, ::uml::memory::Access::read);
	require(cached_first != nullptr);
	require(*reinterpret_cast<std::uint64_t *>(cached_first) == first_value);
	require(darwin_uml_static_vm_map(
		reinterpret_cast<void *>(cached_page), fd,
		DARWIN_UML_GUEST_PAGE_SIZE, DARWIN_UML_GUEST_PAGE_SIZE,
		PROT_READ | PROT_WRITE, 0, 1) == 0);
	require(tlb->translate(cached_page, ::uml::memory::Access::read) == cached_first);
	tlb->prepare(generation,
		darwin_uml_static_vm_changes(generation));
	auto *cached_second = tlb->translate(cached_page, ::uml::memory::Access::read);
	require(cached_second != nullptr);
	require(cached_second != cached_first);
	require(*reinterpret_cast<std::uint64_t *>(cached_second) ==
		collision_value);

	require(darwin_uml_static_vm_destroy(generation) == 0);
}

} // namespace

int main()
{
	data_only_stream_and_execution_boundary();
	coherent_data_cache_maintenance_retires();
	emulated_identification_registers_read_sanitized_values();
	code_synchronization_is_a_precise_execution_boundary();
	mapped_memory_runtime_boundaries();
	return 0;
}
