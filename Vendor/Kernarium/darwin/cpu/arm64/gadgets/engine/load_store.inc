/* SPDX-License-Identifier: GPL-2.0-or-later */
/* engine/sme.inc assembles this file a second time with
 * UML_GADGET_SME_TWIN set, as the streaming twins of its GPR families; the
 * #if blocks below hold what has no twin there: the nop, which is shared,
 * and the SIMD&FP families, whose registers streaming mode moves. */
#if !UML_GADGET_SME_TWIN
gadget nop
	next_gadget
#endif

/* The alignment check of an SP base, which must be 16-byte aligned: a
 * misaligned one takes the alignment fault of the direction, \load selecting
 * the read or the write. A forwarding variant reaches it through its own cold
 * stub, \read_stub or \write_stub; the labels come from the caller because a
 * nested macro's \@ is its own. A direction the caller never takes may pass 0
 * for its stub. The fault targets default to the scalar families'. The file
 * leaves it defined for scalar_unsigned_fast's other users, which purge it
 * with that macro (inplace_memory.inc and sme.inc). */
.macro sp_alignment_check base, load, forward, read_stub, write_stub, read_fault=Lscalar_alignment_fault_read_indexed, write_fault=Lscalar_alignment_fault_write_indexed
	tst \base, #0xf
	.if \load
		.if \forward
	b.ne \read_stub
		.else
	b.ne \read_fault
		.endif
	.else
		.if \forward
	b.ne \write_stub
		.else
	b.ne \write_fault
		.endif
	.endif
.endm

/* Literal form invariants are resolved while translating. The near entries'
 * one operand word holds the target address in bits 47:0, the destination in
 * bits 55:48 (an integer register's CpuState byte offset, or a vector
 * register's offset in sixteen-byte units) and the instruction's position in
 * its block in bits 63:56, which a fault reports in place of a PC word. A
 * target that does not fit 48 bits takes the far entries: operand bits 15:0
 * are the CpuState destination byte offset and bits 63:16 the signed PC
 * displacement, and the following word is the guest PC used for address
 * formation and precise memory faults. */
.macro literal_integer_fast name, instruction, host_register, bytes, extension, discard, far=0, forward=0
	.set Lsd\@, Lscalar_offset8_48
	.if \far
	.set Lsd\@, Lscalar_offset0
	.endif
	.if \discard
	.set Lsd\@, Lscalar_none
	.endif
	.if \far
scalar_entry \name, \forward, (17 - 1), Lsd\@, Lscalar_none, Lscalar_none, Lscalar_none, 2
	ldr x8, [_stream]
	ldr x9, [_stream, #8]
	and x19, x8, #0xffff
	asr x10, x8, #16
	add x7, x9, x10
	branch_if_cross_page x7, \bytes, x11, Lliteral_integer_fast_cross\@
	add _stream, _stream, #8
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \forward
	\instruction \host_register\()0, [x7]
	.if !\discard
	str x0, [_vcpu, x19]
	.endif
	add _stream, _stream, #8
	next_gadget

Lliteral_integer_fast_cross\@:
	add _stream, _stream, #8
	transfer_slow 0, x7, \bytes
	.else
scalar_entry \name, \forward, (17 - 1), Lsd\@, Lscalar_none, Lscalar_none, Lscalar_none, 1
	ldr x8, [_stream], #8
	ubfx x7, x8, #0, #48
	.if !\discard
	ubfx x19, x8, #48, #8
	.endif
	branch_if_cross_page x7, \bytes, x11, Lliteral_integer_fast_cross\@
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	\instruction \host_register\()0, [x7]
	.if !\discard
	str x0, [_vcpu, x19]
	.endif
	next_gadget

Lliteral_integer_fast_cross\@:
	transfer_slow 0, x7, \bytes, fault=uml_arm64_gadget_memory_fault_indexed
	.endif
	.if \bytes == 8
	fmov x0, d0
	.else
	fmov w0, s0
	.endif
	.if \extension
	sxtw x0, w0
	.endif
	.if !\discard
	str x0, [_vcpu, x19]
	.endif
	.if \far
	add _stream, _stream, #8
	.endif
	next_gadget
	scalar_variants \forward, Lscalar_none, Lscalar_none, Lscalar_none, literal_integer_fast, \name, \instruction, \host_register, \bytes, \extension, \discard, \far
.endm

.macro literal_integer_modes name, instruction, host_register, bytes, extension, far=0
	literal_integer_fast \name\()_r, \instruction, \host_register, \bytes, \extension, 0, \far
	literal_integer_fast \name\()_z, \instruction, \host_register, \bytes, \extension, 1, \far
.endm

literal_integer_modes literal_load32_fast, ldr, w, 4, 0
literal_integer_modes literal_load64_fast, ldr, x, 8, 0
literal_integer_modes literal_load32_signed_fast, ldrsw, x, 4, 1
literal_integer_modes literal_load32_far, ldr, w, 4, 0, 1
literal_integer_modes literal_load64_far, ldr, x, 8, 0, 1
literal_integer_modes literal_load32_signed_far, ldrsw, x, 4, 1, 1

.purgem literal_integer_modes
.purgem literal_integer_fast

#if !UML_GADGET_SME_TWIN
.macro literal_vector_fast name, instruction, host_register, bytes, far=0
gadget \name
	.if \far
	ldr x8, [_stream]
	ldr x9, [_stream, #8]
	and x19, x8, #0xffff
	asr x10, x8, #16
	add x7, x9, x10
	branch_if_cross_page x7, \bytes, x11, Lliteral_vector_fast_cross\@
	add _stream, _stream, #8
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE
	\instruction \host_register\()0, [x7]
	str q0, [_vcpu, x19]
	add _stream, _stream, #8
	next_gadget

Lliteral_vector_fast_cross\@:
	add _stream, _stream, #8
	transfer_slow 0, x7, \bytes
	str q0, [_vcpu, x19]
	add _stream, _stream, #8
	next_gadget
	.else
	ldr x8, [_stream], #8
	ubfx x7, x8, #0, #48
	ubfx x19, x8, #48, #6
	branch_if_cross_page x7, \bytes, x11, Lliteral_vector_fast_cross\@
	translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, fault=uml_arm64_gadget_memory_fault_indexed
	\instruction \host_register\()0, [x7]
	str q0, [_vcpu, x19, lsl #4]
	next_gadget

Lliteral_vector_fast_cross\@:
	transfer_slow 0, x7, \bytes, fault=uml_arm64_gadget_memory_fault_indexed
	str q0, [_vcpu, x19, lsl #4]
	next_gadget
	.endif
.endm

literal_vector_fast literal_load_vector32_fast, ldr, s, 4
literal_vector_fast literal_load_vector64_fast, ldr, d, 8
literal_vector_fast literal_load_vector128_fast, ldr, q, 16
literal_vector_fast literal_load_vector32_far, ldr, s, 4, 1
literal_vector_fast literal_load_vector64_far, ldr, d, 8, 1
literal_vector_fast literal_load_vector128_far, ldr, q, 16, 1

.purgem literal_vector_fast
#endif

/* Production word/doubleword unsigned-immediate transfers.  The translator
 * converts both register numbers to CpuState byte offsets and folds XZR into
 * a distinct entry. Stream: one word of three 16-bit fields, [Rt offset, Rn
 * offset, byte displacement], and the instruction's position in its block in
 * bits 63:56, which a fault reports in place of a PC word. The only
 * conditional branches left are the runtime cross-page and
 * TLB/fault decisions; valid same-page hits execute no opcode or register-mode
 * tests. */
.macro scalar_unsigned_fast load, bits64, transfer_zero, base_sp, suffix, baked=0, forward=0
	.set Lsd\@, Lscalar_offset0
	.set Lsm\@, Lscalar_offset0
	.if \baked
	.set Lsd\@, \baked
	.set Lsm\@, \baked
	.endif
	.if !\load || \transfer_zero
	.set Lsd\@, Lscalar_none
	.endif
	.set Lsn\@, Lscalar_offset16
	.if \load || \transfer_zero
	.set Lsm\@, Lscalar_none
	.endif
scalar_entry \suffix, \forward, (17 - \load), Lsd\@, Lsn\@, Lsm\@, Lscalar_none, 1
	.if !\load && (\forward & 2)
	mov x22, x0
	.endif
	ldr x8, [_stream], #8
	.if !\baked
	and x19, x8, #0xffff
	.endif
	ubfx x20, x8, #16, #16
	ubfx x21, x8, #32, #16
	scalar_read \forward, 1, x7, x20
	.if \base_sp
	sp_alignment_check x7, \load, \forward, Lscalar_forward_alignment_read\@, Lscalar_forward_alignment_write\@
	.endif
	add x7, x7, x21
	.if !\base_sp
	/* Top-byte ignore: the TLB is keyed by the untagged address. */
	ldrb w9, [_vcpu, #UML_GADGET_CPU_TBI_ENABLED]
	cbz w9, Lscalar_unsigned_untagged\@
	ubfx x7, x7, #0, #56
Lscalar_unsigned_untagged\@:
	.endif
	.set Lsb\@, 4
	.if \bits64
	.set Lsb\@, 8
	.endif
	.if \load
	transfer_translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, Lsb\@, x7, Ltt_cold\@, Ltt_resume\@, Lscalar_unsigned_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
		.if \bits64
	ldr x0, [x7]
		.else
	ldr w0, [x7]
		.endif
		.if !\transfer_zero
	.if \baked
	str x0, [_vcpu, #(\baked & 0x7fff)]
	.else
	str x0, [_vcpu, x19]
	.endif
		.endif
	.else
	transfer_translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, Lsb\@, x7, Ltt_cold\@, Ltt_resume\@, Lscalar_unsigned_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
		.if \transfer_zero
	mov x0, xzr
		.else
	.if !(\forward & 2)
	.if \baked
	ldr x0, [_vcpu, #(\baked & 0x7fff)]
	.else
	ldr x0, [_vcpu, x19]
	.endif
	.endif
		.endif
		.if \bits64
	scalar_memory_transfer 0, \forward, 2, str, x
		.else
	scalar_memory_transfer 0, \forward, 2, str, w
		.endif
	.endif
	next_gadget
	/* The baked family is one entry per guest register, so whatever each
	 * copy holds is multiplied by 31. Only the fast path differs between
	 * the copies; the TLB-miss and page-crossing tails are two thirds of
	 * an entry and are reached only when the access leaves the cached
	 * page, so they go to a cold section and the fast paths of a mode pack
	 * together. Branches between the two are ordinary direct branches. */
	.if \baked
Ltt_cold\@:
	b Ltt_cold_out\@
Lscalar_unsigned_cross\@:
	b Lscalar_unsigned_cross_out\@
	.pushsection GADGET_SECTION_COLD
	.endif
	.if \load
	.if \baked
	transfer_translate_cold 0, UML_GADGET_TLB_ENTRY_READ_PAGE, Lsb\@, x7, Ltt_cold_out\@, Ltt_resume\@, Lscalar_unsigned_cross_out\@, \forward, outofline=1, fault=uml_arm64_gadget_memory_fault_indexed
	.else
	transfer_translate_cold 0, UML_GADGET_TLB_ENTRY_READ_PAGE, Lsb\@, x7, Ltt_cold\@, Ltt_resume\@, Lscalar_unsigned_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	.endif
	.else
	.if \baked
	transfer_translate_cold 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, Lsb\@, x7, Ltt_cold_out\@, Ltt_resume\@, Lscalar_unsigned_cross_out\@, \forward, outofline=1, fault=uml_arm64_gadget_memory_fault_indexed
	.else
	transfer_translate_cold 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, Lsb\@, x7, Ltt_cold\@, Ltt_resume\@, Lscalar_unsigned_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	.endif
	.endif

	.if \baked
Lscalar_unsigned_cross_out\@:
	.else
Lscalar_unsigned_cross\@:
	.endif
	.if \load
		.if \bits64
	transfer_slow 0, x7, 8, fault=uml_arm64_gadget_memory_fault_indexed
	fmov x0, d0
		.else
	transfer_slow 0, x7, 4, fault=uml_arm64_gadget_memory_fault_indexed
	fmov w0, s0
		.endif
		.if !\transfer_zero
	.if \baked
	str x0, [_vcpu, #(\baked & 0x7fff)]
	.else
	str x0, [_vcpu, x19]
	.endif
		.endif
	.else
		.if \transfer_zero
	mov x0, xzr
		.else
	.if !(\forward & 2)
	.if \baked
	ldr x0, [_vcpu, #(\baked & 0x7fff)]
	.else
	ldr x0, [_vcpu, x19]
	.endif
	.endif
		.endif
		.if \bits64
	scalar_store_vector \forward, 2, 1
	transfer_slow 1, x7, 8, fault=uml_arm64_gadget_memory_fault_indexed
		.else
	scalar_store_vector \forward, 2, 0
	transfer_slow 1, x7, 4, fault=uml_arm64_gadget_memory_fault_indexed
		.endif
	.endif
	next_gadget
	.if \baked
	.popsection
	.endif
	.if \forward
Lscalar_forward_alignment_read\@:
	b Lscalar_alignment_fault_read_indexed
	.endif
	.if \forward
Lscalar_forward_alignment_write\@:
	b Lscalar_alignment_fault_write_indexed
	.endif
	scalar_variants \forward, Lsn\@, Lsm\@, Lscalar_none, scalar_unsigned_fast, \load, \bits64, \transfer_zero, \base_sp, \suffix, \baked
.endm

/* A word or doubleword transfer of a register takes the in-place family,
 * which bakes the register, so only XZR still selects these shared entries. */
.macro scalar_unsigned_modes load, bits64, name
scalar_unsigned_fast \load, \bits64, 1, 0, \name\()_z
scalar_unsigned_fast \load, \bits64, 1, 1, \name\()_z_sp
.endm

scalar_unsigned_modes 1, 0, load32_unsigned_fast
scalar_unsigned_modes 1, 1, load64_unsigned_fast
scalar_unsigned_modes 0, 0, store32_unsigned_fast
scalar_unsigned_modes 0, 1, store64_unsigned_fast

.purgem scalar_unsigned_modes

/* Translation-specialized byte/halfword and sign-extending unsigned-offset
 * scalar accesses. The stream operand carries CpuState byte offsets for Rt/Rn,
 * the already-scaled displacement and the position, laid out as above, so the
 * runtime does not inspect an opcode or branch on register 31. */
.macro scalar_unsigned_load_narrow_fast name, instruction, host_register, bytes, extension, transfer_zero, base_sp, forward=0
	.set Lsd\@, Lscalar_offset0
	.if \transfer_zero
	.set Lsd\@, Lscalar_none
	.endif
	.set Lsn\@, Lscalar_offset16
scalar_entry \name, \forward, (17 - 1), Lsd\@, Lsn\@, Lscalar_none, Lscalar_none, 1
	ldr x8, [_stream], #8
	and x19, x8, #0xffff
	ubfx x20, x8, #16, #16
	ubfx x21, x8, #32, #16
	scalar_read \forward, 1, x7, x20
	.if \base_sp
	sp_alignment_check x7, 1, \forward, Lscalar_forward_alignment_read\@, 0
	.endif
	add x7, x7, x21
	.if !\base_sp
	/* Top-byte ignore: the TLB is keyed by the untagged address. */
	ldrb w9, [_vcpu, #UML_GADGET_CPU_TBI_ENABLED]
	cbz w9, Lscalar_unsigned_narrow_load_untagged\@
	ubfx x7, x7, #0, #56
Lscalar_unsigned_narrow_load_untagged\@:
	.endif
	transfer_translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \bytes, x7, Ltt_cold\@, Ltt_resume\@, Lscalar_unsigned_narrow_load_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	\instruction \host_register\()0, [x7]
	.if !\transfer_zero
	str x0, [_vcpu, x19]
	.endif
	next_gadget
	transfer_translate_cold 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \bytes, x7, Ltt_cold\@, Ltt_resume\@, Lscalar_unsigned_narrow_load_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed

	.if \bytes > 1
Lscalar_unsigned_narrow_load_cross\@:
	transfer_slow 0, x7, \bytes, fault=uml_arm64_gadget_memory_fault_indexed
	.if \bytes == 2
	umov w0, v0.h[0]
	.else
	fmov w0, s0
	.endif
	.if \extension == 1
	sxtb w0, w0
	.elseif \extension == 2
	sxtb x0, w0
	.elseif \extension == 3
	sxth w0, w0
	.elseif \extension == 4
	sxth x0, w0
	.elseif \extension == 5
	sxtw x0, w0
	.endif
	.if !\transfer_zero
	str x0, [_vcpu, x19]
	.endif
	next_gadget
	.endif
	.if \forward
Lscalar_forward_alignment_read\@:
	b Lscalar_alignment_fault_read_indexed
	.endif
	scalar_variants \forward, Lsn\@, Lscalar_none, Lscalar_none, scalar_unsigned_load_narrow_fast, \name, \instruction, \host_register, \bytes, \extension, \transfer_zero, \base_sp
.endm

.macro scalar_unsigned_load_narrow_modes name, instruction, host_register, bytes, extension
	scalar_unsigned_load_narrow_fast \name\()_r, \instruction, \host_register, \bytes, \extension, 0, 0
	scalar_unsigned_load_narrow_fast \name\()_z, \instruction, \host_register, \bytes, \extension, 1, 0
	scalar_unsigned_load_narrow_fast \name\()_r_sp, \instruction, \host_register, \bytes, \extension, 0, 1
	scalar_unsigned_load_narrow_fast \name\()_z_sp, \instruction, \host_register, \bytes, \extension, 1, 1
.endm

scalar_unsigned_load_narrow_modes load8_unsigned_fast, ldrb, w, 1, 0
scalar_unsigned_load_narrow_modes load16_unsigned_fast, ldrh, w, 2, 0
scalar_unsigned_load_narrow_modes load8_signed_w_fast, ldrsb, w, 1, 1
scalar_unsigned_load_narrow_modes load8_signed_x_fast, ldrsb, x, 1, 2
scalar_unsigned_load_narrow_modes load16_signed_w_fast, ldrsh, w, 2, 3
scalar_unsigned_load_narrow_modes load16_signed_x_fast, ldrsh, x, 2, 4
scalar_unsigned_load_narrow_modes load32_signed_x_fast, ldrsw, x, 4, 5

.purgem scalar_unsigned_load_narrow_modes
.purgem scalar_unsigned_load_narrow_fast

.macro scalar_unsigned_store_narrow_fast name, instruction, bytes, transfer_zero, base_sp, forward=0
	.set Lsn\@, Lscalar_offset16
	.set Lsm\@, Lscalar_offset0
	.if \transfer_zero
	.set Lsm\@, Lscalar_none
	.endif
scalar_entry \name, \forward, (17 - 0), Lscalar_none, Lsn\@, Lsm\@, Lscalar_none, 1
	.if !0 && (\forward & 2)
	mov x22, x0
	.endif
	ldr x8, [_stream], #8
	and x19, x8, #0xffff
	ubfx x20, x8, #16, #16
	ubfx x21, x8, #32, #16
	scalar_read \forward, 1, x7, x20
	.if \base_sp
	sp_alignment_check x7, 0, \forward, 0, Lscalar_forward_alignment_write\@
	.endif
	add x7, x7, x21
	.if !\base_sp
	/* Top-byte ignore: the TLB is keyed by the untagged address. */
	ldrb w9, [_vcpu, #UML_GADGET_CPU_TBI_ENABLED]
	cbz w9, Lscalar_unsigned_narrow_store_untagged\@
	ubfx x7, x7, #0, #56
Lscalar_unsigned_narrow_store_untagged\@:
	.endif
	transfer_translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, \bytes, x7, Ltt_cold\@, Ltt_resume\@, Lscalar_unsigned_narrow_store_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	.if \transfer_zero
	mov x0, xzr
	.else
	.if !(\forward & 2)
	ldr x0, [_vcpu, x19]
	.endif
	.endif
	scalar_memory_transfer 0, \forward, 2, \instruction, w
	next_gadget
	transfer_translate_cold 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, \bytes, x7, Ltt_cold\@, Ltt_resume\@, Lscalar_unsigned_narrow_store_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed

	.if \bytes > 1
Lscalar_unsigned_narrow_store_cross\@:
	.if \transfer_zero
	mov x0, xzr
	.else
	.if !(\forward & 2)
	ldr x0, [_vcpu, x19]
	.endif
	.endif
	scalar_store_vector \forward, 2, 1
	transfer_slow 1, x7, \bytes, fault=uml_arm64_gadget_memory_fault_indexed
	next_gadget
	.endif
	.if \forward
Lscalar_forward_alignment_write\@:
	b Lscalar_alignment_fault_write_indexed
	.endif
	scalar_variants \forward, Lsn\@, Lsm\@, Lscalar_none, scalar_unsigned_store_narrow_fast, \name, \instruction, \bytes, \transfer_zero, \base_sp
.endm

.macro scalar_unsigned_store_narrow_modes name, instruction, bytes
	scalar_unsigned_store_narrow_fast \name\()_r, \instruction, \bytes, 0, 0
	scalar_unsigned_store_narrow_fast \name\()_z, \instruction, \bytes, 1, 0
	scalar_unsigned_store_narrow_fast \name\()_r_sp, \instruction, \bytes, 0, 1
	scalar_unsigned_store_narrow_fast \name\()_z_sp, \instruction, \bytes, 1, 1
.endm

scalar_unsigned_store_narrow_modes store8_unsigned_fast, strb, 1
scalar_unsigned_store_narrow_modes store16_unsigned_fast, strh, 2

.purgem scalar_unsigned_store_narrow_modes
.purgem scalar_unsigned_store_narrow_fast

#if !UML_GADGET_SME_TWIN
/* All SIMD&FP unsigned-offset widths use a translated CpuState vector offset,
 * translated base offset, and scaled displacement. Width and direction are
 * fixed by the selected symbol. */
.macro vector_unsigned_fast name, load, instruction, host_register, bytes, base_sp, forward=0
	.set Lsn\@, Lscalar_offset16
scalar_entry \name, \forward, (17 - \load), Lscalar_none, Lsn\@, Lscalar_none, Lscalar_none, 1
	ldr x8, [_stream], #8
	and x19, x8, #0xffff
	ubfx x20, x8, #16, #16
	ubfx x21, x8, #32, #16
	scalar_read \forward, 1, x7, x20
	.if \base_sp
	sp_alignment_check x7, \load, \forward, Lscalar_forward_alignment_read\@, Lscalar_forward_alignment_write\@
	.endif
	add x7, x7, x21
	.if !\base_sp
	ldrb w9, [_vcpu, #UML_GADGET_CPU_TBI_ENABLED]
	cbnz w9, Lvector_unsigned_tagged\@
Lvector_unsigned_translate\@:
	.endif
	.if \load
	transfer_translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \bytes, x7, Ltt_cold\@, Ltt_resume\@, Lvector_unsigned_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	\instruction \host_register\()0, [x7]
	str q0, [_vcpu, x19]
	.else
	transfer_translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, \bytes, x7, Ltt_cold\@, Ltt_resume\@, Lvector_unsigned_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	ldr q0, [_vcpu, x19]
	\instruction \host_register\()0, [x7]
	.endif
	next_gadget
	.if \load
	transfer_translate_cold 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \bytes, x7, Ltt_cold\@, Ltt_resume\@, Lvector_unsigned_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	.else
	transfer_translate_cold 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, \bytes, x7, Ltt_cold\@, Ltt_resume\@, Lvector_unsigned_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	.endif

Lvector_unsigned_cross\@:
	.if \load
	transfer_slow 0, x7, \bytes, fault=uml_arm64_gadget_memory_fault_indexed
	str q0, [_vcpu, x19]
	.else
	ldr q0, [_vcpu, x19]
	transfer_slow 1, x7, \bytes, fault=uml_arm64_gadget_memory_fault_indexed
	.endif
	next_gadget

	.if !\base_sp
Lvector_unsigned_tagged\@:
	/* TBI strips the logical tag and rejoins the fast path. */
	ubfx x7, x7, #0, #56
	b Lvector_unsigned_translate\@
	.endif
	.if \forward
Lscalar_forward_alignment_read\@:
	b Lscalar_alignment_fault_read_indexed
	.endif
	.if \forward
Lscalar_forward_alignment_write\@:
	b Lscalar_alignment_fault_write_indexed
	.endif
	scalar_variants \forward, Lsn\@, Lscalar_none, Lscalar_none, vector_unsigned_fast, \name, \load, \instruction, \host_register, \bytes, \base_sp
.endm

.macro vector_unsigned_modes name, load, instruction, host_register, bytes
	vector_unsigned_fast vector_\name\()_unsigned_fast, \load, \instruction, \host_register, \bytes, 0
	vector_unsigned_fast vector_\name\()_unsigned_fast_sp, \load, \instruction, \host_register, \bytes, 1
.endm

vector_unsigned_modes load8, 1, ldr, b, 1
vector_unsigned_modes load16, 1, ldr, h, 2
vector_unsigned_modes load32, 1, ldr, s, 4
vector_unsigned_modes load64, 1, ldr, d, 8
vector_unsigned_modes load128, 1, ldr, q, 16
vector_unsigned_modes store8, 0, str, b, 1
vector_unsigned_modes store16, 0, str, h, 2
vector_unsigned_modes store32, 0, str, s, 4
vector_unsigned_modes store64, 0, str, d, 8
vector_unsigned_modes store128, 0, str, q, 16

.purgem vector_unsigned_modes
.purgem vector_unsigned_fast
#endif

/* Scalar unscaled/pre/post-indexed accesses. Operand: Rt CpuState offset in
 * bits 15:0, Rn offset in 31:16, signed imm9 in 47:32 and the instruction's
 * position in its block in 63:56. Mode 0 is adjusted
 * address without writeback, mode 1 pre-index, mode 2 post-index, and mode 3
 * post-index with architecturally suppressed overlapping-load writeback. */
.macro scalar_other_immediate_fast name, load, instruction, host_register, bytes, extension, mode, transfer_zero, base_sp, forward=0
	.set Lsd\@, Lscalar_offset0
	.if !\load || \transfer_zero || \mode == 1 || \mode == 2
	.set Lsd\@, Lscalar_none
	.endif
	.set Lsn\@, Lscalar_offset16
	.set Lsm\@, Lscalar_offset0
	.if \load || \transfer_zero
	.set Lsm\@, Lscalar_none
	.endif
scalar_entry \name, \forward, (17 - \load), Lsd\@, Lsn\@, Lsm\@, Lscalar_none, 1
	.if !\load && (\forward & 2)
	mov x22, x0
	.endif
	ldr x8, [_stream], #8
	and x19, x8, #0xffff
	ubfx x20, x8, #16, #16
	sbfx x21, x8, #32, #16
	scalar_read \forward, 1, x24, x20
	.if \base_sp
	sp_alignment_check x24, \load, \forward, Lscalar_forward_alignment_read\@, Lscalar_forward_alignment_write\@
	.endif
	add x25, x24, x21
	.if \mode >= 2
	mov x26, x24
	.else
	mov x26, x25
	.endif
	.if !\base_sp
	/* Top-byte ignore: the TLB is keyed by the untagged address. */
	ldrb w9, [_vcpu, #UML_GADGET_CPU_TBI_ENABLED]
	cbz w9, Lscalar_other_immediate_untagged\@
	ubfx x26, x26, #0, #56
Lscalar_other_immediate_untagged\@:
	.endif
	mov x7, x26
	.if \load
	transfer_translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \bytes, x7, Ltt_cold\@, Ltt_resume\@, Lscalar_other_immediate_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	scalar_memory_transfer \load, \forward, 2, \instruction, \host_register
		.if !\transfer_zero
	str x0, [_vcpu, x19]
		.endif
	.else
	transfer_translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, \bytes, x7, Ltt_cold\@, Ltt_resume\@, Lscalar_other_immediate_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
		.if \transfer_zero
	mov x0, xzr
		.else
	.if !(\forward & 2)
	ldr x0, [_vcpu, x19]
	.endif
		.endif
	scalar_memory_transfer \load, \forward, 2, \instruction, \host_register
	.endif
	.if \mode == 1 || \mode == 2
	str x25, [_vcpu, x20]
	.endif
	next_gadget
	.if \load
	transfer_translate_cold 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \bytes, x7, Ltt_cold\@, Ltt_resume\@, Lscalar_other_immediate_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	.else
	transfer_translate_cold 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, \bytes, x7, Ltt_cold\@, Ltt_resume\@, Lscalar_other_immediate_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	.endif

	.if \bytes > 1
Lscalar_other_immediate_cross\@:
	.if \load
	transfer_slow 0, x26, \bytes, fault=uml_arm64_gadget_memory_fault_indexed
		.if \bytes == 2
	umov w0, v0.h[0]
		.elseif \bytes == 4
	fmov w0, s0
		.else
	fmov x0, d0
		.endif
		.if \extension == 1
	sxtb w0, w0
		.elseif \extension == 2
	sxtb x0, w0
		.elseif \extension == 3
	sxth w0, w0
		.elseif \extension == 4
	sxth x0, w0
		.elseif \extension == 5
	sxtw x0, w0
		.endif
		.if !\transfer_zero
	str x0, [_vcpu, x19]
		.endif
	.else
		.if \transfer_zero
	mov x0, xzr
		.else
	.if !(\forward & 2)
	ldr x0, [_vcpu, x19]
	.endif
		.endif
	scalar_store_vector \forward, 2, 1
	transfer_slow 1, x26, \bytes, fault=uml_arm64_gadget_memory_fault_indexed
	.endif
	.if \mode == 1 || \mode == 2
	str x25, [_vcpu, x20]
	.endif
	next_gadget
	.endif
	.if \forward
Lscalar_forward_alignment_read\@:
	b Lscalar_alignment_fault_read_indexed
	.endif
	.if \forward
Lscalar_forward_alignment_write\@:
	b Lscalar_alignment_fault_write_indexed
	.endif
	scalar_variants \forward, Lsn\@, Lsm\@, Lscalar_none, scalar_other_immediate_fast, \name, \load, \instruction, \host_register, \bytes, \extension, \mode, \transfer_zero, \base_sp
.endm

.macro scalar_other_immediate_operation name, load, instruction, host_register, bytes, extension
	scalar_other_immediate_fast scalar_other_\name\()_offset_r, \load, \instruction, \host_register, \bytes, \extension, 0, 0, 0
	scalar_other_immediate_fast scalar_other_\name\()_offset_z, \load, \instruction, \host_register, \bytes, \extension, 0, 1, 0
	scalar_other_immediate_fast scalar_other_\name\()_offset_r_sp, \load, \instruction, \host_register, \bytes, \extension, 0, 0, 1
	scalar_other_immediate_fast scalar_other_\name\()_offset_z_sp, \load, \instruction, \host_register, \bytes, \extension, 0, 1, 1
	scalar_other_immediate_fast scalar_other_\name\()_pre_r, \load, \instruction, \host_register, \bytes, \extension, 1, 0, 0
	scalar_other_immediate_fast scalar_other_\name\()_pre_z, \load, \instruction, \host_register, \bytes, \extension, 1, 1, 0
	scalar_other_immediate_fast scalar_other_\name\()_pre_r_sp, \load, \instruction, \host_register, \bytes, \extension, 1, 0, 1
	scalar_other_immediate_fast scalar_other_\name\()_pre_z_sp, \load, \instruction, \host_register, \bytes, \extension, 1, 1, 1
	scalar_other_immediate_fast scalar_other_\name\()_post_r, \load, \instruction, \host_register, \bytes, \extension, 2, 0, 0
	scalar_other_immediate_fast scalar_other_\name\()_post_z, \load, \instruction, \host_register, \bytes, \extension, 2, 1, 0
	scalar_other_immediate_fast scalar_other_\name\()_post_r_sp, \load, \instruction, \host_register, \bytes, \extension, 2, 0, 1
	scalar_other_immediate_fast scalar_other_\name\()_post_z_sp, \load, \instruction, \host_register, \bytes, \extension, 2, 1, 1
	.if \load
	/* Suppressed writeback requires Rn == Rt != 31, so only the ordinary
	 * transfer-register entry is reachable. */
	scalar_other_immediate_fast scalar_other_\name\()_post_suppress_r, \load, \instruction, \host_register, \bytes, \extension, 3, 0, 0
	.endif
.endm

scalar_other_immediate_operation store8, 0, strb, w, 1, 0
scalar_other_immediate_operation load8u, 1, ldrb, w, 1, 0
scalar_other_immediate_operation load8sx, 1, ldrsb, x, 1, 2
scalar_other_immediate_operation load8sw, 1, ldrsb, w, 1, 1
scalar_other_immediate_operation store16, 0, strh, w, 2, 0
scalar_other_immediate_operation load16u, 1, ldrh, w, 2, 0
scalar_other_immediate_operation load16sx, 1, ldrsh, x, 2, 4
scalar_other_immediate_operation load16sw, 1, ldrsh, w, 2, 3
scalar_other_immediate_operation store32, 0, str, w, 4, 0
scalar_other_immediate_operation load32u, 1, ldr, w, 4, 0
scalar_other_immediate_operation load32sx, 1, ldrsw, x, 4, 5
scalar_other_immediate_operation store64, 0, str, x, 8, 0
scalar_other_immediate_operation load64u, 1, ldr, x, 8, 0

.purgem scalar_other_immediate_operation
.purgem scalar_other_immediate_fast

/* Only translation-selected SP forms can reach these cold paths. All GPR-base
 * scalar-memory gadgets retain their prior branch shape. The operand has
 * already been consumed, so _stream points just past the word that holds the
 * instruction's position. */
.p2align 4
Lscalar_alignment_fault_read_indexed:
	stp x1, x2, [sp, #-16]!
	mov x0, x2
	ldr x1, [_vcpu, #UML_GADGET_CPU_SP]
	mov w2, #0
	helper_call uml_arm64_alignment_fault
	ldp x1, x2, [sp], #16
	b uml_arm64_gadget_memory_fault_indexed

.p2align 4
Lscalar_alignment_fault_write_indexed:
	stp x1, x2, [sp, #-16]!
	mov x0, x2
	ldr x1, [_vcpu, #UML_GADGET_CPU_SP]
	mov w2, #1
	helper_call uml_arm64_alignment_fault
	ldp x1, x2, [sp], #16
	b uml_arm64_gadget_memory_fault_indexed

/* Register-offset form. The upper operand half stores the translated Rm
 * CpuState byte offset. Extension (UXTW/X/SXTW) and optional scaling are fixed
 * in the gadget symbol. */
.macro scalar_other_register_fast name, load, instruction, host_register, bytes, address_extension, shifted, result_extension, transfer_zero, base_sp, forward=0
	.set Lsd\@, Lscalar_offset0
	.if !\load || \transfer_zero
	.set Lsd\@, Lscalar_none
	.endif
	.set Lsn\@, Lscalar_offset16
	.set Lsm\@, Lscalar_offset32
	.set Lsa\@, Lscalar_offset0
	.if \load || \transfer_zero
	.set Lsa\@, Lscalar_none
	.endif
scalar_entry \name, \forward, (17 - \load), Lsd\@, Lsn\@, Lsm\@, Lsa\@, 1
	.if !\load && (\forward & 4)
	mov x22, x0
	.endif
	ldr x8, [_stream], #8
	and x19, x8, #0xffff
	ubfx x20, x8, #16, #16
	ubfx x21, x8, #32, #16
	scalar_read \forward, 1, x24, x20
	.if \base_sp
	sp_alignment_check x24, \load, \forward, Lscalar_forward_alignment_read\@, Lscalar_forward_alignment_write\@
	.endif
	scalar_read \forward, 2, x26, x21
	.if \address_extension == 0
	mov w26, w26
	.elseif \address_extension == 2
	sxtw x26, w26
	.endif
	.if \shifted
		.if \bytes == 2
	lsl x26, x26, #1
		.elseif \bytes == 4
	lsl x26, x26, #2
		.elseif \bytes == 8
	lsl x26, x26, #3
		.endif
	.endif
	add x26, x24, x26
	.if !\base_sp
	/* Top-byte ignore: the TLB is keyed by the untagged address. */
	ldrb w9, [_vcpu, #UML_GADGET_CPU_TBI_ENABLED]
	cbz w9, Lscalar_other_register_untagged\@
	ubfx x26, x26, #0, #56
Lscalar_other_register_untagged\@:
	.endif
	mov x7, x26
	.if \load
	transfer_translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \bytes, x7, Ltt_cold\@, Ltt_resume\@, Lscalar_other_register_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	scalar_memory_transfer \load, \forward, 4, \instruction, \host_register
		.if !\transfer_zero
	str x0, [_vcpu, x19]
		.endif
	.else
	transfer_translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, \bytes, x7, Ltt_cold\@, Ltt_resume\@, Lscalar_other_register_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
		.if \transfer_zero
	mov x0, xzr
		.else
	.if !(\forward & 4)
	ldr x0, [_vcpu, x19]
	.endif
		.endif
	scalar_memory_transfer \load, \forward, 4, \instruction, \host_register
	.endif
	next_gadget
	.if \load
	transfer_translate_cold 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \bytes, x7, Ltt_cold\@, Ltt_resume\@, Lscalar_other_register_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	.else
	transfer_translate_cold 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, \bytes, x7, Ltt_cold\@, Ltt_resume\@, Lscalar_other_register_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	.endif

	.if \bytes > 1
Lscalar_other_register_cross\@:
	.if \load
	transfer_slow 0, x26, \bytes, fault=uml_arm64_gadget_memory_fault_indexed
		.if \bytes == 2
	umov w0, v0.h[0]
		.elseif \bytes == 4
	fmov w0, s0
		.else
	fmov x0, d0
		.endif
		.if \result_extension == 1
	sxtb w0, w0
		.elseif \result_extension == 2
	sxtb x0, w0
		.elseif \result_extension == 3
	sxth w0, w0
		.elseif \result_extension == 4
	sxth x0, w0
		.elseif \result_extension == 5
	sxtw x0, w0
		.endif
		.if !\transfer_zero
	str x0, [_vcpu, x19]
		.endif
	.else
		.if \transfer_zero
	mov x0, xzr
		.else
	.if !(\forward & 4)
	ldr x0, [_vcpu, x19]
	.endif
		.endif
	scalar_store_vector \forward, 4, 1
	transfer_slow 1, x26, \bytes, fault=uml_arm64_gadget_memory_fault_indexed
	.endif
	next_gadget
	.endif
	.if \forward
Lscalar_forward_alignment_read\@:
	b Lscalar_alignment_fault_read_indexed
	.endif
	.if \forward
Lscalar_forward_alignment_write\@:
	b Lscalar_alignment_fault_write_indexed
	.endif
	scalar_variants \forward, Lsn\@, Lsm\@, Lsa\@, scalar_other_register_fast, \name, \load, \instruction, \host_register, \bytes, \address_extension, \shifted, \result_extension, \transfer_zero, \base_sp
.endm

.macro scalar_other_register_extension name, load, instruction, host_register, bytes, result_extension, address_extension, extension_name, shifted
	scalar_other_register_fast scalar_other_\name\()_\extension_name\()_\shifted\()_r, \load, \instruction, \host_register, \bytes, \address_extension, \shifted, \result_extension, 0, 0
	scalar_other_register_fast scalar_other_\name\()_\extension_name\()_\shifted\()_z, \load, \instruction, \host_register, \bytes, \address_extension, \shifted, \result_extension, 1, 0
	scalar_other_register_fast scalar_other_\name\()_\extension_name\()_\shifted\()_r_sp, \load, \instruction, \host_register, \bytes, \address_extension, \shifted, \result_extension, 0, 1
	scalar_other_register_fast scalar_other_\name\()_\extension_name\()_\shifted\()_z_sp, \load, \instruction, \host_register, \bytes, \address_extension, \shifted, \result_extension, 1, 1
.endm

/* The final parameter controls result sign extension; address extension is
 * supplied explicitly by scalar_other_register_extension below. */
.macro scalar_other_register_all name, load, instruction, host_register, bytes, result_extension
	scalar_other_register_extension \name, \load, \instruction, \host_register, \bytes, \result_extension, 0, uxtw, 0
	scalar_other_register_extension \name, \load, \instruction, \host_register, \bytes, \result_extension, 1, x, 0
	scalar_other_register_extension \name, \load, \instruction, \host_register, \bytes, \result_extension, 2, sxtw, 0
	scalar_other_register_extension \name, \load, \instruction, \host_register, \bytes, \result_extension, 0, uxtw, 1
	scalar_other_register_extension \name, \load, \instruction, \host_register, \bytes, \result_extension, 1, x, 1
	scalar_other_register_extension \name, \load, \instruction, \host_register, \bytes, \result_extension, 2, sxtw, 1
.endm

scalar_other_register_all store8, 0, strb, w, 1, 0
scalar_other_register_all load8u, 1, ldrb, w, 1, 0
scalar_other_register_all load8sx, 1, ldrsb, x, 1, 2
scalar_other_register_all load8sw, 1, ldrsb, w, 1, 1
scalar_other_register_all store16, 0, strh, w, 2, 0
scalar_other_register_all load16u, 1, ldrh, w, 2, 0
scalar_other_register_all load16sx, 1, ldrsh, x, 2, 4
scalar_other_register_all load16sw, 1, ldrsh, w, 2, 3
scalar_other_register_all store32, 0, str, w, 4, 0
scalar_other_register_all load32u, 1, ldr, w, 4, 0
scalar_other_register_all load32sx, 1, ldrsw, x, 4, 5
scalar_other_register_all store64, 0, str, x, 8, 0
scalar_other_register_all load64u, 1, ldr, x, 8, 0

.purgem scalar_other_register_all
.purgem scalar_other_register_extension
.purgem scalar_other_register_fast

#if !UML_GADGET_SME_TWIN
/* SIMD&FP unscaled/pre/post-indexed and register-offset forms use the same
 * translated address descriptors as scalar memory, with the V-register byte
 * offset in bits 15:0. */
.macro vector_other_immediate_fast name, load, instruction, host_register, bytes, mode, base_sp, forward=0
	.set Lsn\@, Lscalar_offset16
scalar_entry \name, \forward, (17 - \load), Lscalar_none, Lsn\@, Lscalar_none, Lscalar_none, 1
	ldr x8, [_stream], #8
	and x19, x8, #0xffff
	ubfx x20, x8, #16, #16
	sbfx x21, x8, #32, #16
	scalar_read \forward, 1, x24, x20
	.if \base_sp
	sp_alignment_check x24, \load, \forward, Lscalar_forward_alignment_read\@, Lscalar_forward_alignment_write\@
	.endif
	add x25, x24, x21
	.if \mode == 2
	mov x26, x24
	.else
	mov x26, x25
	.endif
	.if !\base_sp || \mode != 0
	ldrb w9, [_vcpu, #UML_GADGET_CPU_TBI_ENABLED]
	cbnz w9, Lvector_other_immediate_tagged\@
	.endif
Lvector_other_immediate_translate\@:
	mov x7, x26
	.if \load
	transfer_translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \bytes, x7, Ltt_cold\@, Ltt_resume\@, Lvector_other_immediate_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	\instruction \host_register\()0, [x7]
	str q0, [_vcpu, x19]
	.else
	transfer_translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, \bytes, x7, Ltt_cold\@, Ltt_resume\@, Lvector_other_immediate_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	ldr q0, [_vcpu, x19]
	\instruction \host_register\()0, [x7]
	.endif
	.if \mode == 1 || \mode == 2
	str x25, [_vcpu, x20]
	.endif
	next_gadget
	.if \load
	transfer_translate_cold 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \bytes, x7, Ltt_cold\@, Ltt_resume\@, Lvector_other_immediate_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	.else
	transfer_translate_cold 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, \bytes, x7, Ltt_cold\@, Ltt_resume\@, Lvector_other_immediate_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	.endif

Lvector_other_immediate_cross\@:
	.if \load
	transfer_slow 0, x26, \bytes, fault=uml_arm64_gadget_memory_fault_indexed
	str q0, [_vcpu, x19]
	.else
	ldr q0, [_vcpu, x19]
	transfer_slow 1, x26, \bytes, fault=uml_arm64_gadget_memory_fault_indexed
	.endif
	.if \mode == 1 || \mode == 2
	str x25, [_vcpu, x20]
	.endif
	next_gadget

	.if !\base_sp || \mode != 0
Lvector_other_immediate_tagged\@:
	/* TBI strips the logical tag and rejoins the fast path. */
	ubfx x26, x26, #0, #56
	b Lvector_other_immediate_translate\@
	.endif
	.if \forward
Lscalar_forward_alignment_read\@:
	b Lscalar_alignment_fault_read_indexed
	.endif
	.if \forward
Lscalar_forward_alignment_write\@:
	b Lscalar_alignment_fault_write_indexed
	.endif
	scalar_variants \forward, Lsn\@, Lscalar_none, Lscalar_none, vector_other_immediate_fast, \name, \load, \instruction, \host_register, \bytes, \mode, \base_sp
.endm

.macro vector_other_immediate_operation name, load, instruction, host_register, bytes
	vector_other_immediate_fast vector_other_\name\()_offset, \load, \instruction, \host_register, \bytes, 0, 0
	vector_other_immediate_fast vector_other_\name\()_pre, \load, \instruction, \host_register, \bytes, 1, 0
	vector_other_immediate_fast vector_other_\name\()_post, \load, \instruction, \host_register, \bytes, 2, 0
	vector_other_immediate_fast vector_other_\name\()_offset_sp, \load, \instruction, \host_register, \bytes, 0, 1
	vector_other_immediate_fast vector_other_\name\()_pre_sp, \load, \instruction, \host_register, \bytes, 1, 1
	vector_other_immediate_fast vector_other_\name\()_post_sp, \load, \instruction, \host_register, \bytes, 2, 1
.endm

.macro vector_other_register_fast name, load, instruction, host_register, bytes, extension, shifted, base_sp, forward=0
	.set Lsn\@, Lscalar_offset16
	.set Lsm\@, Lscalar_offset32
scalar_entry \name, \forward, (17 - \load), Lscalar_none, Lsn\@, Lsm\@, Lscalar_none, 1
	ldr x8, [_stream], #8
	and x19, x8, #0xffff
	ubfx x20, x8, #16, #16
	ubfx x21, x8, #32, #16
	scalar_read \forward, 1, x24, x20
	.if \base_sp
	sp_alignment_check x24, \load, \forward, Lscalar_forward_alignment_read\@, Lscalar_forward_alignment_write\@
	.endif
	scalar_read \forward, 2, x26, x21
	.if \extension == 0
	mov w26, w26
	.elseif \extension == 2
	sxtw x26, w26
	.endif
	.if \shifted
		.if \bytes == 2
	lsl x26, x26, #1
		.elseif \bytes == 4
	lsl x26, x26, #2
		.elseif \bytes == 8
	lsl x26, x26, #3
		.elseif \bytes == 16
	lsl x26, x26, #4
		.endif
	.endif
	add x26, x24, x26
	ldrb w9, [_vcpu, #UML_GADGET_CPU_TBI_ENABLED]
	cbnz w9, Lvector_other_register_tagged\@
Lvector_other_register_translate\@:
	mov x7, x26
	.if \load
	transfer_translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \bytes, x7, Ltt_cold\@, Ltt_resume\@, Lvector_other_register_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	\instruction \host_register\()0, [x7]
	str q0, [_vcpu, x19]
	.else
	transfer_translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, \bytes, x7, Ltt_cold\@, Ltt_resume\@, Lvector_other_register_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	ldr q0, [_vcpu, x19]
	\instruction \host_register\()0, [x7]
	.endif
	next_gadget
	.if \load
	transfer_translate_cold 0, UML_GADGET_TLB_ENTRY_READ_PAGE, \bytes, x7, Ltt_cold\@, Ltt_resume\@, Lvector_other_register_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	.else
	transfer_translate_cold 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, \bytes, x7, Ltt_cold\@, Ltt_resume\@, Lvector_other_register_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	.endif

Lvector_other_register_cross\@:
	.if \load
	transfer_slow 0, x26, \bytes, fault=uml_arm64_gadget_memory_fault_indexed
	str q0, [_vcpu, x19]
	.else
	ldr q0, [_vcpu, x19]
	transfer_slow 1, x26, \bytes, fault=uml_arm64_gadget_memory_fault_indexed
	.endif
	next_gadget

Lvector_other_register_tagged\@:
	/* TBI strips the logical tag and rejoins the fast path. */
	ubfx x26, x26, #0, #56
	b Lvector_other_register_translate\@
	.if \forward
Lscalar_forward_alignment_read\@:
	b Lscalar_alignment_fault_read_indexed
	.endif
	.if \forward
Lscalar_forward_alignment_write\@:
	b Lscalar_alignment_fault_write_indexed
	.endif
	scalar_variants \forward, Lsn\@, Lsm\@, Lscalar_none, vector_other_register_fast, \name, \load, \instruction, \host_register, \bytes, \extension, \shifted, \base_sp
.endm

.macro vector_other_register_extension name, load, instruction, host_register, bytes, extension, extension_name, shifted
	vector_other_register_fast vector_other_\name\()_\extension_name\()_\shifted, \load, \instruction, \host_register, \bytes, \extension, \shifted, 0
	vector_other_register_fast vector_other_\name\()_\extension_name\()_\shifted\()_sp, \load, \instruction, \host_register, \bytes, \extension, \shifted, 1
.endm

.macro vector_other_operation name, load, instruction, host_register, bytes
	vector_other_immediate_operation \name, \load, \instruction, \host_register, \bytes
	vector_other_register_extension \name, \load, \instruction, \host_register, \bytes, 0, uxtw, 0
	vector_other_register_extension \name, \load, \instruction, \host_register, \bytes, 1, x, 0
	vector_other_register_extension \name, \load, \instruction, \host_register, \bytes, 2, sxtw, 0
	vector_other_register_extension \name, \load, \instruction, \host_register, \bytes, 0, uxtw, 1
	vector_other_register_extension \name, \load, \instruction, \host_register, \bytes, 1, x, 1
	vector_other_register_extension \name, \load, \instruction, \host_register, \bytes, 2, sxtw, 1
.endm

vector_other_operation store8, 0, str, b, 1
vector_other_operation load8, 1, ldr, b, 1
vector_other_operation store16, 0, str, h, 2
vector_other_operation load16, 1, ldr, h, 2
vector_other_operation store32, 0, str, s, 4
vector_other_operation load32, 1, ldr, s, 4
vector_other_operation store64, 0, str, d, 8
vector_other_operation load64, 1, ldr, d, 8
vector_other_operation store128, 0, str, q, 16
vector_other_operation load128, 1, ldr, q, 16

.purgem vector_other_operation
.purgem vector_other_register_extension
.purgem vector_other_register_fast
.purgem vector_other_immediate_operation
.purgem vector_other_immediate_fast
#endif

/* Translation-specialized integer pair transfers. The packed operand holds
 * the Rt and Rt2 CpuState byte offsets in two 9-bit fields, the signed,
 * already-scaled displacement in the 14 bits above them, the Rn offset in bits
 * 32 to 47, and the instruction's position in its block in the top eight bits,
 * which a fault reports in place of a PC word. Width, direction,
 * writeback mode, sign extension, SP versus GPR base, and both XZR cases are
 * fixed by the symbol. Mode 0 is adjusted address without writeback, mode 1
 * pre-index, mode 2 post-index, and mode 3 post-index with suppressed load
 * writeback. */
.macro integer_pair_fast name, load, instruction, host_register, bytes, sign_extend, mode, first_zero, second_zero, sp_base, forward=0
	.set Lsn\@, Lscalar_offset32
scalar_entry \name, \forward, (17 - \load), Lscalar_none, Lsn\@, Lscalar_none, Lscalar_none, 1
	ldr x8, [_stream], #8
	and x22, x8, #0x1ff
	ubfx x23, x8, #9, #9
	ubfx x24, x8, #32, #16
	sbfx x6, x8, #18, #14
	scalar_read \forward, 1, x16, x24
	.if \sp_base
	sp_alignment_check x16, \load, \forward, Lscalar_forward_alignment_pair_read\@, Lscalar_forward_alignment_pair_write\@, uml_arm64_gadget_pair_alignment_fault_read_indexed, uml_arm64_gadget_pair_alignment_fault_write_indexed
	.endif
	add x21, x16, x6
	.if \mode >= 2
	mov x19, x16
	.else
	mov x19, x21
	.endif
	.if !\sp_base
	/* TBI keeps the ordinary pair fast path after stripping the logical
	 * tag from the effective address. An SP base keeps its tag here and the
	 * TLB entry is keyed by it (Tlb::translate_tagged). */
	ldrb w9, [_vcpu, #UML_GADGET_CPU_TBI_ENABLED]
	cbz w9, Linteger_pair_untagged\@
	ubfx x19, x19, #0, #56
Linteger_pair_untagged\@:
	.endif
	/* Comparing the page bits of the first and final transfer bytes catches
	 * both page crossing and address wraparound with one hot-path branch. */
	.if \load
	transfer_translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, (2 * \bytes), x19, Ltt_cold\@, Ltt_resume\@, Linteger_pair_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	\instruction \host_register\()6, \host_register\()7, [x19]
		.if !\first_zero
	str x6, [_vcpu, x22]
		.endif
		.if !\second_zero
	str x7, [_vcpu, x23]
		.endif
	.else
	transfer_translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, (2 * \bytes), x19, Ltt_cold\@, Ltt_resume\@, Linteger_pair_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
		.if \first_zero
	mov x6, xzr
		.else
	ldr x6, [_vcpu, x22]
		.endif
		.if \second_zero
	mov x7, xzr
		.else
	ldr x7, [_vcpu, x23]
		.endif
	\instruction \host_register\()6, \host_register\()7, [x19]
	.endif
	.if \mode == 1 || \mode == 2
	str x21, [_vcpu, x24]
	.endif
	next_gadget
	.if \load
	transfer_translate_cold 0, UML_GADGET_TLB_ENTRY_READ_PAGE, (2 * \bytes), x19, Ltt_cold\@, Ltt_resume\@, Linteger_pair_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	.else
	transfer_translate_cold 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, (2 * \bytes), x19, Ltt_cold\@, Ltt_resume\@, Linteger_pair_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	.endif

Linteger_pair_cross\@:
	.if \load
	transfer_slow 0, x19, (\bytes * 2), fault=uml_arm64_gadget_memory_fault_indexed
		.if \bytes == 4
	fmov w6, s0
#if UML_GADGET_SME_TWIN
	/* A lane move other than element 0 is AdvSIMD, which streaming
	 * mode does not keep; FMOV is kept. */
	fmov x7, d0
	lsr x7, x7, #32
#else
	umov w7, v0.s[1]
#endif
			.if \sign_extend
	sxtw x6, w6
	sxtw x7, w7
			.endif
		.else
	fmov x6, d0
#if UML_GADGET_SME_TWIN
	fmov x7, v0.d[1]
#else
	umov x7, v0.d[1]
#endif
		.endif
		.if !\first_zero
	str x6, [_vcpu, x22]
		.endif
		.if !\second_zero
	str x7, [_vcpu, x23]
		.endif
	.else
		.if \first_zero
	mov x6, xzr
		.else
	ldr x6, [_vcpu, x22]
		.endif
		.if \second_zero
	mov x7, xzr
		.else
	ldr x7, [_vcpu, x23]
		.endif
		.if \bytes == 4
#if UML_GADGET_SME_TWIN
	mov w9, w6
	bfi x9, x7, #32, #32
	fmov d0, x9
#else
	fmov s0, w6
	mov v0.s[1], w7
#endif
		.else
	fmov d0, x6
#if UML_GADGET_SME_TWIN
	fmov v0.d[1], x7
#else
	mov v0.d[1], x7
#endif
		.endif
	transfer_slow 1, x19, (\bytes * 2), fault=uml_arm64_gadget_memory_fault_indexed
	.endif
	.if \mode == 1 || \mode == 2
	str x21, [_vcpu, x24]
	.endif
	next_gadget

	.if \forward
Lscalar_forward_alignment_pair_read\@:
	b uml_arm64_gadget_pair_alignment_fault_read_indexed
	.endif
	.if \forward
Lscalar_forward_alignment_pair_write\@:
	b uml_arm64_gadget_pair_alignment_fault_write_indexed
	.endif
	scalar_variants \forward, Lsn\@, Lscalar_none, Lscalar_none, integer_pair_fast, \name, \load, \instruction, \host_register, \bytes, \sign_extend, \mode, \first_zero, \second_zero, \sp_base
.endm

.macro integer_pair_mode name, load, instruction, host_register, bytes, sign_extend, mode
	integer_pair_fast integer_pair_\name\()_\mode\()_rr, \load, \instruction, \host_register, \bytes, \sign_extend, \mode, 0, 0, 0
	integer_pair_fast integer_pair_\name\()_\mode\()_rz, \load, \instruction, \host_register, \bytes, \sign_extend, \mode, 0, 1, 0
	integer_pair_fast integer_pair_\name\()_\mode\()_zr, \load, \instruction, \host_register, \bytes, \sign_extend, \mode, 1, 0, 0
	integer_pair_fast integer_pair_\name\()_\mode\()_zz, \load, \instruction, \host_register, \bytes, \sign_extend, \mode, 1, 1, 0
	integer_pair_fast integer_pair_\name\()_\mode\()_rr_sp, \load, \instruction, \host_register, \bytes, \sign_extend, \mode, 0, 0, 1
	integer_pair_fast integer_pair_\name\()_\mode\()_rz_sp, \load, \instruction, \host_register, \bytes, \sign_extend, \mode, 0, 1, 1
	integer_pair_fast integer_pair_\name\()_\mode\()_zr_sp, \load, \instruction, \host_register, \bytes, \sign_extend, \mode, 1, 0, 1
	integer_pair_fast integer_pair_\name\()_\mode\()_zz_sp, \load, \instruction, \host_register, \bytes, \sign_extend, \mode, 1, 1, 1
.endm

.macro integer_pair_store_operation name, instruction, host_register, bytes
	integer_pair_mode \name, 0, \instruction, \host_register, \bytes, 0, 0
	integer_pair_mode \name, 0, \instruction, \host_register, \bytes, 0, 1
	integer_pair_mode \name, 0, \instruction, \host_register, \bytes, 0, 2
.endm

.macro integer_pair_load_operation name, instruction, host_register, bytes, sign_extend
	integer_pair_mode \name, 1, \instruction, \host_register, \bytes, \sign_extend, 0
	integer_pair_mode \name, 1, \instruction, \host_register, \bytes, \sign_extend, 1
	integer_pair_mode \name, 1, \instruction, \host_register, \bytes, \sign_extend, 2
	/* Mode 3 is post-index with WBSUPPRESS and is load-only. */
	integer_pair_mode \name, 1, \instruction, \host_register, \bytes, \sign_extend, 3
.endm

integer_pair_store_operation store32, stp, w, 4
integer_pair_load_operation load32, ldp, w, 4, 0
integer_pair_load_operation load32s, ldpsw, x, 4, 1
integer_pair_store_operation store64, stp, x, 8
integer_pair_load_operation load64, ldp, x, 8, 0

.purgem integer_pair_load_operation
.purgem integer_pair_store_operation
.purgem integer_pair_mode
.purgem integer_pair_fast

#if !UML_GADGET_SME_TWIN
/* Translation-specialized SIMD&FP pairs. The packed operand holds the Rt and
 * Rt2 VectorRegister byte offsets and the Rn offset in three 12-bit fields,
 * the signed, already-scaled displacement in the 12 bits above them, and the
 * instruction's position in its block in the top eight bits, which a fault
 * reports in place of a PC word. Width, direction, address mode, and SP
 * versus GPR base are fixed by the selected symbol. */
.macro vector_pair_fast name, load, instruction, host_register, bytes, mode, sp_base, forward=0
	.set Lsn\@, Lscalar_offset12_24
scalar_entry \name, \forward, (17 - \load), Lscalar_none, Lsn\@, Lscalar_none, Lscalar_none, 1
	ldr x8, [_stream], #8
	and x22, x8, #0xfff
	ubfx x23, x8, #12, #12
	ubfx x24, x8, #24, #12
	sbfx x6, x8, #36, #12
	scalar_read \forward, 1, x16, x24
	.if \sp_base
	sp_alignment_check x16, \load, \forward, Lscalar_forward_alignment_pair_read\@, Lscalar_forward_alignment_pair_write\@, uml_arm64_gadget_pair_alignment_fault_read_indexed, uml_arm64_gadget_pair_alignment_fault_write_indexed
	.endif
	add x21, x16, x6
	.if \mode == 2
	mov x19, x16
	.else
	mov x19, x21
	.endif
	.if !\sp_base || \mode != 0
	ldrb w9, [_vcpu, #UML_GADGET_CPU_TBI_ENABLED]
	cbnz w9, Lvector_pair_tagged\@
Lvector_pair_translate\@:
	.endif
	.if \load
	transfer_translate 0, UML_GADGET_TLB_ENTRY_READ_PAGE, (2 * \bytes), x19, Ltt_cold\@, Ltt_resume\@, Lvector_pair_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	\instruction \host_register\()0, \host_register\()1, [x19]
	str q0, [_vcpu, x22]
	str q1, [_vcpu, x23]
	.else
	transfer_translate 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, (2 * \bytes), x19, Ltt_cold\@, Ltt_resume\@, Lvector_pair_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	ldr q0, [_vcpu, x22]
	ldr q1, [_vcpu, x23]
	\instruction \host_register\()0, \host_register\()1, [x19]
	.endif
	.if \mode == 1 || \mode == 2
	str x21, [_vcpu, x24]
	.endif
	next_gadget
	.if \load
	transfer_translate_cold 0, UML_GADGET_TLB_ENTRY_READ_PAGE, (2 * \bytes), x19, Ltt_cold\@, Ltt_resume\@, Lvector_pair_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	.else
	transfer_translate_cold 1, UML_GADGET_TLB_ENTRY_WRITE_PAGE, (2 * \bytes), x19, Ltt_cold\@, Ltt_resume\@, Lvector_pair_cross\@, \forward, fault=uml_arm64_gadget_memory_fault_indexed
	.endif

Lvector_pair_cross\@:
	.if \load
	transfer_slow 0, x19, (\bytes * 2), fault=uml_arm64_gadget_memory_fault_indexed
		.if \bytes == 4
	movi v1.16b, #0
	mov v1.s[0], v0.s[1]
	movi v2.16b, #0
	mov v2.s[0], v0.s[0]
	mov v0.16b, v2.16b
		.elseif \bytes == 8
	movi v1.16b, #0
	mov v1.d[0], v0.d[1]
	movi v2.16b, #0
	mov v2.d[0], v0.d[0]
	mov v0.16b, v2.16b
		.endif
	str q0, [_vcpu, x22]
	str q1, [_vcpu, x23]
	.else
	ldr q0, [_vcpu, x22]
	ldr q1, [_vcpu, x23]
		.if \bytes == 4
	mov v0.s[1], v1.s[0]
		.elseif \bytes == 8
	mov v0.d[1], v1.d[0]
		.endif
	transfer_slow 1, x19, (\bytes * 2), fault=uml_arm64_gadget_memory_fault_indexed
	.endif
	.if \mode == 1 || \mode == 2
	str x21, [_vcpu, x24]
	.endif
	next_gadget

	.if !\sp_base || \mode != 0
Lvector_pair_tagged\@:
	/* TBI strips the logical tag and rejoins the fast path. */
	ubfx x19, x19, #0, #56
	b Lvector_pair_translate\@
	.endif
	.if \forward
Lscalar_forward_alignment_pair_read\@:
	b uml_arm64_gadget_pair_alignment_fault_read_indexed
	.endif
	.if \forward
Lscalar_forward_alignment_pair_write\@:
	b uml_arm64_gadget_pair_alignment_fault_write_indexed
	.endif
	scalar_variants \forward, Lsn\@, Lscalar_none, Lscalar_none, vector_pair_fast, \name, \load, \instruction, \host_register, \bytes, \mode, \sp_base
.endm

.macro vector_pair_operation name, load, instruction, host_register, bytes
	vector_pair_fast vector_pair_\name\()_0, \load, \instruction, \host_register, \bytes, 0, 0
	vector_pair_fast vector_pair_\name\()_1, \load, \instruction, \host_register, \bytes, 1, 0
	vector_pair_fast vector_pair_\name\()_2, \load, \instruction, \host_register, \bytes, 2, 0
	vector_pair_fast vector_pair_\name\()_0_sp, \load, \instruction, \host_register, \bytes, 0, 1
	vector_pair_fast vector_pair_\name\()_1_sp, \load, \instruction, \host_register, \bytes, 1, 1
	vector_pair_fast vector_pair_\name\()_2_sp, \load, \instruction, \host_register, \bytes, 2, 1
.endm

vector_pair_operation store32, 0, stp, s, 4
vector_pair_operation load32, 1, ldp, s, 4
vector_pair_operation store64, 0, stp, d, 8
vector_pair_operation load64, 1, ldp, d, 8
vector_pair_operation store128, 0, stp, q, 16
vector_pair_operation load128, 1, ldp, q, 16

.purgem vector_pair_operation
.purgem vector_pair_fast
#endif

/* Only SP-specialized pair gadgets can reach these cold paths. GPR-base
 * gadgets contain no alignment test or branch. */
.p2align 4
uml_arm64_gadget_pair_alignment_fault_read_indexed:
	stp x1, x2, [sp, #-16]!
	mov x0, x2
	mov x1, x16
	mov w2, #0
	helper_call uml_arm64_alignment_fault
	ldp x1, x2, [sp], #16
	b uml_arm64_gadget_memory_fault_indexed

.p2align 4
uml_arm64_gadget_pair_alignment_fault_write_indexed:
	stp x1, x2, [sp, #-16]!
	mov x0, x2
	mov x1, x16
	mov w2, #1
	helper_call uml_arm64_alignment_fault
	ldp x1, x2, [sp], #16
	b uml_arm64_gadget_memory_fault_indexed

uml_arm64_gadget_memory_fault:
	ldr x8, [_stream]
	str _stream, [_vcpu, #UML_GADGET_CPU_INDIRECT_MISS]
	str x8, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, #UML_GADGET_EVENT_MEMORY_FAULT
	b uml_arm64_gadget_leave

/* A transfer that carries no PC word has one operand, just consumed, that
 * holds the instruction's position in its block in its top eight bits
 * instead. Report the position with bit 0 set, which no instruction address
 * has, and the stream word it came from; the C++ boundary turns the two into
 * the PC using that block's runs. */
uml_arm64_gadget_memory_fault_indexed:
	ldr x8, [_stream, #-8]!
	lsr x8, x8, #56
	lsl x8, x8, #2
	orr x8, x8, #1
	str _stream, [_vcpu, #UML_GADGET_CPU_INDIRECT_MISS]
	str x8, [_vcpu, #UML_GADGET_CPU_PC]
	mov w0, #UML_GADGET_EVENT_MEMORY_FAULT
	b uml_arm64_gadget_leave
