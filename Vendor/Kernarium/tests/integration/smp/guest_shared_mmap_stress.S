/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Freestanding arm64/Linux workload for Darwin UML's shared-mm synchronizer.
 * Ten clone threads check data and permissions while churning distant slots.
 * The parent checks fork COW and revocation of a sibling's cached write access.
 */

	.equ NR_WRITE,		64
	.equ NR_FUTEX,		98
	.equ NR_RT_SIGACTION,	134
	.equ NR_RT_SIGRETURN,	139
	.equ NR_MUNMAP,		215
	.equ NR_CLONE,		220
	.equ NR_MMAP,		222
	.equ NR_MPROTECT,	226
	.equ NR_WAIT4,		260
	.equ NR_EXIT,		93
	.equ NR_EXIT_GROUP,	94

	.equ PROT_READ_WRITE,	3
	.equ MAP_FIXED_PRIVATE_ANON, 0x32
	.equ CLONE_THREAD_FLAGS, 0x50f00
	.equ THREAD_COUNT,	10
	.equ STACK_SIZE,	0x100000
	.equ SLOT_SIZE,		0x4000
	.equ ITERATIONS,	2000

	.section .text
	.global _start
	.type _start, %function
_start:
	adrp	x21, remaining
	add	x21, x21, :lo12:remaining
	mov	w0, #THREAD_COUNT
	stlr	w0, [x21]

	adrp	x25, failed
	add	x25, x25, :lo12:failed
	stlr	wzr, [x25]
	adrp	x27, start_churn
	add	x27, x27, :lo12:start_churn
	stlr	wzr, [x27]

	mov	x20, #0
.Lspawn:
	/* Give each raw clone thread a private 1 MiB stack. */
	mov	x0, #0
	mov	x1, #STACK_SIZE
	mov	x2, #PROT_READ_WRITE
	mov	x3, #0x22		/* MAP_PRIVATE | MAP_ANONYMOUS */
	mov	x4, #-1
	mov	x5, #0
	mov	x8, #NR_MMAP
	svc	#0
	tbnz	x0, #63, .Lparent_mmap_fail
	add	x1, x0, #STACK_SIZE
	and	x1, x1, #-16

	movz	x0, #0x0005, lsl #16
	movk	x0, #0x0f00
	mov	x2, #0
	mov	x3, #0
	mov	x4, #0
	mov	x8, #NR_CLONE
	svc	#0
	cbz	x0, .Lchild
	tbnz	x0, #63, .Lparent_clone_fail

	add	x20, x20, #1
	cmp	x20, #THREAD_COUNT
	b.ne	.Lspawn

	/* Run the publication checks while sibling tasks fault and change VMAs. */
	mov	w0, #1
	stlr	w0, [x27]
	mov	x0, #0
	mov	x1, #SLOT_SIZE
	mov	x2, #PROT_READ_WRITE
	mov	x3, #0x22
	mov	x4, #-1
	mov	x5, #0
	mov	x8, #NR_MMAP
	svc	#0
	tbnz	x0, #63, .Lparent_cow_fail
	mov	x19, x0
	mov	x28, #0x5a5a
	str	x28, [x19]
	str	x28, [x19, #0x1000]
	str	x28, [x19, #0x2000]
	str	x28, [x19, #0x3000]

	/* A forked child must not change the parent's formerly writable pages. */
	mov	x0, #17		/* SIGCHLD, without CLONE_VM */
	mov	x1, #0
	mov	x2, #0
	mov	x3, #0
	mov	x4, #0
	mov	x8, #NR_CLONE
	svc	#0
	cbz	x0, .Lcow_child
	tbnz	x0, #63, .Lparent_cow_fail
	mov	x26, x0
	adrp	x27, probe_state
	add	x27, x27, :lo12:probe_state
	str	wzr, [x27]
	mov	x1, x27
	mov	x2, #0
	mov	x3, #0
	mov	x8, #NR_WAIT4
	svc	#0
	cmp	x0, x26
	b.ne	.Lparent_cow_fail
	ldr	w0, [x27]
	cbnz	w0, .Lparent_cow_fail
	ldr	x0, [x19]
	cmp	x0, x28
	b.ne	.Lparent_cow_fail
	ldr	x0, [x19, #0x1000]
	cmp	x0, x28
	b.ne	.Lparent_cow_fail
	ldr	x0, [x19, #0x2000]
	cmp	x0, x28
	b.ne	.Lparent_cow_fail
	ldr	x0, [x19, #0x3000]
	cmp	x0, x28
	b.ne	.Lparent_cow_fail

	/* The child shares this mm but owns its signal disposition and exit status. */
	mov	x0, #0
	mov	x1, #STACK_SIZE
	mov	x2, #PROT_READ_WRITE
	mov	x3, #0x22
	mov	x4, #-1
	mov	x5, #0
	mov	x8, #NR_MMAP
	svc	#0
	tbnz	x0, #63, .Lparent_revoke_fail
	add	x1, x0, #STACK_SIZE
	and	x1, x1, #-16
	stlr	wzr, [x27]
	mov	x0, #0x111		/* CLONE_VM | SIGCHLD */
	mov	x2, #0
	mov	x3, #0
	mov	x4, #0
	mov	x8, #NR_CLONE
	svc	#0
	cbz	x0, .Lrevocation_child
	tbnz	x0, #63, .Lparent_revoke_fail
	mov	x26, x0
.Lwait_warm_write:
	ldar	w0, [x27]
	cbnz	w0, .Lrevoke_write
	yield
	b	.Lwait_warm_write
.Lrevoke_write:
	mov	x0, x19
	mov	x1, #SLOT_SIZE
	mov	x2, #1		/* PROT_READ */
	mov	x8, #NR_MPROTECT
	svc	#0
	cbnz	x0, .Lparent_revoke_fail
	mov	w0, #2
	stlr	w0, [x27]
	mov	x0, x26
	add	x1, x27, #4
	mov	x2, #0
	mov	x3, #0
	mov	x8, #NR_WAIT4
	svc	#0
	cmp	x0, x26
	b.ne	.Lparent_revoke_fail
	ldr	w0, [x27, #4]
	cbnz	w0, .Lparent_revoke_fail
	mov	x0, x19
	mov	x1, #SLOT_SIZE
	mov	x2, #PROT_READ_WRITE
	mov	x8, #NR_MPROTECT
	svc	#0
	cbnz	x0, .Lparent_revoke_fail
	ldr	x0, [x19]
	cmp	x0, x28
	b.ne	.Lparent_revoke_fail
	mov	x0, x19
	mov	x1, #SLOT_SIZE
	mov	x8, #NR_MUNMAP
	svc	#0
	cbnz	x0, .Lparent_revoke_fail
	adrp	x0, publication_done
	add	x0, x0, :lo12:publication_done
	mov	w1, #1
	stlr	w1, [x0]

.Lwait:
	ldar	w0, [x21]
	cbz	w0, .Ldone
	yield
	b	.Lwait

.Ldone:
	ldar	w0, [x25]
	cbnz	w0, .Lexit_failure
	adrp	x1, success_message
	add	x1, x1, :lo12:success_message
	mov	x0, #1
	mov	x2, #3
	mov	x8, #NR_WRITE
	svc	#0
	mov	x0, #0
	b	.Lexit_group

.Lparent_mmap_fail:
	mov	w0, #4
	stlr	w0, [x25]
	b	.Lexit_failure
.Lparent_clone_fail:
	mov	w0, #5
	stlr	w0, [x25]
	b	.Lexit_failure
.Lparent_cow_fail:
	mov	w0, #8
	stlr	w0, [x25]
	b	.Lexit_failure
.Lparent_revoke_fail:
	mov	w0, #9
	stlr	w0, [x25]
.Lexit_failure:
.Lexit_group:
	mov	x8, #NR_EXIT_GROUP
	svc	#0
	brk	#0

.Lcow_child:
	ldr	x0, [x19]
	cmp	x0, x28
	b.ne	.Lcow_child_fail
	mvn	x0, x28
	str	x0, [x19]
	str	x0, [x19, #0x1000]
	str	x0, [x19, #0x2000]
	str	x0, [x19, #0x3000]
	ldr	x1, [x19]
	cmp	x1, x0
	b.ne	.Lcow_child_fail
	mov	x0, #0
	b	.Lexit_group
.Lcow_child_fail:
	mov	x0, #8
	b	.Lexit_group

.Lrevocation_child:
	/* Handle only the expected write fault, without a core or kernel warning. */
	sub	sp, sp, #32
	adrp	x0, .Lrevocation_signal
	add	x0, x0, :lo12:.Lrevocation_signal
	str	x0, [sp]
	movz	x0, #0x0400, lsl #16	/* SA_RESTORER | SA_SIGINFO */
	orr	x0, x0, #4
	str	x0, [sp, #8]
	adrp	x0, .Lsignal_return
	add	x0, x0, :lo12:.Lsignal_return
	str	x0, [sp, #16]
	str	xzr, [sp, #24]
	mov	x0, #11		/* SIGSEGV */
	mov	x1, sp
	mov	x2, #0
	mov	x3, #8		/* arm64 kernel sigset size */
	mov	x8, #NR_RT_SIGACTION
	svc	#0
	add	sp, sp, #32
	cbnz	x0, .Lrevocation_child_fail
	mov	x23, #0
	mov	x24, #1000
.Lcached_write:
	str	x28, [x19]
	subs	x24, x24, #1
	b.ne	.Lcached_write
	cbnz	x23, .Lrevocation_child_fail
	mov	w0, #1
	stlr	w0, [x27]
.Lwait_revoked:
	ldar	w0, [x27]
	cmp	w0, #2
	b.eq	.Lretry_cached_write
	yield
	b	.Lwait_revoked
.Lretry_cached_write:
	mov	x23, #1
	mov	x24, #1
	b	.Lcached_write
.Lrevocation_signal:
	cmp	x0, #11
	b.ne	.Lrevocation_child_fail
	ldr	w3, [x1, #8]
	cmp	w3, #2		/* SEGV_ACCERR */
	b.ne	.Lrevocation_child_fail
	ldr	x3, [x1, #16]
	cmp	x3, x19
	b.ne	.Lrevocation_child_fail
	mov	x0, #0
	b	.Lrevocation_child_exit
.Lrevocation_child_fail:
	mov	x0, #9
.Lrevocation_child_exit:
	mov	x8, #NR_EXIT
	svc	#0
	brk	#0
.Lsignal_return:
	mov	x8, #NR_RT_SIGRETURN
	svc	#0
	brk	#0

.Lchild:
	ldar	w0, [x27]
	cbnz	w0, .Lchild_started
	yield
	b	.Lchild
.Lchild_started:
	/*
	 * Keep the slots several TiB apart.  The old tracker expands concurrent
	 * changes into one envelope; the bounded tracker retains ten 16 KiB
	 * intervals and walks only the slots that actually changed.
	 */
	mov	x22, #1
	lsl	x22, x22, #40		/* first slot: 1 TiB */
	lsl	x23, x20, #42		/* stride: 4 TiB */
	add	x22, x22, x23
.Lreset_iterations:
	movz	x24, #(ITERATIONS & 0xffff)
	movk	x24, #((ITERATIONS >> 16) & 0xffff), lsl #16

.Literation:
	mov	x0, x22
	mov	x1, #SLOT_SIZE
	mov	x2, #PROT_READ_WRITE
	mov	x3, #MAP_FIXED_PRIVATE_ANON
	mov	x4, #-1
	mov	x5, #0
	mov	x8, #NR_MMAP
	svc	#0
	tbnz	x0, #63, .Lchild_mmap_fail

	ldr	x0, [x22]
	cbnz	x0, .Lchild_data_fail
	ldr	x0, [x22, #0x1000]
	cbnz	x0, .Lchild_data_fail
	ldr	x0, [x22, #0x2000]
	cbnz	x0, .Lchild_data_fail
	ldr	x0, [x22, #0x3000]
	cbnz	x0, .Lchild_data_fail
	str	x24, [x22]
	str	x20, [x22, #0x1000]
	str	x24, [x22, #0x2000]
	str	x20, [x22, #0x3000]
	bl	.Lcheck_quarters

	/* Mix permission changes into the mapping churn without dominating it. */
	tst	x24, #63
	b.ne	.Lunmap_slot
	mov	x0, x22
	mov	x1, #SLOT_SIZE
	mov	x2, #1		/* PROT_READ */
	mov	x8, #NR_MPROTECT
	svc	#0
	cbnz	x0, .Lchild_protect_fail
	bl	.Lcheck_quarters
	mov	x0, x22
	mov	x1, #SLOT_SIZE
	mov	x2, #PROT_READ_WRITE
	mov	x8, #NR_MPROTECT
	svc	#0
	cbnz	x0, .Lchild_protect_fail
	add	x0, x24, #1
	str	x0, [x22]
	ldr	x1, [x22]
	cmp	x1, x0
	b.ne	.Lchild_data_fail

.Lunmap_slot:
	mov	x0, x22
	mov	x1, #SLOT_SIZE
	mov	x8, #NR_MUNMAP
	svc	#0
	tbnz	x0, #63, .Lchild_munmap_fail

	subs	x24, x24, #1
	b.ne	.Literation
	/* Keep churn active until the parent's COW and revocation checks finish. */
	adrp	x0, publication_done
	add	x0, x0, :lo12:publication_done
	ldar	w0, [x0]
	cbz	w0, .Lreset_iterations
	b	.Lchild_complete

.Lchild_mmap_fail:
	mov	w0, #2
	stlr	w0, [x25]
	b	.Lchild_complete
.Lchild_munmap_fail:
	mov	w0, #3
	stlr	w0, [x25]
	b	.Lchild_complete
.Lchild_data_fail:
	mov	w0, #6
	stlr	w0, [x25]
	b	.Lchild_complete
.Lchild_protect_fail:
	mov	w0, #7
	stlr	w0, [x25]

.Lchild_complete:
	ldxr	w0, [x21]
	sub	w0, w0, #1
	stlxr	w1, w0, [x21]
	cbnz	w1, .Lchild_complete
	mov	x0, #0
	mov	x8, #NR_EXIT
	svc	#0
	brk	#0

.Lcheck_quarters:
	ldr	x0, [x22]
	cmp	x0, x24
	b.ne	.Lchild_data_fail
	ldr	x0, [x22, #0x1000]
	cmp	x0, x20
	b.ne	.Lchild_data_fail
	ldr	x0, [x22, #0x2000]
	cmp	x0, x24
	b.ne	.Lchild_data_fail
	ldr	x0, [x22, #0x3000]
	cmp	x0, x20
	b.ne	.Lchild_data_fail
	ret

	.size _start, .-_start

	.section .rodata
success_message:
	.ascii "OK\n"

	.section .bss
	.p2align 3
remaining:
	.skip 4
failed:
	.skip 4
start_churn:
	.skip 4
probe_state:
	.skip 8
publication_done:
	.skip 4
