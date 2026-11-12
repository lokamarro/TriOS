/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux probe for switching one vCPU between address
 * spaces.
 *
 * A parent and a forked child pinned to the same CPU pass one byte back and
 * forth over two pipes, so every hand-off is a context switch into the other
 * process's address space on one vCPU. The vCPU's software TLB holds one
 * address space at a time, so each switch flushes it, and the process that
 * resumes then refills it for the pages it touches. Each side reads
 * TOUCH_PAGES pages of its own copy of a region after every hand-off, the
 * small working set a shell, a loader or a pipeline stage has between
 * blocking calls.
 *
 * After the fork each process rewrites its copy with its own values, and every
 * touch checks and stores its page, so a run that reports OK has shown that no
 * stale read or write translation from the other process survived a switch.
 */

	.equ NR_PIPE2,			59
	.equ NR_READ,			63
	.equ NR_WRITE,			64
	.equ NR_EXIT,			93
	.equ NR_EXIT_GROUP,		94
	.equ NR_SCHED_SETAFFINITY,	122
	.equ NR_CLONE,			220
	.equ NR_WAIT4,			260
	.equ SIGCHLD,			17

#ifndef BENCH_ROUNDS
#define BENCH_ROUNDS 40000
#endif
#ifndef BENCH_TOUCH_PAGES
#define BENCH_TOUCH_PAGES 32
#endif
	.equ ROUNDS,		BENCH_ROUNDS
	.equ TOUCH_PAGES,	BENCH_TOUCH_PAGES
	.equ GUEST_PAGE_SIZE,	4096

#include "probe_harness.inc"

	probe_start
	/* Seed each page with its index before the fork, so both processes
	 * check the same values through their own mappings. */
	adrp	x0, region
	add	x0, x0, :lo12:region
	mov	x1, #0
	mov	x3, #GUEST_PAGE_SIZE
.Lseed:
	str	x1, [x0]
	add	x0, x0, x3
	add	x1, x1, #1
	cmp	x1, #TOUCH_PAGES
	b.lt	.Lseed

	/* Pin to CPU 0 so the two processes share one vCPU. */
	mov	x0, #0
	mov	x1, #8
	adrp	x2, cpu_mask
	add	x2, x2, :lo12:cpu_mask
	mov	x8, #NR_SCHED_SETAFFINITY
	svc	#0
	cmp	x0, #0
	b.lt	.Lfailure

	adrp	x0, to_child
	add	x0, x0, :lo12:to_child
	mov	x1, #0
	mov	x8, #NR_PIPE2
	svc	#0
	cbnz	x0, .Lfailure
	adrp	x0, to_parent
	add	x0, x0, :lo12:to_parent
	mov	x1, #0
	mov	x8, #NR_PIPE2
	svc	#0
	cbnz	x0, .Lfailure

	/* fork(): clone(SIGCHLD) with no new stack. */
	mov	x0, #SIGCHLD
	mov	x1, #0
	mov	x2, #0
	mov	x3, #0
	mov	x4, #0
	mov	x8, #NR_CLONE
	svc	#0
	cmp	x0, #0
	b.lt	.Lfailure
	b.eq	.Lchild

	/* Parent: send, wait for the reply, touch its working set. */
	mov	x26, #0				/* the parent's values */
	adrp	x20, to_child
	add	x20, x20, :lo12:to_child
	ldr	w21, [x20, #4]			/* write end towards the child */
	adrp	x20, to_parent
	add	x20, x20, :lo12:to_parent
	ldr	w22, [x20]			/* read end from the child */
	movz	x19, #(ROUNDS & 0xffff)
	movk	x19, #((ROUNDS >> 16) & 0xffff), lsl #16
.Lparent_round:
	mov	w0, w21
	bl	.Lsend
	cbnz	x0, .Lfailure
	mov	w0, w22
	bl	.Lreceive
	cbnz	x0, .Lfailure
	bl	.Ltouch
	cbnz	x0, .Lfailure
	subs	x19, x19, #1
	b.ne	.Lparent_round

	mov	x0, #-1
	add	x1, sp, #0
	mov	x2, #0
	mov	x3, #0
	mov	x8, #NR_WAIT4
	svc	#0
	cmp	x0, #0
	b.lt	.Lfailure
	ldr	w4, [sp]
	cbnz	w4, .Lfailure

	probe_finish

	/* Child: wait for the parent, touch its working set, reply. */
.Lchild:
	mov	x26, #(1 << 32)			/* the child's values */
	bl	.Lrewrite
	adrp	x20, to_child
	add	x20, x20, :lo12:to_child
	ldr	w21, [x20]			/* read end from the parent */
	adrp	x20, to_parent
	add	x20, x20, :lo12:to_parent
	ldr	w22, [x20, #4]			/* write end towards the parent */
	movz	x19, #(ROUNDS & 0xffff)
	movk	x19, #((ROUNDS >> 16) & 0xffff), lsl #16
.Lchild_round:
	mov	w0, w21
	bl	.Lreceive
	cbnz	x0, .Lchild_failed
	bl	.Ltouch
	cbnz	x0, .Lchild_failed
	mov	w0, w22
	bl	.Lsend
	cbnz	x0, .Lchild_failed
	subs	x19, x19, #1
	b.ne	.Lchild_round
	mov	x0, #0
	mov	x8, #NR_EXIT
	svc	#0
	brk	#0
.Lchild_failed:
	mov	x0, #1
	mov	x8, #NR_EXIT
	svc	#0
	brk	#0

	/* Write one byte to fd w0; returns 0 on success. */
	.type .Lsend, %function
.Lsend:
	adrp	x1, token
	add	x1, x1, :lo12:token
	mov	x2, #1
	mov	x8, #NR_WRITE
	svc	#0
	sub	x0, x0, #1
	ret
	.size .Lsend, .-.Lsend

	/* Read one byte from fd w0; returns 0 on success. */
	.type .Lreceive, %function
.Lreceive:
	adrp	x1, token
	add	x1, x1, :lo12:token
	mov	x2, #1
	mov	x8, #NR_READ
	svc	#0
	sub	x0, x0, #1
	ret
	.size .Lreceive, .-.Lreceive

	/* Give this process's copy of each page its own value, index + x26. */
	.type .Lrewrite, %function
.Lrewrite:
	adrp	x0, region
	add	x0, x0, :lo12:region
	mov	x1, #0
	mov	x3, #GUEST_PAGE_SIZE
.Lrewrite_page:
	add	x2, x1, x26
	str	x2, [x0]
	add	x0, x0, x3
	add	x1, x1, #1
	cmp	x1, #TOUCH_PAGES
	b.lt	.Lrewrite_page
	ret
	.size .Lrewrite, .-.Lrewrite

	/* Check and store back the first word of each working-set page;
	 * returns 0 if every page holds this process's value, index + x26. */
	.type .Ltouch, %function
.Ltouch:
	adrp	x0, region
	add	x0, x0, :lo12:region
	mov	x1, #0
	mov	x3, #GUEST_PAGE_SIZE
.Ltouch_page:
	ldr	x2, [x0]
	add	x4, x1, x26
	cmp	x2, x4
	b.ne	.Ltouch_failed
	str	x2, [x0]
	add	x0, x0, x3
	add	x1, x1, #1
	cmp	x1, #TOUCH_PAGES
	b.lt	.Ltouch_page
	mov	x0, #0
	ret
.Ltouch_failed:
	mov	x0, #1
	ret
	.size .Ltouch, .-.Ltouch

	probe_end

	.section .data
	.balign 8
cpu_mask:
	.quad 1
to_child:
	.word -1, -1
to_parent:
	.word -1, -1
token:
	.byte 0

	.section .bss
	.p2align 14
region:
	.space TOUCH_PAGES * GUEST_PAGE_SIZE
