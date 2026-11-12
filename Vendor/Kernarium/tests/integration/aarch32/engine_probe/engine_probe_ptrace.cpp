// SPDX-License-Identifier: GPL-2.0-or-later
/* Group "ptrace": a child under ptrace in AArch32. The tracer reads its
 * registers at a BKPT stop (r0-r15, the flags and the mode), changes them and
 * resumes it, sees SIGTRAP with TRAP_BRKPT for the debugger encodings (udf
 * #16 in A32, udf #1 in T32), steps it one instruction at a time through
 * branches, an SVC and an A32/T32 switch, and steps through an IT block with
 * ITSTATE advancing after each instruction. A step over a SWP the kernel
 * emulates (abi.swp 1), or skips for its condition, stops with TRAP_TRACE at
 * the next instruction. The thread area request returns TPIDRURO. */
#include "engine_probe.hpp"

extern "C" {
long pt_regs_arm();
long pt_regs_thumb();
extern const char pt_regs_arm_stop[], pt_regs_thumb_stop[];
long pt_break_arm();
long pt_break_thumb();
extern const char pt_break_arm_insn[], pt_break_thumb_insn[];
long pt_step_it();
extern const char pt_step_it_stop[], pt_step_it_first[];
long pt_step_arm();
extern const char pt_step_arm_stop[], pt_step_arm_s1[], pt_step_arm_s5[], pt_step_arm_t1[];
long pt_step_swp();
extern const char pt_step_swp_stop[], pt_step_swp_s1[], pt_step_swp_s4[];
}

namespace probe {
namespace {

enum : long {
	ptrace_traceme = 0,
	ptrace_cont = 7,
	ptrace_kill = 8,
	ptrace_singlestep = 9,
	ptrace_getregs = 12,
	ptrace_setregs = 13,
	ptrace_get_thread_area = 22,
	ptrace_getsiginfo = 0x4202,
};

constexpr int wall = 0x40000000;
constexpr u32 psr_thumb = 1U << 5;

struct Stop {
	bool stopped;
	int signal;
	long status;
};

Stop wait_for_stop(long pid)
{
	i32 status = 0;
	const long got = sys(nr_wait4, pid, addr(&status), wall, 0);
	Stop stop{};
	stop.status = status;
	if (got != pid)
		return stop;
	stop.stopped = (status & 0xff) == 0x7f;
	stop.signal = (status >> 8) & 0xff;
	return stop;
}

long get_registers(long pid, u32 *regs)
{
	return sys(nr_ptrace, ptrace_getregs, pid, 0, addr(regs));
}

long set_registers(long pid, u32 *regs)
{
	return sys(nr_ptrace, ptrace_setregs, pid, 0, addr(regs));
}

u32 itstate_of(u32 cpsr)
{
	return (((cpsr >> 10) & 0x3f) << 2) | ((cpsr >> 25) & 3);
}

/* A step that did not stop with SIGTRAP: where, as a NOTE line. */
void note_bad_step(long child, u32 step, const Stop &stop)
{
	u32 info[32] = {};
	u32 regs[18] = {};
	sys(nr_ptrace, ptrace_getsiginfo, child, 0, addr(info));
	get_registers(child, regs);
	put("NOTE ptrace step ");
	put_dec(step);
	put(" status ");
	put_hex(static_cast<u32>(stop.status));
	put(" si_code ");
	put_hex(info[2]);
	put(" si_addr ");
	put_hex(info[3]);
	put(" pc ");
	put_hex(regs[15]);
	put(" cpsr ");
	put_hex(regs[16]);
	put("\n");
}

/* Forks a child that asks to be traced and stops itself; returns its pid with
 * the tracer past the SIGSTOP and the child about to run its routine. */
long start_traced(long (*routine)(), word tls)
{
	const long child = fork_child();
	if (child == 0) {
		sys(nr_ptrace, ptrace_traceme, 0, 0, 0);
		if (tls)
			sys(nr_set_tls, static_cast<long>(tls));
		raise_signal(sigstop);
		const long result = routine();
		quit(result & 0xff);
	}
	const Stop stop = wait_for_stop(child);
	if (!stop.stopped || stop.signal != sigstop) {
		sys(nr_kill, child, 9);
		wait_child(child);
		return -1;
	}
	sys(nr_ptrace, ptrace_cont, child, 0, 0);
	return child;
}

void finish(long child)
{
	sys(nr_kill, child, 9);
	wait_child(child);
}

} // namespace

void group_ptrace()
{
	if (under_qemu())
		return;
	char dummy = 0;
	(void)dummy;

	/* Registers at a BKPT stop, changed and resumed, A32 and T32. */
	for (int thumb = 0; thumb < 2; ++thumb) {
		const char *stop_address = thumb ? pt_regs_thumb_stop : pt_regs_arm_stop;
		const long child = start_traced(thumb ? pt_regs_thumb : pt_regs_arm, 0);
		check(child > 0, thumb ? "T32: a traced child starts" : "A32: a traced child starts");
		if (child <= 0)
			continue;
		const Stop stop = wait_for_stop(child);
		check(stop.stopped && stop.signal == sigtrap,
		      thumb ? "T32: bkpt stops the tracee with SIGTRAP" : "A32: bkpt stops the tracee with SIGTRAP");
		SigInfo info{};
		sys(nr_ptrace, ptrace_getsiginfo, child, 0, addr(&info));
		check_eq(static_cast<u32>(info.code), 1, thumb ? "T32: si_code is TRAP_BRKPT" : "A32: si_code is TRAP_BRKPT");
		check_eq(info.address, addr(stop_address) & ~1UL, thumb ? "T32: si_addr is the bkpt" : "A32: si_addr is the bkpt");
		u32 regs[18] = {};
		check_eq(static_cast<u32>(get_registers(child, regs)), 0, "PTRACE_GETREGS");
		check_eq(regs[15], addr(stop_address) & ~1UL, thumb ? "T32: pc is the bkpt" : "A32: pc is the bkpt");
		bool magic = true;
		for (unsigned index = 4; index <= 10; ++index)
			magic = magic && regs[index] == index * 0x11111111U;
		check(magic, thumb ? "T32: r4-r10 are the child's" : "A32: r4-r10 are the child's");
		check_eq(regs[0], 0x10, thumb ? "T32: r0 is the child's" : "A32: r0 is the child's");
		check_eq(regs[16] & 0xf0000000U, 0xa0000000U, thumb ? "T32: N and C are set" : "A32: N and C are set");
		check_eq(regs[16] & 0x1f, 0x10, thumb ? "T32: the mode is User" : "A32: the mode is User");
		check_eq((regs[16] & psr_thumb) != 0, thumb ? 1U : 0U, thumb ? "T32: the T bit is set" : "A32: the T bit is clear");
		regs[0] = 0x42;
		regs[5] = 0x77;
		regs[15] += thumb ? 2 : 4;
		check_eq(static_cast<u32>(set_registers(child, regs)), 0, "PTRACE_SETREGS");
		sys(nr_ptrace, ptrace_cont, child, 0, 0);
		const long status = wait_child(child);
		check(exited_with(status, 0x42 + 0x77), thumb ? "T32: the child runs on with the registers set (r0 + r5)" :
							    "A32: the child runs on with the registers set (r0 + r5)");
		if (!exited_with(status, 0x42 + 0x77))
			finish(child);
	}

	/* The debugger encodings raise SIGTRAP at the tracer too. */
	for (int thumb = 0; thumb < 2; ++thumb) {
		const long child = start_traced(thumb ? pt_break_thumb : pt_break_arm, 0);
		if (child <= 0) {
			check(false, "a traced child starts");
			continue;
		}
		const Stop stop = wait_for_stop(child);
		check(stop.stopped && stop.signal == sigtrap,
		      thumb ? "T32 udf #1 stops the tracee with SIGTRAP" : "A32 udf #16 stops the tracee with SIGTRAP");
		SigInfo info{};
		sys(nr_ptrace, ptrace_getsiginfo, child, 0, addr(&info));
		check_eq(static_cast<u32>(info.code), 1, thumb ? "T32 udf #1: si_code is TRAP_BRKPT" : "A32 udf #16: si_code is TRAP_BRKPT");
		u32 regs[18] = {};
		get_registers(child, regs);
		const word insn = addr(thumb ? pt_break_thumb_insn : pt_break_arm_insn) & ~1UL;
		check_eq(regs[15], insn, thumb ? "T32 udf #1: pc is the instruction" : "A32 udf #16: pc is the instruction");
		regs[15] += thumb ? 2 : 4;
		set_registers(child, regs);
		sys(nr_ptrace, ptrace_cont, child, 0, 0);
		const long status = wait_child(child);
		check(exited_with(status, thumb ? 0x22 : 0x21),
		      thumb ? "T32: the child resumes after udf #1" : "A32: the child resumes after udf #16");
		if (!exited_with(status, thumb ? 0x22 : 0x21))
			finish(child);
	}

	/* The thread area is TPIDRURO. */
	{
		const long child = start_traced(pt_regs_arm, 0x13572468);
		if (child > 0) {
			wait_for_stop(child);
			u32 area = 0;
			check_eq(static_cast<u32>(sys(nr_ptrace, ptrace_get_thread_area, child, 0, addr(&area))), 0,
				 "PTRACE_GET_THREAD_AREA");
			check_eq(area, 0x13572468, "and returns TPIDRURO");
			finish(child);
		}
	}

	/* Single steps through an IT block: ITSTATE after IT, and after each slot. */
	{
		const long child = start_traced(pt_step_it, 0);
		if (child > 0) {
			Stop stop = wait_for_stop(child);
			u32 regs[18] = {};
			get_registers(child, regs);
			const word first = addr(pt_step_it_first) & ~1UL;
			regs[15] = first;
			set_registers(child, regs);
			static const u32 itstate[] = {0x00, 0x00, 0x0b, 0x16, 0x0c, 0x18, 0x00};
			u32 wrong_pc = 0, wrong_it = 0, not_stopped = 0, lost_t = 0;
			for (u32 step = 0; step < 7; ++step) {
				sys(nr_ptrace, ptrace_singlestep, child, 0, 0);
				stop = wait_for_stop(child);
				if (!stop.stopped || stop.signal != sigtrap) {
					++not_stopped;
					note_bad_step(child, step, stop);
					break;
				}
				get_registers(child, regs);
				if (regs[15] != first + 2 * (step + 1))
					++wrong_pc;
				if (itstate_of(regs[16]) != itstate[step])
					++wrong_it;
				if (!(regs[16] & psr_thumb))
					++lost_t;
			}
			check_eq(not_stopped, 0, "each single step stops the child with SIGTRAP");
			check_eq(wrong_pc, 0, "each step advances the pc by one instruction");
			check_eq(wrong_it, 0, "ITSTATE after IT, and after each slot, is 0b, 16, 0c, 18, 00");
			check_eq(lost_t, 0, "the T bit stays set");
			sys(nr_ptrace, ptrace_cont, child, 0, 0);
			const long status = wait_child(child);
			check(exited_with(status, 5), "the IT block ran its slots (1 + 4)");
			if (!exited_with(status, 5))
				finish(child);
		}
	}

	/* Single steps through branches, an SVC and an A32 to T32 switch. */
	{
		const long child = start_traced(pt_step_arm, 0);
		if (child > 0) {
			Stop stop = wait_for_stop(child);
			u32 regs[18] = {};
			get_registers(child, regs);
			const word s1 = addr(pt_step_arm_s1);
			const word s5 = addr(pt_step_arm_s5);
			const word t1 = addr(pt_step_arm_t1) & ~1UL;
			regs[15] = s1;
			set_registers(child, regs);
			const word expected_pc[] = {s1 + 4, s1 + 8, s5, s5 + 4, s5 + 8, s5 + 12, s5 + 16, s5 + 20,
						    t1, t1 + 2, t1 + 4};
			u32 wrong_pc = 0, not_stopped = 0, wrong_state = 0;
			u32 pid_seen = 0;
			for (u32 step = 0; step < 11; ++step) {
				sys(nr_ptrace, ptrace_singlestep, child, 0, 0);
				stop = wait_for_stop(child);
				if (!stop.stopped || stop.signal != sigtrap) {
					++not_stopped;
					note_bad_step(child, step, stop);
					break;
				}
				get_registers(child, regs);
				if (regs[15] != expected_pc[step])
					++wrong_pc;
				const bool thumb = step >= 8;
				if (((regs[16] & psr_thumb) != 0) != thumb) {
					++wrong_state;
					note_bad_step(child, step, stop);
				}
				if (step == 5)
					pid_seen = regs[0];
			}
			check_eq(not_stopped, 0, "each A32 single step stops the child with SIGTRAP");
			check_eq(wrong_pc, 0, "steps follow a branch, an SVC and bx into T32");
			check_eq(wrong_state, 0, "and the T bit follows the instruction set");
			check_eq(pid_seen, static_cast<u32>(child), "stepping over an SVC leaves its result in r0");
			sys(nr_ptrace, ptrace_singlestep, child, 0, 0);
			stop = wait_for_stop(child);
			get_registers(child, regs);
			check((regs[16] & psr_thumb) == 0, "a step through pop {pc} returns to A32");
			sys(nr_ptrace, ptrace_cont, child, 0, 0);
			const long status = wait_child(child);
			check(exited_with(status, 0x37), "the stepped routine returned 0x30 + 7");
			if (!exited_with(status, 0x37))
				finish(child);
		}
	}

	/* Single steps over a SWP the kernel emulates, and one it skips. */
	{
		const long setting = swp_setting();
		const bool swp_ready = setting == 1 || (setting == 0 && swp_set(1));
		if (!swp_ready) {
			line("NOTE ptrace: abi.swp is 0 and not writable by this process; the SWP steps are skipped");
			return;
		}
		const long child = start_traced(pt_step_swp, 0);
		check(child > 0, "a traced child for the SWP steps starts");
		if (child <= 0)
			return;
		Stop stop = wait_for_stop(child);
		u32 regs[18] = {};
		get_registers(child, regs);
		const word s1 = addr(pt_step_swp_s1);
		const word s4 = addr(pt_step_swp_s4);
		regs[15] = s1;
		set_registers(child, regs);

		sys(nr_ptrace, ptrace_singlestep, child, 0, 0);
		stop = wait_for_stop(child);
		check(stop.stopped && stop.signal == sigtrap, "a step over an emulated swp stops the child with SIGTRAP");
		SigInfo info{};
		sys(nr_ptrace, ptrace_getsiginfo, child, 0, addr(&info));
		check_eq(static_cast<u32>(info.code), 2, "and si_code is TRAP_TRACE");
		check_eq(info.address, s1 + 4, "and si_addr is the next instruction");
		get_registers(child, regs);
		check_eq(regs[15], s1 + 4, "the pc is past the swp");
		check_eq(regs[0], 0x55, "the swap is done: Rt holds the old word");

		for (int step = 0; step < 2; ++step) {
			sys(nr_ptrace, ptrace_singlestep, child, 0, 0);
			stop = wait_for_stop(child);
			if (!stop.stopped || stop.signal != sigtrap)
				check(false, "the steps between the swaps stop with SIGTRAP");
		}
		get_registers(child, regs);
		check_eq(regs[15], s4, "the pc is at the conditional swp");
		check_eq(regs[16] & 0x40000000U, 0, "its condition is false: Z is clear");
		sys(nr_ptrace, ptrace_singlestep, child, 0, 0);
		stop = wait_for_stop(child);
		check(stop.stopped && stop.signal == sigtrap, "a step over a swp whose condition is false stops with SIGTRAP");
		sys(nr_ptrace, ptrace_getsiginfo, child, 0, addr(&info));
		check_eq(static_cast<u32>(info.code), 2, "and si_code is TRAP_TRACE");
		check_eq(info.address, s4 + 4, "and si_addr is the next instruction");
		get_registers(child, regs);
		check_eq(regs[15], s4 + 4, "the pc is past the skipped swp");
		check_eq(regs[5], 0x77, "and Rt is untouched");
		sys(nr_ptrace, ptrace_cont, child, 0, 0);
		const long status = wait_child(child);
		check(exited_with(status, 0xaa), "the routine found the emulated swp's store in memory");
		if (!exited_with(status, 0xaa))
			finish(child);
	}
}

} // namespace probe
