// SPDX-License-Identifier: GPL-2.0-or-later
/* The host runtime side of AArch32: the formats an AArch32 task's status and
 * FPSCR travel in (the architectural positions of the status word's fields),
 * how its state loads into and saves out of CpuState around an engine's
 * run_aarch32() (runtime/aarch32_guest.hpp: key, flags, GE, IT, the FPSCR
 * split, the TPIDR registers and CP15BEN; load then save is the identity
 * over random states in both Thumb and ARM state), the bridge event each exit
 * becomes, which entries the kernel may hand over (an entry at an unmapped PC
 * faults there), and how the entry points and the compat HWCAPs follow what
 * the run presents. */
#include "cpu/arm64/aarch32/a32_profile.hpp"
#include "cpu/arm64/aarch32/guest_key.hpp"
#include "cpu/arm64/aarch32/state_format.hpp"
#include "cpu/arm64/cpu_state.hpp"
#include "cpu/arm64/id_registers.hpp"
#include "cpu/engine_factory.hpp"
#include "runtime/aarch32_guest.hpp"
#include "runtime/context.hpp"
#include "uml_darwin_cpu.h"
#include "uml_darwin_guest.h"
#include "uml_darwin_static_mmu.h"
#include "guest_register_tail.hpp"
#include "require.hpp"
#include "runtime_test_route.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace {

using ::uml::testing::require;
namespace a32 = ::uml::cpu::arm64::aarch32;
namespace arm64 = ::uml::cpu::arm64;
namespace guest = ::uml::runtime::aarch32;
using ::uml::cpu::EngineKind;
using ::uml::cpu::ExecutionResult;
using ::uml::cpu::ExitReason;
using ::uml::memory::MemoryAccess;
using ::uml::testing::GuestFpTail;

/* A deterministic stream, so a failure reproduces. */
class Random final {
public:
	explicit Random(std::uint64_t seed) noexcept : state_{seed} {}
	[[nodiscard]] std::uint64_t next() noexcept
	{
		state_ ^= state_ << 13U;
		state_ ^= state_ >> 7U;
		state_ ^= state_ << 17U;
		return state_;
	}

private:
	std::uint64_t state_;
};

constexpr std::uint64_t sentinel = 0x5a5a'a5a5'5a5a'a5a5ULL;
constexpr std::uint32_t ss_bit = 1U << 21;
constexpr std::uint32_t dit_bit = 1U << 24;
constexpr std::uint32_t daif_bits = 0x1c0U;
constexpr std::uint32_t kernel_bits_mask = ss_bit | dit_bit | daif_bits |
	(1U << 20) | (1U << 22) | (1U << 23);

/* A valid entry whose every field differs from the others', in the SPSR
 * layout the kernel hands over. */
[[nodiscard]] darwin_uml_guest_registers sample_registers(
	Random &random, bool thumb)
{
	darwin_uml_guest_registers registers{};
	for (auto &value : registers.x)
		value = random.next();
	registers.fp = random.next();
	registers.lr = random.next();
	registers.sp = random.next();
	const auto pc = static_cast<std::uint32_t>(random.next());
	registers.pc = pc & ~(thumb ? 1U : 3U);
	std::uint32_t psr = a32::psr_mode_user;
	psr |= static_cast<std::uint32_t>(random.next()) &
		(a32::psr_nzcv | a32::psr_q | a32::psr_ge | kernel_bits_mask);
	if (thumb) {
		psr |= a32::psr_t;
		/* Any ITSTATE ITAdvance can reach: the IT block's own byte. */
		psr |= a32::psr_it_bits(static_cast<std::uint8_t>(random.next()));
	}
	registers.pstate = psr;
	registers.dispatch_flags = static_cast<std::uint32_t>(random.next()) &
		(DARWIN_UML_DISPATCH_MM_SHARED | DARWIN_UML_DISPATCH_A32_CP15BEN);
	registers.tpidrro = random.next();
	registers.original_x0 = random.next();
	registers.fpmr = 0;
	registers.svcr = 0;
	return registers;
}

[[nodiscard]] GuestFpTail sample_fp(Random &random)
{
	GuestFpTail fp{};
	for (auto &vector : fp.vectors)
		for (auto &byte : vector)
			byte = static_cast<std::uint8_t>(random.next());
	fp.fpsr = static_cast<std::uint32_t>(random.next());
	fp.fpcr = static_cast<std::uint32_t>(random.next());
	return fp;
}

/* The pieces of the status and of FPSCR, by their architectural positions,
 * independent of state_format.hpp's masks. */
void status_fields_have_their_architectural_positions()
{
	for (unsigned bit = 0; bit < 8; ++bit) {
		const auto it = static_cast<std::uint8_t>(1U << bit);
		const std::uint32_t expected = bit < 2 ? 1U << (25 + bit) :
			1U << (10 + bit - 2);
		require(a32::psr_it_bits(it) == expected,
			"an IT bit is not where the SPSR keeps it");
		require(a32::psr_itstate(expected) == it);
	}
	for (unsigned lane = 0; lane < 4; ++lane) {
		require(a32::psr_ge_mask(1U << (16 + lane)) == 0xffU << (8 * lane),
			"a GE flag does not widen to its byte lane");
		require(a32::psr_ge_bits(0xffU << (8 * lane)) == 1U << (16 + lane));
		/* A flag is set when its byte is not zero, whatever the byte. */
		require(a32::psr_ge_bits(0x01U << (8 * lane)) == 1U << (16 + lane));
	}
	for (unsigned value = 0; value < 256; ++value) {
		const auto it = static_cast<std::uint8_t>(value);
		require(a32::psr_itstate(a32::psr_it_bits(it)) == it);
		require((a32::psr_it_bits(it) & ~a32::psr_it) == 0);
	}
	for (unsigned value = 0; value < 16; ++value)
		require(a32::psr_ge_bits(a32::psr_ge_mask(value << 16)) ==
			value << 16);
	/* The fields an engine owns do not overlap the kernel's. */
	require((a32::psr_engine_owned & kernel_bits_mask) == 0);
	require((a32::psr_engine_owned & (a32::psr_e | a32::psr_mode_mask |
		 daif_bits)) == 0);
	/* The split of FPSCR. */
	require(a32::fpscr_status_mask == 0xf800'009fU &&
		a32::fpscr_bridge_control_mask == 0x07f7'9f00U,
		"the bridge's split of FPSCR moved");
	require(a32::fpscr_control_mask == (1U << 26 | 1U << 25 | 1U << 24 |
		3U << 22 | 1U << 19));
	require((a32::fpscr_control_mask & arm64::fpcr_writable_mask) ==
		a32::fpscr_control_mask,
		"an AArch32 control bit is not one the host takes");
	require((arm64::fpcr_writable_mask & ~a32::fpscr_control_mask) ==
		0x9f00U, "A64 holds the trap enables and only those more");
	require((a32::fpscr_nzcv & a32::fpscr_status_mask) == a32::fpscr_nzcv);
	require((arm64::fpsr_writable_mask | a32::fpscr_nzcv) ==
		a32::fpscr_status_mask);
}

void load_converts_every_field()
{
	Random random{1};
	for (const bool thumb : {false, true}) {
		auto registers = sample_registers(random, thumb);
		registers.pstate = a32::psr_mode_user | a32::psr_nzcv |
			a32::psr_q | (0b1010U << 16) | ss_bit | dit_bit | daif_bits |
			(thumb ? a32::psr_t | a32::psr_it_bits(0x6c) : 0U);
		registers.pc = thumb ? 0x0001'2342U : 0x0001'2344U;
		registers.dispatch_flags = DARWIN_UML_DISPATCH_A32_CP15BEN;
		registers.tpidrro = 0xffff'ffff'b6f0'0004ULL;
		for (unsigned index = 0; index < 15; ++index)
			registers.x[index] = 0xdead'beef'0000'0000ULL | (0x100 + index);
		for (unsigned index = 15; index < 29; ++index)
			registers.x[index] = sentinel + index;
		auto fp = sample_fp(random);
		fp.fpsr = 0xffff'ffffU;
		fp.fpcr = 0xffff'ffffU;

		arm64::CpuState cpu{};
		cpu.registers[15] = sentinel;
		cpu.registers[29] = sentinel + 1;
		cpu.registers[30] = sentinel + 2;
		cpu.sp = sentinel + 3;
		cpu.tbi_enabled = true;
		cpu.exclusive_address = 0x1000;
		cpu.exclusive_key = 0x1001;
		cpu.exclusive_value = 0x1002;
		cpu.exclusive_value2 = 0x1003;
		cpu.exclusive_version = 0x1004;
		cpu.exclusive_size = 8;
		cpu.gadget_zero = 0x1005;
		cpu.gadget_indirect_miss = &cpu;
		cpu.svcr = 3;
		const unsigned long tls = 0xcafe'f00d'1234'5678UL;
		guest::load_a32_cpu(cpu, registers, fp, tls,
			arm64::fpcr_writable_mask);

		require(cpu.a32 == 1 && !cpu.tbi_enabled,
			"AArch32 has no top byte ignore");
		for (unsigned index = 0; index < 15; ++index)
			require(cpu.registers[index] == 0x100 + index,
				"R0-R14 are not zero-extended");
		require(cpu.registers[15] == sentinel &&
			cpu.registers[29] == sentinel + 1 &&
			cpu.registers[30] == sentinel + 2 && cpu.sp == sentinel + 3,
			"a register that is no part of the task changed");
		require(cpu.pc == (thumb ? a32::t32_key(0x0001'2342U) :
			a32::a32_key(0x0001'2344U)));
		require(a32::key_is_thumb(cpu.pc) == thumb);
		require(cpu.nzcv == 0xf000'0000U && cpu.a32_q == 1);
		require(cpu.a32_ge_mask == 0xff00'ff00U,
			"GE 0b1010 is not the mask of lanes 1 and 3");
		require(cpu.a32_itstate == (thumb ? 0x6c : 0));
		require(cpu.a32_cp15ben == 1);
		require(cpu.a32_tpidruro == 0xb6f0'0004U);
		require(cpu.tls == 0x1234'5678U);
		require(cpu.exclusive_address == ~std::uint64_t{0} &&
			cpu.exclusive_key == 0 && cpu.exclusive_value == 0 &&
			cpu.exclusive_value2 == 0 && cpu.exclusive_version == 0 &&
			cpu.exclusive_size == 0 && cpu.gadget_zero == 0 &&
			cpu.gadget_indirect_miss == nullptr,
			"the monitor survived the entry");
		require(cpu.svcr == 0 && cpu.sme_task == nullptr);
		require(cpu.a32_fpscr_nzcv == 0xf000'0000U);
		require(!cpu.fp_loaded, "the vectors are not loaded lazily");
		cpu.access_fp_state();
		require(cpu.fpcr == a32::fpscr_control_mask,
			"FPSCR control: only AHP, DN, FZ, RMode and FZ16");
		require(cpu.fpsr == arm64::fpsr_writable_mask,
			"FPSCR status without NZCV");
		require(std::memcmp(cpu.vectors.data(), fp.vectors,
			sizeof(fp.vectors)) == 0, "the vectors did not load");
	}

	/* CP15BEN follows the flag alone. */
	constexpr std::uint32_t shared = DARWIN_UML_DISPATCH_MM_SHARED;
	constexpr std::uint32_t barriers = DARWIN_UML_DISPATCH_A32_CP15BEN;
	for (const std::uint32_t flags : {0U, shared, barriers, barriers | shared}) {
		auto registers = sample_registers(random, false);
		registers.dispatch_flags = flags;
		auto fp = sample_fp(random);
		arm64::CpuState cpu{};
		guest::load_a32_cpu(cpu, registers, fp, 0, arm64::fpcr_writable_mask);
		require(cpu.a32_cp15ben ==
			((flags & DARWIN_UML_DISPATCH_A32_CP15BEN) != 0 ? 1 : 0));
	}

	/* A PC that is not aligned stays so in its key, so the engine's PC
	 * alignment test sees it, and comes back as it was. */
	for (const bool thumb : {false, true}) {
		auto registers = sample_registers(random, thumb);
		registers.pc = 0x1003U;
		auto fp = sample_fp(random);
		arm64::CpuState cpu{};
		guest::load_a32_cpu(cpu, registers, fp, 0, arm64::fpcr_writable_mask);
		require(!a32::key_is_canonical(cpu.pc),
			"a misaligned PC became a key an engine would cache");
		auto out = registers;
		unsigned long tls = 0;
		guest::save_a32_cpu(cpu, out, fp, tls);
		require(out.pc == 0x1003U);
	}

	/* The host's FPCR mask limits the control bits. */
	{
		auto registers = sample_registers(random, false);
		auto fp = sample_fp(random);
		fp.fpcr = 0xffff'ffffU;
		arm64::CpuState cpu{};
		guest::load_a32_cpu(cpu, registers, fp, 0, 0);
		cpu.access_fp_state();
		require(cpu.fpcr == 0);
		guest::load_a32_cpu(cpu, registers, fp, 0, 1U << 26 | 1U << 4);
		cpu.access_fp_state();
		require(cpu.fpcr == 1U << 26);
	}
}

void save_repacks_what_the_engine_owns()
{
	Random random{2};
	for (const bool thumb : {false, true}) {
		auto registers = sample_registers(random, thumb);
		registers.pstate = a32::psr_mode_user | ss_bit | dit_bit |
			daif_bits | (thumb ? a32::psr_t | a32::psr_it_bits(0x0c) : 0U);
		const auto original = registers;
		auto fp = sample_fp(random);
		arm64::CpuState cpu{};
		unsigned long tls = 0;
		guest::load_a32_cpu(cpu, registers, fp, 0x1111, arm64::fpcr_writable_mask);
		cpu.access_fp_state();

		/* What an engine might have done: moved the PC across an
		 * interworking branch, set the flags, used a register. */
		cpu.pc = a32::key_from_target(0x4000'2001U);
		cpu.registers[3] = 0xffff'fffeU;
		cpu.registers[14] = 0x8000'0001U;
		cpu.nzcv = 0x9000'0000U;
		cpu.a32_q = 1;
		cpu.a32_ge_mask = 0x0000'ffffU;
		cpu.a32_itstate = 0xe8;
		cpu.tls = 0x2222;
		cpu.a32_fpscr_nzcv = 0x2000'0000U;
		cpu.fpsr = 0x0800'0010U;
		cpu.fpcr = 1U << 24;
		std::memset(cpu.vectors[3].bytes.data(), 0xab, 16);
		cpu.fp_dirty = true;
		const auto before_tpidrro = registers.tpidrro;
		guest::save_a32_cpu(cpu, registers, fp, tls);

		require(registers.x[3] == 0xffff'fffeU && registers.x[14] == 0x8000'0001U,
			"R3 and LR did not return as 32-bit values");
		for (unsigned index = 0; index < 15; ++index)
			require(registers.x[index] >> 32 == 0,
				"R0-R14 return zero-extended");
		for (unsigned index = 15; index < 29; ++index)
			require(registers.x[index] == original.x[index],
				"an x register that is no part of the task changed");
		require(registers.fp == original.fp && registers.lr == original.lr &&
			registers.sp == original.sp, "fp, lr or sp changed");
		require(registers.pc == 0x4000'2000U, "PC without T");
		const std::uint32_t expected = a32::psr_mode_user | ss_bit |
			dit_bit | daif_bits | a32::psr_t | 0x9000'0000U | a32::psr_q |
			(0b0011U << 16) | a32::psr_it_bits(0xe8);
		require(registers.pstate == expected,
			"the status did not repack: engine fields from the state, the "
			"kernel's bits as they were");
		require(registers.tpidrro == before_tpidrro,
			"TPIDRURO is read only");
		require(tls == 0x2222);
		require(fp.fpcr == (1U << 24) && fp.fpsr == (0x2000'0000U | 0x0800'0010U),
			"FPSCR is rejoined across fpsr and fpcr");
		require(std::memcmp(fp.vectors[3], cpu.vectors[3].bytes.data(), 16) == 0,
			"a written vector did not return");
		require(cpu.a32 == 0, "the AArch32 marker outlives the entry");
	}

	/* An untouched FP image is the kernel's: the vectors are not written back
	 * and FPSCR returns as the entry masked it. */
	{
		auto registers = sample_registers(random, false);
		auto fp = sample_fp(random);
		fp.fpcr = 0xffff'ffffU;
		fp.fpsr = 0xffff'ffffU;
		const auto image = fp;
		arm64::CpuState cpu{};
		unsigned long tls = 0;
		guest::load_a32_cpu(cpu, registers, fp, 0, arm64::fpcr_writable_mask);
		require(!cpu.fp_loaded);
		guest::save_a32_cpu(cpu, registers, fp, tls);
		require(std::memcmp(fp.vectors, image.vectors, sizeof(fp.vectors)) == 0,
			"an untouched image changed");
		require(fp.fpcr == a32::fpscr_control_mask &&
			fp.fpsr == (arm64::fpsr_writable_mask | a32::fpscr_nzcv),
			"FPSCR of an untouched image");
	}
}

/* Load then save reproduces the kernel's state exactly, over many states, and
 * the FP halves as the profile masks them. */
void load_then_save_is_the_identity()
{
	Random random{3};
	for (unsigned round = 0; round < 20000; ++round) {
		const bool thumb = (round & 1U) != 0;
		auto registers = sample_registers(random, thumb);
		/* Any PC, aligned or not. */
		registers.pc = static_cast<std::uint32_t>(random.next());
		const auto original = registers;
		auto fp = sample_fp(random);
		const auto image = fp;
		arm64::CpuState cpu{};
		unsigned long tls = 0;
		guest::load_a32_cpu(cpu, registers, fp, 0x7777'0000U + round,
			arm64::fpcr_writable_mask);
		cpu.registers[15] = random.next();
		guest::save_a32_cpu(cpu, registers, fp, tls);
		for (unsigned index = 0; index < 15; ++index)
			require(registers.x[index] ==
				(original.x[index] & 0xffff'ffffU));
		require(registers.pc == original.pc && registers.pstate ==
			original.pstate, "the status did not return unchanged");
		require(registers.dispatch_flags == original.dispatch_flags &&
			registers.tpidrro == original.tpidrro &&
			registers.original_x0 == original.original_x0 &&
			registers.sp == original.sp && registers.lr == original.lr &&
			registers.fp == original.fp);
		require(tls == 0x7777'0000U + round);
		require(fp.fpcr == (image.fpcr & a32::fpscr_control_mask) &&
			fp.fpsr == (image.fpsr & a32::fpscr_status_mask),
			"FPSCR is not the image masked by the profile");
		require(std::memcmp(fp.vectors, image.vectors, sizeof(fp.vectors)) == 0);
	}
}

void only_valid_entries_are_accepted()
{
	Random random{4};
	for (unsigned round = 0; round < 64; ++round) {
		const auto registers = sample_registers(random, (round & 1U) != 0);
		require(guest::entry_is_valid(registers),
			"a valid entry was refused");
	}
	auto base = sample_registers(random, true);
	base.pstate = a32::psr_mode_user | a32::psr_t;
	require(guest::entry_is_valid(base));

	auto refuse = [&](auto &&change) {
		auto registers = base;
		change(registers);
		require(!guest::entry_is_valid(registers),
			"an entry the kernel may not hand over was accepted");
	};
	/* An AArch64 status, and every mode but User. */
	refuse([](auto &r) { r.pstate &= ~a32::psr_mode_mask; });
	for (const std::uint32_t mode : {0x11U, 0x12U, 0x13U, 0x17U, 0x1bU, 0x1fU,
		     0x1aU, 0x0U})
		refuse([mode](auto &r) {
			r.pstate = (r.pstate & ~a32::psr_mode_mask) | mode;
		});
	/* Big-endian state: SETEND is undefined, so the kernel never sets E. */
	refuse([](auto &r) { r.pstate |= a32::psr_e; });
	/* IT bits belong to a Thumb state. */
	refuse([](auto &r) {
		r.pstate = (r.pstate & ~a32::psr_t) | a32::psr_it_bits(0x08);
	});
	refuse([](auto &r) {
		r.pstate = (r.pstate & ~a32::psr_t) | a32::psr_it_bits(0x01);
	});
	/* Streaming mode or ZA. */
	refuse([](auto &r) { r.svcr = 1; });
	refuse([](auto &r) { r.svcr = 2; });
	/* A and I and F, SS and DIT are the kernel's to set. */
	auto registers = base;
	registers.pstate |= daif_bits | ss_bit | dit_bit;
	require(guest::entry_is_valid(registers));
}

[[nodiscard]] ExecutionResult exit_of(ExitReason reason)
{
	ExecutionResult result;
	result.reason = reason;
	return result;
}

void exits_become_bridge_events()
{
	arm64::CpuState cpu{};
	cpu.pc = a32::t32_key(0x8000'1002U);
	darwin_uml_guest_registers registers{};
	const auto event = [&](const ExecutionResult &result, bool stop = false) {
		registers.fault = {};
		registers.fault.error_code = -1;
		registers.fault.trap_number = -1;
		return guest::exit_event(registers, cpu, result, stop);
	};

	require(event(exit_of(ExitReason::system_call)) ==
		DARWIN_UML_EVENT_SYSCALL);
	require(event(exit_of(ExitReason::illegal_instruction)) ==
		DARWIN_UML_EVENT_ILLEGAL_INSTRUCTION);
	require(event(exit_of(ExitReason::instruction_budget)) ==
		DARWIN_UML_EVENT_INTERRUPT);
	require(event(exit_of(ExitReason::interrupt)) ==
		DARWIN_UML_EVENT_INTERRUPT);
	require(event(exit_of(ExitReason::interrupt), true) ==
		DARWIN_UML_EVENT_STOP);

	for (const auto access : {MemoryAccess::read, MemoryAccess::write,
		     MemoryAccess::execute}) {
		for (const bool alignment : {false, true}) {
			auto result = exit_of(ExitReason::memory_fault);
			result.fault.address = 0xffff'0f60U;
			result.fault.access = access;
			result.fault.alignment = alignment;
			require(event(result) == (alignment ?
				DARWIN_UML_EVENT_DATA_ALIGNMENT : DARWIN_UML_EVENT_FAULT));
			require(registers.fault.address == 0xffff'0f60U &&
				registers.fault.trap_number == DARWIN_UML_FAULT_PAGE);
			require(registers.fault.error_code ==
				(access == MemoryAccess::read ? 0 :
				 access == MemoryAccess::write ? DARWIN_UML_FAULT_WRITE :
				 DARWIN_UML_FAULT_EXECUTE));
		}
	}

	/* BKPT: the immediate and the BKPT32 exception class, at its own PC. */
	auto bkpt = exit_of(ExitReason::breakpoint);
	bkpt.syndrome = 0xfedc'abcdU;
	require(event(bkpt) == DARWIN_UML_EVENT_BREAKPOINT);
	require(registers.fault.error_code == 0xabcd &&
		registers.fault.trap_number == 0x38 &&
		registers.fault.address == 0x8000'1002U,
		"BKPT: immediate, exception class and PC");
	/* A completed single step: the software-step class, at the next PC. */
	require(event(exit_of(ExitReason::single_step)) ==
		DARWIN_UML_EVENT_BREAKPOINT);
	require(registers.fault.error_code == 0 &&
		registers.fault.trap_number == 0x32 &&
		registers.fault.address == 0x8000'1002U);
	cpu.pc = a32::a32_key(0x1000'0ffcU);
	require(event(exit_of(ExitReason::single_step)) ==
		DARWIN_UML_EVENT_BREAKPOINT &&
		registers.fault.address == 0x1000'0ffcU);

	/* A PC alignment fault names the 32-bit target the engine reports. */
	auto misaligned = exit_of(ExitReason::pc_alignment);
	misaligned.fault.address = 0x1000'0002U;
	require(event(misaligned) == DARWIN_UML_EVENT_PC_ALIGNMENT);
	require(registers.fault.address == 0x1000'0002U &&
		registers.fault.error_code == 0 && registers.fault.trap_number == 0);

	/* An address wider than 32 bits is the engine's error, not a guest
	 * fault the compat siginfo would truncate. */
	for (const std::uint64_t address : {0x1'0000'0000ULL,
		     0xa64d'000f'5000'1040ULL, ~0ULL}) {
		auto wide = exit_of(ExitReason::memory_fault);
		wide.fault.address = address;
		require(event(wide) == DARWIN_UML_EVENT_FATAL,
			"a fault above 4 GiB reached the guest");
		auto target = exit_of(ExitReason::pc_alignment);
		target.fault.address = address | 2U;
		require(event(target) == DARWIN_UML_EVENT_FATAL,
			"a PC alignment fault above 4 GiB reached the guest");
	}
	auto top = exit_of(ExitReason::memory_fault);
	top.fault.address = 0xffff'ffffU;
	require(event(top) == DARWIN_UML_EVENT_FAULT &&
		registers.fault.address == 0xffff'ffffU);

	/* No trap enables and no SME in AArch32. */
	require(event(exit_of(ExitReason::floating_point_trap)) ==
		DARWIN_UML_EVENT_FATAL);
	require(event(exit_of(ExitReason::sme_access)) == DARWIN_UML_EVENT_FATAL);
	require(event(exit_of(ExitReason::fatal)) == DARWIN_UML_EVENT_FATAL);
}

void compat_hwcaps_follow_the_host()
{
	::uml::testing::RuntimeTestRoute route;
	const auto words = arm64::detail::guest_compat_hwcaps(
		::uml::runtime::current_guest_host_capabilities());
	require(darwin_uml_guest_compat_hwcap() == words[0]);
	require(darwin_uml_guest_compat_hwcap2() == words[1]);
	/* A non-zero word is the single source of truth that this run executes
	 * AArch32: every run sets the words arm64 Linux sets for any 32-bit
	 * task, and NEON. */
	require((darwin_uml_guest_compat_hwcap() &
		arm64::detail::a32_default_hwcap) ==
		arm64::detail::a32_default_hwcap);
	require((darwin_uml_guest_compat_hwcap() &
		arm64::detail::a32_hwcap_neon) != 0);
	/* The 64-bit words are untouched by any of it. */
	require(darwin_uml_guest_hwcap() ==
		arm64::detail::guest_hwcaps(
			::uml::runtime::current_guest_host_capabilities())[0]);
}

/* A valid state is entered in the engine; every state the kernel may not
 * hand over is fatal. */
template<typename Enter>
void entries_follow_the_kernel_contract(Enter enter, unsigned long generation)
{
	::uml::testing::RuntimeTestRoute route;
	Random random{5};

	for (const bool thumb : {false, true}) {
		auto registers = sample_registers(random, thumb);
		registers.pc = 0x0070'0000U | (thumb ? 0U : 0U);
		registers.pstate &= ~(ss_bit | a32::psr_it);
		GuestFpTail fp = sample_fp(random);
		unsigned long tls = 0x77;
		/* A generation is never reused: an engine may cache the context it
		 * built for one, so the Thumb entry gets its own. */
		const unsigned long entry_generation = generation + (thumb ? 1 : 0);
		require(darwin_uml_static_vm_activate(entry_generation) == 0);
		const int event = enter(&registers, &fp, sizeof(fp),
			entry_generation, &tls);
		require(darwin_uml_static_vm_destroy(entry_generation) == 0);
		/* Nothing is mapped, so the first fetch faults at the PC. */
		require(event == DARWIN_UML_EVENT_FAULT,
			"an AArch32 entry at an unmapped PC did not fault");
		require(registers.fault.address == 0x0070'0000U &&
			registers.fault.error_code == DARWIN_UML_FAULT_EXECUTE &&
			registers.pc == 0x0070'0000U);
		require(((registers.pstate & a32::psr_t) != 0) == thumb &&
			(registers.pstate & a32::psr_mode_mask) == a32::psr_mode_user);
	}

	/* An invalid state is fatal. */
	auto registers = sample_registers(random, false);
	registers.pstate |= a32::psr_e;
	GuestFpTail fp = sample_fp(random);
	unsigned long tls = 0;
	require(enter(&registers, &fp, sizeof(fp), generation, &tls) ==
		DARWIN_UML_EVENT_FATAL);
	registers = sample_registers(random, false);
	registers.svcr = 1;
	require(enter(&registers, &fp, sizeof(fp), generation, &tls) ==
		DARWIN_UML_EVENT_FATAL);
}

} // namespace

int main()
{
	status_fields_have_their_architectural_positions();
	load_converts_every_field();
	save_repacks_what_the_engine_owns();
	load_then_save_is_the_identity();
	only_valid_entries_are_accepted();
	exits_become_bridge_events();
	compat_hwcaps_follow_the_host();
	entries_follow_the_kernel_contract(darwin_uml_static_enter_guest, 810);
	if (::uml::cpu::cpu_engine_available(EngineKind::jit))
		entries_follow_the_kernel_contract(darwin_uml_jit_enter_guest, 812);
	std::puts("arm64-a32-host passed");
	return 0;
}
