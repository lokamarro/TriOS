// SPDX-License-Identifier: GPL-2.0-or-later
#include "jit_test_support.hpp"
#include "runtime_test_route.hpp"
#include "cpu/arm64/jit/jit_cache_geometry.hpp"
#include "cpu/arm64/jit/jit_executor.hpp"
#include "cpu/arm64/jit/jit_test_api.hpp"
#include "cpu/arm64/id_registers.hpp"
#include "static_mmu_test_support.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

using namespace uml::emulator::tests;

namespace {

[[nodiscard]] constexpr std::uint32_t pair(
	unsigned operation, bool vector, bool load, unsigned mode,
	unsigned imm7, unsigned rn, unsigned rt, unsigned rt2)
{
	return 0x2800'0000U | ((operation & 3U) << 30) |
		(static_cast<unsigned>(vector) << 26) |
		((mode & 3U) << 23) |
		(static_cast<unsigned>(load) << 22) |
		((imm7 & 0x7fU) << 15) | ((rt2 & 31U) << 10) |
		((rn & 31U) << 5) | (rt & 31U);
}

void inline_tlb_and_cross_page_fallback()
{
	constexpr unsigned long generation = 403;
	constexpr std::uint64_t base = 0x72'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr std::uint64_t value = 0x1122'3344'5566'7788ULL;
	JitBacking backing{3};
	backing.put32(0, 0xf900'0401U); // str x1, [x0, #8]
	backing.put32(4, 0xf940'0402U); // ldr x2, [x0, #8]
	backing.put32(8, 0xf900'0081U); // str x1, [x4]
	backing.put32(12, 0xf940'0083U); // ldr x3, [x4]
	backing.put32(16, 0xf86a'6805U); // ldr x5, [x0, x10]
	backing.put32(20, 0xd400'0001U); // svc #0
	JitMapping mapping{generation, base, backing, 3};
	darwin_uml_guest_registers registers{};
	uml::testing::GuestFpTail fp{};
	registers.pc = base;
	registers.x[0] = base + page;
	registers.x[1] = value;
	registers.x[4] = base + 2 * page - 4;
	registers.x[10] = page - 4;

	require(enter_jit(registers, fp, generation) ==
		DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[2] == value);
	require(registers.x[3] == value);
	require(registers.x[5] == value);
	require(backing.get64(page + 8) == value);
}

void write_fault_is_precise()
{
	constexpr unsigned long generation = 404;
	constexpr std::uint64_t base = 0x75'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	JitBacking backing{2};
	backing.put32(0, 0xf900'0021U); // str x1, [x1]
	JitMapping mapping{
		generation, base, backing, 2, PROT_READ | PROT_EXEC};
	darwin_uml_guest_registers registers{};
	uml::testing::GuestFpTail fp{};
	registers.pc = base;
	registers.x[1] = base + page;

	require(enter_jit(registers, fp, generation) ==
		DARWIN_UML_EVENT_FAULT);
	require(registers.pc == base);
	require(registers.fault.address == base + page);
	require(registers.fault.error_code == DARWIN_UML_FAULT_WRITE);
}

void guest_jit_code_invalidation()
{
	constexpr unsigned long generation = 405;
	constexpr std::uint64_t base = 0x77'0000;
	JitBacking backing{1};
	backing.put32(0, 0xd280'0020U); // movz x0, #1
	backing.put32(4, 0xd400'0001U); // svc #0
	JitMapping mapping{generation, base, backing, 1};
	darwin_uml_guest_registers registers{};
	uml::testing::GuestFpTail fp{};
	registers.pc = base;
	require(enter_jit(registers, fp, generation) ==
		DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[0] == 1);

	void *code = nullptr;
	require(darwin_uml_static_vm_translate(
		generation, base, DARWIN_UML_STATIC_WRITE, &code) ==
		DARWIN_UML_STATIC_TRANSLATED);
	const std::uint32_t replacement = 0xd280'0040U; // movz x0, #2
	std::memcpy(code, &replacement, sizeof(replacement));
	darwin_uml_static_vm_invalidate_code(base);
	registers.pc = base;
	require(enter_jit(registers, fp, generation) ==
		DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[0] == 2);
}

void coherent_data_cache_maintenance_retires()
{
	constexpr unsigned long generation = 4052;
	constexpr std::uint64_t base = 0x79'0000;
	JitBacking backing{1};
	backing.put32(0, 0xd50b'7e2cU); // dc civac, x12
	backing.put32(4, 0x9100'0400U); // add x0, x0, #1
	backing.put32(8, 0xd400'0001U); // svc #0
	JitMapping mapping{generation, base, backing, 1};
	darwin_uml_guest_registers registers{};
	uml::testing::GuestFpTail fp{};
	registers.pc = base;
	registers.x[0] = 6;
	registers.x[12] = 0x1234'5678;

	require(enter_jit(registers, fp, generation) ==
		DARWIN_UML_EVENT_SYSCALL);
	require(registers.pc == base + 12);
	require(registers.x[0] == 7);
}

void emulated_identification_registers_read_sanitized_values()
{
	namespace detail = uml::cpu::arm64::detail;
	constexpr unsigned long generation = 4053;
	constexpr std::uint64_t base = 0x7a'0000;
	constexpr std::uint64_t untouched = 0x5555'5555'5555'5555;
	/* Only the fields AT_HWCAP also advertises may be set in
	 * ID_AA64PFR0_EL1: FP, AdvSIMD and DIT. The virtual CPU has no SVE, so
	 * that field reads zero. */
	constexpr std::uint64_t advertised_processor_feature_fields =
		0x000f'0000'00ff'0000ULL;
	JitBacking backing{1};
	backing.put32(0, 0xd538'0001U);  // mrs x1, midr_el1
	backing.put32(4, 0xd538'00a2U);  // mrs x2, mpidr_el1
	backing.put32(8, 0xd538'0400U);  // mrs x0, id_aa64pfr0_el1
	backing.put32(12, 0xd538'061fU); // mrs xzr, id_aa64isar0_el1
	backing.put32(16, 0xd400'0001U); // svc #0
	JitMapping mapping{generation, base, backing, 1};
	darwin_uml_guest_registers registers{};
	uml::testing::GuestFpTail fp{};
	registers.pc = base;
	registers.x[3] = untouched;

	require(enter_jit(registers, fp, generation) ==
		DARWIN_UML_EVENT_SYSCALL);
	require(registers.pc == base + 20);
	require(registers.x[1] == detail::emulated_midr_el1);
	require(registers.x[2] == detail::emulated_mpidr_el1);
	require((registers.x[0] &
		~advertised_processor_feature_fields) == 0);
	require(registers.x[3] == untouched);
}

void unemulated_identification_register_is_illegal()
{
	constexpr unsigned long generation = 4054;
	constexpr std::uint64_t base = 0x7b'0000;
	JitBacking backing{1};
	backing.put32(0, 0xd538'0104U); // mrs x4, id_pfr0_el1
	backing.put32(4, 0xd400'0001U); // svc #0
	JitMapping mapping{generation, base, backing, 1};
	darwin_uml_guest_registers registers{};
	uml::testing::GuestFpTail fp{};
	registers.pc = base;

	require(enter_jit(registers, fp, generation) ==
		DARWIN_UML_EVENT_ILLEGAL_INSTRUCTION);
	require(registers.pc == base);
	require(registers.x[4] == 0);
}

void guest_code_synchronization_instruction_invalidates_jit()
{
	constexpr unsigned long generation = 4051;
	constexpr std::uint64_t base = 0x78'0000;
	constexpr std::size_t invalidator_offset = 16;
	JitBacking backing{1};
	backing.put32(0, 0xd280'0020U);  // movz x0, #1
	backing.put32(4, 0xd400'0001U);  // svc #0
	backing.put32(invalidator_offset, 0xd50b'7521U); // ic ivau, x1
	backing.put32(invalidator_offset + 4, 0x9100'0442U); // add x2, x2, #1
	backing.put32(invalidator_offset + 8, 0xd400'0001U); // svc #0
	JitMapping mapping{generation, base, backing, 1};
	darwin_uml_guest_registers registers{};
	uml::testing::GuestFpTail fp{};

	registers.pc = base;
	require(enter_jit(registers, fp, generation) ==
		DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[0] == 1);

	void *code = nullptr;
	require(darwin_uml_static_vm_translate(
		generation, base, DARWIN_UML_STATIC_WRITE, &code) ==
		DARWIN_UML_STATIC_TRANSLATED);
	const std::uint32_t replacement = 0xd280'0040U; // movz x0, #2
	std::memcpy(code, &replacement, sizeof(replacement));

	const auto before = darwin_uml_static_vm_code_changes(generation);
	registers.pc = base + invalidator_offset;
	registers.x[1] = base;
	registers.x[2] = 0;
	require(enter_jit(registers, fp, generation) ==
		DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[2] == 1);
	require(darwin_uml_static_vm_code_changes(generation) == before + 1);

	registers.pc = base;
	require(enter_jit(registers, fp, generation) ==
		DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[0] == 2);
}

void cross_vcpu_store_invalidates_jit_reservation()
{
	constexpr unsigned long generation = 4062;
	constexpr std::uint64_t base = 0x7a'4000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr std::uint64_t observed = 0x1122'3344'5566'7788ULL;
	constexpr std::uint64_t replacement = 0x8877'6655'4433'2211ULL;
	const auto data = base + page;

	JitBacking backing{2};
	backing.put32(0, 0xc85f'7c23U);  // ldxr x3, [x1]
	backing.put32(4, 0xd400'0001U);  // svc #0
	backing.put32(8, 0xf900'0022U);  // str x2, [x1]
	backing.put32(12, 0xd400'0001U); // svc #0
	backing.put32(16, 0xc805'7c24U); // stxr w5, x4, [x1]
	backing.put32(20, 0xd400'0001U); // svc #0
	backing.put64(page, observed);
	JitMapping mapping{generation, base, backing, 2};

	uml::cpu::arm64::JitExecutor first;
	uml::cpu::arm64::JitExecutor second;
	uml::memory::StaticGuestMemory shared_memory;
	uml::cpu::arm64::CpuState first_cpu{};
	uml::cpu::arm64::CpuState second_cpu{};
	first_cpu.pc = base;
	first_cpu.registers[1] = data;
	first_cpu.registers[4] = replacement;
	second_cpu.pc = base + 8;
	second_cpu.registers[1] = data;
	/*
	 * Store the exact observed bits so only reservation-version
	 * invalidation can make the later conditional store fail.
	 */
	second_cpu.registers[2] = observed;

	require(first.begin_execution(generation, first_cpu.pc));
	std::size_t executed = 0;
	require(first.run_block(first_cpu, shared_memory, generation, executed) ==
		::uml::cpu::EngineEvent::continued);
	require(executed == 1);
	require(first_cpu.pc == base + 4);
	require(first_cpu.registers[3] == observed);
	require(first_cpu.exclusive_size == 8);

	require(second.begin_execution(generation, second_cpu.pc));
	require(second.run_block(
			second_cpu, shared_memory, generation, executed) ==
		::uml::cpu::EngineEvent::system_call);
	require(executed == 2);
	require(second_cpu.pc == base + 16);
	second.end_execution();

	first_cpu.pc = base + 16;
	require(first.run_block(first_cpu, shared_memory, generation, executed) ==
		::uml::cpu::EngineEvent::continued);
	require(executed == 1);
	require(first_cpu.pc == base + 20);
	require(first_cpu.registers[5] == 1);
	require(first_cpu.exclusive_size == 0);
	require(backing.get64(page) == observed);
	first.end_execution();
}

void reservation_contention_keeps_native_store_block()
{
	constexpr unsigned long generation = 4063;
	constexpr std::uint64_t base = 0x7a'8000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr std::uint64_t observed = 0x1020'3040'5060'7080ULL;
	constexpr std::uint64_t unrelated = 0x8877'6655'4433'2211ULL;
	const auto reserved_data = base + page;
	const auto other_data = reserved_data + 64;

	JitBacking backing{2};
	backing.put32(0, 0xc85f'7c23U);  // ldxr x3, [x1]
	backing.put32(4, 0xd400'0001U);  // svc #0
	backing.put32(8, 0xf900'00c2U);  // str x2, [x6]
	backing.put32(12, 0xf900'0024U); // str x4, [x1]
	backing.put32(16, 0xd400'0001U); // svc #0
	backing.put32(20, 0xc805'7c24U); // stxr w5, x4, [x1]
	backing.put32(24, 0xd400'0001U); // svc #0
	backing.put64(page, observed);
	backing.put64(page + 64, 0);
	JitMapping mapping{generation, base, backing, 2};

	uml::cpu::arm64::JitExecutor first;
	uml::cpu::arm64::JitExecutor second;
	uml::memory::StaticGuestMemory shared_memory;
	uml::cpu::arm64::CpuState first_cpu{};
	uml::cpu::arm64::CpuState second_cpu{};
	first_cpu.pc = base;
	first_cpu.registers[1] = reserved_data;
	first_cpu.registers[4] = observed;
	second_cpu.pc = base + 8;
	second_cpu.registers[1] = reserved_data;
	second_cpu.registers[2] = unrelated;
	second_cpu.registers[4] = observed;
	second_cpu.registers[6] = other_data;

	require(first.begin_execution(generation, first_cpu.pc));
	std::size_t executed = 0;
	require(first.run_block(first_cpu, shared_memory, generation, executed) ==
		::uml::cpu::EngineEvent::continued);
	require(executed == 1);
	require(first_cpu.exclusive_size == 8);

	require(second.begin_execution(generation, second_cpu.pc));
	require(second.run_block(
			second_cpu, shared_memory, generation, executed) ==
		::uml::cpu::EngineEvent::system_call);
	/* Contention must select the tracked native copy, not the former
	 * one-instruction semantic fallback. */
	require(executed == 3);
	require(second_cpu.pc == base + 20);
	require(backing.get64(page + 64) == unrelated);
	second.end_execution();

	first_cpu.pc = base + 20;
	require(first.run_block(first_cpu, shared_memory, generation, executed) ==
		::uml::cpu::EngineEvent::continued);
	require(executed == 1);
	require(first_cpu.registers[5] == 1);
	require(backing.get64(page) == observed);
	first.end_execution();
}

void pair_cross_page_slow_path_resumes_native_translation()
{
	constexpr unsigned long generation = 700;
	constexpr std::uint64_t base = 0x7f'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr std::uint64_t first = 0x1122'3344'5566'7788ULL;
	constexpr std::uint64_t second = 0x99aa'bbcc'ddee'ff00ULL;
	JitBacking backing{3};
	backing.put32(0, pair(2, false, false, 2, 0, 2, 3, 4));
	backing.put32(4, pair(2, false, true, 2, 0, 2, 5, 6));
	backing.put32(8, 0xd400'0001U);
	JitMapping mapping{generation, base, backing, 3};
	darwin_uml_guest_registers registers{};
	uml::testing::GuestFpTail fp{};
	registers.pc = base;
	registers.x[2] = base + 2 * page - 8;
	registers.x[3] = first;
	registers.x[4] = second;

	require(enter_jit(registers, fp, generation) ==
		DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[5] == first);
	require(registers.x[6] == second);
}

void deferred_fp_source_is_materialized_and_refreshed()
{
	constexpr unsigned long generation = 701;
	constexpr std::uint64_t base = 0x80'0000;
	constexpr std::uint64_t first = 0x0123'4567'89ab'cdefULL;
	constexpr std::uint64_t second = 0xfedc'ba98'7654'3210ULL;
	JitBacking backing{1};
	backing.put32(0, 0x9e66'0000U); // fmov x0, d0
	backing.put32(4, 0xd400'0001U); // svc #0
	JitMapping mapping{generation, base, backing, 1};
	darwin_uml_guest_registers registers{};
	uml::testing::GuestFpTail fp{};
	registers.pc = base;
	std::memcpy(fp.vectors[0], &first, sizeof(first));

	require(enter_jit(registers, fp, generation) ==
		DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[0] == first);

	/* A fresh entry defers a new kernel frame even when the native block is
	 * already cached.  The JIT boundary must materialize that frame before
	 * loading a vector register.
	 */
	registers.pc = base;
	std::memcpy(fp.vectors[0], &second, sizeof(second));
	require(enter_jit(registers, fp, generation) ==
		DARWIN_UML_EVENT_SYSCALL);
	require(registers.x[0] == second);
}

void direct_links_are_bounded_to_one_translation_window()
{
	constexpr unsigned long generation = 702;
	constexpr std::uint64_t base = 0x81'0000;
	JitBacking backing{1};
	backing.put32(0, 0x1400'0000U); // b .
	JitMapping mapping{generation, base, backing, 1};
	uml::cpu::arm64::JitExecutor executor;
	uml::memory::StaticGuestMemory memory;
	uml::cpu::arm64::CpuState cpu{};
	cpu.pc = base;

	require(executor.begin_execution(generation, cpu.pc));
	std::size_t executed = 0;
	const auto chain_limit =
		uml::cpu::arm64::jit::testing::link_chain_instruction_limit();
	require(executor.run_block(cpu, memory, generation, executed) ==
		::uml::cpu::EngineEvent::continued);
	require(executed == chain_limit);
	require(cpu.pc == base);
	executor.end_execution();
}

void published_links_can_chain_store_blocks()
{
	constexpr unsigned long generation = 703;
	constexpr std::uint64_t base = 0x82'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr std::uint64_t stored = 0x1234'5678'9abc'def0ULL;
	JitBacking backing{2};
	backing.put32(0, 0xf900'0001U);  // str x1, [x0]
	backing.put32(4, 0x1400'0001U);  // b +4
	backing.put32(8, 0x9100'0442U);  // add x2, x2, #1
	backing.put32(12, 0x17ff'fffdU); // b -12
	JitMapping mapping{generation, base, backing, 2};
	uml::cpu::arm64::JitExecutor executor;
	uml::memory::StaticGuestMemory memory;
	uml::cpu::arm64::CpuState cpu{};
	cpu.pc = base;
	cpu.registers[0] = base + page;
	cpu.registers[1] = stored;

	require(executor.begin_execution(generation, cpu.pc));
	std::size_t executed = 0;
	/* The first miss compiles the store side and stops at the still-cold
	 * successor. A read-only outer invocation cannot publish a later store,
	 * so the reverse edge deliberately stops at that boundary. */
	require(executor.run_block(cpu, memory, generation, executed) ==
		::uml::cpu::EngineEvent::continued);
	require(executed == 2);
	require(cpu.pc == base + 8);
	const auto chain_limit =
		uml::cpu::arm64::jit::testing::link_chain_instruction_limit();
	require(executor.run_block(cpu, memory, generation, executed) ==
		::uml::cpu::EngineEvent::continued);
	require(executed == 2);
	require(cpu.pc == base);
	require(cpu.registers[2] == 1);

	/* Entering through the published store side can link through both blocks. */
	cpu.pc = base;
	require(executor.run_block(cpu, memory, generation, executed) ==
		::uml::cpu::EngineEvent::continued);
	require(executed == chain_limit);
	require(cpu.registers[2] == 1 + chain_limit / 4);
	require(backing.get64(page) == stored);

	/* A guest code write must clear every successor activated in the old
	 * code epoch without scanning inactive historical link cells.  Replace
	 * the loop increment after both sides are hot, then require the first
	 * pass to stop at the cleared target and all later linked passes to use
	 * only the replacement block.
	 */
	void *code = nullptr;
	require(darwin_uml_static_vm_translate(
		generation, base + 8, DARWIN_UML_STATIC_WRITE, &code) ==
		DARWIN_UML_STATIC_TRANSLATED);
	const std::uint32_t replacement = 0x9100'0842U; // add x2, x2, #2
	std::memcpy(code, &replacement, sizeof(replacement));
	darwin_uml_static_vm_invalidate_code(base + 8);
	cpu.pc = base;
	cpu.registers[2] = 0;
	require(executor.run_block(cpu, memory, generation, executed) ==
		::uml::cpu::EngineEvent::continued);
	require(executed == 2);
	require(cpu.pc == base + 8);
	require(executor.run_block(cpu, memory, generation, executed) ==
		::uml::cpu::EngineEvent::continued);
	require(executed == 2);
	require(cpu.pc == base);
	require(cpu.registers[2] == 2);
	executor.end_execution();
}

void reservation_helpers_are_isolated_from_published_links()
{
	constexpr unsigned long generation = 704;
	constexpr std::uint64_t base = 0x83'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr std::uint64_t stored = 0x1020'3040'5060'7080ULL;
	JitBacking backing{2};
	backing.put32(0, 0xf900'0001U);  // str x1, [x0]
	backing.put32(4, 0x1400'0001U);  // b +4
	backing.put32(8, 0xc85f'7c02U);  // ldxr x2, [x0]
	backing.put32(12, 0xd400'0001U); // svc #0
	backing.put32(16, 0xf900'0001U); // str x1, [x0]
	backing.put32(20, 0xc85f'7c03U); // ldxr x3, [x0]
	backing.put32(24, 0xd400'0001U); // svc #0
	JitMapping mapping{generation, base, backing, 2};
	uml::cpu::arm64::JitExecutor executor;
	uml::memory::StaticGuestMemory memory;
	uml::cpu::arm64::CpuState cpu{};
	cpu.registers[0] = base + page;
	cpu.registers[1] = stored;

	/* Warm the reservation block, then release its monitor while preserving
	 * the compiled target and its deliberately inactive link cell. */
	cpu.pc = base + 8;
	require(executor.begin_execution(generation, cpu.pc));
	std::size_t executed = 0;
	require(executor.run_block(cpu, memory, generation, executed) ==
		::uml::cpu::EngineEvent::continued);
	require(executed == 1);
	require(cpu.pc == base + 12);
	executor.end_execution();

	/* A store-published predecessor must stop before the warmed target. */
	cpu.pc = base;
	require(executor.begin_execution(generation, cpu.pc));
	require(executor.run_block(cpu, memory, generation, executed) ==
		::uml::cpu::EngineEvent::continued);
	require(executed == 2);
	require(cpu.pc == base + 8);
	executor.end_execution();

	/* The compiler must also split a sequential block before the same helper,
	 * so the earlier store publication is gone before a reservation begins. */
	cpu.pc = base + 16;
	require(executor.begin_execution(generation, cpu.pc));
	require(executor.run_block(cpu, memory, generation, executed) ==
		::uml::cpu::EngineEvent::continued);
	require(executed == 1);
	require(cpu.pc == base + 20);
	executor.end_execution();
}

void linked_successor_keeps_one_frame_through_tlb_helper_and_exits()
{
	constexpr unsigned long generation = 705;
	constexpr std::uint64_t base = 0x84'0000;
	constexpr auto page = DARWIN_UML_GUEST_PAGE_SIZE;
	constexpr std::uint64_t first = 0x0123'4567'89ab'cdefULL;
	constexpr std::uint64_t second = 0xfedc'ba98'7654'3210ULL;
	JitBacking backing{3};
	backing.put32(0, 0x1400'0002U);  // b +8
	backing.put32(8, 0x9100'0463U);  // add x3, x3, #1
	backing.put32(12, 0xf940'0002U); // ldr x2, [x0]
	backing.put32(16, 0x9100'0464U); // add x4, x3, #1
	backing.put32(20, 0xd400'0001U); // svc #0
	backing.put64(page, first);
	backing.put64(2 * page, second);
	JitMapping mapping{generation, base, backing, 3};
	uml::cpu::arm64::JitExecutor executor;
	uml::memory::StaticGuestMemory memory;
	uml::cpu::arm64::CpuState cpu{};
	std::size_t executed = 0;

	/* Compile the predecessor before its successor so the first edge takes
	 * the nullable fallback. */
	cpu.pc = base;
	require(executor.begin_execution(generation, cpu.pc));
	require(executor.run_block(cpu, memory, generation, executed) ==
		::uml::cpu::EngineEvent::continued);
	require(executed == 1);
	require(cpu.pc == base + 8);
	executor.end_execution();

	/* Compile and publish the successor while warming only its first data
	 * page. */
	cpu.registers[0] = base + page;
	cpu.registers[3] = 1;
	require(executor.begin_execution(generation, cpu.pc));
	require(executor.run_block(cpu, memory, generation, executed) ==
		::uml::cpu::EngineEvent::system_call);
	require(executed == 4);
	require(cpu.registers[2] == first);
	require(cpu.registers[3] == 2);
	require(cpu.registers[4] == 3);
	executor.end_execution();

	/* The linked successor enters after its external prologue. Its first
	 * access to the second data page forces the preserving TLB helper, then a
	 * syscall must unwind the predecessor-owned frame exactly once. */
	cpu.pc = base;
	cpu.registers[0] = base + 2 * page;
	cpu.registers[2] = 0;
	cpu.registers[3] = 10;
	cpu.registers[4] = 0;
	require(executor.begin_execution(generation, cpu.pc));
	require(executor.run_block(cpu, memory, generation, executed) ==
		::uml::cpu::EngineEvent::system_call);
	require(executed == 5);
	require(cpu.pc == base + 24);
	require(cpu.registers[2] == second);
	require(cpu.registers[3] == 11);
	require(cpu.registers[4] == 12);
	executor.end_execution();

	/* A miss which resolves to no mapping takes the successor's cold fault
	 * exit. The earlier add retires and is flushed, the later add does not,
	 * and the same single owner frame is unwound. */
	cpu.pc = base;
	cpu.registers[0] = base + 3 * page;
	cpu.registers[3] = 20;
	cpu.registers[4] = 0x55;
	require(executor.begin_execution(generation, cpu.pc));
	require(executor.run_block(cpu, memory, generation, executed) ==
		::uml::cpu::EngineEvent::memory_fault);
	require(executed == 2);
	require(cpu.pc == base + 12);
	require(cpu.registers[3] == 21);
	require(cpu.registers[4] == 0x55);
	require(executor.fault_address() == base + 3 * page);
	require(executor.fault_access() ==
		uml::memory::Access::read);
	executor.end_execution();
}

} // namespace

int main()
{
	const uml::testing::RuntimeTestRoute runtime_route;
	require(uml::cpu::arm64::set_jit_cache_budget(
		256U * 1024U * 1024U, 3));
	require(jit_available() == 1);
	inline_tlb_and_cross_page_fallback();
	write_fault_is_precise();
	guest_jit_code_invalidation();
	coherent_data_cache_maintenance_retires();
	emulated_identification_registers_read_sanitized_values();
	unemulated_identification_register_is_illegal();
	guest_code_synchronization_instruction_invalidates_jit();
	cross_vcpu_store_invalidates_jit_reservation();
	reservation_contention_keeps_native_store_block();
	pair_cross_page_slow_path_resumes_native_translation();
	deferred_fp_source_is_materialized_and_refreshed();
	direct_links_are_bounded_to_one_translation_window();
	published_links_can_chain_store_blocks();
	reservation_helpers_are_isolated_from_published_links();
	linked_successor_keeps_one_frame_through_tlb_helper_and_exits();
	return 0;
}
