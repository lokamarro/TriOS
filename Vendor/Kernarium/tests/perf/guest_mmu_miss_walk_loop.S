/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux control for the software-MMU miss path.
 *
 * A guest load resolves in the per-engine software TLB, and only a miss calls
 * darwin_uml_static_vm_translate(). That call is where the address space is
 * resolved and the mapping looked up, so it is the only path on which the
 * registry lookup, the lifecycle lease and the per-generation mapping lock are
 * paid. Nothing in tests/perf reached it before this probe:
 * guest_tlb_32m_alias_loop touches exactly two pages, which live in the TLB
 * from the second iteration onward, because it was built to expose aliasing
 * in the direct index rather than to generate misses; and guest_memory_site_loop
 * hammers a single site.
 *
 * This probe makes every access a miss instead. It walks a working set of
 * BENCH_PAGES guest pages, one page apart, which at the default of 65,536 pages
 * is 256 MiB against a TLB of 16,384 direct-mapped entries. A linear walk of
 * four times the TLB's capacity evicts each entry before it is reached again,
 * so a pass costs one translate() per page.
 *
 * BENCH_THREADS threads share the address space through CLONE_VM, so their
 * misses land on the same generation state and the same process-wide registry -
 * the shape Geekbench's multi-core phase produces, and the reason a per-miss
 * lock acquisition costs more than its uncontended latency suggests. Each thread
 * keeps its own TLB, so sharing the region does not turn the other threads'
 * misses into hits.
 *
 * Run with --workers 1. The probe generates its own concurrency; parallel copies
 * would give each process its own address space and measure something else.
 *
 * Every pass sums the first word of each page and compares against the value the
 * parent wrote there, so a run that reports OK has demonstrated that all
 * BENCH_PAGES translations resolved to the right host page, for every thread and
 * every pass. The check is per thread and touches no shared line, keeping the
 * measured loop free of contention that is not the MMU's.
 */

	.equ NR_WRITE,		64
	.equ NR_EXIT,		93
	.equ NR_EXIT_GROUP,	94
	.equ NR_CLONE,		220
	.equ NR_WAIT4,		260

	/* Share the address space but stay an ordinary child so wait4 reaps it. */
	.equ CLONE_VM,		0x00000100
	.equ SIGCHLD,		17
	.equ CLONE_FLAGS,	(CLONE_VM | SIGCHLD)

#ifndef BENCH_THREADS
#define BENCH_THREADS 10
#endif
/* Twice the 32,768-entry TLB, so a linear walk misses on every page. */
#ifndef BENCH_PAGES
#define BENCH_PAGES 65536
#endif
#ifndef BENCH_PASSES
#define BENCH_PASSES 16
#endif
	.equ THREADS,		BENCH_THREADS
	.equ PAGES,		BENCH_PAGES
	.equ PASSES,		BENCH_PASSES
	.equ GUEST_PAGE_SIZE,	4096
	.equ STACK_SIZE,	16384
	/* Sum of the page indices the parent seeds, checked after every pass. */
	.equ EXPECTED_SUM,	(PAGES * (PAGES - 1) / 2)

#include "probe_harness.inc"

	probe_start
	/* Seed each page before any thread exists, so the working set is faulted
	 * in once and the measured passes contain no first-touch faults. */
	adrp	x0, region
	add	x0, x0, :lo12:region
	mov	x1, #0
	movz	x2, #(PAGES & 0xffff)
	movk	x2, #((PAGES >> 16) & 0xffff), lsl #16
	mov	x3, #GUEST_PAGE_SIZE
.Lseed:
	str	x1, [x0]
	add	x0, x0, x3
	add	x1, x1, #1
	cmp	x1, x2
	b.lt	.Lseed

	mov	x21, #0				/* children already started */

.Lspawn:
	cmp	x21, #(THREADS - 1)
	b.ge	.Lrun_self
	/* Child stack top = stacks + (index + 1) * STACK_SIZE. Index zero stays
	 * reserved for the parent, which keeps the stack the kernel gave it. */
	adrp	x1, stacks
	add	x1, x1, :lo12:stacks
	add	x22, x21, #1
	mov	x2, #STACK_SIZE
	madd	x1, x22, x2, x1
	mov	x0, #CLONE_FLAGS
	mov	x2, #0
	mov	x3, #0
	mov	x4, #0
	mov	x8, #NR_CLONE
	svc	#0
	cmp	x0, #0
	b.lt	.Lfailure
	b.eq	.Lchild
	add	x21, x21, #1
	b	.Lspawn

.Lchild:
	bl	.Lwork
	cbnz	x0, .Lchild_failed
	mov	x0, #0
	mov	x8, #NR_EXIT
	svc	#0
	brk	#0
.Lchild_failed:
	mov	x0, #1
	mov	x8, #NR_EXIT_GROUP
	svc	#0
	brk	#0

.Lrun_self:
	bl	.Lwork
	cbnz	x0, .Lfailure
	mov	x20, #0
.Lwait_loop:
	cmp	x20, #(THREADS - 1)
	b.ge	.Lsucceed
	mov	x0, #-1				/* any child */
	add	x1, sp, #0			/* child status */
	mov	x2, #0
	mov	x3, #0
	mov	x8, #NR_WAIT4
	svc	#0
	cmp	x0, #0
	b.lt	.Lfailure
	ldr	w4, [sp]
	cbnz	w4, .Lfailure			/* a child's sum did not match */
	add	x20, x20, #1
	b	.Lwait_loop

.Lsucceed:
	probe_finish

	/* PASSES linear walks of the working set. Returns 0 on success, 1 if any
	 * pass read a value the parent did not write. */
	.type .Lwork, %function
.Lwork:
	movz	x19, #(PASSES & 0xffff)
	movk	x19, #((PASSES >> 16) & 0xffff), lsl #16
	movz	x23, #(PAGES & 0xffff)
	movk	x23, #((PAGES >> 16) & 0xffff), lsl #16
	movz	x24, #(EXPECTED_SUM & 0xffff)
	movk	x24, #((EXPECTED_SUM >> 16) & 0xffff), lsl #16
	movk	x24, #((EXPECTED_SUM >> 32) & 0xffff), lsl #32
	movk	x24, #((EXPECTED_SUM >> 48) & 0xffff), lsl #48
	mov	x25, #GUEST_PAGE_SIZE

.Lpass:
	adrp	x0, region
	add	x0, x0, :lo12:region
	mov	x1, #0				/* pages walked */
	mov	x2, #0				/* running sum */
.Lwalk:
	ldr	x3, [x0]
	add	x2, x2, x3
	add	x0, x0, x25
	add	x1, x1, #1
	cmp	x1, x23
	b.lt	.Lwalk

	cmp	x2, x24
	b.ne	.Lwork_failed
	subs	x19, x19, #1
	b.ne	.Lpass
	mov	x0, #0
	ret
.Lwork_failed:
	mov	x0, #1
	ret
	.size .Lwork, .-.Lwork

	probe_end

	.section .bss
	.balign 16
stacks:
	.space THREADS * STACK_SIZE

	.p2align 14
region:
	.space PAGES * GUEST_PAGE_SIZE
