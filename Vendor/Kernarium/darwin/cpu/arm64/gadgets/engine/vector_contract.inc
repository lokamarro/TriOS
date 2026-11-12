/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Complete write-through SIMD results in v2. Ordinary hot paths are unchanged;
 * only consumers with a proved adjacent dependency select static variants.
 * All forwarded inputs must be captured before v2 is overwritten. */
.set Lvector_register0, 0x4000
.set Lvector_register8, 0x4000 | 8
.set Lvector_register16, 0x4000 | 16

.pushsection __DATA_CONST,__gadget_vector
.p2align 3
.globl CSYM(uml_arm64_vector_contract_begin)
CSYM(uml_arm64_vector_contract_begin):
.popsection

.macro vector_pointer name, mask, sources
 .if (\mask & \sources) == \mask
 .quad CSYM(uml_arm64_gadget_\name\()_vector_\mask)
 .else
 .quad 0
 .endif
.endm

.macro vector_entry name, forward, destination, first=Lscalar_none, second=Lscalar_none, third=Lscalar_none, words=1
 .if \forward == 0
 gadget \name
 .set Lvector_sources\@, ((\first != Lscalar_none) & 1) | (((\second != Lscalar_none) & 1) << 1) | (((\third != Lscalar_none) & 1) << 2)
 .pushsection __DATA_CONST,__gadget_vector
 .quad CSYM(uml_arm64_gadget_\name)
 .irp mask, 1, 2, 3, 4, 5, 6, 7
 vector_pointer \name, \mask, Lvector_sources\@
 .endr
 .short \destination, \first, \second, \third
 /* Vector contracts leave the unread family slot zero. */
 .short 0, \words
 .long 0
 .popsection
 .else
 gadget \name\()_vector_\forward
 .endif
.endm

.macro vector_variants forward, first, second, third, generator, arguments:vararg
 .set Lvector_masks\@, ((\first != Lscalar_none) & 1) | (((\second != Lscalar_none) & 1) << 1) | (((\third != Lscalar_none) & 1) << 2)
 .if \forward == 0 && Lvector_masks\@ != 0
 .pushsection GADGET_SECTION_VECTOR_FORWARD
 .irp mask, 1, 2, 3, 4, 5, 6, 7
 .if (\mask & Lvector_masks\@) == \mask
 \generator \arguments, \mask
 .endif
 .endr
 .popsection
 .endif
.endm

/* Preserve LDR's exact upper-bit clearing, including narrow FP accesses.
 * In-place q2 reads disappear; other reads use a register move, not memory.
 * The assembler rejects an accessor outside the declared scratch bank. */
.macro vector_read_move value, width, number
 .ifc \width, q
 .if \number != 2
 mov v\number\().16b, v2.16b
 .endif
 .else
 .ifc \width, b
 umov w6, v2.b[0]
 fmov s\number, w6
 .else
 .ifc \width, h
 fmov \value, h2
 .else
 .ifc \width, s
 fmov \value, s2
 .else
 fmov \value, d2
 .endif
 .endif
 .endif
 .endif
.endm

.macro vector_read forward, bit, value, address:vararg
 .if \forward & \bit
 .set Lvector_read_found\@, 0
 .irp width, q, d, s, h, b
 .irp number, 0, 1, 2, 3, 4
 .ifc \value, \width\number
 .set Lvector_read_found\@, 1
 vector_read_move \value, \width, \number
 .endif
 .endr
 .endr
 .if !Lvector_read_found\@
 .error "unsupported vector forwarding register"
 .endif
 .else
 ldr \value, \address
 .endif
.endm
