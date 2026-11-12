// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * A freestanding armhf program for the guest kernel's AArch32 support.
 *
 * It is one position-independent .text section with no relocations and no
 * writable data (its state lives on the stack), so make_elf32.py can wrap
 * it into an ELF32 executable without a linker.  Run with no argument it
 * checks, and prints one PASS or FAIL line for each check followed by a
 * summary, and exits with the failure count:
 *
 *   write; the auxiliary vector; set_tls and TPIDRURO, TPIDRURW and the
 *   kuser helpers; signals through the sigpage and through SA_RESTORER
 *   with the core, VFP and flag state restored; cacheflush of code
 *   written in ARM and Thumb state; an IT block around SVC; uname and
 *   /proc/cpuinfo under PER_LINUX32; the 4 KiB unit of mmap2 offsets and
 *   the 64-bit-argument system calls; a nanosleep restarted after
 *   SIGSTOP and SIGCONT; and a 32 -> 64 -> 32 execve chain.
 *
 * The other modes, each by its first argument:
 *
 *   extra    integer-only checks of the kernel side: threads through clone
 *            and clone3 with CLONE_SETTLS; a SIGSEGV handler that steps
 *            over the access; stack growth, brk and mmap2 below the
 *            vectors; pipe2, ppoll_time64, clock_gettime64, getrandom,
 *            socketpair with SCM_RIGHTS, statx and getdents64; vfork and
 *            execve; a timer signal into running code; rseq, the robust
 *            list and sigaltstack; and seccomp filters on the ARM arch and
 *            EABI numbers.
 *   tracer   this program as the tracer of a 32-bit tracee, through the
 *            ARM-private ptrace requests GETREGS, SETREGS, PEEKUSR,
 *            GETVFPREGS, GET_THREAD_AREA and SET_SYSCALL.
 *   vfp      sets a handler for SIGUSR1 which records and rewrites the VFP
 *            context of its signal frame, signals itself, stops with
 *            SIGSTOP, and writes what the handler saw to its standard
 *            output, for a tracer to compare.
 *   segv, undef, bkpt  end in the signal the instruction raises.
 *   leaf     the end of the 32 -> 64 -> 32 chain, which exits 37.
 *   trace    sets r4 and r5 to known values, stops itself with SIGSTOP for
 *            a tracer to read and rewrite them, and exits with the r5 it
 *            then finds.
 *   exclusives  every exclusive and ordered transfer, each after a
 *            sched_yield, for a run beside a 64-bit task that leaves its
 *            registers set on the same CPU (aarch64_dirty.s); exits with
 *            the number of words that came out wrong.
 *   swp      SWP and SWPB on a stack word, a page nothing touched, a page
 *            only read, a file mapping, with every condition, and the faults
 *            of an unaligned or unmapped address and a read-only page; run
 *            with abi.swp 1.
 *   swp-one, swp-pc  one SWP, and one with the pc as Rt: with abi.swp 0 the
 *            first ends in SIGILL, with 1 it exits 6 and the second ends in
 *            SIGILL.
 *   swp-tokens  swaps a word and a byte of the shared page that descriptor 9
 *            maps with SWP and SWPB, beside the 64-bit task of aarch64_dirty.s
 *            doing the same with load/store-exclusive pairs, until both have
 *            met each other's token (the "tokens" mode there); the page
 *            holds the result.
 */

	.syntax	unified
	.arch	armv7-a
	.fpu	neon-vfpv4
	.arm
	.text

/* ARM EABI system calls. */
	.equ	SYS_restart_syscall, 0
	.equ	SYS_exit,	1
	.equ	SYS_fork,	2
	.equ	SYS_read,	3
	.equ	SYS_write,	4
	.equ	SYS_open,	5
	.equ	SYS_close,	6
	.equ	SYS_unlink,	10
	.equ	SYS_execve,	11
	.equ	SYS_getpid,	20
	.equ	SYS_kill,	37
	.equ	SYS_readlink,	85
	.equ	SYS_munmap,	91
	.equ	SYS_wait4,	114
	.equ	SYS_uname,	122
	.equ	SYS_personality, 136
	.equ	SYS_sched_yield, 158
	.equ	SYS_nanosleep,	162
	.equ	SYS_rt_sigreturn, 173
	.equ	SYS_rt_sigaction, 174
	.equ	SYS_rt_sigprocmask, 175
	.equ	SYS_pread64,	180
	.equ	SYS_mmap2,	192
	.equ	SYS_ftruncate64, 194
	.equ	SYS_fstat64,	197
	.equ	SYS_exit_group,	248
	.equ	SYS_clock_gettime, 263
	.equ	SYS_cacheflush,	0xf0002
	.equ	SYS_set_tls,	0xf0005

	.equ	SIGUSR1,	10
	.equ	SIGCONT,	18
	.equ	SIGSTOP,	19
	.equ	SA_SIGINFO,	0x00000004
	.equ	SA_RESTORER,	0x04000000
	.equ	AT_PAGESZ,	6
	.equ	AT_PLATFORM,	15
	.equ	AT_HWCAP,	16
	.equ	PER_LINUX32,	8

/* The frame, from r11. */
	.equ	F_PREFIX,	0x000	/* check names carry this prefix */
	.equ	F_PAGESZ,	0x004
	.equ	F_HWCAP,	0x008
	.equ	F_PLATFORM,	0x00c
	.equ	F_PID,		0x010
	.equ	F_H_COUNT,	0x018	/* what the signal handler saw */
	.equ	F_H_SIGNO,	0x01c
	.equ	F_H_PID,	0x020
	.equ	F_H_CODE,	0x024
	.equ	F_H_LR,		0x028
	.equ	F_H_MAGIC,	0x02c
	.equ	F_H_CPSR,	0x030
	.equ	F_H_MASK,	0x038
	.equ	F_SA,		0x040	/* struct sigaction, 20 bytes */
	.equ	F_WORK,		0x060	/* 64 bytes of scratch */
	.equ	F_ARGVX,	0x0a0	/* an argument vector */
	.equ	F_UNAME,	0x100	/* struct utsname */
	.equ	F_PATH,		0x300
	.equ	F_CPU,		0x400	/* /proc/cpuinfo, 0x1100 bytes */
	.equ	F_FILE,		0x1500	/* one 4 KiB chunk of the mmap2 file */
	.equ	F_STAT,		0x2500	/* struct stat64 */
	.equ	FRAME_SIZE,	0x2600

	.equ	UC_ARM_SP,	84	/* ucontext offsets */
	.equ	UC_ARM_CPSR,	96
	.equ	UC_REGSPACE,	232
	.equ	VFP_MAGIC,	0x56465001

/* A 32-bit constant in any register. */
.macro movi reg, imm
	movw	\reg, #((\imm) & 0xffff)
	movt	\reg, #(((\imm) >> 16) & 0xffff)
.endm

/* A system call; the arguments are in place. */
.macro sc nr
	movi	r7, \nr
	svc	#0
.endm

/* The address of a symbol, from anywhere: no relocation, no flags. */
.macro lea reg, sym
	ldr	\reg, .Llit\@
.Lpc\@:
	add	\reg, pc, \reg
	b	.Lend\@
.Llit\@:
	.word	\sym - (.Lpc\@ + 8)
.Lend\@:
.endm

/* Branch to \label when the first argument is the string \name. */
.macro mode name, label
	ldr	r0, [r10, #8]
	lea	r1, \name
	bl	streq
	cmp	r0, #0
	bne	\label
.endm

.macro str_ name, text
\name:	.asciz	"\text"
.endm

	.globl	_start
_start:
	mov	r10, sp			/* the kernel's stack: argc, argv, ... */
	sub	sp, sp, #FRAME_SIZE
	bic	sp, sp, #7
	mov	r11, sp
	mov	r8, #0			/* failures */
	mov	r9, #0			/* checks */
	lea	r0, s_empty
	str	r0, [r11, #F_PREFIX]
	bl	parse_auxv

	ldr	r0, [r10]
	cmp	r0, #2
	blt	run_all
	mode	s_leaf, mode_leaf
	mode	s_trace, mode_trace
	mode	s_segv, mode_segv
	mode	s_undef, mode_undef
	mode	s_bkpt, mode_bkpt
	mode	s_extra, mode_extra
	mode	s_tracer, mode_tracer
	mode	s_vfp, mode_vfp
	mode	s_exclusives, mode_exclusives
	mode	s_swp, mode_swp
	mode	s_swp_one, mode_swp_one
	mode	s_swp_pc, mode_swp_pc
	mode	s_swp_tokens, mode_swp_tokens

run_all:
	lea	r0, s_start
	bl	puts
	bl	t_basic
	bl	t_tls
	bl	t_kuser
	mov	r0, #0
	lea	r1, s_pre_sigpage
	bl	t_signal
	movi	r0, SA_RESTORER
	lea	r1, s_pre_restorer
	bl	t_signal
	bl	t_cacheflush
	bl	t_thumb
	bl	t_mmap
	bl	t_personality
	bl	t_sleep
	bl	t_exec
summary:
	lea	r0, s_empty
	str	r0, [r11, #F_PREFIX]

	lea	r0, s_summary_a
	bl	puts
	mov	r0, r8
	bl	putdec
	lea	r0, s_summary_b
	bl	puts
	mov	r0, r9
	bl	putdec
	lea	r0, s_summary_c
	bl	puts
	mov	r0, r8
	cmp	r0, #100
	movgt	r0, #100
	sc	SYS_exit_group

mode_leaf:
	lea	r0, s_leaf_line
	bl	puts
	mov	r0, #37
	sc	SYS_exit_group

mode_segv:
	mov	r1, #0x10
	ldr	r0, [r1]
	mov	r0, #5
	sc	SYS_exit_group

mode_undef:
	.word	0xe7f000f0
	mov	r0, #6
	sc	SYS_exit_group

mode_bkpt:
	.word	0xe1200070
	mov	r0, #7
	sc	SYS_exit_group

mode_trace:
	movi	r4, 0x44444444
	mov	r5, #7
	sc	SYS_getpid
	mov	r1, #SIGSTOP
	sc	SYS_kill
	mov	r0, r5
	sc	SYS_exit_group

/* ---- helpers ---- */

/* r0: a string -> its length. */
strlen:
	mov	r1, r0
1:	ldrb	r2, [r1], #1
	cmp	r2, #0
	bne	1b
	sub	r0, r1, r0
	sub	r0, r0, #1
	bx	lr

/* r0, r1: strings -> r0 = 1 if equal. */
streq:
1:	ldrb	r2, [r0], #1
	ldrb	r3, [r1], #1
	cmp	r2, r3
	bne	2f
	cmp	r2, #0
	bne	1b
	mov	r0, #1
	bx	lr
2:	mov	r0, #0
	bx	lr

/* r0: haystack, r1: its length, r2: a needle string -> the match or 0. */
find:
	push	{r4, r5, r6, lr}
	add	r1, r0, r1
1:	cmp	r0, r1
	bhs	3f
	mov	r4, r0
	mov	r5, r2
2:	ldrb	r6, [r5], #1
	cmp	r6, #0
	beq	4f
	cmp	r4, r1
	bhs	3f
	ldrb	r3, [r4], #1
	cmp	r3, r6
	beq	2b
	add	r0, r0, #1
	b	1b
3:	mov	r0, #0
4:	pop	{r4, r5, r6, pc}

/* r0: a string -> standard output. */
puts:
	push	{r4, r7, lr}
	mov	r4, r0
	bl	strlen
	mov	r2, r0
	mov	r1, r4
	mov	r0, #1
	sc	SYS_write
	pop	{r4, r7, pc}

/* r0: a value -> "0x%08x". */
puthex:
	push	{r4, r5, r7, lr}
	sub	sp, sp, #16
	mov	r1, #'0'
	strb	r1, [sp]
	mov	r1, #'x'
	strb	r1, [sp, #1]
	mov	r2, #8
	add	r3, sp, #2
1:	lsr	r1, r0, #28
	lsl	r0, r0, #4
	cmp	r1, #10
	addlo	r1, r1, #'0'
	addhs	r1, r1, #('a' - 10)
	strb	r1, [r3], #1
	subs	r2, r2, #1
	bne	1b
	mov	r1, sp
	mov	r0, #1
	mov	r2, #10
	sc	SYS_write
	add	sp, sp, #16
	pop	{r4, r5, r7, pc}

/* r0: a value -> decimal. */
putdec:
	push	{r4, r5, r7, lr}
	sub	sp, sp, #16
	add	r4, sp, #12
	mov	r5, #0
	movi	r3, 0xcccccccd
1:	umull	r2, r1, r0, r3
	lsr	r1, r1, #3
	add	r2, r1, r1, lsl #2
	sub	r2, r0, r2, lsl #1
	add	r2, r2, #'0'
	strb	r2, [r4, #-1]!
	add	r5, r5, #1
	movs	r0, r1
	bne	1b
	mov	r1, r4
	mov	r2, r5
	mov	r0, #1
	sc	SYS_write
	add	sp, sp, #16
	pop	{r4, r5, r7, pc}

/* "PASS|FAIL <prefix><name>" -- r0: passed, r1: name.  Counts the check. */
report:
	push	{r4, lr}
	mov	r4, r1
	add	r9, r9, #1
	cmp	r0, #0
	bne	1f
	add	r8, r8, #1
	lea	r0, s_fail
	b	2f
1:	lea	r0, s_pass
2:	bl	puts
	ldr	r0, [r11, #F_PREFIX]
	bl	puts
	mov	r0, r4
	bl	puts
	lea	r0, s_nl
	bl	puts
	pop	{r4, pc}

/* r0: the value, r1: the wanted value, r2: name. */
check_eq:
	push	{r4, r5, r6, lr}
	mov	r4, r0
	mov	r5, r1
	mov	r6, r2
	cmp	r4, r5
	bne	1f
	mov	r0, #1
	mov	r1, r6
	bl	report
	pop	{r4, r5, r6, pc}
1:	add	r9, r9, #1
	add	r8, r8, #1
	lea	r0, s_fail
	bl	puts
	ldr	r0, [r11, #F_PREFIX]
	bl	puts
	mov	r0, r6
	bl	puts
	lea	r0, s_got
	bl	puts
	mov	r0, r4
	bl	puthex
	lea	r0, s_want
	bl	puts
	mov	r0, r5
	bl	puthex
	lea	r0, s_nl
	bl	puts
	pop	{r4, r5, r6, pc}

/* Check the value in r0 against \want. */
.macro eq want, name
	movi	r1, \want
	lea	r2, \name
	bl	check_eq
.endm

/* Check that a double register holds hi:lo. */
.macro chk_d dreg, lo, hi, name
	vmov	r0, r1, \dreg
	movi	r2, \lo
	movi	r3, \hi
	eor	r0, r0, r2
	eor	r1, r1, r3
	orrs	r0, r0, r1
	moveq	r0, #1
	movne	r0, #0
	lea	r1, \name
	bl	report
.endm

/* Keep AT_PAGESZ, AT_HWCAP and AT_PLATFORM in the frame. */
parse_auxv:
	ldr	r1, [r10]
	add	r0, r10, #4
	add	r0, r0, r1, lsl #2
	add	r0, r0, #4
1:	ldr	r1, [r0], #4
	cmp	r1, #0
	bne	1b
2:	ldr	r1, [r0], #4
	ldr	r2, [r0], #4
	cmp	r1, #0
	bxeq	lr
	cmp	r1, #AT_PAGESZ
	streq	r2, [r11, #F_PAGESZ]
	cmp	r1, #AT_HWCAP
	streq	r2, [r11, #F_HWCAP]
	cmp	r1, #AT_PLATFORM
	streq	r2, [r11, #F_PLATFORM]
	b	2b

/* ---- write, the auxiliary vector ---- */

t_basic:
	push	{r4, lr}
	lea	r0, s_hello
	bl	strlen
	mov	r4, r0
	mov	r2, r0
	mov	r0, #1
	lea	r1, s_hello
	sc	SYS_write
	mov	r1, r4
	lea	r2, n_write
	bl	check_eq

	ldr	r0, [r11, #F_PAGESZ]
	cmp	r0, #0x1000
	moveq	r0, #1
	movne	r0, #0
	lea	r1, n_pagesz
	bl	report

	ldr	r0, [r11, #F_HWCAP]
	cmp	r0, #0
	movne	r0, #1
	lea	r1, n_hwcap
	bl	report

	ldr	r0, [r11, #F_PLATFORM]
	cmp	r0, #0
	beq	1f
	lea	r1, s_v8l
	bl	streq
1:	lea	r1, n_platform
	bl	report
	pop	{r4, pc}

/* ---- set_tls, TPIDRURO, TPIDRURW ---- */

t_tls:
	push	{r4, lr}
	movi	r0, 0x12345678
	sc	SYS_set_tls
	mrc	p15, #0, r0, c13, c0, #3
	eq	0x12345678, n_tpidruro

	movi	r0, 0xa5a5a5a0
	mcr	p15, #0, r0, c13, c0, #2
	sc	SYS_sched_yield
	mrc	p15, #0, r0, c13, c0, #2
	eq	0xa5a5a5a0, n_tpidrurw
	mrc	p15, #0, r0, c13, c0, #3
	eq	0x12345678, n_tpidruro_kept
	pop	{r4, pc}

/* ---- the kuser helpers at 0xffff0000 ---- */

t_kuser:
	push	{r4, lr}
	movi	r4, 0xffff0fe0		/* __kuser_get_tls */
	blx	r4
	eq	0x12345678, n_get_tls

	movi	r4, 0xffff0ffc		/* __kuser_helper_version */
	ldr	r0, [r4]
	cmp	r0, #5
	movhs	r0, #1
	movlo	r0, #0
	lea	r1, n_kuser_version
	bl	report

	movi	r4, 0xffff0fa0		/* __kuser_memory_barrier */
	blx	r4

	mov	r0, #5			/* __kuser_cmpxchg */
	str	r0, [r11, #F_WORK]
	mov	r0, #5
	mov	r1, #9
	add	r2, r11, #F_WORK
	movi	r4, 0xffff0fc0
	blx	r4
	eq	0, n_cmpxchg_ok
	ldr	r0, [r11, #F_WORK]
	eq	9, n_cmpxchg_stored
	mov	r0, #5			/* the word is 9 now */
	mov	r1, #7
	add	r2, r11, #F_WORK
	movi	r4, 0xffff0fc0
	blx	r4
	cmp	r0, #0
	movne	r0, #1
	lea	r1, n_cmpxchg_fail
	bl	report
	ldr	r0, [r11, #F_WORK]
	eq	9, n_cmpxchg_kept

	/* __kuser_cmpxchg64: old, new and the target at F_WORK + 8..24 */
	mov	r0, #0
	str	r0, [r11, #(F_WORK + 12)]
	mov	r0, #21
	str	r0, [r11, #(F_WORK + 8)]	@ target = 21
	str	r0, [r11, #(F_WORK + 16)]	@ old = 21
	mov	r0, #0
	str	r0, [r11, #(F_WORK + 20)]
	mov	r0, #43
	str	r0, [r11, #(F_WORK + 24)]	@ new = 43
	mov	r0, #0
	str	r0, [r11, #(F_WORK + 28)]
	add	r0, r11, #(F_WORK + 16)
	add	r1, r11, #(F_WORK + 24)
	add	r2, r11, #(F_WORK + 8)
	movi	r4, 0xffff0f60
	blx	r4
	eq	0, n_cmpxchg64_ok
	ldr	r0, [r11, #(F_WORK + 8)]
	eq	43, n_cmpxchg64_stored
	pop	{r4, pc}

/* ---- signals ---- */

/* The handler runs with the interrupted r4-r11, so r11 is the frame. */
handler:
	str	r0, [r11, #F_H_SIGNO]
	ldr	r3, [r1, #12]		/* si_pid */
	str	r3, [r11, #F_H_PID]
	ldr	r3, [r1, #8]		/* si_code */
	str	r3, [r11, #F_H_CODE]
	str	lr, [r11, #F_H_LR]
	ldr	r3, [r2, #UC_REGSPACE]
	str	r3, [r11, #F_H_MAGIC]
	ldr	r3, [r2, #UC_ARM_CPSR]
	str	r3, [r11, #F_H_CPSR]
	mov	r0, #0			/* SIG_BLOCK, nothing: read the mask */
	mov	r1, #0
	add	r2, r11, #F_H_MASK
	mov	r3, #8
	sc	SYS_rt_sigprocmask
	ldr	r0, [r11, #F_H_COUNT]
	add	r0, r0, #1
	str	r0, [r11, #F_H_COUNT]
	/* Leave everything the test checks changed. */
	movi	r0, 0xdeadbeef
	vmov	d8, r0, r0
	vmov	d9, r0, r0
	vmov	d16, r0, r0
	vmov	d31, r0, r0
	vmrs	r0, fpscr
	bic	r0, r0, #0x00c00000
	orr	r0, r0, #0x00400000	/* round towards +infinity */
	vmsr	fpscr, r0
	mov	r0, #0
	msr	APSR_nzcvq, r0
	bx	lr

restorer:
	mov	r7, #SYS_rt_sigreturn
	svc	#0

/* r0: sigaction flags, r1: the check-name prefix. */
t_signal:
	push	{r4, r5, r6, lr}
	str	r1, [r11, #F_PREFIX]
	mov	r4, r0
	lea	r0, handler
	str	r0, [r11, #F_SA]
	orr	r0, r4, #SA_SIGINFO
	str	r0, [r11, #(F_SA + 4)]
	lea	r0, restorer
	str	r0, [r11, #(F_SA + 8)]
	mov	r0, #0
	str	r0, [r11, #(F_SA + 12)]
	str	r0, [r11, #(F_SA + 16)]
	mov	r0, #SIGUSR1
	add	r1, r11, #F_SA
	mov	r2, #0
	mov	r3, #8
	sc	SYS_rt_sigaction
	eq	0, n_sigaction

	mov	r0, #0
	str	r0, [r11, #F_H_COUNT]
	sc	SYS_getpid
	mov	r5, r0
	/* The state that has to come back. */
	movi	r0, 0x11111111
	movi	r1, 0x22222222
	vmov	d8, r0, r1
	movi	r0, 0x33333333
	movi	r1, 0x44444444
	vmov	d16, r0, r1
	movi	r0, 0x55555555
	movi	r1, 0x66666666
	vmov	d31, r0, r1
	vmrs	r0, fpscr
	orr	r0, r0, #0x00c00000	/* round towards zero */
	vmsr	fpscr, r0
	mov	r0, r5
	mov	r1, #SIGUSR1
	movi	r3, 0xa8000000		/* N, C and Q */
	msr	APSR_nzcvq, r3
	sc	SYS_kill
	mrs	r6, apsr

	ldr	r0, [r11, #F_H_COUNT]
	eq	1, n_handler_ran
	ldr	r0, [r11, #F_H_SIGNO]
	eq	SIGUSR1, n_signo
	ldr	r0, [r11, #F_H_PID]
	mov	r1, r5
	lea	r2, n_si_pid
	bl	check_eq
	ldr	r0, [r11, #F_H_CODE]
	eq	0, n_si_code
	ldr	r0, [r11, #F_H_MAGIC]
	eq	VFP_MAGIC, n_vfp_magic
	ldr	r0, [r11, #F_H_CPSR]
	and	r0, r0, #0x3f
	eq	0x10, n_frame_cpsr
	ldr	r0, [r11, #F_H_MASK]
	and	r0, r0, #(1 << (SIGUSR1 - 1))
	eq	(1 << (SIGUSR1 - 1)), n_blocked

	tst	r4, #SA_RESTORER
	beq	1f
	ldr	r0, [r11, #F_H_LR]
	lea	r1, restorer
	lea	r2, n_restorer
	bl	check_eq
	b	2f
1:	ldr	r0, [r11, #F_H_LR]
	ldr	r0, [r0]
	eq	0xe3a070ad, n_sigpage

2:	chk_d	d8, 0x11111111, 0x22222222, n_d8
	chk_d	d16, 0x33333333, 0x44444444, n_d16
	chk_d	d31, 0x55555555, 0x66666666, n_d31
	vmrs	r0, fpscr
	and	r0, r0, #0x00c00000
	eq	0x00c00000, n_fpscr
	and	r0, r6, #0xf8000000
	eq	0xa8000000, n_flags

	mov	r0, #0			/* the mask is back */
	mov	r1, #0
	add	r2, r11, #F_WORK
	mov	r3, #8
	sc	SYS_rt_sigprocmask
	ldr	r0, [r11, #F_WORK]
	and	r0, r0, #(1 << (SIGUSR1 - 1))
	eq	0, n_unblocked
	lea	r0, s_empty
	str	r0, [r11, #F_PREFIX]
	pop	{r4, r5, r6, pc}

/* ---- cacheflush ---- */

/* r4: code at its start; flush the 8 bytes. */
flush8:
	push	{r7, lr}
	mov	r0, r4
	add	r1, r4, #8
	mov	r2, #0
	sc	SYS_cacheflush
	pop	{r7, pc}

t_cacheflush:
	push	{r4, r5, lr}
	mov	r0, #0
	ldr	r1, [r11, #F_PAGESZ]
	mov	r2, #7			/* PROT_READ | PROT_WRITE | PROT_EXEC */
	mov	r3, #0x22		/* MAP_PRIVATE | MAP_ANONYMOUS */
	mvn	r4, #0
	mov	r5, #0
	sc	SYS_mmap2
	mov	r4, r0
	cmn	r0, #4096
	movlo	r0, #1
	movhs	r0, #0
	lea	r1, n_mmap_rwx
	bl	report
	cmn	r4, #4096
	bhs	9f

	movi	r0, 0xe3a0002a		/* mov r0, #42 ; bx lr */
	str	r0, [r4]
	movi	r0, 0xe12fff1e
	str	r0, [r4, #4]
	bl	flush8
	eq	0, n_cacheflush
	blx	r4
	eq	42, n_cf_arm
	movi	r0, 0xe3a00063		/* mov r0, #99 */
	str	r0, [r4]
	bl	flush8
	blx	r4
	eq	99, n_cf_arm_rewrite

	movi	r0, 0x47702007		/* movs r0, #7 ; bx lr  (Thumb) */
	str	r0, [r4]
	bl	flush8
	orr	r0, r4, #1
	blx	r0
	eq	7, n_cf_thumb
	movi	r0, 0x47702008		/* movs r0, #8 */
	str	r0, [r4]
	bl	flush8
	orr	r0, r4, #1
	blx	r0
	eq	8, n_cf_thumb_rewrite

	mov	r0, r4
	ldr	r1, [r11, #F_PAGESZ]
	sc	SYS_munmap
9:	pop	{r4, r5, pc}

/* ---- Thumb state: the IT block around an SVC ---- */

	.thumb
	.thumb_func
thumb_fn:
	push	{r4, r5, r7, lr}
	movs	r7, #20			/* getpid */
	movs	r1, #0			/* Z */
	it	eq
	svceq	#0
	movs	r1, #1			/* flag-setting only outside an IT block */
	mov	r5, r0
	mrs	r4, apsr
	lsrs	r4, r4, #30		/* N and Z */
	mov	r0, r4
	mov	r1, r5
	pop	{r4, r5, r7, pc}
	.arm

t_thumb:
	push	{r4, r5, lr}
	lea	r0, thumb_fn
	orr	r0, r0, #1
	blx	r0
	mov	r4, r0
	mov	r5, r1
	mov	r0, r4
	eq	0, n_thumb_itstate
	sc	SYS_getpid
	mov	r1, r5
	lea	r2, n_thumb_svc_result
	bl	check_eq
	pop	{r4, r5, pc}

/* ---- mmap2 offsets and the 64-bit-argument system calls ---- */

t_mmap:
	push	{r4, r5, r6, lr}
	lea	r0, s_tmpfile
	movi	r1, 0x242		/* O_RDWR | O_CREAT | O_TRUNC */
	mov	r2, #0x180		/* 0600 */
	sc	SYS_open
	mov	r6, r0
	cmp	r0, #0
	movge	r0, #1
	movlt	r0, #0
	lea	r1, n_open
	bl	report
	cmp	r6, #0
	blt	9f

	mov	r5, #0			/* eight 4 KiB chunks, each tagged */
1:	movi	r0, 0x1000
	add	r0, r0, r5
	add	r1, r11, #F_FILE
	str	r0, [r1]
	mov	r0, r6
	movi	r2, 4096
	sc	SYS_write
	add	r5, r5, #1
	cmp	r5, #8
	blt	1b

	mov	r0, r6			/* pread64 (fd, buf, 4, pad, lo, hi) */
	add	r1, r11, #F_FILE
	mov	r2, #4
	mov	r3, #0
	movi	r4, 8192
	mov	r5, #0
	sc	SYS_pread64
	eq	4, n_pread64
	add	r0, r11, #F_FILE
	ldr	r0, [r0]
	eq	0x1002, n_pread64_data

	/* The offset is in 4 KiB units whatever the page size. */
	mov	r0, #0
	ldr	r1, [r11, #F_PAGESZ]
	mov	r2, #1			/* PROT_READ */
	mov	r3, #2			/* MAP_PRIVATE */
	mov	r4, r6
	ldr	r5, [r11, #F_PAGESZ]
	lsr	r5, r5, #12
	sc	SYS_mmap2
	mov	r4, r0
	cmn	r0, #4096
	movlo	r0, #1
	movhs	r0, #0
	lea	r1, n_mmap2
	bl	report
	cmn	r4, #4096
	bhs	2f
	ldr	r0, [r4]
	ldr	r1, [r11, #F_PAGESZ]
	lsr	r1, r1, #12
	add	r1, r1, #0x1000
	lea	r2, n_mmap2_data
	bl	check_eq
	mov	r0, r4
	ldr	r1, [r11, #F_PAGESZ]
	sc	SYS_munmap

2:	mov	r0, #0			/* an offset of one unit */
	ldr	r1, [r11, #F_PAGESZ]
	mov	r2, #1
	mov	r3, #2
	mov	r4, r6
	mov	r5, #1
	sc	SYS_mmap2
	mov	r4, r0
	cmn	r4, #4096		/* the second chunk */
	bhs	5f
	ldr	r0, [r4]
	eq	0x1001, n_mmap2_unit
	mov	r0, r4
	ldr	r1, [r11, #F_PAGESZ]
	sc	SYS_munmap
	b	4f
5:	mov	r0, #0
	lea	r1, n_mmap2_unit
	bl	report

4:	mov	r0, r6			/* ftruncate64 (fd, pad, lo, hi) */
	mov	r1, #0
	movi	r2, 0x2000
	mov	r3, #0
	sc	SYS_ftruncate64
	eq	0, n_ftruncate64
	mov	r0, r6
	add	r1, r11, #F_STAT
	sc	SYS_fstat64
	eq	0, n_fstat64
	add	r1, r11, #F_STAT
	ldr	r0, [r1, #48]
	eq	0x2000, n_st_size_lo
	add	r1, r11, #F_STAT
	ldr	r0, [r1, #52]
	eq	0, n_st_size_hi
	mov	r0, r6
	sc	SYS_close
	lea	r0, s_tmpfile
	sc	SYS_unlink
9:	pop	{r4, r5, r6, pc}

/* ---- PER_LINUX32: uname and /proc/cpuinfo ---- */

t_personality:
	push	{r4, r5, lr}
	mov	r0, #PER_LINUX32
	sc	SYS_personality
	eq	0, n_per_linux32
	add	r0, r11, #F_UNAME
	sc	SYS_uname
	eq	0, n_uname
	add	r0, r11, #(F_UNAME + 65 * 4)
	lea	r1, s_armv8l
	bl	streq
	lea	r1, n_uname_armv8l
	bl	report

	lea	r0, s_cpuinfo
	mov	r1, #0
	mov	r2, #0
	sc	SYS_open
	mov	r4, r0
	cmp	r0, #0
	blt	8f
	mov	r0, r4
	add	r1, r11, #F_CPU
	movi	r2, 0x1000
	sc	SYS_read
	mov	r5, r0
	mov	r0, r4
	sc	SYS_close
	cmp	r5, #0
	ble	8f
	mov	r2, #0
	add	r1, r11, #F_CPU
	strb	r2, [r1, r5]
	add	r0, r11, #F_CPU
	mov	r1, r5
	lea	r2, s_model
	bl	find
	cmp	r0, #0
	movne	r0, #1
	lea	r1, n_cpuinfo_model
	bl	report
	add	r0, r11, #F_CPU
	mov	r1, r5
	lea	r2, s_v8l_paren
	bl	find
	cmp	r0, #0
	movne	r0, #1
	lea	r1, n_cpuinfo_v8l
	bl	report
	add	r0, r11, #F_CPU
	mov	r1, r5
	lea	r2, s_features
	bl	find
	cmp	r0, #0
	movne	r0, #1
	lea	r1, n_cpuinfo_features
	bl	report
	b	9f
8:	mov	r0, #0
	lea	r1, n_cpuinfo_read
	bl	report

9:	mov	r0, #0			/* PER_LINUX: the 64-bit name again */
	sc	SYS_personality
	eq	PER_LINUX32, n_per_linux_prev
	add	r0, r11, #F_UNAME
	sc	SYS_uname
	add	r0, r11, #(F_UNAME + 65 * 4)
	lea	r1, s_aarch64
	bl	streq
	lea	r1, n_uname_aarch64
	bl	report
	pop	{r4, r5, pc}

/* ---- a nanosleep restarted after a stop ---- */

t_sleep:
	push	{r4, r5, r6, lr}
	sc	SYS_fork
	cmp	r0, #0
	beq	sleep_child
	mov	r4, r0
	movs	r0, r0
	movmi	r0, #0
	movpl	r0, #1
	lea	r1, n_fork
	bl	report

	mov	r0, #0			/* let the child reach nanosleep */
	str	r0, [r11, #F_WORK]
	movi	r0, 150000000
	str	r0, [r11, #(F_WORK + 4)]
	add	r0, r11, #F_WORK
	mov	r1, #0
	sc	SYS_nanosleep

	mov	r0, r4
	mov	r1, #SIGSTOP
	sc	SYS_kill
	mov	r0, r4			/* wait4 (pid, &status, WUNTRACED) */
	add	r1, r11, #(F_WORK + 8)
	mov	r2, #2
	mov	r3, #0
	sc	SYS_wait4
	ldr	r0, [r11, #(F_WORK + 8)]
	movi	r1, 0x00001300 | 0x7f	/* stopped by SIGSTOP */
	lea	r2, n_child_stopped
	bl	check_eq
	mov	r0, r4
	mov	r1, #SIGCONT
	sc	SYS_kill
	mov	r0, r4
	add	r1, r11, #(F_WORK + 8)
	mov	r2, #0
	mov	r3, #0
	sc	SYS_wait4
	ldr	r0, [r11, #(F_WORK + 8)]
	eq	0, n_nanosleep_restarted
	pop	{r4, r5, r6, pc}

/* The child sleeps 800 ms and exits 0 if nanosleep finished after that long
 * with a zero result. */
sleep_child:
	mov	r0, #1			/* CLOCK_MONOTONIC */
	add	r1, r11, #(F_WORK + 16)
	sc	SYS_clock_gettime
	mov	r0, #0
	str	r0, [r11, #F_WORK]
	movi	r0, 800000000
	str	r0, [r11, #(F_WORK + 4)]
	add	r0, r11, #F_WORK
	mov	r1, #0
	sc	SYS_nanosleep
	mov	r4, r0
	mov	r0, #1
	add	r1, r11, #(F_WORK + 24)
	sc	SYS_clock_gettime
	ldr	r0, [r11, #(F_WORK + 24)]	/* seconds */
	ldr	r1, [r11, #(F_WORK + 16)]
	sub	r0, r0, r1
	movi	r2, 1000000000
	mul	r0, r0, r2
	ldr	r1, [r11, #(F_WORK + 28)]	/* nanoseconds */
	ldr	r2, [r11, #(F_WORK + 20)]
	sub	r1, r1, r2
	add	r0, r0, r1
	movi	r1, 790000000
	cmp	r0, r1
	movlt	r0, #1
	movge	r0, #0
	cmp	r4, #0
	movne	r0, #2
	sc	SYS_exit_group

/* ---- 32 -> 64 -> 32: exec the shell, which execs this program again ---- */

t_exec:
	push	{r4, lr}
	sc	SYS_fork
	cmp	r0, #0
	beq	exec_child
	mov	r4, r0
	mov	r0, r4
	add	r1, r11, #(F_WORK + 8)
	mov	r2, #0
	mov	r3, #0
	sc	SYS_wait4
	ldr	r0, [r11, #(F_WORK + 8)]
	movi	r1, 37 << 8
	lea	r2, n_exec_chain
	bl	check_eq
	pop	{r4, pc}

exec_child:
	lea	r0, s_self
	add	r1, r11, #F_PATH
	mov	r2, #255
	sc	SYS_readlink
	cmp	r0, #0
	ble	1f
	mov	r1, #0
	add	r2, r11, #F_PATH
	strb	r1, [r2, r0]
	lea	r0, s_sh_arg0		/* sh -c 'exec "$0" leaf' <this program> */
	str	r0, [r11, #F_ARGVX]
	lea	r0, s_dash_c
	str	r0, [r11, #(F_ARGVX + 4)]
	lea	r0, s_exec_leaf
	str	r0, [r11, #(F_ARGVX + 8)]
	add	r0, r11, #F_PATH
	str	r0, [r11, #(F_ARGVX + 12)]
	mov	r0, #0
	str	r0, [r11, #(F_ARGVX + 16)]
	lea	r0, s_bin_sh
	add	r1, r11, #F_ARGVX
	add	r2, r11, #(F_ARGVX + 16)	/* an empty environment */
	sc	SYS_execve
1:	mov	r0, #111
	sc	SYS_exit_group

/* This program's path into F_PATH -> r0 (F_PATH). */
self_path:
	push	{r7, lr}
	lea	r0, s_self
	add	r1, r11, #F_PATH
	mov	r2, #255
	sc	SYS_readlink
	cmp	r0, #0
	movle	r0, #0
	mov	r1, #0
	add	r2, r11, #F_PATH
	strb	r1, [r2, r0]
	mov	r0, r2
	pop	{r7, pc}

/* ---- DEV extras: threads, fault signals, stack, brk ---- */

	.equ	SYS_clone,	120
	.equ	SYS_futex,	240
	.equ	SYS_gettid,	224
	.equ	SYS_brk,	45
	.equ	SYS_getppid,	64
	.equ	SIGSEGV,	11

mode_extra:
	lea	r0, s_start
	bl	puts
	bl	t_thread
	bl	t_segvh
	bl	t_stack
	bl	t_brk
	bl	t_mmap_hi
	bl	t_syscalls
	bl	t_vfork
	bl	t_timer
	bl	t_clone3
	bl	t_registrations
	bl	t_altstack
	bl	t_seccomp
	b	summary

/* clone with CLONE_SETTLS, PARENT_SETTID and CHILD_CLEARTID. */
t_thread:
	push	{r4, r5, r6, lr}
	movi	r0, 0x600d0000
	sc	SYS_set_tls
	mov	r0, #0
	movi	r1, 0x2000
	mov	r2, #3
	mov	r3, #0x22
	mvn	r4, #0
	mov	r5, #0
	sc	SYS_mmap2
	mov	r6, r0
	cmn	r0, #4096
	movlo	r0, #1
	movhs	r0, #0
	lea	r1, n_th_stack
	bl	report
	cmn	r6, #4096
	bhs	9f
	mov	r0, #0
	str	r0, [r11, #F_WORK]
	movi	r0, 0xffffffff
	str	r0, [r11, #(F_WORK + 4)]
	movi	r0, 0x003d0f00
	add	r1, r6, #0x2000
	add	r2, r11, #F_WORK
	movi	r3, 0x55aa1230
	add	r4, r11, #(F_WORK + 4)
	sc	SYS_clone
	cmp	r0, #0
	beq	thread_child
	movs	r5, r0
	movgt	r0, #1
	movle	r0, #0
	lea	r1, n_th_clone
	bl	report
1:	ldr	r0, [r11, #(F_WORK + 4)]
	cmp	r0, #0
	beq	2f
	mov	r2, r0
	mov	r1, #0
	mov	r3, #0
	add	r0, r11, #(F_WORK + 4)
	sc	SYS_futex
	b	1b
2:	ldr	r0, [r11, #F_WORK]
	mov	r1, r5
	lea	r2, n_th_ptid
	bl	check_eq
	ldr	r0, [r11, #(F_WORK + 8)]
	eq	0x55aa1230, n_th_tls
	ldr	r0, [r11, #(F_WORK + 12)]
	mov	r1, r5
	lea	r2, n_th_gettid
	bl	check_eq
	mrc	p15, #0, r0, c13, c0, #3
	eq	0x600d0000, n_th_parent_tls
	ldr	r0, [r11, #(F_WORK + 16)]
	eq	0x77, n_th_sp
	mov	r0, r6
	movi	r1, 0x2000
	sc	SYS_munmap
9:	pop	{r4, r5, r6, pc}

thread_child:
	mrc	p15, #0, r0, c13, c0, #3
	str	r0, [r11, #(F_WORK + 8)]
	sc	SYS_gettid
	str	r0, [r11, #(F_WORK + 12)]
	mov	r0, #0x77
	str	r0, [r11, #(F_WORK + 16)]
	mov	r0, #0
	sc	SYS_exit

/* A SIGSEGV handler that records the fault and steps over the instruction. */
segv_handler:
	ldr	r3, [r1, #12]		/* si_addr */
	str	r3, [r11, #(F_WORK + 24)]
	ldr	r3, [r1, #8]		/* si_code */
	str	r3, [r11, #(F_WORK + 28)]
	ldr	r3, [r2, #100]		/* fault_address */
	str	r3, [r11, #(F_WORK + 32)]
	ldr	r3, [r2, #92]		/* arm_pc: step over the access */
	add	r3, r3, #4
	str	r3, [r2, #92]
	ldr	r3, [r11, #(F_WORK + 36)]
	add	r3, r3, #1
	str	r3, [r11, #(F_WORK + 36)]
	bx	lr

t_segvh:
	push	{r4, r5, lr}
	lea	r0, segv_handler
	str	r0, [r11, #F_SA]
	mov	r0, #SA_SIGINFO
	str	r0, [r11, #(F_SA + 4)]
	mov	r0, #0
	str	r0, [r11, #(F_SA + 8)]
	str	r0, [r11, #(F_SA + 12)]
	str	r0, [r11, #(F_SA + 16)]
	str	r0, [r11, #(F_WORK + 36)]
	mov	r0, #SIGSEGV
	add	r1, r11, #F_SA
	mov	r2, #0
	mov	r3, #8
	sc	SYS_rt_sigaction
	eq	0, n_sv_sigaction
	movi	r4, 0x12345678
	mov	r1, #0x10
	ldr	r2, [r1]		/* faults; the handler skips it */
	mov	r0, r4
	eq	0x12345678, n_sv_r4_kept
	ldr	r0, [r11, #(F_WORK + 24)]
	eq	0x10, n_sv_si_addr
	ldr	r0, [r11, #(F_WORK + 28)]
	eq	1, n_sv_si_code
	ldr	r0, [r11, #(F_WORK + 32)]
	eq	0x10, n_sv_fault_address
	movi	r1, 0xfffff000
	str	r4, [r1]		/* the top of the 32-bit space */
	ldr	r0, [r11, #(F_WORK + 24)]
	eq	0xfffff000, n_sv_si_addr_high
	ldr	r0, [r11, #(F_WORK + 36)]
	eq	2, n_sv_count
	pop	{r4, r5, pc}

/* The stack grows downward by a megabyte on a single access. */
t_stack:
	push	{r4, lr}
	mov	r4, sp
	movi	r0, 0x100000
	sub	r0, sp, r0
	movi	r1, 0x5a5a5a5a
	str	r1, [r0]
	ldr	r0, [r0]
	eq	0x5a5a5a5a, n_stack_grow
	pop	{r4, pc}

t_brk:
	push	{r4, lr}
	mov	r0, #0
	sc	SYS_brk
	mov	r4, r0
	add	r0, r4, #0x8000
	sc	SYS_brk
	add	r1, r4, #0x8000
	lea	r2, n_brk_grow
	bl	check_eq
	movi	r1, 0x77665544
	str	r1, [r4]
	ldr	r0, [r4]
	eq	0x77665544, n_brk_data
	mov	r0, r4
	sc	SYS_brk
	pop	{r4, pc}

	.equ	SYS_pipe2,	359
	.equ	SYS_ppoll_time64, 414
	.equ	SYS_clock_gettime64, 403
	.equ	SYS_getrandom,	384
	.equ	SYS_socketpair,	288
	.equ	SYS_sendmsg,	296
	.equ	SYS_recvmsg,	297
	.equ	SYS_statx,	397
	.equ	SYS_getdents64,	217

/* The 32-bit layouts glibc's time64 and socket calls rely on. */
t_syscalls:
	push	{r4, r5, r6, lr}
	add	r0, r11, #F_WORK
	mov	r1, #0
	sc	SYS_pipe2
	eq	0, n_sc_pipe2
	ldr	r6, [r11, #F_WORK]		/* read end */
	ldr	r5, [r11, #(F_WORK + 4)]	/* write end */
	mov	r0, r5
	lea	r1, s_hello
	mov	r2, #1
	sc	SYS_write
	eq	1, n_sc_pipe_write
	str	r6, [r11, #(F_WORK + 16)]	/* pollfd: fd, POLLIN, revents */
	mov	r0, #1
	str	r0, [r11, #(F_WORK + 20)]
	mov	r0, #0
	str	r0, [r11, #(F_WORK + 24)]	/* timespec64: 0 */
	str	r0, [r11, #(F_WORK + 28)]
	str	r0, [r11, #(F_WORK + 32)]
	str	r0, [r11, #(F_WORK + 36)]
	add	r0, r11, #(F_WORK + 16)
	mov	r1, #1
	add	r2, r11, #(F_WORK + 24)
	mov	r3, #0
	mov	r4, #8
	sc	SYS_ppoll_time64
	eq	1, n_sc_ppoll
	ldr	r0, [r11, #(F_WORK + 20)]
	lsr	r0, r0, #16
	eq	1, n_sc_revents
	mov	r0, r6
	add	r1, r11, #(F_WORK + 40)
	mov	r2, #1
	sc	SYS_read
	eq	1, n_sc_pipe_read
	ldrb	r0, [r11, #(F_WORK + 40)]
	eq	0x61, n_sc_pipe_byte

	mov	r0, #1				/* CLOCK_MONOTONIC */
	add	r1, r11, #(F_WORK + 48)
	sc	SYS_clock_gettime64
	eq	0, n_sc_clock_gettime64
	ldr	r0, [r11, #(F_WORK + 56)]	/* tv_nsec */
	movi	r1, 1000000000
	cmp	r0, r1
	movlo	r0, #1
	movhs	r0, #0
	lea	r1, n_sc_clock_nsec
	bl	report
	ldr	r0, [r11, #(F_WORK + 52)]	/* tv_sec high word */
	eq	0, n_sc_clock_hi

	add	r0, r11, #(F_WORK + 64)
	mov	r1, #8
	mov	r2, #0
	sc	SYS_getrandom
	eq	8, n_sc_getrandom

	mov	r0, #1				/* AF_UNIX */
	mov	r1, #1				/* SOCK_STREAM */
	mov	r2, #0
	add	r3, r11, #(F_WORK + 72)
	sc	SYS_socketpair
	eq	0, n_sc_socketpair
	/* iovec at F_CPU: one byte of s_hello */
	lea	r0, s_hello
	str	r0, [r11, #F_CPU]
	mov	r0, #1
	str	r0, [r11, #(F_CPU + 4)]
	/* msghdr at F_CPU + 16: name, namelen, iov, iovlen, control,
	 * controllen, flags */
	mov	r0, #0
	str	r0, [r11, #(F_CPU + 16)]
	str	r0, [r11, #(F_CPU + 20)]
	add	r0, r11, #F_CPU
	str	r0, [r11, #(F_CPU + 24)]
	mov	r0, #1
	str	r0, [r11, #(F_CPU + 28)]
	add	r0, r11, #(F_CPU + 0x40)
	str	r0, [r11, #(F_CPU + 32)]
	mov	r0, #16
	str	r0, [r11, #(F_CPU + 36)]
	mov	r0, #0
	str	r0, [r11, #(F_CPU + 40)]
	/* cmsghdr: len 16, SOL_SOCKET, SCM_RIGHTS, the pipe's read end */
	mov	r0, #16
	str	r0, [r11, #(F_CPU + 0x40)]
	mov	r0, #1
	str	r0, [r11, #(F_CPU + 0x44)]
	str	r0, [r11, #(F_CPU + 0x48)]
	str	r6, [r11, #(F_CPU + 0x4c)]
	ldr	r0, [r11, #(F_WORK + 72)]
	add	r1, r11, #(F_CPU + 16)
	mov	r2, #0
	sc	SYS_sendmsg
	eq	1, n_sc_sendmsg
	/* the receiving msghdr at F_CPU + 0x100 */
	add	r0, r11, #(F_WORK + 96)
	str	r0, [r11, #(F_CPU + 0x100)]	/* iov base: a byte of scratch */
	mov	r0, #8
	str	r0, [r11, #(F_CPU + 0x104)]
	mov	r0, #0
	str	r0, [r11, #(F_CPU + 0x110)]
	str	r0, [r11, #(F_CPU + 0x114)]
	add	r0, r11, #(F_CPU + 0x100)
	str	r0, [r11, #(F_CPU + 0x118)]
	mov	r0, #1
	str	r0, [r11, #(F_CPU + 0x11c)]
	add	r0, r11, #(F_CPU + 0x140)
	str	r0, [r11, #(F_CPU + 0x120)]
	mov	r0, #32
	str	r0, [r11, #(F_CPU + 0x124)]
	mov	r0, #0
	str	r0, [r11, #(F_CPU + 0x128)]
	ldr	r0, [r11, #(F_WORK + 76)]
	add	r1, r11, #(F_CPU + 0x110)
	mov	r2, #0
	sc	SYS_recvmsg
	eq	1, n_sc_recvmsg
	ldr	r0, [r11, #(F_CPU + 0x124)]	/* msg_controllen */
	eq	16, n_sc_controllen
	ldr	r0, [r11, #(F_CPU + 0x140)]	/* cmsg_len */
	eq	16, n_sc_cmsg_len
	ldr	r0, [r11, #(F_CPU + 0x144)]
	eq	1, n_sc_cmsg_level
	ldr	r0, [r11, #(F_CPU + 0x148)]
	eq	1, n_sc_cmsg_type
	ldr	r4, [r11, #(F_CPU + 0x14c)]	/* the received descriptor */
	mov	r0, r5				/* write a byte, read it there */
	lea	r1, s_hello
	mov	r2, #1
	sc	SYS_write
	mov	r0, r4
	add	r1, r11, #(F_WORK + 104)
	mov	r2, #1
	sc	SYS_read
	eq	1, n_sc_passed_fd

	movi	r0, 0xffffff9c			/* AT_FDCWD */
	lea	r1, s_root
	mov	r2, #0
	movi	r3, 0x7ff
	add	r4, r11, #(F_CPU + 0x200)
	sc	SYS_statx
	eq	0, n_sc_statx
	ldr	r0, [r11, #(F_CPU + 0x200 + 28)]
	and	r0, r0, #0xf000
	eq	0x4000, n_sc_statx_dir
	lea	r0, s_root
	movi	r1, 0x4000			/* O_DIRECTORY */
	mov	r2, #0
	sc	SYS_open
	mov	r4, r0
	cmp	r0, #0
	movge	r0, #1
	movlt	r0, #0
	lea	r1, n_sc_open_dir
	bl	report
	mov	r0, r4
	add	r1, r11, #(F_CPU + 0x300)
	movi	r2, 1024
	sc	SYS_getdents64
	cmp	r0, #0
	movgt	r0, #1
	movle	r0, #0
	lea	r1, n_sc_getdents64
	bl	report
	mov	r0, r4
	sc	SYS_close
	pop	{r4, r5, r6, pc}

	.equ	SYS_vfork,	190
	.equ	SYS_prctl,	172
	.equ	SYS_seccomp,	383
	.equ	SYS_setitimer,	104
	.equ	SIGALRM,	14

/* vfork and execve from the child, as posix_spawn does. */
t_vfork:
	push	{r4, lr}
	sc	SYS_vfork
	cmp	r0, #0
	beq	vfork_child
	mov	r4, r0
	cmp	r0, #0
	movgt	r0, #1
	movle	r0, #0
	lea	r1, n_vf_pid
	bl	report
	mov	r0, r4
	add	r1, r11, #(F_WORK + 8)
	mov	r2, #0
	mov	r3, #0
	sc	SYS_wait4
	ldr	r0, [r11, #(F_WORK + 8)]
	eq	0x2500, n_vf_status
	pop	{r4, pc}

vfork_child:
	bl	self_path
	str	r0, [r11, #F_ARGVX]
	lea	r0, s_leaf
	str	r0, [r11, #(F_ARGVX + 4)]
	mov	r0, #0
	str	r0, [r11, #(F_ARGVX + 8)]
	add	r0, r11, #F_PATH
	add	r1, r11, #F_ARGVX
	add	r2, r11, #(F_ARGVX + 8)
	sc	SYS_execve
	mov	r0, #111
	sc	SYS_exit_group

alrm_handler:
	mov	r0, #1
	str	r0, [r11, #(F_WORK + 60)]
	bx	lr

/* A timer signal reaches a task that is running user code. */
t_timer:
	push	{r4, lr}
	mov	r0, #0
	str	r0, [r11, #(F_WORK + 60)]
	lea	r0, alrm_handler
	str	r0, [r11, #F_SA]
	mov	r0, #0
	str	r0, [r11, #(F_SA + 4)]
	str	r0, [r11, #(F_SA + 8)]
	str	r0, [r11, #(F_SA + 12)]
	str	r0, [r11, #(F_SA + 16)]
	mov	r0, #SIGALRM
	add	r1, r11, #F_SA
	mov	r2, #0
	mov	r3, #8
	sc	SYS_rt_sigaction
	eq	0, n_tm_sigaction
	mov	r0, #0
	str	r0, [r11, #F_WORK]
	str	r0, [r11, #(F_WORK + 4)]
	str	r0, [r11, #(F_WORK + 8)]
	movi	r0, 50000
	str	r0, [r11, #(F_WORK + 12)]
	mov	r0, #0
	add	r1, r11, #F_WORK
	mov	r2, #0
	sc	SYS_setitimer
	eq	0, n_tm_setitimer
	movi	r4, 0x08000000
1:	ldr	r0, [r11, #(F_WORK + 60)]
	cmp	r0, #0
	bne	2f
	subs	r4, r4, #1
	bne	1b
2:	ldr	r0, [r11, #(F_WORK + 60)]
	eq	1, n_tm_fired
	pop	{r4, pc}

/* seccomp sees an ARM task: AUDIT_ARCH_ARM and the EABI numbers. */
t_seccomp:
	push	{r4, lr}
	mov	r0, #38			/* PR_SET_NO_NEW_PRIVS */
	mov	r1, #1
	mov	r2, #0
	mov	r3, #0
	mov	r4, #0
	sc	SYS_prctl
	eq	0, n_sec_nnp
	/* ld arch; jeq AUDIT_ARCH_ARM -> allow; ret ERRNO(EACCES); allow */
	movi	r0, 0x00000020
	str	r0, [r11, #(F_WORK + 0)]
	mov	r0, #4
	str	r0, [r11, #(F_WORK + 4)]
	movi	r0, 0x00010015
	str	r0, [r11, #(F_WORK + 8)]
	movi	r0, 0x40000028
	str	r0, [r11, #(F_WORK + 12)]
	mov	r0, #6
	str	r0, [r11, #(F_WORK + 16)]
	movi	r0, 0x0005000d
	str	r0, [r11, #(F_WORK + 20)]
	mov	r0, #6
	str	r0, [r11, #(F_WORK + 24)]
	movi	r0, 0x7fff0000
	str	r0, [r11, #(F_WORK + 28)]
	mov	r0, #4			/* struct sock_fprog: len, filter */
	str	r0, [r11, #(F_WORK + 32)]
	add	r0, r11, #F_WORK
	str	r0, [r11, #(F_WORK + 36)]
	mov	r0, #1			/* SECCOMP_SET_MODE_FILTER */
	mov	r1, #0
	add	r2, r11, #(F_WORK + 32)
	sc	SYS_seccomp
	eq	0, n_sec_filter_arch
	sc	SYS_getpid
	cmp	r0, #0
	movgt	r0, #1
	movle	r0, #0
	lea	r1, n_sec_arch_ok
	bl	report
	/* ld nr; jeq getpid -> ERRNO(EPERM); allow */
	mov	r0, #0x20
	str	r0, [r11, #(F_WORK + 0)]
	mov	r0, #0
	str	r0, [r11, #(F_WORK + 4)]
	movi	r0, 0x01000015
	str	r0, [r11, #(F_WORK + 8)]
	mov	r0, #20
	str	r0, [r11, #(F_WORK + 12)]
	mov	r0, #6
	str	r0, [r11, #(F_WORK + 16)]
	movi	r0, 0x00050001
	str	r0, [r11, #(F_WORK + 20)]
	mov	r0, #6
	str	r0, [r11, #(F_WORK + 24)]
	movi	r0, 0x7fff0000
	str	r0, [r11, #(F_WORK + 28)]
	mov	r0, #1
	mov	r1, #0
	add	r2, r11, #(F_WORK + 32)
	sc	SYS_seccomp
	eq	0, n_sec_filter_nr
	sc	SYS_getpid
	eq	0xffffffff, n_sec_getpid_denied
	sc	SYS_gettid
	cmp	r0, #0
	movgt	r0, #1
	movle	r0, #0
	lea	r1, n_sec_gettid_ok
	bl	report
	pop	{r4, pc}

	.equ	SYS_clone3,	435
	.equ	SYS_rseq,	398
	.equ	SYS_set_robust_list, 338
	.equ	SYS_get_robust_list, 339
	.equ	SYS_sigaltstack, 186
	.equ	SA_ONSTACK,	0x08000000

/* clone3, as glibc 2.34 and later start threads. */
t_clone3:
	push	{r4, r5, r6, lr}
	mov	r0, #0
	movi	r1, 0x2000
	mov	r2, #3
	mov	r3, #0x22
	mvn	r4, #0
	mov	r5, #0
	sc	SYS_mmap2
	mov	r6, r0
	cmn	r0, #4096
	bhs	9f
	mov	r0, #0
	str	r0, [r11, #F_WORK]
	movi	r0, 0xffffffff
	str	r0, [r11, #(F_WORK + 4)]
	/* struct clone_args at F_CPU, 88 bytes of 64-bit fields */
	mov	r0, #0
	add	r1, r11, #F_CPU
	add	r2, r1, #88
1:	str	r0, [r1], #4
	cmp	r1, r2
	blo	1b
	movi	r0, 0x003d0f00		/* flags */
	str	r0, [r11, #(F_CPU + 0)]
	add	r0, r11, #(F_WORK + 4)	/* child_tid */
	str	r0, [r11, #(F_CPU + 16)]
	add	r0, r11, #F_WORK	/* parent_tid */
	str	r0, [r11, #(F_CPU + 24)]
	str	r6, [r11, #(F_CPU + 40)]	/* stack */
	movi	r0, 0x2000
	str	r0, [r11, #(F_CPU + 48)]	/* stack_size */
	movi	r0, 0x55aa1230
	str	r0, [r11, #(F_CPU + 56)]	/* tls */
	add	r0, r11, #F_CPU
	mov	r1, #88
	sc	SYS_clone3
	cmp	r0, #0
	beq	clone3_child
	movs	r5, r0
	movgt	r0, #1
	movle	r0, #0
	lea	r1, n_c3_clone3
	bl	report
	cmp	r5, #0
	ble	9f
3:	ldr	r0, [r11, #(F_WORK + 4)]
	cmp	r0, #0
	beq	4f
	mov	r2, r0
	mov	r1, #0
	mov	r3, #0
	add	r0, r11, #(F_WORK + 4)
	sc	SYS_futex
	b	3b
4:	ldr	r0, [r11, #(F_WORK + 8)]
	eq	0x55aa1230, n_c3_tls
	ldr	r0, [r11, #(F_WORK + 12)]
	mov	r1, r5
	lea	r2, n_c3_gettid
	bl	check_eq
	ldr	r0, [r11, #(F_WORK + 16)]
	eq	0x77, n_c3_sp
	mov	r0, r6
	movi	r1, 0x2000
	sc	SYS_munmap
9:	pop	{r4, r5, r6, pc}

clone3_child:
	mrc	p15, #0, r0, c13, c0, #3
	str	r0, [r11, #(F_WORK + 8)]
	sc	SYS_gettid
	str	r0, [r11, #(F_WORK + 12)]
	mov	r0, #0x77
	str	r0, [r11, #(F_WORK + 16)]
	mov	r0, #0
	sc	SYS_exit

/* rseq and the robust list register with 32-bit layouts. */
t_registrations:
	push	{r4, lr}
	/* struct rseq is 32 bytes, 32-byte aligned: align inside the frame */
	add	r4, r11, #(F_CPU + 0x400)
	add	r4, r4, #31
	bic	r4, r4, #31
	mov	r0, r4
	mov	r1, #32
	mov	r2, #0
	movi	r3, 0x53053053
	sc	SYS_rseq
	cmp	r0, #0
	moveq	r1, #1
	movne	r1, #0
	cmn	r0, #38
	moveq	r1, #1
	mov	r0, r1
	lea	r1, n_reg_rseq
	bl	report
	ldr	r0, [r4, #4]			/* cpu_id */
	cmp	r0, #0
	movge	r0, #1
	movlt	r0, #0
	lea	r1, n_reg_rseq_cpu
	bl	report
	mov	r0, r4
	mov	r1, #32
	movi	r2, 1				/* RSEQ_FLAG_UNREGISTER */
	movi	r3, 0x53053053
	sc	SYS_rseq
	cmp	r0, #0
	moveq	r1, #1
	movne	r1, #0
	cmn	r0, #38
	moveq	r1, #1
	mov	r0, r1
	lea	r1, n_reg_rseq_unreg
	bl	report
	add	r0, r11, #(F_CPU + 0x440)	/* a robust list head: 12 bytes */
	str	r0, [r0]
	mov	r1, #0
	str	r1, [r0, #4]
	str	r1, [r0, #8]
	mov	r1, #12
	sc	SYS_set_robust_list
	eq	0, n_reg_set_robust
	mov	r0, #0
	add	r1, r11, #(F_CPU + 0x480)
	add	r2, r11, #(F_CPU + 0x490)
	sc	SYS_get_robust_list
	eq	0, n_reg_get_robust
	ldr	r0, [r11, #(F_CPU + 0x490)]
	eq	12, n_reg_robust_len
	pop	{r4, pc}

/* A handler on an alternate stack. */
altstack_handler:
	mov	r3, sp
	str	r3, [r11, #(F_WORK + 8)]
	bx	lr

t_altstack:
	push	{r4, r5, lr}
	mov	r0, #0
	movi	r1, 0x4000
	mov	r2, #3
	mov	r3, #0x22
	mvn	r4, #0
	mov	r5, #0
	sc	SYS_mmap2
	mov	r5, r0
	cmn	r0, #4096
	bhs	9f
	str	r5, [r11, #(F_WORK + 16)]	/* stack_t: sp, flags, size */
	mov	r0, #0
	str	r0, [r11, #(F_WORK + 20)]
	movi	r0, 0x4000
	str	r0, [r11, #(F_WORK + 24)]
	add	r0, r11, #(F_WORK + 16)
	mov	r1, #0
	sc	SYS_sigaltstack
	eq	0, n_alt_sigaltstack
	lea	r0, altstack_handler
	str	r0, [r11, #F_SA]
	movi	r0, SA_ONSTACK | SA_SIGINFO
	str	r0, [r11, #(F_SA + 4)]
	mov	r0, #0
	str	r0, [r11, #(F_SA + 8)]
	str	r0, [r11, #(F_SA + 12)]
	str	r0, [r11, #(F_SA + 16)]
	mov	r0, #SIGUSR1
	add	r1, r11, #F_SA
	mov	r2, #0
	mov	r3, #8
	sc	SYS_rt_sigaction
	eq	0, n_alt_sigaction
	mov	r0, #0
	str	r0, [r11, #(F_WORK + 8)]
	sc	SYS_getpid
	mov	r1, #SIGUSR1
	sc	SYS_kill
	ldr	r0, [r11, #(F_WORK + 8)]
	cmp	r0, r5
	movhs	r1, #1
	movlo	r1, #0
	movi	r2, 0x4000
	add	r2, r5, r2
	cmp	r0, r2
	movlo	r0, #1
	movhs	r0, #0
	and	r0, r0, r1
	lea	r1, n_alt_on_stack
	bl	report
	mov	r0, r5
	movi	r1, 0x4000
	sc	SYS_munmap
9:	pop	{r4, r5, pc}

/* Anonymous mappings land below the vectors; a fixed one at the top of the
 * 32-bit space is refused. */
t_mmap_hi:
	push	{r4, r5, lr}
	mov	r0, #0
	movi	r1, 0x04000000
	mov	r2, #3
	mov	r3, #0x22
	mvn	r4, #0
	mov	r5, #0
	sc	SYS_mmap2
	mov	r4, r0
	cmn	r0, #4096
	movlo	r0, #1
	movhs	r0, #0
	lea	r1, n_mm_64m
	bl	report
	movi	r0, 0xffff0000
	cmp	r4, r0
	movlo	r0, #1
	movhs	r0, #0
	lea	r1, n_mm_below_vectors
	bl	report
	movi	r1, 0x04000000
	mov	r0, r4
	sc	SYS_munmap
	/* 3 GiB does not fit beside the stack and libraries: ENOMEM or ok,
	 * but never an address above the vectors. */
	mov	r0, #0
	movi	r1, 0xc0000000
	mov	r2, #3
	mov	r3, #0x22
	mvn	r4, #0
	mov	r5, #0
	sc	SYS_mmap2
	mov	r4, r0
	cmn	r4, #4096
	bhs	1f
	movi	r1, 0xffff0000
	sub	r1, r1, r0
	movi	r2, 0xc0000000
	cmp	r1, r2
	movhs	r0, #1
	movlo	r0, #0
	lea	r1, n_mm_3g_fits
	bl	report
	mov	r0, r4
	movi	r1, 0xc0000000
	sc	SYS_munmap
	b	2f
1:	mov	r0, r4
	eq	0xfffffff4, n_mm_3g_enomem
2:	pop	{r4, r5, pc}

/* ---- a 32-bit tracer: the legacy ARM ptrace requests ---- */

	.equ	SYS_ptrace,	26

mode_tracer:
	lea	r0, s_start
	bl	puts
	bl	t_tracer
	b	summary

tr_wait:
	push	{lr}
	mov	r0, r4
	add	r1, r11, #(F_WORK + 8)
	mov	r2, #0
	mov	r3, #0
	sc	SYS_wait4
	ldr	r0, [r11, #(F_WORK + 8)]
	pop	{pc}

/* ptrace (request r0, pid r4, addr r2, data r3) */
.macro ptr req, addr, data
	mov	r0, #\req
	mov	r1, r4
	movi	r2, \addr
	movi	r3, \data
	sc	SYS_ptrace
.endm

.macro ptr_buf req, addr, off
	mov	r0, #\req
	mov	r1, r4
	movi	r2, \addr
	add	r3, r11, #\off
	sc	SYS_ptrace
.endm

t_tracer:
	push	{r4, r5, r6, lr}
	sc	SYS_fork
	cmp	r0, #0
	beq	tracer_child
	mov	r4, r0
	bl	tr_wait
	eq	0x57f, n_tr_exec_stop
	ptr_buf	12, 0, F_UNAME
	eq	0, n_tr_getregs
	ldr	r0, [r11, #(F_UNAME + 60)]
	lea	r1, _start
	lea	r2, n_tr_pc
	bl	check_eq
	ldr	r0, [r11, #(F_UNAME + 64)]
	and	r0, r0, #0x1f
	eq	0x10, n_tr_cpsr
	ptr_buf	27, 0, F_CPU
	eq	0, n_tr_getvfp
	ptr_buf	22, 0, F_WORK
	eq	0, n_tr_thread_area
	ptr_buf	3, 0x10000, (F_WORK + 4)
	eq	0, n_tr_peekusr
	ldr	r0, [r11, #(F_WORK + 4)]
	eq	0x10000, n_tr_text_addr
	ptr_buf	3, 0x3c, (F_WORK + 4)		/* PEEKUSR of r15 */
	ldr	r0, [r11, #(F_WORK + 4)]
	lea	r1, _start
	lea	r2, n_tr_peekusr_pc
	bl	check_eq
	movi	r0, 0x5151
	str	r0, [r11, #(F_UNAME + 20)]
	ptr_buf	13, 0, F_UNAME
	eq	0, n_tr_setregs
	ptr_buf	12, 0, (F_UNAME + 0x80)
	ldr	r0, [r11, #(F_UNAME + 0x80 + 20)]
	eq	0x5151, n_tr_setregs_took
	ptr	24, 0, 0			/* PTRACE_SYSCALL */
	bl	tr_wait
	eq	0x57f, n_tr_syscall_stop
	ptr_buf	12, 0, F_UNAME
	ldr	r0, [r11, #(F_UNAME + 28)]
	eq	4, n_tr_syscall_nr
	ptr	23, 0, 20			/* PTRACE_SET_SYSCALL: getpid */
	eq	0, n_tr_set_syscall
	ptr	24, 0, 0
	bl	tr_wait
	eq	0x57f, n_tr_exit_stop
	ptr_buf	12, 0, F_UNAME
	ldr	r0, [r11, #(F_UNAME + 0)]
	mov	r1, r4
	lea	r2, n_tr_replaced
	bl	check_eq
	ptr	7, 0, 0				/* PTRACE_CONT */
	bl	tr_wait
	eq	0x2500, n_tr_exit_status
	pop	{r4, r5, r6, pc}

tracer_child:
	mov	r0, #0
	mov	r1, #0
	mov	r2, #0
	mov	r3, #0
	sc	SYS_ptrace
	bl	self_path
	str	r0, [r11, #F_ARGVX]
	lea	r0, s_leaf
	str	r0, [r11, #(F_ARGVX + 4)]
	mov	r0, #0
	str	r0, [r11, #(F_ARGVX + 8)]
	add	r0, r11, #F_PATH
	add	r1, r11, #F_ARGVX
	add	r2, r11, #(F_ARGVX + 8)
	sc	SYS_execve
	mov	r0, #111
	sc	SYS_exit_group

/* ---- VFP state through a signal frame, checked by a tracer ---- */

vfp_handler:
	add	r3, r2, #UC_REGSPACE
	ldr	r0, [r3]
	str	r0, [r11, #(F_PATH + 0)]
	ldr	r0, [r3, #4]
	str	r0, [r11, #(F_PATH + 4)]
	ldr	r0, [r3, #(8 + 3 * 8)]
	str	r0, [r11, #(F_PATH + 8)]
	ldr	r0, [r3, #(8 + 3 * 8 + 4)]
	str	r0, [r11, #(F_PATH + 12)]
	ldr	r0, [r3, #(8 + 17 * 8)]
	str	r0, [r11, #(F_PATH + 16)]
	ldr	r0, [r3, #(8 + 17 * 8 + 4)]
	str	r0, [r11, #(F_PATH + 20)]
	ldr	r0, [r3, #(8 + 32 * 8)]
	str	r0, [r11, #(F_PATH + 24)]
	movi	r0, 0xc0ffee00
	str	r0, [r3, #(8 + 3 * 8)]
	movi	r0, 0xfeedc0de
	str	r0, [r3, #(8 + 3 * 8 + 4)]
	movi	r0, 0x0badf00d
	str	r0, [r3, #(8 + 31 * 8)]
	movi	r0, 0x40000000		/* FPSCR: Z */
	str	r0, [r3, #(8 + 32 * 8)]
	bx	lr

mode_vfp:
	lea	r0, vfp_handler
	str	r0, [r11, #F_SA]
	mov	r0, #SA_SIGINFO
	str	r0, [r11, #(F_SA + 4)]
	mov	r0, #0
	str	r0, [r11, #(F_SA + 8)]
	str	r0, [r11, #(F_SA + 12)]
	str	r0, [r11, #(F_SA + 16)]
	mov	r0, #SIGUSR1
	add	r1, r11, #F_SA
	mov	r2, #0
	mov	r3, #8
	sc	SYS_rt_sigaction
	sc	SYS_getpid
	mov	r4, r0
	mov	r1, #SIGUSR1
	sc	SYS_kill
	mov	r0, r4
	mov	r1, #SIGSTOP
	sc	SYS_kill
	mov	r0, #1
	add	r1, r11, #F_PATH
	mov	r2, #28
	sc	SYS_write
	mov	r0, #0
	sc	SYS_exit_group


/* ---- SWP and SWPB (modes "swp", "swp-one", "swp-pc", "swp-tokens") ---- */

	.equ	SYS_mprotect,	125

/* With abi.swp 0 this ends in SIGILL; with 1 the swap runs and it exits 6. */
mode_swp_one:
	add	r2, r11, #F_WORK
	mov	r1, #1
	swp	r0, r1, [r2]
	mov	r0, #6
	sc	SYS_exit_group

/* The pc as Rt is UNDEFINED whatever abi.swp is; exits 6 if it ran. */
mode_swp_pc:
	add	r2, r11, #F_WORK
	mov	r1, #1
	.word	0xe102f091		/* swp pc, r1, [r2] */
	mov	r0, #6
	sc	SYS_exit_group

mode_swp:
	lea	r0, s_start
	bl	puts
	bl	t_swp
	bl	t_swp_pages
	bl	t_swp_faults
	b	summary

/* The exchange and its conditions on a stack word (F_WORK + 0..16). */
t_swp:
	push	{r4, r5, r6, lr}
	add	r4, r11, #F_WORK
	movi	r0, 0x11223344
	str	r0, [r4]
	movi	r1, 0xaabbccdd
	swp	r3, r1, [r4]
	mov	r0, r3
	eq	0x11223344, n_sw_old
	ldr	r0, [r4]
	eq	0xaabbccdd, n_sw_new
	add	r2, r4, #1
	mov	r1, #0x5a
	swpb	r3, r1, [r2]
	mov	r0, r3
	eq	0xcc, n_sw_byte_old
	ldr	r0, [r4]
	eq	0xaabb5add, n_sw_byte_new

	/* Rt == Rt2, the form CTS runs. */
	mov	r1, #9
	str	r1, [r4, #4]
	add	r2, r4, #4
	mov	r0, #7
	swp	r0, r0, [r2]
	eq	9, n_sw_same_old
	ldr	r0, [r4, #4]
	eq	7, n_sw_same_new

	/* A condition that fails changes neither Rt nor memory; one that
	 * holds swaps. */
	mov	r0, #0xaa
	str	r0, [r4, #8]
	mov	r1, #0xbb
	add	r2, r4, #8
	mov	r3, #0x77
	cmp	r1, r1			/* Z set */
	swpne	r3, r1, [r2]
	mov	r0, r3
	eq	0x77, n_sw_ne_rt
	ldr	r0, [r4, #8]
	eq	0xaa, n_sw_ne_mem
	mov	r1, #0xbb
	add	r2, r4, #8
	mov	r3, #0x77
	cmp	r1, r1
	swpeq	r3, r1, [r2]
	mov	r0, r3
	eq	0xaa, n_sw_eq_rt
	ldr	r0, [r4, #8]
	eq	0xbb, n_sw_eq_mem
	mov	r1, #0xcc
	add	r2, r4, #8
	mov	r3, #0x77
	cmp	r1, r1
	swpbne	r3, r1, [r2]
	mov	r0, r3
	eq	0x77, n_sw_bne_rt
	ldr	r0, [r4, #8]
	eq	0xbb, n_sw_bne_mem
	mov	r1, #0xcc
	add	r2, r4, #8
	mov	r3, #0x77
	cmp	r1, #0			/* Z clear, C set */
	swpbcs	r3, r1, [r2]
	mov	r0, r3
	eq	0xbb, n_sw_bcs_rt
	ldr	r0, [r4, #8]
	eq	0xcc, n_sw_bcs_mem
	pop	{r4, r5, r6, pc}

/* Pages in the states a store can meet. */
t_swp_pages:
	push	{r4, r5, r6, lr}
	mov	r0, #0
	movi	r1, 3 * 4096
	mov	r2, #3			/* PROT_READ | PROT_WRITE */
	mov	r3, #0x22		/* MAP_PRIVATE | MAP_ANONYMOUS */
	mvn	r4, #0
	mov	r5, #0
	sc	SYS_mmap2
	mov	r6, r0
	cmn	r0, #4096
	movlo	r0, #1
	movhs	r0, #0
	lea	r1, n_swp_mmap
	bl	report
	cmn	r6, #4096
	bhs	9f

	/* A page nothing touched. */
	movi	r1, 0x600dcafe
	swp	r3, r1, [r6]
	mov	r0, r3
	eq	0, n_swp_fresh_old
	ldr	r0, [r6]
	eq	0x600dcafe, n_swp_fresh_new

	/* A page the task only read: the zero page until it is written. */
	add	r4, r6, #0x1000
	ldr	r0, [r4]
	eq	0, n_swp_zero_read
	movi	r1, 0x5eed0001
	swp	r3, r1, [r4]
	mov	r0, r3
	eq	0, n_swp_zero_old
	ldr	r0, [r4]
	eq	0x5eed0001, n_swp_zero_new

	/* A byte of another fresh page: only that byte is written. */
	add	r4, r6, #0x2000
	mov	r1, #0x5a
	add	r2, r4, #3
	swpb	r3, r1, [r2]
	mov	r0, r3
	eq	0, n_swp_byte_old
	ldr	r0, [r4]
	eq	0x5a000000, n_swp_byte_only
	mov	r0, r6
	movi	r1, 3 * 4096
	sc	SYS_munmap

	/* A shared file mapping: the file sees the swap. */
	lea	r0, s_tmpfile
	movi	r1, 0x242		/* O_RDWR | O_CREAT | O_TRUNC */
	mov	r2, #0x180		/* 0600 */
	sc	SYS_open
	str	r0, [r11, #(F_WORK + 40)]
	cmp	r0, #0
	movge	r0, #1
	movlt	r0, #0
	lea	r1, n_swp_open
	bl	report
	ldr	r0, [r11, #(F_WORK + 40)]
	cmp	r0, #0
	blt	9f
	mov	r1, #0			/* ftruncate64 (fd, pad, lo, hi) */
	movi	r2, 4096
	mov	r3, #0
	sc	SYS_ftruncate64
	eq	0, n_swp_truncate
	mov	r0, #0
	movi	r1, 4096
	mov	r2, #3
	mov	r3, #1			/* MAP_SHARED */
	ldr	r4, [r11, #(F_WORK + 40)]
	mov	r5, #0
	sc	SYS_mmap2
	mov	r6, r0
	cmn	r0, #4096
	movlo	r0, #1
	movhs	r0, #0
	lea	r1, n_swp_file_map
	bl	report
	cmn	r6, #4096
	bhs	8f
	movi	r1, 0xf11e0001
	swp	r3, r1, [r6]
	mov	r0, r3
	eq	0, n_swp_file_old
	ldr	r0, [r11, #(F_WORK + 40)]	/* pread64 (fd, buf, 4, pad, lo, hi) */
	add	r1, r11, #F_FILE
	mov	r2, #4
	mov	r3, #0
	mov	r4, #0
	mov	r5, #0
	sc	SYS_pread64
	eq	4, n_swp_file_pread
	add	r0, r11, #F_FILE
	ldr	r0, [r0]
	eq	0xf11e0001, n_swp_file_data
	mov	r0, r6
	movi	r1, 4096
	sc	SYS_munmap
8:	ldr	r0, [r11, #(F_WORK + 40)]
	sc	SYS_close
	lea	r0, s_tmpfile
	sc	SYS_unlink
9:	pop	{r4, r5, r6, pc}

/* The signal an access that cannot be made raises: SIGSEGV with the pc at
 * the instruction, si_addr the address, and a zero fault address in the
 * frame; the handler steps over the instruction. */
t_swp_faults:
	push	{r4, r5, r6, lr}
	lea	r0, segv_handler
	str	r0, [r11, #F_SA]
	mov	r0, #SA_SIGINFO
	str	r0, [r11, #(F_SA + 4)]
	mov	r0, #0
	str	r0, [r11, #(F_SA + 8)]
	str	r0, [r11, #(F_SA + 12)]
	str	r0, [r11, #(F_SA + 16)]
	str	r0, [r11, #(F_WORK + 36)]
	mov	r0, #SIGSEGV
	add	r1, r11, #F_SA
	mov	r2, #0
	mov	r3, #8
	sc	SYS_rt_sigaction
	eq	0, n_swpf_sigaction

	/* An unaligned SWP: the stack word's mapping exists, so ACCERR. */
	add	r4, r11, #F_WORK
	movi	r0, 0x0badf00d
	str	r0, [r4]
	add	r2, r4, #1
	movi	r3, 0x07777777
	mov	r1, #0x33
	swp	r3, r1, [r2]
	mov	r0, r3
	eq	0x07777777, n_swpf_un_rt
	ldr	r0, [r4]
	eq	0x0badf00d, n_swpf_un_mem
	ldr	r0, [r11, #(F_WORK + 24)]
	add	r1, r4, #1
	lea	r2, n_swpf_un_addr
	bl	check_eq
	ldr	r0, [r11, #(F_WORK + 28)]
	eq	2, n_swpf_un_code
	ldr	r0, [r11, #(F_WORK + 32)]
	eq	0, n_swpf_un_frame
	ldr	r0, [r11, #(F_WORK + 36)]
	eq	1, n_swpf_un_count

	/* Above every mapping: MAPERR. */
	movi	r2, 0xfffff000
	movi	r3, 0x07777777
	mov	r1, #0x33
	swp	r3, r1, [r2]
	mov	r0, r3
	eq	0x07777777, n_swpf_hi_rt
	ldr	r0, [r11, #(F_WORK + 24)]
	eq	0xfffff000, n_swpf_hi_addr
	ldr	r0, [r11, #(F_WORK + 28)]
	eq	1, n_swpf_hi_code
	ldr	r0, [r11, #(F_WORK + 32)]
	eq	0, n_swpf_hi_frame
	ldr	r0, [r11, #(F_WORK + 36)]
	eq	2, n_swpf_hi_count

	/* Below the first mapping: arm64 Linux's find_vma gives ACCERR. */
	mov	r2, #0x10
	movi	r3, 0x07777777
	mov	r1, #0x33
	swpb	r3, r1, [r2]
	mov	r0, r3
	eq	0x07777777, n_swpf_lo_rt
	ldr	r0, [r11, #(F_WORK + 24)]
	eq	0x10, n_swpf_lo_addr
	ldr	r0, [r11, #(F_WORK + 28)]
	eq	2, n_swpf_lo_code
	ldr	r0, [r11, #(F_WORK + 36)]
	eq	3, n_swpf_lo_count

	/* A condition that fails makes no access: no fault. */
	mov	r2, #0x10
	movi	r3, 0x07777777
	mov	r1, #0x33
	cmp	r1, r1
	swpne	r3, r1, [r2]
	mov	r0, r3
	eq	0x07777777, n_swpf_cond_rt
	ldr	r0, [r11, #(F_WORK + 36)]
	eq	3, n_swpf_cond_count

	/* A read-only page: ACCERR, memory untouched; writable again, it works. */
	mov	r0, #0
	movi	r1, 4096
	mov	r2, #3
	mov	r3, #0x22
	mvn	r4, #0
	mov	r5, #0
	sc	SYS_mmap2
	mov	r6, r0
	cmn	r0, #4096
	movlo	r0, #1
	movhs	r0, #0
	lea	r1, n_swpf_mmap
	bl	report
	cmn	r6, #4096
	bhs	9f
	movi	r0, 0x600d600d
	str	r0, [r6]
	mov	r0, r6
	movi	r1, 4096
	mov	r2, #1			/* PROT_READ */
	sc	SYS_mprotect
	eq	0, n_swpf_mprotect_ro
	movi	r3, 0x07777777
	mov	r1, #0x33
	swp	r3, r1, [r6]
	mov	r0, r3
	eq	0x07777777, n_swpf_ro_rt
	ldr	r0, [r6]
	eq	0x600d600d, n_swpf_ro_mem
	ldr	r0, [r11, #(F_WORK + 24)]
	mov	r1, r6
	lea	r2, n_swpf_ro_addr
	bl	check_eq
	ldr	r0, [r11, #(F_WORK + 28)]
	eq	2, n_swpf_ro_code
	ldr	r0, [r11, #(F_WORK + 36)]
	eq	4, n_swpf_ro_count
	mov	r0, r6
	movi	r1, 4096
	mov	r2, #3
	sc	SYS_mprotect
	eq	0, n_swpf_mprotect_rw
	mov	r1, #0x33
	swp	r3, r1, [r6]
	mov	r0, r3
	eq	0x600d600d, n_swpf_rw_old
	ldr	r0, [r6]
	eq	0x33, n_swpf_rw_new
	ldr	r0, [r11, #(F_WORK + 36)]
	eq	4, n_swpf_rw_count
9:	pop	{r4, r5, r6, pc}

/* Swaps beside a 64-bit task (aarch64_dirty.s, mode "tokens"). Descriptor 9
 * is a file page both map shared:
 *
 *   +0  word slot    +4  ready count    +8  byte slot (a word of 0..255)
 *   +16 this task's word  +20 its byte    +24 the 64-bit task's word
 *   +28 words this task saw (OR)  +32 bytes it saw  +36 its swaps
 *   +40 done flag    +44 the 64-bit task's byte  +48 its swaps
 *
 * Three tokens, 1 (here), 2 (there) and 4 (the slot), move between the
 * holders, so the three always hold a permutation of them unless a swap
 * tore. This task swaps the word with SWP and the byte with SWPB until it
 * has seen all three tokens in each slot, at least SWP_MIN_ROUNDS and at
 * most SWP_MAX_ROUNDS times; the other swaps with LDAXR/STLXR meanwhile.
 * Exits 0, 98 if the other never came, 99 if the page did not map. */
	.equ	SWP_MIN_ROUNDS,	20000
	.equ	SWP_MAX_ROUNDS,	400000

mode_swp_tokens:
	mov	r0, #0
	movi	r1, 4096
	mov	r2, #3
	mov	r3, #1			/* MAP_SHARED */
	mov	r4, #9
	mov	r5, #0
	sc	SYS_mmap2
	cmn	r0, #4096
	movhs	r0, #99
	bhs	9f
	mov	r6, r0
	add	r3, r6, #4		/* ready: wait for both tasks */
1:	ldrex	r1, [r3]
	add	r1, r1, #1
	strex	r2, r1, [r3]
	cmp	r2, #0
	bne	1b
	movi	r5, 200000
2:	ldr	r1, [r6, #4]
	cmp	r1, #2
	bhs	3f
	sc	SYS_sched_yield
	subs	r5, r5, #1
	bne	2b
	mov	r0, #98
	b	9f
3:	mov	r4, #1			/* the word token */
	mov	r5, #1			/* the byte token */
	mov	r8, #0			/* the word tokens seen */
	mov	r9, #0			/* the byte tokens seen */
	mov	r10, #0			/* the rounds */
	add	r12, r6, #8
4:	swp	r0, r4, [r6]
	orr	r8, r8, r0
	mov	r4, r0
	swpb	r0, r5, [r12]
	orr	r9, r9, r0
	mov	r5, r0
	add	r10, r10, #1
	movi	r1, SWP_MAX_ROUNDS
	cmp	r10, r1
	bhs	5f
	movi	r1, SWP_MIN_ROUNDS
	cmp	r10, r1
	blo	4b
	and	r0, r8, r9
	cmp	r0, #7
	bne	4b
5:	str	r4, [r6, #16]
	str	r5, [r6, #20]
	str	r8, [r6, #28]
	str	r9, [r6, #32]
	str	r10, [r6, #36]
	dmb	ish
	mov	r1, #1
	str	r1, [r6, #40]
	mov	r0, #0
9:	sc	SYS_exit_group

/* ---- exclusives after 64-bit code (mode "exclusives") ---- */

/* A 64-bit task (aarch64_dirty.s) set x15-x30 when it executed this
 * program, and another one on the same CPU sets them again before each of
 * its sched_yield calls, so every return from one of ours enters with them
 * set. Each of XROUNDS rounds takes a fresh page of an anonymous mapping:
 * the Thumb half reads it with a plain LDR first, so the first exclusive
 * load meets the read-only zero page and the store the write fault, as a
 * fresh .bss page does; then every exclusive and ordered form adds one to a
 * word of its own, each after a sched_yield. Exits with the number of
 * words that did not come out 1; a fault ends it in SIGSEGV. */
	.equ	XROUNDS,	64

mode_exclusives:
	mov	r0, #0
	movi	r1, XROUNDS * 4096
	mov	r2, #3			/* PROT_READ | PROT_WRITE */
	mov	r3, #0x22		/* MAP_PRIVATE | MAP_ANONYMOUS */
	mvn	r4, #0
	mov	r5, #0
	sc	SYS_mmap2
	cmn	r0, #4096
	movhi	r0, #99
	bhi	9f
	mov	r6, r0			/* the mapping */
	mov	r10, #0			/* the round */
	mov	r8, #0			/* failures */
1:	add	r4, r6, r10, lsl #12
	lea	r0, x_thumb
	orr	r0, r0, #1
	blx	r0
	add	r8, r8, r0
	add	r4, r6, r10, lsl #12
	add	r4, r4, #0x800
	bl	x_arm
	add	r8, r8, r0
	add	r10, r10, #1
	cmp	r10, #XROUNDS
	blt	1b
	lea	r0, s_x_line
	bl	puts
	mov	r0, r8
	bl	putdec
	lea	r0, s_summary_b
	bl	puts
	mov	r0, #(XROUNDS * 20)
	bl	putdec
	lea	r0, s_summary_c
	bl	puts
	mov	r0, r8
	cmp	r0, #100
	movgt	r0, #100
9:	sc	SYS_exit_group

	.arch	armv8-a

/* One round's Thumb half, on the page at r4: returns its failures in r0.
 * r6 counts them, r5 the attempts of one sequence, r7 is the system call. */
.macro t_yield
	movs	r7, #SYS_sched_yield
	svc	#0
.endm

/* The word at r3 is 1, or count a failure. */
.macro t_expect_one
	ldr	r1, [r3]
	cmp	r1, #1
	it	ne
	addne	r6, r6, #1
.endm

/* LDREX and STREX with an offset: the base stays in r4. */
.macro t_offset off
	t_yield
	ldr	r1, [r4, #\off]
	movs	r5, #0
1:	ldrex	r1, [r4, #\off]
	adds	r1, r1, #1
	strex	r2, r1, [r4, #\off]
	adds	r5, r5, #1
	cmp	r2, #0
	beq	2f
	cmp	r5, #255
	blo	1b
2:	add	r3, r4, #\off
	t_expect_one
.endm

/* A load and store exclusive pair of one register at r4 + off. */
.macro t_single load, store, off
	t_yield
	add	r3, r4, #\off
	movs	r5, #0
1:	\load	r1, [r3]
	adds	r1, r1, #1
	\store	r2, r1, [r3]
	adds	r5, r5, #1
	cmp	r2, #0
	beq	2f
	cmp	r5, #255
	blo	1b
2:	t_expect_one
.endm

.macro t_double load, store, off
	t_yield
	add	r3, r4, #\off
	movs	r5, #0
1:	\load	r0, r1, [r3]
	adds	r0, r0, #1
	\store	r2, r0, r1, [r3]
	adds	r5, r5, #1
	cmp	r2, #0
	beq	2f
	cmp	r5, #255
	blo	1b
2:	t_expect_one
.endm

	.thumb
	.thumb_func
x_thumb:
	push	{r4, r5, r6, r7, lr}
	movs	r6, #0
	t_offset 0x40
	t_offset 0x3fc
	t_offset 0
	t_single ldaex, stlex, 0x100
	t_single ldrexb, strexb, 0x104
	t_single ldrexh, strexh, 0x108
	t_single ldaexb, stlexb, 0x10c
	t_single ldaexh, stlexh, 0x110
	t_double ldrexd, strexd, 0x118
	t_double ldaexd, stlexd, 0x120
	t_yield
	add	r3, r4, #0x128
	movs	r1, #1
	stl	r1, [r3]
	lda	r1, [r3]
	add	r3, r4, #0x12c
	stl	r1, [r3]
	t_expect_one
	mov	r0, r6
	pop	{r4, r5, r6, r7, pc}

	.arm
	.p2align 2

/* One round's ARM half, on r4 = the page + 0x800: the same, with no offset
 * form, which A32 does not have. */
.macro a_yield
	mov	r7, #SYS_sched_yield
	svc	#0
.endm

.macro a_expect_one
	ldr	r1, [r3]
	cmp	r1, #1
	addne	r6, r6, #1
.endm

.macro a_single load, store, off
	a_yield
	add	r3, r4, #\off
	mov	r5, #0
1:	\load	r1, [r3]
	add	r1, r1, #1
	\store	r2, r1, [r3]
	add	r5, r5, #1
	cmp	r2, #0
	beq	2f
	cmp	r5, #255
	blo	1b
2:	a_expect_one
.endm

.macro a_double load, store, off
	a_yield
	add	r3, r4, #\off
	mov	r5, #0
1:	\load	r0, r1, [r3]
	add	r0, r0, #1
	\store	r2, r0, r1, [r3]
	add	r5, r5, #1
	cmp	r2, #0
	beq	2f
	cmp	r5, #255
	blo	1b
2:	a_expect_one
.endm

x_arm:
	push	{r4, r5, r6, r7, lr}
	mov	r6, #0
	a_single ldrex, strex, 0
	a_single ldaex, stlex, 0x8
	a_single ldrexb, strexb, 0x10
	a_single ldrexh, strexh, 0x14
	a_single ldaexb, stlexb, 0x18
	a_single ldaexh, stlexh, 0x1c
	a_double ldrexd, strexd, 0x20
	a_double ldaexd, stlexd, 0x28
	a_yield
	add	r3, r4, #0x30
	mov	r1, #1
	stl	r1, [r3]
	lda	r1, [r3]
	add	r3, r4, #0x34
	stl	r1, [r3]
	a_expect_one
	mov	r0, r6
	pop	{r4, r5, r6, r7, pc}

	.arch	armv7-a


/* ---- strings ---- */

	.balign	4
	str_	s_empty, ""
	str_	s_nl, "\n"
	str_	s_start, "a32-probe: start\n"
	str_	s_pass, "PASS "
	str_	s_fail, "FAIL "
	str_	s_got, ": got "
	str_	s_want, " want "
	str_	s_summary_a, "a32-probe: "
	str_	s_summary_b, " failure(s) in "
	str_	s_summary_c, " check(s)\n"
	str_	s_leaf, "leaf"
	str_	s_trace, "trace"
	str_	s_leaf_line, "a32-probe: leaf\n"
	str_	s_v8l, "v8l"
	str_	s_armv8l, "armv8l"
	str_	s_aarch64, "aarch64"
	str_	s_model, "ARMv8 Processor rev "
	str_	s_v8l_paren, "(v8l)\n"
	str_	s_features, "Features\t:"
	str_	s_cpuinfo, "/proc/cpuinfo"
	str_	s_tmpfile, "/tmp/a32-probe.dat"
	str_	s_self, "/proc/self/exe"
	str_	s_bin_sh, "/bin/sh"
	str_	s_sh_arg0, "sh"
	str_	s_dash_c, "-c"
	str_	s_exec_leaf, "exec \"$0\" leaf"
	str_	s_pre_sigpage, "sigpage: "
	str_	s_pre_restorer, "SA_RESTORER: "
	str_	s_hello, "a32-probe: hello from AArch32\n"
	str_	n_write, "write"
	str_	n_pagesz, "AT_PAGESZ is 4 KiB"
	str_	n_hwcap, "AT_HWCAP is not zero"
	str_	n_platform, "AT_PLATFORM is v8l"
	str_	n_tpidruro, "set_tls sets TPIDRURO"
	str_	n_tpidrurw, "TPIDRURW holds its value"
	str_	n_tpidruro_kept, "TPIDRURW leaves TPIDRURO alone"
	str_	n_get_tls, "kuser get_tls"
	str_	n_kuser_version, "kuser helper version"
	str_	n_cmpxchg_ok, "kuser cmpxchg succeeds"
	str_	n_cmpxchg_stored, "kuser cmpxchg stores"
	str_	n_cmpxchg_fail, "kuser cmpxchg fails on a mismatch"
	str_	n_cmpxchg_kept, "kuser cmpxchg keeps the word on a mismatch"
	str_	n_cmpxchg64_ok, "kuser cmpxchg64 succeeds"
	str_	n_cmpxchg64_stored, "kuser cmpxchg64 stores"
	str_	n_sigaction, "rt_sigaction"
	str_	n_handler_ran, "the handler ran once"
	str_	n_signo, "the handler gets the signal number"
	str_	n_si_pid, "siginfo si_pid"
	str_	n_si_code, "siginfo si_code is SI_USER"
	str_	n_vfp_magic, "the frame holds the VFP context"
	str_	n_frame_cpsr, "the frame's CPSR is ARM user mode"
	str_	n_blocked, "the signal is blocked in its handler"
	str_	n_restorer, "the handler returns through sa_restorer"
	str_	n_sigpage, "the handler returns through the sigpage"
	str_	n_d8, "d8 restored"
	str_	n_d16, "d16 restored"
	str_	n_d31, "d31 restored"
	str_	n_fpscr, "FPSCR rounding mode restored"
	str_	n_flags, "NZCVQ restored"
	str_	n_unblocked, "the mask is restored"
	str_	n_mmap_rwx, "mmap of an RWX page"
	str_	n_cacheflush, "cacheflush returns 0"
	str_	n_cf_arm, "cacheflush: ARM code runs"
	str_	n_cf_arm_rewrite, "cacheflush: rewritten ARM code runs"
	str_	n_cf_thumb, "cacheflush: Thumb code runs"
	str_	n_cf_thumb_rewrite, "cacheflush: rewritten Thumb code runs"
	str_	n_thumb_itstate, "ITSTATE is advanced after SVC"
	str_	n_thumb_svc_result, "SVC in an IT block returns"
	str_	n_open, "open"
	str_	n_pread64, "pread64"
	str_	n_pread64_data, "pread64 reads the file offset"
	str_	n_mmap2, "mmap2"
	str_	n_mmap2_data, "mmap2 offset is in 4 KiB units"
	str_	n_mmap2_unit, "mmap2 maps a 4 KiB offset"
	str_	n_ftruncate64, "ftruncate64"
	str_	n_fstat64, "fstat64"
	str_	n_st_size_lo, "stat64 st_size"
	str_	n_st_size_hi, "stat64 st_size high word"
	str_	n_per_linux32, "personality(PER_LINUX32)"
	str_	n_uname, "uname"
	str_	n_uname_armv8l, "uname is armv8l under PER_LINUX32"
	str_	n_cpuinfo_read, "read /proc/cpuinfo"
	str_	n_cpuinfo_model, "cpuinfo has a model name"
	str_	n_cpuinfo_v8l, "cpuinfo names the v8l platform"
	str_	n_cpuinfo_features, "cpuinfo has compat features"
	str_	n_per_linux_prev, "personality(PER_LINUX) returns PER_LINUX32"
	str_	n_uname_aarch64, "uname is aarch64 again"
	str_	n_fork, "fork"
	str_	n_child_stopped, "the child stops on SIGSTOP"
	str_	n_nanosleep_restarted, "a stopped nanosleep completes"
	str_	n_exec_chain, "execve 32 -> 64 -> 32"
	str_	s_extra, "extra"
	str_	s_tracer, "tracer"
	str_	n_c3_clone3, "clone3 returns the tid"
	str_	n_c3_tls, "clone3: CLONE_SETTLS sets TPIDRURO"
	str_	n_c3_gettid, "clone3: gettid in the child"
	str_	n_c3_sp, "clone3: the child ran on its stack"
	str_	n_reg_rseq, "rseq registers"
	str_	n_reg_rseq_cpu, "rseq cpu_id is set"
	str_	n_reg_rseq_unreg, "rseq unregisters"
	str_	n_reg_set_robust, "set_robust_list"
	str_	n_reg_get_robust, "get_robust_list"
	str_	n_reg_robust_len, "get_robust_list reports 12 bytes"
	str_	n_alt_sigaltstack, "sigaltstack"
	str_	n_alt_sigaction, "sigaltstack: rt_sigaction"
	str_	n_alt_on_stack, "the handler ran on the alternate stack"
	str_	n_vf_pid, "vfork returns the child's pid"
	str_	n_vf_status, "vfork + execve in the child"
	str_	n_tm_sigaction, "timer: rt_sigaction"
	str_	n_tm_setitimer, "timer: setitimer"
	str_	n_tm_fired, "timer: SIGALRM reached a running task"
	str_	n_sec_nnp, "seccomp: no_new_privs"
	str_	n_sec_filter_arch, "seccomp: install the arch filter"
	str_	n_sec_arch_ok, "seccomp: the task's arch is AUDIT_ARCH_ARM"
	str_	n_sec_filter_nr, "seccomp: install the nr filter"
	str_	n_sec_getpid_denied, "seccomp: getpid (20) is denied"
	str_	n_sec_gettid_ok, "seccomp: gettid (224) is allowed"
	str_	s_root, "/"
	str_	n_sc_pipe2, "syscalls: pipe2"
	str_	n_sc_pipe_write, "syscalls: write to the pipe"
	str_	n_sc_ppoll, "syscalls: ppoll_time64 sees the pipe"
	str_	n_sc_revents, "syscalls: ppoll revents is POLLIN"
	str_	n_sc_pipe_read, "syscalls: read from the pipe"
	str_	n_sc_pipe_byte, "syscalls: the byte round-trips"
	str_	n_sc_clock_gettime64, "syscalls: clock_gettime64"
	str_	n_sc_clock_nsec, "syscalls: clock_gettime64 nanoseconds"
	str_	n_sc_clock_hi, "syscalls: clock_gettime64 high word"
	str_	n_sc_getrandom, "syscalls: getrandom"
	str_	n_sc_socketpair, "syscalls: socketpair"
	str_	n_sc_sendmsg, "syscalls: sendmsg with SCM_RIGHTS"
	str_	n_sc_recvmsg, "syscalls: recvmsg"
	str_	n_sc_controllen, "syscalls: msg_controllen"
	str_	n_sc_cmsg_len, "syscalls: cmsg_len"
	str_	n_sc_cmsg_level, "syscalls: cmsg_level"
	str_	n_sc_cmsg_type, "syscalls: cmsg_type"
	str_	n_sc_passed_fd, "syscalls: the passed descriptor works"
	str_	n_sc_statx, "syscalls: statx"
	str_	n_sc_statx_dir, "syscalls: statx mode of /"
	str_	n_sc_open_dir, "syscalls: open O_DIRECTORY"
	str_	n_sc_getdents64, "syscalls: getdents64"
	str_	s_vfp, "vfp"
	str_	s_exclusives, "exclusives"
	str_	s_swp, "swp"
	str_	s_swp_one, "swp-one"
	str_	s_swp_pc, "swp-pc"
	str_	s_swp_tokens, "swp-tokens"
	str_	n_sw_old, "swp: Rt gets the old word"
	str_	n_sw_new, "swp: the word is Rt2"
	str_	n_sw_byte_old, "swpb: Rt gets the old byte"
	str_	n_sw_byte_new, "swpb: only the byte changes"
	str_	n_sw_same_old, "swp r0, r0: Rt gets the old word"
	str_	n_sw_same_new, "swp r0, r0: the word is the old r0"
	str_	n_sw_ne_rt, "swpne with Z set leaves Rt"
	str_	n_sw_ne_mem, "swpne with Z set leaves the word"
	str_	n_sw_eq_rt, "swpeq with Z set takes the old word"
	str_	n_sw_eq_mem, "swpeq with Z set stores"
	str_	n_sw_bne_rt, "swpbne with Z set leaves Rt"
	str_	n_sw_bne_mem, "swpbne with Z set leaves the byte"
	str_	n_sw_bcs_rt, "swpbcs with C set takes the old byte"
	str_	n_sw_bcs_mem, "swpbcs with C set stores"
	str_	n_swp_mmap, "swp: mmap2 of three pages"
	str_	n_swp_fresh_old, "swp: a page nothing touched reads 0"
	str_	n_swp_fresh_new, "swp: the store reaches a page nothing touched"
	str_	n_swp_zero_read, "swp: a page the task read is the zero page"
	str_	n_swp_zero_old, "swp: the zero page reads 0"
	str_	n_swp_zero_new, "swp: the store replaces the zero page"
	str_	n_swp_byte_old, "swpb: a fresh page's byte reads 0"
	str_	n_swp_byte_only, "swpb: only one byte of the fresh page changes"
	str_	n_swp_open, "swp: open the file"
	str_	n_swp_truncate, "swp: ftruncate64 of the file"
	str_	n_swp_file_map, "swp: mmap2 of the file, shared"
	str_	n_swp_file_old, "swp: the file page reads 0"
	str_	n_swp_file_pread, "swp: pread64 of the file"
	str_	n_swp_file_data, "swp: the file holds the stored word"
	str_	n_swpf_sigaction, "swp faults: rt_sigaction"
	str_	n_swpf_un_rt, "unaligned swp leaves Rt"
	str_	n_swpf_un_mem, "unaligned swp leaves the word"
	str_	n_swpf_un_addr, "unaligned swp: si_addr is the address"
	str_	n_swpf_un_code, "unaligned swp: si_code is SEGV_ACCERR"
	str_	n_swpf_un_frame, "unaligned swp: fault_address in the frame is 0"
	str_	n_swpf_un_count, "unaligned swp: SIGSEGV once"
	str_	n_swpf_hi_rt, "swp above every mapping leaves Rt"
	str_	n_swpf_hi_addr, "swp above every mapping: si_addr"
	str_	n_swpf_hi_code, "swp above every mapping: SEGV_MAPERR"
	str_	n_swpf_hi_frame, "swp above every mapping: fault_address in the frame is 0"
	str_	n_swpf_hi_count, "swp above every mapping: SIGSEGV again"
	str_	n_swpf_lo_rt, "swpb below the first mapping leaves Rt"
	str_	n_swpf_lo_addr, "swpb below the first mapping: si_addr"
	str_	n_swpf_lo_code, "swpb below the first mapping: SEGV_ACCERR"
	str_	n_swpf_lo_count, "swpb below the first mapping: SIGSEGV again"
	str_	n_swpf_cond_rt, "swpne at a bad address leaves Rt"
	str_	n_swpf_cond_count, "swpne at a bad address raises no signal"
	str_	n_swpf_mmap, "swp faults: mmap2 of a page"
	str_	n_swpf_mprotect_ro, "swp faults: mprotect PROT_READ"
	str_	n_swpf_ro_rt, "swp to a read-only page leaves Rt"
	str_	n_swpf_ro_mem, "swp to a read-only page leaves the word"
	str_	n_swpf_ro_addr, "swp to a read-only page: si_addr"
	str_	n_swpf_ro_code, "swp to a read-only page: SEGV_ACCERR"
	str_	n_swpf_ro_count, "swp to a read-only page: SIGSEGV again"
	str_	n_swpf_mprotect_rw, "swp faults: mprotect PROT_READ|PROT_WRITE"
	str_	n_swpf_rw_old, "swp to the page once writable: old word"
	str_	n_swpf_rw_new, "swp to the page once writable: new word"
	str_	n_swpf_rw_count, "swp to the page once writable: no signal"
	str_	s_x_line, "a32-probe: exclusives: "
	str_	n_tr_exec_stop, "tracer: exec stop with SIGTRAP"
	str_	n_tr_getregs, "tracer: PTRACE_GETREGS"
	str_	n_tr_pc, "tracer: pc is the entry"
	str_	n_tr_cpsr, "tracer: cpsr is user mode"
	str_	n_tr_getvfp, "tracer: PTRACE_GETVFPREGS"
	str_	n_tr_thread_area, "tracer: PTRACE_GET_THREAD_AREA"
	str_	n_tr_peekusr, "tracer: PTRACE_PEEKUSR text address"
	str_	n_tr_text_addr, "tracer: the text address is 0x10000"
	str_	n_tr_peekusr_pc, "tracer: PEEKUSR of pc"
	str_	n_tr_setregs, "tracer: PTRACE_SETREGS"
	str_	n_tr_setregs_took, "tracer: SETREGS took"
	str_	n_tr_syscall_stop, "tracer: PTRACE_SYSCALL stops"
	str_	n_tr_syscall_nr, "tracer: r7 holds the syscall number"
	str_	n_tr_set_syscall, "tracer: PTRACE_SET_SYSCALL"
	str_	n_tr_exit_stop, "tracer: the exit stop"
	str_	n_tr_replaced, "tracer: the replaced call (getpid) ran"
	str_	n_tr_exit_status, "tracer: the tracee exits 37"
	str_	n_th_stack, "thread: mmap of a stack"
	str_	n_th_clone, "thread: clone returns the tid"
	str_	n_th_ptid, "thread: CLONE_PARENT_SETTID stores the tid"
	str_	n_th_tls, "thread: CLONE_SETTLS sets TPIDRURO"
	str_	n_th_gettid, "thread: gettid in the child"
	str_	n_th_parent_tls, "thread: the parent's TPIDRURO is kept"
	str_	n_th_sp, "thread: the child ran on its stack"
	str_	n_sv_sigaction, "segv: rt_sigaction"
	str_	n_sv_r4_kept, "segv: registers survive the handler"
	str_	n_sv_si_addr, "segv: si_addr is the address"
	str_	n_sv_si_code, "segv: si_code is SEGV_MAPERR"
	str_	n_sv_fault_address, "segv: fault_address in the frame"
	str_	n_sv_si_addr_high, "segv: si_addr near the top of the 32-bit space"
	str_	n_sv_count, "segv: handler ran twice"
	str_	n_stack_grow, "the stack grows by a megabyte"
	str_	n_brk_grow, "brk grows"
	str_	n_brk_data, "brk memory works"
	str_	n_mm_64m, "mmap2 of 64 MiB"
	str_	n_mm_below_vectors, "mmap2 lands below the vectors"
	str_	n_mm_3g_fits, "a 3 GiB mapping ends below the vectors"
	str_	n_mm_3g_enomem, "a 3 GiB mapping is refused with ENOMEM"
	str_	s_segv, "segv"
	str_	s_undef, "undef"
	str_	s_bkpt, "bkpt"
