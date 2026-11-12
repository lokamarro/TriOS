/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The host-register frame that a gadget engine's entry routine builds and its
 * exit takes down again. Both engines include this file.
 *
 * AAPCS64 makes x19-x28, the frame pointer x29 and the link register x30 the
 * callee's to preserve, and the engines keep the stream pointer, the retired
 * count and the guest's flags in some of them, so the entry routine saves all
 * twelve. x18 is reserved by Apple platforms, which clear it when they
 * interrupt a thread, so no engine uses it and the frame has no slot for it.
 *
 * From the stack pointer after gadget_host_frame_push, lowest address first:
 *
 *	0x00	x29, x30
 *	0x10	x19, x20
 *	0x20	x21, x22
 *	0x30	x23, x24
 *	0x40	x25, x26
 *	0x50	x27, x28
 *	0x60	the engine's own slots
 *
 * An engine passes the byte size of its slots, a multiple of 16, to both
 * macros and names a slot by its offset from GADGET_HOST_FRAME_SAVED. The
 * frame is the saved registers plus the slots, so the stack pointer stays
 * 16-byte aligned for the helper calls the gadgets make.
 */
#define GADGET_HOST_FRAME_SAVED 0x60

.macro gadget_host_frame_push slots=0
	sub sp, sp, #(GADGET_HOST_FRAME_SAVED + \slots)
	stp x29, x30, [sp]
	stp x19, x20, [sp, #0x10]
	stp x21, x22, [sp, #0x20]
	stp x23, x24, [sp, #0x30]
	stp x25, x26, [sp, #0x40]
	stp x27, x28, [sp, #0x50]
.endm

/* The stack pointer moves before the first store and after the last load, so
 * a saved register is never below it. */
.macro gadget_host_frame_pop slots=0
	ldp x29, x30, [sp]
	ldp x19, x20, [sp, #0x10]
	ldp x21, x22, [sp, #0x20]
	ldp x23, x24, [sp, #0x30]
	ldp x25, x26, [sp, #0x40]
	ldp x27, x28, [sp, #0x50]
	add sp, sp, #(GADGET_HOST_FRAME_SAVED + \slots)
.endm
