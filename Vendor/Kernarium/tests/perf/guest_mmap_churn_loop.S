/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux probe for mapping churn: an allocator that maps a
 * region, touches it and gives it back.
 *
 * Each round maps REGION_PAGES anonymous pages, writes the first word of
 * each (one fault per page, which publishes the page to the host mapping),
 * checks them, and unmaps the region, which removes every published page
 * again and makes each vCPU's software TLB drop the range at its next
 * dispatch boundary. Loaders and allocators do this constantly: scudo and
 * jemalloc return large blocks with munmap, and every dlopen maps and
 * unmaps segments.
 *
 * With BENCH_SPINNERS > 0, that many threads sharing the address space spin
 * on guest code until the churn ends, so every unmap also has to stop them:
 * the multithreaded process an application is.
 */

	.equ NR_WRITE,		64
	.equ NR_EXIT,		93
	.equ NR_EXIT_GROUP,	94
	.equ NR_CLONE,		220
	.equ NR_WAIT4,		260
	.equ NR_MUNMAP,		215
	.equ NR_MMAP,		222

	.equ PROT_READ_WRITE,	3
	.equ MAP_PRIVATE_ANON,	0x22
	/* Share the address space but stay an ordinary child so wait4 reaps it. */
	.equ CLONE_VM_SIGCHLD,	0x111

#ifndef BENCH_ROUNDS
#define BENCH_ROUNDS 4000
#endif
#ifndef BENCH_REGION_PAGES
#define BENCH_REGION_PAGES 64
#endif
#ifndef BENCH_SPINNERS
#define BENCH_SPINNERS 0
#endif
	.equ ROUNDS,		BENCH_ROUNDS
	.equ REGION_PAGES,	BENCH_REGION_PAGES
	.equ GUEST_PAGE_SIZE,	4096
	.equ REGION_BYTES,	REGION_PAGES * GUEST_PAGE_SIZE
	.equ SPINNERS,		BENCH_SPINNERS
	.equ STACK_SIZE,	16384

#include "probe_harness.inc"

	probe_start
	mov	x21, #0				/* spinners started */
.Lspawn:
	cmp	x21, #SPINNERS
	b.ge	.Lchurn
	adrp	x1, stacks
	add	x1, x1, :lo12:stacks
	add	x22, x21, #1
	mov	x2, #STACK_SIZE
	madd	x1, x22, x2, x1			/* this spinner's stack top */
	mov	x0, #CLONE_VM_SIGCHLD
	mov	x2, #0
	mov	x3, #0
	mov	x4, #0
	mov	x8, #NR_CLONE
	svc	#0
	cmp	x0, #0
	b.lt	.Lfailure
	b.eq	.Lspin
	add	x21, x21, #1
	b	.Lspawn

	/* Spinners run guest code until the churn publishes its end. */
.Lspin:
	adrp	x9, finished
	add	x9, x9, :lo12:finished
	mov	x10, #0
.Lspin_loop:
	add	x10, x10, #1
	eor	x11, x10, x10, lsr #3
	add	x10, x10, x11, lsr #60
	ldr	w12, [x9]
	cbz	w12, .Lspin_loop
	mov	x0, #0
	mov	x8, #NR_EXIT
	svc	#0
	brk	#0

.Lchurn:
	movz	x19, #(ROUNDS & 0xffff)
	movk	x19, #((ROUNDS >> 16) & 0xffff), lsl #16
	mov	x23, #GUEST_PAGE_SIZE
.Lround:
	mov	x0, #0
	movz	x1, #(REGION_BYTES & 0xffff)
	movk	x1, #((REGION_BYTES >> 16) & 0xffff), lsl #16
	mov	x2, #PROT_READ_WRITE
	mov	x3, #MAP_PRIVATE_ANON
	mov	x4, #-1
	mov	x5, #0
	mov	x8, #NR_MMAP
	svc	#0
	cmn	x0, #4095
	b.hs	.Lfailure
	mov	x20, x0				/* region */

	/* Touch every page, then read each back. */
	mov	x0, x20
	mov	x1, #0
.Lwrite_page:
	add	x2, x1, x19
	str	x2, [x0]
	add	x0, x0, x23
	add	x1, x1, #1
	cmp	x1, #REGION_PAGES
	b.lt	.Lwrite_page
	mov	x0, x20
	mov	x1, #0
.Lcheck_page:
	ldr	x2, [x0]
	add	x3, x1, x19
	cmp	x2, x3
	b.ne	.Lfailure
	add	x0, x0, x23
	add	x1, x1, #1
	cmp	x1, #REGION_PAGES
	b.lt	.Lcheck_page

	mov	x0, x20
	movz	x1, #(REGION_BYTES & 0xffff)
	movk	x1, #((REGION_BYTES >> 16) & 0xffff), lsl #16
	mov	x8, #NR_MUNMAP
	svc	#0
	cbnz	x0, .Lfailure
	subs	x19, x19, #1
	b.ne	.Lround

	adrp	x9, finished
	add	x9, x9, :lo12:finished
	mov	w10, #1
	stlr	w10, [x9]
.Lreap:
	cbz	x21, .Lsucceed
	mov	x0, #-1
	add	x1, sp, #0
	mov	x2, #0
	mov	x3, #0
	mov	x8, #NR_WAIT4
	svc	#0
	cmp	x0, #0
	b.lt	.Lfailure
	sub	x21, x21, #1
	b	.Lreap

.Lsucceed:
	probe_finish
	probe_end

	.section .bss
	.balign 16
finished:
	.space 16
stacks:
	.space (SPINNERS + 1) * STACK_SIZE
