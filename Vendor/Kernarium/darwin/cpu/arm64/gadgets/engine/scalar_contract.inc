/* SPDX-License-Identifier: GPL-2.0-or-later */
/* One write-through result in x0. It is live only between declared adjacent
 * producers/consumers. Templates emit ordinary bodies, source-mask variants,
 * and immutable operand contracts. No runtime checks or executable writes. */
_scalar_x7 .req x0
_scalar_w7 .req w0
_scalar_x11 .req x0
_scalar_x12 .req x0
_scalar_w12 .req w0
_scalar_x13 .req x0
_scalar_w13 .req w0
_scalar_x14 .req x0
_scalar_w14 .req w0
_scalar_x15 .req x0
_scalar_w15 .req w0
_scalar_x16 .req x0
_scalar_w16 .req w0

/* First stream operand: shift[5:0], width[11:6], byte scaling[13:12].
 * Bit 15 denotes a constant CpuState offset; 0xffff denotes no operand. */
.set Lscalar_none, 0xffff
.set Lscalar_offset0, (16 << 6)
.set Lscalar_offset16, (16 << 6) | 16
.set Lscalar_offset32, (16 << 6) | 32
.set Lscalar_offset48, (16 << 6) | 48
.set Lscalar_offset8_48, (8 << 6) | 48
.set Lscalar_offset12_24, (12 << 6) | 24
.set Lscalar_register0, (3 << 12) | (5 << 6)
.set Lscalar_register8, (3 << 12) | (5 << 6) | 8
.set Lscalar_register16, (3 << 12) | (5 << 6) | 16
.set Lscalar_sp, 0x8000 | UML_GADGET_CPU_SP

.pushsection __DATA_CONST,__gadget_scalar
.p2align 3
.globl CSYM(uml_arm64_scalar_contract_begin)
CSYM(uml_arm64_scalar_contract_begin):
.popsection

.macro scalar_pointer name, mask, sources
 .if (\mask & \sources) == \mask
 .quad CSYM(uml_arm64_gadget_\name\()_forward_\mask)
 .else
 .quad 0
 .endif
.endm

/* Memory addressing sometimes also needs the decoded offset for writeback. */
.macro scalar_read forward, bit, value, index
 .if \forward & \bit
 mov \value, x0
 .else
 ldr \value, [_vcpu, \index]
 .endif
.endm

/* Store-data consumers capture into callee-saved x22 before translation can
 * call a slow helper. Use it directly at the store, avoiding a reload/move. */
.macro scalar_memory_transfer load, forward, bit, instruction, host_register
 .if !\load && (\forward & \bit)
 \instruction \host_register\()22, [x7]
 .else
 \instruction \host_register\()0, [x7]
 .endif
.endm
.macro scalar_store_vector forward, bit, wide
 .if \forward & \bit
 .if \wide
 fmov d0, x22
 .else
 fmov s0, w22
 .endif
 .else
 .if \wide
 fmov d0, x0
 .else
 fmov s0, w0
 .endif
 .endif
.endm

/* ScalarContract is 80 bytes. Catalog lookups occur only in Builder. */
.macro scalar_entry name, forward, family, destination, first=Lscalar_none, second=Lscalar_none, third=Lscalar_none, words=1
 .if \forward == 0
 gadget \name
 .set Lscalar_sources\@, ((\first != Lscalar_none) & 1) | (((\second != Lscalar_none) & 1) << 1) | (((\third != Lscalar_none) & 1) << 2)
 .pushsection __DATA_CONST,__gadget_scalar
 .quad CSYM(uml_arm64_gadget_\name)
 scalar_pointer \name, 1, Lscalar_sources\@
 scalar_pointer \name, 2, Lscalar_sources\@
 scalar_pointer \name, 3, Lscalar_sources\@
 scalar_pointer \name, 4, Lscalar_sources\@
 scalar_pointer \name, 5, Lscalar_sources\@
 scalar_pointer \name, 6, Lscalar_sources\@
 scalar_pointer \name, 7, Lscalar_sources\@
 .short \destination, \first, \second, \third
 .short \family, \words
 .long 0
 .popsection
 .else
 gadget \name\()_forward_\forward
 .endif
.endm

/* Masks exist only for accessors present in the specialization. A separate
 * section preserves the original bank's entry sizes and cache-line phase.
 * Each participating template's last parameter is its forwarding mask. */
.macro scalar_variants forward, first, second, third, generator, arguments:vararg
 .set Lvariant_sources\@, ((\first != Lscalar_none) & 1) | (((\second != Lscalar_none) & 1) << 1) | (((\third != Lscalar_none) & 1) << 2)
 .if \forward == 0 && Lvariant_sources\@ != 0
 .pushsection GADGET_SECTION_FORWARD
 .irp mask, 1, 2, 3, 4, 5, 6, 7
 .if (\mask & Lvariant_sources\@) == \mask
 \generator \arguments, \mask
 .endif
 .endr
 .popsection
 .endif
.endm

/* Every forwarded input must be consumed before the body overwrites x0. */
.macro scalar_load forward, bit, value, index, operand, shift, width=16, scale=0, base=_vcpu
 .if \forward & \bit
 .ifnc \value, x0
 mov \value, x0
 .endif
 .else
 .if \width == 5 && \scale == 3
 ubfx w\index, w\operand, #\shift, #\width
 .else
 ubfx x\index, x\operand, #\shift, #\width
 .endif
 ldr \value, [\base, x\index, lsl #\scale]
 .endif
.endm
.macro scalar_load_sp forward, bit, value
 .if \forward & \bit
 mov \value, x0
 .else
 ldr \value, [_vcpu, #UML_GADGET_CPU_SP]
 .endif
.endm
