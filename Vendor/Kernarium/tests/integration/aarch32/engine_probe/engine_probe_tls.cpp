// SPDX-License-Identifier: GPL-2.0-or-later
/* Group "tls": the two thread registers a 32-bit task has: TPIDRURO, set by
 * the set_tls system call and by CLONE_SETTLS and read by user code, and
 * TPIDRURW, which user code reads and writes. Each belongs to a task, so they
 * have to survive system calls, signals and context switches, be inherited by
 * fork and clone as arm and arm64 Linux do, and be separate between threads.
 * Writing TPIDRURO from user code is undefined. */
#include "engine_probe.hpp"

namespace probe {
namespace {

word read_ro()
{
	word value;
	asm volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(value));
	return value;
}

word read_rw()
{
	word value;
	asm volatile("mrc p15, 0, %0, c13, c0, 2" : "=r"(value));
	return value;
}

void write_rw(word value)
{
	asm volatile("mcr p15, 0, %0, c13, c0, 2" : : "r"(value));
}

/* The kernel's get_tls helper. */
word kuser_tls()
{
	register word result asm("r0");
	asm volatile("movw ip, #0x0fe0\n\tmovt ip, #0xffff\n\tblx ip"
		     : "=r"(result) : : "ip", "lr", "memory");
	return result;
}

struct Report {
	word ro_at_start, rw_at_start;
	word ro_after, rw_after;
	u32 bad;
	u32 done;
};
Report *shared_report;

/* Holds `ro` and `rw` for `rounds` system calls and yields; counts a change. */
u32 hold(word ro, word rw, u32 rounds)
{
	u32 bad = 0;
	for (u32 round = 0; round < rounds; ++round) {
		if (read_ro() != ro || read_rw() != rw || kuser_tls() != ro)
			++bad;
		yield();
		if (round % 8 == 0)
			sleep_ms(1);
		if (getpid() <= 0)
			++bad;
	}
	return bad;
}

struct ThreadArgument {
	word expect_ro;
	word write_rw;
	word inherited_rw;
	Report report;
};

long thread_body(void *argument)
{
	auto *state = static_cast<ThreadArgument *>(argument);
	state->report.ro_at_start = read_ro();
	state->report.rw_at_start = read_rw();
	write_rw(state->write_rw);
	state->report.bad = hold(state->expect_ro, state->write_rw, 200);
	state->report.ro_after = read_ro();
	state->report.rw_after = read_rw();
	return 0;
}

} // namespace

void group_tls()
{
	/* TPIDRURO is what set_tls says, through every way to read it. */
	check_eq(static_cast<u32>(sys(nr_set_tls, 0x10203040)), 0, "set_tls");
	check_eq(read_ro(), 0x10203040, "TPIDRURO holds the value");
	check_eq(kuser_tls(), 0x10203040, "the kuser get_tls helper returns it");

	/* TPIDRURW is written by user code. */
	write_rw(0x5a5a1234);
	check_eq(read_rw(), 0x5a5a1234, "TPIDRURW reads back what was written");
	check_eq(hold(0x10203040, 0x5a5a1234, 100), 0, "both survive 100 system calls and yields");

	/* A signal handler sees the same registers, and what it writes to
	 * TPIDRURW is the thread's after it returns. */
	{
		static word handler_ro, handler_rw;
		auto handler = [](int, SigInfo *, UContext *) {
			handler_ro = read_ro();
			handler_rw = read_rw();
			write_rw(0x77770001);
		};
		set_handler(sigusr1, handler);
		raise_signal(sigusr1);
		reset_handler(sigusr1);
		check_eq(handler_ro, 0x10203040, "a signal handler sees TPIDRURO");
		check_eq(handler_rw, 0x5a5a1234, "a signal handler sees TPIDRURW");
		check_eq(read_ro(), 0x10203040, "TPIDRURO is unchanged after the handler");
		check_eq(read_rw(), 0x77770001, "TPIDRURW as the handler left it is the thread's");
		write_rw(0x5a5a1234);
	}

	/* Writing TPIDRURO from user code is undefined. */
	catch_signal(sigill, 4);
	asm volatile("mcr p15, 0, %0, c13, c0, 3" : : "r"(0x1234));
	release_signal(sigill);
	check_eq(caught.count, 1, "a write to TPIDRURO raises SIGILL");
	check_eq(read_ro(), 0x10203040, "and does not change it");

	shared_report = ptr<Report>(mmap_pages(4096, prot_read | prot_write, map_shared | map_anonymous));

	/* fork: the child starts with both, and has its own afterwards. */
	{
		const long child = fork_child();
		if (child == 0) {
			shared_report->ro_at_start = read_ro();
			shared_report->rw_at_start = read_rw();
			sys(nr_set_tls, 0x22223333);
			write_rw(0x44445555);
			shared_report->bad = hold(0x22223333, 0x44445555, 200);
			shared_report->ro_after = read_ro();
			shared_report->rw_after = read_rw();
			quit(0);
		}
		const long status = wait_child(child);
		check(exited_with(status, 0), "the forked child ran");
		check_eq(shared_report->ro_at_start, 0x10203040, "a forked child inherits TPIDRURO");
		check_eq(shared_report->rw_at_start, 0x5a5a1234, "a forked child inherits TPIDRURW");
		check_eq(shared_report->bad, 0, "the child keeps its own values across yields");
		check_eq(shared_report->ro_after, 0x22223333, "the child's TPIDRURO is its own");
		check_eq(read_ro(), 0x10203040, "the parent's TPIDRURO is unchanged");
		check_eq(read_rw(), 0x5a5a1234, "and its TPIDRURW");
	}

	/* Context switches between two runnable tasks. */
	{
		const long child = fork_child();
		if (child == 0) {
			sys(nr_set_tls, 0x0badc0de);
			write_rw(0x0cafe001);
			shared_report->bad = hold(0x0badc0de, 0x0cafe001, 400);
			quit(0);
		}
		const u32 mine = hold(0x10203040, 0x5a5a1234, 400);
		wait_child(child);
		check_eq(mine, 0, "two tasks switching in and out keep their own registers (parent)");
		check_eq(shared_report->bad, 0, "and (child)");
	}

	/* Threads: CLONE_SETTLS gives the thread its TPIDRURO; TPIDRURW starts as
	 * the creator's and is then the thread's. */
	{
		ThreadArgument threads[3];
		Thread handles[3];
		bool started = true;
		for (unsigned index = 0; index < 3; ++index) {
			threads[index] = {0x30000000U + index * 0x111, 0x60000000U + index * 0x222, 0, {}};
			started = started && thread_start(handles[index], thread_body, &threads[index],
							  threads[index].expect_ro);
		}
		check(started, "three threads start with CLONE_SETTLS");
		const u32 mine = hold(0x10203040, 0x5a5a1234, 300);
		bool joined = true;
		for (unsigned index = 0; index < 3; ++index)
			joined = joined && thread_join(handles[index]);
		check(joined, "the threads exit");
		check_eq(mine, 0, "the creator keeps its registers while the threads run");
		for (unsigned index = 0; index < 3; ++index) {
			const auto &report = threads[index].report;
			check_eq(report.ro_at_start, threads[index].expect_ro,
				 "a CLONE_SETTLS thread starts with the tls argument as TPIDRURO");
			check_eq(report.rw_at_start, 0x5a5a1234,
				 "and TPIDRURW as the creator had it");
			check_eq(report.bad, 0, "each thread keeps its own across yields");
			check_eq(report.rw_after, threads[index].write_rw, "TPIDRURW is the thread's");
		}
		check_eq(read_rw(), 0x5a5a1234, "the creator's TPIDRURW is unchanged by its threads");
	}
}

} // namespace probe
