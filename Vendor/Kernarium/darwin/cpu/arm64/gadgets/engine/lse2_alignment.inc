/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FEAT_LSE2 alignment of single-copy-atomic accesses. With LSE2, an LSE
 * atomic, an exclusive, or a load-acquire/store-release access that is not
 * naturally aligned but lies inside one 16-byte block completes single-copy
 * atomically; only one that crosses a block takes an alignment fault. Every
 * host that advertises LSE2 executes the gadget's native instruction on such
 * an address with the same result, so a gadget only has to decide whether
 * the guest may issue it: the TLB's unaligned_atomics flag, set when the
 * host implements LSE2 and the guest therefore sees USCAT.
 *
 * The check in each gadget stays one test and branch (single_copy_alignment
 * in system_atomic.inc). A misaligned address reaches the gadget's stub here,
 * which names its resume point and enters the shared rule for its width and
 * direction. The stubs and rule sit after every established family, so no
 * gadget moved to make room for them, and a conditional branch from the
 * atomic families still reaches them. The lists below mirror the families
 * that use the check; a gadget without its stub, or a stub without its
 * gadget, is an undefined label. Scratch: x8 and x9, both dead at every
 * check, which resumes at the gadget's translation. */

.macro lse2_alignment_rule mask, direction
	.p2align 4
uml_arm64_gadget_lse2_alignment_\direction\()_\mask:
	movz x9, #(UML_GADGET_TLB_UNALIGNED_ATOMICS & 0xffff)
	movk x9, #(UML_GADGET_TLB_UNALIGNED_ATOMICS >> 16), lsl #16
	ldrb w9, [_swtlb, x9]
	cbz w9, uml_arm64_gadget_atomic_alignment_fault_\direction
	and x9, x7, #15
	cmp x9, #(15 - \mask)
	b.hi uml_arm64_gadget_atomic_alignment_fault_\direction
	br x8
.endm

lse2_alignment_rule 1, read
lse2_alignment_rule 3, read
lse2_alignment_rule 7, read
lse2_alignment_rule 1, write
lse2_alignment_rule 3, write
lse2_alignment_rule 7, write

.purgem lse2_alignment_rule

.macro lse2_stub name, mask, write
Llse2_stub_\name:
	adr x8, Llse2_resume_\name
	.if \write
	b uml_arm64_gadget_lse2_alignment_write_\mask
	.else
	b uml_arm64_gadget_lse2_alignment_read_\mask
	.endif
.endm

/* Checked widths of the scalar families: halfword, word, doubleword. */
.macro lse2_stub_widths prefix, suffix, write
	lse2_stub \prefix\()_h_\suffix, 1, \write
	lse2_stub \prefix\()_w_\suffix, 3, \write
	lse2_stub \prefix\()_x_\suffix, 7, \write
.endm

.macro lse2_stub_modes4 prefix, write
	lse2_stub_widths \prefix, rr, \write
	lse2_stub_widths \prefix, rz, \write
	lse2_stub_widths \prefix, zr, \write
	lse2_stub_widths \prefix, zz, \write
.endm

.macro lse2_stub_modes2 prefix, write
	lse2_stub_widths \prefix, r, \write
	lse2_stub_widths \prefix, z, \write
.endm

/* LSE read-modify-write and compare-and-swap. */
.macro lse2_stub_rmw_orders operation
	.irp order, relaxed, release, acquire, acquire_release
	lse2_stub_modes4 atomic_\operation\()_\order, 1
	.endr
.endm

.irp operation, ldadd, ldclr, ldeor, ldset, ldsmax, ldsmin, ldumax, ldumin, swp
	lse2_stub_rmw_orders \operation
.endr

.purgem lse2_stub_rmw_orders

.macro lse2_stub_pair prefix, write, modes:vararg
	.irp mode, \modes
	lse2_stub \prefix\()_w_\mode, 7, \write
	.endr
.endm

.irp order, relaxed, release, acquire, acquire_release
	lse2_stub_modes4 compare_swap_\order, 1
	lse2_stub_pair compare_swap_pair_\order, 1, rr, rz, zr, zz
.endr

/* Exclusives. */
.irp order, relaxed, acquire
	lse2_stub_modes2 exclusive_load_\order, 0
	lse2_stub_pair exclusive_pair_load_\order, 0, rr, rz, zr, zz
.endr
.irp order, relaxed, release
	lse2_stub_modes4 exclusive_store_\order, 1
	lse2_stub_pair exclusive_pair_store_\order, 1, rrr, rrz, rzr, rzz, zrr, zrz, zzr, zzz
.endr

.purgem lse2_stub_pair

/* Load-acquire and store-release, including LDAPR, LDAPUR and STLUR. */
	lse2_stub_modes2 ordered_load_acquire, 0
	lse2_stub_modes2 ordered_store_release, 1
.irp mode, r, z
	lse2_stub rcpc_load_zero_h_\mode, 1, 0
	lse2_stub rcpc_load_zero_w_\mode, 3, 0
	lse2_stub rcpc_load_zero_x_\mode, 7, 0
	lse2_stub rcpc_load_sh_w_\mode, 1, 0
	lse2_stub rcpc_load_sh_x_\mode, 1, 0
	lse2_stub rcpc_load_sw_x_\mode, 3, 0
.endr
	lse2_stub_modes2 rcpc_store, 1

.purgem lse2_stub_modes2
.purgem lse2_stub_modes4
.purgem lse2_stub_widths
.purgem lse2_stub
