/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Unsigned-offset scalar transfers whose transferred register is a baked
 * CpuState displacement.
 *
 * A memory gadget touches CpuState twice: it reads the base register to form
 * the address, and it stores the loaded value (or reads the value to store).
 * Only a store-to-load pair whose two accesses both name a base and a constant
 * displacement forwards at rename, so which of the two is baked decides what
 * the change buys. Baking both costs one entry per base register per
 * transferred register, which no template can afford; baking one costs 31.
 *
 * Baking the transferred register gains more than baking the base read (G199),
 * so it is the one taken here: a loaded value is nearly always consumed soon
 * afterwards, while a base is commonly a long-lived pointer whose producer is
 * far away or absent.
 *
 * Baking the transferred register is also what keeps an in-place update from
 * regressing. When only the ALU entry is baked, both of the loaded register's
 * CpuState hops become mixed -- the load gadget stores through an index while
 * the baked ALU reads a displacement, and the reverse on the way out -- and a
 * mismatched pair costs more than one that is indexed on both sides. Baking
 * this family removes that mismatch as well as adding the forwarded pair.
 *
 * Scope is the unsigned-offset 32- and 64-bit forms, which are the most common
 * transfer shape in translated code. The unscaled, pre/post-indexed and
 * register-offset families are deliberately left alone: they are 477 entries
 * against these 8, and at 31 copies each they would cost megabytes of bank for
 * a slice this has not yet been shown to pay for. The zero-register modes need
 * nothing, because XZR never reaches CpuState.
 */

.macro scalar_unsigned_inplace_register load, bits64, reg, prefix
	.set Lipmem\@, 0x8000 | (\reg * 8)
scalar_unsigned_fast \load, \bits64, 0, 0, \prefix\()_r_\reg, Lipmem\@
scalar_unsigned_fast \load, \bits64, 0, 1, \prefix\()_r_sp_\reg, Lipmem\@
.endm

.macro scalar_unsigned_inplace_modes load, bits64, prefix
	.irp reg, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
scalar_unsigned_inplace_register \load, \bits64, \reg, \prefix
	.endr
.endm

scalar_unsigned_inplace_modes 0, 0, store32_unsigned_inplace
scalar_unsigned_inplace_modes 0, 1, store64_unsigned_inplace
scalar_unsigned_inplace_modes 1, 0, load32_unsigned_inplace
scalar_unsigned_inplace_modes 1, 1, load64_unsigned_inplace

/* One flat selection table so the translator indexes rather than switches:
 * index = ((operation * 2 + bits64) * 2 + base_sp) * 31 + register, with
 * operation 0 for a store and 1 for a load. Entries are the base gadgets;
 * adjacent forwarding replaces a selection afterwards through the ordinary
 * scalar contract, which the baked variants join unchanged. */
.macro scalar_unsigned_inplace_row prefix, mode
	.irp reg, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30
	.quad CSYM(uml_arm64_gadget_\prefix\()_\mode\()_\reg)
	.endr
.endm

.pushsection __DATA_CONST,__gadget_inplace
.p2align 3
.globl CSYM(uml_arm64_gadget_scalar_unsigned_inplace_table)
CSYM(uml_arm64_gadget_scalar_unsigned_inplace_table):
scalar_unsigned_inplace_row store32_unsigned_inplace, r
scalar_unsigned_inplace_row store32_unsigned_inplace, r_sp
scalar_unsigned_inplace_row store64_unsigned_inplace, r
scalar_unsigned_inplace_row store64_unsigned_inplace, r_sp
scalar_unsigned_inplace_row load32_unsigned_inplace, r
scalar_unsigned_inplace_row load32_unsigned_inplace, r_sp
scalar_unsigned_inplace_row load64_unsigned_inplace, r
scalar_unsigned_inplace_row load64_unsigned_inplace, r_sp
.popsection
.purgem scalar_unsigned_inplace_row

.purgem scalar_unsigned_inplace_modes
.purgem scalar_unsigned_inplace_register
.purgem scalar_unsigned_fast
.purgem sp_alignment_check

