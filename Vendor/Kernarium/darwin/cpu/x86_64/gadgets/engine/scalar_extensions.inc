/* SPDX-License-Identifier: GPL-2.0-or-later */
/* MOVBE, CRC32C, ADCX, and ADOX. */

.macro reverse_scalar width, destination, source
	.if \width == 16
	rev16 \destination, \source
	uxth \destination, \destination
	.elseif \width == 32
	rev \destination, \source
	.else
	rev \destination, \source
	.endif
.endm

.macro movbe_load width, load, host_register, store, store_register
gadget movbe_\width\()_rm
	effective_address
	mov x25, x7
	read_operand (\width / 8), \load, \host_register, 24
	reverse_scalar \width, \host_register\()24, \host_register\()24
	and x8, x23, #0xffff
	\store \store_register\()24, [_vcpu, x8]
	retire_and_next
.endm

.macro movbe_store width, load, host_register, store
gadget movbe_\width\()_mr
	effective_address
	mov x25, x7
	and x8, x23, #0xffff
	\load \host_register\()24, [_vcpu, x8]
	reverse_scalar \width, \host_register\()24, \host_register\()24
	page_cross_check (\width / 8), Lx86_movbe_store_cross\@
	translate 1, UML_X86_64_TLB_WRITE_PAGE, (\width / 8)
	\store \host_register\()24, [x7]
	finish_store (\width / 8)
Lx86_movbe_store_cross\@:
	cross_page_store x24, (\width / 8)
	finish_store (\width / 8)
.endm

movbe_load 16, ldrh, w, strh, w
movbe_load 32, ldr, w, str, x
movbe_load 64, ldr, x, str, x
movbe_store 16, ldrh, w, strh
movbe_store 32, ldr, w, str
movbe_store 64, ldr, x, str

.purgem movbe_store
.purgem movbe_load
.purgem reverse_scalar

.arch_extension crc

.macro crc32_compute width, instruction, load, host_register
	ldr w24, [_vcpu, x25]
	\instruction w24, w24, \host_register\()15
	str x24, [_vcpu, x25]
.endm

.macro crc32_register width, instruction, load, host_register
gadget crc32_\width\()_rr
	ldr x8, [_stream], #8
	and x25, x8, #0xffff
	ubfx x9, x8, #16, #16
	\load \host_register\()15, [_vcpu, x9]
	crc32_compute \width, \instruction, \load, \host_register
	retire_and_next
.endm

.macro crc32_memory width, instruction, load, host_register
gadget crc32_\width\()_rm
	effective_address
	mov x25, x7
	read_operand (\width / 8), \load, \host_register, 15
	and x25, x23, #0xffff
	crc32_compute \width, \instruction, \load, \host_register
	retire_and_next
.endm

crc32_register 8, crc32cb, ldrb, w
crc32_register 16, crc32ch, ldrh, w
crc32_register 32, crc32cw, ldr, w
crc32_register 64, crc32cx, ldr, x
crc32_memory 8, crc32cb, ldrb, w
crc32_memory 16, crc32ch, ldrh, w
crc32_memory 32, crc32cw, ldr, w
crc32_memory 64, crc32cx, ldr, x

.purgem crc32_memory
.purgem crc32_register
.purgem crc32_compute

.macro adx_compute flag_bit, width, host_register, store_register
	bl uml_x86_64_gadget_materialize_flags
	ldr x10, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	ubfx w8, w10, #\flag_bit, #1
	cmp w8, #1
	adcs \host_register\()27, \host_register\()24, \host_register\()26
	cset w9, cs
	.if \width == 32
	mov w27, w27
	.endif
	str \store_register\()27, [_vcpu, x25]
	mov x8, #1
	lsl x8, x8, #\flag_bit
	bic x10, x10, x8
	cbz w9, Lx86_adx_flag_ready\@
	orr x10, x10, x8
Lx86_adx_flag_ready\@:
	orr x10, x10, #0x2
	str x10, [_vcpu, #UML_X86_64_CPU_RFLAGS]
.endm

.macro adx_register name, flag_bit, width, load, host_register, store_register
gadget \name\()_\width\()_rr
	ldr x8, [_stream], #8
	and x25, x8, #0xffff
	ubfx x9, x8, #16, #16
	\load \host_register\()24, [_vcpu, x25]
	\load \host_register\()26, [_vcpu, x9]
	adx_compute \flag_bit, \width, \host_register, \store_register
	retire_and_next
.endm

.macro adx_memory name, flag_bit, width, load, host_register, store_register
gadget \name\()_\width\()_rm
	effective_address
	mov x25, x7
	read_operand (\width / 8), \load, \host_register, 26
	and x25, x23, #0xffff
	\load \host_register\()24, [_vcpu, x25]
	adx_compute \flag_bit, \width, \host_register, \store_register
	retire_and_next
.endm

.macro adx_width name, flag_bit, width, load, host_register, store_register
	adx_register \name, \flag_bit, \width, \load, \host_register, \store_register
	adx_memory \name, \flag_bit, \width, \load, \host_register, \store_register
.endm

adx_width adcx, 0, 32, ldr, w, x
adx_width adcx, 0, 64, ldr, x, x
adx_width adox, 11, 32, ldr, w, x
adx_width adox, 11, 64, ldr, x, x

.purgem adx_width
.purgem adx_memory
.purgem adx_register
.purgem adx_compute
