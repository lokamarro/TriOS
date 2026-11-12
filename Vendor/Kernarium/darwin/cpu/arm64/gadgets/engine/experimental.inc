/* SPDX-License-Identifier: GPL-2.0-or-later */

/*
 * An accepted family, appended after the established bank for layout
 * stability; the file name records where it entered the bank.
 *
 * Bounded destination residency for non-faulting integer vector MLA runs.
 * q24 is caller-saved by AAPCS64 and otherwise unused by the gadget engine.
 * The builder emits load and flush inside the same translated block, and it
 * rejects a run when either source aliases the dirty guest destination.
 */
gadget simd_destination_resident_load
	ldr x8, [_stream], #8
	ldr q24, [_vcpu, x8]
	next_gadget

.macro simd_destination_resident_mla name, arrangement
gadget simd_destination_resident_mla_\name
	ldr x8, [_stream], #8
	and x10, x8, #0xffff
	lsr x11, x8, #16
	ldr q0, [_vcpu, x10]
	ldr q1, [_vcpu, x11]
	mla v24.\arrangement, v0.\arrangement, v1.\arrangement
	next_gadget
.endm

simd_destination_resident_mla 8b, 8b
simd_destination_resident_mla 4h, 4h
simd_destination_resident_mla 2s, 2s
simd_destination_resident_mla 16b, 16b
simd_destination_resident_mla 8h, 8h
simd_destination_resident_mla 4s, 4s

.purgem simd_destination_resident_mla

gadget simd_destination_resident_flush
	ldr x8, [_stream], #8
	str q24, [_vcpu, x8]
	next_gadget
