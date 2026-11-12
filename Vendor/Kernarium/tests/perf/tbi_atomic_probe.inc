/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Shared setup of the guest_tagged_*_loop.S probes: the atomic, ordered and
 * exclusive forms reach memory through a top-byte-tagged pointer, as every
 * Android process with heap pointer tagging makes them. The emulator ignores
 * the top byte by default, as the Android product runs it.
 *
 * With no argument the probe tags its pointer (tag 0xb4, bionic's heap tag).
 * With any argument it runs the same instructions through an untagged
 * pointer, which is the matched control: the tag is the only difference.
 *
 * tbi_probe_pointers leaves, for a probe that includes this file after
 * probe_harness.inc:
 *   x20  the pointer the probe's atomic, ordered or exclusive forms use;
 *   x21  the same page through an untagged pointer, which a probe's plain
 *        loads and stores use;
 *   x22  the data page, two words of which hold 1 and 2.
 */

.macro tbi_probe_pointers
	adrp	x22, data_page
	add	x22, x22, :lo12:data_page
	ldr	x9, [sp]
	movz	x10, #0xb400, lsl #48
	cmp	x9, #1
	csel	x10, x10, xzr, eq
	orr	x20, x22, x10
	mov	x21, x22
.endm

.macro tbi_probe_data
	.section .data
	.p2align 12
data_page:
	.xword 1
	.xword 2
	.zero 4080
.endm
