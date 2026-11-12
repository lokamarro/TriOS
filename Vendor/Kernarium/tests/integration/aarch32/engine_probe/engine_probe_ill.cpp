// SPDX-License-Identifier: GPL-2.0-or-later
/* Group "ill": what an AArch32 task may not execute raises SIGILL with the
 * address of the instruction, from both instruction sets: UDF, undefined
 * encodings, coprocessor instructions that do not exist or that need a
 * privilege, SMC, HVC, ERET, SETEND, and SWP and SWPB while abi.swp is 0 (the
 * "swp" group runs them at 1). The encodings a debugger plants (BKPT, UDF
 * #16, and the two Thumb forms) raise SIGTRAP. The CP15 barrier operations
 * run, unless abi.cp15_barrier is 0. The hints, barriers and exclusives that
 * must run, do. */
#include "engine_probe.hpp"

extern "C" {
#define DECLARE(name) void name()
DECLARE(ill_udf0_arm); DECLARE(ill_udf17_arm); DECLARE(ill_udf_ffff_arm); DECLARE(ill_media_arm);
DECLARE(ill_cdp_arm); DECLARE(ill_ldc_cp3_arm); DECLARE(ill_mrc_midr_arm);
DECLARE(ill_mcr_sctlr_arm); DECLARE(ill_mcr_iciallu_arm); DECLARE(ill_mcr_dccimvac_arm);
DECLARE(ill_mcr_ttbr0_arm); DECLARE(ill_smc_arm); DECLARE(ill_hvc_arm); DECLARE(ill_eret_arm);
DECLARE(ill_swp_arm); DECLARE(ill_swpb_arm); DECLARE(ill_setend_arm);
DECLARE(ill_udf0_thumb); DECLARE(ill_udf2_thumb); DECLARE(ill_udfw1_thumb);
DECLARE(ill_mrc_midr_thumb); DECLARE(ill_mcr_sctlr_thumb); DECLARE(ill_setend_thumb);
DECLARE(ill_smc_thumb);
DECLARE(brk_bkpt_arm); DECLARE(brk_udf16_arm); DECLARE(brk_bkpt_thumb); DECLARE(brk_udf1_thumb);
DECLARE(brk_udfw0_thumb);
DECLARE(cp_dmb_arm); DECLARE(cp_dsb_arm); DECLARE(cp_isb_arm);
DECLARE(cp_dmb_thumb); DECLARE(cp_dsb_thumb); DECLARE(cp_isb_thumb);
long ok_hints_arm(void *readable);
long ok_hints_thumb(void *readable);
}

namespace probe {
namespace {

struct Case {
	void (*routine)();
	word skip;
	const char *name;
	/* qemu-user runs these instead of raising SIGILL. */
	bool implemented_by_qemu;
	/* The kernel emulates these when abi.swp is 1. */
	bool swp = false;
};

const Case undefined_cases[] = {
	{ill_udf0_arm, 4, "A32 udf #0", false},
	{ill_udf17_arm, 4, "A32 udf #17 (not the breakpoint encoding)", false},
	{ill_udf_ffff_arm, 4, "A32 udf #0xffff", false},
	{ill_media_arm, 4, "A32 undefined media encoding", false},
	{ill_cdp_arm, 4, "A32 cdp to coprocessor 3", false},
	{ill_ldc_cp3_arm, 4, "A32 ldc to coprocessor 3", false},
	{ill_mrc_midr_arm, 4, "A32 read of MIDR", true},
	{ill_mcr_sctlr_arm, 4, "A32 write of SCTLR", false},
	{ill_mcr_iciallu_arm, 4, "A32 ICIALLU", false},
	{ill_mcr_dccimvac_arm, 4, "A32 DCCIMVAC", false},
	{ill_mcr_ttbr0_arm, 4, "A32 write of TTBR0", false},
	{ill_smc_arm, 4, "A32 smc", false},
	{ill_hvc_arm, 4, "A32 hvc", false},
	{ill_eret_arm, 4, "A32 eret", false},
	{ill_swp_arm, 4, "A32 swp", true, true},
	{ill_swpb_arm, 4, "A32 swpb", true, true},
	{ill_setend_arm, 4, "A32 setend be", true},
	{ill_udf0_thumb, 2, "T32 udf #0", false},
	{ill_udf2_thumb, 2, "T32 udf #2 (not the breakpoint encoding)", false},
	{ill_udfw1_thumb, 4, "T32 udf.w #1 (not the breakpoint encoding)", false},
	{ill_mrc_midr_thumb, 4, "T32 read of MIDR", true},
	{ill_mcr_sctlr_thumb, 4, "T32 write of SCTLR", false},
	{ill_setend_thumb, 2, "T32 setend be", true},
	{ill_smc_thumb, 4, "T32 smc", false},
};

const Case trap_cases[] = {
	{brk_bkpt_arm, 4, "A32 bkpt #0x12", false},
	{brk_udf16_arm, 4, "A32 udf #16", true},
	{brk_bkpt_thumb, 2, "T32 bkpt #0x34", false},
	{brk_udf1_thumb, 2, "T32 udf #1", true},
	{brk_udfw0_thumb, 4, "T32 udf.w #0", true},
};

const Case barrier_cases[] = {
	{cp_dmb_arm, 4, "A32 CP15 DMB", false},
	{cp_dsb_arm, 4, "A32 CP15 DSB", false},
	{cp_isb_arm, 4, "A32 CP15 ISB", false},
	{cp_dmb_thumb, 4, "T32 CP15 DMB", false},
	{cp_dsb_thumb, 4, "T32 CP15 DSB", false},
	{cp_isb_thumb, 4, "T32 CP15 ISB", false},
};

const char *labelled(char *buffer, const char *prefix, const char *text)
{
	word length = 0;
	for (const char *p = prefix; *p; ++p)
		buffer[length++] = *p;
	buffer[length++] = ':';
	buffer[length++] = ' ';
	for (const char *p = text; *p; ++p)
		buffer[length++] = *p;
	buffer[length] = 0;
	return buffer;
}

/* Whether the process may write abi.cp15_barrier: root may, an Android shell
 * may not, and then its checks have nothing to switch. */
bool cp15_barrier_writable()
{
	const long fd = sys(nr_openat, -100, addr("/proc/sys/abi/cp15_barrier"), 1);
	if (fd < 0)
		return false;
	sys(nr_close, fd);
	return true;
}

bool set_cp15_barrier(const char *value)
{
	const long fd = sys(nr_openat, -100, addr("/proc/sys/abi/cp15_barrier"), 1);
	if (fd < 0)
		return false;
	const long written = sys(nr_write, fd, addr(value), 2);
	sys(nr_close, fd);
	return written == 2;
}

/* Runs every barrier case and returns how many raised SIGILL. */
u32 run_barriers(u32 *wrong_address)
{
	u32 raised = 0;
	for (const Case &barrier : barrier_cases) {
		catch_signal(sigill, barrier.skip);
		barrier.routine();
		release_signal(sigill);
		if (caught.count == 1) {
			++raised;
			if (caught.pc != (addr(barrier.routine) & ~1UL))
				++*wrong_address;
		}
	}
	return raised;
}

} // namespace

void group_ill()
{
	char name[128];
	/* SWP and SWPB raise SIGILL only while abi.swp is 0. */
	const bool swp_emulated = !under_qemu() && swp_setting() == 1;
	for (const Case &test : undefined_cases) {
		if (test.implemented_by_qemu && under_qemu())
			continue;
		if (test.swp && swp_emulated)
			continue;
		catch_signal(sigill, test.skip);
		test.routine();
		release_signal(sigill);
		check_eq(caught.count, 1, labelled(name, test.name, "raises SIGILL"));
		check_eq(caught.address, addr(test.routine) & ~1UL, labelled(name, test.name, "si_addr is the instruction"));
		/* qemu-user reports ILL_ILLOPN. */
		if (!under_qemu())
			check_eq(static_cast<u32>(caught.code), 1,
				 labelled(name, test.name, "si_code is ILL_ILLOPC"));
	}

	for (const Case &test : trap_cases) {
		if (test.implemented_by_qemu && under_qemu())
			continue;
		catch_signal(sigtrap, test.skip);
		test.routine();
		release_signal(sigtrap);
		check_eq(caught.count, 1, labelled(name, test.name, "raises SIGTRAP"));
		check_eq(static_cast<u32>(caught.code), 1, labelled(name, test.name, "si_code is TRAP_BRKPT"));
		check_eq(caught.address, addr(test.routine) & ~1UL, labelled(name, test.name, "si_addr is the instruction"));
		check_eq(caught.pc, addr(test.routine) & ~1UL, labelled(name, test.name, "the frame's pc is the instruction"));
	}

	/* The CP15 barriers run under the default setting. */
	u32 wrong_address = 0;
	check_eq(run_barriers(&wrong_address), 0, "the CP15 barrier operations run by default");

	/* Hints, barriers, exclusives and prefetches run. */
	static u32 word_value = 5;
	check_eq(static_cast<u32>(ok_hints_arm(&word_value)), 1, "A32 hints, barriers, clrex, pld and pli run");
	check_eq(static_cast<u32>(ok_hints_thumb(&word_value)), 1, "T32 hints, barriers, clrex, pld and pli run");

	/* abi.cp15_barrier: 0 makes the barriers UNDEFINED, 1 and 2 run them. */
	if (!under_qemu() && !cp15_barrier_writable()) {
		line("NOTE ill: abi.cp15_barrier is not writable by this process; the switch is not tested");
	} else if (!under_qemu()) {
		check(set_cp15_barrier("0\n"), "abi.cp15_barrier accepts 0");
		wrong_address = 0;
		const u32 raised = run_barriers(&wrong_address);
		check_eq(raised, 6, "with abi.cp15_barrier 0 all six CP15 barriers raise SIGILL");
		check_eq(wrong_address, 0, "at the instruction");
		set_cp15_barrier("2\n");
		wrong_address = 0;
		check_eq(run_barriers(&wrong_address), 0, "with abi.cp15_barrier 2 they run");
		set_cp15_barrier("0\n");
		check_eq(run_barriers(&wrong_address), 6, "and with 0 again they raise SIGILL");
		check(set_cp15_barrier("1\n"), "abi.cp15_barrier accepts 1");
		check_eq(run_barriers(&wrong_address), 0, "with 1 (the default) they run again");
	}
}

} // namespace probe
