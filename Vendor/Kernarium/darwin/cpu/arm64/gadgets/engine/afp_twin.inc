/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FEAT_AFP twins. Where the virtual CPU implements FEAT_AFP (the
 * implements_afp() predicate), the builder runs a twin in place of each entry
 * whose code would ignore FPCR.FIZ, AH or NEP: a scalar result the host
 * instruction writes over a cleared register instead of the architectural
 * merge source, a sign operation outside the guest FP environment, a software
 * model without the alternative controls, or an FPCR access masked without
 * them. The twins live in their own section and carry no forwarding variants,
 * so no established entry moves and other hosts translate exactly as before.
 * Each row of the table below maps one entry, or one equal-stride
 * fixed-point table, to its twin. */
.pushsection __DATA_CONST,__gadget_afp
.p2align 3
.globl CSYM(uml_arm64_afp_twin_begin)
CSYM(uml_arm64_afp_twin_begin):
.popsection

.macro afp_twins_begin
	.pushsection GADGET_SECTION_AFP
.endm

.macro afp_twins_end
	.popsection
.endm

/* Entries [base_begin, base_end) run as [twin_begin, twin_end). */
.macro afp_twin_row base_begin, base_end, twin_begin, twin_end
	.pushsection __DATA_CONST,__gadget_afp
	.quad \base_begin, \base_end, \twin_begin, \twin_end
	.popsection
.endm

/* The twin of one entry. Gadgets are 16-byte aligned, so a four-byte range
 * names exactly one. */
.macro afp_gadget name
	gadget \name\()_afp
	afp_twin_row CSYM(uml_arm64_gadget_\name), CSYM(uml_arm64_gadget_\name) + 4, CSYM(uml_arm64_gadget_\name\()_afp), CSYM(uml_arm64_gadget_\name\()_afp) + 4
.endm

/* The gadget \name, or with \afp its twin. */
.macro gadget_or_twin name, afp
	.if \afp
	afp_gadget \name
	.else
	gadget \name
	.endif
.endm

/* The row of a fixed-point table whose twin is \name\()_afp. */
.macro afp_table_row name
	afp_twin_row CSYM(uml_arm64_gadget_\name), CSYM(uml_arm64_gadget_\name\()_end), CSYM(uml_arm64_gadget_\name\()_afp), CSYM(uml_arm64_gadget_\name\()_afp_end)
.endm

.macro afp_twin_table_end
	.pushsection __DATA_CONST,__gadget_afp
	.globl CSYM(uml_arm64_afp_twin_end)
CSYM(uml_arm64_afp_twin_end):
	.popsection
.endm
