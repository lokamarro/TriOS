// SPDX-License-Identifier: GPL-2.0-or-later
/* Freestanding x86-64 and i386 Linux guest probe: what the x86_64-userspace
 * slice delivers for x86 exceptions and interrupts, and what a tracer of the
 * same ABI reads and writes, against native Linux x86.
 *
 * Only a system call's own return can be restarted: a signal taken between
 * two instructions leaves the accumulator alone even when it holds an
 * -ERESTART* code, and a tracer that rewrites orig_ax at a system call's entry
 * changes or skips that call.
 *
 * A repeated string instruction retires once however long it runs, and the
 * kernel services its timer only when the engine returns to it, so a loop of
 * long REP STOSB or REPE CMPSB must still see a 1 ms interval timer about as
 * often as ordinary code does.
 *
 * The x86-64 build, whose second argument is the path of the i386 build, runs
 * every case in a child of the matching ABI. Prints one line per case and
 * exits with the number of failures.
 */

namespace {

using u16 = unsigned short;
using u32 = unsigned int;
using u64 = unsigned long long;
using word = unsigned long;

#if defined(__x86_64__)
constexpr long nr_write = 1;
constexpr long nr_rt_sigaction = 13;
constexpr long nr_setitimer = 38;
constexpr long nr_getpid = 39;
constexpr long nr_fork = 57;
constexpr long nr_execve = 59;
constexpr long nr_wait4 = 61;
constexpr long nr_kill = 62;
constexpr long nr_ptrace = 101;
constexpr long nr_getppid = 110;
constexpr long nr_sigaltstack = 131;
constexpr long nr_sched_setaffinity = 203;
constexpr long nr_clock_gettime = 228;
constexpr long nr_exit_group = 231;

inline long raw_syscall(long number, long a0 = 0, long a1 = 0, long a2 = 0,
			long a3 = 0)
{
	long result;
	register long r10 __asm__("r10") = a3;
	__asm__ volatile("syscall"
			 : "=a"(result)
			 : "a"(number), "D"(a0), "S"(a1), "d"(a2), "r"(r10)
			 : "rcx", "r11", "memory", "cc");
	return result;
}

__asm__(".text\n"
	".global probe_restorer\n"
	".type probe_restorer,@function\n"
	"probe_restorer:\n"
	"	mov $15, %eax\n"
	"	syscall\n"
	"	hlt\n"
	".size probe_restorer,.-probe_restorer\n"
	".global _start\n"
	".type _start,@function\n"
	"_start:\n"
	"	xor %ebp, %ebp\n"
	"	mov %rsp, %rdi\n"
	"	and $-16, %rsp\n"
	"	call probe_main\n"
	"	mov %eax, %edi\n"
	"	mov $231, %eax\n"
	"	syscall\n"
	"	hlt\n"
	".size _start,.-_start\n"
	/* REP STOSB of zero: (buffer, count). */
	".global probe_stos\n"
	"probe_stos:\n"
	"	movq %rsi, %rcx\n"
	"	xorl %eax, %eax\n"
	"	rep stosb\n"
	"	ret\n"
	/* REPE CMPSB: (left, right, count), returns what RCX kept. */
	".global probe_cmps\n"
	"probe_cmps:\n"
	"	movq %rdx, %rcx\n"
	"	repe cmpsb\n"
	"	movq %rcx, %rax\n"
	"	ret\n"
	/* ICEBP, INT 4: each traps after itself. */
	".global probe_icebp\n"
	"probe_icebp:\n"
	"	.byte 0xf1\n"
	".global probe_icebp_end\n"
	"probe_icebp_end:\n"
	"	ret\n"
	".global probe_int4\n"
	"probe_int4:\n"
	"	int $4\n"
	".global probe_int4_end\n"
	"probe_int4_end:\n"
	"	ret\n"
	/* Spins (loops) times in 2-byte instructions with (value) in RAX and
	 * returns RAX. A signal that wrongly restarted a system call here would
	 * move RIP back one whole instruction. */
	".global probe_hold_ax\n"
	"probe_hold_ax:\n"
	"	movq %rdi, %rax\n"
	"	movq %rsi, %rcx\n"
	".Lprobe_hold_ax_loop:\n"
	"	.byte 0x66, 0x90\n"
	"	.byte 0xff, 0xc9\n"
	"	jnz .Lprobe_hold_ax_loop\n"
	"	ret\n"
	/* Stores pi through a SIGTRAP taken with it on the x87 stack: (out). */
	".global probe_x87_signal\n"
	"probe_x87_signal:\n"
	"	fninit\n"
	"	fldpi\n"
	"	int3\n"
	"	fstpl (%rdi)\n"
	"	ret\n"
	/* One access at (address): a read, a write, and a fetch. */
	".global probe_pf_read\n"
	"probe_pf_read:\n"
	".global probe_pf_read_insn\n"
	"probe_pf_read_insn:\n"
	"	movl (%rdi), %eax\n"
	".global probe_pf_read_end\n"
	"probe_pf_read_end:\n"
	"	ret\n"
	".global probe_pf_write\n"
	"probe_pf_write:\n"
	".global probe_pf_write_insn\n"
	"probe_pf_write_insn:\n"
	"	movl %eax, (%rdi)\n"
	".global probe_pf_write_end\n"
	"probe_pf_write_end:\n"
	"	ret\n"
	".global probe_pf_fetch\n"
	"probe_pf_fetch:\n"
	"	jmp *%rdi\n"
	".global probe_pf_fetch_end\n"
	"probe_pf_fetch_end:\n"
	"	ret\n"
	/* Returns to (address) after a return of its own, so the second
	 * return finds no call of this dispatch to match. */
	".global probe_pf_return\n"
	"probe_pf_return:\n"
	"	leaq 1f(%rip), %rax\n"
	"	pushq %rax\n"
	"	ret\n"
	"1:	pushq %rdi\n"
	"	ret\n"
	".global probe_pf_return_end\n"
	"probe_pf_return_end:\n"
	"	ret\n"
	/* One access through a non-canonical (address): a load and a store
	 * through RAX, a load based on RBP and one indexed by it, MOVSB from
	 * it, and a push with it as RSP. */
	".global probe_nc_load\n"
	"probe_nc_load:\n"
	"	movq %rdi, %rax\n"
	".global probe_nc_load_insn\n"
	"probe_nc_load_insn:\n"
	"	movq (%rax), %rax\n"
	".global probe_nc_load_end\n"
	"probe_nc_load_end:\n"
	"	ret\n"
	".global probe_nc_store\n"
	"probe_nc_store:\n"
	"	movq %rdi, %rax\n"
	".global probe_nc_store_insn\n"
	"probe_nc_store_insn:\n"
	"	movq %rcx, (%rax)\n"
	".global probe_nc_store_end\n"
	"probe_nc_store_end:\n"
	"	ret\n"
	".global probe_nc_rbp\n"
	"probe_nc_rbp:\n"
	"	pushq %rbp\n"
	"	movq %rdi, %rbp\n"
	".global probe_nc_rbp_insn\n"
	"probe_nc_rbp_insn:\n"
	"	movq 8(%rbp), %rax\n"
	".global probe_nc_rbp_end\n"
	"probe_nc_rbp_end:\n"
	"	popq %rbp\n"
	"	ret\n"
	".global probe_nc_rbp_index\n"
	"probe_nc_rbp_index:\n"
	"	pushq %rbp\n"
	"	movq %rdi, %rbp\n"
	"	xorl %eax, %eax\n"
	".global probe_nc_rbp_index_insn\n"
	"probe_nc_rbp_index_insn:\n"
	"	movq (%rax,%rbp,1), %rax\n"
	".global probe_nc_rbp_index_end\n"
	"probe_nc_rbp_index_end:\n"
	"	popq %rbp\n"
	"	ret\n"
	".global probe_nc_movs\n"
	"probe_nc_movs:\n"
	"	movq %rdi, %rsi\n"
	"	leaq probe_nc_scratch(%rip), %rdi\n"
	".global probe_nc_movs_insn\n"
	"probe_nc_movs_insn:\n"
	"	movsb\n"
	".global probe_nc_movs_end\n"
	"probe_nc_movs_end:\n"
	"	ret\n"
	".global probe_nc_push\n"
	"probe_nc_push:\n"
	"	movq %rsp, probe_nc_saved_sp(%rip)\n"
	"	movq %rdi, %rsp\n"
	".global probe_nc_push_insn\n"
	"probe_nc_push_insn:\n"
	"	pushq %rax\n"
	".global probe_nc_push_end\n"
	"probe_nc_push_end:\n"
	"	movq probe_nc_saved_sp(%rip), %rsp\n"
	"	ret\n"
	/* (count) getpid calls with (stack pointer) as RSP; returns the last
	 * result. */
	".global probe_getpid_on\n"
	"probe_getpid_on:\n"
	"	movq %rsp, probe_nc_saved_sp(%rip)\n"
	"	movq %rdi, %rsp\n"
	"1:	movl $39, %eax\n"
	"	syscall\n"
	"	decq %rsi\n"
	"	jnz 1b\n"
	"	movq probe_nc_saved_sp(%rip), %rsp\n"
	"	ret\n"
	".data\n"
	".balign 8\n"
	"probe_nc_saved_sp:\n"
	"	.quad 0\n"
	"probe_nc_scratch:\n"
	"	.quad 0\n"
	".text\n"
	/* REPE CMPSB of five bytes that differ at the third, then REP STOSB
	 * of none: (left, right). */
	".global probe_rep_step\n"
	"probe_rep_step:\n"
	"	movl $5, %ecx\n"
	".global probe_rep_step_insn\n"
	"probe_rep_step_insn:\n"
	"	repe cmpsb\n"
	".global probe_rep_step_end\n"
	"probe_rep_step_end:\n"
	"	xorl %ecx, %ecx\n"
	".global probe_rep_zero_insn\n"
	"probe_rep_zero_insn:\n"
	"	rep stosb\n"
	".global probe_rep_zero_end\n"
	"probe_rep_zero_end:\n"
	"	ret\n"
	/* The traced child: known x87, SSE and general register state, then
	 * #UD for the tracer's stop. The tracer resumes it at
	 * probe_traced_resume, which exits with RBX's low byte. */
	".global probe_traced\n"
	"probe_traced:\n"
	"	fninit\n"
	"	fldcw probe_control_word(%rip)\n"
	"	ldmxcsr probe_mxcsr_value(%rip)\n"
	"	fld1\n"
	"	movabs $0x1122334455667788, %rbx\n"
	".global probe_traced_stop\n"
	"probe_traced_stop:\n"
	"	ud2\n"
	".global probe_traced_resume\n"
	"probe_traced_resume:\n"
	"	mov %ebx, %edi\n"
	"	mov $231, %eax\n"
	"	syscall\n"
	"	hlt\n"
	".section .rodata\n"
	"probe_control_word:\n"
	"	.short 0x027f\n"
	"	.balign 4\n"
	"probe_mxcsr_value:\n"
	"	.long 0x3f80\n"
	".text\n");

/* uc_mcontext: uc_flags, uc_link and a 24-byte stack_t, then r8-r15, rdi,
 * rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags, csgsfs, err and trapno. */
constexpr unsigned frame_di_offset = 40 + 8 * 8;
constexpr unsigned frame_cx_offset = 40 + 14 * 8;
constexpr unsigned frame_ip_offset = 40 + 16 * 8;
constexpr unsigned frame_flags_offset = 40 + 17 * 8;
constexpr unsigned frame_error_offset = 40 + 19 * 8;
constexpr unsigned frame_trapno_offset = 40 + 20 * 8;
constexpr unsigned siginfo_address_offset = 16;

/* struct user_regs_struct and struct user. */
constexpr unsigned user_regs_words = 27;
constexpr unsigned user_bx = 5;
constexpr unsigned user_cx = 11;
constexpr unsigned user_ax = 10;
constexpr unsigned user_orig_ax = 15;
constexpr unsigned user_ip = 16;
constexpr unsigned user_cs = 17;
constexpr unsigned user_flags = 18;
constexpr unsigned user_ss = 20;
constexpr word user_debugreg7 = 848 + 7 * 8;
/* sizeof(struct user): PTRACE_PEEKUSER rejects it and beyond. */
constexpr word user_past = 928;
constexpr word expected_bx = 0x1122334455667788UL;
constexpr word expected_cs = 0x33;
constexpr word user_debugreg = 848;
#elif defined(__i386__)
constexpr long nr_fork = 2;
constexpr long nr_write = 4;
constexpr long nr_getpid = 20;
constexpr long nr_ptrace = 26;
constexpr long nr_kill = 37;
constexpr long nr_getppid = 64;
constexpr long nr_setitimer = 104;
constexpr long nr_wait4 = 114;
constexpr long nr_rt_sigaction = 174;
constexpr long nr_sched_setaffinity = 241;
constexpr long nr_set_thread_area = 243;
constexpr long nr_get_thread_area = 244;
constexpr long nr_exit_group = 252;
constexpr long nr_clock_gettime = 265;

inline long raw_syscall(long number, long a0 = 0, long a1 = 0, long a2 = 0,
			long a3 = 0)
{
	long result;
	__asm__ volatile("int $0x80"
			 : "=a"(result)
			 : "a"(number), "b"(a0), "c"(a1), "d"(a2), "S"(a3)
			 : "memory", "cc");
	return result;
}

__asm__(".text\n"
	".global probe_restorer\n"
	".type probe_restorer,@function\n"
	"probe_restorer:\n"
	"	mov $173, %eax\n"
	"	int $0x80\n"
	"	hlt\n"
	".size probe_restorer,.-probe_restorer\n"
	".global _start\n"
	".type _start,@function\n"
	"_start:\n"
	"	xor %ebp, %ebp\n"
	"	mov %esp, %eax\n"
	"	and $-16, %esp\n"
	"	sub $12, %esp\n"
	"	push %eax\n"
	"	call probe_main\n"
	"	mov %eax, %ebx\n"
	"	mov $252, %eax\n"
	"	int $0x80\n"
	"	hlt\n"
	".size _start,.-_start\n"
	".global probe_stos\n"
	"probe_stos:\n"
	"	pushl %edi\n"
	"	movl 8(%esp), %edi\n"
	"	movl 12(%esp), %ecx\n"
	"	xorl %eax, %eax\n"
	"	rep stosb\n"
	"	popl %edi\n"
	"	ret\n"
	".global probe_cmps\n"
	"probe_cmps:\n"
	"	pushl %esi\n"
	"	pushl %edi\n"
	"	movl 12(%esp), %esi\n"
	"	movl 16(%esp), %edi\n"
	"	movl 20(%esp), %ecx\n"
	"	repe cmpsb\n"
	"	movl %ecx, %eax\n"
	"	popl %edi\n"
	"	popl %esi\n"
	"	ret\n"
	".global probe_icebp\n"
	"probe_icebp:\n"
	"	.byte 0xf1\n"
	".global probe_icebp_end\n"
	"probe_icebp_end:\n"
	"	ret\n"
	".global probe_int4\n"
	"probe_int4:\n"
	"	int $4\n"
	".global probe_int4_end\n"
	"probe_int4_end:\n"
	"	ret\n"
	".global probe_hold_ax\n"
	"probe_hold_ax:\n"
	"	movl 4(%esp), %eax\n"
	"	movl 8(%esp), %ecx\n"
	".Lprobe_hold_ax_loop:\n"
	"	.byte 0x66, 0x90\n"
	"	.byte 0xff, 0xc9\n"
	"	jnz .Lprobe_hold_ax_loop\n"
	"	ret\n"
	/* INTO after adding (value) to 0x7fffffff: overflows for 1, not 0. */
	".global probe_into\n"
	"probe_into:\n"
	"	movl 4(%esp), %eax\n"
	"	addl $0x7fffffff, %eax\n"
	".global probe_into_insn\n"
	"probe_into_insn:\n"
	"	into\n"
	".global probe_into_end\n"
	"probe_into_end:\n"
	"	ret\n"
	/* BOUND EAX, [EDX] and BOUND AX, [EDX]: (index, bounds). */
	".global probe_bound32\n"
	"probe_bound32:\n"
	"	movl 4(%esp), %eax\n"
	"	movl 8(%esp), %edx\n"
	".global probe_bound32_insn\n"
	"probe_bound32_insn:\n"
	"	.byte 0x62, 0x02\n"
	".global probe_bound32_end\n"
	"probe_bound32_end:\n"
	"	ret\n"
	".global probe_bound16\n"
	"probe_bound16:\n"
	"	movl 4(%esp), %eax\n"
	"	movl 8(%esp), %edx\n"
	".global probe_bound16_insn\n"
	"probe_bound16_insn:\n"
	"	.byte 0x66, 0x62, 0x02\n"
	".global probe_bound16_end\n"
	"probe_bound16_end:\n"
	"	ret\n"
	".global probe_x87_signal\n"
	"probe_x87_signal:\n"
	"	movl 4(%esp), %eax\n"
	"	fninit\n"
	"	fldpi\n"
	"	int3\n"
	"	fstpl (%eax)\n"
	"	ret\n"
	".global probe_pf_read\n"
	"probe_pf_read:\n"
	"	movl 4(%esp), %edx\n"
	".global probe_pf_read_insn\n"
	"probe_pf_read_insn:\n"
	"	movl (%edx), %eax\n"
	".global probe_pf_read_end\n"
	"probe_pf_read_end:\n"
	"	ret\n"
	".global probe_pf_write\n"
	"probe_pf_write:\n"
	"	movl 4(%esp), %edx\n"
	".global probe_pf_write_insn\n"
	"probe_pf_write_insn:\n"
	"	movl %eax, (%edx)\n"
	".global probe_pf_write_end\n"
	"probe_pf_write_end:\n"
	"	ret\n"
	".global probe_pf_fetch\n"
	"probe_pf_fetch:\n"
	"	movl 4(%esp), %edx\n"
	"	jmp *%edx\n"
	".global probe_pf_fetch_end\n"
	"probe_pf_fetch_end:\n"
	"	ret\n"
	".global probe_pf_return\n"
	"probe_pf_return:\n"
	"	movl 4(%esp), %edx\n"
	"	pushl $1f\n"
	"	ret\n"
	"1:	pushl %edx\n"
	"	ret\n"
	".global probe_pf_return_end\n"
	"probe_pf_return_end:\n"
	"	ret\n"
	/* Loads the dword at (base) + (index) * 4. */
	".global probe_wrap_index\n"
	"probe_wrap_index:\n"
	"	movl 4(%esp), %eax\n"
	"	movl 8(%esp), %ecx\n"
	".global probe_wrap_index_insn\n"
	"probe_wrap_index_insn:\n"
	"	movl (%eax,%ecx,4), %eax\n"
	".global probe_wrap_index_end\n"
	"probe_wrap_index_end:\n"
	"	ret\n"
	/* Increments the dword at (base) + (index) and returns it. */
	".global probe_wrap_sum\n"
	"probe_wrap_sum:\n"
	"	movl 4(%esp), %eax\n"
	"	movl 8(%esp), %ecx\n"
	"	incl (%eax,%ecx)\n"
	"	movl (%eax,%ecx), %eax\n"
	"	ret\n"
	/* Stores (value) at 0xe0000014 and returns it plus twice the dword at
	 * 0xe0000010, through the absolute, based and offset forms. */
	".global probe_wrap_absolute\n"
	"probe_wrap_absolute:\n"
	"	movl 4(%esp), %ecx\n"
	"	movl %ecx, 0xe0000014\n"
	"	movl 0xe0000014, %edx\n"
	"	xorl %eax, %eax\n"
	"	addl 0xe0000010(%eax), %edx\n"
	"	movl 0xe0000010, %eax\n"
	"	addl %edx, %eax\n"
	"	ret\n"
	".global probe_wrap_load\n"
	"probe_wrap_load:\n"
	"	movl 4(%esp), %eax\n"
	".global probe_wrap_load_insn\n"
	"probe_wrap_load_insn:\n"
	"	movl -8(%eax), %eax\n"
	".global probe_wrap_load_end\n"
	"probe_wrap_load_end:\n"
	"	ret\n"
	/* Returns the dwords LODSL and an ordinary load read and the byte
	 * XLATB reads at GS:(offset), summed, with GS's base 0xfffff000. */
	".global probe_wrap_gs\n"
	"probe_wrap_gs:\n"
	"	pushl %esi\n"
	"	pushl %ebx\n"
	"	movl 12(%esp), %esi\n"
	"	movl %esi, %ebx\n"
	"	lodsl %gs:(%esi), %eax\n"
	"	movl %eax, %edx\n"
	"	xorl %eax, %eax\n"
	"	.byte 0x65, 0xd7\n" /* XLATB through GS */
	"	addl %eax, %edx\n"
	"	movl 12(%esp), %eax\n"
	"	addl %gs:(%eax), %edx\n"
	"	movl %edx, %eax\n"
	"	popl %ebx\n"
	"	popl %esi\n"
	"	ret\n"
	/* A push with ESP 0. */
	".global probe_wrap_push\n"
	"probe_wrap_push:\n"
	"	movl %esp, wrap_saved_sp\n"
	"	xorl %esp, %esp\n"
	".global probe_wrap_push_insn\n"
	"probe_wrap_push_insn:\n"
	"	pushl $1\n"
	".global probe_wrap_push_end\n"
	"probe_wrap_push_end:\n"
	"	movl wrap_saved_sp, %esp\n"
	"	ret\n"
	/* (count) getpid calls with (stack pointer) as ESP; returns the last
	 * result. */
	".global probe_getpid_on\n"
	"probe_getpid_on:\n"
	"	movl 4(%esp), %edx\n"
	"	movl 8(%esp), %ecx\n"
	"	movl %esp, wrap_saved_sp\n"
	"	movl %edx, %esp\n"
	"1:	movl $20, %eax\n"
	"	int $0x80\n"
	"	decl %ecx\n"
	"	jnz 1b\n"
	"	movl wrap_saved_sp, %esp\n"
	"	ret\n"
	/* REP STOSB of 4 bytes at (address). */
	".global probe_wrap_stos\n"
	"probe_wrap_stos:\n"
	"	pushl %edi\n"
	"	movl 8(%esp), %edi\n"
	"	movl $4, %ecx\n"
	".global probe_wrap_stos_insn\n"
	"probe_wrap_stos_insn:\n"
	"	rep stosb\n"
	".global probe_wrap_stos_end\n"
	"probe_wrap_stos_end:\n"
	"	popl %edi\n"
	"	ret\n"
	/* Reads the word at GS:0 after loading GS with (selector). */
	".global probe_gs_word\n"
	"probe_gs_word:\n"
	"	movl 4(%esp), %eax\n"
	"	movw %ax, %gs\n"
	"	movl %gs:0, %eax\n"
	"	ret\n"
	".global probe_rep_step\n"
	"probe_rep_step:\n"
	"	pushl %esi\n"
	"	pushl %edi\n"
	"	movl 12(%esp), %esi\n"
	"	movl 16(%esp), %edi\n"
	"	movl $5, %ecx\n"
	".global probe_rep_step_insn\n"
	"probe_rep_step_insn:\n"
	"	repe cmpsb\n"
	".global probe_rep_step_end\n"
	"probe_rep_step_end:\n"
	"	xorl %ecx, %ecx\n"
	".global probe_rep_zero_insn\n"
	"probe_rep_zero_insn:\n"
	"	rep stosb\n"
	".global probe_rep_zero_end\n"
	"probe_rep_zero_end:\n"
	"	popl %edi\n"
	"	popl %esi\n"
	"	ret\n"
	/* (tls_entry), which the tracer reads from ESI. */
	".global probe_traced\n"
	"probe_traced:\n"
	"	movl 4(%esp), %esi\n"
	"	fninit\n"
	"	fldcw probe_control_word\n"
	"	ldmxcsr probe_mxcsr_value\n"
	"	fld1\n"
	"	movl $0x11223344, %ebx\n"
	".global probe_traced_stop\n"
	"probe_traced_stop:\n"
	"	ud2\n"
	".global probe_traced_resume\n"
	"probe_traced_resume:\n"
	"	movl $252, %eax\n"
	"	int $0x80\n"
	"	hlt\n"
	".section .rodata\n"
	"probe_control_word:\n"
	"	.short 0x027f\n"
	"	.balign 4\n"
	"probe_mxcsr_value:\n"
	"	.long 0x3f80\n"
	".text\n");

/* uc_mcontext: uc_flags, uc_link and a 12-byte stack_t, then gs, fs, es, ds,
 * edi, esi, ebp, esp, ebx, edx, ecx, eax, trapno, err, eip, cs and eflags. */
constexpr unsigned frame_di_offset = 20 + 4 * 4;
constexpr unsigned frame_cx_offset = 20 + 10 * 4;
constexpr unsigned frame_trapno_offset = 20 + 12 * 4;
constexpr unsigned frame_error_offset = 20 + 13 * 4;
constexpr unsigned frame_ip_offset = 20 + 14 * 4;
constexpr unsigned frame_flags_offset = 20 + 16 * 4;
constexpr unsigned siginfo_address_offset = 12;

/* struct user_regs_struct and struct user of i386. */
constexpr unsigned user_regs_words = 17;
constexpr unsigned user_bx = 0;
constexpr unsigned user_si = 3;
constexpr unsigned user_cx = 1;
constexpr unsigned user_ax = 6;
constexpr unsigned user_orig_ax = 11;
constexpr unsigned user_ip = 12;
constexpr unsigned user_cs = 13;
constexpr unsigned user_flags = 14;
constexpr unsigned user_ss = 16;
constexpr word user_debugreg7 = 252 + 7 * 4;
/* Linux accepts sizeof(struct user32), 284, itself and rejects beyond. */
constexpr word user_past = 288;
constexpr word expected_bx = 0x11223344UL;
constexpr word expected_cs = 0x23;
constexpr word user_debugreg = 252;
#else
#error "the exception probe is an x86-64 or i386 program"
#endif

#if defined(__x86_64__)
#define PROBE_RIP "(%rip)"
#define PROBE_SP "%rsp"
#define PROBE_BP "%rbp"
#define PROBE_ARG_AX "	movq %rdi, %rax\n"
#define PROBE_ARG_CX_AX "	movq %rdi, %rcx\n	movl %esi, %eax\n"
#define PROBE_AX "%rax"
#define PROBE_CX "%rcx"
#else
#define PROBE_RIP ""
#define PROBE_SP "%esp"
#define PROBE_BP "%ebp"
#define PROBE_ARG_AX "	movl 4(%esp), %eax\n"
#define PROBE_ARG_CX_AX "	movl 4(%esp), %ecx\n	movl 8(%esp), %eax\n"
#define PROBE_AX "%eax"
#define PROBE_CX "%ecx"
#endif

/* An unmasked SIMD exception raises #XM at its instruction; an unmasked x87
 * one raises #MF at the next waiting instruction. Each case leaves MXCSR or
 * the x87 status word as the handler returned them in probe_fp_saved[0],
 * and XMM0 or the FISTP destination in probe_fp_saved[1]. */
/* An x87 instruction that needs a free register, run on a full stack of
 * seven 1.0 and pi on top with invalid operation unmasked. The overflow it
 * raises is checked before it computes anything: nothing is pushed, and the
 * waiting instruction after it takes #MF. pi, stored as a single, shows the
 * stack as the handler left it. */
#define PROBE_MF_FULL_STACK(name, insn) \
	".global probe_mf_" name "\n" \
	"probe_mf_" name ":\n" \
	"	fninit\n" \
	"	movw $0x37e, probe_fp_scratch" PROBE_RIP "\n" \
	"	fldcw probe_fp_scratch" PROBE_RIP "\n" \
	"	fld1\n	fld1\n	fld1\n	fld1\n	fld1\n	fld1\n	fld1\n" \
	"	fldpi\n" \
	"	" insn "\n" \
	".global probe_mf_" name "_insn\n" \
	"probe_mf_" name "_insn:\n" \
	"	fwait\n" \
	".global probe_mf_" name "_end\n" \
	"probe_mf_" name "_end:\n" \
	"	fnstsw probe_fp_saved" PROBE_RIP "\n" \
	"	fnclex\n" \
	"	fstps probe_fp_saved+4" PROBE_RIP "\n" \
	"	fninit\n" \
	"	ret\n"

__asm__(".text\n"
	".global probe_xm_div\n"
	"probe_xm_div:\n"
	"	movl $0x1d80, probe_fp_scratch" PROBE_RIP "\n"
	"	ldmxcsr probe_fp_scratch" PROBE_RIP "\n"
	"	movl $0x3f800000, %eax\n"
	"	movd %eax, %xmm0\n"
	"	xorps %xmm1, %xmm1\n"
	".global probe_xm_div_insn\n"
	"probe_xm_div_insn:\n"
	"	divss %xmm1, %xmm0\n"
	".global probe_xm_div_end\n"
	"probe_xm_div_end:\n"
	"	stmxcsr probe_fp_saved" PROBE_RIP "\n"
	"	movd %xmm0, %eax\n"
	"	movl %eax, probe_fp_saved+4" PROBE_RIP "\n"
	"	movl $0x1f80, probe_fp_scratch" PROBE_RIP "\n"
	"	ldmxcsr probe_fp_scratch" PROBE_RIP "\n"
	"	ret\n"
	".global probe_xm_over\n"
	"probe_xm_over:\n"
	"	movl $0x1b80, probe_fp_scratch" PROBE_RIP "\n"
	"	ldmxcsr probe_fp_scratch" PROBE_RIP "\n"
	"	movl $0x7f7fffff, %eax\n"
	"	movd %eax, %xmm0\n"
	"	movaps %xmm0, %xmm1\n"
	".global probe_xm_over_insn\n"
	"probe_xm_over_insn:\n"
	"	mulss %xmm1, %xmm0\n"
	".global probe_xm_over_end\n"
	"probe_xm_over_end:\n"
	"	stmxcsr probe_fp_saved" PROBE_RIP "\n"
	"	movd %xmm0, %eax\n"
	"	movl %eax, probe_fp_saved+4" PROBE_RIP "\n"
	"	movl $0x1f80, probe_fp_scratch" PROBE_RIP "\n"
	"	ldmxcsr probe_fp_scratch" PROBE_RIP "\n"
	"	ret\n"
	".global probe_xm_under\n"
	"probe_xm_under:\n"
	"	movl $0x1780, probe_fp_scratch" PROBE_RIP "\n"
	"	ldmxcsr probe_fp_scratch" PROBE_RIP "\n"
	"	movl $0x00800001, %eax\n"
	"	movd %eax, %xmm0\n"
	"	movl $0x3f000000, %eax\n"
	"	movd %eax, %xmm1\n"
	".global probe_xm_under_insn\n"
	"probe_xm_under_insn:\n"
	"	mulss %xmm1, %xmm0\n"
	".global probe_xm_under_end\n"
	"probe_xm_under_end:\n"
	"	stmxcsr probe_fp_saved" PROBE_RIP "\n"
	"	movd %xmm0, %eax\n"
	"	movl %eax, probe_fp_saved+4" PROBE_RIP "\n"
	"	movl $0x1f80, probe_fp_scratch" PROBE_RIP "\n"
	"	ldmxcsr probe_fp_scratch" PROBE_RIP "\n"
	"	ret\n"
	".global probe_xm_prec\n"
	"probe_xm_prec:\n"
	"	movl $0x0f80, probe_fp_scratch" PROBE_RIP "\n"
	"	ldmxcsr probe_fp_scratch" PROBE_RIP "\n"
	"	movl $0x3f800000, %eax\n"
	"	movd %eax, %xmm0\n"
	"	movl $0x40400000, %eax\n"
	"	movd %eax, %xmm1\n"
	".global probe_xm_prec_insn\n"
	"probe_xm_prec_insn:\n"
	"	divss %xmm1, %xmm0\n"
	".global probe_xm_prec_end\n"
	"probe_xm_prec_end:\n"
	"	stmxcsr probe_fp_saved" PROBE_RIP "\n"
	"	movd %xmm0, %eax\n"
	"	movl %eax, probe_fp_saved+4" PROBE_RIP "\n"
	"	movl $0x1f80, probe_fp_scratch" PROBE_RIP "\n"
	"	ldmxcsr probe_fp_scratch" PROBE_RIP "\n"
	"	ret\n"
	".global probe_xm_inv\n"
	"probe_xm_inv:\n"
	"	movl $0x1f00, probe_fp_scratch" PROBE_RIP "\n"
	"	ldmxcsr probe_fp_scratch" PROBE_RIP "\n"
	"	movl $0xbf800000, %eax\n"
	"	movd %eax, %xmm1\n"
	"	movl $0x3f800000, %eax\n"
	"	movd %eax, %xmm0\n"
	".global probe_xm_inv_insn\n"
	"probe_xm_inv_insn:\n"
	"	sqrtss %xmm1, %xmm0\n"
	".global probe_xm_inv_end\n"
	"probe_xm_inv_end:\n"
	"	stmxcsr probe_fp_saved" PROBE_RIP "\n"
	"	movd %xmm0, %eax\n"
	"	movl %eax, probe_fp_saved+4" PROBE_RIP "\n"
	"	movl $0x1f80, probe_fp_scratch" PROBE_RIP "\n"
	"	ldmxcsr probe_fp_scratch" PROBE_RIP "\n"
	"	ret\n"
	".global probe_xm_masked\n"
	"probe_xm_masked:\n"
	"	movl $0x0f80, probe_fp_scratch" PROBE_RIP "\n"
	"	ldmxcsr probe_fp_scratch" PROBE_RIP "\n"
	"	movl $0x3f800000, %eax\n"
	"	movd %eax, %xmm0\n"
	"	xorps %xmm1, %xmm1\n"
	".global probe_xm_masked_insn\n"
	"probe_xm_masked_insn:\n"
	"	divss %xmm1, %xmm0\n"
	".global probe_xm_masked_end\n"
	"probe_xm_masked_end:\n"
	"	stmxcsr probe_fp_saved" PROBE_RIP "\n"
	"	movd %xmm0, %eax\n"
	"	movl %eax, probe_fp_saved+4" PROBE_RIP "\n"
	"	movl $0x1f80, probe_fp_scratch" PROBE_RIP "\n"
	"	ldmxcsr probe_fp_scratch" PROBE_RIP "\n"
	"	ret\n"
	".global probe_mf_div\n"
	"probe_mf_div:\n"
	"	fninit\n"
	"	movw $0x37b, probe_fp_scratch" PROBE_RIP "\n"
	"	fldcw probe_fp_scratch" PROBE_RIP "\n"
	"	fldz\n"
	"	fld1\n"
	"	fdivp\n"
	".global probe_mf_div_insn\n"
	"probe_mf_div_insn:\n"
	"	fwait\n"
	".global probe_mf_div_end\n"
	"probe_mf_div_end:\n"
	"	fnstsw probe_fp_saved" PROBE_RIP "\n"
	"	fninit\n"
	"	ret\n"
	".global probe_mf_inv\n"
	"probe_mf_inv:\n"
	"	fninit\n"
	"	movw $0x37e, probe_fp_scratch" PROBE_RIP "\n"
	"	fldcw probe_fp_scratch" PROBE_RIP "\n"
	"	fld1\n"
	"	fchs\n"
	"	fsqrt\n"
	".global probe_mf_inv_insn\n"
	"probe_mf_inv_insn:\n"
	"	fwait\n"
	".global probe_mf_inv_end\n"
	"probe_mf_inv_end:\n"
	"	fnstsw probe_fp_saved" PROBE_RIP "\n"
	"	fninit\n"
	"	ret\n"
	".global probe_mf_stack\n"
	"probe_mf_stack:\n"
	"	fninit\n"
	"	movw $0x37e, probe_fp_scratch" PROBE_RIP "\n"
	"	fldcw probe_fp_scratch" PROBE_RIP "\n"
	"	fld1\n"
	"	fld1\n"
	"	fld1\n"
	"	fld1\n"
	"	fld1\n"
	"	fld1\n"
	"	fld1\n"
	"	fld1\n"
	"	fld1\n"
	".global probe_mf_stack_insn\n"
	"probe_mf_stack_insn:\n"
	"	fwait\n"
	".global probe_mf_stack_end\n"
	"probe_mf_stack_end:\n"
	"	fnstsw probe_fp_saved" PROBE_RIP "\n"
	"	fninit\n"
	"	ret\n"
	PROBE_MF_FULL_STACK("fxtract", "fxtract")
	PROBE_MF_FULL_STACK("fptan", "fptan")
	PROBE_MF_FULL_STACK("fsincos", "fsincos")
	".global probe_mf_unmask\n"
	"probe_mf_unmask:\n"
	"	fninit\n"
	"	fldz\n"
	"	fld1\n"
	"	fdivp\n"
	"	movw $0x37b, probe_fp_scratch" PROBE_RIP "\n"
	"	fldcw probe_fp_scratch" PROBE_RIP "\n"
	".global probe_mf_unmask_insn\n"
	"probe_mf_unmask_insn:\n"
	"	fwait\n"
	".global probe_mf_unmask_end\n"
	"probe_mf_unmask_end:\n"
	"	fnstsw probe_fp_saved" PROBE_RIP "\n"
	"	fninit\n"
	"	ret\n"
	".global probe_mf_mmx\n"
	"probe_mf_mmx:\n"
	"	fninit\n"
	"	movw $0x37b, probe_fp_scratch" PROBE_RIP "\n"
	"	fldcw probe_fp_scratch" PROBE_RIP "\n"
	"	fldz\n"
	"	fld1\n"
	"	fdivp\n"
	".global probe_mf_mmx_insn\n"
	"probe_mf_mmx_insn:\n"
	"	movq %mm0, %mm1\n"
	".global probe_mf_mmx_end\n"
	"probe_mf_mmx_end:\n"
	"	fnstsw probe_fp_saved" PROBE_RIP "\n"
	"	fninit\n"
	"	emms\n"
	"	ret\n"
	".global probe_mf_fist\n"
	"probe_mf_fist:\n"
	"	movl $0x12345678, probe_fp_saved+4" PROBE_RIP "\n"
	"	fninit\n"
	"	movw $0x37e, probe_fp_scratch" PROBE_RIP "\n"
	"	fldcw probe_fp_scratch" PROBE_RIP "\n"
	"	movl $0x7f000000, probe_fp_scratch" PROBE_RIP "\n"
	"	flds probe_fp_scratch" PROBE_RIP "\n"
	"	fistpl probe_fp_saved+4" PROBE_RIP "\n"
	".global probe_mf_fist_insn\n"
	"probe_mf_fist_insn:\n"
	"	fwait\n"
	".global probe_mf_fist_end\n"
	"probe_mf_fist_end:\n"
	"	fnstsw probe_fp_saved" PROBE_RIP "\n"
	"	fninit\n"
	"	ret\n"
	/* Delphi's control words: FCW $1332 and MXCSR $1900 unmask invalid
	 * operation, division by zero and overflow. Arithmetic that raises
	 * none of them runs as it would masked, leaving its results, the
	 * inexact flags and the status word in probe_delphi_out. */
	".global probe_delphi_clean\n"
	"probe_delphi_clean:\n"
	"	fninit\n"
	"	movw $0x1332, probe_fp_scratch" PROBE_RIP "\n"
	"	fldcw probe_fp_scratch" PROBE_RIP "\n"
	"	movl $0x1900, probe_fp_scratch" PROBE_RIP "\n"
	"	ldmxcsr probe_fp_scratch" PROBE_RIP "\n"
	"	movl $3, probe_fp_scratch" PROBE_RIP "\n"
	"	fildl probe_fp_scratch" PROBE_RIP "\n"
	"	fld1\n"
	"	fdivrp\n"
	"	fld %st(0)\n"
	"	fmulp\n"
	"	fsqrt\n"
	"	fldpi\n"
	"	faddp\n"
	"	fld %st(0)\n"
	"	fstpl probe_delphi_out+8" PROBE_RIP "\n"
	"	fistpl probe_delphi_out" PROBE_RIP "\n"
	"	fnstsw probe_delphi_out+4" PROBE_RIP "\n"
	"	movl $7, %eax\n"
	"	cvtsi2sd %eax, %xmm0\n"
	"	movl $3, %eax\n"
	"	cvtsi2sd %eax, %xmm1\n"
	"	divsd %xmm1, %xmm0\n"
	"	mulsd %xmm0, %xmm0\n"
	"	sqrtsd %xmm0, %xmm0\n"
	"	addsd %xmm1, %xmm0\n"
	"	movsd %xmm0, probe_delphi_out+16" PROBE_RIP "\n"
	"	cvttsd2si %xmm0, %eax\n"
	"	movl %eax, probe_delphi_out+24" PROBE_RIP "\n"
	"	stmxcsr probe_delphi_out+28" PROBE_RIP "\n"
	".global probe_delphi_clean_end\n"
	"probe_delphi_clean_end:\n"
	"	fninit\n"
	"	movl $0x1f80, probe_fp_scratch" PROBE_RIP "\n"
	"	ldmxcsr probe_fp_scratch" PROBE_RIP "\n"
	"	ret\n"
	/* Under FCW $1332 a division by zero raises #MF at the next x87
	 * instruction, here FLD1, which does not run. */
	".global probe_delphi_div\n"
	"probe_delphi_div:\n"
	"	fninit\n"
	"	movw $0x1332, probe_fp_scratch" PROBE_RIP "\n"
	"	fldcw probe_fp_scratch" PROBE_RIP "\n"
	"	fldz\n"
	"	fld1\n"
	"	fdivp\n"
	".global probe_delphi_div_insn\n"
	"probe_delphi_div_insn:\n"
	"	fld1\n"
	".global probe_delphi_div_end\n"
	"probe_delphi_div_end:\n"
	"	fnstsw probe_fp_saved" PROBE_RIP "\n"
	"	fninit\n"
	"	ret\n"
	/* Under MXCSR $1900 a division by zero raises #XM at DIVSD. */
	".global probe_delphi_divsd\n"
	"probe_delphi_divsd:\n"
	"	movl $0x1900, probe_fp_scratch" PROBE_RIP "\n"
	"	ldmxcsr probe_fp_scratch" PROBE_RIP "\n"
	"	movl $1, %eax\n"
	"	cvtsi2sd %eax, %xmm0\n"
	"	xorpd %xmm1, %xmm1\n"
	".global probe_delphi_divsd_insn\n"
	"probe_delphi_divsd_insn:\n"
	"	divsd %xmm1, %xmm0\n"
	".global probe_delphi_divsd_end\n"
	"probe_delphi_divsd_end:\n"
	"	stmxcsr probe_fp_saved" PROBE_RIP "\n"
	"	movq %xmm0, %xmm2\n"
	"	psrlq $32, %xmm2\n"
	"	movd %xmm2, %eax\n"
	"	movl %eax, probe_fp_saved+4" PROBE_RIP "\n"
	"	movl $0x1f80, probe_fp_scratch" PROBE_RIP "\n"
	"	ldmxcsr probe_fp_scratch" PROBE_RIP "\n"
	"	ret\n"
	/* Under MXCSR $1900 COMISD of a quiet NaN, CVTTSD2SI of a value out
	 * of range and a MULPD lane that overflows raise #XM too. */
	".global probe_delphi_comisd\n"
	"probe_delphi_comisd:\n"
	"	movl $0x1900, probe_fp_scratch" PROBE_RIP "\n"
	"	ldmxcsr probe_fp_scratch" PROBE_RIP "\n"
	"	movsd probe_delphi_values" PROBE_RIP ", %xmm0\n"
	"	movl $1, %eax\n"
	"	cvtsi2sd %eax, %xmm1\n"
	".global probe_delphi_comisd_insn\n"
	"probe_delphi_comisd_insn:\n"
	"	comisd %xmm1, %xmm0\n"
	".global probe_delphi_comisd_end\n"
	"probe_delphi_comisd_end:\n"
	"	stmxcsr probe_fp_saved" PROBE_RIP "\n"
	"	movq %xmm0, %xmm2\n"
	"	psrlq $32, %xmm2\n"
	"	movd %xmm2, %eax\n"
	"	movl %eax, probe_fp_saved+4" PROBE_RIP "\n"
	"	movl $0x1f80, probe_fp_scratch" PROBE_RIP "\n"
	"	ldmxcsr probe_fp_scratch" PROBE_RIP "\n"
	"	ret\n"
	".global probe_delphi_cvt\n"
	"probe_delphi_cvt:\n"
	"	movl $0x1900, probe_fp_scratch" PROBE_RIP "\n"
	"	ldmxcsr probe_fp_scratch" PROBE_RIP "\n"
	"	movsd probe_delphi_values+8" PROBE_RIP ", %xmm0\n"
	".global probe_delphi_cvt_insn\n"
	"probe_delphi_cvt_insn:\n"
	"	cvttsd2si %xmm0, %eax\n"
	".global probe_delphi_cvt_end\n"
	"probe_delphi_cvt_end:\n"
	"	stmxcsr probe_fp_saved" PROBE_RIP "\n"
	"	movq %xmm0, %xmm2\n"
	"	psrlq $32, %xmm2\n"
	"	movd %xmm2, %eax\n"
	"	movl %eax, probe_fp_saved+4" PROBE_RIP "\n"
	"	movl $0x1f80, probe_fp_scratch" PROBE_RIP "\n"
	"	ldmxcsr probe_fp_scratch" PROBE_RIP "\n"
	"	ret\n"
	".global probe_delphi_mulpd\n"
	"probe_delphi_mulpd:\n"
	"	movl $0x1900, probe_fp_scratch" PROBE_RIP "\n"
	"	ldmxcsr probe_fp_scratch" PROBE_RIP "\n"
	"	movupd probe_delphi_values+16" PROBE_RIP ", %xmm0\n"
	"	movupd probe_delphi_values+32" PROBE_RIP ", %xmm1\n"
	".global probe_delphi_mulpd_insn\n"
	"probe_delphi_mulpd_insn:\n"
	"	mulpd %xmm1, %xmm0\n"
	".global probe_delphi_mulpd_end\n"
	"probe_delphi_mulpd_end:\n"
	"	stmxcsr probe_fp_saved" PROBE_RIP "\n"
	"	movq %xmm0, %xmm2\n"
	"	psrlq $32, %xmm2\n"
	"	movd %xmm2, %eax\n"
	"	movl %eax, probe_fp_saved+4" PROBE_RIP "\n"
	"	movl $0x1f80, probe_fp_scratch" PROBE_RIP "\n"
	"	ldmxcsr probe_fp_scratch" PROBE_RIP "\n"
	"	ret\n"
	/* An infinity plus one, and UCOMISD of a quiet NaN, raise nothing
	 * under MXCSR $1900; the results and MXCSR go to
	 * probe_delphi_special_out. */
	".global probe_delphi_specials\n"
	"probe_delphi_specials:\n"
	"	movl $0x1900, probe_fp_scratch" PROBE_RIP "\n"
	"	ldmxcsr probe_fp_scratch" PROBE_RIP "\n"
	"	movsd probe_delphi_values+48" PROBE_RIP ", %xmm0\n"
	"	movl $1, %eax\n"
	"	cvtsi2sd %eax, %xmm1\n"
	"	addsd %xmm1, %xmm0\n"
	"	movsd %xmm0, probe_delphi_special_out" PROBE_RIP "\n"
	"	movsd probe_delphi_values" PROBE_RIP ", %xmm2\n"
	"	ucomisd %xmm1, %xmm2\n"
	"	setp %al\n"
	"	movzbl %al, %eax\n"
	"	movl %eax, probe_delphi_special_out+8" PROBE_RIP "\n"
	"	stmxcsr probe_delphi_special_out+12" PROBE_RIP "\n"
	".global probe_delphi_specials_end\n"
	"probe_delphi_specials_end:\n"
	"	movl $0x1f80, probe_fp_scratch" PROBE_RIP "\n"
	"	ldmxcsr probe_fp_scratch" PROBE_RIP "\n"
	"	ret\n"
	".data\n"
	".balign 8\n"
	".global probe_fp_saved\n"
	"probe_fp_saved:\n"
	"	.long 0, 0\n"
	"probe_fp_scratch:\n"
	"	.long 0\n"
	".balign 8\n"
	".global probe_delphi_out\n"
	"probe_delphi_out:\n"
	"	.long 0, 0, 0, 0, 0, 0, 0, 0\n"
	".global probe_delphi_special_out\n"
	"probe_delphi_special_out:\n"
	"	.long 0, 0, 0, 0\n"
	/* A quiet NaN, 1e20, {1, DBL_MAX}, {1, 1.5} and an infinity. */
	"probe_delphi_values:\n"
	"	.quad 0x7ff8000000000000, 0x4415af1d78b58c40\n"
	"	.quad 0x3ff0000000000000, 0x7fefffffffffffff\n"
	"	.quad 0x3ff0000000000000, 0x3ff8000000000000\n"
	"	.quad 0x7ff0000000000000\n"
	".text\n");

/* INT n of a vector user mode may not use raises #GP. */
__asm__(".text\n"
	".global probe_int_1\n"
	"probe_int_1:\n"
	"	int $0x1\n"
	".global probe_int_1_end\n"
	"probe_int_1_end:\n"
	"	ret\n"
	".global probe_int_2\n"
	"probe_int_2:\n"
	"	int $0x2\n"
	".global probe_int_2_end\n"
	"probe_int_2_end:\n"
	"	ret\n"
	".global probe_int_41\n"
	"probe_int_41:\n"
	"	int $0x41\n"
	".global probe_int_41_end\n"
	"probe_int_41_end:\n"
	"	ret\n"
	".global probe_int_ff\n"
	"probe_int_ff:\n"
	"	int $0xff\n"
	".global probe_int_ff_end\n"
	"probe_int_ff_end:\n"
	"	ret\n"
	);

/* The debug-register tracee's work: one instruction to step, then ICEBP. */
__asm__(".text\n"
	".global probe_debug_child\n"
	"probe_debug_child:\n"
	"	nop\n"
	"	.byte 0xf1\n"
	"	ret\n");

/* Both ABIs assemble these alike. UD2 raises #UD. */
__asm__(".text\n"
	".global probe_ud2\n"
	"probe_ud2:\n"
	"	ud2\n"
	".global probe_ud2_end\n"
	"probe_ud2_end:\n"
	"	ret\n");

/* Encodings that hardware or the product leaves undefined, and SYSEXIT
 * and the system instructions that user mode may not run. The first lists
 * decode alike in both modes; SYSRET and SWAPGS are undefined outside
 * 64-bit mode, and a REX prefix names CR8, which user mode may not move,
 * and CR9 to CR15, which are undefined. */
#if defined(__x86_64__)
#define UNDEFINED_MODE_CASES(X)                                              \
	X(group1_82, "0x82, 0xc0, 0x01")                                     \
	X(mov_from_cr9, "0x44, 0x0f, 0x20, 0xc8")
#define PRIVILEGED_MODE_CASES(X)                                             \
	X(sysret, "0x0f, 0x07")                                              \
	X(swapgs, "0x0f, 0x01, 0xf8")                                        \
	X(mov_to_cr8, "0x44, 0x0f, 0x22, 0xc0")
#else
#define UNDEFINED_MODE_CASES(X)                                              \
	X(sysret, "0x0f, 0x07")                                              \
	X(swapgs, "0x0f, 0x01, 0xf8")
#define PRIVILEGED_MODE_CASES(X)
#endif
#define PROBE_BYTES(name, bytes)                                             \
	".global probe_" #name "\n"                                          \
	"probe_" #name ":\n"                                                 \
	"	.byte " bytes "\n"                                           \
	".global probe_" #name "_end\n"                                      \
	"probe_" #name "_end:\n"                                             \
	"	ret\n"
#define UNDEFINED_CASES(X)                                                   \
	X(lea_register, "0x8d, 0xc0")                                        \
	X(lock_cmp, "0xf0, 0x83, 0x3c, 0x24, 0x00")                          \
	X(lock_bt, "0xf0, 0x0f, 0xba, 0x24, 0x24, 0x00")                     \
	X(movhps_register, "0x0f, 0x17, 0xc0")                               \
	X(movntq_register, "0x0f, 0xe7, 0xc0")                               \
	X(fxsave_register, "0x0f, 0xae, 0xc0")                               \
	X(reserved_0f3e, "0x0f, 0x3e")                                       \
	X(reserved_0f3f, "0x0f, 0x3f")                                       \
	X(lddqu_register, "0xf2, 0x0f, 0xf0, 0xc0")                          \
	X(movlpd_register, "0x66, 0x0f, 0x12, 0xc0")                         \
	X(emms_66, "0x66, 0x0f, 0x77")                                       \
	X(x87_d9_1, "0xd9, 0x08")                                            \
	X(x87_db_4, "0xdb, 0x20")                                            \
	X(x87_dd_5, "0xdd, 0x28")                                            \
	X(x87_db_f8, "0xdb, 0xf8")                                           \
	X(xgetbv_66, "0x66, 0x0f, 0x01, 0xd0")                               \
	X(xgetbv_f3, "0xf3, 0x0f, 0x01, 0xd0")                               \
	X(xsetbv_66, "0x66, 0x0f, 0x01, 0xd1")                               \
	X(mov_from_cr1, "0x0f, 0x20, 0xc8")                                  \
	X(mov_to_cr5, "0x0f, 0x22, 0xe8")                                    \
	X(mov_to_cr6, "0x0f, 0x22, 0xf0")                                    \
	X(mov_from_cr7, "0x0f, 0x20, 0xf8")                                  \
	X(movq2dq_memory, "0xf3, 0x0f, 0xd6, 0x00")                          \
	X(movdq2q_memory, "0xf2, 0x0f, 0xd6, 0x00")                          \
	X(clflushopt_register, "0x66, 0x0f, 0xae, 0xf8")                     \
	X(clzero, "0x0f, 0x01, 0xfc")                                        \
	X(femms, "0x0f, 0x0e")                                               \
	X(pi2fw, "0x0f, 0x0f, 0xc1, 0x0c")                                   \
	X(pfadd, "0x0f, 0x0f, 0xc1, 0x9e")                                   \
	X(pswapd, "0x0f, 0x0f, 0xc1, 0xbb")                                  \
	X(pavgusb_memory, "0x0f, 0x0f, 0x00, 0xbf")                          \
	X(amd_3dnow_ff, "0x0f, 0x0f, 0xc1, 0xff")                            \
	X(extrq_immediate, "0x66, 0x0f, 0x78, 0xc1, 0x08, 0x04")             \
	X(extrq_register, "0x66, 0x0f, 0x79, 0xc1")                          \
	X(insertq_immediate, "0xf2, 0x0f, 0x78, 0xc1, 0x08, 0x04")           \
	X(insertq_register, "0xf2, 0x0f, 0x79, 0xc1")                        \
	X(movntss, "0xf3, 0x0f, 0x2b, 0x00")                                 \
	X(movntsd, "0xf2, 0x0f, 0x2b, 0x00")                                 \
	UNDEFINED_MODE_CASES(X)
/* Each takes an unmapped address in AX, which they raise #GP before
 * reading. */
#define PRIVILEGED_CASES(X)                                                  \
	X(ltr, "0x0f, 0x00, 0xd8")                                           \
	X(lidt, "0x0f, 0x01, 0x18")                                          \
	X(lmsw_register, "0x0f, 0x01, 0xf0")                                 \
	X(lmsw_memory, "0x0f, 0x01, 0x30")                                   \
	X(invlpg, "0x0f, 0x01, 0x38")                                        \
	X(mov_to_cr0, "0x0f, 0x22, 0xc0")                                    \
	X(mov_from_cr0, "0x0f, 0x20, 0xc0")                                  \
	X(mov_to_cr4, "0x0f, 0x22, 0xe0")                                    \
	X(lldt_register, "0x0f, 0x00, 0xd0")                                 \
	X(lldt_memory, "0x0f, 0x00, 0x10")                                   \
	X(lgdt, "0x0f, 0x01, 0x10")                                          \
	X(xsetbv, "0x0f, 0x01, 0xd1")                                        \
	PRIVILEGED_MODE_CASES(X)
#define EMIT_UNDEFINED(name, bytes) PROBE_BYTES(name, bytes)
#define EMIT_PRIVILEGED(name, bytes)                                         \
	".global probe_" #name "\n"                                          \
	"probe_" #name ":\n"                                                 \
	PROBE_ARG_AX                                                         \
	".global probe_" #name "_insn\n"                                     \
	"probe_" #name "_insn:\n"                                            \
	"	.byte " bytes "\n"                                           \
	".global probe_" #name "_end\n"                                      \
	"probe_" #name "_end:\n"                                             \
	"	ret\n"
__asm__(".text\n" UNDEFINED_CASES(EMIT_UNDEFINED)
	PRIVILEGED_CASES(EMIT_PRIVILEGED)
	PROBE_BYTES(sysexit, "0x0f, 0x35")
	/* MOV to CS from an unmapped address: #UD before the read. */
	".global probe_mov_cs\n"
	"probe_mov_cs:\n"
	PROBE_ARG_AX
	".global probe_mov_cs_insn\n"
	"probe_mov_cs_insn:\n"
	"	.byte 0x8e, 0x08\n"
	".global probe_mov_cs_end\n"
	"probe_mov_cs_end:\n"
	"	ret\n");
#undef EMIT_UNDEFINED
#undef EMIT_PRIVILEGED
#undef PROBE_BYTES

constexpr word sa_siginfo = 4;
constexpr word sa_restorer = 0x04000000;
constexpr word sa_restart = 0x10000000;
constexpr long sigill = 4;
constexpr long sigtrap = 5;
constexpr long sigkill = 9;
constexpr long sigsegv = 11;
constexpr long sigalrm = 14;
constexpr long sigstop = 19;
constexpr long trap_brkpt = 1;
constexpr long si_kernel = 0x80;
constexpr word flag_reserved = 1UL << 1;
constexpr word flag_rf = 1UL << 16;
constexpr long eio = 5;
constexpr long bus_adraln = 1;
constexpr long sigfpe = 8;
constexpr long fpe_fltdiv = 3;
constexpr long fpe_fltovf = 4;
constexpr long fpe_fltund = 5;
constexpr long fpe_fltres = 6;
constexpr long fpe_fltinv = 7;
constexpr long sigbus = 7;
constexpr long einval = 22;
constexpr long segv_maperr = 1;
/* Below every mapping of a static program, and at mmap_min_addr. */
constexpr word unmapped_page = 0x10000;
constexpr long ill_illopn = 2;

struct KernelSigaction {
	word handler;
	word flags;
	word restorer;
	u32 mask[2];
};

struct Timespec {
	long seconds;
	long nanoseconds;
};

struct Itimerval {
	long interval_seconds;
	long interval_microseconds;
	long value_seconds;
	long value_microseconds;
};

void put(const char *text)
{
	long length = 0;
	while (text[length] != '\0')
		++length;
	raw_syscall(nr_write, 1, reinterpret_cast<long>(text), length);
}

struct Quotient {
	u64 quotient;
	u64 remainder;
};

/* Long division, bit by bit: a freestanding i386 build has no __udivdi3, and
 * compilers differ in which constant divisors they expand inline. */
Quotient divide(u64 dividend, u32 divisor)
{
	Quotient result = {};
	for (int bit = 63; bit >= 0; --bit) {
		result.remainder = result.remainder << 1 | ((dividend >> bit) & 1);
		if (result.remainder >= divisor) {
			result.remainder -= divisor;
			result.quotient |= 1ULL << bit;
		}
	}
	return result;
}

void put_decimal(u64 value)
{
	char text[21];
	int position = 20;
	text[position] = '\0';
	do {
		const Quotient digit = divide(value, 10);
		text[--position] = static_cast<char>('0' + digit.remainder);
		value = digit.quotient;
	} while (value != 0);
	put(text + position);
}

void put_hex(u64 value)
{
	char text[19];
	int position = 18;
	text[position] = '\0';
	do {
		text[--position] = "0123456789abcdef"[value & 15];
		value >>= 4;
	} while (value != 0);
	text[--position] = 'x';
	text[--position] = '0';
	put(text + position);
}

bool equal(const char *left, const char *right)
{
	while (*left != '\0' && *left == *right) {
		++left;
		++right;
	}
	return *left == *right;
}

/* Prints a mismatch and returns whether there was none. */
bool expect(const char *what, u64 actual, u64 expected)
{
	if (actual == expected)
		return true;
	put("  ");
	put(what);
	put(" is ");
	put_hex(actual);
	put(", expected ");
	put_hex(expected);
	put("\n");
	return false;
}

extern "C" void probe_restorer();

bool install(long signal, void (*handler)(int, void *, void *))
{
	KernelSigaction action = {};
	action.handler = reinterpret_cast<word>(handler);
	action.flags = sa_siginfo | sa_restorer;
	action.restorer = reinterpret_cast<word>(&probe_restorer);
	return raw_syscall(nr_rt_sigaction, signal,
			   reinterpret_cast<long>(&action), 0, 8) == 0;
}

u64 now_nanoseconds()
{
	Timespec time = {};
	raw_syscall(nr_clock_gettime, 1, reinterpret_cast<long>(&time));
	return static_cast<u64>(time.seconds) * 1000000000ULL +
	       static_cast<u64>(time.nanoseconds);
}

bool set_interval_timer(long microseconds)
{
	const Itimerval timer = {0, microseconds, 0, microseconds};
	return raw_syscall(nr_setitimer, 0, reinterpret_cast<long>(&timer),
			   0) == 0;
}

extern "C" void probe_stos(void *buffer, word count);
extern "C" word probe_cmps(const void *left, const void *right, word count);

constexpr word stos_bytes = 1UL << 22;
constexpr word stos_rounds = 2000;
constexpr word cmps_bytes = 1UL << 16;
constexpr word cmps_rounds = 1000;
/* Native Linux delivers a 1 ms timer about once a millisecond; this asks for
 * one in every ten. */
constexpr u64 tick_allowance_nanoseconds = 10000000;

alignas(4096) unsigned char stos_buffer[stos_bytes];
alignas(4096) unsigned char cmps_left[cmps_bytes];
alignas(4096) unsigned char cmps_right[cmps_bytes];

volatile u64 ticks;
volatile u64 last_tick;
volatile u64 longest_gap;

void record_tick(int, void *, void *)
{
	const u64 now = now_nanoseconds();
	if (now - last_tick > longest_gap)
		longest_gap = now - last_tick;
	last_tick = now;
	ticks = ticks + 1;
}

/* A 1 ms interval timer keeps firing while `work` runs long repeated string
 * instructions and makes no system call. */
bool timer_ticks_during(const char *what, void (*work)())
{
	if (!install(sigalrm, record_tick)) {
		put("  rt_sigaction failed\n");
		return false;
	}
	ticks = 0;
	longest_gap = 0;
	const u64 started = now_nanoseconds();
	last_tick = started;
	if (!set_interval_timer(1000)) {
		put("  setitimer failed\n");
		return false;
	}
	work();
	set_interval_timer(0);
	const u64 finished = now_nanoseconds();
	if (finished - last_tick > longest_gap)
		longest_gap = finished - last_tick;
	const u64 elapsed = finished - started;
	const bool ok = ticks * tick_allowance_nanoseconds >= elapsed;
	put(ok ? "  " : "  too few: ");
	put(what);
	put(" ran ");
	put_decimal(divide(elapsed, 1000000).quotient);
	put(" ms with ");
	put_decimal(ticks);
	put(" SIGALRM, longest gap ");
	put_decimal(divide(longest_gap, 1000000).quotient);
	put(" ms\n");
	return ok;
}

void repeat_stos()
{
	for (word round = 0; round < stos_rounds; ++round)
		probe_stos(stos_buffer, stos_bytes);
}

volatile word cmps_left_over;

void repeat_cmps()
{
	for (word round = 0; round < cmps_rounds; ++round)
		cmps_left_over = probe_cmps(cmps_left, cmps_right, cmps_bytes);
}

/* What the last synchronous signal showed its handler. */
enum Observed {
	observed_count,
	observed_signal,
	observed_code,
	observed_address,
	observed_ip,
	observed_flags,
	observed_trapno,
	observed_error,
	observed_cx,
	observed_di,
	observed_fields,
};

volatile word observed[observed_fields];
/* Where the handler resumes the task, so an unexpected fault cannot repeat. */
volatile word resume_ip;

word &frame_word(void *context, unsigned offset)
{
	return *reinterpret_cast<word *>(static_cast<unsigned char *>(context) +
					 offset);
}

void record_fault(int signal, void *info, void *context)
{
	const auto *details = static_cast<const unsigned char *>(info);
	observed[observed_count] = observed[observed_count] + 1;
	observed[observed_signal] = static_cast<word>(signal);
	observed[observed_code] =
		static_cast<word>(*reinterpret_cast<const int *>(details + 8));
	observed[observed_address] = *reinterpret_cast<const word *>(
		details + siginfo_address_offset);
	observed[observed_ip] = frame_word(context, frame_ip_offset);
	observed[observed_flags] = frame_word(context, frame_flags_offset);
	observed[observed_trapno] = frame_word(context, frame_trapno_offset);
	observed[observed_error] = frame_word(context, frame_error_offset);
	observed[observed_cx] = frame_word(context, frame_cx_offset);
	observed[observed_di] = frame_word(context, frame_di_offset);
	if (resume_ip != 0)
		frame_word(context, frame_ip_offset) = resume_ip;
}

bool install_fault_handlers()
{
	if (install(sigill, record_fault) && install(sigtrap, record_fault) &&
	    install(sigsegv, record_fault) && install(sigbus, record_fault))
		return true;
	put("  rt_sigaction failed\n");
	return false;
}

struct Delivery {
	long signal;
	long code;
	word address;
	word ip;
	word trapno;
	bool resume_flag;
	word error;
};

/* Runs `work`, resuming after `end` if it faults, and checks that it raised
 * exactly `expected`. */
bool delivers(const char *what, void (*work)(), word end,
	      const Delivery &expected)
{
	for (unsigned index = 0; index < observed_fields; ++index)
		observed[index] = 0;
	resume_ip = end;
	work();
	resume_ip = 0;
	if (observed[observed_count] != 1) {
		put("  ");
		put(what);
		put(" raised ");
		put_decimal(observed[observed_count]);
		put(" signals, the last ");
		put_decimal(observed[observed_signal]);
		put(" code ");
		put_hex(observed[observed_code]);
		put("\n");
		return false;
	}
	bool ok = expect("signal", observed[observed_signal],
			 static_cast<word>(expected.signal));
	ok &= expect("si_code", observed[observed_code],
		     static_cast<word>(expected.code));
	ok &= expect("si_addr", observed[observed_address], expected.address);
	ok &= expect("frame ip", observed[observed_ip], expected.ip);
	ok &= expect("trapno", observed[observed_trapno], expected.trapno);
	ok &= expect("err", observed[observed_error], expected.error);
	ok &= expect("RF", (observed[observed_flags] & flag_rf) != 0,
		     expected.resume_flag);
	if (!ok) {
		put("  in ");
		put(what);
		put("\n");
	}
	return ok;
}

extern "C" word probe_getpid_on(word stack_pointer, word count);

/* Each bad stack pointer case repeats its events this often. */
constexpr word bad_stack_rounds = 16;

/* Keeps the task on CPU 0, so that its events all reach one vCPU. */
bool pin_to_one_cpu()
{
	const word mask = 1;
	if (raw_syscall(nr_sched_setaffinity, 0, sizeof(mask),
			reinterpret_cast<long>(&mask)) == 0)
		return true;
	put("  sched_setaffinity failed\n");
	return false;
}

/* System calls made with (stack_pointer) as the stack pointer return. */
bool calls_on(const char *what, word stack_pointer)
{
	const auto pid = static_cast<word>(raw_syscall(nr_getpid));
	if (probe_getpid_on(stack_pointer, bad_stack_rounds) == pid)
		return true;
	put("  getpid with ");
	put(what);
	put(" failed\n");
	return false;
}

extern "C" void probe_icebp();
extern "C" char probe_icebp_end[];
extern "C" void probe_int4();
extern "C" char probe_int4_end[];

word address_of(const char *label)
{
	return reinterpret_cast<word>(label);
}

/* ICEBP (INT1) raises #DB as a trap: SIGTRAP with TRAP_BRKPT, naming the
 * next instruction. */
bool icebp_traps()
{
	const word end = address_of(probe_icebp_end);
	return install_fault_handlers() &&
	       delivers("ICEBP", probe_icebp, end,
			{sigtrap, trap_brkpt, end, end, 1, false, 0});
}

/* INT 4 raises #OF as a trap, which Linux delivers as SIGSEGV with
 * SI_KERNEL and no address. */
bool int4_traps()
{
	const word end = address_of(probe_int4_end);
	return install_fault_handlers() &&
	       delivers("INT 4", probe_int4, end,
			{sigsegv, si_kernel, 0, end, 4, false, 0});
}

extern "C" void probe_x87_signal(double *out);

/* A signal taken with a value on the x87 stack returns to the same stack. */
bool x87_stack_survives_a_signal()
{
	if (!install_fault_handlers())
		return false;
	double result = 0;
	probe_x87_signal(&result);
	u64 bits;
	__builtin_memcpy(&bits, &result, sizeof(bits));
	return expect("pi stored after the SIGTRAP", bits,
		      0x400921fb54442d18ULL);
}

extern "C" word probe_hold_ax(word value, word loops);

volatile u64 alarms;

void count_alarm(int, void *, void *) { alarms = alarms + 1; }

/* One round spins for about a millisecond or more; the case waits for a few
 * timer signals. */
constexpr word hold_loops = 1UL << 22;
constexpr u64 hold_alarms = 3;
constexpr int hold_rounds = 10000;

/* A signal taken between two instructions, outside any system call, leaves
 * AX alone even when it holds one of the -ERESTART* codes an interrupted
 * system call returns, with SA_RESTART or without. */
bool signal_keeps_ax()
{
	const word restarts[] = {0, sa_restart};
	const long codes[] = {512, 513, 514, 516};
	bool ok = true;
	for (const word restart : restarts) {
		KernelSigaction action = {};
		action.handler = reinterpret_cast<word>(count_alarm);
		action.flags = sa_siginfo | sa_restorer | restart;
		action.restorer = reinterpret_cast<word>(&probe_restorer);
		if (raw_syscall(nr_rt_sigaction, sigalrm,
				reinterpret_cast<long>(&action), 0, 8) != 0) {
			put("  rt_sigaction failed\n");
			return false;
		}
		for (const long code : codes) {
			const word value = static_cast<word>(-code);
			word held = value;
			alarms = 0;
			if (!set_interval_timer(1000)) {
				put("  setitimer failed\n");
				return false;
			}
			for (int round = 0; round < hold_rounds && held == value &&
			     alarms < hold_alarms; ++round)
				held = probe_hold_ax(value, hold_loops);
			set_interval_timer(0);
			put(restart != 0 ? "  SA_RESTART" : "  no SA_RESTART");
			put(", AX -");
			put_decimal(static_cast<u64>(code));
			put(", ");
			put_decimal(alarms);
			put(" SIGALRM\n");
			ok &= expect("AX", held, value);
			if (held == value && alarms < hold_alarms) {
				put("  too few SIGALRM\n");
				ok = false;
			}
		}
	}
	return ok;
}

/* Runs `work`, resuming after `end` if it faults, and checks that it raised
 * nothing. */
bool delivers_nothing(const char *what, void (*work)(), word end)
{
	observed[observed_count] = 0;
	resume_ip = end;
	work();
	resume_ip = 0;
	if (observed[observed_count] == 0)
		return true;
	put("  ");
	put(what);
	put(" raised signal ");
	put_decimal(observed[observed_signal]);
	put("\n");
	return false;
}

#if defined(__i386__)

extern "C" void probe_into(word value);
extern "C" char probe_into_end[];
extern "C" void probe_bound32(word index, const void *bounds);
extern "C" char probe_bound32_insn[];
extern "C" char probe_bound32_end[];
extern "C" void probe_bound16(word index, const void *bounds);
extern "C" char probe_bound16_insn[];
extern "C" char probe_bound16_end[];

void into_overflowed() { probe_into(1); }
void into_clear() { probe_into(0); }

/* INTO raises #OF only when OF is set, as a trap. */
bool into_traps()
{
	const word end = address_of(probe_into_end);
	bool ok = install_fault_handlers();
	ok = ok && delivers_nothing("INTO without overflow", into_clear, end);
	ok = ok && delivers("INTO with overflow", into_overflowed, end,
			    {sigsegv, si_kernel, 0, end, 4, false, 0});
	return ok;
}

const int bounds32[2] = {-2, 5};
const short bounds16[2] = {-2, 5};
volatile word bound_index;

void bound32() { probe_bound32(bound_index, bounds32); }
void bound16() { probe_bound16(bound_index, bounds16); }

/* BOUND compares a signed index with an inclusive pair of bounds and raises
 * #BR, a fault, outside them: SIGSEGV with SI_KERNEL at the instruction. */
bool bound_checks()
{
	if (!install_fault_handlers())
		return false;
	const word insn32 = address_of(probe_bound32_insn);
	const word end32 = address_of(probe_bound32_end);
	const word insn16 = address_of(probe_bound16_insn);
	const word end16 = address_of(probe_bound16_end);
	bool ok = true;
	const long inside[] = {-2, 0, 5};
	for (const long index : inside) {
		bound_index = static_cast<word>(index);
		ok &= delivers_nothing("BOUND r32 in range", bound32, end32);
		ok &= delivers_nothing("BOUND r16 in range", bound16, end16);
	}
	/* 0x7fff is 32767 to the 16-bit form, above its bounds; 0x10000 is
	 * zero to it, within them. */
	const long outside32[] = {-3, 6, 0x7fff};
	for (const long index : outside32) {
		bound_index = static_cast<word>(index);
		ok &= delivers("BOUND r32 out of range", bound32, end32,
			       {sigsegv, si_kernel, 0, insn32, 5, true, 0});
	}
	bound_index = 0x7fff;
	ok &= delivers("BOUND r16 out of range", bound16, end16,
		       {sigsegv, si_kernel, 0, insn16, 5, true, 0});
	bound_index = 0x10000;
	ok &= delivers_nothing("BOUND r16 of a truncated index", bound16,
			       end16);
	return ok;
}
#endif

extern "C" void probe_ud2();
extern "C" char probe_ud2_end[];

/* #UD, vector 6, is a fault: SIGILL with ILL_ILLOPN at the instruction. */
bool ud2_faults()
{
	const word insn = reinterpret_cast<word>(&probe_ud2);
	return install_fault_handlers() &&
	       delivers("UD2", probe_ud2, address_of(probe_ud2_end),
			{sigill, ill_illopn, insn, insn, 6, true, 0});
}

#define DECLARE_UNDEFINED(name, bytes)                                       \
	extern "C" void probe_##name();                                      \
	extern "C" char probe_##name##_end[];
UNDEFINED_CASES(DECLARE_UNDEFINED)
DECLARE_UNDEFINED(sysexit, "")
#undef DECLARE_UNDEFINED
#define DECLARE_PRIVILEGED(name, bytes)                                      \
	extern "C" void probe_##name(word address);                         \
	extern "C" char probe_##name##_insn[];                               \
	extern "C" char probe_##name##_end[];                                \
	void name##_unmapped() { probe_##name(0); }
PRIVILEGED_CASES(DECLARE_PRIVILEGED)
#undef DECLARE_PRIVILEGED
extern "C" void probe_mov_cs(word address);
extern "C" char probe_mov_cs_insn[];
extern "C" char probe_mov_cs_end[];

void mov_cs_unmapped() { probe_mov_cs(0); }

/* Register forms of memory-only instructions, memory forms of
 * register-only ones, LOCK on an instruction that does not write memory,
 * 0x82 in 64-bit mode (outside it, group 1; see the segments case), the
 * 0F 3E and 0F 3F escapes, EMMS, XGETBV and
 * XSETBV with a mandatory prefix, the undefined x87 forms and a move with
 * an undefined control register raise #UD; so does MOV to CS, before it
 * reads its source. SYSEXIT and the system instructions LTR, LLDT, LGDT,
 * LIDT, LMSW, INVLPG, XSETBV and MOV with a control register raise #GP(0)
 * outside ring 0, before they read memory, and so do SYSRET and SWAPGS in
 * 64-bit mode. */
bool undefined_faults()
{
	struct Case {
		const char *what;
		void (*work)();
		const char *end;
	};
#define UNDEFINED_ENTRY(name, bytes) {#name, probe_##name, probe_##name##_end},
	const Case cases[] = {UNDEFINED_CASES(UNDEFINED_ENTRY)};
#undef UNDEFINED_ENTRY
	bool ok = install_fault_handlers();
	for (const Case &one : cases) {
		const word insn = reinterpret_cast<word>(one.work);
		ok = ok && delivers(one.what, one.work, address_of(one.end),
				    {sigill, ill_illopn, insn, insn, 6, true, 0});
	}
	const word mov_cs = address_of(probe_mov_cs_insn);
	ok = ok && delivers("MOV CS from memory", mov_cs_unmapped,
			    address_of(probe_mov_cs_end),
			    {sigill, ill_illopn, mov_cs, mov_cs, 6, true, 0});
	const word sysexit = reinterpret_cast<word>(probe_sysexit);
	ok = ok && delivers("SYSEXIT", probe_sysexit,
			    address_of(probe_sysexit_end),
			    {sigsegv, si_kernel, 0, sysexit, 13, true, 0});
	struct Privileged {
		const char *what;
		void (*work)();
		const char *insn;
		const char *end;
	};
#define PRIVILEGED_ENTRY(name, bytes)                                        \
	{#name, name##_unmapped, probe_##name##_insn, probe_##name##_end},
	const Privileged privileged[] = {PRIVILEGED_CASES(PRIVILEGED_ENTRY)};
#undef PRIVILEGED_ENTRY
	for (const Privileged &one : privileged) {
		const word insn = address_of(one.insn);
		ok = ok && delivers(one.what, one.work, address_of(one.end),
				    {sigsegv, si_kernel, 0, insn, 13, true, 0});
	}
	return ok;
}

/* Each takes its operand's address in AX. The first group requires its
 * image aligned to 16 bytes, or for XSAVE and XRSTOR 64, and raises #GP(0)
 * when it is not; the second accepts any alignment. */
#define ALIGNMENT_FAULT_CASES(X)                                             \
	X(fxsave, "0x0f, 0xae, 0x00")                                        \
	X(fxrstor, "0x0f, 0xae, 0x08")                                       \
	X(xsave, "0x0f, 0xae, 0x20")                                         \
	X(xrstor, "0x0f, 0xae, 0x28")
#define ALIGNMENT_FREE_CASES(X)                                              \
	X(movups_load, "0x0f, 0x10, 0x00")                                   \
	X(lddqu, "0xf2, 0x0f, 0xf0, 0x00")                                   \
	X(vaddps, "0xc5, 0xf8, 0x58, 0x00")
#define EMIT_ALIGNMENT(name, bytes)                                          \
	".global probe_align_" #name "\n"                                    \
	"probe_align_" #name ":\n"                                           \
	PROBE_ARG_AX                                                         \
	".global probe_align_" #name "_insn\n"                               \
	"probe_align_" #name "_insn:\n"                                      \
	"	.byte " bytes "\n"                                           \
	".global probe_align_" #name "_end\n"                                \
	"probe_align_" #name "_end:\n"                                       \
	"	ret\n"
__asm__(".text\n" ALIGNMENT_FAULT_CASES(EMIT_ALIGNMENT)
	ALIGNMENT_FREE_CASES(EMIT_ALIGNMENT));
#undef EMIT_ALIGNMENT
#define DECLARE_ALIGNMENT(name, bytes)                                       \
	extern "C" void probe_align_##name(word address);                   \
	extern "C" char probe_align_##name##_insn[];                         \
	extern "C" char probe_align_##name##_end[];
ALIGNMENT_FAULT_CASES(DECLARE_ALIGNMENT)
ALIGNMENT_FREE_CASES(DECLARE_ALIGNMENT)
#undef DECLARE_ALIGNMENT

/* Zeroed but for FXRSTOR's and XRSTOR's MXCSR, so an operand at any offset
 * restores a valid image. */
alignas(64) unsigned char misaligned_buffer[1024];
word alignment_address;
#define ALIGNMENT_WORK(name, bytes)                                          \
	void align_##name() { probe_align_##name(alignment_address); }
ALIGNMENT_FAULT_CASES(ALIGNMENT_WORK)
ALIGNMENT_FREE_CASES(ALIGNMENT_WORK)
#undef ALIGNMENT_WORK

/* FXSAVE, FXRSTOR, XSAVE and XRSTOR raise #GP(0) for a misaligned image
 * before they access it; XSAVE and XRSTOR require 64 bytes, so 16 is
 * misaligned for them. Unaligned SSE moves and VEX arithmetic accept any
 * operand. */
bool alignment_faults()
{
	struct Case {
		const char *what;
		void (*work)();
		const char *insn;
		const char *end;
	};
#define ALIGNMENT_ENTRY(name, bytes)                                         \
	{#name, align_##name, probe_align_##name##_insn, probe_align_##name##_end},
	const Case faulting[] = {ALIGNMENT_FAULT_CASES(ALIGNMENT_ENTRY)};
	const Case unaligned[] = {ALIGNMENT_FREE_CASES(ALIGNMENT_ENTRY)};
#undef ALIGNMENT_ENTRY
	for (unsigned offset = 0; offset < sizeof(misaligned_buffer); ++offset)
		misaligned_buffer[offset] = 0;
	for (unsigned base = 0; base < 128; base += 8) {
		misaligned_buffer[base + 24] = 0x80;
		misaligned_buffer[base + 25] = 0x1f;
	}
	bool ok = install_fault_handlers();
	const word buffer = reinterpret_cast<word>(misaligned_buffer);
	for (word offset = 8; offset <= 16; offset += 8) {
		alignment_address = buffer + offset;
		for (const Case &one : faulting) {
			const bool wide = one.what[0] == 'x';
			if (offset == 16 && !wide)
				continue;
			const word insn = address_of(one.insn);
			ok &= delivers(one.what, one.work, address_of(one.end),
				       {sigsegv, si_kernel, 0, insn, 13, true, 0});
		}
	}
	alignment_address = buffer + 8;
	for (const Case &one : unaligned)
		ok &= delivers_nothing(one.what, one.work, address_of(one.end));
	return ok;
}

/* Each takes its operand's address in AX. CLFLUSH, CLFLUSHOPT and CLWB read
 * their line as far as paging goes: an operand on an unmapped page raises
 * #PF for a read, at the operand's own address and not its line's. CLDEMOTE
 * and the prefetches are hints that never fault. */
#define CACHE_LINE_FAULT_CASES(X)                                            \
	X(clflush, "0x0f, 0xae, 0x38")                                       \
	X(clflushopt, "0x66, 0x0f, 0xae, 0x38")                              \
	X(clwb, "0x66, 0x0f, 0xae, 0x30")
#define CACHE_LINE_HINT_CASES(X)                                             \
	X(cldemote, "0x0f, 0x1c, 0x00")                                      \
	X(prefetcht0, "0x0f, 0x18, 0x08")                                    \
	X(prefetchnta, "0x0f, 0x18, 0x00")                                   \
	X(prefetchw, "0x0f, 0x0d, 0x08")
#define EMIT_CACHE_LINE(name, bytes)                                         \
	".global probe_line_" #name "\n"                                     \
	"probe_line_" #name ":\n"                                            \
	PROBE_ARG_AX                                                         \
	".global probe_line_" #name "_insn\n"                                \
	"probe_line_" #name "_insn:\n"                                       \
	"	.byte " bytes "\n"                                           \
	".global probe_line_" #name "_end\n"                                 \
	"probe_line_" #name "_end:\n"                                        \
	"	ret\n"
__asm__(".text\n" CACHE_LINE_FAULT_CASES(EMIT_CACHE_LINE)
	CACHE_LINE_HINT_CASES(EMIT_CACHE_LINE));
#undef EMIT_CACHE_LINE
#define DECLARE_CACHE_LINE(name, bytes)                                      \
	extern "C" void probe_line_##name(word address);                    \
	extern "C" char probe_line_##name##_insn[];                          \
	extern "C" char probe_line_##name##_end[];
CACHE_LINE_FAULT_CASES(DECLARE_CACHE_LINE)
CACHE_LINE_HINT_CASES(DECLARE_CACHE_LINE)
#undef DECLARE_CACHE_LINE

constexpr word cache_line_operand = 0x10025;
#define CACHE_LINE_WORK(name, bytes)                                         \
	void line_##name() { probe_line_##name(cache_line_operand); }
CACHE_LINE_FAULT_CASES(CACHE_LINE_WORK)
CACHE_LINE_HINT_CASES(CACHE_LINE_WORK)
#undef CACHE_LINE_WORK

bool cache_line_flushes()
{
	struct Case {
		const char *what;
		void (*work)();
		const char *insn;
		const char *end;
	};
#define CACHE_LINE_ENTRY(name, bytes)                                        \
	{#name, line_##name, probe_line_##name##_insn, probe_line_##name##_end},
	const Case faulting[] = {CACHE_LINE_FAULT_CASES(CACHE_LINE_ENTRY)};
	const Case hints[] = {CACHE_LINE_HINT_CASES(CACHE_LINE_ENTRY)};
#undef CACHE_LINE_ENTRY
	bool ok = install_fault_handlers();
	for (const Case &one : faulting)
		ok &= delivers(one.what, one.work, address_of(one.end),
			       {sigsegv, segv_maperr, cache_line_operand,
				address_of(one.insn), 14, true, 4});
	for (const Case &one : hints)
		ok &= delivers_nothing(one.what, one.work, address_of(one.end));
	return ok;
}

/* XSAVE, XSAVEOPT and XRSTOR with RFBM in (eax) at (ecx) and FXSAVE and
 * FXRSTOR touch only the bytes their components occupy: the legacy area and
 * the header, and the AVX component only when RFBM selects it. */
#define EXTENT_CASES(X)                                                      \
	X(xsave, "xsave (" PROBE_CX ")")                                     \
	X(xsaveopt, "xsaveopt (" PROBE_CX ")")                               \
	X(xrstor, "xrstor (" PROBE_CX ")")                                   \
	X(fxsave, "fxsave (" PROBE_CX ")")                                   \
	X(fxrstor, "fxrstor (" PROBE_CX ")")
#define EMIT_EXTENT(name, instruction)                                       \
	".global probe_extent_" #name "\n"                                   \
	"probe_extent_" #name ":\n"                                          \
	PROBE_ARG_CX_AX                                                      \
	"	xorl %edx, %edx\n"                                          \
	".global probe_extent_" #name "_insn\n"                              \
	"probe_extent_" #name "_insn:\n"                                     \
	"	" instruction "\n"                                          \
	".global probe_extent_" #name "_end\n"                               \
	"probe_extent_" #name "_end:\n"                                      \
	"	ret\n"
__asm__(".text\n" EXTENT_CASES(EMIT_EXTENT));
#undef EMIT_EXTENT
#define DECLARE_EXTENT(name, instruction)                                    \
	extern "C" void probe_extent_##name(word address, word rfbm);        \
	extern "C" char probe_extent_##name##_end[];
EXTENT_CASES(DECLARE_EXTENT)
#undef DECLARE_EXTENT

#if defined(__x86_64__)
constexpr long nr_mprotect_extent = 10;
#else
constexpr long nr_mprotect_extent = 125;
#endif
alignas(4096) unsigned char extent_pages[2 * 4096];
word extent_address;
word extent_rfbm;
#define EXTENT_WORK(name, instruction)                                       \
	void extent_##name() { probe_extent_##name(extent_address, extent_rfbm); }
EXTENT_CASES(EXTENT_WORK)
#undef EXTENT_WORK

bool state_area_extents()
{
	unsigned char *const guard = extent_pages + 4096;
	if (raw_syscall(nr_mprotect_extent, reinterpret_cast<long>(guard), 4096,
			0) != 0) {
		put("  mprotect failed\n");
		return false;
	}
	bool ok = install_fault_handlers();
	struct Case {
		const char *what;
		void (*work)();
		const char *end;
		word length;
		word rfbm;
	};
#define EXTENT_ENTRY(name, length, rfbm)                                     \
	{#name " with RFBM " #rfbm " in " #length " bytes", extent_##name,   \
	 probe_extent_##name##_end, length, rfbm}
	const Case cases[] = {
		EXTENT_ENTRY(xsave, 576, 0), EXTENT_ENTRY(xsave, 576, 1),
		EXTENT_ENTRY(xsave, 576, 3), EXTENT_ENTRY(xsaveopt, 576, 3),
		EXTENT_ENTRY(xrstor, 576, 3), EXTENT_ENTRY(xrstor, 576, 0),
		EXTENT_ENTRY(fxsave, 512, 0), EXTENT_ENTRY(fxrstor, 512, 0),
	};
#undef EXTENT_ENTRY
	for (const Case &one : cases) {
		/* The image restored is a valid one: an x87 control word of
		 * 0x37f, MXCSR 0x1f80, and nothing marked in use. */
		unsigned char *const area = guard - one.length;
		for (word offset = 0; offset < one.length; ++offset)
			area[offset] = 0;
		area[0] = 0x7f;
		area[1] = 0x03;
		area[24] = 0x80;
		area[25] = 0x1f;
		extent_address = reinterpret_cast<word>(area);
		extent_rfbm = one.rfbm;
		ok &= delivers_nothing(one.what, one.work, address_of(one.end));
	}
	return ok;
}

extern "C" void probe_pf_read(word address);
extern "C" char probe_pf_read_insn[];
extern "C" char probe_pf_read_end[];
extern "C" void probe_pf_write(word address);
extern "C" char probe_pf_write_insn[];
extern "C" char probe_pf_write_end[];
extern "C" void probe_pf_fetch(word address);
extern "C" char probe_pf_fetch_end[];
extern "C" void probe_pf_return(word address);
extern "C" char probe_pf_return_end[];

void pf_read() { probe_pf_read(unmapped_page); }
void pf_write() { probe_pf_write(unmapped_page); }
void pf_fetch() { probe_pf_fetch(unmapped_page); }
void pf_jump_null() { probe_pf_fetch(0); }
void pf_return_null() { probe_pf_return(0); }

/* A user access to an unmapped page raises #PF with the U bit, 4, in its
 * error code, and W, 2, for a write or I/D, 0x10, for a fetch. A jump or a
 * return to address 0, a null function pointer's call, fetches from it. */
bool page_faults_are_user()
{
	const word page = unmapped_page;
	bool ok = install_fault_handlers();
	ok = ok && delivers("jump to 0", pf_jump_null,
			    address_of(probe_pf_fetch_end),
			    {sigsegv, segv_maperr, 0, 0, 14, true, 0x14});
	ok = ok && delivers("return to 0", pf_return_null,
			    address_of(probe_pf_return_end),
			    {sigsegv, segv_maperr, 0, 0, 14, true, 0x14});
	ok = ok && delivers("read", pf_read, address_of(probe_pf_read_end),
			    {sigsegv, segv_maperr, page,
			     address_of(probe_pf_read_insn), 14, true, 4});
	ok = ok && delivers("write", pf_write, address_of(probe_pf_write_end),
			    {sigsegv, segv_maperr, page,
			     address_of(probe_pf_write_insn), 14, true, 6});
	ok = ok && delivers("fetch", pf_fetch, address_of(probe_pf_fetch_end),
			    {sigsegv, segv_maperr, page, page, 14, true, 0x14});
	return ok;
}

#if defined(__x86_64__)
constexpr long nr_mprotect_protection = 10;
#else
constexpr long nr_mprotect_protection = 125;
#endif
alignas(4096) unsigned char protection_pages[3 * 4096];
word protection_address;
void pf_protection_write() { probe_pf_write(protection_address); }
void pf_protection_read() { probe_pf_read(protection_address); }
void pf_protection_fetch() { probe_pf_fetch(protection_address); }

/* A fault on a page the task has mapped and touched sets P, 1, in its error
 * code when the access broke the page's protection -- a store to a read-only
 * page, a fetch from one without execute permission -- and leaves it clear
 * when the page is not accessible at all, as a PROT_NONE page is. */
bool page_fault_protection()
{
	unsigned char *const read_only = protection_pages;
	unsigned char *const no_access = protection_pages + 4096;
	unsigned char *const data = protection_pages + 2 * 4096;
	for (word page = 0; page < 3; ++page) {
		protection_pages[page * 4096 + 5] = 1;
		protection_pages[page * 4096 + 6] =
			protection_pages[page * 4096 + 5];
	}
	if (raw_syscall(nr_mprotect_protection,
			reinterpret_cast<long>(read_only), 4096, 1) != 0 ||
	    raw_syscall(nr_mprotect_protection,
			reinterpret_cast<long>(no_access), 4096, 0) != 0 ||
	    raw_syscall(nr_mprotect_protection, reinterpret_cast<long>(data),
			4096, 3) != 0) {
		put("  mprotect failed\n");
		return false;
	}
	constexpr long segv_accerr = 2;
	bool ok = install_fault_handlers();
	const word store_insn = address_of(probe_pf_write_insn);
	const word load_insn = address_of(probe_pf_read_insn);
	protection_address = reinterpret_cast<word>(read_only) + 5;
	ok &= delivers("store to a present read-only page", pf_protection_write,
		       address_of(probe_pf_write_end),
		       {sigsegv, segv_accerr, protection_address, store_insn, 14,
			true, 7});
	protection_address = reinterpret_cast<word>(no_access) + 5;
	ok &= delivers("load from a PROT_NONE page", pf_protection_read,
		       address_of(probe_pf_read_end),
		       {sigsegv, segv_accerr, protection_address, load_insn, 14,
			true, 4});
	ok &= delivers("store to a PROT_NONE page", pf_protection_write,
		       address_of(probe_pf_write_end),
		       {sigsegv, segv_accerr, protection_address, store_insn, 14,
			true, 6});
	ok &= delivers("fetch from a PROT_NONE page", pf_protection_fetch,
		       address_of(probe_pf_fetch_end),
		       {sigsegv, segv_accerr, protection_address,
			protection_address, 14, true, 0x14});
	protection_address = reinterpret_cast<word>(data) + 5;
	ok &= delivers("fetch from a present page without execute",
		       pf_protection_fetch, address_of(probe_pf_fetch_end),
		       {sigsegv, segv_accerr, protection_address,
			protection_address, 14, true, 0x15});
	return ok;
}

extern "C" void probe_int_1();
extern "C" char probe_int_1_end[];
extern "C" void probe_int_2();
extern "C" char probe_int_2_end[];
extern "C" void probe_int_41();
extern "C" char probe_int_41_end[];
extern "C" void probe_int_ff();
extern "C" char probe_int_ff_end[];

/* INT n that its gate refuses to user mode raises #GP, a fault: SIGSEGV
 * with SI_KERNEL, and an error code naming the IDT entry, (n << 3) | 2. */
bool int_n_faults()
{
	struct Case {
		const char *what;
		void (*work)();
		const char *end;
		word error;
	};
	const Case cases[] = {
		{"INT 1", probe_int_1, probe_int_1_end, 0xa},
		{"INT 2", probe_int_2, probe_int_2_end, 0x12},
		{"INT 0x41", probe_int_41, probe_int_41_end, 0x20a},
		{"INT 0xff", probe_int_ff, probe_int_ff_end, 0x7fa},
	};
	bool ok = install_fault_handlers();
	for (const Case &one : cases) {
		const word insn = reinterpret_cast<word>(one.work);
		ok = ok && delivers(one.what, one.work, address_of(one.end),
				    {sigsegv, si_kernel, 0, insn, 13, true,
				     one.error});
	}
	return ok;
}

#if defined(__x86_64__)
extern "C" void probe_nc_load(word address);
extern "C" char probe_nc_load_insn[];
extern "C" char probe_nc_load_end[];
extern "C" void probe_nc_store(word address);
extern "C" char probe_nc_store_insn[];
extern "C" char probe_nc_store_end[];
extern "C" void probe_nc_rbp(word address);
extern "C" char probe_nc_rbp_insn[];
extern "C" char probe_nc_rbp_end[];
extern "C" void probe_nc_rbp_index(word address);
extern "C" char probe_nc_rbp_index_insn[];
extern "C" char probe_nc_rbp_index_end[];
extern "C" void probe_nc_movs(word address);
extern "C" char probe_nc_movs_insn[];
extern "C" char probe_nc_movs_end[];
extern "C" void probe_nc_push(word address);
extern "C" char probe_nc_push_insn[];
extern "C" char probe_nc_push_end[];

/* Non-canonical under 4-level and 5-level paging alike. */
constexpr word noncanonical = 0x8000000000000000UL;
constexpr word sa_onstack = 0x08000000;

alignas(16) unsigned char alternate_stack[16384];

void nc_load() { probe_nc_load(noncanonical); }
void nc_store() { probe_nc_store(noncanonical); }
void nc_rbp() { probe_nc_rbp(noncanonical); }
void nc_rbp_index() { probe_nc_rbp_index(noncanonical); }
void nc_movs() { probe_nc_movs(noncanonical); }
void nc_push() { probe_nc_push(noncanonical); }

/* A push through a non-canonical RSP can deliver its signal only on an
 * alternate stack. */
bool handle_on_alternate_stack(long signal,
			       void (*handler)(int, void *, void *) = record_fault)
{
	const struct {
		word sp;
		long flags;
		word size;
	} stack = {reinterpret_cast<word>(alternate_stack), 0,
		   sizeof(alternate_stack)};
	KernelSigaction action = {};
	action.handler = reinterpret_cast<word>(handler);
	action.flags = sa_siginfo | sa_restorer | sa_onstack;
	action.restorer = reinterpret_cast<word>(&probe_restorer);
	return raw_syscall(nr_sigaltstack, reinterpret_cast<long>(&stack), 0) ==
		       0 &&
	       raw_syscall(nr_rt_sigaction, signal,
			   reinterpret_cast<long>(&action), 0, 8) == 0;
}

/* A non-canonical address raises #GP, SIGSEGV with SI_KERNEL, or #SS,
 * SIGBUS with SI_KERNEL, when the stack segment forms it; neither has an
 * address, both are faults. */
bool noncanonical_faults()
{
	struct Case {
		const char *what;
		void (*work)();
		const char *insn;
		const char *end;
		long signal;
		word vector;
	};
	const Case cases[] = {
		{"load", nc_load, probe_nc_load_insn, probe_nc_load_end,
		 sigsegv, 13},
		{"store", nc_store, probe_nc_store_insn, probe_nc_store_end,
		 sigsegv, 13},
		{"load based on RBP", nc_rbp, probe_nc_rbp_insn,
		 probe_nc_rbp_end, sigbus, 12},
		{"load indexed by RBP", nc_rbp_index, probe_nc_rbp_index_insn,
		 probe_nc_rbp_index_end, sigsegv, 13},
		{"MOVSB", nc_movs, probe_nc_movs_insn, probe_nc_movs_end,
		 sigsegv, 13},
		{"push", nc_push, probe_nc_push_insn, probe_nc_push_end,
		 sigbus, 12},
	};
	bool ok = install_fault_handlers() && handle_on_alternate_stack(sigbus);
	for (const Case &one : cases)
		ok = ok && delivers(one.what, one.work, address_of(one.end),
				    {one.signal, si_kernel, 0,
				     address_of(one.insn), one.vector, true,
				     0});
	return ok;
}

void nc_push_unaligned() { probe_nc_push(noncanonical + 16); }

/* A stack pointer of zero or outside the task's memory, 128 KiB aligned or
 * not, serves a system call and faults a push with #SS however often it
 * comes on one CPU. */
bool bad_stack_pointers()
{
	const Delivery stack_fault = {sigbus, si_kernel, 0,
				      address_of(probe_nc_push_insn), 12, true,
				      0};
	const word end = address_of(probe_nc_push_end);
	bool ok = pin_to_one_cpu() && install_fault_handlers() &&
		  handle_on_alternate_stack(sigbus);
	for (word round = 0; ok && round < bad_stack_rounds; ++round)
		ok = calls_on("RSP 0", 0) &&
		     calls_on("RSP 0x8000000000000000", noncanonical) &&
		     calls_on("RSP 0x8000000000000010", noncanonical + 16) &&
		     delivers("a push with RSP 0x8000000000000000", nc_push,
			      end, stack_fault) &&
		     delivers("a push with RSP 0x8000000000000010",
			      nc_push_unaligned, end, stack_fault);
	return ok;
}
#endif

#if defined(__x86_64__)
/* Each divide loads RAX, RDX and RCX from (state)[0..2], runs the sign or
 * zero extension it names right before it, divides, and stores RAX, RDX
 * and RCX to (state)[3..5]; a memory divisor is (state)[2]. */
#define PROBE_DIVIDE_AFTER(name, extend, insn)                            \
	".global probe_de_" name "\n"                                     \
	"probe_de_" name ":\n"                                            \
	"	movq (%rdi), %rax\n"                                      \
	"	movq 8(%rdi), %rdx\n"                                     \
	"	movq 16(%rdi), %rcx\n"                                    \
	"	" extend "\n"                                             \
	".global probe_de_" name "_insn\n"                                \
	"probe_de_" name "_insn:\n"                                       \
	"	" insn "\n"                                               \
	".global probe_de_" name "_end\n"                                 \
	"probe_de_" name "_end:\n"                                        \
	"	movq %rax, 24(%rdi)\n"                                    \
	"	movq %rdx, 32(%rdi)\n"                                    \
	"	movq %rcx, 40(%rdi)\n"                                    \
	"	ret\n"
#define PROBE_DIVIDE(name, insn) PROBE_DIVIDE_AFTER(name, "", insn)

__asm__(".text\n"
	PROBE_DIVIDE("div8", "divb %cl")
	PROBE_DIVIDE("idiv8", "idivb %cl")
	PROBE_DIVIDE("div16", "divw %cx")
	PROBE_DIVIDE("idiv16", "idivw %cx")
	PROBE_DIVIDE("div32", "divl %ecx")
	PROBE_DIVIDE("idiv32", "idivl %ecx")
	PROBE_DIVIDE("div32m", "divl 16(%rdi)")
	PROBE_DIVIDE("div64", "divq %rcx")
	PROBE_DIVIDE("idiv64", "idivq %rcx")
	PROBE_DIVIDE("idiv64m", "idivq 16(%rdi)")
	PROBE_DIVIDE_AFTER("cqo_idiv64", "cqto", "idivq %rcx")
	PROBE_DIVIDE_AFTER("cdq_idiv32", "cltd", "idivl %ecx")
	PROBE_DIVIDE_AFTER("xor_div64", "xorl %edx, %edx", "divq %rcx")
	PROBE_DIVIDE_AFTER("xor_div32", "xorl %edx, %edx", "divl %ecx"));

#define PROBE_DIVIDE_DECLARE(name)                                        \
	extern "C" void probe_de_##name(word *state);                     \
	extern "C" char probe_de_##name##_insn[];                         \
	extern "C" char probe_de_##name##_end[];                          \
	void de_##name() { probe_de_##name(divide_state); }

word divide_state[6];

PROBE_DIVIDE_DECLARE(div8)
PROBE_DIVIDE_DECLARE(idiv8)
PROBE_DIVIDE_DECLARE(div16)
PROBE_DIVIDE_DECLARE(idiv16)
PROBE_DIVIDE_DECLARE(div32)
PROBE_DIVIDE_DECLARE(idiv32)
PROBE_DIVIDE_DECLARE(div32m)
PROBE_DIVIDE_DECLARE(div64)
PROBE_DIVIDE_DECLARE(idiv64)
PROBE_DIVIDE_DECLARE(idiv64m)
PROBE_DIVIDE_DECLARE(cqo_idiv64)
PROBE_DIVIDE_DECLARE(cdq_idiv32)
PROBE_DIVIDE_DECLARE(xor_div64)
PROBE_DIVIDE_DECLARE(xor_div32)

constexpr long fpe_intdiv = 1;

/* A zero divisor or a quotient too wide for its destination raises #DE, a
 * fault: SIGFPE with FPE_INTDIV at the instruction, which has written
 * nothing. A quotient at the edge of its range does not fault, and neither
 * does a wide one from a dividend that CQO, CDQ or a zeroed RDX just
 * extended. */
bool divide_errors()
{
	struct Case {
		const char *what;
		void (*work)();
		const char *insn;
		const char *end;
		word in[3];
		bool faults;
		word out[2];
	};
#define PROBE_DIVIDE_CASE(name) de_##name, probe_de_##name##_insn,        \
		probe_de_##name##_end
	const Case cases[] = {
		{"DIV r8 overflow", PROBE_DIVIDE_CASE(div8),
		 {0x1234, 0x5a5a5a5a5a5a5a5a, 0x10}, true, {}},
		{"DIV r8 by zero", PROBE_DIVIDE_CASE(div8),
		 {5, 0, 0x100}, true, {}},
		{"IDIV r8 overflow", PROBE_DIVIDE_CASE(idiv8),
		 {0xff80, 0, 0xff}, true, {}},
		{"DIV r16 overflow", PROBE_DIVIDE_CASE(div16),
		 {0, 0x10, 0x10}, true, {}},
		{"IDIV r16 overflow", PROBE_DIVIDE_CASE(idiv16),
		 {0x8000, 0xffff, 0xffff}, true, {}},
		{"DIV r32 overflow", PROBE_DIVIDE_CASE(div32),
		 {0, 0x10, 0x10}, true, {}},
		{"IDIV r32 overflow", PROBE_DIVIDE_CASE(idiv32),
		 {0x80000000, 0xffffffff, 0xffffffff}, true, {}},
		{"DIV m32 by zero", PROBE_DIVIDE_CASE(div32m),
		 {1, 0, 0}, true, {}},
		{"DIV r64 overflow", PROBE_DIVIDE_CASE(div64),
		 {0, 0x10, 0x10}, true, {}},
		{"IDIV r64 of INT64_MIN by -1", PROBE_DIVIDE_CASE(idiv64),
		 {0x8000000000000000, ~0UL, ~0UL}, true, {}},
		{"IDIV r64 of 2^64 by 2", PROBE_DIVIDE_CASE(idiv64),
		 {0, 1, 2}, true, {}},
		{"IDIV m64 of -2^65 by 2", PROBE_DIVIDE_CASE(idiv64m),
		 {0, 0xfffffffffffffffe, 2}, true, {}},
		{"DIV r8 to 0xff", PROBE_DIVIDE_CASE(div8),
		 {0x77000000000001fe, 3, 2}, false,
		 {0x77000000000000ff, 3}},
		{"IDIV r16 to -32768", PROBE_DIVIDE_CASE(idiv16),
		 {0x1111111111110000, 0x222222222222ffff, 2}, false,
		 {0x1111111111118000, 0x2222222222220000}},
		{"IDIV r32 to INT32_MIN", PROBE_DIVIDE_CASE(idiv32),
		 {0x1111111100000000, 0xffffffff, 2}, false,
		 {0x80000000, 0}},
		{"IDIV r64 to INT64_MIN", PROBE_DIVIDE_CASE(idiv64),
		 {0, ~0UL, 2}, false, {0x8000000000000000, 0}},
		{"IDIV r64 after CQO to a wide quotient",
		 PROBE_DIVIDE_CASE(cqo_idiv64), {399960001, 0x5a5a5a5a5a5a5a5a, 97},
		 false, {0x3eeaa2, 0x5f}},
		{"IDIV r64 after CQO of INT64_MIN by -1",
		 PROBE_DIVIDE_CASE(cqo_idiv64), {0x8000000000000000, ~0UL, ~0UL},
		 true, {}},
		{"IDIV r32 after CDQ to a wide quotient",
		 PROBE_DIVIDE_CASE(cdq_idiv32), {399960001, 0x5a5a5a5a5a5a5a5a, 97},
		 false, {0x3eeaa2, 0x5f}},
		{"IDIV r32 after CDQ of INT32_MIN by -1",
		 PROBE_DIVIDE_CASE(cdq_idiv32), {0x80000000, 0xffffffff, 0xffffffff},
		 true, {}},
		{"DIV r64 after zeroing RDX to a wide quotient",
		 PROBE_DIVIDE_CASE(xor_div64), {0xfffffffffffffff0, 0x5a5a, 16},
		 false, {0x0fffffffffffffff, 0}},
		{"DIV r32 after zeroing EDX to a wide quotient",
		 PROBE_DIVIDE_CASE(xor_div32), {0x77777777fffffff0, 0x5a5a, 16},
		 false, {0x0fffffff, 0}},
	};
#undef PROBE_DIVIDE_CASE
	bool ok = install_fault_handlers() && install(sigfpe, record_fault);
	for (const Case &one : cases) {
		for (unsigned index = 0; index < 3; ++index) {
			divide_state[index] = one.in[index];
			divide_state[index + 3] = 0;
		}
		const word insn = address_of(one.insn);
		const bool delivered =
			one.faults ? delivers(one.what, one.work,
					      address_of(one.end),
					      {sigfpe, fpe_intdiv, insn, insn,
					       0, true, 0})
				   : delivers_nothing(one.what, one.work,
						      address_of(one.end));
		const word rax = one.faults ? one.in[0] : one.out[0];
		const word rdx = one.faults ? one.in[1] : one.out[1];
		bool kept = expect("RAX", divide_state[3], rax);
		kept &= expect("RDX", divide_state[4], rdx);
		kept &= expect("RCX", divide_state[5], one.in[2]);
		if (!kept) {
			put("  after ");
			put(one.what);
			put("\n");
		}
		ok &= delivered && kept;
	}
	return ok;
}
#endif

#if defined(__x86_64__)
/* VPMASKMOVD and VMASKMOVPS of YMM0 at (state)[0] under the mask
 * (state)[4..7]; YMM0 starts as (state)[8..11] and the load leaves it in
 * (state)[12..15]. */
__asm__(".text\n"
	".global probe_mask_load\n"
	"probe_mask_load:\n"
	"	movq (%rdi), %rax\n"
	"	vmovdqu 32(%rdi), %ymm1\n"
	"	vmovdqu 64(%rdi), %ymm0\n"
	".global probe_mask_load_insn\n"
	"probe_mask_load_insn:\n"
	"	vpmaskmovd (%rax), %ymm1, %ymm0\n"
	".global probe_mask_load_end\n"
	"probe_mask_load_end:\n"
	"	vmovdqu %ymm0, 96(%rdi)\n"
	"	vzeroupper\n"
	"	ret\n"
	".global probe_mask_store\n"
	"probe_mask_store:\n"
	"	movq (%rdi), %rax\n"
	"	vmovdqu 32(%rdi), %ymm1\n"
	"	vmovdqu 64(%rdi), %ymm0\n"
	".global probe_mask_store_insn\n"
	"probe_mask_store_insn:\n"
	"	vmaskmovps %ymm0, %ymm1, (%rax)\n"
	".global probe_mask_store_end\n"
	"probe_mask_store_end:\n"
	"	vzeroupper\n"
	"	ret\n"
	/* MASKMOVDQU and MASKMOVQ at (address) with a zero mask. */
	".global probe_maskmovdqu_zero\n"
	"probe_maskmovdqu_zero:\n"
	"	pxor %xmm1, %xmm1\n"
	"	pcmpeqb %xmm0, %xmm0\n"
	".global probe_maskmovdqu_zero_insn\n"
	"probe_maskmovdqu_zero_insn:\n"
	"	maskmovdqu %xmm1, %xmm0\n"
	".global probe_maskmovdqu_zero_end\n"
	"probe_maskmovdqu_zero_end:\n"
	"	ret\n"
	".global probe_maskmovq_zero\n"
	"probe_maskmovq_zero:\n"
	"	pxor %mm1, %mm1\n"
	"	pcmpeqb %mm0, %mm0\n"
	".global probe_maskmovq_zero_insn\n"
	"probe_maskmovq_zero_insn:\n"
	"	maskmovq %mm1, %mm0\n"
	".global probe_maskmovq_zero_end\n"
	"probe_maskmovq_zero_end:\n"
	"	emms\n"
	"	ret\n");

extern "C" void probe_mask_load(word *state);
extern "C" char probe_mask_load_insn[];
extern "C" char probe_mask_load_end[];
extern "C" void probe_mask_store(word *state);
extern "C" char probe_mask_store_insn[];
extern "C" char probe_mask_store_end[];
extern "C" void probe_maskmovdqu_zero(unsigned char *address);
extern "C" char probe_maskmovdqu_zero_insn[];
extern "C" char probe_maskmovdqu_zero_end[];
extern "C" void probe_maskmovq_zero(unsigned char *address);
extern "C" char probe_maskmovq_zero_insn[];
extern "C" char probe_maskmovq_zero_end[];

constexpr long nr_munmap = 11;
constexpr word page_bytes = 4096;

alignas(32) word mask_state[16];
/* Three pages no other case touches; the probe unmaps the last. */
alignas(4096) unsigned char mask_pages[3 * page_bytes];

void mask_load() { probe_mask_load(mask_state); }
void mask_store() { probe_mask_store(mask_state); }
unsigned char *maskmov_target;
void maskmovdqu_zero() { probe_maskmovdqu_zero(maskmov_target); }
void maskmovq_zero() { probe_maskmovq_zero(maskmov_target); }

void set_mask_state(const unsigned char *address, unsigned lanes)
{
	auto *values = reinterpret_cast<u32 *>(&mask_state[4]);
	mask_state[0] = reinterpret_cast<word>(address);
	for (unsigned lane = 0; lane < 8; ++lane) {
		values[lane] = (lanes >> lane) & 1U ? 0x80000000U : 0;
		values[lane + 8] = 0x11111111U * (lane + 1);
		values[lane + 16] = 0;
	}
}

/* Checks the first (count) lanes at (bytes): each in (lanes) holds its
 * value and the others zero. */
bool expect_lanes(const char *what, const unsigned char *bytes,
		  unsigned lanes, unsigned count = 8)
{
	const auto *values = reinterpret_cast<const volatile u32 *>(bytes);
	bool ok = true;
	for (unsigned lane = 0; lane < count; ++lane)
		ok &= expect(what, values[lane],
			     (lanes >> lane) & 1U ? 0x11111111U * (lane + 1) : 0);
	return ok;
}

void clear_below(unsigned char *page)
{
	volatile unsigned char *bytes = page - 32;
	for (unsigned offset = 0; offset < 32; ++offset)
		bytes[offset] = 0;
}

/* VMASKMOV and VPMASKMOV access only their enabled lanes: a disabled lane
 * on an unmapped page does not fault and a load zeroes it, every enabled
 * lane completes when the first one misses a cold page, and an enabled
 * lane on an unmapped page raises #PF at its address. MASKMOVDQU and
 * MASKMOVQ, unlike them, fault on any page their operand touches. */
bool masked_moves()
{
	unsigned char *const cold = mask_pages;
	unsigned char *const unmapped = mask_pages + 2 * page_bytes;
	unsigned char *const edge = unmapped - 16;
	if (raw_syscall(nr_munmap, reinterpret_cast<long>(unmapped),
			page_bytes) != 0) {
		put("  munmap failed\n");
		return false;
	}
	bool ok = install_fault_handlers();
	const auto *loaded = reinterpret_cast<const unsigned char *>(
		&mask_state[12]);

	set_mask_state(cold, 0xff);
	ok &= delivers_nothing("VPMASKMOVD from a cold page", mask_load,
			       address_of(probe_mask_load_end));
	ok &= expect_lanes("cold lane", loaded, 0);

	set_mask_state(edge, 0x0f);
	ok &= delivers_nothing("VMASKMOVPS beside an unmapped page", mask_store,
			       address_of(probe_mask_store_end));
	ok &= expect_lanes("stored lane", edge, 0x0f, 4);

	set_mask_state(edge, 0x0f);
	ok &= delivers_nothing("VPMASKMOVD beside an unmapped page", mask_load,
			       address_of(probe_mask_load_end));
	ok &= expect_lanes("loaded lane", loaded, 0x0f);

	/* A masked store writes nothing when it faults. x86 reports the first
	 * byte of the lowest enabled lane when that misses; when only the
	 * second page does and some enabled lane starts below it, the last byte
	 * of the highest enabled lane, whichever lane that is, and the lane
	 * that straddles the edge counts as starting below. A load reports the
	 * lowest enabled lane that reaches the page, from the page's own first
	 * byte if that lane starts before it. */
	constexpr long nr_mprotect_pages = 10;
	constexpr long segv_accerr = 2;
	const auto unmapped_address = reinterpret_cast<word>(unmapped);
	const auto store_at = [&](const char *what, unsigned char *address,
				  unsigned lanes, word fault) {
		clear_below(unmapped);
		set_mask_state(address, lanes);
		return delivers(what, mask_store,
				address_of(probe_mask_store_end),
				{sigsegv, segv_maperr, fault,
				 address_of(probe_mask_store_insn), 14, true, 6});
	};
	ok &= store_at("VMASKMOVPS across the page edge", edge, 0x1f,
		       unmapped_address + 3);
	ok &= expect_lanes("lane of a store that faulted", edge, 0, 4);
	ok &= store_at("VMASKMOVPS onto the page from its start", edge, 0x30,
		       unmapped_address);
	ok &= expect_lanes("lane of a store that faulted", edge, 0, 4);
	ok &= store_at("VMASKMOVPS across the edge to its last lane", edge,
		       0x8f, unmapped_address + 15);
	ok &= expect_lanes("lane of a store that faulted", edge, 0, 4);
	ok &= store_at("VMASKMOVPS with a lane over the edge", unmapped - 2,
		       0x01, unmapped_address + 1);
	ok &= expect_lanes("byte of a store that faulted", unmapped - 8, 0, 2);
	set_mask_state(edge, 0x1f);
	ok &= delivers("VPMASKMOVD from an unmapped page", mask_load,
		       address_of(probe_mask_load_end),
		       {sigsegv, segv_maperr, unmapped_address,
			address_of(probe_mask_load_insn), 14, true, 4});
	set_mask_state(unmapped - 2, 0x01);
	ok &= delivers("VPMASKMOVD with a lane over the edge", mask_load,
		       address_of(probe_mask_load_end),
		       {sigsegv, segv_maperr, unmapped_address,
			address_of(probe_mask_load_insn), 14, true, 4});
	set_mask_state(unmapped + 8, 0x06);
	ok &= delivers("VPMASKMOVD from inside the unmapped page", mask_load,
		       address_of(probe_mask_load_end),
		       {sigsegv, segv_maperr, unmapped_address + 12,
			address_of(probe_mask_load_insn), 14, true, 4});

	/* With the lower page the one that misses, only a lane that starts on
	 * it can fault. */
	unsigned char *const lower = mask_pages;
	unsigned char *const upper = mask_pages + page_bytes;
	unsigned char *const seam = upper - 16;
	if (raw_syscall(nr_mprotect_pages, reinterpret_cast<long>(lower),
			page_bytes, 0) != 0) {
		put("  mprotect failed\n");
		return false;
	}
	set_mask_state(seam, 0x1e);
	ok &= delivers("VMASKMOVPS into the page below", mask_store,
		       address_of(probe_mask_store_end),
		       {sigsegv, segv_accerr,
			reinterpret_cast<word>(seam) + 4,
			address_of(probe_mask_store_insn), 14, true, 6});
	set_mask_state(seam, 0x1e);
	ok &= delivers("VPMASKMOVD from the page below", mask_load,
		       address_of(probe_mask_load_end),
		       {sigsegv, segv_accerr,
			reinterpret_cast<word>(seam) + 4,
			address_of(probe_mask_load_insn), 14, true, 4});
	set_mask_state(seam, 0x30);
	ok &= delivers_nothing("VMASKMOVPS wholly above the page below",
			       mask_store, address_of(probe_mask_store_end));
	const auto *above = reinterpret_cast<const volatile u32 *>(upper);
	ok &= expect("lane above", above[0], 0x55555555U);
	ok &= expect("lane above", above[1], 0x66666666U);

	/* MASKMOVDQU and MASKMOVQ, unlike them, fault on any page their operand
	 * touches, whatever the mask selects, and a zero mask does too.
	 * MASKMOVDQU checks its high quadword first, then the low one, and a
	 * quadword's first byte before its last. */
	const auto maskmovdqu_at = [&](const char *what, unsigned char *address,
				       word fault) {
		maskmov_target = address;
		return delivers(what, maskmovdqu_zero,
				address_of(probe_maskmovdqu_zero_end),
				{sigsegv, segv_maperr, fault,
				 address_of(probe_maskmovdqu_zero_insn), 14,
				 true, 6});
	};
	const auto maskmovq_at = [&](const char *what, unsigned char *address,
				     word fault) {
		maskmov_target = address;
		return delivers(what, maskmovq_zero,
				address_of(probe_maskmovq_zero_end),
				{sigsegv, segv_maperr, fault,
				 address_of(probe_maskmovq_zero_insn), 14, true,
				 6});
	};
	ok &= maskmovdqu_at("MASKMOVDQU with a zero mask onto an unmapped page",
			    unmapped - 8, unmapped_address);
	ok &= maskmovdqu_at("MASKMOVDQU with its high quadword over the edge",
			    unmapped - 12, unmapped_address);
	ok &= maskmovdqu_at("MASKMOVDQU with its low quadword over the edge",
			    unmapped - 4, unmapped_address + 4);
	ok &= maskmovdqu_at("MASKMOVDQU inside an unmapped page", unmapped + 4,
			    unmapped_address + 12);
	maskmov_target = unmapped - 16;
	ok &= delivers_nothing("MASKMOVDQU up to the unmapped page",
			       maskmovdqu_zero,
			       address_of(probe_maskmovdqu_zero_end));
	ok &= maskmovq_at("MASKMOVQ with a zero mask onto an unmapped page",
			  unmapped - 4, unmapped_address);
	ok &= maskmovq_at("MASKMOVQ inside an unmapped page", unmapped + 3,
			  unmapped_address + 3);
	return ok;
}

/* Each gather loads YMM0 (the destination) from (state)[8..11], YMM1 (the
 * mask) from (state)[4..7] and YMM2 (the indices) from (state)[12..15],
 * gathers from the base (state)[0] at scale 1, and leaves YMM0 in
 * (state)[16..19] and YMM1 in (state)[20..23]. */
#define PROBE_GATHER(name, instruction)                                      \
	".global probe_gather_" #name "\n"                                   \
	"probe_gather_" #name ":\n"                                          \
	"	movq (%rdi), %rax\n"                                         \
	"	vmovdqu 32(%rdi), %ymm1\n"                                   \
	"	vmovdqu 64(%rdi), %ymm0\n"                                   \
	"	vmovdqu 96(%rdi), %ymm2\n"                                   \
	".global probe_gather_" #name "_insn\n"                              \
	"probe_gather_" #name "_insn:\n"                                     \
	"	" instruction "\n"                                           \
	".global probe_gather_" #name "_end\n"                               \
	"probe_gather_" #name "_end:\n"                                      \
	"	vmovdqu %ymm0, 128(%rdi)\n"                                  \
	"	vmovdqu %ymm1, 160(%rdi)\n"                                  \
	"	vzeroupper\n"                                                \
	"	ret\n"
__asm__(".text\n"
	PROBE_GATHER(dd256, "vpgatherdd %ymm1, (%rax,%ymm2,1), %ymm0")
	PROBE_GATHER(dd128, "vpgatherdd %xmm1, (%rax,%xmm2,1), %xmm0")
	PROBE_GATHER(qq256, "vpgatherqq %ymm1, (%rax,%ymm2,1), %ymm0")
	PROBE_GATHER(qps128, "vgatherqps %xmm1, (%rax,%xmm2,1), %xmm0")
	PROBE_GATHER(fs, "vpgatherdd %ymm1, %fs:0(,%ymm2,1), %ymm0"));
#undef PROBE_GATHER

#define DECLARE_GATHER(name)                                                 \
	extern "C" void probe_gather_##name(word *state);                    \
	extern "C" char probe_gather_##name##_insn[];                        \
	extern "C" char probe_gather_##name##_end[];
DECLARE_GATHER(dd256)
DECLARE_GATHER(dd128)
DECLARE_GATHER(qq256)
DECLARE_GATHER(qps128)
DECLARE_GATHER(fs)
#undef DECLARE_GATHER

alignas(32) word gather_state[24];
/* Three pages no other case touches: the first holds the pattern, the
 * second stays untouched until a gather reads it, and the probe unmaps the
 * third. */
alignas(4096) unsigned char gather_pages[3 * page_bytes];

void gather_dd256() { probe_gather_dd256(gather_state); }
void gather_dd128() { probe_gather_dd128(gather_state); }
void gather_qq256() { probe_gather_qq256(gather_state); }
void gather_qps128() { probe_gather_qps128(gather_state); }
void gather_fs() { probe_gather_fs(gather_state); }

constexpr long nr_arch_prctl = 158;
constexpr long arch_set_fs = 0x1002;

/* The pattern dword at byte offset (offset) of the first page. */
u32 gather_pattern(word offset) { return 0x5a000000U | static_cast<u32>(offset); }

/* Fills the state for (lanes) elements of (size) bytes at the byte offsets
 * (offsets) from the pages, the mask lanes in (enabled) set to the sign
 * bit and those in (other) to a value without it; the destination starts
 * as 0x11111111 times the lane number plus one. */
void set_gather_state(unsigned size, unsigned lanes, const word *offsets,
		      unsigned enabled, unsigned other = 0)
{
	for (unsigned index = 1; index < 24; ++index)
		gather_state[index] = 0;
	gather_state[0] = reinterpret_cast<word>(gather_pages);
	auto *bytes = reinterpret_cast<unsigned char *>(gather_state);
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const u64 top = size == 4 ? 0x80000000ULL : 0x8000000000000000ULL;
		const u64 mask = (enabled >> lane) & 1U ? top :
			(other >> lane) & 1U ? top - 1 : 0;
		const u64 initial = 0x11111111ULL * (lane + 1);
		for (unsigned byte = 0; byte < size; ++byte) {
			bytes[32 + lane * size + byte] =
				static_cast<unsigned char>(mask >> (8 * byte));
			bytes[64 + lane * size + byte] =
				static_cast<unsigned char>(initial >> (8 * byte));
		}
	}
	/* Indices are dwords for the dd forms and qwords for the others. */
	const unsigned index_size = size == 4 && lanes != 2 ? 4 : 8;
	for (unsigned lane = 0; lane < lanes; ++lane)
		for (unsigned byte = 0; byte < index_size; ++byte)
			bytes[96 + lane * index_size + byte] =
				static_cast<unsigned char>(offsets[lane] >> (8 * byte));
}

/* The element (lane) of (size) bytes that the gather left at (state)
 * offset (base). */
u64 gather_lane(unsigned base, unsigned size, unsigned lane)
{
	const auto *bytes = reinterpret_cast<const volatile unsigned char *>(gather_state);
	u64 value = 0;
	for (unsigned byte = 0; byte < size; ++byte)
		value |= static_cast<u64>(bytes[base + lane * size + byte]) << (8 * byte);
	return value;
}

/* Checks the first (lanes) elements after a gather: those in (done) hold
 * (loaded) and have a zero mask; the others keep their destination and
 * the sign of their mask. */
bool expect_gathered(unsigned size, unsigned lanes, unsigned done,
		     const u64 *loaded, unsigned enabled)
{
	bool ok = true;
	const unsigned sign = size * 8 - 1;
	for (unsigned lane = 0; lane < lanes; ++lane) {
		const u64 value = gather_lane(128, size, lane);
		const u64 mask = gather_lane(160, size, lane);
		if ((done >> lane) & 1U) {
			ok &= expect("gathered element", value, loaded[lane]);
			ok &= expect("its mask", mask, 0);
		} else {
			ok &= expect("element not gathered", value,
				     0x11111111ULL * (lane + 1));
			ok &= expect("its mask", mask,
				     (enabled >> lane) & 1U ?
					     (2ULL << sign) - 1U : 0);
		}
	}
	return ok;
}

/* AVX2 gathers complete their elements in order: an element whose mask
 * sign is clear is not read, an element on a page not yet touched
 * completes, and an element on an unmapped page raises #PF at its address
 * with every element before it gathered or skipped and its mask cleared,
 * and the elements from it on unread, their masks widened from the sign to
 * all ones or zero. An FS override adds the FS base. */
bool gathers()
{
	unsigned char *const first = gather_pages;
	unsigned char *const unmapped = gather_pages + 2 * page_bytes;
	if (raw_syscall(nr_munmap, reinterpret_cast<long>(unmapped),
			page_bytes) != 0) {
		put("  munmap failed\n");
		return false;
	}
	for (word offset = 0; offset < page_bytes; offset += 4) {
		const u32 value = gather_pattern(offset);
		for (unsigned byte = 0; byte < 4; ++byte)
			first[offset + byte] = static_cast<unsigned char>(value >> (8 * byte));
	}
	bool ok = install_fault_handlers();
	const word away = 2 * page_bytes;
	const auto pattern64 = [](word offset) {
		return static_cast<u64>(gather_pattern(offset)) |
			(static_cast<u64>(gather_pattern(offset + 4)) << 32);
	};

	{
		const word offsets[8] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
		u64 loaded[8];
		for (unsigned lane = 0; lane < 8; ++lane)
			loaded[lane] = gather_pattern(offsets[lane]);
		set_gather_state(4, 8, offsets, 0xff);
		ok &= delivers_nothing("VPGATHERDD", gather_dd256,
				       address_of(probe_gather_dd256_end));
		ok &= expect_gathered(4, 8, 0xff, loaded, 0xff);
	}
	{
		/* Lane 1 has its sign clear and points at the unmapped page;
		 * lane 2 is on the untouched page; lane 3 faults. */
		const word offsets[8] = {0x10, away, page_bytes + 0x30, away + 0x40,
					 0x50, 0x60, 0x70, 0x80};
		const u64 loaded[8] = {gather_pattern(0x10), 0, 0, 0, 0, 0, 0, 0};
		set_gather_state(4, 8, offsets, 0xfd, 0x02);
		ok &= delivers("VPGATHERDD onto an unmapped page", gather_dd256,
			       address_of(probe_gather_dd256_end),
			       {sigsegv, segv_maperr, reinterpret_cast<word>(unmapped + 0x40),
				address_of(probe_gather_dd256_insn), 14, true, 4});
		ok &= expect_gathered(4, 8, 0x05, loaded, 0xf8);
	}
	{
		/* An element that runs from the first page onto the unmapped
		 * one faults at the unmapped page. */
		const word offsets[8] = {0x10, away - 2, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
		const u64 loaded[8] = {gather_pattern(0x10), 0, 0, 0, 0, 0, 0, 0};
		set_gather_state(4, 8, offsets, 0xff);
		ok &= delivers("VPGATHERDD across onto an unmapped page", gather_dd256,
			       address_of(probe_gather_dd256_end),
			       {sigsegv, segv_maperr, reinterpret_cast<word>(unmapped),
				address_of(probe_gather_dd256_insn), 14, true, 4});
		ok &= expect_gathered(4, 8, 0x01, loaded, 0xfe);
	}
	{
		const word offsets[4] = {0x10, 0x20, away + 0x30, 0x40};
		const u64 loaded[4] = {gather_pattern(0x10), gather_pattern(0x20), 0, 0};
		set_gather_state(4, 4, offsets, 0x0f);
		ok &= delivers("VPGATHERDD XMM onto an unmapped page", gather_dd128,
			       address_of(probe_gather_dd128_end),
			       {sigsegv, segv_maperr, reinterpret_cast<word>(unmapped + 0x30),
				address_of(probe_gather_dd128_insn), 14, true, 4});
		ok &= expect_gathered(4, 4, 0x03, loaded, 0x0c);
	}
	{
		const word offsets[4] = {0x10, 0x28, away + 0x8, 0x40};
		const u64 loaded[4] = {pattern64(0x10), pattern64(0x28), 0, 0};
		set_gather_state(8, 4, offsets, 0x0f);
		ok &= delivers("VPGATHERQQ onto an unmapped page", gather_qq256,
			       address_of(probe_gather_qq256_end),
			       {sigsegv, segv_maperr, reinterpret_cast<word>(unmapped + 0x8),
				address_of(probe_gather_qq256_insn), 14, true, 4});
		ok &= expect_gathered(8, 4, 0x03, loaded, 0x0c);
	}
	{
		const word offsets[2] = {0x18, away + 0x20};
		const u64 loaded[2] = {gather_pattern(0x18), 0};
		set_gather_state(4, 2, offsets, 0x03);
		ok &= delivers("VGATHERQPS onto an unmapped page", gather_qps128,
			       address_of(probe_gather_qps128_end),
			       {sigsegv, segv_maperr, reinterpret_cast<word>(unmapped + 0x20),
				address_of(probe_gather_qps128_insn), 14, true, 4});
		ok &= expect_gathered(4, 2, 0x01, loaded, 0x02);
	}
	{
		/* An FS override adds the FS base. */
		const word offsets[8] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
		u64 loaded[8];
		for (unsigned lane = 0; lane < 8; ++lane)
			loaded[lane] = gather_pattern(offsets[lane]);
		set_gather_state(4, 8, offsets, 0xff);
		if (raw_syscall(nr_arch_prctl, arch_set_fs,
				reinterpret_cast<long>(gather_pages)) != 0) {
			put("  arch_prctl failed\n");
			return false;
		}
		ok &= delivers_nothing("VPGATHERDD with an FS override", gather_fs,
				       address_of(probe_gather_fs_end));
		ok &= expect_gathered(4, 8, 0xff, loaded, 0xff);
	}
	{
		const word offsets[8] = {0x10, 0x20, 0x30, away + 0x40, 0x50, 0x60, 0x70, 0x80};
		const u64 loaded[8] = {gather_pattern(0x10), gather_pattern(0x20),
				       gather_pattern(0x30), 0, 0, 0, 0, 0};
		set_gather_state(4, 8, offsets, 0xff);
		ok &= delivers("VPGATHERDD with an FS override onto an unmapped page",
			       gather_fs, address_of(probe_gather_fs_end),
			       {sigsegv, segv_maperr, reinterpret_cast<word>(unmapped + 0x40),
				address_of(probe_gather_fs_insn), 14, true, 4});
		ok &= expect_gathered(4, 8, 0x07, loaded, 0xf8);
	}
	return ok;
}

/* Each fault-state case loads RFLAGS from (block)[16] and the registers
 * from (block)[0..15] in x86 order, skipping RSP, runs its setup, records
 * every register, RFLAGS, the x87 status and control words, MXCSR and XMM0
 * as they stand before the instruction, switches to its own stack if it
 * names one, and runs the instruction, whose store meets a read-only page
 * or whose load meets an unmapped one. The fault handler records the frame
 * and resumes after the instruction on the case's own stack. */
#define PROBE_STATE(name, setup, stack, instruction)                         \
	".global probe_state_" #name "\n"                                    \
	"probe_state_" #name ":\n"                                           \
	"	push %rbx\n"                                                 \
	"	push %rbp\n"                                                 \
	"	push %r12\n"                                                 \
	"	push %r13\n"                                                 \
	"	push %r14\n"                                                 \
	"	push %r15\n"                                                 \
	"	movq %rsp, state_resume_rsp(%rip)\n"                         \
	"	leaq 9f(%rip), %rax\n"                                       \
	"	movq %rax, state_resume_ip(%rip)\n"                          \
	"	pushq 128(%rdi)\n"                                           \
	"	popfq\n"                                                     \
	"	movq 0(%rdi), %rax\n"                                        \
	"	movq 8(%rdi), %rcx\n"                                        \
	"	movq 16(%rdi), %rdx\n"                                       \
	"	movq 24(%rdi), %rbx\n"                                       \
	"	movq 40(%rdi), %rbp\n"                                       \
	"	movq 48(%rdi), %rsi\n"                                       \
	"	movq 64(%rdi), %r8\n"                                        \
	"	movq 72(%rdi), %r9\n"                                        \
	"	movq 80(%rdi), %r10\n"                                       \
	"	movq 88(%rdi), %r11\n"                                       \
	"	movq 96(%rdi), %r12\n"                                       \
	"	movq 104(%rdi), %r13\n"                                      \
	"	movq 112(%rdi), %r14\n"                                      \
	"	movq 120(%rdi), %r15\n"                                      \
	"	movq 56(%rdi), %rdi\n"                                       \
	"	" setup "\n"                                                 \
	"	movq %r8, state_expect+0(%rip)\n"                            \
	"	movq %r9, state_expect+8(%rip)\n"                            \
	"	movq %r10, state_expect+16(%rip)\n"                          \
	"	movq %r11, state_expect+24(%rip)\n"                          \
	"	movq %r12, state_expect+32(%rip)\n"                          \
	"	movq %r13, state_expect+40(%rip)\n"                          \
	"	movq %r14, state_expect+48(%rip)\n"                          \
	"	movq %r15, state_expect+56(%rip)\n"                          \
	"	movq %rdi, state_expect+64(%rip)\n"                          \
	"	movq %rsi, state_expect+72(%rip)\n"                          \
	"	movq %rbp, state_expect+80(%rip)\n"                          \
	"	movq %rbx, state_expect+88(%rip)\n"                          \
	"	movq %rdx, state_expect+96(%rip)\n"                          \
	"	movq %rax, state_expect+104(%rip)\n"                         \
	"	movq %rcx, state_expect+112(%rip)\n"                         \
	"	pushfq\n"                                                    \
	"	popq state_expect+136(%rip)\n"                               \
	"	fnstsw state_expect_fsw(%rip)\n"                             \
	"	fnstcw state_expect_fcw(%rip)\n"                             \
	"	stmxcsr state_expect_mxcsr(%rip)\n"                          \
	"	movq %xmm0, state_expect_xmm0(%rip)\n"                       \
	"	" stack "\n"                                                 \
	"	movq %rsp, state_expect+120(%rip)\n"                         \
	".global probe_state_" #name "_insn\n"                               \
	"probe_state_" #name "_insn:\n"                                      \
	"	" instruction "\n"                                           \
	"9:\n"                                                               \
	"	movq state_resume_rsp(%rip), %rsp\n"                         \
	"	cld\n"                                                       \
	"	fninit\n"                                                    \
	"	pop %r15\n"                                                  \
	"	pop %r14\n"                                                  \
	"	pop %r13\n"                                                  \
	"	pop %r12\n"                                                  \
	"	pop %rbp\n"                                                  \
	"	pop %rbx\n"                                                  \
	"	ret\n"
/* A repeated compare whose third element meets the unmapped page has run
 * two or three: the setup runs those elements alone, so the expected pointers
 * and count are theirs, and then sets every arithmetic flag. The restart
 * moves the pointers and count back, so the instruction runs the elements
 * again from the flags the setup left. x86 keeps those flags while a
 * repeated compare faults on a later element, whatever the elements before it
 * compared, so the expected flags are not theirs. The REPE elements compare
 * equal quadwords, the REPNE ones unequal bytes, the last 0x80 with 0. */
#define CMPS_MIDWAY_REPE                                                     \
	"movq $0x33, -12(%r9); movq $0x246, -4(%r9); leaq -12(%r9), %rsi; "  \
	"leaq -16(%r10), %rdi; movl $5, %ecx; cmpsq; cmpsq; "                \
	"leaq -2(%rcx), %rcx; pushq $0x8d5; popfq"
#define CMPS_MIDWAY_REPNE                                                    \
	"movl $0x00800201, (%r9); movq %r9, %rsi; leaq -3(%r10), %rdi; "     \
	"movl $5, %ecx; cmpsb; cmpsb; cmpsb; leaq -3(%rcx), %rcx; "          \
	"pushq $0x8d5; popfq"
#define CMPS_MIDWAY_RESTART(bytes, elements)                                 \
	"leaq -" #bytes "(%rsi), %rsi; leaq -" #bytes "(%rdi), %rdi; "        \
	"leaq " #elements "(%rcx), %rcx"
/* REP MOVSB through FS, whose base the run sets to 0x1000, from the end of
 * the read-only page into the writable one: its third element meets the
 * unmapped page. The setup moves the first two alone, and the restart moves
 * the pointers and count back. RSI is an offset from the FS base. */
#define FS_MOVS_MIDWAY                                                       \
	"leaq -0x1002(%r10), %rsi; leaq -64(%r9), %rdi; movl $5, %ecx; "     \
	"movsb %fs:(%rsi), %es:(%rdi); movsb %fs:(%rsi), %es:(%rdi); "       \
	"leaq -2(%rcx), %rcx"
#define STRING_RESTART(elements)                                             \
	"leaq -" #elements "(%rsi), %rsi; leaq -" #elements "(%rdi), %rdi; " \
	"leaq " #elements "(%rcx), %rcx"
#define STATE_CASES(X)                                                       \
	X(adc, "", "", "adcq %rbx, (%r8)")                                   \
	X(adc_across, "", "", "adcq %rbx, (%r9)")                            \
	X(adc_indexed, "movl $0x40, %ecx", "",                               \
	  "adcq %rbx, -0x100(%r8,%rcx,4)")                                   \
	X(sbb, "", "", "sbbl $1, (%r8)")                                     \
	X(rcl, "", "", "rclq $1, (%r8)")                                     \
	X(rcr, "movl $3, %ecx", "", "rcrq %cl, (%r8)")                       \
	X(add, "", "", "addq %rbx, (%r8)")                                   \
	X(inc, "", "", "incl (%r8)")                                         \
	X(neg, "", "", "negq (%r8)")                                         \
	X(shl, "", "", "shlq $3, (%r8)")                                     \
	X(shld, "movl $5, %ecx", "", "shldq %cl, %rbx, (%r8)")               \
	X(bts, "movl $5, %ebx", "", "btsq %rbx, (%r8)")                      \
	X(pop_memory, "", "", "popq (%r8)")                                  \
	X(push, "", "movq %r8, %rsp", "pushq %rbx")                          \
	X(push_memory, "", "movq %r8, %rsp", "pushq 8(%r9)")                 \
	X(call, "", "movq %r8, %rsp", "call 9f")                             \
	X(call_memory, "", "movq %r8, %rsp", "call *8(%r9)")                 \
	X(enter, "", "movq %r8, %rsp", "enter $16, $0")                      \
	X(enter_nested_load, "leaq 0x108(%r10), %rbp", "leaq -64(%r9), %rsp",\
	  "enter $16, $3")                                                   \
	X(pushf, "", "movq %r8, %rsp", "pushfq")                             \
	X(movs, "movq %r8, %rsi; leaq 8(%r8), %rdi", "", "movsq")            \
	X(movs_down, "std; movq %r8, %rsi; leaq 8(%r8), %rdi", "", "movsb")  \
	X(stos, "movq %r8, %rdi", "", "stosq")                               \
	X(rep_stos, "movq %r8, %rdi; movl $5, %ecx", "", "rep stosb")        \
	X(rep_movs, "leaq -64(%r9), %rsi; movq %r8, %rdi; movl $5, %ecx", "",\
	  "rep movsb")                                                       \
	X(rep_movs_fs, "leaq -0x1040(%r9), %rsi; movq %r8, %rdi; "           \
	  "movl $5, %ecx", "", "rep movsb %fs:(%rsi), %es:(%rdi)")           \
	X(lods_fs, "leaq -0x1000(%r10), %rsi", "", "lodsl %fs:(%rsi), %eax") \
	X(rep_lods_fs, "leaq -0x1000(%r10), %rsi; movl $5, %ecx", "",        \
	  "rep lodsb %fs:(%rsi), %al")                                       \
	X(rep_movs_fs_midway, FS_MOVS_MIDWAY, STRING_RESTART(2),             \
	  "rep movsb %fs:(%rsi), %es:(%rdi)")                                \
	X(repe_cmps, "movq %r8, %rsi; movq %r10, %rdi; movl $5, %ecx", "",   \
	  "repe cmpsb")                                                      \
	X(repe_cmps_midway, CMPS_MIDWAY_REPE, CMPS_MIDWAY_RESTART(16, 2),     \
	  "repe cmpsq")                                                      \
	X(repne_cmps_midway, CMPS_MIDWAY_REPNE, CMPS_MIDWAY_RESTART(3, 3),    \
	  "repne cmpsb")                                                     \
	X(cmpxchg_equal, "movq (%r8), %rax", "", "cmpxchgq %rcx, (%r8)")     \
	X(cmpxchg_unequal, "", "", "cmpxchgq %rcx, (%r8)")                   \
	X(cmpxchg8b, "", "", "cmpxchg8b (%r8)")                              \
	X(xadd, "", "", "xaddq %rbx, (%r8)")                                 \
	X(xchg, "", "", "xchgq %rbx, (%r8)")                                 \
	X(lock_adc, "", "", "lock adcq %rbx, (%r8)")                         \
	X(fstp, "fninit; fld1; fld1", "", "fstpl (%r8)")                     \
	X(fistp, "fninit; fld1; fld1", "", "fistpl (%r8)")                   \
	X(fisttp, "fninit; fld1; fld1", "", "fisttpl (%r8)")                 \
	X(fstpt, "fninit; fld1; fld1", "", "fstpt (%r8)")                    \
	X(fbstp, "fninit; fld1; fld1", "", "fbstp (%r8)")                    \
	X(fnstenv, "fninit; fld1", "", "fnstenv -12(%r9)")                   \
	X(fnsave, "fninit; fld1", "", "fnsave -12(%r9)")                     \
	X(frstor, "fninit; fld1", "", "frstor -60(%r10)")                    \
	X(fldenv, "fninit; fld1", "", "fldenv -16(%r10)")                    \
	X(fxrstor, "fninit; fld1; pcmpeqd %xmm0, %xmm0", "",                 \
	  "fxrstor -256(%r10)")                                              \
	X(xrstor, "fninit; fld1; pcmpeqd %xmm0, %xmm0; movl $7, %eax; "      \
	  "xorl %edx, %edx", "", "xrstor -768(%r10)")                        \
	X(iretq, "", "leaq -24(%r10), %rsp", "iretq")                        \
	X(leave, "movq %r10, %rbp", "", "leave")                             \
	X(ret, "", "movq %r10, %rsp", "ret")                                 \
	X(ret_imm, "", "movq %r10, %rsp", "ret $16")                         \
	X(lret, "", "movq %r10, %rsp", "lretq")                              \
	X(pop, "", "movq %r10, %rsp", "popq %rbx")                           \
	X(popf, "", "movq %r10, %rsp", "popfq")                              \
	X(push_load, "", "", "pushq (%r10)")                                 \
	X(lods, "movq %r10, %rsi", "", "lodsq")                              \
	X(rep_lods, "movq %r10, %rsi; movl $5, %ecx", "", "rep lodsb")       \
	X(scas, "movq %r10, %rdi", "", "scasq")                              \
	X(repne_scas, "movq %r10, %rdi; movl $5, %ecx", "", "repne scasb")   \
	X(xlat, "movq %r10, %rbx", "", "xlatb")                              \
	X(cmpxchg16b, "", "", "cmpxchg16b (%r8)")                            \
	X(adc_load, "", "", "adcq (%r10), %rbx")                             \
	X(div_load, "", "", "divq (%r10)")                                   \
	X(cmp_unread, "", "", "cmpq %rbx, (%r10); addq %rcx, %rcx")          \
	X(test_unread, "", "", "testl %ebx, (%r10); addq %rcx, %rcx")        \
	X(bt_unread, "", "", "btl $3, (%r10); addq %rcx, %rcx")              \
	X(rcl_zero, "xorl %ecx, %ecx", "", "rclb %cl, (%r10)")               \
	X(rcr_zero, "", "", "rcrw $0, (%r10)")                               \
	X(push_unread, "", "movq %r8, %rsp", "pushq %rbx; addq %rcx, %rcx")  \
	X(pop_unread, "", "movq %r10, %rsp", "popq %rbx; addq %rcx, %rcx")   \
	X(movnt_unread, "", "", "movntps %xmm0, (%r8); addq %rcx, %rcx")     \
	X(movntdqa_unread, "", "",                                           \
	  "movntdqa (%r10), %xmm1; addq %rcx, %rcx")                         \
	X(fld, "fninit; fld1", "", "fldl (%r10)")                            \
	X(fild, "fninit; fld1", "", "fildl (%r10)")                          \
	X(fldt, "fninit; fld1", "", "fldt (%r10)")                           \
	X(fbld, "fninit; fld1", "", "fbld (%r10)")                           \
	X(fadd_load, "fninit; fld1", "", "faddl (%r10)")                     \
	X(fldcw, "fninit; fld1", "", "fldcw (%r10)")                         \
	X(ldmxcsr, "fninit; fld1", "", "ldmxcsr (%r10)")                     \
	X(ldmxcsr_reserved, "fninit; fld1", "", "ldmxcsr 0x318(%r8)")        \
	X(fxrstor_reserved, "fninit; fld1; pcmpeqd %xmm0, %xmm0", "",        \
	  "fxrstor 0x300(%r8)")                                              \
	X(xrstor_reserved, "fninit; fld1; pcmpeqd %xmm0, %xmm0; movl $7, %eax",\
	  "", "xrstor 0x300(%r8)")                                           \
	X(xrstor_sse_reserved, "fninit; fld1; pcmpeqd %xmm0, %xmm0; "        \
	  "movl $2, %eax", "", "xrstor 0x300(%r8)")                          \
	X(xrstor_avx_reserved, "fninit; fld1; pcmpeqd %xmm0, %xmm0; "        \
	  "movl $4, %eax", "", "xrstor 0x300(%r8)")

#define EMIT_STATE(name, setup, stack, instruction)                          \
	PROBE_STATE(name, setup, stack, instruction)
__asm__(".text\n" STATE_CASES(EMIT_STATE));
#undef EMIT_STATE
#undef PROBE_STATE

#define DECLARE_STATE(name, setup, stack, instruction)                       \
	extern "C" void probe_state_##name(const word *block);               \
	extern "C" char probe_state_##name##_insn[];
STATE_CASES(DECLARE_STATE)
#undef DECLARE_STATE

/* The state before the instruction, in the frame's register order. */
extern "C" {
volatile word state_resume_rsp;
volatile word state_resume_ip;
word state_expect[18];
u16 state_expect_fsw;
u16 state_expect_fcw;
u32 state_expect_mxcsr;
u64 state_expect_xmm0;
}

/* The frame's general registers, R8 to RFLAGS, and x87 words. */
constexpr unsigned frame_registers = 18;
word state_frame[frame_registers];
u16 state_frame_fsw;
u16 state_frame_fcw;
u32 state_frame_mxcsr;
u64 state_frame_xmm0;
const char *const frame_register_names[frame_registers] = {
	"R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15", "RDI",
	"RSI", "RBP", "RBX", "RDX", "RAX", "RCX", "RSP", "RIP", "RFLAGS"};
constexpr unsigned frame_rsp_offset = 40 + 15 * 8;
constexpr unsigned frame_fpstate_offset = 40 + 23 * 8;

void record_state(int signal, void *info, void *context)
{
	resume_ip = 0;
	record_fault(signal, info, context);
	for (unsigned index = 0; index < frame_registers; ++index)
		state_frame[index] = frame_word(context, 40 + index * 8);
	const auto *fpstate = reinterpret_cast<const unsigned char *>(
		frame_word(context, frame_fpstate_offset));
	if (fpstate != nullptr) {
		state_frame_fcw = *reinterpret_cast<const u16 *>(fpstate);
		state_frame_fsw = *reinterpret_cast<const u16 *>(fpstate + 2);
		state_frame_mxcsr = *reinterpret_cast<const u32 *>(fpstate + 24);
		state_frame_xmm0 = *reinterpret_cast<const u64 *>(fpstate + 160);
	}
	frame_word(context, frame_ip_offset) = state_resume_ip;
	frame_word(context, frame_rsp_offset) = state_resume_rsp;
}

struct StateCase {
	const char *name;
	void (*probe)(const word *block);
	const char *instruction;
	bool x87;
};

#define STATE_ENTRY(name, setup, stack, instruction)                         \
	{#name, probe_state_##name, probe_state_##name##_insn,                \
	 (setup)[0] == 'f'},
const StateCase state_cases[] = {STATE_CASES(STATE_ENTRY)};
#undef STATE_ENTRY

constexpr long nr_mprotect = 10;
constexpr long prot_read = 1;
/* A writable page, a read-only one and an unmapped one. */
alignas(4096) unsigned char state_pages[3 * page_bytes];

/* An instruction whose store or load faults has done nothing: the frame
 * shows every register, RFLAGS, the x87 stack and control word, MXCSR and
 * XMM0 as the instruction found them, and RIP at it, whatever the
 * instruction computed before, so the guest that resolves the fault runs it
 * again from scratch. A repeated string instruction has done the elements
 * before the faulting one, with their flags. */
bool fault_state()
{
	unsigned char *const read_only = state_pages + page_bytes;
	unsigned char *const unmapped = state_pages + 2 * page_bytes;
	for (word offset = 0; offset < page_bytes; ++offset)
		read_only[offset] = static_cast<unsigned char>(0x5a + offset);
	/* An XSAVE image of x87, SSE and AVX state whose AVX part runs onto the
	 * unmapped page, with FCW 0x27f, MXCSR 0x1f80 and zero registers. */
	unsigned char *const image = unmapped - 768;
	for (word offset = 0; offset < 768 - 24; ++offset)
		image[offset] = 0;
	image[0] = 0x7f;
	image[1] = 0x02;
	image[24] = 0x80;
	image[25] = 0x1f;
	image[512] = 7;
	/* The MXCSR cases restore a zeroed image at 0x400 of the read-only page,
	 * with XSTATE_BV 0 and a reserved bit in its MXCSR. */
	unsigned char *const reserved_mxcsr = read_only + 0x400;
	for (word offset = 0; offset < 832; ++offset)
		reserved_mxcsr[offset] = 0;
	reserved_mxcsr[0] = 0x7f;
	reserved_mxcsr[1] = 0x02;
	reserved_mxcsr[26] = 1;
	/* IRETQ's RIP, CS and RFLAGS slots, before its RSP on the unmapped
	 * page. */
	const word iret_frame[3] = {reinterpret_cast<word>(state_pages), 0x33,
				    0x246};
	for (unsigned byte = 0; byte < sizeof(iret_frame); ++byte)
		(unmapped - sizeof(iret_frame))[byte] =
			reinterpret_cast<const unsigned char *>(iret_frame)[byte];
	if (raw_syscall(nr_mprotect, reinterpret_cast<long>(read_only),
			page_bytes, prot_read) != 0 ||
	    raw_syscall(nr_munmap, reinterpret_cast<long>(unmapped),
			page_bytes) != 0) {
		put("  mprotect or munmap failed\n");
		return false;
	}
	/* A push or call faults with RSP on the read-only page. */
	if (!handle_on_alternate_stack(sigsegv, record_state) ||
	    !handle_on_alternate_stack(sigbus, record_state)) {
		put("  sigaltstack or rt_sigaction failed\n");
		return false;
	}
	/* The FS cases address their source through a base of 0x1000. */
	if (raw_syscall(nr_arch_prctl, arch_set_fs, 0x1000) != 0) {
		put("  arch_prctl failed\n");
		return false;
	}
	word block[17];
	for (unsigned index = 0; index < 16; ++index)
		block[index] = 0x0f0e0d0c0b0a0908ULL + 0x0101010101010101ULL * index;
	block[8] = reinterpret_cast<word>(read_only + 0x100);
	block[9] = reinterpret_cast<word>(read_only - 4);
	block[10] = reinterpret_cast<word>(unmapped);
	/* CF, PF, AF, SF and OF set; ZF and DF clear. */
	block[16] = 0x897;
	bool ok = true;
	for (const auto &test : state_cases) {
		observed[observed_count] = 0;
		test.probe(block);
		bool good = expect("signals", observed[observed_count], 1);
		good &= expect("signal", observed[observed_signal],
			       static_cast<word>(sigsegv));
		good &= expect("frame ip", state_frame[16],
			       reinterpret_cast<word>(test.instruction));
		for (unsigned index = 0; index < 16; ++index)
			good &= expect(frame_register_names[index],
				       state_frame[index], state_expect[index]);
		good &= expect("RFLAGS", state_frame[17] & 0xcd5,
			       state_expect[17] & 0xcd5);
		if (test.x87) {
			good &= expect("x87 TOP", state_frame_fsw & 0x3800U,
				       state_expect_fsw & 0x3800U);
			good &= expect("x87 control", state_frame_fcw,
				       state_expect_fcw);
			good &= expect("MXCSR", state_frame_mxcsr,
				       state_expect_mxcsr);
			good &= expect("XMM0", state_frame_xmm0,
				       state_expect_xmm0);
		}
		if (!good) {
			put("  in ");
			put(test.name);
			put("\n");
		}
		ok &= good;
	}
	return ok;
}
#endif

#if defined(__i386__)
/* Each i386 fault-state case loads EFLAGS from (block)[8] and the registers
 * from (block)[0..7] in x86 order, skipping ESP, runs its setup, records
 * every register, EFLAGS, the x87 status and control words, MXCSR and XMM0
 * as they stand before the instruction, switches to its own stack if it
 * names one, and runs the instruction. Its four pages are unmapped,
 * writable, read-only and unmapped: EDX points into the read-only page, EBP
 * 2 bytes below it, ESI at the last page and EDI 16 bytes into the
 * writable one, so a stack there takes four dwords before a push faults.
 * The fault handler records the frame and resumes after the instruction on
 * the case's own stack. */
#define PROBE_STATE(name, setup, stack, instruction)                         \
	".global probe_state_" #name "\n"                                    \
	"probe_state_" #name ":\n"                                           \
	"	push %ebx\n"                                                 \
	"	push %ebp\n"                                                 \
	"	push %esi\n"                                                 \
	"	push %edi\n"                                                 \
	"	movl %esp, state_resume_sp\n"                                \
	"	movl $9f, state_resume_ip\n"                                 \
	"	movl 20(%esp), %eax\n"                                       \
	"	pushl 32(%eax)\n"                                            \
	"	popfl\n"                                                     \
	"	movl 4(%eax), %ecx\n"                                        \
	"	movl 8(%eax), %edx\n"                                        \
	"	movl 12(%eax), %ebx\n"                                       \
	"	movl 20(%eax), %ebp\n"                                       \
	"	movl 24(%eax), %esi\n"                                       \
	"	movl 28(%eax), %edi\n"                                       \
	"	movl 0(%eax), %eax\n"                                        \
	"	" setup "\n"                                                 \
	"	movl %edi, state_expect+0\n"                                 \
	"	movl %esi, state_expect+4\n"                                 \
	"	movl %ebp, state_expect+8\n"                                 \
	"	movl %ebx, state_expect+16\n"                                \
	"	movl %edx, state_expect+20\n"                                \
	"	movl %ecx, state_expect+24\n"                                \
	"	movl %eax, state_expect+28\n"                                \
	"	pushfl\n"                                                    \
	"	popl state_expect+36\n"                                      \
	"	fnstsw state_expect_fsw\n"                                   \
	"	fnstcw state_expect_fcw\n"                                   \
	"	stmxcsr state_expect_mxcsr\n"                                \
	"	movq %xmm0, state_expect_xmm0\n"                             \
	"	" stack "\n"                                                 \
	"	movl %esp, state_expect+12\n"                                \
	".global probe_state_" #name "_insn\n"                               \
	"probe_state_" #name "_insn:\n"                                      \
	"	" instruction "\n"                                           \
	"9:\n"                                                               \
	"	movl state_resume_sp, %esp\n"                                \
	"	cld\n"                                                       \
	"	fninit\n"                                                    \
	"	pop %edi\n"                                                  \
	"	pop %esi\n"                                                  \
	"	pop %ebp\n"                                                  \
	"	pop %ebx\n"                                                  \
	"	ret\n"
/* The stores meet the read-only page, or for a _midway case the unmapped
 * one below the writable page after the instruction's first stores; the
 * loads meet the last page, after the first loads for a multi-part one. */
/* POPAD and POPAW load EDI, ESI and EBP from the three slots below the
 * unmapped page, skip ESP's, and meet the page at EBX's. x86 loads the
 * registers in that order, so the three keep what they loaded when the
 * fourth load faults, and ESP stays. The setup stashes the three in XMM1 to
 * XMM3, which the case does not compare, and loads them as POPA does; the
 * restart puts them back. */
#define POPA_PARTIAL                                                         \
	"movd %edi, %xmm1; movd %ebp, %xmm2; movd %esi, %xmm3; "             \
	"movl -16(%esi), %edi; movl -8(%esi), %ebp; movl -12(%esi), %esi"
#define POPAW_PARTIAL                                                        \
	"movd %edi, %xmm1; movd %ebp, %xmm2; movd %esi, %xmm3; "             \
	"movw -8(%esi), %di; movw -4(%esi), %bp; movw -6(%esi), %si"
/* REP MOVSB through GS, whose TLS base the run sets to 0x1000, from the
 * end of the read-only page into the writable one: its third element meets
 * the unmapped page. ESI is an offset from the GS base. */
#define GS_MOVS_MIDWAY                                                       \
	"leal -0x1002(%esi), %esi; movl $5, %ecx; "                         \
	"movsb %gs:(%esi), %es:(%edi); movsb %gs:(%esi), %es:(%edi); "       \
	"leal -2(%ecx), %ecx"
#define STRING_RESTART(elements)                                             \
	"leal -" #elements "(%esi), %esi; leal -" #elements "(%edi), %edi; " \
	"leal " #elements "(%ecx), %ecx"
#define STATE_CASES(X)                                                       \
	X(adc, "", "", "adcl %ebx, (%edx)")                                  \
	X(adc_across, "", "", "adcl %ebx, (%ebp)")                           \
	X(adc_absolute, "", "", "adcl %ebx, state_pages+0x2100")             \
	X(sbb, "", "", "sbbl $1, (%edx)")                                    \
	X(rcl, "", "", "rcll $1, (%edx)")                                    \
	X(rcr, "movl $3, %ecx", "", "rcrl %cl, (%edx)")                      \
	X(neg, "", "", "negl (%edx)")                                        \
	X(shld, "movl $5, %ecx", "", "shldl %cl, %ebx, (%edx)")              \
	X(bts, "movl $5, %ebx", "", "btsl %ebx, (%edx)")                     \
	X(xadd, "", "", "xaddl %ebx, (%edx)")                                \
	X(xchg, "", "", "xchgl %ebx, (%edx)")                                \
	X(cmpxchg8b, "", "", "cmpxchg8b (%edx)")                             \
	X(lock_adc, "", "", "lock adcl %ebx, (%edx)")                        \
	X(pop_memory, "", "", "popl (%edx)")                                 \
	X(pop_memory16, "", "", "popw (%edx)")                               \
	X(push, "", "movl %edx, %esp", "pushl %ebx")                         \
	X(push16, "", "movl %edx, %esp", "pushw %bx")                        \
	X(push_immediate, "", "movl %edx, %esp", "pushl $0x12345678")        \
	X(push_memory, "", "movl %edx, %esp", "pushl 8(%ebp)")               \
	X(push_es, "", "movl %edx, %esp", "pushl %es")                       \
	X(push_cs, "", "movl %edx, %esp", "pushl %cs")                       \
	X(push_ss, "", "movl %edx, %esp", "pushl %ss")                       \
	X(push_ds, "", "movl %edx, %esp", "pushl %ds")                       \
	X(push_fs, "", "movl %edx, %esp", "pushl %fs")                       \
	X(push_gs, "", "movl %edx, %esp", "pushl %gs")                       \
	X(call, "", "movl %edx, %esp", "call 9f")                            \
	X(call_memory, "", "movl %edx, %esp", "call *8(%ebp)")               \
	X(call_far, "", "movl %edx, %esp", "lcall *(%edx)")                  \
	X(call_far_midway, "", "leal -12(%edi), %esp", "lcall *(%edx)")      \
	X(enter, "", "movl %edx, %esp", "enter $16, $0")                     \
	X(enter_nested, "leal 64(%edx), %ebp", "movl %edx, %esp",            \
	  "enter $16, $3")                                                   \
	X(enter_nested_load, "leal 0x108(%esi), %ebp", "movl %edi, %esp",    \
	  "enter $16, $3")                                                   \
	X(pushf, "", "movl %edx, %esp", "pushfl")                            \
	X(pusha, "", "movl %edx, %esp", "pushal")                            \
	X(pusha_midway, "", "movl %edi, %esp", "pushal")                     \
	X(pushaw_midway, "", "leal -8(%edi), %esp", "pushaw")                \
	X(movs, "movl %edx, %esi; leal 4(%edx), %edi", "", "movsl")          \
	X(stos, "movl %edx, %edi", "", "stosl")                              \
	X(rep_stos, "movl %edx, %edi; movl $5, %ecx", "", "rep stosb")       \
	X(rep_movs, "leal -64(%ebp), %esi; movl %edx, %edi; movl $5, %ecx",  \
	  "", "rep movsb")                                                   \
	X(fstp, "fninit; fld1; fld1", "", "fstpl (%edx)")                    \
	X(fistp, "fninit; fld1; fld1", "", "fistpl (%edx)")                  \
	X(fstpt, "fninit; fld1; fld1", "", "fstpt (%edx)")                   \
	X(fnstenv, "fninit; fld1", "", "fnstenv -12(%ebp)")                  \
	X(fnsave, "fninit; fld1", "", "fnsave -12(%ebp)")                    \
	X(frstor, "fninit; fld1", "", "frstor -60(%esi)")                    \
	X(fldenv, "fninit; fld1", "", "fldenv -16(%esi)")                    \
	X(fxrstor, "fninit; fld1; pcmpeqd %xmm0, %xmm0", "",                 \
	  "fxrstor -256(%esi)")                                              \
	X(iret, "", "leal -8(%esi), %esp", "iretl")                          \
	X(leave, "movl %esi, %ebp", "", "leave")                             \
	X(ret, "", "movl %esi, %esp", "ret")                                 \
	X(ret_imm, "", "movl %esi, %esp", "ret $16")                         \
	X(lret, "", "leal -4(%esi), %esp", "lret")                           \
	X(lret16, "", "leal -2(%esi), %esp", "lretw")                        \
	X(pop, "", "movl %esi, %esp", "popl %ebx")                           \
	X(popf, "", "movl %esi, %esp", "popfl")                              \
	X(pop_es, "", "movl %esi, %esp", "popl %es")                         \
	X(pop_ds, "", "movl %esi, %esp", "popl %ds")                         \
	X(pop_fs, "", "movl %esi, %esp", "popl %fs")                         \
	X(pop_gs, "", "movl %esi, %esp", "popl %gs")                         \
	X(popa, POPA_PARTIAL, "movd %xmm1, %edi; movd %xmm2, %ebp; "          \
	  "movd %xmm3, %esi; leal -16(%esi), %esp", "popal")                 \
	X(popaw, POPAW_PARTIAL, "movd %xmm1, %edi; movd %xmm2, %ebp; "        \
	  "movd %xmm3, %esi; leal -8(%esi), %esp", "popaw")                  \
	X(bound, "", "", "bound %eax, -4(%esi)")                             \
	X(push_load, "", "", "pushl (%esi)")                                 \
	X(lods, "", "", "lodsl")                                             \
	X(rep_movs_gs, "leal -0xfd0(%edi), %esi; movl %edx, %edi; "         \
	  "movl $5, %ecx", "", "rep movsb %gs:(%esi), %es:(%edi)")           \
	X(lods_gs, "leal -0x1000(%esi), %esi", "", "lodsl %gs:(%esi), %eax") \
	X(rep_lods_gs, "leal -0x1000(%esi), %esi; movl $5, %ecx", "",        \
	  "rep lodsb %gs:(%esi), %al")                                       \
	X(rep_movs_gs_midway, GS_MOVS_MIDWAY, STRING_RESTART(2),             \
	  "rep movsb %gs:(%esi), %es:(%edi)")                                \
	X(scas, "movl %esi, %edi", "", "scasl")                              \
	X(xlat, "movl %esi, %ebx", "", "xlatb")                              \
	X(adc_load, "", "", "adcl (%esi), %ebx")                             \
	X(div_load, "", "", "divl (%esi)")                                   \
	X(cmp_unread, "", "", "cmpl %ebx, (%esi); addl %ecx, %ecx")          \
	X(test_unread, "", "", "testl %ebx, (%esi); addl %ecx, %ecx")        \
	X(bt_unread, "", "", "btl $3, (%esi); addl %ecx, %ecx")              \
	X(rcl_zero, "xorl %ecx, %ecx", "", "rclb %cl, (%esi)")               \
	X(rcr_zero, "", "", "rcrw $0, (%esi)")                               \
	X(push_unread, "", "movl %edx, %esp", "pushl %ebx; addl %ecx, %ecx") \
	X(pop_unread, "", "movl %esi, %esp", "popl %ebx; addl %ecx, %ecx")   \
	X(movnt_unread, "", "", "movntps %xmm0, (%edx); addl %ecx, %ecx")    \
	X(movntdqa_unread, "", "",                                           \
	  "movntdqa (%esi), %xmm1; addl %ecx, %ecx")                         \
	X(fld, "fninit; fld1", "", "fldl (%esi)")                            \
	X(fild, "fninit; fld1", "", "fildl (%esi)")                          \
	X(fldt, "fninit; fld1", "", "fldt (%esi)")                           \
	X(fldcw, "fninit; fld1", "", "fldcw (%esi)")                         \
	X(ldmxcsr, "fninit; fld1", "", "ldmxcsr (%esi)")                     \
	X(ldmxcsr_reserved, "fninit; fld1", "", "ldmxcsr 0x318(%edx)")       \
	X(fxrstor_reserved, "fninit; fld1; pcmpeqd %xmm0, %xmm0", "",        \
	  "fxrstor 0x300(%edx)")                                             \
	X(xrstor_reserved, "fninit; fld1; pcmpeqd %xmm0, %xmm0; movl $7, %eax",\
	  "", "xrstor 0x300(%edx)")                                          \
	X(xrstor_sse_reserved, "fninit; fld1; pcmpeqd %xmm0, %xmm0; "        \
	  "movl $2, %eax", "", "xrstor 0x300(%edx)")                         \
	X(xrstor_avx_reserved, "fninit; fld1; pcmpeqd %xmm0, %xmm0; "        \
	  "movl $4, %eax", "", "xrstor 0x300(%edx)")

#define EMIT_STATE(name, setup, stack, instruction)                          \
	PROBE_STATE(name, setup, stack, instruction)
__asm__(".text\n" STATE_CASES(EMIT_STATE));
#undef EMIT_STATE
#undef PROBE_STATE

#define DECLARE_STATE(name, setup, stack, instruction)                       \
	extern "C" void probe_state_##name(const word *block);               \
	extern "C" char probe_state_##name##_insn[];
STATE_CASES(DECLARE_STATE)
#undef DECLARE_STATE

/* Calls the routine (pointer)[0] names and pushes and pops (pointer)[1]:
 * reads from a read-only page that must not fault. */
__asm__(".text\n"
	".global probe_read_only_reads\n"
	"probe_read_only_reads:\n"
	"	movl 4(%esp), %eax\n"
	"	call *(%eax)\n"
	"	pushl 4(%eax)\n"
	"	popl %eax\n"
	".global probe_read_only_reads_end\n"
	"probe_read_only_reads_end:\n"
	"	ret\n"
	".global probe_return_only\n"
	"probe_return_only:\n"
	"	ret\n");
extern "C" void probe_read_only_reads(const word *pointers);
extern "C" char probe_read_only_reads_end[];
extern "C" void probe_return_only();

/* The state before the instruction, in the frame's register order. */
extern "C" {
volatile word state_resume_sp;
volatile word state_resume_ip;
word state_expect[10];
u16 state_expect_fsw;
u16 state_expect_fcw;
u32 state_expect_mxcsr;
u64 state_expect_xmm0;
}

/* The frame's EDI to EAX, EIP and EFLAGS, and x87 words. */
constexpr unsigned frame_registers = 10;
word state_frame[frame_registers];
u16 state_frame_fsw;
u16 state_frame_fcw;
u32 state_frame_mxcsr;
u64 state_frame_xmm0;
const char *const frame_register_names[8] = {
	"EDI", "ESI", "EBP", "ESP", "EBX", "EDX", "ECX", "EAX"};
constexpr unsigned frame_edi_offset = 20 + 4 * 4;
constexpr unsigned frame_esp_offset = 20 + 7 * 4;
constexpr unsigned frame_fpstate_offset = 20 + 19 * 4;

void record_state(int signal, void *info, void *context)
{
	resume_ip = 0;
	record_fault(signal, info, context);
	for (unsigned index = 0; index < 8; ++index)
		state_frame[index] =
			frame_word(context, frame_edi_offset + index * 4);
	state_frame[8] = frame_word(context, frame_ip_offset);
	state_frame[9] = frame_word(context, frame_flags_offset);
	/* struct _fpstate_32: the FNSAVE environment, then the FXSAVE image
	 * at 112, whose MXCSR is at 136 and XMM0 at 272. */
	const auto *fpstate = reinterpret_cast<const unsigned char *>(
		frame_word(context, frame_fpstate_offset));
	if (fpstate != nullptr) {
		state_frame_fcw = *reinterpret_cast<const u16 *>(fpstate);
		state_frame_fsw = *reinterpret_cast<const u16 *>(fpstate + 4);
		state_frame_mxcsr = *reinterpret_cast<const u32 *>(fpstate + 136);
		state_frame_xmm0 = *reinterpret_cast<const u64 *>(fpstate + 272);
	}
	frame_word(context, frame_ip_offset) = state_resume_ip;
	frame_word(context, frame_esp_offset) = state_resume_sp;
}

struct StateCase {
	const char *name;
	void (*probe)(const word *block);
	const char *instruction;
	bool x87;
};

#define STATE_ENTRY(name, setup, stack, instruction)                         \
	{#name, probe_state_##name, probe_state_##name##_insn,               \
	 (setup)[0] == 'f'},
const StateCase state_cases[] = {STATE_CASES(STATE_ENTRY)};
#undef STATE_ENTRY

constexpr long nr_munmap = 91;
constexpr long nr_mprotect = 125;
constexpr long nr_sigaltstack = 186;
constexpr long prot_read = 1;
constexpr word page_bytes = 4096;
constexpr word sa_onstack = 0x08000000;
extern "C" {
/* adc_absolute names a page by its address. */
alignas(4096) unsigned char state_pages[4 * page_bytes];
}
alignas(16) unsigned char alternate_stack[16384];

bool handle_on_alternate_stack(long signal,
			       void (*handler)(int, void *, void *))
{
	const struct {
		word sp;
		long flags;
		word size;
	} stack = {reinterpret_cast<word>(alternate_stack), 0,
		   sizeof(alternate_stack)};
	KernelSigaction action = {};
	action.handler = reinterpret_cast<word>(handler);
	action.flags = sa_siginfo | sa_restorer | sa_onstack;
	action.restorer = reinterpret_cast<word>(&probe_restorer);
	return raw_syscall(nr_sigaltstack, reinterpret_cast<long>(&stack), 0) ==
		       0 &&
	       raw_syscall(nr_rt_sigaction, signal,
			   reinterpret_cast<long>(&action), 0, 8) == 0;
}

const word *read_only_pointers;
void read_only_reads() { probe_read_only_reads(read_only_pointers); }

/* An instruction whose store or load faults has done nothing, as on x86-64,
 * through the forms only 32-bit code has: segment pushes and pops, PUSHA,
 * POPA, BOUND, far CALL and RET, and absolute operands. A read of a
 * read-only page by an instruction that also stores elsewhere succeeds. */
bool fault_state()
{
	unsigned char *const below = state_pages;
	unsigned char *const writable = state_pages + page_bytes;
	unsigned char *const read_only = state_pages + 2 * page_bytes;
	unsigned char *const unmapped = state_pages + 3 * page_bytes;
	for (word offset = 0; offset < page_bytes; ++offset)
		read_only[offset] = static_cast<unsigned char>(0x5a + offset);
	auto *pointers = reinterpret_cast<word *>(read_only + 0x200);
	pointers[0] = reinterpret_cast<word>(&probe_return_only);
	pointers[1] = 0x1234;
	read_only_pointers = pointers;
	/* The MXCSR cases restore a zeroed image at 0x400 of the read-only page,
	 * with XSTATE_BV 0 and a reserved bit in its MXCSR. */
	unsigned char *const reserved_mxcsr = read_only + 0x400;
	for (word offset = 0; offset < 832; ++offset)
		reserved_mxcsr[offset] = 0;
	reserved_mxcsr[0] = 0x7f;
	reserved_mxcsr[1] = 0x02;
	reserved_mxcsr[26] = 1;
	if (raw_syscall(nr_mprotect, reinterpret_cast<long>(read_only),
			page_bytes, prot_read) != 0 ||
	    raw_syscall(nr_munmap, reinterpret_cast<long>(unmapped),
			page_bytes) != 0 ||
	    raw_syscall(nr_munmap, reinterpret_cast<long>(below),
			page_bytes) != 0) {
		put("  mprotect or munmap failed\n");
		return false;
	}
	bool ok = install_fault_handlers() &&
		  delivers_nothing("reads of a read-only page", read_only_reads,
				   address_of(probe_read_only_reads_end));
	/* A push or call faults with ESP on the read-only page. */
	if (!handle_on_alternate_stack(sigsegv, record_state) ||
	    !handle_on_alternate_stack(sigbus, record_state)) {
		put("  sigaltstack or rt_sigaction failed\n");
		return false;
	}
	/* The GS cases address their source through a TLS base of 0x1000. */
	struct {
		u32 entry_number;
		u32 base_addr;
		u32 limit;
		u32 flags;
	} descriptor = {~0U, 0x1000, 0xfffff, 1U | 1U << 4 | 1U << 6};
	if (raw_syscall(nr_set_thread_area,
			reinterpret_cast<long>(&descriptor)) != 0) {
		put("  set_thread_area failed\n");
		return false;
	}
	const u32 selector = descriptor.entry_number << 3 | 3;
	__asm__ volatile("movw %w0, %%gs" : : "r"(selector));
	word block[9];
	for (unsigned index = 0; index < 8; ++index)
		block[index] = 0x0b0a0908UL + 0x01010101UL * index;
	block[2] = reinterpret_cast<word>(read_only + 0x100);
	block[5] = reinterpret_cast<word>(read_only - 2);
	block[6] = reinterpret_cast<word>(unmapped);
	block[7] = reinterpret_cast<word>(writable + 16);
	/* CF, PF, AF, SF and OF set; ZF and DF clear. */
	block[8] = 0x897;
	for (const auto &test : state_cases) {
		observed[observed_count] = 0;
		test.probe(block);
		bool good = expect("signals", observed[observed_count], 1);
		good &= expect("signal", observed[observed_signal],
			       static_cast<word>(sigsegv));
		good &= expect("frame ip", state_frame[8],
			       reinterpret_cast<word>(test.instruction));
		for (unsigned index = 0; index < 8; ++index)
			good &= expect(frame_register_names[index],
				       state_frame[index], state_expect[index]);
		good &= expect("EFLAGS", state_frame[9] & 0xcd5,
			       state_expect[9] & 0xcd5);
		if (test.x87) {
			good &= expect("x87 TOP", state_frame_fsw & 0x3800U,
				       state_expect_fsw & 0x3800U);
			good &= expect("x87 control", state_frame_fcw,
				       state_expect_fcw);
			good &= expect("MXCSR", state_frame_mxcsr,
				       state_expect_mxcsr);
			good &= expect("XMM0", state_frame_xmm0,
				       state_expect_xmm0);
		}
		if (!good) {
			put("  in ");
			put(test.name);
			put("\n");
		}
		ok &= good;
	}
	return ok;
}

extern "C" word probe_wrap_index(word base, word index);
extern "C" char probe_wrap_index_insn[];
extern "C" char probe_wrap_index_end[];
extern "C" word probe_wrap_sum(word base, word index);
extern "C" word probe_wrap_absolute(word value);
extern "C" word probe_wrap_gs(word offset);
extern "C" word probe_wrap_load(word address);
extern "C" char probe_wrap_load_insn[];
extern "C" char probe_wrap_load_end[];
extern "C" void probe_wrap_push();
extern "C" char probe_wrap_push_insn[];
extern "C" char probe_wrap_push_end[];
extern "C" void probe_wrap_stos(word address);
extern "C" char probe_wrap_stos_insn[];
extern "C" char probe_wrap_stos_end[];
extern "C" {
volatile word wrap_saved_sp;
}

constexpr long nr_old_mmap = 90;
constexpr word wrap_high_page = 0xe0000000;
volatile word wrap_value = 0x600d;

void wrap_index_fault() { (void)probe_wrap_index(0x10, 0x3ffffffe); }
void wrap_load_fault() { (void)probe_wrap_load(4); }
void wrap_stos_fault() { probe_wrap_stos(0xfffffffe); }

/* A 32-bit effective address wraps at 4 GiB: base plus scaled index, an
 * absolute operand at or above 2 GiB, a segment base plus the offset, the
 * stack and a string's operands. An access through a wrapped address that
 * reaches no mapping faults with the wrapped address. */
bool addresses_wrap()
{
	const struct {
		u32 address;
		u32 length;
		u32 prot;
		u32 flags;
		u32 fd;
		u32 offset;
	} high = {wrap_high_page, page_bytes, 3, 0x32, ~0U, 0};
	if (static_cast<word>(raw_syscall(nr_old_mmap,
		reinterpret_cast<long>(&high))) != wrap_high_page) {
		put("  mmap at 0xe0000000 failed\n");
		return false;
	}
	auto *const page = reinterpret_cast<volatile u32 *>(wrap_high_page);
	page[4] = 0x1111;
	const word value = reinterpret_cast<word>(&wrap_value);
	bool ok = expect("(base, index * 4) past 4 GiB",
			 probe_wrap_index(value, 0x40000000), 0x600d);
	ok &= expect("INC (base, index) past 4 GiB",
		     probe_wrap_sum(value + 0x80000000, 0x80000000), 0x600e);
	ok &= expect("absolute operands at 0xe0000010",
		     probe_wrap_absolute(0x2222), 0x2222 + 2 * 0x1111);
	ok &= expect("the absolute store", page[5], 0x2222);
	/* A segment base plus an offset wraps too. */
	struct {
		u32 entry_number;
		u32 base_addr;
		u32 limit;
		u32 flags;
	} descriptor = {~0U, 0xfffff000, 0xfffff, 1U | 1U << 4 | 1U << 6};
	if (raw_syscall(nr_set_thread_area,
			reinterpret_cast<long>(&descriptor)) != 0) {
		put("  set_thread_area failed\n");
		return false;
	}
	const u32 selector = descriptor.entry_number << 3 | 3;
	__asm__ volatile("movw %w0, %%gs" : : "r"(selector));
	const word current = wrap_value;
	ok &= expect("LODSL, XLATB and a load through GS past 4 GiB",
		     probe_wrap_gs(value + 0x1000),
		     2 * current + (current & 0xff));
	ok = ok && install_fault_handlers() &&
	     handle_on_alternate_stack(sigsegv, record_fault);
	ok = ok && delivers("a load indexed past 4 GiB", wrap_index_fault,
			    address_of(probe_wrap_index_end),
			    {sigsegv, segv_maperr, 8,
			     address_of(probe_wrap_index_insn), 14, true, 4});
	ok = ok && delivers("a load below 0", wrap_load_fault,
			    address_of(probe_wrap_load_end),
			    {sigsegv, segv_maperr, 0xfffffffc,
			     address_of(probe_wrap_load_insn), 14, true, 4});
	ok = ok && delivers("a push with ESP 0", probe_wrap_push,
			    address_of(probe_wrap_push_end),
			    {sigsegv, segv_maperr, 0xfffffffc,
			     address_of(probe_wrap_push_insn), 14, true, 6});
	ok = ok && delivers("REP STOSB across 4 GiB", wrap_stos_fault,
			    address_of(probe_wrap_stos_end),
			    {sigsegv, segv_maperr, 0xfffffffe,
			     address_of(probe_wrap_stos_insn), 14, true, 6});
	return ok;
}

/* ESP 0 serves a system call and faults a push with the wrapped address
 * however often it comes on one CPU. */
bool bad_stack_pointers()
{
	bool ok = pin_to_one_cpu() && install_fault_handlers() &&
		  handle_on_alternate_stack(sigsegv, record_fault);
	for (word round = 0; ok && round < bad_stack_rounds; ++round)
		ok = calls_on("ESP 0", 0) &&
		     delivers("a push with ESP 0", probe_wrap_push,
			      address_of(probe_wrap_push_end),
			      {sigsegv, segv_maperr, 0xfffffffc,
			       address_of(probe_wrap_push_insn), 14, true, 6});
	return ok;
}
#endif

extern "C" void probe_xm_div();
extern "C" char probe_xm_div_insn[];
extern "C" char probe_xm_div_end[];
extern "C" void probe_xm_over();
extern "C" char probe_xm_over_insn[];
extern "C" char probe_xm_over_end[];
extern "C" void probe_xm_under();
extern "C" char probe_xm_under_insn[];
extern "C" char probe_xm_under_end[];
extern "C" void probe_xm_prec();
extern "C" char probe_xm_prec_insn[];
extern "C" char probe_xm_prec_end[];
extern "C" void probe_xm_inv();
extern "C" char probe_xm_inv_insn[];
extern "C" char probe_xm_inv_end[];
extern "C" void probe_xm_masked();
extern "C" char probe_xm_masked_insn[];
extern "C" char probe_xm_masked_end[];
extern "C" void probe_mf_div();
extern "C" char probe_mf_div_insn[];
extern "C" char probe_mf_div_end[];
extern "C" void probe_mf_inv();
extern "C" char probe_mf_inv_insn[];
extern "C" char probe_mf_inv_end[];
extern "C" void probe_mf_stack();
extern "C" char probe_mf_stack_insn[];
extern "C" char probe_mf_stack_end[];
extern "C" void probe_mf_fxtract();
extern "C" char probe_mf_fxtract_insn[];
extern "C" char probe_mf_fxtract_end[];
extern "C" void probe_mf_fptan();
extern "C" char probe_mf_fptan_insn[];
extern "C" char probe_mf_fptan_end[];
extern "C" void probe_mf_fsincos();
extern "C" char probe_mf_fsincos_insn[];
extern "C" char probe_mf_fsincos_end[];
extern "C" void probe_mf_unmask();
extern "C" char probe_mf_unmask_insn[];
extern "C" char probe_mf_unmask_end[];
extern "C" void probe_mf_mmx();
extern "C" char probe_mf_mmx_insn[];
extern "C" char probe_mf_mmx_end[];
extern "C" void probe_mf_fist();
extern "C" char probe_mf_fist_insn[];
extern "C" char probe_mf_fist_end[];
extern "C" void probe_delphi_div();
extern "C" char probe_delphi_div_insn[];
extern "C" char probe_delphi_div_end[];
extern "C" void probe_delphi_divsd();
extern "C" char probe_delphi_divsd_insn[];
extern "C" char probe_delphi_divsd_end[];
extern "C" void probe_delphi_clean();
extern "C" char probe_delphi_clean_end[];
extern "C" void probe_delphi_comisd();
extern "C" char probe_delphi_comisd_insn[];
extern "C" char probe_delphi_comisd_end[];
extern "C" void probe_delphi_cvt();
extern "C" char probe_delphi_cvt_insn[];
extern "C" char probe_delphi_cvt_end[];
extern "C" void probe_delphi_mulpd();
extern "C" char probe_delphi_mulpd_insn[];
extern "C" char probe_delphi_mulpd_end[];
extern "C" void probe_delphi_specials();
extern "C" char probe_delphi_specials_end[];
extern "C" volatile u32 probe_delphi_special_out[4];
extern "C" volatile u32 probe_fp_saved[2];
extern "C" volatile u32 probe_delphi_out[8];

/* The MXCSR and x87 control word a signal handler starts with. */
volatile u32 handler_mxcsr;
volatile u16 handler_control;

void record_fp_fault(int signal, void *info, void *context)
{
	u32 mxcsr = 0;
	u16 control = 0;
	__asm__ volatile("stmxcsr %0\n\tfnstcw %1" : "=m"(mxcsr), "=m"(control));
	handler_mxcsr = mxcsr;
	handler_control = control;
	record_fault(signal, info, context);
}

/* Runs `work`, resuming after `end` if it faults, and checks it raised
 * nothing. */
bool raises_nothing(const char *what, void (*work)(), word end)
{
	observed[observed_count] = 0;
	resume_ip = end;
	work();
	resume_ip = 0;
	if (observed[observed_count] == 0)
		return true;
	put("  ");
	put(what);
	put(" raised signal ");
	put_decimal(observed[observed_signal]);
	put("\n");
	return false;
}

/* An unmasked SIMD floating-point exception raises #XM, a fault: SIGFPE
 * with the code of the highest-priority unmasked flag, at the instruction,
 * whose destination it leaves alone. An unmasked x87 exception sets the
 * status word's ES and B and raises #MF, also SIGFPE, at the next waiting
 * instruction, an MMX one included; one checked before the computation
 * leaves the stack and a memory destination alone. Either handler starts
 * with every exception masked. */
bool floating_point_exceptions()
{
	struct Case {
		const char *what;
		void (*work)();
		const char *insn;
		const char *end;
		long code;
		word trapno;
		u32 state;
		u32 value;
		const char *value_of = nullptr;
	};
	/* A `value` of zero is not checked; `value_of` names it when it is
	 * neither XMM0 nor a FISTP destination. */
	const Case cases[] = {
		{"DIVSS by zero", probe_xm_div, probe_xm_div_insn, probe_xm_div_end,
		 fpe_fltdiv, 19, 0x1d84, 0x3f800000},
		{"MULSS overflow", probe_xm_over, probe_xm_over_insn,
		 probe_xm_over_end, fpe_fltovf, 19, 0x1ba8, 0x7f7fffff},
		{"MULSS underflow", probe_xm_under, probe_xm_under_insn,
		 probe_xm_under_end, fpe_fltund, 19, 0x1790, 0x00800001},
		{"DIVSS inexact", probe_xm_prec, probe_xm_prec_insn,
		 probe_xm_prec_end, fpe_fltres, 19, 0x0fa0, 0x3f800000},
		{"SQRTSS of -1", probe_xm_inv, probe_xm_inv_insn,
		 probe_xm_inv_end, fpe_fltinv, 19, 0x1f01, 0x3f800000},
		{"FDIVP by zero", probe_mf_div, probe_mf_div_insn,
		 probe_mf_div_end, fpe_fltdiv, 16, 0xb084, 0},
		{"FSQRT of -1", probe_mf_inv, probe_mf_inv_insn,
		 probe_mf_inv_end, fpe_fltinv, 16, 0xb881, 0},
		{"x87 stack overflow", probe_mf_stack, probe_mf_stack_insn,
		 probe_mf_stack_end, fpe_fltinv, 16, 0x82c1, 0},
		{"FXTRACT on a full stack", probe_mf_fxtract,
		 probe_mf_fxtract_insn, probe_mf_fxtract_end, fpe_fltinv, 16,
		 0x82c1, 0x40490fdb, "x87 top of stack after"},
		{"FPTAN on a full stack", probe_mf_fptan, probe_mf_fptan_insn,
		 probe_mf_fptan_end, fpe_fltinv, 16, 0x82c1, 0x40490fdb,
		 "x87 top of stack after"},
		{"FSINCOS on a full stack", probe_mf_fsincos,
		 probe_mf_fsincos_insn, probe_mf_fsincos_end, fpe_fltinv, 16,
		 0x82c1, 0x40490fdb, "x87 top of stack after"},
		{"FLDCW unmasking a pending flag", probe_mf_unmask,
		 probe_mf_unmask_insn, probe_mf_unmask_end, fpe_fltdiv, 16,
		 0xb884, 0},
		{"MMX after a pending exception", probe_mf_mmx,
		 probe_mf_mmx_insn, probe_mf_mmx_end, fpe_fltdiv, 16, 0xb084, 0},
		{"FISTP out of range", probe_mf_fist, probe_mf_fist_insn,
		 probe_mf_fist_end, fpe_fltinv, 16, 0xb881, 0x12345678},
		{"FDIVP by zero under FCW $1332", probe_delphi_div,
		 probe_delphi_div_insn, probe_delphi_div_end, fpe_fltdiv, 16,
		 0xb084, 0},
		{"DIVSD by zero under MXCSR $1900", probe_delphi_divsd,
		 probe_delphi_divsd_insn, probe_delphi_divsd_end, fpe_fltdiv, 19,
		 0x1904, 0x3ff00000},
		{"COMISD of a quiet NaN under MXCSR $1900", probe_delphi_comisd,
		 probe_delphi_comisd_insn, probe_delphi_comisd_end, fpe_fltinv, 19,
		 0x1901, 0x7ff80000},
		{"CVTTSD2SI out of range under MXCSR $1900", probe_delphi_cvt,
		 probe_delphi_cvt_insn, probe_delphi_cvt_end, fpe_fltinv, 19,
		 0x1901, 0x4415af1d},
		{"MULPD overflow under MXCSR $1900", probe_delphi_mulpd,
		 probe_delphi_mulpd_insn, probe_delphi_mulpd_end, fpe_fltovf, 19,
		 0x1928, 0x3ff00000},
	};
	bool ok = install(sigfpe, record_fp_fault);
	for (const Case &one : cases) {
		const word insn = address_of(one.insn);
		handler_mxcsr = 0;
		handler_control = 0;
		bool one_ok = delivers(one.what, one.work, address_of(one.end),
				       {sigfpe, one.code, insn, insn, one.trapno,
					true, 0});
		one_ok &= expect(one.trapno == 19 ? "MXCSR after" :
					"x87 status after", probe_fp_saved[0],
				 one.state);
		if (one.value != 0)
			one_ok &= expect(one.value_of ? one.value_of :
					 one.trapno == 19 ? "XMM0 after" :
						"FISTP destination after",
					 probe_fp_saved[1], one.value);
		one_ok &= expect("handler MXCSR", handler_mxcsr, 0x1f80);
		one_ok &= expect("handler x87 control", handler_control, 0x37f);
		if (!one_ok) {
			put("  in ");
			put(one.what);
			put("\n");
		}
		ok &= one_ok;
	}
	/* Delphi's control words raise nothing for arithmetic that raises
	 * none of the exceptions they unmask. The status word is compared
	 * without C1, which records whether the last rounding went up, and
	 * MXCSR without the flags of the masked denormal, underflow and
	 * precision exceptions, which the engine records only for an
	 * instruction it runs checked. */
	ok &= raises_nothing("arithmetic under FCW $1332 and MXCSR $1900",
			     probe_delphi_clean,
			     address_of(probe_delphi_clean_end));
	for (unsigned index = 0; index < 8; ++index) {
		static const char *const what[] = {
			"x87 result", "x87 status", "x87 double low",
			"x87 double high", "SSE double low", "SSE double high",
			"SSE result", "MXCSR",
		};
		/* As native Linux reports them. */
		static const u32 native[] = {
			6, 0x20, 0xaa22168c, 0x401890fd, 0x55555556, 0x40155555,
			5, 0x1920,
		};
		const u32 mask = index == 1 ? 0xfdffU : index == 7 ? ~0x32U :
			~0U;
		ok &= expect(what[index], probe_delphi_out[index] & mask,
			     native[index] & mask);
	}
	ok &= raises_nothing("an infinity and a quiet NaN under MXCSR $1900",
			     probe_delphi_specials,
			     address_of(probe_delphi_specials_end));
	for (unsigned index = 0; index < 4; ++index) {
		static const char *const what[] = {
			"infinity plus one low", "infinity plus one high",
			"UCOMISD parity", "MXCSR after specials",
		};
		static const u32 native[] = {0, 0x7ff00000, 1, 0x1900};
		ok &= expect(what[index], probe_delphi_special_out[index],
			     native[index]);
	}
	/* A masked exception only sets its flag. */
	ok &= raises_nothing("masked DIVSS by zero", probe_xm_masked,
			     address_of(probe_xm_masked_end));
	ok &= expect("MXCSR after a masked exception", probe_fp_saved[0],
		     0x0f84);
	ok &= expect("XMM0 after a masked exception", probe_fp_saved[1],
		     0x7f800000);
	return ok;
}

/* With RFLAGS.AC set, one access through (buffer); each case clears AC, and
 * restores the stack, after its instruction. */
__asm__(".text\n"
	".global probe_ac_load\n"
	"probe_ac_load:\n"
	PROBE_ARG_AX
	"	pushf\n"
	"	orl $0x40000, (" PROBE_SP ")\n"
	"	popf\n"
	".global probe_ac_load_insn\n"
	"probe_ac_load_insn:\n"
	"	movl 1(" PROBE_AX "), %edx\n"
	".global probe_ac_load_end\n"
	"probe_ac_load_end:\n"
	"	pushf\n"
	"	andl $~0x40000, (" PROBE_SP ")\n"
	"	popf\n"
	"	ret\n"
	".global probe_ac_word\n"
	"probe_ac_word:\n"
	PROBE_ARG_AX
	"	pushf\n"
	"	orl $0x40000, (" PROBE_SP ")\n"
	"	popf\n"
	".global probe_ac_word_insn\n"
	"probe_ac_word_insn:\n"
	"	movw 2(" PROBE_AX "), %dx\n"
	".global probe_ac_word_end\n"
	"probe_ac_word_end:\n"
	"	pushf\n"
	"	andl $~0x40000, (" PROBE_SP ")\n"
	"	popf\n"
	"	ret\n"
	".global probe_ac_byte\n"
	"probe_ac_byte:\n"
	PROBE_ARG_AX
	"	pushf\n"
	"	orl $0x40000, (" PROBE_SP ")\n"
	"	popf\n"
	".global probe_ac_byte_insn\n"
	"probe_ac_byte_insn:\n"
	"	movb 1(" PROBE_AX "), %dl\n"
	".global probe_ac_byte_end\n"
	"probe_ac_byte_end:\n"
	"	pushf\n"
	"	andl $~0x40000, (" PROBE_SP ")\n"
	"	popf\n"
	"	ret\n"
	".global probe_ac_x87\n"
	"probe_ac_x87:\n"
	PROBE_ARG_AX
	"	pushf\n"
	"	orl $0x40000, (" PROBE_SP ")\n"
	"	popf\n"
	".global probe_ac_x87_insn\n"
	"probe_ac_x87_insn:\n"
	"	fldl 4(" PROBE_AX ")\n"
	".global probe_ac_x87_end\n"
	"probe_ac_x87_end:\n"
	"	pushf\n"
	"	andl $~0x40000, (" PROBE_SP ")\n"
	"	popf\n"
	"	fninit\n"
	"	ret\n"
	".global probe_ac_sse\n"
	"probe_ac_sse:\n"
	PROBE_ARG_AX
	"	pushf\n"
	"	orl $0x40000, (" PROBE_SP ")\n"
	"	popf\n"
	".global probe_ac_sse_insn\n"
	"probe_ac_sse_insn:\n"
	"	movsd 4(" PROBE_AX "), %xmm0\n"
	".global probe_ac_sse_end\n"
	"probe_ac_sse_end:\n"
	"	pushf\n"
	"	andl $~0x40000, (" PROBE_SP ")\n"
	"	popf\n"
	"	ret\n"
	".global probe_ac_movups\n"
	"probe_ac_movups:\n"
	PROBE_ARG_AX
	"	pushf\n"
	"	orl $0x40000, (" PROBE_SP ")\n"
	"	popf\n"
	".global probe_ac_movups_insn\n"
	"probe_ac_movups_insn:\n"
	"	movups 8(" PROBE_AX "), %xmm0\n"
	".global probe_ac_movups_end\n"
	"probe_ac_movups_end:\n"
	"	pushf\n"
	"	andl $~0x40000, (" PROBE_SP ")\n"
	"	popf\n"
	"	ret\n"
	".global probe_ac_push\n"
	"probe_ac_push:\n"
	"	push " PROBE_BP "\n"
	"	mov " PROBE_SP ", " PROBE_BP "\n"
	"	sub $2, " PROBE_SP "\n"
	"	pushf\n"
	"	orl $0x40000, (" PROBE_SP ")\n"
	"	popf\n"
	".global probe_ac_push_insn\n"
	"probe_ac_push_insn:\n"
	"	push " PROBE_AX "\n"
	".global probe_ac_push_end\n"
	"probe_ac_push_end:\n"
	"	mov " PROBE_BP ", " PROBE_SP "\n"
	"	pushf\n"
	"	andl $~0x40000, (" PROBE_SP ")\n"
	"	popf\n"
	"	pop " PROBE_BP "\n"
	"	ret\n"
	);

extern "C" void probe_ac_load(void *buffer);
extern "C" char probe_ac_load_insn[];
extern "C" char probe_ac_load_end[];
extern "C" void probe_ac_word(void *buffer);
extern "C" char probe_ac_word_insn[];
extern "C" char probe_ac_word_end[];
extern "C" void probe_ac_byte(void *buffer);
extern "C" char probe_ac_byte_insn[];
extern "C" char probe_ac_byte_end[];
extern "C" void probe_ac_x87(void *buffer);
extern "C" char probe_ac_x87_insn[];
extern "C" char probe_ac_x87_end[];
extern "C" void probe_ac_sse(void *buffer);
extern "C" char probe_ac_sse_insn[];
extern "C" char probe_ac_sse_end[];
extern "C" void probe_ac_movups(void *buffer);
extern "C" char probe_ac_movups_insn[];
extern "C" char probe_ac_movups_end[];
extern "C" void probe_ac_push(void *buffer);
extern "C" char probe_ac_push_insn[];
extern "C" char probe_ac_push_end[];

alignas(64) unsigned char alignment_buffer[64];

void ac_load() { probe_ac_load(alignment_buffer); }
void ac_word() { probe_ac_word(alignment_buffer); }
void ac_byte() { probe_ac_byte(alignment_buffer); }
void ac_x87() { probe_ac_x87(alignment_buffer); }
/* An 80-bit x87 value as FSTP m80 stores it. */
struct X87Value {
	u64 significand;
	u16 sign_exponent;
} __attribute__((packed));

const int x87_three = 3;
const int x87_minus_six = -6;
const int x87_out_of_short = 40000;
const X87Value x87_largest = {0x8000000000000000ULL, 0x7ffe};
const X87Value x87_quiet_nan = {0xc000000000000000ULL, 0x7fff};

/* Masked x87 exceptions set their sticky status-word flags, which only
 * FNCLEX and FNINIT clear; an operation that raises none leaves them. C1
 * says a result was rounded up, as 1/3, an overflow to infinity and pi
 * stored as a single are. A zero result of FPREM, FSIN or FSINCOS keeps
 * its sign. Each case starts from FNINIT, so FCW is 0x37f and TOP counts
 * its pushes. */
bool x87_status_flags()
{
	struct Case {
		const char *what;
		u16 status;
		u16 expected;
	};
	u16 status[10] = {};
	short stored = 0;
	X87Value value[5] = {};
	__asm__ volatile("fninit\n\tfldz\n\tfld1\n\tfdivp\n\t"
			 "fnstsw %0\n\tfnclex\n\tfnstsw %1"
			 : "=m"(status[0]), "=m"(status[1]) : : "memory");
	__asm__ volatile("fninit\n\tfld1\n\tfidivl %1\n\tfnstsw %0"
			 : "=m"(status[2]) : "m"(x87_three) : "memory");
	__asm__ volatile("fninit\n\tfldt %1\n\tfadd %%st(0), %%st(0)\n\t"
			 "fnstsw %0"
			 : "=m"(status[3]) : "m"(x87_largest) : "memory");
	__asm__ volatile("fninit\n\tfld1\n\tfchs\n\tfsqrt\n\tfnstsw %0\n\t"
			 "fstp %%st(0)\n\tfld1\n\tfld1\n\tfcom %%st(1)\n\t"
			 "fnstsw %1"
			 : "=m"(status[4]), "=m"(status[5]) : : "memory");
	__asm__ volatile("fninit\n\tfldt %1\n\tfld1\n\tfucom %%st(1)\n\t"
			 "fnstsw %0"
			 : "=m"(status[6]) : "m"(x87_quiet_nan) : "memory");
	__asm__ volatile("fninit\n\tfildl %2\n\tfistps %1\n\tfnstsw %0"
			 : "=m"(status[7]), "=m"(stored)
			 : "m"(x87_out_of_short) : "memory");
	__asm__ volatile("fninit\n\tfldpi\n\tfstps %1\n\tfnstsw %0"
			 : "=m"(status[8]), "=m"(value[0].significand)
			 : : "memory");
	__asm__ volatile("fninit\n\tfldz\n\tfchs\n\tfsin\n\tfstpt %0\n\t"
			 "fildl %3\n\tfildl %4\n\tfprem\n\tfstpt %1\n\t"
			 "fninit\n\tfldz\n\tfchs\n\tfsincos\n\tfstp %%st(0)\n\t"
			 "fstpt %2\n\tfninit"
			 : "=m"(value[0]), "=m"(value[1]), "=m"(value[2])
			 : "m"(x87_three), "m"(x87_minus_six)
			 : "memory");
	/* An empty register is a stack underflow: FSTP ST(0) pops it with IE
	 * and SF set, and FCHS and FABS leave the indefinite it reads as. */
	__asm__ volatile("fninit\n\tfstp %%st(0)\n\tfnstsw %0\n\tfninit"
			 : "=m"(status[9]) : : "memory");
	__asm__ volatile("fninit\n\tfchs\n\tfstpt %0\n\tfninit\n\t"
			 "fabs\n\tfstpt %1\n\tfninit"
			 : "=m"(value[3]), "=m"(value[4]) : : "memory");
	const Case cases[] = {
		{"FDIVP of 1 by 0 sets ZE", status[0], 0x3804},
		{"FNCLEX clears the flags", status[1], 0x3800},
		{"FIDIV of 1 by 3 sets PE and C1", status[2], 0x3a20},
		{"FADD overflow sets OE, PE and C1", status[3], 0x3a28},
		{"FSQRT of -1 sets IE", status[4], 0x3801},
		{"IE stays across an ordered FCOM", status[5], 0x7001},
		{"FUCOM of a quiet NaN leaves IE clear", status[6], 0x7500},
		{"FISTP m16 out of range sets only IE", status[7], 0x0001},
		{"FSTP m32 of pi sets PE and C1", status[8], 0x0220},
		{"FSTP ST(0) of an empty register sets IE and SF", status[9],
		 0x0841},
	};
	bool ok = true;
	for (const Case &one : cases) {
		if (!expect("status word", one.status, one.expected)) {
			put("  in ");
			put(one.what);
			put("\n");
			ok = false;
		}
	}
	ok &= expect("FISTP m16 out of range stores", static_cast<u16>(stored),
		     0x8000);
	const char *const zeros[] = {"FSIN of -0", "FPREM of -6 by 3",
				     "FSINCOS of -0, its sine"};
	for (unsigned index = 0; index < 3; ++index) {
		const bool negative_zero = value[index].significand == 0 &&
					   value[index].sign_exponent == 0x8000;
		if (!negative_zero) {
			put("  ");
			put(zeros[index]);
			put(" is not -0: ");
			put_hex(value[index].sign_exponent);
			put(" ");
			put_hex(value[index].significand);
			put("\n");
			ok = false;
		}
	}
	const char *const underflows[] = {"FCHS", "FABS"};
	for (unsigned index = 3; index < 5; ++index) {
		if (value[index].sign_exponent != 0xffff ||
		    value[index].significand != 0xc000000000000000ULL) {
			put("  ");
			put(underflows[index - 3]);
			put(" of an empty register is not the indefinite: ");
			put_hex(value[index].sign_exponent);
			put(" ");
			put_hex(value[index].significand);
			put("\n");
			ok = false;
		}
	}
	return ok;
}

void ac_sse() { probe_ac_sse(alignment_buffer); }
void ac_movups() { probe_ac_movups(alignment_buffer); }
void ac_push() { probe_ac_push(alignment_buffer); }

/* With RFLAGS.AC set, a misaligned access of two, four or eight bytes, an
 * x87 or SSE one included, or a push through a misaligned stack, raises
 * #AC, a fault: SIGBUS with BUS_ADRALN and no address, error code zero. A
 * byte, an aligned access and an unaligned 16-byte SSE access raise
 * nothing. */
bool alignment_checks()
{
	struct Case {
		const char *what;
		void (*work)();
		const char *insn;
		const char *end;
	};
	const Case faults[] = {
		{"MOV of four bytes at +1", ac_load, probe_ac_load_insn,
		 probe_ac_load_end},
		{"FLD of eight bytes at +4", ac_x87, probe_ac_x87_insn,
		 probe_ac_x87_end},
		{"MOVSD of eight bytes at +4", ac_sse, probe_ac_sse_insn,
		 probe_ac_sse_end},
		{"PUSH with the stack two bytes off", ac_push, probe_ac_push_insn,
		 probe_ac_push_end},
	};
	bool ok = install_fault_handlers();
	for (const Case &one : faults)
		ok &= delivers(one.what, one.work, address_of(one.end),
			       {sigbus, bus_adraln, 0, address_of(one.insn), 17,
				true, 0});
	ok &= raises_nothing("MOV of two bytes at +2", ac_word,
			     address_of(probe_ac_word_end));
	ok &= raises_nothing("MOV of a byte at +1", ac_byte,
			     address_of(probe_ac_byte_end));
	ok &= raises_nothing("MOVUPS at +8", ac_movups,
			     address_of(probe_ac_movups_end));
	return ok;
}

constexpr long ptrace_traceme = 0;
constexpr long ptrace_peekuser = 3;
constexpr long ptrace_pokeuser = 6;
constexpr long ptrace_cont = 7;
constexpr long ptrace_getregs = 12;
constexpr long ptrace_singlestep = 9;
constexpr long ptrace_syscall = 24;
constexpr long ptrace_setregs = 13;
constexpr long ptrace_getfpregs = 14;
constexpr long ptrace_setfpregs = 15;
#if defined(__i386__)
constexpr long ptrace_getfpxregs = 18;
constexpr long ptrace_get_thread_area = 25;
#endif

long ptrace(long request, long child, word address, const void *data)
{
	return raw_syscall(nr_ptrace, request, child, static_cast<long>(address),
			   reinterpret_cast<long>(data));
}

long wait_for(long pid, int &status)
{
	return raw_syscall(nr_wait4, pid, reinterpret_cast<long>(&status), 0, 0);
}

extern "C" [[noreturn]] void probe_traced(word tls_entry);
extern "C" char probe_traced_stop[];
extern "C" char probe_traced_resume[];

#if defined(__i386__)
constexpr u32 tls_base = 0x12345000;
/* An x86-64 kernel's GDT holds the i386 TLS entries at 12 to 14, and
 * set_thread_area hands out the first free one. */
constexpr u32 tls_first_entry = 12;
constexpr u32 tls_flags = 1U | 1U << 4 | 1U << 6;
#endif

struct UserDesc {
	u32 entry_number;
	u32 base_addr;
	u32 limit;
	u32 flags;
};

/* FXSAVE: cwd, swd, the abridged tag byte, MXCSR and ST(0). */
constexpr unsigned fxsave_swd = 2;
constexpr unsigned fxsave_twd = 4;
constexpr unsigned fxsave_mxcsr = 24;
constexpr unsigned fxsave_st0 = 32;
#if defined(__i386__)
/* The i387 FSAVE image: 32-bit cwd, swd and twd, then after the pointers
 * eight packed 10-byte registers. */
constexpr unsigned fsave_swd = 4;
constexpr unsigned fsave_twd = 8;
constexpr unsigned fsave_st0 = 28;
#endif

template <typename T> T field(const unsigned char *image, unsigned offset)
{
	T value;
	__builtin_memcpy(&value, image + offset, sizeof(value));
	return value;
}

/* ST(0) holds 1.0 and is the only register in use, at physical 7. */
bool holds_one(const unsigned char *image, unsigned st0)
{
	bool ok = expect("ST(0) significand", field<u64>(image, st0),
			 0x8000000000000000ULL);
	ok &= expect("ST(0) exponent", field<u16>(image, st0 + 8), 0x3fff);
	return ok;
}

bool fxsave_image(const unsigned char *image, u16 control)
{
	bool ok = expect("FXSAVE cwd", field<u16>(image, 0), control);
	ok &= expect("FXSAVE TOP", (field<u16>(image, fxsave_swd) >> 11) & 7, 7);
	ok &= expect("FXSAVE tag", image[fxsave_twd], 0x80);
	ok &= expect("FXSAVE mxcsr", field<u32>(image, fxsave_mxcsr), 0x3f80);
	ok &= holds_one(image, fxsave_st0);
	return ok;
}

#if defined(__i386__)
bool fsave_image(const unsigned char *image, u16 control)
{
	bool ok = expect("FSAVE cwd", field<u32>(image, 0) & 0xffff, control);
	ok &= expect("FSAVE TOP", (field<u32>(image, fsave_swd) >> 11) & 7, 7);
	ok &= expect("FSAVE tag", field<u32>(image, fsave_twd) & 0xffff, 0x3fff);
	ok &= holds_one(image, fsave_st0);
	return ok;
}
#endif

/* What the tracer checks at the child's #UD stop; the child still runs
 * afterwards, so a failure here is not fatal to it. */
bool check_stop(long child)
{
	word regs[user_regs_words] = {};
	bool ok = expect("PTRACE_GETREGS", static_cast<word>(ptrace(
		ptrace_getregs, child, 0, regs)), 0);
	ok &= expect("ip", regs[user_ip], address_of(probe_traced_stop));
	ok &= expect("bx", regs[user_bx], expected_bx);
	ok &= expect("cs", regs[user_cs], expected_cs);
	ok &= expect("ss", regs[user_ss], 0x2b);
	ok &= expect("flags reserved bit and RF",
		     regs[user_flags] & (flag_reserved | flag_rf),
		     flag_reserved | flag_rf);

	word value = 1;
	ok &= expect("PTRACE_PEEKUSER ip", static_cast<word>(ptrace(
		ptrace_peekuser, child, user_ip * sizeof(word), &value)), 0);
	ok &= expect("peeked ip", value, address_of(probe_traced_stop));
	value = 1;
	ok &= expect("PTRACE_PEEKUSER DR7", static_cast<word>(ptrace(
		ptrace_peekuser, child, user_debugreg7, &value)), 0);
	ok &= expect("peeked DR7", value, 0);
	ok &= expect("PTRACE_PEEKUSER past struct user", static_cast<word>(
		ptrace(ptrace_peekuser, child, user_past, &value)),
		static_cast<word>(-eio));

	alignas(16) unsigned char image[512] = {};
#if defined(__x86_64__)
	ok &= expect("PTRACE_GETFPREGS", static_cast<word>(ptrace(
		ptrace_getfpregs, child, 0, image)), 0);
	ok &= fxsave_image(image, 0x027f);
	image[0] = 0x7f;
	image[1] = 0x03;
	ok &= expect("PTRACE_SETFPREGS", static_cast<word>(ptrace(
		ptrace_setfpregs, child, 0, image)), 0);
	__builtin_memset(image, 0, sizeof(image));
	ptrace(ptrace_getfpregs, child, 0, image);
	ok &= fxsave_image(image, 0x037f);
#else
	ok &= expect("PTRACE_GETFPREGS", static_cast<word>(ptrace(
		ptrace_getfpregs, child, 0, image)), 0);
	ok &= fsave_image(image, 0x027f);
	ok &= expect("PTRACE_GETFPXREGS", static_cast<word>(ptrace(
		ptrace_getfpxregs, child, 0, image)), 0);
	ok &= fxsave_image(image, 0x027f);
	/* An FSAVE write changes x87 state and keeps SSE state. */
	ptrace(ptrace_getfpregs, child, 0, image);
	image[0] = 0x7f;
	image[1] = 0x03;
	ok &= expect("PTRACE_SETFPREGS", static_cast<word>(ptrace(
		ptrace_setfpregs, child, 0, image)), 0);
	__builtin_memset(image, 0, sizeof(image));
	ptrace(ptrace_getfpregs, child, 0, image);
	ok &= fsave_image(image, 0x037f);
	__builtin_memset(image, 0, sizeof(image));
	ptrace(ptrace_getfpxregs, child, 0, image);
	ok &= fxsave_image(image, 0x037f);

	UserDesc descriptor = {};
	ok &= expect("PTRACE_GET_THREAD_AREA", static_cast<word>(ptrace(
		ptrace_get_thread_area, child, regs[user_si], &descriptor)), 0);
	ok &= expect("thread area entry", descriptor.entry_number,
		     regs[user_si]);
	ok &= expect("thread area base", descriptor.base_addr, tls_base);
	ok &= expect("the child's TLS entry", regs[user_si], tls_first_entry);
	ok &= expect("PTRACE_GET_THREAD_AREA below the TLS entries",
		     static_cast<word>(ptrace(ptrace_get_thread_area, child, 6,
					      &descriptor)),
		     static_cast<word>(-einval));
#endif

	regs[user_ip] = address_of(probe_traced_resume);
	ok &= expect("PTRACE_SETREGS", static_cast<word>(ptrace(
		ptrace_setregs, child, 0, regs)), 0);
	word reread[user_regs_words] = {};
	ptrace(ptrace_getregs, child, 0, reread);
	ok &= expect("ip after PTRACE_SETREGS", reread[user_ip],
		     address_of(probe_traced_resume));
	ok &= expect("PTRACE_POKEUSER bx", static_cast<word>(ptrace(
		ptrace_pokeuser, child, user_bx * sizeof(word),
		reinterpret_cast<const void *>(42))), 0);
	value = 0;
	ptrace(ptrace_peekuser, child, user_bx * sizeof(word), &value);
	ok &= expect("peeked bx after PTRACE_POKEUSER", value, 42);
	return ok;
}

/* A tracer of the same ABI reads the child's registers, FP state and TLS at
 * a signal stop, and its register writes take effect. */
bool tracer_sees_registers()
{
	const long child = raw_syscall(nr_fork);
	if (child == 0) {
		word tls_entry = 0;
#if defined(__i386__)
		/* 32-bit, 4 KiB-granular, usable: flags seg_32bit,
		 * limit_in_pages and useable. */
		UserDesc descriptor = {~0U, tls_base, 0xfffff, tls_flags};
		if (raw_syscall(nr_set_thread_area,
				reinterpret_cast<long>(&descriptor)) != 0)
			raw_syscall(nr_exit_group, 3);
		tls_entry = descriptor.entry_number;
#endif
		raw_syscall(nr_ptrace, ptrace_traceme, 0, 0, 0);
		probe_traced(tls_entry);
	}
	int status = 0;
	if (child < 0 || wait_for(child, status) != child) {
		put("  fork or wait failed\n");
		return false;
	}
	if ((status & 0xff) != 0x7f || ((status >> 8) & 0xff) != sigill) {
		put("  the child did not stop with SIGILL: status ");
		put_hex(static_cast<u32>(status));
		put("\n");
		raw_syscall(nr_kill, child, sigkill);
		wait_for(child, status);
		return false;
	}
	bool ok = check_stop(child);
	if (ptrace(ptrace_cont, child, 0, nullptr) != 0 ||
	    wait_for(child, status) != child) {
		put("  PTRACE_CONT failed\n");
		raw_syscall(nr_kill, child, sigkill);
		wait_for(child, status);
		return false;
	}
	ok &= expect("child exit status", static_cast<u32>(status), 42 << 8);
	return ok;
}

/* Waits for the child's next stop and checks that `signal` caused it. */
bool stopped_with(long child, long signal, const char *what)
{
	int status = 0;
	if (wait_for(child, status) == child && (status & 0xff) == 0x7f &&
	    ((status >> 8) & 0xff) == signal)
		return true;
	put("  no ");
	put(what);
	put(" stop: status ");
	put_hex(static_cast<u32>(status));
	put("\n");
	return false;
}

/* Resumes the child to its next system call entry or exit. */
bool syscall_stop(long child, const char *what)
{
	return ptrace(ptrace_syscall, child, 0, nullptr) == 0 &&
	       stopped_with(child, sigtrap, what);
}

word peek_user(long child, unsigned index)
{
	word value = 0x5a5a5a5a;
	if (ptrace(ptrace_peekuser, child, index * sizeof(word), &value) != 0)
		put("  PTRACE_PEEKUSER failed\n");
	return value;
}

bool poke_user(long child, unsigned index, word value)
{
	return expect("PTRACE_POKEUSER", static_cast<word>(ptrace(
		ptrace_pokeuser, child, index * sizeof(word),
		reinterpret_cast<const void *>(value))), 0);
}

/* A tracer that rewrites orig_ax at a system call's entry stop runs another
 * call in its place, or with -1 skips it and leaves the AX it wrote; at a
 * stop outside any system call orig_ax reads -1. */
bool tracer_changes_syscalls()
{
	const word self = static_cast<word>(raw_syscall(nr_getpid));
	const long child = raw_syscall(nr_fork);
	if (child == 0) {
		const long parent = raw_syscall(nr_getppid);
		raw_syscall(nr_ptrace, ptrace_traceme, 0, 0, 0);
		raw_syscall(nr_kill, raw_syscall(nr_getpid), sigstop);
		const long replaced = raw_syscall(nr_getpid);
		const long skipped = raw_syscall(nr_getpid);
		__asm__ volatile("int3");
		raw_syscall(nr_exit_group, (replaced == parent ? 0 : 1) |
			    (skipped == 42 ? 0 : 2));
	}
	if (child < 0) {
		put("  fork failed\n");
		return false;
	}
	bool ok = stopped_with(child, sigstop, "SIGSTOP");
	ok = ok && syscall_stop(child, "getpid entry");
	ok = ok && expect("orig_ax at getpid's entry",
			  peek_user(child, user_orig_ax), nr_getpid);
	ok = ok && poke_user(child, user_orig_ax, nr_getppid);
	ok = ok && syscall_stop(child, "replaced call's exit");
	ok = ok && expect("AX of getppid run in getpid's place",
			  peek_user(child, user_ax), self);
	ok = ok && syscall_stop(child, "second getpid entry");
	ok = ok && poke_user(child, user_orig_ax, ~word{0});
	ok = ok && poke_user(child, user_ax, 42);
	ok = ok && syscall_stop(child, "skipped call's exit");
	ok = ok && expect("AX of a skipped call", peek_user(child, user_ax), 42);
	ok = ok && ptrace(ptrace_cont, child, 0, nullptr) == 0 &&
	     stopped_with(child, sigtrap, "INT3");
	ok = ok && expect("orig_ax at an INT3 stop",
			  peek_user(child, user_orig_ax), ~word{0});
	int status = 0;
	if (!ok || ptrace(ptrace_cont, child, 0, nullptr) != 0 ||
	    wait_for(child, status) != child) {
		raw_syscall(nr_kill, child, sigkill);
		wait_for(child, status);
		return false;
	}
	return expect("child exit status", static_cast<u32>(status), 0);
}

extern "C" void probe_debug_child();

/* DR6 reads with its reserved bits set; BS, 0x4000, marks a single step. */
constexpr word dr6_idle = 0xffff0ff0;
constexpr word dr6_step = 0x4000;
/* DR7: breakpoint 0 enabled, a 4-byte write watchpoint. */
constexpr word dr7_watch_write = 0xd0001;

alignas(8) volatile u32 watched;

word peek_debugreg(long child, unsigned index)
{
	word value = 0x5a5a5a5a;
	if (ptrace(ptrace_peekuser, child,
		   user_debugreg + index * sizeof(word), &value) != 0)
		put("  PTRACE_PEEKUSER of a debug register failed\n");
	return value;
}

word poke_debugreg(long child, unsigned index, word value)
{
	return static_cast<word>(ptrace(ptrace_pokeuser, child,
		user_debugreg + index * sizeof(word),
		reinterpret_cast<const void *>(value)));
}

/* A tracer sees the debug registers x86 Linux keeps: DR6 with its reserved
 * bits set, BS after a single step and clear after ICEBP; DR0 to DR3 and
 * DR7 as written, a kernel address or an invalid length and type refused.
 * A write that enables a watchpoint either arms one that fires or fails. */
bool tracer_debug_registers()
{
	const long child = raw_syscall(nr_fork);
	if (child == 0) {
		raw_syscall(nr_ptrace, ptrace_traceme, 0, 0, 0);
		raw_syscall(nr_kill, raw_syscall(nr_getpid), sigstop);
		probe_debug_child();
		watched = 1;
		raw_syscall(nr_exit_group, 0);
	}
	if (child < 0) {
		put("  fork failed\n");
		return false;
	}
	bool ok = stopped_with(child, sigstop, "SIGSTOP");
	ok = ok && expect("DR6 of a new task", peek_debugreg(child, 6),
			  dr6_idle);
	ok = ok && expect("DR7 of a new task", peek_debugreg(child, 7), 0);
	ok = ok && ptrace(ptrace_singlestep, child, 0, nullptr) == 0 &&
	     stopped_with(child, sigtrap, "single-step");
	ok = ok && expect("DR6 after a single step", peek_debugreg(child, 6),
			  dr6_idle | dr6_step);
	ok = ok && ptrace(ptrace_cont, child, 0, nullptr) == 0 &&
	     stopped_with(child, sigtrap, "ICEBP");
	ok = ok && expect("DR6 after ICEBP", peek_debugreg(child, 6), dr6_idle);
	long armed = -1;
	if (ok) {
		const word address = reinterpret_cast<word>(&watched);
		ok &= expect("PTRACE_POKEUSER DR6", poke_debugreg(child, 6, 0), 0);
		ok &= expect("DR6 as written", peek_debugreg(child, 6), 0);
		ok &= expect("PTRACE_POKEUSER DR6 back",
			     poke_debugreg(child, 6, dr6_idle), 0);
		ok &= expect("PTRACE_POKEUSER DR4", poke_debugreg(child, 4, 1),
			     static_cast<word>(-eio));
		ok &= expect("PTRACE_POKEUSER DR5", poke_debugreg(child, 5, 0),
			     static_cast<word>(-eio));
		ok &= expect("PTRACE_POKEUSER DR1",
			     poke_debugreg(child, 1, address), 0);
		ok &= expect("DR1 as written", peek_debugreg(child, 1), address);
#if defined(__x86_64__)
		ok &= expect("PTRACE_POKEUSER DR2 of a kernel address",
			     poke_debugreg(child, 2, 0xffffffff81000000UL),
			     static_cast<word>(-einval));
		ok &= expect("DR2 after a refused write",
			     peek_debugreg(child, 2), 0);
#endif
		ok &= expect("PTRACE_POKEUSER DR7 without an enable",
			     poke_debugreg(child, 7, 0x100), 0);
		ok &= expect("DR7 as written", peek_debugreg(child, 7), 0x100);
		ok &= expect("PTRACE_POKEUSER DR7 of a 4-byte execute breakpoint",
			     poke_debugreg(child, 7, 0xc0001),
			     static_cast<word>(-einval));
		ok &= expect("DR7 after a refused write",
			     peek_debugreg(child, 7), 0x100);
		ok &= expect("PTRACE_POKEUSER DR0",
			     poke_debugreg(child, 0, address), 0);
		armed = static_cast<long>(
			poke_debugreg(child, 7, dr7_watch_write));
		if (armed == 0)
			ok &= expect("DR7 as armed", peek_debugreg(child, 7),
				     dr7_watch_write);
		else
			ok &= expect("PTRACE_POKEUSER DR7 arming",
				     static_cast<word>(armed),
				     static_cast<word>(-einval)) &&
			      expect("DR7 after refused arming",
				     peek_debugreg(child, 7), 0x100);
	}
	if (ok && armed == 0) {
		ok = ptrace(ptrace_cont, child, 0, nullptr) == 0 &&
		     stopped_with(child, sigtrap, "watchpoint");
		ok = ok && expect("DR6 B0 after the watchpoint",
				  peek_debugreg(child, 6) & 0xf, 1);
		ok = ok && expect("PTRACE_POKEUSER DR7 clear",
				  poke_debugreg(child, 7, 0), 0);
	}
	int status = 0;
	if (!ok || ptrace(ptrace_cont, child, 0, nullptr) != 0 ||
	    wait_for(child, status) != child) {
		raw_syscall(nr_kill, child, sigkill);
		wait_for(child, status);
		return false;
	}
	return expect("child exit status", static_cast<u32>(status), 0);
}

#if defined(__i386__)
extern "C" u32 probe_gs_word(u32 selector);

alignas(16) u32 tls_block[4] = {0x5a5aa5a5, 0, 0, 0};

/* set_thread_area with entry -1 takes the first TLS entry, 12, whose
 * selector 0x63 then addresses the new base, as glibc's GS does; entries
 * outside 12 to 14 are refused. */
bool tls_entries()
{
	UserDesc descriptor = {~0U, reinterpret_cast<u32>(tls_block), 0xfffff,
			       tls_flags};
	bool ok = expect("set_thread_area", static_cast<word>(raw_syscall(
		nr_set_thread_area, reinterpret_cast<long>(&descriptor))), 0);
	ok &= expect("entry set_thread_area chose", descriptor.entry_number,
		     tls_first_entry);
	const u32 selector = descriptor.entry_number << 3 | 3;
	ok &= expect("its selector", selector, 0x63);
	ok &= expect("GS:0 through it", probe_gs_word(selector), tls_block[0]);
	UserDesc read = {tls_first_entry, 0, 0, 0};
	ok &= expect("get_thread_area", static_cast<word>(raw_syscall(
		nr_get_thread_area, reinterpret_cast<long>(&read))), 0);
	ok &= expect("its base", read.base_addr,
		     reinterpret_cast<u32>(tls_block));
	UserDesc old = {6, 0, 0, 0};
	ok &= expect("get_thread_area of entry 6", static_cast<word>(
		raw_syscall(nr_get_thread_area, reinterpret_cast<long>(&old))),
		static_cast<word>(-einval));
	old = {6, reinterpret_cast<u32>(tls_block), 0xfffff, tls_flags};
	ok &= expect("set_thread_area of entry 6", static_cast<word>(
		raw_syscall(nr_set_thread_area, reinterpret_cast<long>(&old))),
		static_cast<word>(-einval));
	return ok;
}
#endif

extern "C" void probe_rep_step(const void *left, const void *right);
extern "C" char probe_rep_step_insn[];
extern "C" char probe_rep_step_end[];
extern "C" char probe_rep_zero_insn[];
extern "C" char probe_rep_zero_end[];

const unsigned char rep_left[5] = {1, 2, 3, 4, 5};
const unsigned char rep_right[5] = {1, 2, 9, 4, 5};

struct StepStop {
	word ip;
	word cx;
	bool resume_flag;
};

/* Steps the child until it leaves the instruction at `insn`, which it
 * must be at, and checks each stop against `expected`. */
bool steps_through(long child, word insn, const StepStop *expected,
		   unsigned count)
{
	bool ok = true;
	for (unsigned index = 0; ok && index < count; ++index) {
		ok = ptrace(ptrace_singlestep, child, 0, nullptr) == 0 &&
		     stopped_with(child, sigtrap, "single-step");
		word regs[user_regs_words] = {};
		ok = ok && ptrace(ptrace_getregs, child, 0, regs) == 0;
		ok = ok && expect("ip after a step", regs[user_ip],
				  expected[index].ip);
		ok = ok && expect("cx after a step", regs[user_cx],
				  expected[index].cx);
		ok = ok && expect("RF after a step",
				  (regs[user_flags] & flag_rf) != 0,
				  expected[index].resume_flag);
		if (!ok) {
			put("  at step ");
			put_decimal(index + 1);
			put(" of the instruction at ");
			put_hex(insn);
			put("\n");
		}
	}
	return ok;
}

/* A single step of a repeated string instruction runs one iteration and
 * traps with RIP still naming the instruction and RF set, until an
 * iteration ends it; one with a zero count steps over once. */
bool tracer_steps_repeats()
{
	const long child = raw_syscall(nr_fork);
	if (child == 0) {
		raw_syscall(nr_ptrace, ptrace_traceme, 0, 0, 0);
		raw_syscall(nr_kill, raw_syscall(nr_getpid), sigstop);
		probe_rep_step(rep_left, rep_right);
		raw_syscall(nr_exit_group, 0);
	}
	if (child < 0) {
		put("  fork failed\n");
		return false;
	}
	bool ok = stopped_with(child, sigstop, "SIGSTOP");
	const word insn = address_of(probe_rep_step_insn);
	/* Up to the REPE CMPSB, a few instructions of the child's own. */
	for (int steps = 0; ok; ++steps) {
		word regs[user_regs_words] = {};
		ok = ptrace(ptrace_getregs, child, 0, regs) == 0;
		if (!ok || regs[user_ip] == insn)
			break;
		ok = steps < 1000 &&
		     ptrace(ptrace_singlestep, child, 0, nullptr) == 0 &&
		     stopped_with(child, sigtrap, "single-step");
	}
	const StepStop compare[] = {
		{insn, 4, true},
		{insn, 3, true},
		{address_of(probe_rep_step_end), 2, false},
	};
	ok = ok && steps_through(child, insn, compare, 3);
	const StepStop clear[] = {{address_of(probe_rep_zero_insn), 0, false}};
	ok = ok && steps_through(child, address_of(probe_rep_step_end), clear, 1);
	const StepStop none[] = {{address_of(probe_rep_zero_end), 0, false}};
	ok = ok && steps_through(child, address_of(probe_rep_zero_insn), none, 1);
	int status = 0;
	if (!ok || ptrace(ptrace_cont, child, 0, nullptr) != 0 ||
	    wait_for(child, status) != child) {
		raw_syscall(nr_kill, child, sigkill);
		wait_for(child, status);
		return false;
	}
	return expect("child exit status", static_cast<u32>(status), 0);
}

/* Segments as Linux user code sees them: the GDT entries Linux gives every
 * task and an LDT that modify_ldt installs. */
struct LdtEntry {
	u32 entry_number;
	u32 base_addr;
	u32 limit;
	u32 flags;
};

constexpr u32 ldt_32bit = 1U;
constexpr u32 ldt_expand_down = 1U << 1;
constexpr u32 ldt_code = 2U << 1;
constexpr u32 ldt_read_only = 1U << 3;
constexpr u32 ldt_pages = 1U << 4;
constexpr u32 ldt_not_present = 1U << 5;
#if defined(__x86_64__)
constexpr long nr_modify_ldt = 154;
#else
constexpr long nr_modify_ldt = 123;
#endif

/* The LDT the case installs: a 256-byte data segment over segment_buffer,
 * a flat 32-bit code segment, a data segment that is not present, a
 * read-only data segment over segment_buffer, an expand-down data segment
 * whose lowest offset, 0x1000, is segment_buffer, a 128-byte stack
 * segment over segment_stack, and data segments of 512, 576 and 832 bytes
 * over segment_buffer: the extents of FXSAVE, of XSAVE without the AVX
 * component and of XSAVE with it. */
constexpr u32 ldt_data = 0x07;
constexpr u32 ldt_code32 = 0x0f;
constexpr u32 ldt_absent = 0x17;
constexpr u32 ldt_read_only_data = 0x1f;
constexpr u32 ldt_down = 0x27;

alignas(4096) unsigned char segment_buffer[8192];
alignas(4096) unsigned char segment_stack[4096];
/* A far pointer an instruction reads, and room either side of it. */
alignas(16) unsigned char segment_pointer[32];

bool write_ldt(u32 entry, word base, u32 limit, u32 flags)
{
	LdtEntry descriptor = {entry, static_cast<u32>(base), limit, flags};
	return raw_syscall(nr_modify_ldt, 0x11,
			   reinterpret_cast<long>(&descriptor),
			   sizeof(descriptor)) == 0;
}

bool install_ldt()
{
	const word buffer = reinterpret_cast<word>(segment_buffer);
	if (write_ldt(0, buffer, 0xff, ldt_32bit) &&
	    write_ldt(1, 0, 0xfffff, ldt_32bit | ldt_code | ldt_pages) &&
	    write_ldt(2, buffer, 0xff, ldt_32bit | ldt_not_present) &&
	    write_ldt(3, buffer, 0xff, ldt_32bit | ldt_read_only) &&
	    write_ldt(4, buffer - 0x1000, 0xfff, ldt_32bit | ldt_expand_down) &&
	    write_ldt(5, reinterpret_cast<word>(segment_stack), 0x7f, ldt_32bit) &&
	    write_ldt(6, buffer, 0x1ff, ldt_32bit) &&
	    write_ldt(7, buffer, 0x23f, ldt_32bit) &&
	    write_ldt(8, buffer, 0x33f, ldt_32bit))
		return true;
	put("  modify_ldt failed\n");
	return false;
}

/* modify_ldt reads back the descriptors it wrote, accessed and at DPL 3. */
bool ldt_reads_back()
{
	u64 table[10] = {};
	const long bytes = raw_syscall(nr_modify_ldt, 0,
				       reinterpret_cast<long>(table),
				       sizeof(table));
	const u64 base = reinterpret_cast<word>(segment_buffer);
	const u64 data = 0xffULL | (base & 0xffffffULL) << 16 |
			 0x40f3ULL << 40 | (base >> 24 & 0xffULL) << 56;
	bool ok = expect("modify_ldt read size", static_cast<word>(bytes),
			 sizeof(table));
	ok &= expect("LDT entry 0", table[0], data);
	ok &= expect("LDT entry 1", table[1], 0x00cffb000000ffffULL);
	ok &= expect("LDT entry 9", table[9], 0);
	return ok;
}

extern "C" word probe_seg_lar(word selector);
extern "C" word probe_seg_lsl(word selector);
extern "C" word probe_seg_verr(word selector);
extern "C" word probe_seg_verw(word selector);
extern "C" void probe_seg_movss_null();
extern "C" char probe_seg_movss_null_insn[];
extern "C" char probe_seg_movss_null_end[];
extern "C" void probe_seg_movds_far();
extern "C" char probe_seg_movds_far_insn[];
extern "C" char probe_seg_movds_far_end[];
extern "C" void probe_seg_absent();
extern "C" char probe_seg_absent_insn[];
extern "C" char probe_seg_absent_end[];
extern "C" void probe_seg_farjmp_data(void *pointer);
extern "C" char probe_seg_farjmp_data_insn[];
extern "C" char probe_seg_farjmp_data_end[];
extern "C" void probe_seg_lss_null(void *pointer);
extern "C" char probe_seg_lss_null_insn[];
extern "C" char probe_seg_lss_null_end[];
extern "C" word probe_seg_ldt_call(void *pointer);
extern "C" word probe_seg_fs_read(word selector);
extern "C" void probe_seg_iret_bad();
extern "C" char probe_seg_iret_bad_insn[];
extern "C" char probe_seg_iret_bad_end[];

void seg_farjmp_data() { probe_seg_farjmp_data(segment_pointer); }
void seg_lss_null() { probe_seg_lss_null(segment_pointer); }

#if defined(__x86_64__)
__asm__(".text\n"
	"probe_seg_lar: mov $-1, %rax\n lar %edi, %eax\n jz 1f\n mov $-1, %rax\n1: ret\n"
	"probe_seg_lsl: mov $-1, %rax\n lsl %edi, %eax\n jz 1f\n mov $-1, %rax\n1: ret\n"
	"probe_seg_verr: xor %eax, %eax\n verr %di\n setz %al\n ret\n"
	"probe_seg_verw: xor %eax, %eax\n verw %di\n setz %al\n ret\n"
	"probe_seg_movss_null: xor %eax, %eax\n"
	"probe_seg_movss_null_insn: mov %eax, %ss\n"
	"probe_seg_movss_null_end: ret\n"
	"probe_seg_movds_far: mov $0x4142, %eax\n"
	"probe_seg_movds_far_insn: mov %eax, %ds\n"
	"probe_seg_movds_far_end: xor %eax, %eax\n mov %eax, %ds\n ret\n"
	"probe_seg_absent: mov $0x17, %eax\n"
	"probe_seg_absent_insn: mov %eax, %es\n"
	"probe_seg_absent_end: xor %eax, %eax\n mov %eax, %es\n ret\n"
	"probe_seg_farjmp_data: movl $0, (%rdi)\n movw $0x2b, 4(%rdi)\n"
	"probe_seg_farjmp_data_insn: ljmp *(%rdi)\n"
	"probe_seg_farjmp_data_end: ret\n"
	"probe_seg_lss_null: movl $0, (%rdi)\n movw $0, 4(%rdi)\n"
	"probe_seg_lss_null_insn: lss (%rdi), %eax\n"
	"probe_seg_lss_null_end: ret\n"
	/* A far CALL through m16:32 to the LDT's 32-bit code segment, which
	 * runs 0x82 as group 1 and returns with a 32-bit far RET, on a stack
	 * below 4 GiB. */
	"probe_seg_ldt_call: push %r12\n mov %rsp, %r12\n"
	"	lea segment_stack_top(%rip), %rsp\n"
	"	lea seg_ldt_target(%rip), %rax\n mov %eax, (%rdi)\n movw $0x0f, 4(%rdi)\n"
	"	mov $0x10, %eax\n lcall *(%rdi)\n"
	"	mov %r12, %rsp\n pop %r12\n ret\n"
	".code32\n"
	"seg_ldt_target: .byte 0x82, 0xc0, 0x05\n mov %cs, %ecx\n shl $16, %ecx\n or %ecx, %eax\n lret\n"
	".code64\n"
	"probe_seg_fs_read: mov %edi, %fs\n mov %fs:4, %eax\n xor %ecx, %ecx\n mov %ecx, %fs\n ret\n"
	/* IRETQ to a data selector as CS. */
	"probe_seg_iret_bad: mov %rsp, %r11\n pushq $0x2b\n push %r11\n pushfq\n pushq $0x2b\n"
	"	lea probe_seg_iret_bad_end(%rip), %rax\n push %rax\n"
	"probe_seg_iret_bad_insn: iretq\n"
	"probe_seg_iret_bad_end: mov %r11, %rsp\n ret\n"
	"probe_seg_cx16: push %rbx\n xor %eax, %eax\n xor %edx, %edx\n xor %ebx, %ebx\n xor %ecx, %ecx\n"
	"probe_seg_cx16_insn: lock cmpxchg16b (%rdi)\n"
	"probe_seg_cx16_end: pop %rbx\n ret\n"
	"probe_seg_cx16_plain: push %rbx\n xor %eax, %eax\n xor %edx, %edx\n xor %ebx, %ebx\n xor %ecx, %ecx\n"
	"probe_seg_cx16_plain_insn: cmpxchg16b (%rdi)\n"
	"probe_seg_cx16_plain_end: pop %rbx\n ret\n"
	".pushsection .bss\n"
	".balign 16\n"
	"segment_low_stack: .skip 4096\n"
	"segment_stack_top:\n"
	".popsection\n");

extern "C" void probe_seg_cx16(void *operand);
extern "C" char probe_seg_cx16_insn[];
extern "C" char probe_seg_cx16_end[];
extern "C" void probe_seg_cx16_plain(void *operand);
extern "C" char probe_seg_cx16_plain_insn[];
extern "C" char probe_seg_cx16_plain_end[];

void seg_cx16() { probe_seg_cx16(segment_buffer + 8); }
void seg_cx16_plain() { probe_seg_cx16_plain(segment_buffer + 24); }
#else
__asm__(".text\n"
	"probe_seg_lar: mov 4(%esp), %ecx\n mov $-1, %eax\n lar %ecx, %eax\n jz 1f\n mov $-1, %eax\n1: ret\n"
	"probe_seg_lsl: mov 4(%esp), %ecx\n mov $-1, %eax\n lsl %ecx, %eax\n jz 1f\n mov $-1, %eax\n1: ret\n"
	"probe_seg_verr: mov 4(%esp), %ecx\n xor %eax, %eax\n verr %cx\n setz %al\n ret\n"
	"probe_seg_verw: mov 4(%esp), %ecx\n xor %eax, %eax\n verw %cx\n setz %al\n ret\n"
	"probe_seg_movss_null: xor %eax, %eax\n"
	"probe_seg_movss_null_insn: mov %eax, %ss\n"
	"probe_seg_movss_null_end: ret\n"
	"probe_seg_movds_far: mov $0x4142, %eax\n"
	"probe_seg_movds_far_insn: mov %eax, %ds\n"
	"probe_seg_movds_far_end: mov $0x2b, %eax\n mov %eax, %ds\n ret\n"
	"probe_seg_absent: mov $0x17, %eax\n"
	"probe_seg_absent_insn: mov %eax, %es\n"
	"probe_seg_absent_end: mov $0x2b, %eax\n mov %eax, %es\n ret\n"
	"probe_seg_farjmp_data: mov 4(%esp), %ecx\n movl $0, (%ecx)\n movw $0x2b, 4(%ecx)\n"
	"probe_seg_farjmp_data_insn: ljmp *(%ecx)\n"
	"probe_seg_farjmp_data_end: ret\n"
	"probe_seg_lss_null: mov 4(%esp), %ecx\n movl $0, (%ecx)\n movw $0, 4(%ecx)\n"
	"probe_seg_lss_null_insn: lss (%ecx), %eax\n"
	"probe_seg_lss_null_end: ret\n"
	/* A direct far CALL to the LDT's 32-bit code segment. */
	"probe_seg_ldt_call: mov $0x10, %eax\n lcall $0x0f, $seg_ldt_target\n ret\n"
	"seg_ldt_target: .byte 0x82, 0xc0, 0x05\n mov %cs, %ecx\n shl $16, %ecx\n or %ecx, %eax\n lret\n"
	"probe_seg_fs_read: mov 4(%esp), %ecx\n mov %ecx, %fs\n mov %fs:4, %eax\n xor %ecx, %ecx\n mov %ecx, %fs\n ret\n"
	/* IRET to a data selector as CS. */
	"probe_seg_iret_bad: mov %esp, seg_saved_esp\n pushf\n push $0x2b\n push $probe_seg_iret_bad_end\n"
	"probe_seg_iret_bad_insn: iret\n"
	"probe_seg_iret_bad_end: mov seg_saved_esp, %esp\n ret\n"
	/* A read through ES of the offset in the second argument, with the
	 * selector in the first. */
	"probe_seg_es_read: mov 4(%esp), %eax\n mov 8(%esp), %ecx\n mov %eax, %es\n"
	"probe_seg_es_read_insn: mov %es:(%ecx), %eax\n"
	"probe_seg_es_read_end: mov $0x2b, %ecx\n mov %ecx, %es\n ret\n"
	/* XSAVE, XRSTOR and FXSAVE of ES:0 with RFBM in EDX:EAX, the second
	 * argument, through the selector in the first. */
	"probe_seg_xsave: mov 4(%esp), %ecx\n mov 8(%esp), %eax\n mov %ecx, %es\n xor %edx, %edx\n xor %ecx, %ecx\n"
	"probe_seg_xsave_insn: xsave %es:(%ecx)\n"
	"probe_seg_xsave_end: mov $0x2b, %ecx\n mov %ecx, %es\n ret\n"
	"probe_seg_xrstor: mov 4(%esp), %ecx\n mov 8(%esp), %eax\n mov %ecx, %es\n xor %edx, %edx\n xor %ecx, %ecx\n"
	"probe_seg_xrstor_insn: xrstor %es:(%ecx)\n"
	"probe_seg_xrstor_end: mov $0x2b, %ecx\n mov %ecx, %es\n ret\n"
	"probe_seg_fxsave: mov 4(%esp), %ecx\n mov %ecx, %es\n xor %ecx, %ecx\n"
	"probe_seg_fxsave_insn: fxsave %es:(%ecx)\n"
	"probe_seg_fxsave_end: mov $0x2b, %ecx\n mov %ecx, %es\n ret\n"
	/* REP STOSB from ES:0xf0 for 0x20 bytes, across the limit 0xff. */
	"probe_seg_stos: push %edi\n mov $0x07, %eax\n mov %eax, %es\n mov $0xf0, %edi\n"
	"	mov $0x20, %ecx\n mov $0x5a, %al\n cld\n"
	"probe_seg_stos_insn: rep stosb\n"
	"probe_seg_stos_end: mov $0x2b, %eax\n mov %eax, %es\n pop %edi\n ret\n"
	/* Stores through a read-only data segment and through CS. */
	"probe_seg_ro_write: mov $0x1f, %eax\n mov %eax, %es\n"
	"probe_seg_ro_write_insn: movb $1, %es:0\n"
	"probe_seg_ro_write_end: mov $0x2b, %eax\n mov %eax, %es\n ret\n"
	"probe_seg_cs_write: mov 4(%esp), %ecx\n"
	"probe_seg_cs_write_insn: movb $1, %cs:(%ecx)\n"
	"probe_seg_cs_write_end: ret\n"
	/* DS with a base: an absolute operand and a based one. */
	"probe_seg_ds_base: mov $0x07, %eax\n mov %eax, %ds\n mov 4, %eax\n mov $8, %ecx\n add (%ecx), %eax\n"
	"	mov $0x2b, %ecx\n mov %ecx, %ds\n ret\n"
	/* A null DS, then an access through it. */
	"probe_seg_null_ds: mov 4(%esp), %ecx\n xor %eax, %eax\n mov %eax, %ds\n"
	"probe_seg_null_ds_insn: mov (%ecx), %eax\n"
	"probe_seg_null_ds_end: mov $0x2b, %ecx\n mov %ecx, %ds\n ret\n"
	/* A push through the LDT stack segment, limit 0x7f, with the ESP the
	 * argument gives. */
	"probe_seg_ss_push: mov %esp, seg_saved_esp\n mov 4(%esp), %edx\n mov $0x2f, %eax\n"
	"	mov %eax, %ss\n mov %edx, %esp\n"
	"probe_seg_ss_push_insn: push %eax\n"
	"probe_seg_ss_push_end: mov $0x2b, %eax\n mov %eax, %ss\n mov seg_saved_esp, %esp\n ret\n"
	/* LES, LDS, LFS, LGS and LSS of the LDT data segment through m16:32;
	 * each returns the selector loaded above the offset's low half. */
	"probe_seg_les: mov 4(%esp), %ecx\n les (%ecx), %eax\n mov %es, %edx\n shl $16, %edx\n mov %ax, %dx\n"
	"	mov $0x2b, %eax\n mov %eax, %es\n mov %edx, %eax\n ret\n"
	"probe_seg_lds: mov 4(%esp), %ecx\n lds (%ecx), %eax\n mov %ds, %edx\n shl $16, %edx\n mov %ax, %dx\n"
	"	mov $0x2b, %eax\n mov %eax, %ds\n mov %edx, %eax\n ret\n"
	"probe_seg_lfs: mov 4(%esp), %ecx\n lfs (%ecx), %eax\n mov %fs, %edx\n shl $16, %edx\n mov %ax, %dx\n"
	"	xor %eax, %eax\n mov %eax, %fs\n mov %edx, %eax\n ret\n"
	"probe_seg_lgs: mov 4(%esp), %ecx\n push %gs\n lgs (%ecx), %eax\n mov %gs, %edx\n shl $16, %edx\n mov %ax, %dx\n"
	"	pop %gs\n mov %edx, %eax\n ret\n"
	"probe_seg_lss: mov 4(%esp), %ecx\n lss (%ecx), %eax\n mov %ss, %edx\n mov $0x2b, %eax\n mov %eax, %ss\n"
	"	shl $16, %edx\n mov (%ecx), %ax\n mov %ax, %dx\n mov %edx, %eax\n ret\n"
	/* To 64-bit code and back: a direct far JMP to CS 0x33, and an
	 * indirect one through m16:32 to CS 0x23. */
	"probe_seg_mode_switch: mov 4(%esp), %ecx\n movl $seg_back32, (%ecx)\n movw $0x23, 4(%ecx)\n"
	"	mov $1, %eax\n ljmp $0x33, $seg_long\n"
	".code64\n"
	"seg_long: add $1, %eax\n movl %ecx, %ecx\n mov %cs, %edx\n shl $16, %edx\n or %edx, %eax\n ljmpl *(%rcx)\n"
	".code32\n"
	"seg_back32: ret\n"
	/* 0x82 is group 1 outside 64-bit mode. */
	"probe_seg_group1_82: mov $0x10, %eax\n .byte 0x82, 0xc0, 0x05\n .byte 0x82, 0xe8, 0x01\n"
	"	.byte 0x82, 0xf8, 0x14\n setz %cl\n movzbl %cl, %ecx\n shl $8, %ecx\n or %ecx, %eax\n ret\n"
	".pushsection .bss\n"
	"seg_saved_esp: .long 0\n"
	".popsection\n");

extern "C" word probe_seg_es_read(word selector, word offset);
extern "C" char probe_seg_es_read_insn[];
extern "C" char probe_seg_es_read_end[];
extern "C" void probe_seg_xsave(word selector, word rfbm);
extern "C" char probe_seg_xsave_insn[];
extern "C" char probe_seg_xsave_end[];
extern "C" void probe_seg_xrstor(word selector, word rfbm);
extern "C" char probe_seg_xrstor_insn[];
extern "C" char probe_seg_xrstor_end[];
extern "C" void probe_seg_fxsave(word selector);
extern "C" char probe_seg_fxsave_insn[];
extern "C" char probe_seg_fxsave_end[];
extern "C" void probe_seg_stos();
extern "C" char probe_seg_stos_insn[];
extern "C" char probe_seg_stos_end[];
extern "C" void probe_seg_ro_write();
extern "C" char probe_seg_ro_write_insn[];
extern "C" char probe_seg_ro_write_end[];
extern "C" void probe_seg_cs_write(void *target);
extern "C" char probe_seg_cs_write_insn[];
extern "C" char probe_seg_cs_write_end[];
extern "C" word probe_seg_ds_base();
extern "C" word probe_seg_null_ds(const void *source);
extern "C" char probe_seg_null_ds_insn[];
extern "C" char probe_seg_null_ds_end[];
extern "C" void probe_seg_ss_push(word stack_pointer);
extern "C" char probe_seg_ss_push_insn[];
extern "C" char probe_seg_ss_push_end[];
extern "C" word probe_seg_les(const void *pointer);
extern "C" word probe_seg_lds(const void *pointer);
extern "C" word probe_seg_lfs(const void *pointer);
extern "C" word probe_seg_lgs(const void *pointer);
extern "C" word probe_seg_lss(const void *pointer);
extern "C" word probe_seg_mode_switch(void *pointer);
extern "C" word probe_seg_group1_82();

word segment_offset;
word segment_selector;
word segment_rfbm;
void seg_es_read() { probe_seg_es_read(segment_selector, segment_offset); }
void seg_xsave() { probe_seg_xsave(segment_selector, segment_rfbm); }
void seg_xrstor() { probe_seg_xrstor(segment_selector, segment_rfbm); }
void seg_fxsave() { probe_seg_fxsave(segment_selector); }
void seg_cs_write() { probe_seg_cs_write(segment_buffer + 0x200); }
void seg_null_ds() { probe_seg_null_ds(segment_buffer); }
word segment_stack_pointer;
void seg_ss_push() { probe_seg_ss_push(segment_stack_pointer); }
#endif

#if !defined(__x86_64__)
constexpr u32 ldt_legacy_area = 0x37;
constexpr u32 ldt_header_area = 0x3f;
constexpr u32 ldt_avx_area = 0x47;

/* FXSAVE and FXRSTOR touch 512 bytes through a segment, XSAVE, XSAVEOPT and
 * XRSTOR the legacy area and the header, 576, and the AVX component, up to
 * 832, only when RFBM selects it, so a segment limit below the extent of
 * the RFBM raises #GP(0) and one at it does not. */
bool segment_state_areas()
{
	/* A valid image: an x87 control word of 0x37f, MXCSR 0x1f80, and no
	 * component marked in use. */
	for (word index = 0; index < 1024; ++index)
		segment_buffer[index] = 0;
	segment_buffer[0] = 0x7f;
	segment_buffer[1] = 0x03;
	segment_buffer[24] = 0x80;
	segment_buffer[25] = 0x1f;
	bool ok = true;
	const word xsave_insn = address_of(probe_seg_xsave_insn);
	const word xsave_end = address_of(probe_seg_xsave_end);
	const word xrstor_insn = address_of(probe_seg_xrstor_insn);
	const word xrstor_end = address_of(probe_seg_xrstor_end);
	const word fxsave_insn = address_of(probe_seg_fxsave_insn);
	const word fxsave_end = address_of(probe_seg_fxsave_end);
	segment_selector = ldt_legacy_area;
	ok &= delivers_nothing("FXSAVE in 512 bytes", seg_fxsave, fxsave_end);
	segment_selector = ldt_data;
	ok &= delivers("FXSAVE beyond 256 bytes", seg_fxsave, fxsave_end,
		       {sigsegv, si_kernel, 0, fxsave_insn, 13, true, 0});
	segment_selector = ldt_legacy_area;
	segment_rfbm = 0;
	ok &= delivers("XSAVE with RFBM 0 in 512 bytes", seg_xsave, xsave_end,
		       {sigsegv, si_kernel, 0, xsave_insn, 13, true, 0});
	segment_selector = ldt_header_area;
	const word without_avx[] = {0, 1, 3};
	const word with_avx[] = {4, 7};
	for (const word rfbm : without_avx) {
		segment_rfbm = rfbm;
		ok &= delivers_nothing("XSAVE without AVX in 576 bytes",
				       seg_xsave, xsave_end);
		ok &= delivers_nothing("XRSTOR without AVX in 576 bytes",
				       seg_xrstor, xrstor_end);
	}
	for (const word rfbm : with_avx) {
		segment_rfbm = rfbm;
		ok &= delivers("XSAVE with AVX in 576 bytes", seg_xsave,
			       xsave_end,
			       {sigsegv, si_kernel, 0, xsave_insn, 13, true, 0});
		ok &= delivers("XRSTOR with AVX in 576 bytes", seg_xrstor,
			       xrstor_end,
			       {sigsegv, si_kernel, 0, xrstor_insn, 13, true, 0});
	}
	segment_selector = ldt_avx_area;
	for (const word rfbm : with_avx) {
		segment_rfbm = rfbm;
		ok &= delivers_nothing("XSAVE with AVX in 832 bytes", seg_xsave,
				       xsave_end);
		ok &= delivers_nothing("XRSTOR with AVX in 832 bytes",
				       seg_xrstor, xrstor_end);
	}
	return ok;
}
#endif

/* Selector loads check the descriptor they name: SS takes no null
 * selector, a selector beyond the GDT or the LDT raises #GP and one whose
 * segment is not present #NP, each with the selector's index as its error
 * code; a far JMP to a data segment and an IRET to one raise #GP. The LDT
 * that modify_ldt writes reads back as Linux fills it, LAR, LSL, VERR and
 * VERW read its descriptors, FS loads its base from one, and a far CALL
 * reaches its 32-bit code segment, which runs 0x82 as group 1. CMPXCHG16B
 * raises #GP(0) for a misaligned operand, locked or not. In 32-bit code,
 * every access checks its segment's limit -- an expand-down one from below --
 * a REP STOSB stops at the first element beyond it with ECX and EDI there,
 * a store through a read-only segment or CS raises #GP(0), DS and SS add
 * their bases, a push beyond SS's limit raises #SS(0), FXSAVE, XSAVE and
 * XRSTOR check the extent their RFBM selects (see segment_state_areas), and
 * far JMPs reach 64-bit code and come back. */
bool segments()
{
	bool ok = install_fault_handlers() && install_ldt();
	if (!ok)
		return false;
	for (word index = 0; index < sizeof(segment_buffer); ++index)
		segment_buffer[index] = static_cast<unsigned char>(index * 7 + 1);
	ok &= ldt_reads_back();
	ok &= expect("LAR of the LDT data segment", probe_seg_lar(ldt_data),
		     0x40f300);
	ok &= expect("LAR of the LDT code segment", probe_seg_lar(ldt_code32),
		     0xcffb00);
	ok &= expect("LAR of the absent segment", probe_seg_lar(ldt_absent),
		     0x407300);
	ok &= expect("LSL of the LDT data segment", probe_seg_lsl(ldt_data),
		     0xff);
	ok &= expect("LSL of the expand-down segment", probe_seg_lsl(ldt_down),
		     0xfff);
	ok &= expect("LSL beyond the LDT", probe_seg_lsl(0x4f), ~0UL);
	ok &= expect("VERR, VERW of the read-only segment",
		     probe_seg_verr(ldt_read_only_data) << 1 |
			     probe_seg_verw(ldt_read_only_data), 2);
	ok &= expect("VERR, VERW of the LDT data segment",
		     probe_seg_verr(ldt_data) << 1 | probe_seg_verw(ldt_data), 3);
	ok &= expect("VERR, VERW of the LDT code segment",
		     probe_seg_verr(ldt_code32) << 1 | probe_seg_verw(ldt_code32),
		     2);
	const word ss_insn = address_of(probe_seg_movss_null_insn);
	ok &= delivers("MOV SS of a null selector", probe_seg_movss_null,
		       address_of(probe_seg_movss_null_end),
		       {sigsegv, si_kernel, 0, ss_insn, 13, true, 0});
	const word ds_insn = address_of(probe_seg_movds_far_insn);
	ok &= delivers("MOV DS beyond the GDT", probe_seg_movds_far,
		       address_of(probe_seg_movds_far_end),
		       {sigsegv, si_kernel, 0, ds_insn, 13, true, 0x4140});
	const word absent_insn = address_of(probe_seg_absent_insn);
	ok &= delivers("MOV ES of an absent segment", probe_seg_absent,
		       address_of(probe_seg_absent_end),
		       {sigbus, si_kernel, 0, absent_insn, 11, true, 0x14});
	const word jmp_insn = address_of(probe_seg_farjmp_data_insn);
	ok &= delivers("far JMP to a data segment", seg_farjmp_data,
		       address_of(probe_seg_farjmp_data_end),
		       {sigsegv, si_kernel, 0, jmp_insn, 13, true, 0x28});
	const word lss_insn = address_of(probe_seg_lss_null_insn);
	ok &= delivers("LSS of a null selector", seg_lss_null,
		       address_of(probe_seg_lss_null_end),
		       {sigsegv, si_kernel, 0, lss_insn, 13, true, 0});
	const word iret_insn = address_of(probe_seg_iret_bad_insn);
	ok &= delivers("IRET to a data segment", probe_seg_iret_bad,
		       address_of(probe_seg_iret_bad_end),
		       {sigsegv, si_kernel, 0, iret_insn, 13, true, 0x28});
	ok &= expect("far CALL to LDT code, 0x82 there",
		     probe_seg_ldt_call(segment_pointer), 0x000f0015);
	u32 fs_word;
	__builtin_memcpy(&fs_word, segment_buffer + 4, sizeof(fs_word));
	ok &= expect("FS:4 through the LDT data segment",
		     probe_seg_fs_read(ldt_data) & 0xffffffffUL, fs_word);
#if defined(__x86_64__)
	const word cx16 = address_of(probe_seg_cx16_insn);
	ok &= delivers("misaligned LOCK CMPXCHG16B", seg_cx16,
		       address_of(probe_seg_cx16_end),
		       {sigsegv, si_kernel, 0, cx16, 13, true, 0});
	const word cx16_plain = address_of(probe_seg_cx16_plain_insn);
	ok &= delivers("misaligned CMPXCHG16B", seg_cx16_plain,
		       address_of(probe_seg_cx16_plain_end),
		       {sigsegv, si_kernel, 0, cx16_plain, 13, true, 0});
#else
	u32 word_at;
	const word es_insn = address_of(probe_seg_es_read_insn);
	const word es_end = address_of(probe_seg_es_read_end);
	__builtin_memcpy(&word_at, segment_buffer + 0xfc, sizeof(word_at));
	ok &= expect("ES:0xfc, the last dword below the limit",
		     probe_seg_es_read(ldt_data, 0xfc), word_at);
	const word beyond[] = {0xfd, 0x100, 0xffffffff};
	for (const word offset : beyond) {
		segment_selector = ldt_data;
		segment_offset = offset;
		ok &= delivers("a dword read across ES's limit", seg_es_read,
			       es_end, {sigsegv, si_kernel, 0, es_insn, 13,
					true, 0});
	}
	__builtin_memcpy(&word_at, segment_buffer, sizeof(word_at));
	ok &= expect("ES:0x1000 through the expand-down segment",
		     probe_seg_es_read(ldt_down, 0x1000), word_at);
	segment_selector = ldt_down;
	segment_offset = 0xfff;
	ok &= delivers("a read below the expand-down segment", seg_es_read,
		       es_end, {sigsegv, si_kernel, 0, es_insn, 13, true, 0});
	const word null_insn = address_of(probe_seg_null_ds_insn);
	ok &= delivers("an access through a null DS", seg_null_ds,
		       address_of(probe_seg_null_ds_end),
		       {sigsegv, si_kernel, 0, null_insn, 13, true, 0});
	for (word index = 0xe0; index < 0x110; ++index)
		segment_buffer[index] = 0;
	const word stos_insn = address_of(probe_seg_stos_insn);
	ok &= delivers("REP STOSB across ES's limit", probe_seg_stos,
		       address_of(probe_seg_stos_end),
		       {sigsegv, si_kernel, 0, stos_insn, 13, true, 0});
	ok &= expect("ECX at the fault", observed[observed_cx], 0x10);
	ok &= expect("EDI at the fault", observed[observed_di], 0x100);
	ok &= expect("the last byte stored", segment_buffer[0xff], 0x5a);
	ok &= expect("the first byte beyond", segment_buffer[0x100], 0);
	const word ro_insn = address_of(probe_seg_ro_write_insn);
	ok &= delivers("a store through a read-only segment",
		       probe_seg_ro_write, address_of(probe_seg_ro_write_end),
		       {sigsegv, si_kernel, 0, ro_insn, 13, true, 0});
	const word cs_insn = address_of(probe_seg_cs_write_insn);
	ok &= delivers("a store through CS", seg_cs_write,
		       address_of(probe_seg_cs_write_end),
		       {sigsegv, si_kernel, 0, cs_insn, 13, true, 0});
	u32 first;
	u32 second;
	__builtin_memcpy(&first, segment_buffer + 4, sizeof(first));
	__builtin_memcpy(&second, segment_buffer + 8, sizeof(second));
	ok &= expect("DS adds its base", probe_seg_ds_base(), first + second);
	u32 pointer_image[2] = {0x12345678, ldt_data};
	ok &= expect("LES", probe_seg_les(pointer_image), 0x00075678);
	ok &= expect("LDS", probe_seg_lds(pointer_image), 0x00075678);
	ok &= expect("LFS", probe_seg_lfs(pointer_image), 0x00075678);
	ok &= expect("LGS", probe_seg_lgs(pointer_image), 0x00075678);
	ok &= expect("LSS", probe_seg_lss(pointer_image), 0x00075678);
	ok &= expect("far JMPs to 64-bit code and back",
		     probe_seg_mode_switch(segment_pointer), 0x00330002);
	ok &= expect("0x82 as group 1", probe_seg_group1_82(), 0x114);
	if (!handle_on_alternate_stack(sigbus, record_fault)) {
		put("  sigaltstack or rt_sigaction failed\n");
		return false;
	}
	segment_stack_pointer = 0x80;
	ok &= delivers_nothing("a push at SS:0x7c", seg_ss_push,
			       address_of(probe_seg_ss_push_end));
	ok &= expect("the pushed dword", segment_stack[0x7c], 0x2f);
	segment_stack_pointer = 0x84;
	const word push_insn = address_of(probe_seg_ss_push_insn);
	ok &= delivers("a push beyond SS's limit", seg_ss_push,
		       address_of(probe_seg_ss_push_end),
		       {sigbus, si_kernel, 0, push_insn, 12, true, 0});
	ok &= segment_state_areas();
#endif
	return ok;
}

#if defined(__x86_64__)
/* Runs `path mode` and reports whether it exited 0. */
bool in_child(const char *path, const char *mode)
{
	const char *arguments[] = {path, mode, nullptr};
	const char *environment[] = {nullptr};
	const long child = raw_syscall(nr_fork);
	if (child == 0) {
		raw_syscall(nr_execve, reinterpret_cast<long>(path),
			    reinterpret_cast<long>(arguments),
			    reinterpret_cast<long>(environment));
		raw_syscall(nr_exit_group, 127);
	}
	int status = 0;
	if (child < 0 || wait_for(child, status) != child)
		return false;
	if ((status & 0x7f) != 0) {
		put("  killed by signal ");
		put_decimal(static_cast<u32>(status & 0x7f));
		put("\n");
		return false;
	}
	return ((status >> 8) & 0xff) == 0;
}

int failures;

void report(const char *name, bool ok)
{
	put(ok ? "PASS " : "FAIL ");
	put(name);
	put("\n");
	failures += ok ? 0 : 1;
}

int run_all(const char *self, const char *i386_probe)
{
	report("x86-64 a 1 ms timer fires during long REP STOSB",
	       in_child(self, "timer-stos"));
	report("x86-64 a 1 ms timer fires during long REPE CMPSB",
	       in_child(self, "timer-cmps"));
	report("i386 a 1 ms timer fires during long REP STOSB",
	       in_child(i386_probe, "timer-stos"));
	report("i386 a 1 ms timer fires during long REPE CMPSB",
	       in_child(i386_probe, "timer-cmps"));
	report("x86-64 ICEBP delivers SIGTRAP TRAP_BRKPT after itself",
	       in_child(self, "icebp"));
	report("i386 ICEBP delivers SIGTRAP TRAP_BRKPT after itself",
	       in_child(i386_probe, "icebp"));
	report("i386 BOUND passes in range and raises #BR outside it",
	       in_child(i386_probe, "bound"));
	report("x86-64 INT 4 delivers SIGSEGV SI_KERNEL",
	       in_child(self, "int4"));
	report("i386 INT 4 delivers SIGSEGV SI_KERNEL",
	       in_child(i386_probe, "int4"));
	report("i386 INTO delivers SIGSEGV SI_KERNEL on overflow",
	       in_child(i386_probe, "into"));
	report("x86-64 a signal keeps the x87 stack",
	       in_child(self, "x87-signal"));
	report("i386 a signal keeps the x87 stack",
	       in_child(i386_probe, "x87-signal"));
	report("x86-64 a signal between instructions keeps RAX",
	       in_child(self, "signal-ax"));
	report("i386 a signal between instructions keeps EAX",
	       in_child(i386_probe, "signal-ax"));
	report("x86-64 tracer reads and writes registers, FP state",
	       in_child(self, "tracer"));
	report("i386 tracer reads and writes registers, FP state and TLS",
	       in_child(i386_probe, "tracer"));
	report("x86-64 tracer changes and skips system calls by orig_ax",
	       in_child(self, "tracer-syscall"));
	report("i386 tracer changes and skips system calls by orig_eax",
	       in_child(i386_probe, "tracer-syscall"));
	report("x86-64 UD2 delivers SIGILL ILL_ILLOPN for #UD",
	       in_child(self, "ud2"));
	report("i386 UD2 delivers SIGILL ILL_ILLOPN for #UD",
	       in_child(i386_probe, "ud2"));
	report("x86-64 undefined encodings raise #UD, privileged ones #GP",
	       in_child(self, "undefined"));
	report("i386 undefined encodings raise #UD, privileged ones #GP",
	       in_child(i386_probe, "undefined"));
	report("x86-64 #PF error codes carry the user bit",
	       in_child(self, "page-fault"));
	report("i386 #PF error codes carry the user bit",
	       in_child(i386_probe, "page-fault"));
	report("x86-64 a page fault sets P for a protection violation",
	       in_child(self, "page-fault-protection"));
	report("i386 a page fault sets P for a protection violation",
	       in_child(i386_probe, "page-fault-protection"));
	report("x86-64 tracer reads and writes the debug registers",
	       in_child(self, "tracer-debug"));
	report("i386 tracer reads and writes the debug registers",
	       in_child(i386_probe, "tracer-debug"));
	report("i386 TLS takes GDT entries 12 to 14",
	       in_child(i386_probe, "tls"));
	report("x86-64 INT n raises #GP naming its IDT entry",
	       in_child(self, "int-n"));
	report("i386 INT n raises #GP naming its IDT entry",
	       in_child(i386_probe, "int-n"));
	report("x86-64 a non-canonical address raises #GP or #SS",
	       in_child(self, "noncanonical"));
	report("x86-64 DIV and IDIV raise #DE and write nothing",
	       in_child(self, "divide-error"));
	report("x86-64 masked moves access the lanes x86 does",
	       in_child(self, "masked-moves"));
	report("x86-64 AVX2 gathers complete the elements before a fault",
	       in_child(self, "gathers"));
	report("x86-64 a faulting access leaves the state as the instruction found it",
	       in_child(self, "fault-state"));
	report("i386 a faulting access leaves the state as the instruction found it",
	       in_child(i386_probe, "fault-state"));
	report("i386 effective addresses wrap at 4 GiB",
	       in_child(i386_probe, "address-wrap"));
	report("x86-64 bad stack pointers call and fault on one CPU",
	       in_child(self, "stack-pointers"));
	report("i386 ESP 0 calls and faults on one CPU",
	       in_child(i386_probe, "stack-pointers"));
	report("x86-64 a single step runs one iteration of a REP string",
	       in_child(self, "tracer-step"));
	report("i386 a single step runs one iteration of a REP string",
	       in_child(i386_probe, "tracer-step"));
	report("x86-64 unmasked SSE and x87 exceptions raise #XM and #MF",
	       in_child(self, "fp-exceptions"));
	report("i386 unmasked SSE and x87 exceptions raise #XM and #MF",
	       in_child(i386_probe, "fp-exceptions"));
	report("x86-64 RFLAGS.AC checks alignment",
	       in_child(self, "alignment-check"));
	report("i386 RFLAGS.AC checks alignment",
	       in_child(i386_probe, "alignment-check"));
	report("x86-64 a misaligned extended-state image raises #GP",
	       in_child(self, "alignment-faults"));
	report("i386 a misaligned extended-state image raises #GP",
	       in_child(i386_probe, "alignment-faults"));
	report("x86-64 CLFLUSH, CLFLUSHOPT and CLWB fault on an unmapped line",
	       in_child(self, "cache-line"));
	report("i386 CLFLUSH, CLFLUSHOPT and CLWB fault on an unmapped line",
	       in_child(i386_probe, "cache-line"));
	report("x86-64 XSAVE and XRSTOR touch only the components RFBM names",
	       in_child(self, "state-area"));
	report("i386 XSAVE and XRSTOR touch only the components RFBM names",
	       in_child(i386_probe, "state-area"));
	report("x86-64 masked x87 exceptions set sticky status flags",
	       in_child(self, "x87-status"));
	report("i386 masked x87 exceptions set sticky status flags",
	       in_child(i386_probe, "x87-status"));
	report("x86-64 selector loads, the LDT and far transfers",
	       in_child(self, "segments"));
	report("i386 segment limits, bases, the LDT and far transfers",
	       in_child(i386_probe, "segments"));
	return failures;
}
#endif

} // namespace

extern "C" int probe_main(word *stack)
{
	const long count = static_cast<long>(stack[0]);
	char **arguments = reinterpret_cast<char **>(stack + 1);
	if (count == 2) {
		const char *mode = arguments[1];
		if (equal(mode, "idle"))
			return 0;
		if (equal(mode, "timer-stos"))
			return timer_ticks_during("REP STOSB", repeat_stos) ? 0 : 1;
		if (equal(mode, "timer-cmps"))
			return timer_ticks_during("REPE CMPSB", repeat_cmps) ? 0 : 1;
		if (equal(mode, "icebp"))
			return icebp_traps() ? 0 : 1;
		if (equal(mode, "int4"))
			return int4_traps() ? 0 : 1;
		if (equal(mode, "x87-signal"))
			return x87_stack_survives_a_signal() ? 0 : 1;
		if (equal(mode, "signal-ax"))
			return signal_keeps_ax() ? 0 : 1;
		if (equal(mode, "tracer"))
			return tracer_sees_registers() ? 0 : 1;
		if (equal(mode, "tracer-syscall"))
			return tracer_changes_syscalls() ? 0 : 1;
		if (equal(mode, "ud2"))
			return ud2_faults() ? 0 : 1;
		if (equal(mode, "undefined"))
			return undefined_faults() ? 0 : 1;
		if (equal(mode, "page-fault"))
			return page_faults_are_user() ? 0 : 1;
		if (equal(mode, "page-fault-protection"))
			return page_fault_protection() ? 0 : 1;
		if (equal(mode, "tracer-debug"))
			return tracer_debug_registers() ? 0 : 1;
		if (equal(mode, "int-n"))
			return int_n_faults() ? 0 : 1;
		if (equal(mode, "tracer-step"))
			return tracer_steps_repeats() ? 0 : 1;
		if (equal(mode, "fp-exceptions"))
			return floating_point_exceptions() ? 0 : 1;
		if (equal(mode, "alignment-faults"))
			return alignment_faults() ? 0 : 1;
		if (equal(mode, "cache-line"))
			return cache_line_flushes() ? 0 : 1;
		if (equal(mode, "state-area"))
			return state_area_extents() ? 0 : 1;
		if (equal(mode, "alignment-check"))
			return alignment_checks() ? 0 : 1;
		if (equal(mode, "x87-status"))
			return x87_status_flags() ? 0 : 1;
		if (equal(mode, "stack-pointers"))
			return bad_stack_pointers() ? 0 : 1;
		if (equal(mode, "segments"))
			return segments() ? 0 : 1;
#if defined(__i386__)
		if (equal(mode, "into"))
			return into_traps() ? 0 : 1;
		if (equal(mode, "bound"))
			return bound_checks() ? 0 : 1;
		if (equal(mode, "tls"))
			return tls_entries() ? 0 : 1;
		if (equal(mode, "fault-state"))
			return fault_state() ? 0 : 1;
		if (equal(mode, "address-wrap"))
			return addresses_wrap() ? 0 : 1;
#else
		if (equal(mode, "noncanonical"))
			return noncanonical_faults() ? 0 : 1;
		if (equal(mode, "divide-error"))
			return divide_errors() ? 0 : 1;
		if (equal(mode, "masked-moves"))
			return masked_moves() ? 0 : 1;
		if (equal(mode, "gathers"))
			return gathers() ? 0 : 1;
		if (equal(mode, "fault-state"))
			return fault_state() ? 0 : 1;
		return run_all(arguments[0], arguments[1]);
#endif
	}
#if defined(__x86_64__)
	put("usage: exception-probe I386_PROBE\n");
#endif
	return 100;
}

using size_type = decltype(sizeof(0));

extern "C" void *memset(void *destination, int value, size_type length)
{
	auto *bytes = static_cast<volatile unsigned char *>(destination);
	for (size_type index = 0; index < length; ++index)
		bytes[index] = static_cast<unsigned char>(value);
	return destination;
}

extern "C" void *memcpy(void *destination, const void *source,
			size_type length)
{
	auto *to = static_cast<volatile unsigned char *>(destination);
	const auto *from = static_cast<const unsigned char *>(source);
	for (size_type index = 0; index < length; ++index)
		to[index] = from[index];
	return destination;
}
