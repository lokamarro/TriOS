// SPDX-License-Identifier: GPL-2.0-or-later
/* Group "signals": delivery and return through the sigpage and through
 * SA_RESTORER, the frame's registers and flags in A32 and T32, a fault inside
 * an IT block, redirecting the pc across instruction sets, restarting a
 * blocked system call from either instruction set, a T32 handler, nesting on
 * an alternate stack, and the signal mask. */
#include "engine_probe.hpp"

extern "C" {
void sig_restorer_rt();
void sig_restorer_old();
void sig_fault_arm(probe::u32 *out, void *bad);
void sig_fault_thumb(probe::u32 *out, void *bad);
extern const char sig_fault_arm_insn[];
extern const char sig_fault_thumb_insn[];
probe::u32 sig_it_fault(void *bad);
extern const char sig_it_fault_insn[];
long sig_jump_arm(void *bad);
long sig_jump_thumb(void *bad);
long sig_jump_target_arm();
long sig_jump_target_thumb();
long sig_read_arm(int fd, void *buffer, probe::u32 length);
long sig_read_thumb(int fd, void *buffer, probe::u32 length);
extern const char sig_read_arm_svc[];
extern const char sig_read_thumb_svc[];
long sig_tgkill_arm(long pid, long tid, long signal);
long sig_tgkill_thumb(long pid, long tid, long signal);
void sig_thumb_handler(int signal, probe::SigInfo *info, probe::UContext *context);
extern probe::u32 sig_thumb_seen[6];
}

namespace probe {
namespace {

constexpr u32 flag_mask = 0xf80f0000U;
constexpr u32 psr_thumb = 1U << 5;
constexpr word unmapped = 0x20;

/* What the last handler saw; asserted outside the handler. */
struct Seen {
	u32 count;
	i32 signo;
	i32 code;
	word address;
	word pc;
	word cpsr;
	word fault_address;
	word lr;
	word frame_alignment;
	u32 mask_has_signal;
	word r[11];
	word sp;
};
Seen seen;
/* How a handler treats its frame. */
word skip_bytes;
word resume_r5;
bool change_frame;
word redirect_pc;
bool redirect_thumb;
word altstack_low, altstack_high;
u32 order[8];
u32 order_count;

void record(int signo, SigInfo *info, UContext *context)
{
	auto &m = context->mcontext;
	++seen.count;
	seen.signo = signo;
	seen.code = info->code;
	seen.address = info->address;
	seen.pc = m.pc;
	seen.cpsr = m.cpsr;
	seen.fault_address = m.fault_address;
	seen.sp = m.sp;
	seen.frame_alignment = reinterpret_cast<word>(__builtin_frame_address(0)) & 7;
	/* The mask in force now: the signal blocks itself. */
	SigSet current{};
	sys(nr_rt_sigprocmask, 0, 0, addr(&current), 8);
	seen.mask_has_signal = (current.bits[(signo - 1) / 32] >> ((signo - 1) % 32)) & 1;
	const word *registers = &m.r0;
	for (unsigned index = 0; index < 11; ++index)
		seen.r[index] = registers[index];
}

[[gnu::noinline]] void fault_handler(int signo, SigInfo *info, UContext *context)
{
	record(signo, info, context);
	seen.lr = reinterpret_cast<word>(__builtin_return_address(0));
	auto &m = context->mcontext;
	if (change_frame) {
		m.r0 = 0x1234;
		m.r5 = 0xdead5555U;
		/* Z and V set, N, C and Q clear, GE = 0b0110. */
		m.cpsr = (m.cpsr & ~flag_mask) | 0x50060000U;
	}
	if (redirect_pc) {
		m.pc = redirect_pc;
		m.cpsr = redirect_thumb ? (m.cpsr | psr_thumb) : (m.cpsr & ~psr_thumb);
		return;
	}
	if (resume_r5)
		m.r5 = resume_r5;
	m.pc += skip_bytes;
}

long old_sigaction(int signal, word handler, word flags, word restorer)
{
	struct {
		word handler;
		u32 mask;
		word flags;
		word restorer;
	} action{handler, 0, flags | (restorer ? sa_restorer : 0), restorer};
	return sys(nr_sigaction, signal, addr(&action), 0);
}

void plain_handler(int signo, SigInfo *info, UContext *context)
{
	record(signo, info, context);
	seen.lr = reinterpret_cast<word>(__builtin_return_address(0));
}

void old_handler(int signo)
{
	++seen.count;
	seen.signo = signo;
}

void altstack_inner(int, SigInfo *, UContext *)
{
	order[order_count++] = 2;
	const word here = reinterpret_cast<word>(__builtin_frame_address(0));
	order[order_count++] = (here >= altstack_low && here < altstack_high) ? 3 : 9;
}

void altstack_outer(int, SigInfo *, UContext *)
{
	order[order_count++] = 1;
	raise_signal(sigusr2);
	order[order_count++] = 4;
}

void restart_handler(int signo, SigInfo *info, UContext *context)
{
	record(signo, info, context);
}

/* Reads one byte from a pipe in `reader`, interrupted by a one-shot timer, with
 * a child that writes later. Returns the read's result; 0xdead when the timer
 * fired before the read blocked and the attempt must be repeated. */
long blocked_read(long (*reader)(int, void *, u32), const char *svc_address,
		  word flags, bool *interrupted_inside, unsigned milliseconds)
{
	i32 fds[2];
	sys(nr_pipe2, addr(fds), 0);
	const long pid = fork_child();
	if (pid == 0) {
		sys(nr_close, fds[0]);
		sleep_ms(milliseconds * 4);
		sys(nr_write, fds[1], addr("x"), 1);
		quit(0);
	}
	sys(nr_close, fds[1]);
	seen = {};
	set_handler(sigalrm, restart_handler, sa_siginfo | flags);
	struct {
		long interval_seconds, interval_microseconds;
		long value_seconds, value_microseconds;
	} timer{0, 0, static_cast<long>(milliseconds / 1000),
		static_cast<long>(milliseconds % 1000) * 1000};
	sys(nr_setitimer, 0 /* ITIMER_REAL */, addr(&timer), 0);
	char byte = 0;
	const long result = reader(fds[0], &byte, 1);
	sys(nr_close, fds[0]);
	wait_child(pid);
	reset_handler(sigalrm);
	const word at = seen.pc;
	*interrupted_inside = seen.count == 1 &&
		(at == addr(svc_address) || at == addr(svc_address) + (svc_address == sig_read_arm_svc ? 4 : 2));
	if (result == 1 && byte != 'x')
		return -9999;
	return result;
}

} // namespace

void group_signals()
{
	const long pid = getpid();
	const long tid = gettid();

	/* Delivery through the sigpage: the return address is in [sigpage]. */
	word sigpage_start = 0, sigpage_end = 0;
	const bool have_sigpage = find_mapping("[sigpage]", &sigpage_start, &sigpage_end);
	if (!under_qemu())
		check(have_sigpage, "/proc/self/maps names a [sigpage] mapping");
	seen = {};
	check_eq(static_cast<u32>(set_handler(sigusr1, plain_handler)), 0, "rt_sigaction");
	check_eq(static_cast<u32>(raise_signal(sigusr1)), 0, "tgkill to self");
	check_eq(seen.count, 1, "a handler runs once");
	check_eq(static_cast<u32>(seen.signo), sigusr1, "the handler gets the signal number");
	check_eq(static_cast<u32>(seen.code), static_cast<u32>(-6), "si_code is SI_TKILL");
	check_eq(seen.address, static_cast<u32>(pid), "si_pid is the sender");
	if (!under_qemu())
		check(seen.lr >= sigpage_start && seen.lr < sigpage_end,
		      "the default return address is in [sigpage]");
	check_eq(seen.frame_alignment, 0, "the handler runs on an 8-byte aligned stack");
	check_eq(seen.mask_has_signal, 1, "the signal is masked in the handler");
	check_eq(seen.cpsr & 0x1f, 0x10, "the frame's mode is User");
	check_eq(seen.cpsr & psr_thumb, 0, "an A32 interruption saves T = 0");
	SigSet pending{};
	sys(nr_rt_sigprocmask, 0, 0, addr(&pending), 8);
	check_eq((pending.bits[0] >> (sigusr1 - 1)) & 1, 0, "the mask is restored after the handler");

	/* SA_RESTORER with a custom rt_sigreturn restorer. */
	seen = {};
	check_eq(static_cast<u32>(set_handler(sigusr1, plain_handler, sa_siginfo,
					      addr(sig_restorer_rt))), 0, "rt_sigaction with a restorer");
	raise_signal(sigusr1);
	check_eq(seen.count, 1, "a handler with SA_RESTORER runs");
	check_eq(seen.lr, addr(sig_restorer_rt), "its return address is the restorer");

	/* The old, non-RT sigaction and sigreturn. */
	seen = {};
	check_eq(static_cast<u32>(old_sigaction(sigusr1, addr(old_handler), 0,
						addr(sig_restorer_old))), 0, "sigaction with a restorer");
	raise_signal(sigusr1);
	check_eq(seen.count, 1, "a handler returning through sigreturn runs");
	check_eq(static_cast<u32>(seen.signo), sigusr1, "and gets the signal number");
	seen = {};
	old_sigaction(sigusr1, addr(old_handler), 0, 0);
	raise_signal(sigusr1);
	check_eq(seen.count, 1, "a non-RT handler with the default restorer runs");
	reset_handler(sigusr1);

	/* The frame's registers and flags, A32 and T32. */
	struct {
		void (*routine)(u32 *, void *);
		const char *insn;
		word skip;
		bool thumb;
		const char *name;
	} faults[] = {
		{sig_fault_arm, sig_fault_arm_insn, 4, false, "A32"},
		{sig_fault_thumb, sig_fault_thumb_insn, 2, true, "T32"},
	};
	for (auto &fault : faults) {
		seen = {};
		skip_bytes = fault.skip;
		change_frame = true;
		redirect_pc = 0;
		resume_r5 = 0;
		set_handler(sigsegv, fault_handler);
		u32 out[12] = {};
		fault.routine(out, ptr<void>(unmapped));
		reset_handler(sigsegv);
		change_frame = false;
		char name[96];
		auto label = [&](const char *text) {
			word length = 0;
			for (const char *p = fault.name; *p; ++p)
				name[length++] = *p;
			name[length++] = ':';
			name[length++] = ' ';
			for (const char *p = text; *p; ++p)
				name[length++] = *p;
			name[length] = 0;
			return static_cast<const char *>(name);
		};
		check_eq(seen.count, 1, label("a load from an unmapped address raises SIGSEGV"));
		check_eq(static_cast<u32>(seen.signo), sigsegv, label("the signal is SIGSEGV"));
		check_eq(static_cast<u32>(seen.code), 1, label("si_code is SEGV_MAPERR"));
		check_eq(seen.address, unmapped, label("si_addr is the address"));
		if (!under_qemu())
			check_eq(seen.fault_address, unmapped,
				 label("sigcontext.fault_address is the address"));
		check_eq(seen.pc, addr(fault.insn), label("the frame's pc is the faulting instruction"));
		check_eq((seen.cpsr & psr_thumb) != 0, fault.thumb, label("the frame's T bit"));
		check_eq(seen.cpsr & flag_mask, 0xa8090000U,
			 label("the frame has N, C, Q and GE = 0b1001"));
		bool registers_ok = true;
		for (unsigned index = 2; index <= 10; ++index)
			registers_ok = registers_ok && seen.r[index] == index * 0x11111111U;
		check(registers_ok, label("r2-r10 are in the frame"));
		check_eq(out[1], 0x1234, label("r0 as the handler changed it is restored"));
		check_eq(out[6], 0xdead5555U, label("r5 as the handler changed it is restored"));
		bool others_ok = out[3] == 0x22222222U && out[4] == 0x33333333U;
		for (unsigned index = 4; index <= 10; ++index)
			if (index != 5)
				others_ok = others_ok && out[index + 1] == index * 0x11111111U;
		check(others_ok, label("the registers the handler left alone are restored"));
		check_eq(out[0] & flag_mask, 0x50060000U,
			 label("the flags, Q and GE are restored from the frame"));
	}

	/* A fault in the first slot of an IT block, resumed. */
	{
		static u32 value = 0x77;
		seen = {};
		skip_bytes = 0;
		resume_r5 = addr(&value);
		change_frame = false;
		redirect_pc = 0;
		set_handler(sigsegv, fault_handler);
		const u32 result = sig_it_fault(ptr<void>(unmapped));
		reset_handler(sigsegv);
		resume_r5 = 0;
		check_eq(seen.count, 1, "an IT slot's load faults once");
		check_eq(seen.pc, addr(sig_it_fault_insn), "the frame's pc is the faulting slot");
		const u32 itstate = (((seen.cpsr >> 10) & 0x3f) << 2) | ((seen.cpsr >> 25) & 3);
		check_eq(itstate, 0x07, "the frame carries ITSTATE of ITTEE EQ at its first slot");
		check_eq(seen.cpsr & psr_thumb, psr_thumb, "the frame's T bit is set");
		check_eq(result >> 8, 0x77, "the load runs when resumed");
		check_eq(result & 0xff, 1, "the rest of the block runs under the saved IT state");
	}

	/* The handler redirects the pc across instruction sets. */
	for (int to_thumb = 0; to_thumb < 2; ++to_thumb) {
		seen = {};
		change_frame = false;
		resume_r5 = 0;
		skip_bytes = 0;
		redirect_thumb = to_thumb;
		redirect_pc = to_thumb ? (addr(sig_jump_target_thumb) & ~1U) :
					 addr(sig_jump_target_arm);
		set_handler(sigsegv, fault_handler);
		const long got = to_thumb ? sig_jump_arm(ptr<void>(unmapped)) :
					    sig_jump_thumb(ptr<void>(unmapped));
		reset_handler(sigsegv);
		redirect_pc = 0;
		check_eq(static_cast<u32>(got), to_thumb ? 0x54 : 0x41,
			 to_thumb ? "sigreturn into T32 from A32 code" : "sigreturn into A32 from T32 code");
	}

	/* A signal that interrupts a blocked read restarts it (SA_RESTART) or
	 * fails it with EINTR, from A32 and from T32. */
	struct {
		long (*reader)(int, void *, u32);
		const char *svc;
		const char *name;
	} readers[] = {
		{sig_read_arm, sig_read_arm_svc, "A32"},
		{sig_read_thumb, sig_read_thumb_svc, "T32"},
	};
	for (auto &reader : readers) {
		for (int restart = 1; restart >= 0; --restart) {
			bool inside = false;
			long result = 0;
			for (unsigned milliseconds = 60; milliseconds <= 960; milliseconds *= 2) {
				result = blocked_read(reader.reader, reader.svc,
						      restart ? sa_restart : 0, &inside, milliseconds);
				if (inside)
					break;
			}
			char name[80];
			word length = 0;
			for (const char *p = reader.name; *p; ++p)
				name[length++] = *p;
			const char *text = restart ? " read interrupted with SA_RESTART restarts" :
						     " read interrupted without SA_RESTART is EINTR";
			for (const char *p = text; *p; ++p)
				name[length++] = *p;
			name[length] = 0;
			check(inside, "the timer signal interrupted the blocked system call");
			check_eq(static_cast<u32>(result), restart ? 1U : static_cast<u32>(-eintr), name);
		}
	}

	/* A T32 handler, entered by an A32 and by a T32 interruption. */
	sig_thumb_seen[4] = 0;
	{
		KernelSigaction action{};
		action.handler = addr(sig_thumb_handler);
		action.flags = sa_siginfo;
		check_eq(static_cast<u32>(sys(nr_rt_sigaction, sigusr2, addr(&action), 0, 8)), 0,
			 "rt_sigaction with a T32 handler (bit 0 set)");
	}
	check_eq(static_cast<u32>(sig_tgkill_arm(pid, tid, sigusr2)), 0, "tgkill from A32");
	check_eq(sig_thumb_seen[4], 1, "a T32 handler runs when A32 code is interrupted");
	check_eq(sig_thumb_seen[0], sigusr2, "and gets the signal number");
	/* A T32 handler returns through the T32 trampoline of the sigpage: its
	 * address has bit 0 set so that the handler's bx lr stays in T32. */
	const u32 lr_thumb = sig_thumb_seen[3];
	if (!under_qemu())
		check(have_sigpage && (lr_thumb & ~1U) >= sigpage_start && (lr_thumb & ~1U) < sigpage_end,
		      "a T32 handler returns through the sigpage");
	check_eq(lr_thumb & 1, 1, "its return address names T32 (bit 0 set)");
	check_eq(sig_thumb_seen[5] & psr_thumb, 0, "the A32 interruption saved T = 0");
	check_eq(static_cast<u32>(sig_tgkill_thumb(pid, tid, sigusr2)), 0, "tgkill from T32");
	check_eq(sig_thumb_seen[4], 2, "a T32 handler runs when T32 code is interrupted");
	check_eq(sig_thumb_seen[5] & psr_thumb, psr_thumb, "the T32 interruption saved T = 1");
	reset_handler(sigusr2);

	/* Nested signals on an alternate stack. */
	{
		const word size = 1 << 16;
		const word stack = mmap_pages(size, prot_read | prot_write);
		altstack_low = stack;
		altstack_high = stack + size;
		StackT alternate{stack, 0, size};
		check_eq(static_cast<u32>(sys(186 /* sigaltstack */, addr(&alternate), 0)), 0, "sigaltstack");
		order_count = 0;
		set_handler(sigusr1, altstack_outer, sa_siginfo | sa_onstack);
		set_handler(sigusr2, altstack_inner, sa_siginfo | sa_onstack);
		raise_signal(sigusr1);
		check_eq(order_count, 4, "a signal raised in a handler is delivered");
		check(order[0] == 1 && order[1] == 2 && order[2] == 3 && order[3] == 4,
		      "the nested handler runs on the alternate stack, then the outer one finishes");
		StackT disabled{0, 4 /* SS_DISABLE */, 0};
		sys(186, addr(&disabled), 0);
		reset_handler(sigusr1);
		reset_handler(sigusr2);
		sys(nr_munmap, stack, size);
	}
}

} // namespace probe
