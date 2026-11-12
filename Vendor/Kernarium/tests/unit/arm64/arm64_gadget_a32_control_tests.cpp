// SPDX-License-Identifier: GPL-2.0-or-later
/* The gadget engine's AArch32 control contract: what an A32 or T32 entry
 * retires, where it stops and what it leaves in the state at each exit
 * (aarch32/guest_key.hpp, engine_execution.hpp). Instruction semantics are
 * the corpus's; these programs pin the translator's control behaviour:
 *
 * - retirement counts, with skipped conditional instructions retired and a
 *   faulting or undefined instruction not, and conditional data processing
 *   that sets no flags as a select rather than behind a skip;
 * - the key every exit leaves in cpu.pc, the architectural fault address,
 *   and ITSTATE at SVC, at a fault and in an entry in the middle of a block;
 * - interworking branches in both directions and the A32 target with
 *   bit 1 set;
 * - the instruction budget, single step and code that changes in place;
 * - exclusives (including loops whose plain stores share the target's TLB
 *   set), the Armv8 acquire and release forms, the compare-and-swap helpers
 *   of the kernel's user-helper page, the CP15 and counter reads and the
 *   system forms;
 * - LDM, STM and LDRD across a page boundary, mapped and not, and the
 *   memory gadgets' TLB probe hitting inside a page and not across one, and
 *   in the set the TLB's own fill path chose;
 * - the floating-point environment: Advanced SIMD floating point runs under
 *   the standard FPSCR value while VFP keeps the guest's (in either order, in
 *   a skipped conditional run and across a block that ends inside a run), and
 *   the conversions, VMAX, VMIN, VADD, VSUB and VMUL run in whichever
 *   environment is native exactly when their operands allow it, with the
 *   value, flags and environment left checked against the host's own
 *   instruction under the matching FPCR;
 * - that no gadget writes the callee-saved halves of V8-V15 (the corpus
 *   runner checks them around every case), and that every writeback of a
 *   VLD1 or VST1 copy moves the bytes and the base inside a page and across
 *   one;
 * - the library-exit count gate: with the semantic library's exit counters
 *   on from a clean state, only a form the engine leaves to the library, or
 *   a block transfer that crossed a page, reaches it, and the forms the
 *   engine leaves to it do; and
 * - that a form of an extension (CRC32, AES, SHA-1, PMULL, FP16, DotProd,
 *   RDM, FCMA, FHM, BF16, I8MM, JSCVT) the host has runs (as a gadget, or in
 *   the semantic library when none lowers it) and one of an extension it
 *   lacks is UNDEFINED, in the illegal-instruction exit and not retired.
 *   The test builds one guest on the machine's capabilities and one on a
 *   host with none of the optional ones.
 *
 * Everything runs the production loop (run_translated_aarch32_blocks)
 * over the gadget executor. */
#include "arm64_gadget_a32_test_support.hpp"
#include "runtime_test_route.hpp"

#include "cpu/arm64/aarch32/decode.hpp"
#include "cpu/arm64/aarch32/semantics_exit_counters.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace ::uml::emulator::tests::a32;

/* The encodings the programs use, by instruction. */
namespace a {
constexpr std::uint32_t mov_r0_1 = 0xe3a0'0001U;
constexpr std::uint32_t mov_r1_2 = 0xe3a0'1002U;
constexpr std::uint32_t mov_r1_7 = 0xe3a0'1007U;
constexpr std::uint32_t add_r0_r0_2 = 0xe280'0002U;
constexpr std::uint32_t svc = 0xef00'0000U;
constexpr std::uint32_t cmp_r0_0 = 0xe350'0000U;
constexpr std::uint32_t addne_r1_r1_1 = 0x1281'1001U;
constexpr std::uint32_t addeq_r2_r2_1 = 0x0282'2001U;
constexpr std::uint32_t bne_plus_0 = 0x1a00'0000U; /* to the instruction after the next */
constexpr std::uint32_t b_self = 0xeaff'fffeU;
constexpr std::uint32_t ldr_r1_r2 = 0xe592'1000U;
constexpr std::uint32_t udf = 0xe7f0'00f0U;
constexpr std::uint32_t bkpt_1234 = 0xe121'2374U;
constexpr std::uint32_t bx_r0 = 0xe12f'ff10U;
constexpr std::uint32_t blx_r1 = 0xe12f'ff31U;
constexpr std::uint32_t ldrex_r1_r2 = 0xe192'1f9fU;
constexpr std::uint32_t strex_r3_r1_r2 = 0xe182'3f91U;
constexpr std::uint32_t clrex = 0xf57f'f01fU;
constexpr std::uint32_t sevl = 0xe320'f005U;
/* The Armv8 acquire and release forms, on [r2]. */
constexpr std::uint32_t lda_r1_r2 = 0xe192'1c9fU;
constexpr std::uint32_t ldab_r1_r2 = 0xe1d2'1c9fU;
constexpr std::uint32_t ldah_r1_r2 = 0xe1f2'1c9fU;
constexpr std::uint32_t stl_r1_r2 = 0xe182'fc91U;
constexpr std::uint32_t stlb_r1_r2 = 0xe1c2'fc91U;
constexpr std::uint32_t stlh_r1_r2 = 0xe1e2'fc91U;
constexpr std::uint32_t ldaex_r1_r2 = 0xe192'1e9fU;
constexpr std::uint32_t ldaexb_r1_r2 = 0xe1d2'1e9fU;
constexpr std::uint32_t ldaexh_r1_r2 = 0xe1f2'1e9fU;
constexpr std::uint32_t ldaexd_r6_r7_r2 = 0xe1b2'6e9fU;
constexpr std::uint32_t stlex_r3_r1_r2 = 0xe182'3e91U;
constexpr std::uint32_t stlexb_r3_r1_r2 = 0xe1c2'3e91U;
constexpr std::uint32_t stlexh_r3_r1_r2 = 0xe1e2'3e91U;
constexpr std::uint32_t stlexd_r3_r6_r7_r2 = 0xe1a2'3e96U;
constexpr std::uint32_t cmp_r3_0 = 0xe353'0000U;
constexpr std::uint32_t mrc_tpidruro = 0xee1d'0f70U;
constexpr std::uint32_t mrc_tpidrurw = 0xee1d'0f50U;
constexpr std::uint32_t mcr_tpidrurw = 0xee0d'0f50U;
constexpr std::uint32_t mcr_tpidruro = 0xee0d'0f70U;
constexpr std::uint32_t mcr_dmb = 0xee07'0fbaU;
constexpr std::uint32_t mrc_cntfrq = 0xee1e'0f10U;
constexpr std::uint32_t mrrc_cntvct = 0xec51'0f1eU;
constexpr std::uint32_t mrs_apsr = 0xe10f'0000U;
constexpr std::uint32_t mov_r0_pc = 0xe1a0'000fU;
constexpr std::uint32_t ldm_r0_r1_r4 = 0xe890'001eU;
constexpr std::uint32_t stm_r0_wb_r1_r4 = 0xe8a0'001eU;
constexpr std::uint32_t ldrd_r2_r0 = 0xe1c0'20d0U;
} // namespace a

namespace t {
constexpr std::uint16_t movs_r0_1 = 0x2001;
constexpr std::uint16_t movs_r0_2 = 0x2002;
constexpr std::uint16_t movs_r1_7 = 0x2107;
constexpr std::uint16_t cmp_r0_0 = 0x2800;
constexpr std::uint16_t cmp_r3_0 = 0x2b00;
constexpr std::uint16_t svc = 0xdf00;
constexpr std::uint16_t it_eq = 0xbf08;
constexpr std::uint16_t itt_eq = 0xbf04;
constexpr std::uint16_t ldr_r1_r2 = 0x6811;
constexpr std::uint16_t str_r1_r2 = 0x6011;
constexpr std::uint16_t udf = 0xde00;
constexpr std::uint16_t bkpt_42 = 0xbe42;
constexpr std::uint16_t bx_r0 = 0x4700;
constexpr std::uint16_t mov_r0_pc = 0x4678;
constexpr std::uint16_t b_self = 0xe7fe;
constexpr std::uint16_t sevl = 0xbf50;
} // namespace t

/* ------------------------------------------------------------ retirement */

void straight_line_retires_every_instruction()
{
	{
		Guest guest{930};
		guest.a32(0, {a::mov_r0_1, a::add_r0_r0_2, a::svc});
		auto cpu = a32_state();
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(result.retired_instructions == 3);
		require(cpu.pc == aarch32::a32_key(code_base + 12));
		require(reg(cpu, 0) == 3);
		require(cpu.a32_itstate == 0);
	}
	{
		/* One 16-bit, one 32-bit, one 16-bit: three instructions in
		 * eight bytes. */
		Guest guest{931};
		guest.t32(0, {t::movs_r0_1, 0xf100, 0x0002, t::svc});
		auto cpu = t32_state();
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(result.retired_instructions == 3);
		require(cpu.pc == aarch32::t32_key(code_base + 8));
		require(reg(cpu, 0) == 3);
	}
}

void skipped_conditional_instructions_retire()
{
	Guest guest{932};
	guest.a32(0, {0xe3a0'0000U, a::cmp_r0_0, a::addne_r1_r1_1,
		      a::addeq_r2_r2_1, a::svc});
	auto cpu = a32_state();
	cpu.registers[1] = 10;
	cpu.registers[2] = 20;
	const auto result = guest.run(cpu);
	require(result.reason == ExitReason::system_call);
	require(result.retired_instructions == 5);
	require(reg(cpu, 1) == 10 && reg(cpu, 2) == 21);
}

} // namespace

/* Every skip gadget of a condition: the one whose operand is the distance and
 * those holding a distance of two to nine words. */
#define UML_A32_SKIPS(X, cc)                                                \
	X(cc) X(cc##_2) X(cc##_3) X(cc##_4) X(cc##_5) X(cc##_6) X(cc##_7)  \
		X(cc##_8) X(cc##_9)
#define UML_A32_ALL_SKIPS(X)                                                \
	UML_A32_SKIPS(X, eq) UML_A32_SKIPS(X, ne) UML_A32_SKIPS(X, cs)       \
	UML_A32_SKIPS(X, cc) UML_A32_SKIPS(X, mi) UML_A32_SKIPS(X, pl)       \
	UML_A32_SKIPS(X, vs) UML_A32_SKIPS(X, vc) UML_A32_SKIPS(X, hi)       \
	UML_A32_SKIPS(X, ls) UML_A32_SKIPS(X, ge) UML_A32_SKIPS(X, lt)       \
	UML_A32_SKIPS(X, gt) UML_A32_SKIPS(X, le)
#define UML_A32_DECLARE_SKIP(name) void uml_arm64_gadget_a32_skip_##name();
#define UML_A32_SKIP_ADDRESS(name)                                          \
	reinterpret_cast<std::uintptr_t>(&uml_arm64_gadget_a32_skip_##name),

extern "C" {
UML_A32_ALL_SKIPS(UML_A32_DECLARE_SKIP)
}

namespace {

/* Whether a translation of `words` at the code base holds a skip gadget. */
[[nodiscard]] bool translation_skips(std::span<const std::uint32_t> words)
{
	static const std::uintptr_t skips[] = {
		UML_A32_ALL_SKIPS(UML_A32_SKIP_ADDRESS)};
	const gadgets::BuildOptions options;
	const auto output = gadgets::a32::build(
		{.key = aarch32::a32_key(code_base), .code = std::as_bytes(words)},
		{}, std::pmr::get_default_resource(), {}, options,
		gadgets::a32::decode_profile(options), 64);
	for (const auto word : output.result.program.words())
		if (std::find(std::begin(skips), std::end(skips), word) !=
		    std::end(skips))
			return true;
	return false;
}

/* A conditional data-processing instruction that sets no flags is a select
 * of its result and the register's old value, with no skip and so no branch
 * on the guest's data; one that sets flags, a conditional compare and a
 * conditional load keep their skip. */
void conditional_data_processing_selects_without_a_skip()
{
	const std::array<std::uint32_t, 7> words{
		0xe350'0000U, /* cmp r0, #0 */
		0x03a0'1005U, /* moveq r1, #5 */
		0x1282'2001U, /* addne r2, r2, #1 */
		0xb263'3000U, /* rsblt r3, r3, #0 */
		0xc301'4234U, /* movwgt r4, #0x1234 */
		0x11a0'5006U, /* movne r5, r6 */
		a::svc};
	require(!translation_skips(words));
	struct Case final {
		std::uint32_t r0, r1, r2, r3, r4, r5;
	};
	for (const auto &item : {Case{0, 5, 20, 30, 40, 50},
				 Case{1, 10, 21, 30, 0x1234, 60},
				 Case{~0U, 10, 21, 0U - 30U, 40, 60}}) {
		Guest guest{1070 + item.r0 % 4};
		for (std::uint32_t index = 0; index < words.size(); ++index)
			guest.a32(4 * index, {words[index]});
		auto cpu = a32_state();
		cpu.registers[0] = item.r0;
		for (unsigned index = 1; index <= 6; ++index)
			cpu.registers[index] = 10 * index;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(result.retired_instructions == words.size());
		require(reg(cpu, 1) == item.r1 && reg(cpu, 2) == item.r2 &&
			reg(cpu, 3) == item.r3 && reg(cpu, 4) == item.r4 &&
			reg(cpu, 5) == item.r5);
	}
	/* addsne r2, r2, #1; cmpne r0, #1; ldrne r1, [r2] */
	for (const auto word : {0x1292'2001U, 0x1350'0001U, 0x1592'1000U}) {
		const std::array<std::uint32_t, 2> pair{word, a::svc};
		require(translation_skips(pair));
	}
}

void conditional_branch_retires_taken_or_not()
{
	for (const bool taken : {false, true}) {
		Guest guest{taken ? 933UL : 934UL};
		/* cmp r0,#0 ; bne over ; mov r1,#7 ; over: svc */
		guest.a32(0, {a::cmp_r0_0, a::bne_plus_0, a::mov_r1_7, a::svc});
		auto cpu = a32_state();
		cpu.registers[0] = taken ? 1 : 0;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(result.retired_instructions == (taken ? 3U : 4U));
		require(reg(cpu, 1) == (taken ? 0U : 7U));
		require(cpu.pc == aarch32::a32_key(code_base + 16));
	}
}

void conditional_supervisor_call_is_skipped_or_taken()
{
	for (const bool taken : {false, true}) {
		Guest guest{taken ? 935UL : 936UL};
		/* cmp r0,#0 ; svceq ; mov r1,#7 ; svc */
		guest.a32(0, {a::cmp_r0_0, 0x0f00'0000U, a::mov_r1_7, a::svc});
		auto cpu = a32_state();
		cpu.registers[0] = taken ? 0 : 1;
		auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		if (taken) {
			require(result.retired_instructions == 2);
			require(cpu.pc == aarch32::a32_key(code_base + 8));
			require(reg(cpu, 1) == 0);
		} else {
			require(result.retired_instructions == 4);
			require(cpu.pc == aarch32::a32_key(code_base + 16));
			require(reg(cpu, 1) == 7);
		}
	}
}

/* ------------------------------------------------------------ exits */

void a_data_fault_stops_at_the_instruction()
{
	{
		Guest guest{937};
		guest.a32(0, {a::mov_r0_1, a::ldr_r1_r2, a::svc});
		auto cpu = a32_state();
		cpu.registers[1] = 0x1111;
		cpu.registers[2] = unmapped + 0x40;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::memory_fault);
		require(result.retired_instructions == 1);
		require(cpu.pc == aarch32::a32_key(code_base + 4));
		require(result.fault.address == unmapped + 0x40);
		require(result.fault.access == ::uml::memory::MemoryAccess::read);
		require(reg(cpu, 0) == 1 && reg(cpu, 1) == 0x1111);
	}
	{
		Guest guest{938};
		guest.t32(0, {t::movs_r0_1, t::str_r1_r2, t::svc});
		auto cpu = t32_state();
		cpu.registers[2] = unmapped + 0x80;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::memory_fault);
		require(result.retired_instructions == 1);
		require(cpu.pc == aarch32::t32_key(code_base + 2));
		require(result.fault.address == unmapped + 0x80);
		require(result.fault.access == ::uml::memory::MemoryAccess::write);
		require(cpu.a32_itstate == 0);
	}
}

void a_fault_inside_an_it_block_keeps_its_itstate()
{
	/* cmp r0,#0 ; it eq ; ldreq r1,[r2] ; svc */
	Guest guest{939};
	guest.t32(0, {t::cmp_r0_0, t::it_eq, t::ldr_r1_r2, t::svc});
	auto cpu = t32_state();
	cpu.registers[2] = unmapped;
	const auto result = guest.run(cpu);
	require(result.reason == ExitReason::memory_fault);
	require(result.retired_instructions == 2);
	require(cpu.pc == aarch32::t32_key(code_base + 4));
	require(cpu.a32_itstate == 0x08);
	require(result.fault.address == unmapped);

	/* The same block with the condition false does not touch memory. */
	Guest other{940};
	other.t32(0, {t::cmp_r0_0, t::it_eq, t::ldr_r1_r2, t::svc});
	auto skipped = t32_state();
	skipped.registers[0] = 1;
	skipped.registers[2] = unmapped;
	const auto done = other.run(skipped);
	require(done.reason == ExitReason::system_call);
	require(done.retired_instructions == 4);
	require(skipped.a32_itstate == 0);
}

void a_supervisor_call_inside_an_it_block_resumes_mid_block()
{
	/* cmp r0,#0 ; itt eq ; svceq ; moveq r0,#2 ; svc */
	Guest guest{941};
	guest.t32(0, {t::cmp_r0_0, t::itt_eq, t::svc, t::movs_r0_2, t::svc});
	auto cpu = t32_state();
	auto result = guest.run(cpu);
	require(result.reason == ExitReason::system_call);
	require(result.retired_instructions == 3);
	require(cpu.pc == aarch32::t32_key(code_base + 6));
	/* ITSTATE for the next instruction: ITAdvance of 0x04. */
	require(cpu.a32_itstate == 0x08);
	require(reg(cpu, 0) == 0);

	/* The entry in the middle of the block runs one instruction at a time
	 * until ITSTATE is zero, then the rest as usual. */
	result = guest.run(cpu);
	require(result.reason == ExitReason::system_call);
	require(result.retired_instructions == 2);
	require(cpu.pc == aarch32::t32_key(code_base + 10));
	require(cpu.a32_itstate == 0);
	require(reg(cpu, 0) == 2);

	/* A condition that fails skips the rest of the block's instructions
	 * from the same entry. */
	Guest failing{942};
	failing.t32(0, {t::cmp_r0_0, t::itt_eq, t::svc, t::movs_r0_2, t::svc});
	auto skipped = t32_state();
	skipped.registers[0] = 5;
	require(failing.run(skipped).retired_instructions == 5);
	require(reg(skipped, 0) == 5);
}

void an_undefined_instruction_does_not_retire()
{
	{
		Guest guest{943};
		guest.a32(0, {a::mov_r0_1, a::udf, a::svc});
		auto cpu = a32_state();
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::illegal_instruction);
		require(result.retired_instructions == 1);
		require(cpu.pc == aarch32::a32_key(code_base + 4));
	}
	{
		Guest guest{944};
		guest.t32(0, {t::movs_r0_1, t::udf, t::svc});
		auto cpu = t32_state();
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::illegal_instruction);
		require(result.retired_instructions == 1);
		require(cpu.pc == aarch32::t32_key(code_base + 2));
	}
	/* A word the profile does not have: SWP is undefined, never run. */
	Guest swp{945};
	swp.a32(0, {a::mov_r0_1, 0xe102'1093U, a::svc});
	auto cpu = a32_state();
	const auto result = swp.run(cpu);
	require(result.reason == ExitReason::illegal_instruction);
	require(result.retired_instructions == 1);
	require(cpu.pc == aarch32::a32_key(code_base + 4));
}

void a_conditional_undefined_word_is_skipped_when_its_condition_fails()
{
	/* An unallocated coprocessor word under a condition that fails
	 * retires like any skipped instruction; one that passes is the
	 * undefined-instruction exit. Coprocessor 7 is in no profile. */
	const std::uint32_t mrc_p7 = 0x0e10'0710U; /* mrceq p7, 0, r0, c0, c0, 0 */
	for (const bool pass : {false, true}) {
		Guest guest{pass ? 946UL : 947UL};
		guest.a32(0, {a::cmp_r0_0, mrc_p7, a::svc});
		auto cpu = a32_state();
		cpu.registers[0] = pass ? 0 : 1;
		const auto result = guest.run(cpu);
		if (pass) {
			require(result.reason == ExitReason::illegal_instruction);
			require(result.retired_instructions == 1);
			require(cpu.pc == aarch32::a32_key(code_base + 4));
		} else {
			require(result.reason == ExitReason::system_call);
			require(result.retired_instructions == 3);
		}
	}
}

/* BKPT retires like SVC and the A64 BRK do, and leaves the PC at itself. */
void a_breakpoint_reports_its_immediate()
{
	{
		Guest guest{948};
		guest.a32(0, {a::mov_r0_1, a::bkpt_1234, a::svc});
		auto cpu = a32_state();
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::breakpoint);
		require(result.retired_instructions == 2);
		require(result.syndrome == 0x1234);
		require(cpu.pc == aarch32::a32_key(code_base + 4));
	}
	{
		Guest guest{949};
		guest.t32(0, {t::movs_r0_1, t::bkpt_42, t::svc});
		auto cpu = t32_state();
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::breakpoint);
		require(result.retired_instructions == 2);
		require(result.syndrome == 0x42);
		require(cpu.pc == aarch32::t32_key(code_base + 2));
	}
}

/* ------------------------------------------------------------ branches */

void interworking_branches_switch_state_both_ways()
{
	{
		/* A32 bx to a T32 address (bit 0 set). */
		Guest guest{950};
		guest.a32(0, {a::bx_r0});
		guest.t32(0x100, {t::movs_r1_7, t::svc});
		auto cpu = a32_state();
		cpu.registers[0] = code_base + 0x100 + 1;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(result.retired_instructions == 3);
		require(cpu.pc == aarch32::t32_key(code_base + 0x100 + 4));
		require(reg(cpu, 1) == 7);
	}
	{
		/* T32 bx to an A32 address (bit 0 clear). */
		Guest guest{951};
		guest.t32(0, {t::bx_r0});
		guest.a32(0x100, {a::mov_r1_7, a::svc});
		auto cpu = t32_state();
		cpu.registers[0] = code_base + 0x100;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(result.retired_instructions == 3);
		require(cpu.pc == aarch32::a32_key(code_base + 0x100 + 8));
		require(reg(cpu, 1) == 7);
	}
	{
		/* blx r1 from A32 to T32 sets lr to the next A32 instruction. */
		Guest guest{952};
		guest.a32(0, {a::blx_r1});
		guest.t32(0x100, {t::movs_r1_7, t::svc});
		auto cpu = a32_state();
		cpu.registers[1] = code_base + 0x100 + 1;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(reg(cpu, 14) == code_base + 4);
	}
}

void an_a32_target_with_bit_one_set_is_a_pc_alignment_exit()
{
	Guest guest{953};
	guest.a32(0, {a::mov_r1_2, a::bx_r0});
	auto cpu = a32_state();
	cpu.registers[0] = code_base + 0x102;
	const auto result = guest.run(cpu);
	require(result.reason == ExitReason::pc_alignment);
	require(result.retired_instructions == 2);
	require(result.fault.address == code_base + 0x102);
	require(result.fault.access == ::uml::memory::MemoryAccess::execute);
}

void a_fetch_fault_reports_the_unfetchable_address()
{
	Guest guest{954};
	guest.a32(0, {a::mov_r1_2, a::bx_r0});
	auto cpu = a32_state();
	cpu.registers[0] = unmapped;
	const auto result = guest.run(cpu);
	require(result.reason == ExitReason::memory_fault);
	require(result.retired_instructions == 2);
	require(result.fault.address == unmapped);
	require(result.fault.access == ::uml::memory::MemoryAccess::execute);
	require(cpu.pc == aarch32::a32_key(unmapped));
}

/* A 32-bit T32 instruction whose first halfword ends the last mapped page
 * has its second halfword on a page that cannot be fetched: the fetch fault
 * is at that halfword, from a translated block and from the one-instruction
 * path a single step takes. */
void a_t32_instruction_split_over_an_unfetchable_page_faults_at_its_second_halfword()
{
	constexpr std::uint32_t last = data_pages * page - 2;
	for (const bool single_step : {false, true}) {
		Guest guest{single_step ? 1501UL : 1500UL};
		guest.t32(last, {0xf000});
		auto cpu = t32_state(code_base + last);
		const auto result = guest.run(cpu, 10'000, single_step);
		require(result.reason == ExitReason::memory_fault);
		require(result.retired_instructions == 0);
		require(result.fault.address == code_base + last + 2);
		require(result.fault.access ==
			::uml::memory::MemoryAccess::execute);
		require(cpu.pc == aarch32::t32_key(code_base + last));
	}
}

void the_pc_reads_as_the_architectural_value()
{
	{
		Guest guest{955};
		guest.a32(0, {a::mov_r1_2, a::mov_r0_pc, a::svc});
		auto cpu = a32_state();
		(void)guest.run(cpu);
		require(reg(cpu, 0) == code_base + 4 + 8);
	}
	{
		Guest guest{956};
		guest.t32(0, {t::movs_r0_1, t::mov_r0_pc, t::svc});
		auto cpu = t32_state();
		(void)guest.run(cpu);
		require(reg(cpu, 0) == code_base + 2 + 4);
	}
}

/* ------------------------------------------------------------ loop control */

void the_instruction_budget_ends_a_loop()
{
	for (const bool thumb : {false, true}) {
		Guest guest{thumb ? 957UL : 958UL};
		if (thumb)
			guest.t32(0, {t::b_self});
		else
			guest.a32(0, {a::b_self});
		auto cpu = thumb ? t32_state() : a32_state();
		const auto entry = cpu.pc;
		const auto result = guest.run(cpu, 100);
		require(result.reason == ExitReason::instruction_budget);
		require(result.retired_instructions >= 100);
		require(cpu.pc == entry);
	}
}

void single_step_retires_one_instruction_at_a_time()
{
	for (const bool thumb : {false, true}) {
		Guest guest{thumb ? 959UL : 960UL};
		if (thumb)
			guest.t32(0, {t::movs_r0_1, 0xf100, 0x0002, t::svc});
		else
			guest.a32(0, {a::mov_r0_1, a::add_r0_r0_2, a::svc});
		auto cpu = thumb ? t32_state() : a32_state();
		const std::uint32_t second = thumb ? 2U : 4U;
		const std::uint32_t third = thumb ? 6U : 8U;
		auto step = guest.run(cpu, 100, true);
		require(step.reason == ExitReason::single_step);
		require(step.retired_instructions == 1);
		require(cpu.pc == (thumb ? aarch32::t32_key(code_base + second)
					 : aarch32::a32_key(code_base + second)));
		require(reg(cpu, 0) == 1);
		step = guest.run(cpu, 100, true);
		require(step.reason == ExitReason::single_step);
		require(step.retired_instructions == 1);
		require(cpu.pc == (thumb ? aarch32::t32_key(code_base + third)
					 : aarch32::a32_key(code_base + third)));
		require(reg(cpu, 0) == 3);
		step = guest.run(cpu, 100, true);
		require(step.reason == ExitReason::system_call);
		require(step.retired_instructions == 1);
	}
}

void single_step_walks_an_it_block()
{
	/* cmp r0,#0 ; itt eq ; moveq r0,#2 ; addeq... ; svc */
	Guest guest{961};
	guest.t32(0, {t::cmp_r0_0, t::itt_eq, t::movs_r0_2, t::movs_r1_7, t::svc});
	auto cpu = t32_state();
	std::vector<std::uint8_t> itstates;
	for (int step = 0; step < 4; ++step) {
		const auto result = guest.run(cpu, 100, true);
		require(result.reason == ExitReason::single_step);
		require(result.retired_instructions == 1);
		itstates.push_back(cpu.a32_itstate);
	}
	/* After cmp: 0; after itt eq: the block's ITSTATE; after the first
	 * conditional instruction: advanced; after the last: 0. */
	require(itstates == (std::vector<std::uint8_t>{0x00, 0x04, 0x08, 0x00}));
	require(reg(cpu, 0) == 2 && reg(cpu, 1) == 7);
}

void changed_code_is_translated_again()
{
	{
		Guest guest{962};
		guest.a32(0, {a::mov_r0_1, a::svc});
		auto cpu = a32_state();
		require(guest.run(cpu).reason == ExitReason::system_call);
		require(reg(cpu, 0) == 1);
		const std::uint32_t replacement = 0xe3a0'0009U; /* mov r0,#9 */
		guest.patch(code_base, std::as_bytes(std::span{&replacement, 1}));
		cpu = a32_state();
		require(guest.run(cpu).reason == ExitReason::system_call);
		require(reg(cpu, 0) == 9);
	}
	{
		/* A halfword of a 32-bit T32 instruction. */
		Guest guest{963};
		guest.t32(0, {0xf04f, 0x0001, t::svc}); /* mov.w r0,#1 */
		auto cpu = t32_state();
		require(guest.run(cpu).reason == ExitReason::system_call);
		require(reg(cpu, 0) == 1);
		const std::uint16_t replacement = 0x0005;
		guest.patch(code_base + 2, std::as_bytes(std::span{&replacement, 1}));
		cpu = t32_state();
		require(guest.run(cpu).reason == ExitReason::system_call);
		require(reg(cpu, 0) == 5);
	}
}

void a_a32_block_and_a_t32_block_at_one_address_are_distinct()
{
	/* Translations of the two instruction sets coexist in one cache: the
	 * key, which carries the set, names the translation. */
	Guest guest{964};
	guest.a32(0, {a::mov_r0_1, a::svc});
	auto first = a32_state();
	require(guest.run(first).reason == ExitReason::system_call);
	require(reg(first, 0) == 1);
	guest.t32(0x40, {t::movs_r0_2, t::svc});
	auto second = t32_state(code_base + 0x40);
	require(guest.run(second).reason == ExitReason::system_call);
	require(reg(second, 0) == 2);
	first = a32_state();
	require(guest.run(first).reason == ExitReason::system_call);
	require(reg(first, 0) == 1);
}

/* ------------------------------------------------------------ system */

/* A host store-exclusive may fail spuriously, and the first pass over a
 * page the host has not touched does, so a success is always a loop:
 *   top: ldrex r1,[r2] ; add r1,r1,#1 ; strex r3,r1,[r2] ; cmp r3,#0 ;
 *        bne top ; svc */
void exclusive_loops_complete_and_store_once()
{
	for (const bool thumb : {false, true}) {
		Guest guest{thumb ? 965UL : 966UL};
		if (thumb)
			guest.t32(0, {0xe852, 0x1f00, 0x1c49, 0xe842, 0x1300,
				      t::cmp_r3_0, 0xd1f8 /* bne top */, t::svc});
		else
			guest.a32(0, {a::ldrex_r1_r2, 0xe281'1001U,
				      a::strex_r3_r1_r2, a::cmp_r3_0,
				      0x1aff'fffaU /* bne top */, a::svc});
		guest.data32(0x20, 41);
		auto cpu = thumb ? t32_state() : a32_state();
		cpu.registers[2] = data_base + 0x20;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(reg(cpu, 3) == 0);
		require(guest.data32(0x20) == 42);
	}
}

void a_store_without_a_reservation_fails()
{
	/* No load, a cleared reservation, and a load of another address:
	 * none may store. */
	Guest lone{967};
	lone.a32(0, {a::strex_r3_r1_r2, a::svc});
	lone.data32(0x20, 41);
	auto unreserved = a32_state();
	unreserved.registers[1] = 99;
	unreserved.registers[2] = data_base + 0x20;
	require(lone.run(unreserved).reason == ExitReason::system_call);
	require(reg(unreserved, 3) == 1);
	require(lone.data32(0x20) == 41);

	Guest cleared{968};
	cleared.a32(0, {a::ldrex_r1_r2, a::clrex, a::strex_r3_r1_r2, a::svc});
	cleared.data32(0x20, 41);
	auto after_clear = a32_state();
	after_clear.registers[2] = data_base + 0x20;
	require(cleared.run(after_clear).reason == ExitReason::system_call);
	require(reg(after_clear, 3) == 1);
	require(cleared.data32(0x20) == 41);
}

/* The mechanism the exclusives share with A64 (engine/exclusive.inc) makes
 * a loop finish whatever the TLB holds: plain stores to pages that share
 * the target's TLB set, before or between the exclusive load and store,
 * would otherwise evict the target's write entry on every pass. */
[[nodiscard]] constexpr std::uint64_t tlb_set(std::uint64_t key)
{
	return key & (::uml::memory::tlb_entry_count - 1);
}

[[nodiscard]] std::uint32_t colliding_page(std::uint32_t target,
	unsigned ordinal)
{
	const std::uint64_t target_key = target >> DARWIN_UML_GUEST_PAGE_SHIFT;
	unsigned seen = 0;
	for (std::uint64_t key = target_key + 1;; ++key) {
		if (tlb_set(key) != tlb_set(target_key))
			continue;
		if (++seen == ordinal)
			return static_cast<std::uint32_t>(
				key << DARWIN_UML_GUEST_PAGE_SHIFT);
	}
}

/* A program assembled instruction by instruction: an A32 word is four
 * bytes; a T32 value up to 0xffff is a halfword and anything above is a
 * 32-bit instruction, first halfword in the high half. */
class Code final {
public:
	explicit Code(bool thumb) : thumb_{thumb} {}

	void add(std::uint32_t instruction)
	{
		if (!thumb_) {
			bytes_ += 4;
			words_.push_back(instruction);
		} else if (instruction > 0xffffU) {
			bytes_ += 4;
			words_.push_back(instruction);
		} else {
			bytes_ += 2;
			words_.push_back(instruction);
		}
	}
	[[nodiscard]] std::uint32_t size() const noexcept { return bytes_; }

	/* A conditional branch back to the start, taken when NE. */
	void branch_back_ne()
	{
		if (thumb_) {
			const auto back = -static_cast<std::int32_t>(bytes_ + 4);
			add(0xd100U | (static_cast<std::uint32_t>(back / 2) & 0xffU));
		} else {
			const auto back = -static_cast<std::int32_t>(bytes_ + 8) / 4;
			add(0x1a00'0000U | (static_cast<std::uint32_t>(back) & 0xff'ffffU));
		}
	}

	void install(Guest &guest) const
	{
		std::uint32_t offset = 0;
		for (const auto word : words_) {
			if (!thumb_) {
				guest.a32(offset, {word});
				offset += 4;
			} else if (word > 0xffffU) {
				guest.t32(offset, {static_cast<std::uint16_t>(word >> 16),
						   static_cast<std::uint16_t>(word)});
				offset += 4;
			} else {
				guest.t32(offset, {static_cast<std::uint16_t>(word)});
				offset += 2;
			}
		}
	}

private:
	bool thumb_;
	std::uint32_t bytes_{};
	std::vector<std::uint32_t> words_;
};

void exclusive_loops_progress_with_colliding_pages()
{
	enum class Form { word, byte, half, pair };
	unsigned long generation = 1000;
	for (const bool ordered : {false, true})
	for (const bool thumb : {false, true})
	for (const bool inside : {false, true})
	for (const auto form : {Form::word, Form::byte, Form::half, Form::pair}) {
		/* The byte and halfword forms are exercised in A32; T32 has
		 * the same gadgets behind a different decoder. */
		if (thumb && (form == Form::byte || form == Form::half))
			continue;
		Guest guest{generation++};
		const std::uint32_t first = colliding_page(data_base, 1);
		const std::uint32_t second = colliding_page(data_base, 2);
		guest.map_data(first);
		guest.map_data(second);

		/* r2 is the target, r5 and r8 the pages that collide with it,
		 * r4 the value stored there. */
		Code code{thumb};
		const auto plain_stores = [&] {
			code.add(thumb ? 0x602cU : 0xe585'4000U);
			code.add(thumb ? 0xf8c8'4000U : 0xe588'4000U);
		};
		if (!inside)
			plain_stores();
		/* The acquire forms of the same loads. */
		switch (form) {
		case Form::word:
			code.add(thumb ? (ordered ? 0xe8d2'1fefU : 0xe852'1f00U) :
					 (ordered ? a::ldaex_r1_r2 : a::ldrex_r1_r2));
			break;
		case Form::byte:
			code.add(ordered ? a::ldaexb_r1_r2 : 0xe1d2'1f9fU);
			break; /* ldrexb r1,[r2] */
		case Form::half:
			code.add(ordered ? a::ldaexh_r1_r2 : 0xe1f2'1f9fU);
			break; /* ldrexh r1,[r2] */
		case Form::pair:
			code.add(thumb ? (ordered ? 0xe8d2'67ffU : 0xe8d2'677fU) :
					 (ordered ? a::ldaexd_r6_r7_r2 : 0xe1b2'6f9fU));
			break; /* ldrexd r6,r7,[r2] */
		}
		if (inside)
			plain_stores();
		if (form == Form::pair) {
			code.add(thumb ? 0x1c76U : 0xe296'6001U); /* adds r6,r6,#1 */
			code.add(thumb ? 0xf147'0700U : 0xe2a7'7000U); /* adc r7,r7,#0 */
		} else {
			code.add(thumb ? 0x1c49U : 0xe281'1001U); /* add r1,r1,#1 */
		}
		switch (form) {
		case Form::word:
			code.add(thumb ? (ordered ? 0xe8c2'1fe3U : 0xe842'1300U) :
					 (ordered ? a::stlex_r3_r1_r2 : a::strex_r3_r1_r2));
			break;
		case Form::byte:
			code.add(ordered ? a::stlexb_r3_r1_r2 : 0xe1c2'3f91U);
			break; /* strexb r3,r1,[r2] */
		case Form::half:
			code.add(ordered ? a::stlexh_r3_r1_r2 : 0xe1e2'3f91U);
			break; /* strexh r3,r1,[r2] */
		case Form::pair:
			code.add(thumb ? (ordered ? 0xe8c2'67f3U : 0xe8c2'6773U) :
					 (ordered ? a::stlexd_r3_r6_r7_r2 : 0xe1a2'3f96U));
			break; /* strexd r3,r6,r7,[r2] */
		}
		code.add(thumb ? t::cmp_r3_0 : a::cmp_r3_0);
		code.branch_back_ne();
		code.add(thumb ? t::svc : a::svc);
		code.install(guest);

		guest.data32(0x20, 41);
		guest.data32(0x24, 1000);
		auto cpu = thumb ? t32_state() : a32_state();
		cpu.registers[2] = data_base + 0x20;
		cpu.registers[4] = 0x5a;
		cpu.registers[5] = first + 0x10;
		cpu.registers[8] = second + 0x10;
		const auto result = guest.run(cpu, 20'000);
		require(result.reason == ExitReason::system_call);
		require(reg(cpu, 3) == 0);
		switch (form) {
		case Form::word:
			require(guest.data32(0x20) == 42);
			break;
		case Form::byte:
			require((guest.data32(0x20) & 0xff) == 42);
			break;
		case Form::half:
			require((guest.data32(0x20) & 0xffff) == 42);
			break;
		case Form::pair:
			/* The low word was incremented. */
			require(guest.data32(0x20) == 42 && guest.data32(0x24) == 1000);
			break;
		}
	}
}

void a_misaligned_exclusive_faults_as_an_alignment_fault()
{
	Guest guest{969};
	guest.a32(0, {a::mov_r0_1, a::ldrex_r1_r2, a::svc});
	auto cpu = a32_state();
	cpu.registers[2] = data_base + 0x22;
	const auto result = guest.run(cpu);
	require(result.reason == ExitReason::memory_fault);
	require(result.fault.alignment);
	require(result.fault.address == data_base + 0x22);
	require(result.retired_instructions == 1);
	require(cpu.pc == aarch32::a32_key(code_base + 4));
}

/* ------------------------------------------- acquire and release (Armv8) */

/* LDA*, STL*: the store writes only its width, the load zero-extends, in
 * both instruction sets. */
void acquire_loads_and_release_stores_round_trip()
{
	struct Size {
		std::uint32_t a32_store, a32_load, t32_store, t32_load;
		std::uint32_t mask;
	};
	constexpr Size sizes[] = {
		{a::stl_r1_r2, a::lda_r1_r2, 0xe8c2'1fafU, 0xe8d2'1fafU,
		 0xffff'ffffU},
		{a::stlb_r1_r2, a::ldab_r1_r2, 0xe8c2'1f8fU, 0xe8d2'1f8fU,
		 0xffU},
		{a::stlh_r1_r2, a::ldah_r1_r2, 0xe8c2'1f9fU, 0xe8d2'1f9fU,
		 0xffffU}};
	unsigned long generation = 1040;
	for (const bool thumb : {false, true})
	for (const auto &size : sizes) {
		Guest guest{generation++};
		if (thumb)
			guest.t32(0, {static_cast<std::uint16_t>(size.t32_store >> 16),
				      static_cast<std::uint16_t>(size.t32_store),
				      0x2100 /* movs r1,#0 */,
				      static_cast<std::uint16_t>(size.t32_load >> 16),
				      static_cast<std::uint16_t>(size.t32_load),
				      t::svc});
		else
			guest.a32(0, {size.a32_store, 0xe3a0'1000U /* mov r1,#0 */,
				      size.a32_load, a::svc});
		guest.data32(0x20, 0xaabb'ccddU);
		auto cpu = thumb ? t32_state() : a32_state();
		cpu.registers[1] = 0x1122'3344U;
		cpu.registers[2] = data_base + 0x20;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(result.retired_instructions == 4);
		require(reg(cpu, 1) == (0x1122'3344U & size.mask));
		require(guest.data32(0x20) ==
			((0xaabb'ccddU & ~size.mask) | (0x1122'3344U & size.mask)));
	}
}

void an_unaligned_acquire_or_release_is_an_alignment_fault()
{
	struct Case {
		std::uint32_t word;
		std::uint32_t address;
		bool write;
	};
	const Case cases[] = {
		{a::lda_r1_r2, 0x22, false}, {a::lda_r1_r2, 0x21, false},
		{a::ldah_r1_r2, 0x21, false}, {a::stl_r1_r2, 0x22, true},
		{a::stlh_r1_r2, 0x21, true}, {a::ldaex_r1_r2, 0x21, false},
		{a::ldaexh_r1_r2, 0x21, false},
		{a::ldaexd_r6_r7_r2, 0x24, false},
		{a::stlex_r3_r1_r2, 0x22, true},
		{a::stlexh_r3_r1_r2, 0x21, true},
		{a::stlexd_r3_r6_r7_r2, 0x24, true}};
	unsigned long generation = 1050;
	for (const auto &item : cases) {
		Guest guest{generation++};
		guest.a32(0, {a::mov_r0_1, item.word, a::svc});
		auto cpu = a32_state();
		cpu.registers[1] = 0x77;
		cpu.registers[2] = data_base + item.address;
		cpu.registers[3] = 0x55;
		cpu.registers[6] = 0x66;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::memory_fault);
		require(result.fault.alignment);
		require(result.fault.address == data_base + item.address);
		require(result.retired_instructions == 1);
		require(cpu.pc == aarch32::a32_key(code_base + 4));
		require(reg(cpu, 1) == 0x77 && reg(cpu, 3) == 0x55 &&
			reg(cpu, 6) == 0x66);
	}
}

void an_acquire_or_release_to_unmapped_memory_faults_without_effect()
{
	unsigned long generation = 1070;
	for (const auto word : {a::lda_r1_r2, a::stl_r1_r2, a::ldaex_r1_r2,
				a::stlex_r3_r1_r2}) {
		Guest guest{generation++};
		guest.a32(0, {a::mov_r0_1, word, a::svc});
		auto cpu = a32_state();
		cpu.registers[1] = 0x1111;
		cpu.registers[2] = unmapped + 0x40;
		cpu.registers[3] = 0x2222;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::memory_fault);
		require(!result.fault.alignment);
		require(result.fault.address == unmapped + 0x40);
		require(result.retired_instructions == 1);
		require(cpu.pc == aarch32::a32_key(code_base + 4));
		require(reg(cpu, 1) == 0x1111 && reg(cpu, 3) == 0x2222);
	}
}

/* The exclusive and ordered transfers are the A64 part's gadgets, which carry
 * no key: they report the position of their instruction in the block. A T32
 * block mixes two instruction lengths, so the position is not the offset, and
 * the engine must turn it back into the key of that instruction. */
void a_t32_exclusive_or_ordered_fault_names_its_instruction()
{
	struct Case {
		std::uint16_t first, second;
		std::uint32_t address;
		bool alignment;
	};
	const Case cases[] = {
		{0xe852, 0x1f00, unmapped + 0x40, false}, /* ldrex r1,[r2] */
		{0xe852, 0x1f00, data_base + 0x22, true},
		{0xe842, 0x1300, unmapped + 0x40, false}, /* strex r3,r1,[r2] */
		{0xe842, 0x1300, data_base + 0x22, true},
		{0xe8d2, 0x1fef, unmapped + 0x40, false}, /* ldaex r1,[r2] */
		{0xe8c2, 0x1fe3, unmapped + 0x40, false}, /* stlex r3,r1,[r2] */
		{0xe8d2, 0x1faf, unmapped + 0x40, false}, /* lda r1,[r2] */
		{0xe8d2, 0x1faf, data_base + 0x22, true},
		{0xe8c2, 0x1faf, unmapped + 0x40, false}, /* stl r1,[r2] */
		{0xe8d2, 0x677f, unmapped + 0x40, false}, /* ldrexd r6,r7,[r2] */
		{0xe8d2, 0x677f, data_base + 0x24, true},
		{0xe8c2, 0x6773, unmapped + 0x40, false}, /* strexd r3,r6,r7,[r2] */
	};
	unsigned long generation = 1090;
	for (const auto &item : cases) {
		Guest guest{generation++};
		/* 16, 32 and 16 bits ahead of it: the transfer is the fourth
		 * instruction, eight bytes in. */
		guest.t32(0, {t::movs_r0_1, 0xf04f, 0x0001, t::movs_r0_1,
			      item.first, item.second, t::svc});
		auto cpu = t32_state();
		cpu.registers[1] = 0x77;
		cpu.registers[2] = item.address;
		cpu.registers[3] = 0x55;
		cpu.registers[6] = 0x66;
		cpu.registers[7] = 0x88;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::memory_fault);
		require(result.fault.alignment == item.alignment);
		require(result.fault.address == item.address);
		require(result.retired_instructions == 3);
		require(cpu.pc == aarch32::t32_key(code_base + 8));
		require(reg(cpu, 1) == 0x77 && reg(cpu, 3) == 0x55 &&
			reg(cpu, 6) == 0x66 && reg(cpu, 7) == 0x88);
	}

	/* The same inside a linked successor, where only the block the stream
	 * belongs to can say which instruction the position names. */
	for (const auto &item : {cases[0], cases[11]}) {
		Guest guest{generation++};
		guest.t32(0, {0xe07e /* b.n 0x100 */});
		guest.t32(0x100, {t::movs_r0_1, 0xf04f, 0x0001, t::movs_r0_1,
				  item.first, item.second, t::svc});
		for (unsigned run = 0; run < 2; ++run) {
			auto cpu = t32_state();
			cpu.registers[1] = 0x77;
			cpu.registers[2] = item.address;
			cpu.registers[3] = 0x55;
			cpu.registers[6] = 0x66;
			cpu.registers[7] = 0x88;
			const auto result = guest.run(cpu);
			require(result.reason == ExitReason::memory_fault);
			require(result.fault.address == item.address);
			require(result.retired_instructions == 4);
			require(cpu.pc == aarch32::t32_key(code_base + 0x100 + 8));
			require(reg(cpu, 1) == 0x77 && reg(cpu, 3) == 0x55 &&
				reg(cpu, 6) == 0x66 && reg(cpu, 7) == 0x88);
		}
	}
}

void a_release_store_without_a_reservation_fails()
{
	Guest lone{1061};
	lone.a32(0, {a::stlex_r3_r1_r2, a::svc});
	lone.data32(0x20, 41);
	auto unreserved = a32_state();
	unreserved.registers[1] = 99;
	unreserved.registers[2] = data_base + 0x20;
	require(lone.run(unreserved).reason == ExitReason::system_call);
	require(reg(unreserved, 3) == 1);
	require(lone.data32(0x20) == 41);

	/* An acquire load reserves for either kind of store, and CLREX takes
	 * the reservation away. */
	Guest cleared{1062};
	cleared.a32(0, {a::ldaex_r1_r2, a::clrex, a::stlex_r3_r1_r2, a::svc});
	cleared.data32(0x20, 41);
	auto after_clear = a32_state();
	after_clear.registers[2] = data_base + 0x20;
	require(cleared.run(after_clear).reason == ExitReason::system_call);
	require(reg(after_clear, 3) == 1);
	require(cleared.data32(0x20) == 41);
}

/* The compare-and-swap helpers of arm64 Linux's compat kuser page, which
 * are conditional release-exclusives in a retry loop:
 *   1: ldaex r3,[r2] ; subs r3,r3,r0 ; stlexeq r3,r1,[r2] ; teqeq r3,#1 ;
 *      beq 1b ; rsbs r0,r3,#0 ; svc
 * and the 64-bit one on a register pair. */
void the_kuser_compare_and_swap_succeeds_or_reports_a_mismatch()
{
	for (const bool match : {true, false}) {
		Guest guest{match ? 1063UL : 1064UL};
		guest.a32(0, {0xe192'3e9fU, 0xe053'3000U, 0x0182'3e91U,
			      0x0333'0001U, 0x0aff'fffaU, 0xe273'0000U, a::svc});
		guest.data32(0x20, 41);
		auto cpu = a32_state();
		cpu.registers[0] = match ? 41 : 40;
		cpu.registers[1] = 42;
		cpu.registers[2] = data_base + 0x20;
		require(guest.run(cpu, 20'000).reason == ExitReason::system_call);
		require((reg(cpu, 0) == 0) == match);
		require(guest.data32(0x20) == (match ? 42U : 41U));
	}
	for (const bool match : {true, false}) {
		Guest guest{match ? 1065UL : 1066UL};
		guest.a32(0, {0xe1b2'6e9fU, 0xe026'8000U, 0xe027'9001U,
			      0xe198'8009U, 0x01a2'3e94U, 0x0333'0001U,
			      0x0aff'fff8U, 0xe273'0000U, a::svc});
		guest.data32(0x20, 0xfffffff0U);
		guest.data32(0x24, 7);
		auto cpu = a32_state();
		cpu.registers[0] = match ? 0xfffffff0U : 0xfffffff1U;
		cpu.registers[1] = 7;
		cpu.registers[3] = 1; /* the status a mismatch leaves */
		cpu.registers[4] = 5;
		cpu.registers[5] = 6;
		cpu.registers[2] = data_base + 0x20;
		require(guest.run(cpu, 20'000).reason == ExitReason::system_call);
		require((reg(cpu, 0) == 0) == match);
		require(guest.data32(0x20) == (match ? 5U : 0xfffffff0U));
		require(guest.data32(0x24) == (match ? 6U : 7U));
	}
}

void set_event_local_is_a_hint()
{
	{
		Guest guest{1067};
		guest.a32(0, {a::sevl, a::mov_r0_1, a::svc});
		auto cpu = a32_state();
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(result.retired_instructions == 3 && reg(cpu, 0) == 1);
	}
	{
		Guest guest{1068};
		guest.t32(0, {t::sevl, t::movs_r0_1, t::svc});
		auto cpu = t32_state();
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(result.retired_instructions == 3 && reg(cpu, 0) == 1);
	}
}

void coprocessor_and_counter_reads()
{
	Guest guest{970};
	guest.a32(0, {a::mrc_tpidruro, 0xe1a0'4000U /* mov r4,r0 */,
		      a::mrc_tpidrurw, 0xe1a0'5000U /* mov r5,r0 */,
		      0xe3a0'0033U /* mov r0,#0x33 */, a::mcr_tpidrurw,
		      a::mrc_tpidrurw, 0xe1a0'6000U /* mov r6,r0 */,
		      a::mrc_cntfrq, 0xe1a0'7000U /* mov r7,r0 */,
		      a::mrrc_cntvct, 0xe1a0'8000U /* mov r8,r0 */, a::svc});
	auto cpu = a32_state();
	cpu.a32_tpidruro = 0xb6f0'0010U;
	cpu.tls = 0x7000'1000U;
	const auto result = guest.run(cpu);
	require(result.reason == ExitReason::system_call);
	require(reg(cpu, 4) == 0xb6f0'0010U);
	require(reg(cpu, 5) == 0x7000'1000U);
	require(reg(cpu, 6) == 0x33);
	require(static_cast<std::uint32_t>(cpu.tls) == 0x33);
	require(reg(cpu, 7) != 0);
	/* CNTVCT is a 64-bit count that has started. */
	require((std::uint64_t{reg(cpu, 1)} << 32 | reg(cpu, 8)) != 0);

	/* TPIDRURO is not writable from EL0. */
	Guest read_only{971};
	read_only.a32(0, {a::mov_r0_1, a::mcr_tpidruro, a::svc});
	auto write = a32_state();
	const auto denied = read_only.run(write);
	require(denied.reason == ExitReason::illegal_instruction);
	require(denied.retired_instructions == 1);
	require(write.pc == aarch32::a32_key(code_base + 4));
}

void the_counter_does_not_run_backwards()
{
	Guest guest{972};
	guest.a32(0, {a::mrrc_cntvct, 0xe1a0'4000U, 0xe1a0'5001U,
		      a::mrrc_cntvct, a::svc});
	auto cpu = a32_state();
	require(guest.run(cpu).reason == ExitReason::system_call);
	const auto first = std::uint64_t{reg(cpu, 5)} << 32 | reg(cpu, 4);
	const auto second = std::uint64_t{reg(cpu, 1)} << 32 | reg(cpu, 0);
	require(second >= first);
}

void cp15_barriers_follow_the_enable()
{
	for (const bool enabled : {false, true}) {
		Guest guest{enabled ? 973UL : 974UL};
		guest.a32(0, {a::mov_r0_1, a::mcr_dmb, a::svc});
		auto cpu = a32_state();
		cpu.a32_cp15ben = enabled ? 1 : 0;
		const auto result = guest.run(cpu);
		if (enabled) {
			require(result.reason == ExitReason::system_call);
			require(result.retired_instructions == 3);
		} else {
			require(result.reason == ExitReason::illegal_instruction);
			require(result.retired_instructions == 1);
			require(cpu.pc == aarch32::a32_key(code_base + 4));
		}
	}
}

void reading_the_status_register_gives_flags_q_ge_and_user_mode()
{
	Guest guest{975};
	guest.a32(0, {a::mrs_apsr, a::svc});
	auto cpu = a32_state();
	cpu.nzcv = 0x6000'0000U;
	cpu.a32_q = 1;
	cpu.a32_ge_mask = 0x00ff'00ffU; /* GE[2] and GE[0] */
	require(guest.run(cpu).reason == ExitReason::system_call);
	require(reg(cpu, 0) == (0x6000'0000U | 1U << 27 | 0b0101U << 16 | 0x10U));
}

/* ------------------------------------------------- VFP and Advanced SIMD */

/* D(n) is the low or high half of V(n / 2). */
void set_d(arm64::CpuState &cpu, unsigned n, std::uint64_t value)
{
	std::memcpy(cpu.vectors[n / 2].bytes.data() + 8U * (n % 2U), &value,
		    sizeof(value));
}

[[nodiscard]] std::uint64_t get_d(const arm64::CpuState &cpu, unsigned n)
{
	std::uint64_t value;
	std::memcpy(&value, cpu.vectors[n / 2].bytes.data() + 8U * (n % 2U),
		    sizeof(value));
	return value;
}

namespace v {
constexpr std::uint32_t vadd_f32_d0_d1_d2 = 0xf201'0d02U; /* Advanced SIMD */
constexpr std::uint32_t vadd_f32_s8_s2_s4 = 0xee31'4a02U; /* VFP */
} // namespace v

/* Advanced SIMD floating point runs under the standard FPSCR value (flush to
 * zero), VFP under the guest's: a denormal sum is flushed by one and kept by
 * the other, in either order, in a skipped conditional run, and across the
 * end of a block that ends inside a run of Advanced SIMD instructions. */
void advanced_simd_floating_point_uses_the_standard_fpscr()
{
	const std::uint64_t denormals = 0x0000'0001'0000'0001ULL;
	for (const unsigned variant : {0U, 1U, 2U, 3U}) {
		Guest guest{990 + variant};
		std::uint32_t offset = 0;
		switch (variant) {
		case 0: /* Advanced SIMD, then VFP */
			guest.a32(0, {v::vadd_f32_d0_d1_d2, v::vadd_f32_s8_s2_s4, a::svc});
			break;
		case 1: /* VFP, then Advanced SIMD, then VFP again */
			guest.a32(0, {v::vadd_f32_s8_s2_s4, v::vadd_f32_d0_d1_d2,
				      v::vadd_f32_s8_s2_s4, a::svc});
			break;
		case 2: { /* a run of Advanced SIMD longer than a block */
			for (unsigned count = 0; count < 70; ++count, offset += 4)
				guest.a32(offset, {v::vadd_f32_d0_d1_d2});
			guest.a32(offset, {v::vadd_f32_s8_s2_s4, a::svc});
			break;
		}
		default: /* a conditional Advanced SIMD instruction, in T32 */
			guest.t32(0, {t::cmp_r0_0, 0xbf18U /* it ne */,
				      0xef01U, 0x0d02U /* vaddne.f32 d0, d1, d2 */,
				      0xee31U, 0x4a02U /* vadd.f32 s8, s2, s4 */,
				      t::svc});
			break;
		}
		for (const bool taken : {true, false}) {
			if (variant != 3 && !taken)
				continue;
			auto cpu = variant == 3 ? t32_state() : a32_state();
			cpu.registers[0] = taken ? 1 : 0;
			set_d(cpu, 0, 0x5555'5555'5555'5555ULL);
			set_d(cpu, 1, denormals);
			set_d(cpu, 2, denormals);
			const auto result = guest.run(cpu);
			require(result.reason == ExitReason::system_call);
			const bool simd_ran = variant != 3 || taken;
			require(get_d(cpu, 0) ==
				(simd_ran ? 0ULL : 0x5555'5555'5555'5555ULL));
			/* S8, the low word of D4, kept its denormal: 1 + 1. */
			require(static_cast<std::uint32_t>(get_d(cpu, 4)) == 2U);
		}
	}
}

/* The native environment holds the Standard FPSCR value or the guest's FPCR
 * and each gadget switches it only when the other one is there: Advanced SIMD
 * floating point stays flushed across a library instruction and across a
 * block, an FPSCR write ends the standard value, and VFP then runs under the
 * guest's FPCR again. */
void the_fp_environment_follows_the_instruction_kind()
{
	constexpr std::uint32_t vadd_f32_d3_d1_d2 = 0xf201'3d02U;
	constexpr std::uint32_t vadd_f32_d9_d1_d2 = 0xf201'9d02U;
	constexpr std::uint32_t vrecps_f32_d5_d6_d7 = 0xf206'5f17U;
	constexpr std::uint32_t vmsr_fpscr_r3 = 0xeee1'3a10U;
	const std::uint64_t denormals = 0x0000'0001'0000'0001ULL;
	for (const bool through_a_branch : {false, true}) {
		Guest guest{1100 + (through_a_branch ? 1U : 0U)};
		std::uint32_t offset = 0;
		guest.a32(offset, {v::vadd_f32_d0_d1_d2, vrecps_f32_d5_d6_d7,
				   vadd_f32_d3_d1_d2});
		offset += 12;
		if (through_a_branch) {
			/* b to the next instruction: it starts a block of its own */
			guest.a32(offset, {0xeaff'ffffU});
			offset += 4;
		}
		guest.a32(offset, {vmsr_fpscr_r3, vadd_f32_d9_d1_d2,
				   v::vadd_f32_s8_s2_s4, a::svc});
		auto cpu = a32_state();
		cpu.registers[3] = 0;
		set_d(cpu, 1, denormals);
		set_d(cpu, 2, denormals);
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(get_d(cpu, 0) == 0 && get_d(cpu, 3) == 0 &&
			get_d(cpu, 9) == 0);
		/* S8 kept its denormal under the guest's FPCR: 1 + 1. */
		require(static_cast<std::uint32_t>(get_d(cpu, 4)) == 2U);
	}
}

/* A conversion between a float and an integer is the same in either native
 * environment unless a denormal input or a rounding mode other than nearest
 * is involved, so it runs in whichever is active, and only the other cases
 * switch to the guest's: the clamp-and-convert idiom (VMAX, VMIN, VCVT) then
 * stays in the Standard FPSCR value. The result and the flags must be the
 * guest's in every case. */
void conversions_to_and_from_integers_run_in_the_active_environment()
{
	constexpr std::uint32_t vadd_f32_d6_d7_d7 = 0xf207'6d07U; /* zeros */
	constexpr std::uint32_t vadd_f32_s9_s2_s3 = 0xee71'4a21U;
	constexpr std::uint32_t vcvt_u32_f32_s8_s4 = 0xeebc'4ac2U;
	constexpr std::uint32_t vcvt_s32_f32_s8_s4 = 0xeebd'4ac2U;
	constexpr std::uint32_t vcvt_f32_s32_s8_s4 = 0xeeb8'4ac2U;
	constexpr std::uint32_t fz = 1U << 24;
	constexpr std::uint32_t rmode_down = 2U << 22;
	struct Case {
		std::uint32_t word;
		std::uint32_t input;
		std::uint32_t fpcr;
		std::uint32_t result;
		std::uint32_t flags; /* IDC IXC IOC of FPSR */
		bool stays; /* Advanced SIMD's environment survives it */
	};
	const Case cases[] = {
		{vcvt_u32_f32_s8_s4, 0x406c'cccdU, 0, 3, 0x10, true},
		{vcvt_s32_f32_s8_s4, 0xc06c'cccdU, 0, 0xffff'fffdU, 0x10, true},
		{vcvt_u32_f32_s8_s4, 0xc06c'cccdU, 0, 0, 0x01, true},
		{vcvt_u32_f32_s8_s4, 0x7f80'0000U, 0, 0xffff'ffffU, 0x01, true},
		{vcvt_u32_f32_s8_s4, 0x7fc0'0000U, 0, 0, 0x01, true},
		{vcvt_u32_f32_s8_s4, 0x0000'0001U, 0, 0, 0x10, false},
		{vcvt_u32_f32_s8_s4, 0x0000'0001U, fz, 0, 0x80, false},
		{vcvt_u32_f32_s8_s4, 0x0000'0000U, 0, 0, 0, true},
		{vcvt_f32_s32_s8_s4, 7U, 0, 0x40e0'0000U, 0, true},
		{vcvt_f32_s32_s8_s4, 0x0100'0001U, 0, 0x4b80'0000U, 0x10, true},
		{vcvt_f32_s32_s8_s4, 0x0100'0001U, rmode_down, 0x4b80'0000U, 0x10, false},
		{vcvt_f32_s32_s8_s4, 0x0100'0003U, rmode_down, 0x4b80'0001U, 0x10, false},
	};
	unsigned long generation = 1900;
	for (const auto &item : cases) {
		for (const unsigned first : {0U, 1U}) {
			Guest guest{generation++};
			/* 0: Advanced SIMD first, so the standard value is native;
			 * 1: VFP first, so the guest's is. */
			guest.a32(0, {first == 0 ? vadd_f32_d6_d7_d7 : vadd_f32_s9_s2_s3,
				      item.word, a::svc});
			auto cpu = a32_state();
			cpu.fpcr = item.fpcr;
			set_d(cpu, 1, 0x3f80'0000'3f80'0000ULL);
			set_d(cpu, 2, item.input);
			require(guest.run(cpu).reason == ExitReason::system_call);
			require(static_cast<std::uint32_t>(get_d(cpu, 4)) == item.result,
				"a conversion gave the wrong value");
			require((cpu.fpsr & 0x91U) == item.flags,
				"a conversion raised the wrong flags");
			require(cpu.a32_fp_standard == (first == 0 && item.stays ? 1U : 0U),
				"a conversion switched the environment when it need not have");
		}
	}
}

/* Single-precision VMAX and VMIN give the same result in either native
 * environment unless an operand is a NaN or a denormal, so they run in the
 * active one and only those operands switch to the standard value, whose
 * flush to zero and default NaN they need. */
void vmax_and_vmin_switch_the_environment_only_for_nan_and_denormal_operands()
{
	constexpr std::uint32_t vmax_f32_d3_d1_d2 = 0xf201'3f02U;
	constexpr std::uint32_t vmin_f32_d3_d1_d2 = 0xf221'3f02U;
	constexpr std::uint32_t vadd_f32_s9_s10_s11 = 0xee75'4a25U;
	constexpr std::uint32_t vadd_f32_d6_d7_d7 = 0xf207'6d07U;
	struct Case {
		std::uint32_t n, m, maximum, minimum;
		std::uint32_t flags; /* IDC IOC of FPSR */
		bool stays;
	};
	const Case cases[] = {
		{0x3f80'0000U, 0x3f00'0000U, 0x3f80'0000U, 0x3f00'0000U, 0, true},
		{0x0000'0000U, 0x8000'0000U, 0x0000'0000U, 0x8000'0000U, 0, true},
		{0x7f80'0000U, 0x3f80'0000U, 0x7f80'0000U, 0x3f80'0000U, 0, true},
		{0xff80'0000U, 0xbf80'0000U, 0xbf80'0000U, 0xff80'0000U, 0, true},
		{0x0080'0000U, 0x8080'0000U, 0x0080'0000U, 0x8080'0000U, 0, true},
		{0x0000'0001U, 0xbf80'0000U, 0x0000'0000U, 0xbf80'0000U, 0x80, false},
		{0x007f'ffffU, 0x3f80'0000U, 0x3f80'0000U, 0x0000'0000U, 0x80, false},
		{0x3f80'0000U, 0x8000'0001U, 0x3f80'0000U, 0x8000'0000U, 0x80, false},
		{0x7fc0'0000U, 0x3f80'0000U, 0x7fc0'0000U, 0x7fc0'0000U, 0, false},
		{0x3f80'0000U, 0x7fc0'1234U, 0x7fc0'0000U, 0x7fc0'0000U, 0, false},
		{0x7f80'0001U, 0x3f80'0000U, 0x7fc0'0000U, 0x7fc0'0000U, 0x01, false},
	};
	unsigned long generation = 1950;
	for (const auto &item : cases) {
		for (const bool minimum : {false, true}) {
			for (const unsigned first : {0U, 1U}) {
				Guest guest{generation++};
				/* 0: Advanced SIMD first, so the standard value is native;
				 * 1: VFP first, so the guest's is. Both add zeros. */
				guest.a32(0, {first == 0 ? vadd_f32_d6_d7_d7 : vadd_f32_s9_s10_s11,
					      minimum ? vmin_f32_d3_d1_d2 : vmax_f32_d3_d1_d2,
					      a::svc});
				auto cpu = a32_state();
				set_d(cpu, 1, std::uint64_t{item.n} << 32 | item.n);
				set_d(cpu, 2, std::uint64_t{item.m} << 32 | item.m);
				require(guest.run(cpu).reason == ExitReason::system_call);
				const std::uint32_t expected = minimum ? item.minimum : item.maximum;
				require(get_d(cpu, 3) == (std::uint64_t{expected} << 32 | expected),
					"VMAX or VMIN gave the wrong value");
				require((cpu.fpsr & 0x81U) == item.flags,
					"VMAX or VMIN raised the wrong flags");
				require(cpu.a32_fp_standard ==
					(first == 0 || !item.stays ? 1U : 0U),
					"VMAX or VMIN switched the environment when it need not have");
			}
		}
	}
}

/* The host's calling convention keeps the low halves of v8 to v15 across the
 * execution boundary, and the translating C++ around it relies on that (a
 * block copy of guest memory held a constant a gadget left there). No gadget
 * may write them. Runs a program of Advanced SIMD, VFP and parallel
 * arithmetic between a write of known values and a read. */
__attribute__((noinline)) void gadgets_keep_the_callee_saved_vector_registers(
	Guest &guest, arm64::CpuState &cpu)
{
	const std::uint64_t before[8] = {
		0x0101'0101'0101'0101ULL, 0x0202'0202'0202'0202ULL,
		0x0303'0303'0303'0303ULL, 0x0404'0404'0404'0404ULL,
		0x0505'0505'0505'0505ULL, 0x0606'0606'0606'0606ULL,
		0x0707'0707'0707'0707ULL, 0x0808'0808'0808'0808ULL};
	std::uint64_t after[8] = {};
	asm volatile("ldp d8, d9, [%0]\n\tldp d10, d11, [%0, #16]\n\t"
		     "ldp d12, d13, [%0, #32]\n\tldp d14, d15, [%0, #48]"
		     :: "r"(before) : "memory", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15");
	const auto result = guest.run(cpu);
	asm volatile("stp d8, d9, [%0]\n\tstp d10, d11, [%0, #16]\n\t"
		     "stp d12, d13, [%0, #32]\n\tstp d14, d15, [%0, #48]"
		     :: "r"(after) : "memory");
	require(result.reason == ExitReason::system_call);
	for (unsigned index = 0; index < 8; ++index)
		require(after[index] == before[index],
			"a gadget wrote a callee-saved vector register");
}

void gadgets_leave_the_callee_saved_vector_registers_alone()
{
	constexpr std::uint32_t vmax_f32_d3_d1_d2 = 0xf201'3f02U;
	constexpr std::uint32_t vmin_f32_q3_q1_q2 = 0xf222'6f44U; /* q-form below */
	constexpr std::uint32_t vcvt_u32_f32_s8_s4 = 0xeebc'4ac2U;
	constexpr std::uint32_t vadd_f32_s9_s10_s11 = 0xee75'4a25U;
	constexpr std::uint32_t uadd8_r0_r1_r2 = 0xe651'0f92U;
	constexpr std::uint32_t sel_r3_r1_r2 = 0xe681'3fb2U;
	unsigned long generation = 2100;
	for (const std::uint32_t operand : {0x3f80'0000U, 0x0000'0001U, 0x7fc0'0000U}) {
		Guest guest{generation++};
		guest.a32(0, {vmax_f32_d3_d1_d2, vmin_f32_q3_q1_q2, vcvt_u32_f32_s8_s4,
			      vadd_f32_s9_s10_s11, uadd8_r0_r1_r2, sel_r3_r1_r2, a::svc});
		auto cpu = a32_state();
		set_d(cpu, 1, std::uint64_t{operand} << 32 | operand);
		set_d(cpu, 2, std::uint64_t{operand} << 32 | operand);
		cpu.registers[1] = 0x8001'ff7fU;
		cpu.registers[2] = 0x7f01'0180U;
		gadgets_keep_the_callee_saved_vector_registers(guest, cpu);
	}
}

/* A single-precision add, subtract or multiply on the host with the given
 * FPCR: the value and the cumulative flags (IDC IXC UFC OFC DZC IOC) it
 * leaves, the reference for what a guest instruction must give. */
struct HostFloat {
	std::uint32_t bits;
	std::uint32_t flags;
};

[[nodiscard]] HostFloat host_float(unsigned operation, std::uint32_t a,
				   std::uint32_t b, std::uint64_t fpcr)
{
	const float x = std::bit_cast<float>(a);
	const float y = std::bit_cast<float>(b);
	float z;
	std::uint64_t saved_fpcr, saved_fpsr, fpsr;
	asm volatile("mrs %0, fpcr\n\tmrs %1, fpsr" : "=r"(saved_fpcr), "=r"(saved_fpsr));
	asm volatile("msr fpcr, %0\n\tmsr fpsr, xzr" :: "r"(fpcr) : "memory");
	if (operation == 0)
		asm volatile("fadd %s0, %s1, %s2" : "=w"(z) : "w"(x), "w"(y));
	else if (operation == 1)
		asm volatile("fsub %s0, %s1, %s2" : "=w"(z) : "w"(x), "w"(y));
	else
		asm volatile("fmul %s0, %s1, %s2" : "=w"(z) : "w"(x), "w"(y));
	asm volatile("mrs %0, fpsr" : "=r"(fpsr));
	asm volatile("msr fpcr, %0\n\tmsr fpsr, %1" :: "r"(saved_fpcr), "r"(saved_fpsr) : "memory");
	return {std::bit_cast<std::uint32_t>(z), static_cast<std::uint32_t>(fpsr & 0x9fU)};
}

/* VADD, VSUB and VMUL of singles, in Advanced SIMD and in VFP, give the same
 * result in either native environment while each nonzero operand's exponent
 * field is 64 to 191 and the guest's rounding mode is round to nearest, so
 * they run in the active one then and switch only for other operands. The
 * value and flags must be those of the architecture whichever way they ran:
 * Advanced SIMD's under the standard value, VFP's under the guest's FPCR. */
void add_sub_and_mul_run_in_the_active_environment_for_ordinary_operands()
{
	constexpr std::uint32_t vadd_f32_d6_d7_d7 = 0xf207'6d07U; /* zeros */
	constexpr std::uint32_t vadd_f32_s29_s30_s31 = 0xee7f'ea2fU; /* zeros */
	constexpr std::uint32_t vfp_ops[3] = {0xee31'3a02U, 0xee31'3a42U, 0xee21'3a02U};
	constexpr std::uint32_t neon_d_ops[3] = {0xf201'3d02U, 0xf221'3d02U, 0xf301'3d12U};
	constexpr std::uint32_t neon_q_ops[3] = {0xf202'6d44U, 0xf222'6d44U, 0xf302'6d54U};
	constexpr std::uint32_t standard = 0x0300'0000U; /* DN FZ, round to nearest */
	struct Operands {
		std::uint32_t a, b;
	};
	const Operands operands[] = {
		{0x3f80'0000U, 0x4000'0000U}, /* 1, 2 */
		{0x3dcc'cccdU, 0x3e4c'cccdU}, /* inexact */
		{0x0000'0000U, 0x0000'0000U},
		{0x8000'0000U, 0x0000'0000U},
		{0x0000'0000U, 0x4060'0000U},
		{0xc0c0'0000U, 0x3f80'0000U},
		{0x2000'0000U, 0x2000'0000U}, /* 2**-63, the lowest in range */
		{0x2000'0000U, 0xa000'0000U},
		{0x1f80'0000U, 0x3f80'0000U}, /* 2**-64 */
		{0x5f80'0000U, 0x3f80'0000U}, /* 2**64, the highest in range */
		{0x5f80'0000U, 0x5f80'0000U}, /* overflow of a product */
		{0x6000'0000U, 0x3f80'0000U}, /* 2**65 */
		{0x0000'0001U, 0x3f80'0000U}, /* denormal */
		{0x3f80'0000U, 0x807f'ffffU},
		{0x7f80'0000U, 0x3f80'0000U}, /* infinity */
		{0x7fc0'0000U, 0x3f80'0000U}, /* quiet NaN */
		{0x3f80'0000U, 0x7f80'0001U}, /* signaling NaN */
		{0x3f80'0000U, 0x3f80'0001U}, /* nearly equal */
	};
	const auto in_range = [](std::uint32_t value) {
		const std::uint32_t exponent = value >> 23 & 0xffU;
		return (value << 1) == 0 || (exponent >= 64 && exponent <= 191);
	};
	unsigned long generation = 2200;
	for (const auto fpcr : {0U, standard, 1U << 22, 3U << 22}) {
		for (const auto &item : operands) {
			for (unsigned operation = 0; operation < 3; ++operation) {
				for (unsigned kind = 0; kind < 3; ++kind) { /* VFP, D, Q */
					for (const unsigned first : {0U, 1U}) {
						Guest guest{generation++};
						/* 0: Advanced SIMD first, so the standard value
						 * is native; 1: VFP first, so the guest's is. */
						const std::uint32_t tested = kind == 0 ? vfp_ops[operation] :
							kind == 1 ? neon_d_ops[operation] : neon_q_ops[operation];
						guest.a32(0, {first == 0 ? vadd_f32_d6_d7_d7 :
							      vadd_f32_s29_s30_s31, tested, a::svc});
						auto cpu = a32_state();
						cpu.fpcr = fpcr;
						/* A quad register has lanes: the case is in the
						 * last one and the rest are ordinary. */
						const std::uint32_t other_a = 0x3f80'0000U;
						const std::uint32_t other_b = 0x4000'0000U;
						if (kind == 2) {
							set_d(cpu, 2, std::uint64_t{other_a} << 32 | other_a);
							set_d(cpu, 3, std::uint64_t{item.a} << 32 | other_a);
							set_d(cpu, 4, std::uint64_t{other_b} << 32 | other_b);
							set_d(cpu, 5, std::uint64_t{item.b} << 32 | other_b);
						} else {
							set_d(cpu, 1, std::uint64_t{item.a} << 32 | item.a);
							set_d(cpu, 2, std::uint64_t{item.b} << 32 | item.b);
						}
						require(guest.run(cpu).reason == ExitReason::system_call);
						const bool neon = kind != 0;
						const auto expected = host_float(operation, item.a, item.b,
							neon ? standard : fpcr);
						const auto ordinary = host_float(operation, other_a, other_b,
							neon ? standard : fpcr);
						if (kind == 0) {
							require(static_cast<std::uint32_t>(get_d(cpu, 3)) == expected.bits,
								"a VFP add, subtract or multiply gave the wrong value");
						} else if (kind == 1) {
							require(get_d(cpu, 3) ==
								(std::uint64_t{expected.bits} << 32 | expected.bits),
								"an Advanced SIMD add, subtract or multiply gave the wrong value");
						} else {
							require(get_d(cpu, 6) ==
								(std::uint64_t{ordinary.bits} << 32 | ordinary.bits) &&
								get_d(cpu, 7) ==
								(std::uint64_t{expected.bits} << 32 | ordinary.bits),
								"a quad add, subtract or multiply gave the wrong value");
						}
						require((cpu.fpsr & 0x9fU) == (expected.flags | ordinary.flags),
							"an add, subtract or multiply raised the wrong flags");
						/* The environment survives an instruction of the
						 * kind it is native for, and any other with
						 * operands the guard passes. */
						const bool own = neon ? first == 0 : first == 1;
						const bool passes = in_range(item.a) && in_range(item.b) &&
							(fpcr & (3U << 22)) == 0;
						const bool stays = own || passes;
						const unsigned active_before = first == 0 ? 1U : 0U;
						const unsigned after = stays ? active_before : neon ? 1U : 0U;
						require(cpu.a32_fp_standard == after,
							"an add, subtract or multiply switched the environment "
							"when it need not have, or did not when it had to");
					}
				}
			}
		}
	}
}

/* The library runs an instruction whose floating point it models in
 * software, so the cumulative flags the native instructions of the block
 * raised before it must reach CpuState first and the flags it raises must
 * survive the native instructions after it. */
void a_library_floating_point_instruction_keeps_the_flags_around_it()
{
	Guest guest{995};
	/* vdiv.f32 s8, s2, s4 (divide by zero) ; vrecps.f32 d5, d6, d7
	 * (overflow and inexact, in the library) ; vadd.f32 s9, s6, s6 (exact) */
	guest.a32(0, {0xee81'4a02U, 0xf206'5f17U, 0xee73'4a03U, a::svc});
	auto cpu = a32_state();
	set_d(cpu, 1, 0x3f80'0000ULL); /* s2 = 1.0, s3 = 0 */
	set_d(cpu, 2, 0);              /* s4 = 0.0 */
	set_d(cpu, 3, 0x3f80'0000ULL); /* s6 = 1.0 */
	set_d(cpu, 6, 0x7f7f'ffff'7f7f'ffffULL);
	set_d(cpu, 7, 0x7f7f'ffff'7f7f'ffffULL);
	const auto result = guest.run(cpu);
	require(result.reason == ExitReason::system_call);
	require(result.retired_instructions == 4);
	require((cpu.fpsr & 0x9fU) == (0x02U | 0x04U | 0x10U)); /* DZC OFC IXC */
	require(static_cast<std::uint32_t>(get_d(cpu, 4) >> 32) == 0x4000'0000U);
}

/* VLDn and VSTn move the elements natively; a hinted access that is
 * misaligned and one that crosses a page are the library's, and the register
 * writeback is Rn plus the size or plus Rm. */
void structure_loads_and_stores_move_the_elements_and_write_back()
{
	constexpr std::uint32_t vld1_32_d0_d1_r0_align128 = 0xf420'0aafU;
	constexpr std::uint32_t vld1_32_d0_d1_r0_post_r2 = 0xf420'0a82U;
	constexpr std::uint32_t vst1_32_d0_d1_r1_post = 0xf401'0a8dU;
	constexpr std::uint32_t vld1_32_d0_d1_r0 = 0xf420'0a8fU;
	const std::uint32_t source = 0x40;
	{
		/* vld1.32 {d0,d1},[r0],r2 ; vst1.32 {d0,d1},[r1]! */
		Guest guest{996};
		guest.a32(0, {vld1_32_d0_d1_r0_post_r2, vst1_32_d0_d1_r1_post, a::svc});
		for (std::uint32_t index = 0; index < 4; ++index)
			guest.data32(source + 4 * index, 0x1111'1111U * (index + 1));
		auto cpu = a32_state();
		cpu.registers[0] = data_base + source;
		cpu.registers[1] = data_base + 0x80;
		cpu.registers[2] = 0x24;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(get_d(cpu, 0) == 0x2222'2222'1111'1111ULL);
		require(get_d(cpu, 1) == 0x4444'4444'3333'3333ULL);
		require(reg(cpu, 0) == data_base + source + 0x24U);
		require(reg(cpu, 1) == data_base + 0x90U);
		for (std::uint32_t index = 0; index < 4; ++index)
			require(guest.data32(0x80 + 4 * index) ==
				0x1111'1111U * (index + 1));
	}
	{
		/* A 128-bit alignment hint on an address 8 bytes off faults. */
		Guest guest{997};
		guest.a32(0, {vld1_32_d0_d1_r0_align128, a::svc});
		auto cpu = a32_state();
		cpu.registers[0] = data_base + source + 8;
		set_d(cpu, 0, 0x5555ULL);
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::memory_fault);
		require(result.fault.alignment);
		require(result.fault.address == data_base + source + 8);
		require(get_d(cpu, 0) == 0x5555ULL);
	}
	{
		/* Without the hint the same address loads. */
		Guest guest{998};
		guest.a32(0, {vld1_32_d0_d1_r0, a::svc});
		guest.data32(source + 8, 0xaabb'ccddU);
		auto cpu = a32_state();
		cpu.registers[0] = data_base + source + 8;
		require(guest.run(cpu).reason == ExitReason::system_call);
		require(static_cast<std::uint32_t>(get_d(cpu, 0)) == 0xaabb'ccddU);
	}
	{
		/* A load that crosses the page loads whole and writes back; one that
		 * ends in an unmapped page faults with the registers unchanged. */
		Guest guest{999};
		guest.a32(0, {0xf420'0a8dU /* vld1.32 {d0,d1},[r0]! */, a::svc});
		const std::uint32_t boundary = page - 8;
		for (std::uint32_t index = 0; index < 4; ++index)
			guest.data32(boundary + 4 * index, 0x0101'0101U * (index + 1));
		auto cpu = a32_state();
		cpu.registers[0] = data_base + boundary;
		require(guest.run(cpu).reason == ExitReason::system_call);
		require(get_d(cpu, 0) == 0x0202'0202'0101'0101ULL);
		require(get_d(cpu, 1) == 0x0404'0404'0303'0303ULL);
		require(reg(cpu, 0) == data_base + boundary + 16);
	}
}

/* VLD1 and VST1 of one to four registers copy the same bytes at every
 * element size, so the 16- and 32-bit forms are gadgets like the 8- and
 * 64-bit ones: an access inside a page never reaches the library. */
void vld1_and_vst1_copies_stay_out_of_the_library()
{
	struct Copy {
		std::uint32_t word;
		unsigned registers;
		bool store;
	};
	const Copy copies[] = {
		{0xf420'074fU, 1, false}, /* vld1.16 {d0}, [r0] */
		{0xf420'0a4fU, 2, false}, /* vld1.16 {d0, d1}, [r0] */
		{0xf420'068dU, 3, false}, /* vld1.32 {d0-d2}, [r0]! */
		{0xf420'0282U, 4, false}, /* vld1.32 {d0-d3}, [r0], r2 */
		{0xf401'0a4fU, 2, true},  /* vst1.16 {d0, d1}, [r1] */
		{0xf401'068dU, 3, true},  /* vst1.32 {d0-d2}, [r1]! */
		{0xf401'028fU, 4, true},  /* vst1.32 {d0-d3}, [r1] */
		{0xf401'075fU, 1, true},  /* vst1.16 {d0}, [r1:64] */
	};
	aarch32::count_a32_library_exits(true);
	unsigned long generation = 1300;
	for (const auto &copy : copies) {
		const auto decoded = aarch32::decode_a32(
			copy.word, aarch32::Profile{});
		require(decoded.legal(), "a VLD1 or VST1 word did not decode");
		Guest guest{generation++};
		guest.a32(0, {copy.word, a::svc});
		for (std::uint32_t index = 0; index < 8; ++index)
			guest.data32(0x40 + 4 * index, 0x0101'0101U * (index + 1));
		auto cpu = a32_state();
		cpu.registers[0] = data_base + 0x40;
		cpu.registers[1] = data_base + 0x80;
		cpu.registers[2] = 0x20;
		for (unsigned n = 0; n < 4; ++n)
			set_d(cpu, n, 0x1111'1111'0000'0000ULL * (n + 1) + n + 1);
		aarch32::reset_a32_library_exits();
		require(guest.run(cpu).reason == ExitReason::system_call);
		require(aarch32::a32_library_exits(decoded.form) == 0,
			"a VLD1 or VST1 copy used the library");
		for (unsigned n = 0; n < copy.registers; ++n) {
			if (copy.store) {
				const std::uint64_t value =
					0x1111'1111'0000'0000ULL * (n + 1) + n + 1;
				require(guest.data32(0x80 + 8 * n) ==
					static_cast<std::uint32_t>(value));
				require(guest.data32(0x84 + 8 * n) ==
					static_cast<std::uint32_t>(value >> 32));
			} else {
				require(get_d(cpu, n) ==
					(std::uint64_t{0x0101'0101U * (2 * n + 2)} << 32 |
					 0x0101'0101U * (2 * n + 1)));
			}
		}
	}
	aarch32::count_a32_library_exits(false);
}

} // namespace

extern "C" {
void uml_arm64_gadget_a32_dp_add_imm();
void uml_arm64_gadget_a32_dp_sub_imm_r3();
void uml_arm64_gadget_a32_neon_vld1_m_16_a();
void uml_arm64_gadget_a32_neon_vld1_m_32_a();
void uml_arm64_gadget_a32_neon_vst1_m_16_a();
}

namespace {

/* Whether a translation of `words` at the code base holds `gadget`. */
[[nodiscard]] bool translation_holds(std::span<const std::uint32_t> words,
				     void (*gadget)())
{
	const gadgets::BuildOptions options;
	const auto output = gadgets::a32::build(
		{.key = aarch32::a32_key(code_base), .code = std::as_bytes(words)},
		{}, std::pmr::get_default_resource(), {}, options,
		gadgets::a32::decode_profile(options), 64);
	for (const auto word : output.result.program.words())
		if (word == reinterpret_cast<std::uintptr_t>(gadget))
			return true;
	return false;
}

/* An ADD or SUB of an immediate that computes the base of the next VLD1 or
 * VST1 copy without writeback, with only Advanced SIMD data processing
 * between, runs inside the copy's gadget: Rd is written before the transfer,
 * so a fault at the copy finds the state the two instructions leave one by
 * one (Rd written, the instructions between retired, the PC at the copy).
 * An instruction between that reads a core register keeps the ADD apart. */
void an_add_that_computes_a_copy_base_runs_in_the_copy()
{
	constexpr std::uint32_t add_r3_r0_16 = 0xe280'3010U;
	constexpr std::uint32_t sub_r3_r3_8 = 0xe243'3008U;
	constexpr std::uint32_t vmul_q13_q13_q9 = 0xf34a'adf2U;
	constexpr std::uint32_t vadd_q8_q8_q9 = 0xf240'0de2U;
	constexpr std::uint32_t vld1_64_d28_d29_r3 = 0xf463'cacfU;
	constexpr std::uint32_t vst1_64_d28_d29_r3 = 0xf443'cacfU;
	constexpr std::uint32_t vld1_32_d0_d3_r3 = 0xf423'028fU;
	constexpr std::uint32_t vdup_32_q0_r3 = 0xeea0'3b10U;
	const auto floats = [](arm64::CpuState &cpu) {
		set_d(cpu, 26, 0x4000'0000'4000'0000ULL); /* 2.0 */
		set_d(cpu, 27, 0x4000'0000'4000'0000ULL);
		set_d(cpu, 18, 0x4040'0000'4040'0000ULL); /* 3.0 */
		set_d(cpu, 19, 0x4040'0000'4040'0000ULL);
	};
	{
		const std::array<std::uint32_t, 4> words{
			add_r3_r0_16, vmul_q13_q13_q9, vld1_64_d28_d29_r3, a::svc};
		require(translation_holds(words,
			&uml_arm64_gadget_a32_neon_vld1_m_16_a));
		require(!translation_holds(words,
			&uml_arm64_gadget_a32_dp_add_imm));
		/* Inside a page, across the page line, and across into the
		 * unmapped page after the data. */
		unsigned long generation = 1080;
		for (const std::uint32_t base :
		     {0x30U, page - 0x18U,
		      static_cast<std::uint32_t>(data_pages * page - 0x18U)}) {
			Guest guest{generation++};
			for (std::uint32_t index = 0; index < words.size(); ++index)
				guest.a32(4 * index, {words[index]});
			for (std::uint32_t offset = 0; offset < 16; offset += 4)
				if (base + 0x10 + offset < data_pages * page)
					guest.data32(base + 0x10 + offset,
						     0x0101'0101U * (offset + 1));
			auto cpu = a32_state();
			cpu.registers[0] = data_base + base;
			floats(cpu);
			const auto result = guest.run(cpu);
			require(reg(cpu, 3) == data_base + base + 0x10);
			require(get_d(cpu, 26) == 0x40c0'0000'40c0'0000ULL);
			if (base + 0x20 > data_pages * page) {
				require(result.reason == ExitReason::memory_fault);
				require(result.retired_instructions == 2);
				require(cpu.pc == aarch32::a32_key(code_base + 8));
				require(result.fault.address ==
					data_base + data_pages * page);
				continue;
			}
			require(result.reason == ExitReason::system_call);
			require(result.retired_instructions == 4);
			require(get_d(cpu, 28) == 0x0505'0505'0101'0101ULL);
			require(get_d(cpu, 29) == 0x0d0d'0d0d'0909'0909ULL);
		}
	}
	{
		/* SUB in place, a 32-byte copy, two instructions between. */
		const std::array<std::uint32_t, 5> words{
			sub_r3_r3_8, vmul_q13_q13_q9, vadd_q8_q8_q9,
			vld1_32_d0_d3_r3, a::svc};
		require(translation_holds(words,
			&uml_arm64_gadget_a32_neon_vld1_m_32_a));
		require(!translation_holds(words,
			&uml_arm64_gadget_a32_dp_sub_imm_r3));
		Guest guest{1087};
		for (std::uint32_t index = 0; index < words.size(); ++index)
			guest.a32(4 * index, {words[index]});
		for (std::uint32_t offset = 0; offset < 32; offset += 4)
			guest.data32(0x38 + offset, 0x1000'0000U + offset);
		auto cpu = a32_state();
		cpu.registers[3] = data_base + 0x40;
		floats(cpu);
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(reg(cpu, 3) == data_base + 0x38);
		require(get_d(cpu, 0) == 0x1000'0004'1000'0000ULL);
		require(get_d(cpu, 3) == 0x1000'001c'1000'0018ULL);
	}
	{
		/* A store, faulting: Rd is written and the store did nothing. */
		const std::array<std::uint32_t, 3> words{
			add_r3_r0_16, vst1_64_d28_d29_r3, a::svc};
		require(translation_holds(words,
			&uml_arm64_gadget_a32_neon_vst1_m_16_a));
		Guest guest{1088};
		for (std::uint32_t index = 0; index < words.size(); ++index)
			guest.a32(4 * index, {words[index]});
		auto cpu = a32_state();
		cpu.registers[0] = unmapped - 0x10;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::memory_fault);
		require(result.retired_instructions == 1);
		require(cpu.pc == aarch32::a32_key(code_base + 4));
		require(reg(cpu, 3) == unmapped);
		require(result.fault.access == ::uml::memory::MemoryAccess::write);
	}
	{
		/* VDUP reads R3 between: the ADD stays apart and VDUP sees it. */
		const std::array<std::uint32_t, 4> words{
			add_r3_r0_16, vdup_32_q0_r3, vld1_64_d28_d29_r3, a::svc};
		require(translation_holds(words,
			&uml_arm64_gadget_a32_dp_add_imm));
		require(!translation_holds(words,
			&uml_arm64_gadget_a32_neon_vld1_m_16_a));
		Guest guest{1089};
		for (std::uint32_t index = 0; index < words.size(); ++index)
			guest.a32(4 * index, {words[index]});
		auto cpu = a32_state();
		cpu.registers[0] = data_base + 0x30;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(get_d(cpu, 0) == (std::uint64_t{data_base + 0x40} << 32 |
					  (data_base + 0x40)));
	}
}

} // namespace

extern "C" {
void uml_arm64_gadget_a32_neon_vld1_m_16_nh();
void uml_arm64_gadget_a32_neon_vst1_m_16_nh();
void uml_arm64_gadget_a32_neon_vld1r_4_p2_i();
void uml_arm64_gadget_a32_neon_vld1_m_16();
void uml_arm64_gadget_a32_neon_vld1_a_4_p2();
}

namespace {

/* A hinted VLD1 or VST1 copy and a VLD1 to all lanes take gadgets of their
 * own, which test the hint's alignment and leave a misaligned access to the
 * library (the corpus's simd-hints suite runs every hint, writeback and
 * misalignment); the general structure gadget is for the other shapes. */
void hinted_copies_and_loads_to_all_lanes_have_their_own_gadgets()
{
	struct Selection final {
		std::uint32_t word;
		void (*fast)();
		void (*general)();
	};
	for (const auto &item : {
		     /* vld1.64 {d22, d23}, [r0:128] */
		     Selection{0xf460'6aefU, &uml_arm64_gadget_a32_neon_vld1_m_16_nh,
			       &uml_arm64_gadget_a32_neon_vld1_m_16},
		     /* vst1.64 {d22, d23}, [r0:128] */
		     Selection{0xf440'6aefU, &uml_arm64_gadget_a32_neon_vst1_m_16_nh,
			       nullptr},
		     /* vld1.32 {d0[], d1[]}, [r9:32]! */
		     Selection{0xf4a9'0cbdU, &uml_arm64_gadget_a32_neon_vld1r_4_p2_i,
			       &uml_arm64_gadget_a32_neon_vld1_a_4_p2}}) {
		const std::array<std::uint32_t, 2> words{item.word, a::svc};
		require(translation_holds(words, item.fast));
		if (item.general != nullptr)
			require(!translation_holds(words, item.general));
	}
	/* The hinted copy at an aligned address and at one the hint refuses. */
	for (const std::uint32_t offset : {0x40U, 0x48U}) {
		const std::array<std::uint32_t, 2> words{0xf460'6aefU, a::svc};
		Guest guest{1098 + offset / 0x48U};
		guest.a32(0, {words[0]});
		guest.a32(4, {words[1]});
		guest.data32(offset, 0x1122'3344U);
		auto cpu = a32_state();
		cpu.registers[0] = data_base + offset;
		const auto result = guest.run(cpu);
		if (offset == 0x40U) {
			require(result.reason == ExitReason::system_call);
			require(get_d(cpu, 22) == 0x1122'3344U);
		} else {
			require(result.reason != ExitReason::system_call);
			require(result.retired_instructions == 0);
		}
	}
}

} // namespace

extern "C" {
void uml_arm64_gadget_a32_vfp_vcmpe_s_apsr();
void uml_arm64_gadget_a32_vfp_vmrs_apsr();
}

namespace {

/* A VCMP or VCMPE followed at once by VMRS APSR_nzcv is one gadget that
 * sets FPSCR.NZCV and copies it to the APSR, for each kind of outcome. */
void a_compare_and_the_flag_transfer_after_it_run_as_one()
{
	const std::array<std::uint32_t, 5> words{
		0xeeb4'3ac4U, /* vcmpe.f32 s6, s8 */
		0xeef1'fa10U, /* vmrs APSR_nzcv, fpscr */
		0xb300'1001U, /* movwlt r1, #1 */
		0xeef1'2a10U, /* vmrs r2, fpscr */
		a::svc};
	require(translation_holds(words, &uml_arm64_gadget_a32_vfp_vcmpe_s_apsr));
	require(!translation_holds(words, &uml_arm64_gadget_a32_vfp_vmrs_apsr));
	struct Case final {
		std::uint32_t s6, s8, r1, fpscr;
	};
	unsigned long generation = 1090;
	for (const auto &item : {Case{0x3f80'0000U, 0x4000'0000U, 1, 0x8000'0000U},
				 Case{0x4000'0000U, 0x3f80'0000U, 0, 0x2000'0000U},
				 Case{0x3f80'0000U, 0x3f80'0000U, 0, 0x6000'0000U},
				 Case{0x3f80'0000U, 0x7fc0'0000U, 1, 0x3000'0001U}}) {
		Guest guest{generation++};
		for (std::uint32_t index = 0; index < words.size(); ++index)
			guest.a32(4 * index, {words[index]});
		auto cpu = a32_state();
		set_d(cpu, 3, item.s6);
		set_d(cpu, 4, item.s8);
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(result.retired_instructions == words.size());
		require(reg(cpu, 1) == item.r1 && reg(cpu, 2) == item.fpscr);
	}
}

/* A conditional VMOV between S or D registers is a select, not a skip. */
void a_conditional_floating_point_move_selects_without_a_skip()
{
	const std::array<std::uint32_t, 5> words{
		0xeeb5'1b40U, /* vcmp.f64 d1, #0 */
		0xeef1'fa10U, /* vmrs APSR_nzcv, fpscr */
		0xceb0'0b42U, /* vmovgt.f64 d0, d2 */
		0xbef0'3a42U, /* vmovlt.f32 s7, s4 */
		a::svc};
	require(!translation_skips(words));
	unsigned long generation = 1095;
	for (const std::uint64_t d1 : {0x3ff0'0000'0000'0000ULL /* 1.0 */,
				       0xbff0'0000'0000'0000ULL /* -1.0 */,
				       0ULL}) {
		Guest guest{generation++};
		for (std::uint32_t index = 0; index < words.size(); ++index)
			guest.a32(4 * index, {words[index]});
		auto cpu = a32_state();
		set_d(cpu, 0, 0x1111'1111'1111'1111ULL);
		set_d(cpu, 1, d1);
		set_d(cpu, 2, 0x2222'2222'2222'2222ULL);
		set_d(cpu, 3, 0x3333'3333'3333'3333ULL);
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(result.retired_instructions == words.size());
		const bool greater = d1 == 0x3ff0'0000'0000'0000ULL;
		const bool less = d1 == 0xbff0'0000'0000'0000ULL;
		require(get_d(cpu, 0) == (greater ? 0x2222'2222'2222'2222ULL :
						    0x1111'1111'1111'1111ULL));
		/* S7 is the high half of D3, S4 the low half of D2. */
		require(get_d(cpu, 3) == (less ? 0x2222'2222'3333'3333ULL :
						 0x3333'3333'3333'3333ULL));
	}
}

/* An A32 block holds every instruction of its window up to block_limit,
 * whatever window it is given; a T32 block stops five short of the limit
 * unless an IT block is open. A straight run longer than a block links on and
 * retires every instruction. */
void an_a32_block_holds_all_sixty_four_instructions()
{
	constexpr std::uint32_t add_r0_1 = 0xe280'0001U;
	std::array<std::uint32_t, 80> words;
	words.fill(add_r0_1);
	std::array<std::uint16_t, 80> halfwords;
	halfwords.fill(0x3001U); /* adds r0, #1 */
	const gadgets::BuildOptions options;
	const auto count = [&](std::uint64_t key,
			       std::span<const std::byte> code,
			       std::size_t window) {
		return gadgets::a32::build({.key = key, .code = code}, {},
			std::pmr::get_default_resource(), {}, options,
			gadgets::a32::decode_profile(options), window)
			.result.program.instruction_count();
	};
	const auto a32_code = std::as_bytes(std::span{words});
	const auto t32_code = std::as_bytes(std::span{halfwords});
	require(gadgets::a32::block_limit == 64);
	require(count(aarch32::a32_key(code_base), a32_code.first(64 * 4), 64) ==
		64);
	require(count(aarch32::a32_key(code_base), a32_code, 80) == 64);
	require(count(aarch32::t32_key(code_base), t32_code.first(64 * 2), 64) ==
		59);
	require(count(aarch32::t32_key(code_base), t32_code, 80) == 59);

	Guest guest{1110};
	for (std::uint32_t index = 0; index < 70; ++index)
		guest.a32(4 * index, {add_r0_1});
	guest.a32(4 * 70, {a::svc});
	auto cpu = a32_state();
	const auto result = guest.run(cpu);
	require(result.reason == ExitReason::system_call);
	require(result.retired_instructions == 71);
	require(reg(cpu, 0) == 70);

	/* A fault at the last position of a full block reports that
	 * instruction. */
	Guest faulting{1111};
	for (std::uint32_t index = 0; index < 63; ++index)
		faulting.a32(4 * index, {add_r0_1});
	faulting.a32(4 * 63, {0xe592'1000U}); /* ldr r1, [r2] */
	faulting.a32(4 * 64, {a::svc});
	auto at_end = a32_state();
	at_end.registers[2] = unmapped;
	const auto fault = faulting.run(at_end);
	require(fault.reason == ExitReason::memory_fault);
	require(fault.retired_instructions == 63);
	require(at_end.pc == aarch32::a32_key(code_base + 4 * 63));
	require(fault.fault.address == unmapped);
	require(reg(at_end, 0) == 63);
}

/* A VLD1 or VST1 copy that crosses a page is a gadget too: the slow transfer
 * helper moves the bytes after translating both pages. */
void vld1_and_vst1_copies_across_a_page_stay_out_of_the_library()
{
	struct Copy {
		std::uint32_t word;
		unsigned registers;
		bool store;
	};
	const Copy copies[] = {
		{0xf420'0a8fU, 2, false}, /* vld1.32 {d0, d1}, [r0] */
		{0xf420'068fU, 3, false}, /* vld1.32 {d0-d2}, [r0] */
		{0xf420'028fU, 4, false}, /* vld1.32 {d0-d3}, [r0] */
		{0xf420'074fU, 1, false}, /* vld1.16 {d0}, [r0] */
		{0xf401'0a8fU, 2, true},  /* vst1.32 {d0, d1}, [r1] */
		{0xf401'068fU, 3, true},  /* vst1.32 {d0-d2}, [r1] */
		{0xf401'028fU, 4, true},  /* vst1.32 {d0-d3}, [r1] */
		{0xf401'074fU, 1, true},  /* vst1.16 {d0}, [r1] */
	};
	aarch32::count_a32_library_exits(true);
	unsigned long generation = 1500;
	for (const auto &copy : copies) {
		const auto decoded = aarch32::decode_a32(
			copy.word, aarch32::Profile{});
		require(decoded.legal(), "a VLD1 or VST1 word did not decode");
		Guest guest{generation++};
		guest.a32(0, {copy.word, a::svc});
		/* Four bytes of every register in the first page, four in the
		 * second, wherever the transfer is long enough. */
		const std::uint32_t start = page - 4 * copy.registers;
		for (std::uint32_t index = 0; index < 8; ++index)
			guest.data32(start + 4 * index, 0x0101'0101U * (index + 1));
		auto cpu = a32_state();
		cpu.registers[0] = data_base + start;
		cpu.registers[1] = data_base + start;
		for (unsigned n = 0; n < 4; ++n)
			set_d(cpu, n, 0x1111'1111'0000'0000ULL * (n + 1) + n + 1);
		aarch32::reset_a32_library_exits();
		require(guest.run(cpu).reason == ExitReason::system_call);
		require(aarch32::a32_library_exits(decoded.form) == 0,
			"a VLD1 or VST1 copy across a page used the library");
		for (unsigned n = 0; n < copy.registers; ++n) {
			if (copy.store) {
				const std::uint64_t value =
					0x1111'1111'0000'0000ULL * (n + 1) + n + 1;
				require(guest.data32(start + 8 * n) ==
					static_cast<std::uint32_t>(value));
				require(guest.data32(start + 8 * n + 4) ==
					static_cast<std::uint32_t>(value >> 32));
			} else {
				require(get_d(cpu, n) ==
					(std::uint64_t{0x0101'0101U * (2 * n + 2)} << 32 |
					 0x0101'0101U * (2 * n + 1)));
			}
		}
	}
	aarch32::count_a32_library_exits(false);
}

/* A VLD1 or VST1 copy without an alignment hint has a gadget for each
 * writeback (none, the size, Rm) and register count; each must move the
 * bytes, inside a page and across one, write back what it should and leave
 * the other core registers alone. */
void vld1_and_vst1_copies_write_back_in_each_mode()
{
	/* type field of VLD1 and VST1 for one to four registers */
	const std::uint32_t types[] = {0x7U, 0xaU, 0x6U, 0x2U};
	unsigned long generation = 2400;
	for (const bool store : {false, true}) {
		for (unsigned registers = 1; registers <= 4; ++registers) {
			for (unsigned mode = 0; mode < 3; ++mode) { /* none, size, Rm = r2 */
				for (const bool crossing : {false, true}) {
					const std::uint32_t word = (store ? 0xf400'0000U : 0xf420'0000U) |
						(store ? 1U : 0U) << 16 | types[registers - 1] << 8 |
						0x80U | (mode == 0 ? 0xfU : mode == 1 ? 0xdU : 0x2U);
					Guest guest{generation++};
					guest.a32(0, {word, a::svc});
					const std::uint32_t size = 8 * registers;
					const std::uint32_t start = crossing ? page - size / 2 : 0x40;
					for (std::uint32_t index = 0; index < 8; ++index)
						guest.data32(start + 4 * index, 0x0101'0101U * (index + 1));
					auto cpu = a32_state();
					cpu.registers[0] = data_base + start;
					cpu.registers[1] = data_base + start;
					cpu.registers[2] = 0x24;
					cpu.registers[3] = 0xfeed'f00dU;
					for (unsigned n = 0; n < 4; ++n)
						set_d(cpu, n, 0x1111'1111'0000'0000ULL * (n + 1) + n + 1);
					require(guest.run(cpu).reason == ExitReason::system_call);
					for (unsigned n = 0; n < registers; ++n) {
						if (store) {
							const std::uint64_t value =
								0x1111'1111'0000'0000ULL * (n + 1) + n + 1;
							require(guest.data32(start + 8 * n) ==
								static_cast<std::uint32_t>(value));
							require(guest.data32(start + 8 * n + 4) ==
								static_cast<std::uint32_t>(value >> 32));
						} else {
							require(get_d(cpu, n) ==
								(std::uint64_t{0x0101'0101U * (2 * n + 2)} << 32 |
								 0x0101'0101U * (2 * n + 1)));
						}
					}
					const unsigned base = store ? 1U : 0U;
					const std::uint32_t expected = data_base + start +
						(mode == 0 ? 0U : mode == 1 ? size : 0x24U);
					require(reg(cpu, base) == expected, "a copy wrote back the wrong base");
					require(reg(cpu, 1 - base) == data_base + start,
						"a copy changed the other base");
					require(reg(cpu, 2) == 0x24U && reg(cpu, 3) == 0xfeed'f00dU,
						"a copy changed an unrelated register");
				}
			}
		}
	}
}

/* ADD and SUB of an immediate to a register in place have an entry per
 * register with its displacement baked in; each must reach its own register
 * and flags. */
void in_place_add_and_sub_of_an_immediate_reach_every_register()
{
	unsigned long generation = 1600;
	for (std::uint32_t reg = 0; reg < 15; ++reg) {
		Guest guest{generation++};
		const std::uint32_t both = reg << 16 | reg << 12;
		guest.a32(0, {0xe280'0010U | both,   /* add rN, rN, #0x10 */
			      0xe240'0008U | both,   /* sub rN, rN, #8 */
			      0xe250'0004U | both,   /* subs rN, rN, #4 */
			      a::svc});
		auto cpu = a32_state();
		for (unsigned other = 0; other < 15; ++other)
			cpu.registers[other] = 0x1000U * (other + 1);
		cpu.registers[reg] = 0xffff'fffcU; /* ends at zero */
		require(guest.run(cpu).reason == ExitReason::system_call);
		for (unsigned other = 0; other < 15; ++other)
			require(static_cast<std::uint32_t>(cpu.registers[other]) ==
				(other == reg ? 0U : 0x1000U * (other + 1)),
				"an in-place immediate reached another register");
		/* subs to zero: Z and C set, N and V clear. */
		require((cpu.nzcv >> 28) == 0x6U, "SUBS to zero set wrong flags");
	}
}

/* A CMP followed at once by a conditional branch is one fused gadget of the
 * AArch64 bank. It must leave the flags and take the branch as the pair
 * does, for every condition and for the register and immediate forms. */
bool condition_holds(unsigned cond, std::uint32_t flags)
{
	const bool n = (flags >> 3 & 1U) != 0, z = (flags >> 2 & 1U) != 0,
		   c = (flags >> 1 & 1U) != 0, v = (flags & 1U) != 0;
	switch (cond) {
	case 0: return z;
	case 1: return !z;
	case 2: return c;
	case 3: return !c;
	case 4: return n;
	case 5: return !n;
	case 6: return v;
	case 7: return !v;
	case 8: return c && !z;
	case 9: return !c || z;
	case 10: return n == v;
	case 11: return n != v;
	case 12: return !z && n == v;
	default: return z || n != v;
	}
}

std::uint32_t compare_flags(std::uint32_t a, std::uint32_t b)
{
	const std::uint32_t result = a - b;
	return (result >> 31) << 3 | (result == 0 ? 4U : 0U) |
	       (a >= b ? 2U : 0U) | (((a ^ b) & (a ^ result)) >> 31);
}

void compare_and_branch_pairs_take_the_branch_as_the_pair_does()
{
	const std::uint32_t values[] = {0U, 1U, 5U, 0xffU, 0x7fff'ffffU,
					0x8000'0000U, 0xffff'ffffU};
	const std::uint32_t immediates[] = {0U, 1U, 5U, 0xffU};
	unsigned long generation = 1700;
	for (std::uint32_t cond = 0; cond < 14; ++cond) {
		/* The immediate form once per immediate, then the register form
		 * with the second operand in r1. */
		for (std::size_t form = 0; form <= std::size(immediates); ++form) {
			const bool immediate = form < std::size(immediates);
			Guest guest{generation++};
			/* cmp r0, #imm or r1; b<cond> +8; mov r5, #1; svc;
			 * mov r5, #2; svc. */
			guest.a32(0, {immediate ? 0xe350'0000U | immediates[form]
						: 0xe150'0001U,
				      cond << 28 | 0x0a00'0001U, 0xe3a0'5001U,
				      a::svc, 0xe3a0'5002U, a::svc});
			for (const auto a_value : values) {
				const auto count = immediate ? 1U : std::size(values);
				for (std::size_t index = 0; index < count; ++index) {
					const auto b_value = immediate ?
						immediates[form] : values[index];
					auto cpu = a32_state();
					cpu.registers[0] = a_value;
					cpu.registers[1] = b_value;
					cpu.registers[5] = 0;
					require(guest.run(cpu).reason ==
						ExitReason::system_call);
					const auto flags = compare_flags(a_value, b_value);
					require(cpu.registers[5] ==
						(condition_holds(cond, flags) ? 2U : 1U),
						"a fused compare and branch took the wrong way");
					require((cpu.nzcv >> 28) == flags,
						"a fused compare and branch left the wrong flags");
				}
			}
		}
	}
}

/* The parallel add and subtract family and SEL are gadgets: Advanced SIMD
 * does the lanes, GE is the byte-lane mask CpuState keeps, and the library
 * runs none of them. */
void parallel_arithmetic_and_sel_stay_out_of_the_library()
{
	struct Step {
		std::uint32_t word;
		unsigned destination;
		std::uint32_t expected;
	};
	/* r0 = 0x80ff7f01, r1 = 0x01010180. UADD8 carries out of byte 2 only,
	 * so GE is 0x00ff0000 for the SEL after it; SASX adds the high half of
	 * Rn to the low half of Rm and subtracts, so its GE is the low pair. */
	const Step steps[] = {
		{0xe650'2f91U, 2, 0x8100'8081U}, /* uadd8 r2, r0, r1 */
		{0xe680'3fb1U, 3, 0x01ff'0180U}, /* sel r3, r0, r1 */
		{0xe660'4ff1U, 4, 0x7ffe'7e00U}, /* uqsub8 r4, r0, r1 */
		{0xe620'5f91U, 5, 0x8100'7f81U}, /* qadd8 r5, r0, r1 */
		{0xe610'6f31U, 6, 0x827f'7e00U}, /* sasx r6, r0, r1 */
		{0xe680'7fb1U, 7, 0x0101'7f01U}, /* sel r7, r0, r1 */
	};
	aarch32::count_a32_library_exits(true);
	Guest guest{1400};
	std::uint32_t offset = 0;
	for (const auto &step : steps) {
		guest.a32(offset, {step.word});
		offset += 4;
	}
	guest.a32(offset, {a::svc});
	auto cpu = a32_state();
	cpu.registers[0] = 0x80ff'7f01U;
	cpu.registers[1] = 0x0101'0180U;
	aarch32::reset_a32_library_exits();
	require(guest.run(cpu).reason == ExitReason::system_call);
	for (const auto &step : steps) {
		const auto decoded = aarch32::decode_a32(
			step.word, aarch32::Profile{});
		require(decoded.legal(), "a parallel word did not decode");
		require(aarch32::a32_library_exits(decoded.form) == 0,
			"a parallel add or subtract or SEL used the library");
		require(static_cast<std::uint32_t>(cpu.registers[step.destination]) ==
			step.expected, "a parallel result was wrong");
	}
	aarch32::count_a32_library_exits(false);
}

/* The runtime loads a task's vectors lazily (CpuState::defer_fp_load) and a
 * program loads them, once, only when it is marked as touching them. A
 * block whose only vector instructions are structure loads or stores, or
 * ones the semantic library runs, is such a program: unmarked, it writes
 * the vectors ahead of a load that overwrites them, and the task's saved
 * state never sees its writes. */
using Vectors = decltype(arm64::CpuState::vectors);

void put_image_d(Vectors &image, unsigned n, std::uint64_t value)
{
	std::memcpy(image[n / 2].bytes.data() + 8U * (n % 2U), &value,
		    sizeof(value));
}

/* What the task's context switch does around a stop: the vectors go back to
 * the saved image if the task wrote them, and the next entry defers them
 * again. */
void save_and_defer_again(arm64::CpuState &cpu, Vectors &image)
{
	if (cpu.fp_dirty)
		image = cpu.vectors;
	cpu.defer_fp_load(image.data(), 0, 0, arm64::fpcr_writable_mask);
}

void structure_and_library_blocks_load_the_deferred_vectors()
{
	constexpr std::uint32_t vld1_64_d16_d17_r0 = 0xf460'0acfU;
	constexpr std::uint32_t vst1_64_d16_d17_r1 = 0xf441'0acfU;
	constexpr std::uint32_t vadd_i32_q8_q8_q8 = 0xf260'08e0U;
	constexpr std::uint32_t vrecps_f32_d5_d6_d7 = 0xf206'5f17U;
	constexpr std::uint32_t b_next = 0xeaff'ffffU;
	constexpr std::uint32_t source = 0x40;
	constexpr std::uint32_t target = 0x80;
	constexpr std::uint64_t kept = 0x0123'4567'89ab'cdefULL;
	constexpr std::uint64_t stale = 0xfedc'ba98'7654'3210ULL;
	constexpr std::uint64_t first = 0x2222'2222'1111'1111ULL;
	constexpr std::uint64_t second = 0x4444'4444'3333'3333ULL;
	const auto fill_source = [](Guest &guest) {
		for (std::uint32_t index = 0; index < 4; ++index)
			guest.data32(source + 4 * index, 0x1111'1111U * (index + 1));
	};
	Vectors image{};
	put_image_d(image, 4, kept);
	put_image_d(image, 16, stale);
	put_image_d(image, 17, stale);
	put_image_d(image, 6, 0x3f80'0000'3f80'0000ULL);
	put_image_d(image, 7, 0x3f80'0000'3f80'0000ULL);

	{
		/* A block of one structure load loads the vectors and marks them
		 * written. */
		Guest guest{1010};
		guest.a32(0, {vld1_64_d16_d17_r0, a::svc});
		fill_source(guest);
		auto cpu = a32_state();
		cpu.registers[0] = data_base + source;
		cpu.defer_fp_load(image.data(), 0, 0, arm64::fpcr_writable_mask);
		require(guest.run(cpu).reason == ExitReason::system_call);
		require(cpu.fp_loaded && cpu.fp_dirty);
		require(get_d(cpu, 16) == first && get_d(cpu, 17) == second);
		require(get_d(cpu, 4) == kept, "the other vectors did not load");
	}
	{
		/* A structure load in a block of its own, then a block that loads
		 * the vectors for another reason: the load's result survives. */
		Guest guest{1011};
		guest.a32(0, {vld1_64_d16_d17_r0, b_next, vadd_i32_q8_q8_q8,
			      vst1_64_d16_d17_r1, a::svc});
		fill_source(guest);
		auto cpu = a32_state();
		cpu.registers[0] = data_base + source;
		cpu.registers[1] = data_base + target;
		cpu.defer_fp_load(image.data(), 0, 0, arm64::fpcr_writable_mask);
		require(guest.run(cpu).reason == ExitReason::system_call);
		require(get_d(cpu, 16) == 2U * first && get_d(cpu, 17) == 2U * second);
		for (std::uint32_t index = 0; index < 4; ++index)
			require(guest.data32(target + 4 * index) ==
				0x2222'2222U * (index + 1));
	}
	{
		/* A store that faults after the load, a stop that saves and defers
		 * the vectors again, and the resumed store: the load survived the
		 * stop. */
		Guest guest{1012};
		guest.a32(0, {vld1_64_d16_d17_r0, vst1_64_d16_d17_r1, a::svc});
		fill_source(guest);
		auto cpu = a32_state();
		cpu.registers[0] = data_base + source;
		cpu.registers[1] = unmapped;
		auto saved = image;
		cpu.defer_fp_load(saved.data(), 0, 0, arm64::fpcr_writable_mask);
		const auto stopped = guest.run(cpu);
		require(stopped.reason == ExitReason::memory_fault);
		require(stopped.fault.address == unmapped);
		require(cpu.fp_loaded && cpu.fp_dirty);
		save_and_defer_again(cpu, saved);
		cpu.registers[1] = data_base + target;
		require(guest.run(cpu).reason == ExitReason::system_call);
		for (std::uint32_t index = 0; index < 4; ++index)
			require(guest.data32(target + 4 * index) ==
				0x1111'1111U * (index + 1));
		require(get_d(cpu, 4) == kept);
	}
	{
		/* A library instruction alone reads the loaded vectors, not zeros,
		 * and its result is marked written. */
		Guest guest{1013};
		guest.a32(0, {vrecps_f32_d5_d6_d7, a::svc});
		auto cpu = a32_state();
		cpu.defer_fp_load(image.data(), 0, 0, arm64::fpcr_writable_mask);
		require(guest.run(cpu).reason == ExitReason::system_call);
		require(cpu.fp_loaded && cpu.fp_dirty);
		require(get_d(cpu, 5) == 0x3f80'0000'3f80'0000ULL, "2 - 1 * 1");
	}
}

/* Single-stepped floating point (the path of a mid-IT entry and of ptrace)
 * loads the deferred vectors and keeps the instruction kind's environment:
 * Advanced SIMD flushes denormals, VFP follows the guest's FPSCR. */
void single_stepped_floating_point_loads_the_deferred_vectors()
{
	constexpr std::uint64_t denormals = 0x0000'0001'0000'0001ULL;
	Vectors image{};
	put_image_d(image, 0, 0x5555'5555'5555'5555ULL);
	put_image_d(image, 1, denormals);
	put_image_d(image, 2, denormals);
	Guest guest{1014};
	/* cmp r0,#0 ; itt ne ; vaddne.f32 d0,d1,d2 ; vaddne.f32 s8,s2,s4 */
	guest.t32(0, {t::cmp_r0_0, 0xbf1c, 0xef01, 0x0d02, 0xee31, 0x4a02, t::svc});
	auto cpu = t32_state();
	cpu.registers[0] = 1;
	cpu.defer_fp_load(image.data(), 0, 0, arm64::fpcr_writable_mask);
	for (int step = 0; step < 4; ++step) {
		const auto result = guest.run(cpu, 100, true);
		require(result.reason == ExitReason::single_step);
		require(result.retired_instructions == 1);
	}
	require(cpu.a32_itstate == 0);
	require(cpu.fp_loaded && cpu.fp_dirty);
	require(get_d(cpu, 0) == 0, "Advanced SIMD did not flush the denormal sum");
	require(static_cast<std::uint32_t>(get_d(cpu, 4)) == 2U,
		"VFP did not keep the denormal");
}

/* ------------------------------------------------- the optional extensions */

/* The engine shows a guest the extensions the host has the A64
 * feature for (a32_features), and a form of any other extension is
 * UNDEFINED, as the HWCAPs say. A word is run in a guest translating on the
 * machine's own capabilities and in one on a host with none of the optional
 * ones: where the extension is there the word executes (as a gadget, or in
 * the semantic library for BF16, I8MM and JSCVT, which no gadget lowers) and
 * both agree, and where it is not the word is the illegal-instruction exit,
 * not retired. The library counts its exits per form. */
struct FeatureWord {
	const char *what;
	std::uint32_t word;
	std::optional<aarch32::Feature> feature; /* none: needs no extension */
	bool library;                            /* no gadget: the library runs it */
};

[[nodiscard]] ::uml::cpu::HostCapabilities bare_host()
{
	auto host = ::uml::cpu::host_capabilities();
	host.supported.reset();
	host.crc32 = host.aes = host.pmull = host.sha1 = host.sha2 = false;
	host.sha3 = host.sha512 = host.fp16 = false;
	return host;
}

void host_features_decide_what_the_guest_may_execute()
{
	using F = aarch32::Feature;
	const FeatureWord words[] = {
		{"crc32w r0, r1, r2", 0xe141'0042U, F::crc32, false},
		{"crc32cb r0, r1, r2", 0xe101'0242U, F::crc32, false},
		{"vsdot.s8 d0, d1, d2", 0xfc21'0d02U, F::dotprod, false},
		{"vudot.u8 q0, q1, q2", 0xfc22'0d54U, F::dotprod, false},
		{"vsdot.s8 d0, d1, d2[1]", 0xfe21'0d22U, F::dotprod, false},
		{"vqrdmlah.s16 d0, d1, d2", 0xf311'0b12U, F::rdm, false},
		{"vqrdmlsh.s32 q0, q1, q2", 0xf322'0c54U, F::rdm, false},
		{"vqrdmlah.s16 d0, d1, d2[1]", 0xf291'0e4aU, F::rdm, false},
		{"vcadd.f32 d0, d1, d2, #90", 0xfc91'0802U, F::fcma, false},
		{"vcmla.f32 q0, q1, q2, #180", 0xfd32'0844U, F::fcma, false},
		{"vcmla.f32 d0, d1, d2[0], #90", 0xfe91'0802U, F::fcma, false},
		{"vfmal.f16 d0, s2, s4", 0xfc21'0812U, F::fhm, false},
		{"vfmsl.f16 q0, d2, d4", 0xfca2'0854U, F::fhm, false},
		{"vadd.f16 s0, s1, s2", 0xee30'0981U, F::fp16, false},
		{"vcvt.s32.f16 s0, s1", 0xeebd'09e0U, F::fp16, false},
		{"vmov.f16 s0, #1.0", 0xeeb7'0900U, F::fp16, false},
		{"vldr.16 s0, [r2]", 0xed92'0900U, F::fp16, false},
		{"aese.8 q0, q1", 0xf3b0'0302U, F::aes, false},
		{"sha1c.32 q0, q1, q2", 0xf202'0c44U, F::sha1, false},
		{"vmull.p64 q0, d1, d2", 0xf2a1'0e02U, F::pmull, false},
		{"vdot.bf16 d0, d1, d2", 0xfc01'0d02U, F::bf16, true},
		{"vusdot.s8 d0, d1, d2", 0xfca1'0d02U, F::i8mm, true},
		{"vjcvt.s32.f64 s0, d1", 0xeeb9'0bc1U, F::jscvt, true},
		/* The Armv8 floating-point additions every host has. */
		{"vmaxnm.f32 d0, d1, d2", 0xf301'0f12U, std::nullopt, false},
		{"vrinta.f32 d0, d1", 0xf3ba'0501U, std::nullopt, false},
	};
	const auto machine = ::uml::cpu::host_capabilities();
	const auto bare = bare_host();
	const auto machine_features =
		arm64::a32_features(machine);
	aarch32::count_a32_library_exits(true);
	unsigned long generation = 1100;
	struct Outcome {
		arm64::CpuState cpu;
		ExitReason reason;
		std::size_t retired;
		std::uint64_t exits;
	};
	for (const auto &item : words) {
		const auto decoded = aarch32::decode_a32(
			item.word, aarch32::Profile{});
		require(decoded.legal(), item.what);
		const auto run = [&](const ::uml::cpu::HostCapabilities &host) {
			Guest guest{generation++, host};
			guest.a32(0, {item.word, a::svc});
			guest.data32(0, 0x3c00'4000U);
			auto cpu = a32_state();
			cpu.registers[1] = 0x1234'5678U;
			cpu.registers[2] = data_base;
			for (unsigned n = 0; n < 8; ++n)
				set_d(cpu, n, 0x3c00'3800'4000'3c00ULL +
					      0x0101'0101'0101'0101ULL * n);
			aarch32::reset_a32_library_exits();
			const auto result = guest.run(cpu);
			return Outcome{cpu, result.reason, result.retired_instructions,
				       aarch32::a32_library_exits(decoded.form)};
		};
		const auto expect = [&](const Outcome &outcome, bool present,
					const char *where) {
			const std::string text = std::string{item.what} + " on " + where;
			if (!present) {
				require(outcome.reason == ExitReason::illegal_instruction,
					text + ": a word of an absent extension executed");
				require(outcome.retired == 0, text + ": it retired");
				return;
			}
			require(outcome.reason == ExitReason::system_call, text);
			require(item.library ? outcome.exits != 0 : outcome.exits == 0,
				text + (item.library ? ": no gadget, but it did not use the library"
						     : ": a gadget form used the library"));
		};
		const auto native = run(machine);
		const auto library = run(bare);
		const bool on_machine = !item.feature ||
					machine_features.has(*item.feature);
		expect(native, on_machine, "the machine");
		expect(library, !item.feature, "a host with no extension");
		if (on_machine && !item.feature) {
			for (unsigned n = 0; n < 16; ++n)
				require(reg(native.cpu, n) == reg(library.cpu, n), item.what);
			for (unsigned n = 0; n < 8; ++n)
				require(get_d(native.cpu, n) == get_d(library.cpu, n), item.what);
		}
	}
	aarch32::count_a32_library_exits(false);
}

/* ------------------------------------------------------------ pages */

void block_transfers_cross_a_page_boundary()
{
	Guest guest{976};
	/* ldm r0,{r1-r4} ; add r1,r1,#256 ; stm r0!,{r1-r4} : the load and
	 * the store both straddle the boundary between the two data pages. */
	guest.a32(0, {a::ldm_r0_r1_r4, 0xe281'1c01U, a::stm_r0_wb_r1_r4, a::svc});
	const std::uint32_t boundary = page - 8;
	for (std::uint32_t index = 0; index < 4; ++index)
		guest.data32(boundary + 4 * index, 0x1000'0000U + index);
	auto cpu = a32_state();
	cpu.registers[0] = data_base + boundary;
	const auto result = guest.run(cpu);
	require(result.reason == ExitReason::system_call);
	require(result.retired_instructions == 4);
	require(reg(cpu, 1) == 0x1000'0100U && reg(cpu, 4) == 0x1000'0003U);
	require(reg(cpu, 0) == data_base + boundary + 16);
	require(guest.data32(boundary) == 0x1000'0100U);
	for (std::uint32_t index = 1; index < 4; ++index)
		require(guest.data32(boundary + 4 * index) ==
			0x1000'0000U + index);
}

/* The semantic library runs a transfer that crosses a page whole, so the PC
 * it loads reaches the branch as the scratch value a lowered load leaves. */
void a_pop_to_pc_across_a_page_boundary_branches()
{
	{
		/* pop {r1, pc} to an A32 target, then to a T32 one. */
		Guest guest{980};
		guest.a32(0, {0xe8bd'8002U, 0xe3a0'2001U, a::svc, 0xe3a0'3001U,
			      0x0000'df00U});
		const std::uint32_t boundary = page - 4;
		guest.data32(boundary, 0x1234'5678U);
		guest.data32(page, code_base + 8U);
		auto cpu = a32_state();
		cpu.registers[13] = data_base + boundary;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		/* The pop, then the svc at the target: the mov in between was
		 * skipped. */
		require(result.retired_instructions == 2);
		require(reg(cpu, 1) == 0x1234'5678U && reg(cpu, 2) == 0);
		require(reg(cpu, 13) == data_base + boundary + 8);
		require(cpu.pc == aarch32::a32_key(code_base + 12U));
	}
	{
		Guest guest{981};
		guest.a32(0, {0xe8bd'8002U, 0xe3a0'2001U, a::svc, 0xe3a0'3001U,
			      0x0000'df00U});
		const std::uint32_t boundary = page - 4;
		guest.data32(boundary, 0x1234'5678U);
		guest.data32(page, (code_base + 16U) | 1U);
		auto cpu = a32_state();
		cpu.registers[13] = data_base + boundary;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(result.retired_instructions == 2);
		require(reg(cpu, 3) == 0);
		require(cpu.pc == aarch32::t32_key(code_base + 18U));
	}
}

/* The library decodes a T32 word in the IT state the stream carries: an
 * instruction of an IT block that crosses a page runs when its condition
 * passes and is skipped when it does not. */
void a_conditional_block_transfer_across_a_page_follows_its_condition()
{
	const std::uint32_t boundary = page - 8;
	for (const bool passes : {true, false}) {
		Guest guest{passes ? 982U : 983U};
		/* it eq ; ldmiaeq.w r0, {r1-r4} ; svc */
		guest.t32(0, {0xbf08, 0xe890, 0x001e, 0xdf00});
		for (std::uint32_t index = 0; index < 4; ++index)
			guest.data32(boundary + 4 * index, 0x2000'0000U + index);
		auto cpu = t32_state();
		cpu.registers[0] = data_base + boundary;
		cpu.nzcv = passes ? 0x4000'0000U : 0U;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::system_call);
		require(result.retired_instructions == 3);
		require(reg(cpu, 1) == (passes ? 0x2000'0000U : 0U));
		require(reg(cpu, 4) == (passes ? 0x2000'0003U : 0U));
	}
}

void a_double_word_load_crosses_a_page_boundary()
{
	Guest guest{977};
	guest.a32(0, {a::ldrd_r2_r0, a::svc});
	guest.data32(page - 4, 0xaaaa'0001U);
	guest.data32(page, 0xbbbb'0002U);
	auto cpu = a32_state();
	cpu.registers[0] = data_base + page - 4;
	require(guest.run(cpu).reason == ExitReason::system_call);
	require(reg(cpu, 2) == 0xaaaa'0001U && reg(cpu, 3) == 0xbbbb'0002U);
}

void a_block_transfer_into_an_unmapped_page_faults_at_that_page()
{
	/* The data ends where the second data page ends; the next page is
	 * not mapped. */
	const std::uint32_t last = data_pages * page - 8;
	{
		Guest guest{978};
		guest.a32(0, {a::stm_r0_wb_r1_r4, a::svc});
		auto cpu = a32_state();
		cpu.registers[0] = data_base + last;
		cpu.registers[1] = 0x11;
		cpu.registers[2] = 0x22;
		cpu.registers[3] = 0x33;
		cpu.registers[4] = 0x44;
		guest.data32(last, 0xdead'0000U);
		guest.data32(last + 4, 0xdead'0004U);
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::memory_fault);
		require(result.retired_instructions == 0);
		require(cpu.pc == aarch32::a32_key(code_base));
		require(result.fault.access == ::uml::memory::MemoryAccess::write);
		/* The fault is at the first word in the page that is not
		 * mapped. */
		require(result.fault.address == data_base + data_pages * page);
		/* The base is not written back, so the instruction can be
		 * restarted; the words ahead of the fault are in memory, as
		 * a sequence of word stores leaves them. */
		require(reg(cpu, 0) == data_base + last);
		require(guest.data32(last) == 0x11);
		require(guest.data32(last + 4) == 0x22);
	}
	{
		Guest guest{979};
		guest.a32(0, {a::ldm_r0_r1_r4, a::svc});
		auto cpu = a32_state();
		cpu.registers[0] = data_base + last;
		const auto result = guest.run(cpu);
		require(result.reason == ExitReason::memory_fault);
		require(result.retired_instructions == 0);
		require(result.fault.access == ::uml::memory::MemoryAccess::read);
		require(result.fault.address >= data_base + data_pages * page);
		require(reg(cpu, 0) == data_base + last);
	}
}

} // namespace

extern "C" {
void uml_arm64_gadget_a32_ld_w_i();
void uml_arm64_gadget_a32_st_h_i();
void uml_arm64_gadget_a32_ld_b_i();
void uml_arm64_gadget_a32_neon_vld1_m_16_n();
void uml_arm64_gadget_a32_neon_vst1_m_16_n();
}

namespace {

/* The probe's set index, in the words of the gadgets that carry it: the
 * shared probe's extract and the scaled add, with no crossing test in front
 * (a crossing transfer only misses). A wrong index would only miss, and the
 * miss helper finds the entry, so the words are what is compared. */
void the_tlb_probe_indexes_the_set_in_two_instructions()
{
	/* ubfx x9, x7, #12, #14; add x10, x2, x9, lsl #5 */
	constexpr std::array<std::uint32_t, 2> small{0xd34c'64e9U,
						      0x8b09'144aU};
	/* ubfx x9, x7, #14, #14; add x10, x2, x9, lsl #6 */
	constexpr std::array<std::uint32_t, 2> large{0xd34e'6ce9U,
						      0x8b09'184aU};
	for (const auto gadget : {&uml_arm64_gadget_a32_ld_w_i,
				  &uml_arm64_gadget_a32_st_h_i,
				  &uml_arm64_gadget_a32_ld_b_i,
				  &uml_arm64_gadget_a32_neon_vld1_m_16_n,
				  &uml_arm64_gadget_a32_neon_vst1_m_16_n}) {
		const auto *words = reinterpret_cast<const std::uint32_t *>(
			reinterpret_cast<std::uintptr_t>(gadget));
		const std::span<const std::uint32_t> sequence =
			page == 4096 ? std::span<const std::uint32_t>{small} :
				       std::span<const std::uint32_t>{large};
		std::size_t at = 0;
		while (at < 24 && !std::equal(sequence.begin(), sequence.end(),
					      words + at))
			++at;
		require(at < 24, "a memory gadget's probe has another index");
		/* No TBZ or TBNZ (a crossing test) ahead of the probe. */
		for (std::size_t index = 0; index < at; ++index)
			require((words[index] & 0x7e00'0000U) != 0x3600'0000U,
				"a memory gadget tests for a crossing in front");
	}
}

/* The memory gadgets' TLB probe, run against a TLB this test fills: an
 * access inside a page whose entry the TLB holds hits it, and one that
 * crosses into the next page does not, although both pages' entries are
 * there. The entries point at host bytes no guest page maps, with other
 * bytes between the two pages' copies, so a crossing access that hit would
 * read or write the bytes between. */
void the_tlb_probe_hits_inside_a_page_and_not_across_one()
{
	constexpr std::uint32_t first = 0x7000'0000U;
	constexpr std::uint32_t second = first + page;
	constexpr std::uint32_t gap = 64;
	std::vector<std::uint8_t> host(2 * page + gap);
	for (std::size_t index = 0; index < host.size(); ++index)
		host[index] = static_cast<std::uint8_t>(index < page ? 0x11 :
			index < page + gap ? 0xee : 0x22);
	auto tlb = std::make_unique<::uml::memory::Tlb>();
	const auto fill = [&](std::uint32_t address, std::uint8_t *bytes) {
		const std::uint64_t key = address >> DARWIN_UML_GUEST_PAGE_SHIFT;
		auto &entry = tlb->entries[tlb_set(key)];
		const auto base = reinterpret_cast<std::uintptr_t>(bytes) -
			address;
		entry.read_page = key;
		entry.write_page = key;
		entry.read_data_minus_address = base;
		entry.write_data_minus_address = base;
	};
	fill(first, host.data());
	fill(second, host.data() + page + gap);
	const gadgets::BuildOptions options;
	const auto run = [&](std::uint32_t word, std::uint32_t address,
			     std::uint32_t value) {
		const auto output = gadgets::a32::build(
			{.key = aarch32::a32_key(code_base),
			 .code = std::as_bytes(std::span{&word, 1}),
			 .single = true},
			{}, std::pmr::get_default_resource(), {}, options,
			gadgets::a32::decode_profile(options), 64);
		auto cpu = a32_state();
		cpu.registers[0] = value;
		cpu.registers[1] = address;
		std::size_t executed = 0;
		const auto event = gadgets::execute(output.result.program, cpu,
						    *tlb, executed);
		require(event != ::uml::cpu::EngineEvent::memory_fault,
			"an access to a page the TLB holds faulted");
		require(executed == 1);
		return cpu;
	};
	constexpr std::uint32_t ldr_r0_r1 = 0xe591'0000U;
	constexpr std::uint32_t ldrh_r0_r1 = 0xe1d1'00b0U;
	constexpr std::uint32_t ldrb_r0_r1 = 0xe5d1'0000U;
	constexpr std::uint32_t str_r0_r1 = 0xe581'0000U;
	constexpr std::uint32_t vld1_8_d0_d1_r1 = 0xf421'0a0fU;
	constexpr std::uint32_t vst1_8_d0_d1_r1 = 0xf401'0a0fU;
	/* Inside each page, up to its last byte. */
	require(reg(run(ldr_r0_r1, first + 8, 0), 0) == 0x1111'1111U);
	require(reg(run(ldr_r0_r1, second + page - 4, 0), 0) == 0x2222'2222U);
	require(reg(run(ldrh_r0_r1, first + page - 2, 0), 0) == 0x1111U);
	require(reg(run(ldrb_r0_r1, first + page - 1, 0), 0) == 0x11U);
	require(get_d(run(vld1_8_d0_d1_r1, first + page - 16, 0), 1) ==
		0x1111'1111'1111'1111ULL);
	/* Across the boundary, from both pages' copies. */
	require(reg(run(ldr_r0_r1, first + page - 2, 0), 0) == 0x2222'1111U);
	require(reg(run(ldr_r0_r1, first + page - 1, 0), 0) == 0x2222'2211U);
	require(reg(run(ldrh_r0_r1, first + page - 1, 0), 0) == 0x2211U);
	{
		const auto cpu = run(vld1_8_d0_d1_r1, first + page - 8, 0);
		require(get_d(cpu, 0) == 0x1111'1111'1111'1111ULL);
		require(get_d(cpu, 1) == 0x2222'2222'2222'2222ULL);
	}
	(void)run(str_r0_r1, first + page - 3, 0x4433'2211U);
	require(host[page - 3] == 0x11 && host[page - 2] == 0x22 &&
		host[page - 1] == 0x33 && host[page + gap] == 0x44);
	{
		auto cpu = a32_state();
		set_d(cpu, 0, 0x0807'0605'0403'0201ULL);
		set_d(cpu, 1, 0x100f'0e0d'0c0b'0a09ULL);
		const auto output = gadgets::a32::build(
			{.key = aarch32::a32_key(code_base),
			 .code = std::as_bytes(std::span{&vst1_8_d0_d1_r1, 1}),
			 .single = true},
			{}, std::pmr::get_default_resource(), {}, options,
			gadgets::a32::decode_profile(options), 64);
		cpu.registers[1] = second - 4;
		std::size_t executed = 0;
		require(gadgets::execute(output.result.program, cpu, *tlb,
					 executed) !=
			::uml::cpu::EngineEvent::memory_fault);
		for (std::uint32_t index = 0; index < 16; ++index)
			require(host[index < 4 ? page - 4 + index :
				     page + gap + index - 4] == index + 1);
	}
	for (std::size_t index = page; index < page + gap; ++index)
		require(host[index] == 0xee, "a crossing access hit");
}

/* An entry the TLB's own fill path (Tlb::translate, the miss helper's) puts
 * in the table hits through the probe the gadgets emit. A probe that looked
 * in another set would only miss, and the miss helper would find the entry,
 * so every entry of the other sets holds the same page with another
 * backing: a probe of the wrong set reads that one instead. The pages
 * spread over the 32-bit space, where the 4 KiB index folds the high page
 * bits into the low ones. */
void an_entry_the_tlb_fills_hits_through_the_emitted_probe()
{
	Guest guest{1104};
	std::array<std::uint32_t, 8> pages{
		0x0000'7000U, 0x0200'0000U, 0x0fff'f000U, 0x1234'5000U,
		0x7000'0000U, 0x8000'1000U, 0xc3a5'e000U, 0xffff'e000U};
	for (auto &address : pages) {
		address &= ~(page - 1U);
		guest.map_data(address);
	}
	std::vector<std::uint8_t> decoy(page, 0xdd);
	const gadgets::BuildOptions options;
	const auto run = [&](::uml::memory::Tlb &tlb, std::uint32_t word,
			     std::uint32_t address, std::uint32_t value) {
		const auto output = gadgets::a32::build(
			{.key = aarch32::a32_key(code_base),
			 .code = std::as_bytes(std::span{&word, 1}),
			 .single = true},
			{}, std::pmr::get_default_resource(), {}, options,
			gadgets::a32::decode_profile(options), 64);
		auto cpu = a32_state();
		cpu.registers[0] = value;
		cpu.registers[1] = address;
		std::size_t executed = 0;
		require(gadgets::execute(output.result.program, cpu, tlb,
					 executed) !=
			::uml::cpu::EngineEvent::memory_fault);
		require(executed == 1);
		return cpu;
	};
	constexpr std::uint32_t ldr_r0_r1 = 0xe591'0000U;
	constexpr std::uint32_t ldrb_r0_r1 = 0xe5d1'0000U;
	constexpr std::uint32_t str_r0_r1 = 0xe581'0000U;
	constexpr std::uint32_t vld1_8_d0_d1_r1 = 0xf421'0a0fU;
	for (const auto address : pages) {
		auto tlb = std::make_unique<::uml::memory::Tlb>();
		tlb->generation = guest.generation();
		const auto key = std::uint64_t{address} >> DARWIN_UML_GUEST_PAGE_SHIFT;
		auto *const host = tlb->translate(address, ::uml::memory::Access::write);
		require(host != nullptr &&
			tlb->translate(address, ::uml::memory::Access::read) == host);
		std::fill_n(reinterpret_cast<std::uint8_t *>(host), page,
			    std::uint8_t{0x5a});
		std::size_t filled = tlb->entries.size();
		for (std::size_t index = 0; index < tlb->entries.size(); ++index)
			if (tlb->entries[index].read_page == key) {
				require(filled == tlb->entries.size());
				filled = index;
			}
		require(filled < tlb->entries.size());
		const auto other = reinterpret_cast<std::uintptr_t>(decoy.data()) -
			address;
		for (std::size_t index = 0; index < tlb->entries.size(); ++index)
			if (index != filled)
				tlb->entries[index] = {key, other, key, other};
		require(reg(run(*tlb, ldr_r0_r1, address + 0x40, 0), 0) ==
			0x5a5a'5a5aU);
		require(reg(run(*tlb, ldrb_r0_r1, address + page - 1, 0), 0) ==
			0x5aU);
		require(get_d(run(*tlb, vld1_8_d0_d1_r1, address + 0x80, 0), 1) ==
			0x5a5a'5a5a'5a5a'5a5aULL);
		(void)run(*tlb, str_r0_r1, address + 0x100, 0x1234'5678U);
		require(reinterpret_cast<std::uint8_t *>(host)[0x100] == 0x78);
		require(std::ranges::all_of(decoy, [](std::uint8_t byte) {
			return byte == 0xdd;
		}));
	}
}

} // namespace

int main()
{
	const ::uml::testing::RuntimeTestRoute runtime_route;
	straight_line_retires_every_instruction();
	skipped_conditional_instructions_retire();
	conditional_data_processing_selects_without_a_skip();
	conditional_branch_retires_taken_or_not();
	conditional_supervisor_call_is_skipped_or_taken();
	a_data_fault_stops_at_the_instruction();
	a_fault_inside_an_it_block_keeps_its_itstate();
	a_supervisor_call_inside_an_it_block_resumes_mid_block();
	an_undefined_instruction_does_not_retire();
	a_conditional_undefined_word_is_skipped_when_its_condition_fails();
	a_breakpoint_reports_its_immediate();
	interworking_branches_switch_state_both_ways();
	an_a32_target_with_bit_one_set_is_a_pc_alignment_exit();
	a_fetch_fault_reports_the_unfetchable_address();
	a_t32_instruction_split_over_an_unfetchable_page_faults_at_its_second_halfword();
	the_pc_reads_as_the_architectural_value();
	the_instruction_budget_ends_a_loop();
	single_step_retires_one_instruction_at_a_time();
	single_step_walks_an_it_block();
	changed_code_is_translated_again();
	a_a32_block_and_a_t32_block_at_one_address_are_distinct();
	exclusive_loops_complete_and_store_once();
	a_store_without_a_reservation_fails();
	exclusive_loops_progress_with_colliding_pages();
	a_misaligned_exclusive_faults_as_an_alignment_fault();
	acquire_loads_and_release_stores_round_trip();
	an_unaligned_acquire_or_release_is_an_alignment_fault();
	an_acquire_or_release_to_unmapped_memory_faults_without_effect();
	a_t32_exclusive_or_ordered_fault_names_its_instruction();
	a_release_store_without_a_reservation_fails();
	the_kuser_compare_and_swap_succeeds_or_reports_a_mismatch();
	set_event_local_is_a_hint();
	coprocessor_and_counter_reads();
	the_counter_does_not_run_backwards();
	cp15_barriers_follow_the_enable();
	reading_the_status_register_gives_flags_q_ge_and_user_mode();
	block_transfers_cross_a_page_boundary();
	a_pop_to_pc_across_a_page_boundary_branches();
	a_conditional_block_transfer_across_a_page_follows_its_condition();
	a_double_word_load_crosses_a_page_boundary();
	the_tlb_probe_hits_inside_a_page_and_not_across_one();
	the_tlb_probe_indexes_the_set_in_two_instructions();
	an_entry_the_tlb_fills_hits_through_the_emitted_probe();
	a_block_transfer_into_an_unmapped_page_faults_at_that_page();
	advanced_simd_floating_point_uses_the_standard_fpscr();
	a_library_floating_point_instruction_keeps_the_flags_around_it();
	the_fp_environment_follows_the_instruction_kind();
	conversions_to_and_from_integers_run_in_the_active_environment();
	vmax_and_vmin_switch_the_environment_only_for_nan_and_denormal_operands();
	gadgets_leave_the_callee_saved_vector_registers_alone();
	add_sub_and_mul_run_in_the_active_environment_for_ordinary_operands();
	structure_loads_and_stores_move_the_elements_and_write_back();
	vld1_and_vst1_copies_stay_out_of_the_library();
	vld1_and_vst1_copies_write_back_in_each_mode();
	vld1_and_vst1_copies_across_a_page_stay_out_of_the_library();
	an_add_that_computes_a_copy_base_runs_in_the_copy();
	hinted_copies_and_loads_to_all_lanes_have_their_own_gadgets();
	a_compare_and_the_flag_transfer_after_it_run_as_one();
	a_conditional_floating_point_move_selects_without_a_skip();
	an_a32_block_holds_all_sixty_four_instructions();
	in_place_add_and_sub_of_an_immediate_reach_every_register();
	compare_and_branch_pairs_take_the_branch_as_the_pair_does();
	parallel_arithmetic_and_sel_stay_out_of_the_library();
	structure_and_library_blocks_load_the_deferred_vectors();
	single_stepped_floating_point_loads_the_deferred_vectors();
	host_features_decide_what_the_guest_may_execute();
	std::puts("arm64-gadget-a32-control: ok");
	return 0;
}
