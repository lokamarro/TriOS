/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SSSE3/SSE4/AVX2 packed-integer operations from opcode map 0F 38. */

.macro vector_map2_horizontal element, subtract, saturate, width, output, left, right
	.if \width == 64
	.if \element == 16
	rev32 v6.4h, v\left\().4h
	rev32 v7.4h, v\right\().4h
	.if \subtract
	.if \saturate
	sqsub v6.4h, v\left\().4h, v6.4h
	sqsub v7.4h, v\right\().4h, v7.4h
	.else
	sub v6.4h, v\left\().4h, v6.4h
	sub v7.4h, v\right\().4h, v7.4h
	.endif
	.else
	.if \saturate
	sqadd v6.4h, v\left\().4h, v6.4h
	sqadd v7.4h, v\right\().4h, v7.4h
	.else
	add v6.4h, v\left\().4h, v6.4h
	add v7.4h, v\right\().4h, v7.4h
	.endif
	.endif
	uzp1 v\output\().4h, v6.4h, v7.4h
	.else
	rev64 v6.2s, v\left\().2s
	rev64 v7.2s, v\right\().2s
	.if \subtract
	sub v6.2s, v\left\().2s, v6.2s
	sub v7.2s, v\right\().2s, v7.2s
	.else
	add v6.2s, v\left\().2s, v6.2s
	add v7.2s, v\right\().2s, v7.2s
	.endif
	uzp1 v\output\().2s, v6.2s, v7.2s
	.endif
	.else
	.if \element == 16
	rev32 v6.8h, v\left\().8h
	rev32 v7.8h, v\right\().8h
	.if \subtract
	.if \saturate
	sqsub v6.8h, v\left\().8h, v6.8h
	sqsub v7.8h, v\right\().8h, v7.8h
	.else
	sub v6.8h, v\left\().8h, v6.8h
	sub v7.8h, v\right\().8h, v7.8h
	.endif
	.else
	.if \saturate
	sqadd v6.8h, v\left\().8h, v6.8h
	sqadd v7.8h, v\right\().8h, v7.8h
	.else
	add v6.8h, v\left\().8h, v6.8h
	add v7.8h, v\right\().8h, v7.8h
	.endif
	.endif
	uzp1 v\output\().8h, v6.8h, v7.8h
	.else
	rev64 v6.4s, v\left\().4s
	rev64 v7.4s, v\right\().4s
	.if \subtract
	sub v6.4s, v\left\().4s, v6.4s
	sub v7.4s, v\right\().4s, v7.4s
	.else
	add v6.4s, v\left\().4s, v6.4s
	add v7.4s, v\right\().4s, v7.4s
	.endif
	uzp1 v\output\().4s, v6.4s, v7.4s
	.endif
	.endif
.endm

.macro vector_map2_maddubs output, left, right
	uxtl v6.8h, v\left\().8b
	sxtl v7.8h, v\right\().8b
	mul v6.8h, v6.8h, v7.8h
	saddlp v6.4s, v6.8h
	uxtl2 v7.8h, v\left\().16b
	sxtl2 v8.8h, v\right\().16b
	mul v7.8h, v7.8h, v8.8h
	saddlp v7.4s, v7.8h
	sqxtn v\output\().4h, v6.4s
	sqxtn2 v\output\().8h, v7.4s
.endm

.macro vector_map2_sign element, output, left, right
	.if \element == 8
	cmgt v6.16b, v\right\().16b, #0
	cmlt v7.16b, v\right\().16b, #0
	neg v8.16b, v\left\().16b
	.elseif \element == 16
	cmgt v6.8h, v\right\().8h, #0
	cmlt v7.8h, v\right\().8h, #0
	neg v8.8h, v\left\().8h
	.else
	cmgt v6.4s, v\right\().4s, #0
	cmlt v7.4s, v\right\().4s, #0
	neg v8.4s, v\left\().4s
	.endif
	and v\output\().16b, v\left\().16b, v6.16b
	and v8.16b, v8.16b, v7.16b
	orr v\output\().16b, v\output\().16b, v8.16b
.endm

/* SQRDMULH rounds exactly as PMULHRSW does in every lane but one: 0x8000
 * times 0x8000 saturates to 0x7fff on the host where x86 wraps to 0x8000.
 * Both inputs equal 0x8000 in that lane and in no other, and inverting the
 * saturated value gives the wrapped one. The mask comes first so an output
 * that aliases an input is still compared before it is written. */
.macro vector_map2_mulhrs output, left, right
	movi v6.8h, #0x80, lsl #8
	cmeq v7.8h, v\left\().8h, v6.8h
	cmeq v6.8h, v\right\().8h, v6.8h
	and v6.16b, v6.16b, v7.16b
	sqrdmulh v\output\().8h, v\left\().8h, v\right\().8h
	eor v\output\().16b, v\output\().16b, v6.16b
.endm

.macro vector_map2_extend opcode, width
	.if \opcode == 0x20 || \opcode == 0x30
	.if \opcode == 0x20
	sxtl v4.8h, v1.8b
	.else
	uxtl v4.8h, v1.8b
	.endif
	.if \width == 256
	.if \opcode == 0x20
	sxtl2 v5.8h, v1.16b
	.else
	uxtl2 v5.8h, v1.16b
	.endif
	.endif
	.elseif \opcode == 0x21 || \opcode == 0x31
	.if \opcode == 0x21
	sxtl v6.8h, v1.8b
	sxtl v4.4s, v6.4h
	.else
	uxtl v6.8h, v1.8b
	uxtl v4.4s, v6.4h
	.endif
	.if \width == 256
	.if \opcode == 0x21
	sxtl2 v5.4s, v6.8h
	.else
	uxtl2 v5.4s, v6.8h
	.endif
	.endif
	.elseif \opcode == 0x22 || \opcode == 0x32
	.if \opcode == 0x22
	sxtl v6.8h, v1.8b
	sxtl v7.4s, v6.4h
	sxtl v4.2d, v7.2s
	.else
	uxtl v6.8h, v1.8b
	uxtl v7.4s, v6.4h
	uxtl v4.2d, v7.2s
	.endif
	.if \width == 256
	.if \opcode == 0x22
	sxtl2 v5.2d, v7.4s
	.else
	uxtl2 v5.2d, v7.4s
	.endif
	.endif
	.elseif \opcode == 0x23 || \opcode == 0x33
	.if \opcode == 0x23
	sxtl v4.4s, v1.4h
	.else
	uxtl v4.4s, v1.4h
	.endif
	.if \width == 256
	.if \opcode == 0x23
	sxtl2 v5.4s, v1.8h
	.else
	uxtl2 v5.4s, v1.8h
	.endif
	.endif
	.elseif \opcode == 0x24 || \opcode == 0x34
	.if \opcode == 0x24
	sxtl v6.4s, v1.4h
	sxtl v4.2d, v6.2s
	.else
	uxtl v6.4s, v1.4h
	uxtl v4.2d, v6.2s
	.endif
	.if \width == 256
	.if \opcode == 0x24
	sxtl2 v5.2d, v6.4s
	.else
	uxtl2 v5.2d, v6.4s
	.endif
	.endif
	.else
	.if \opcode == 0x25
	sxtl v4.2d, v1.2s
	.else
	uxtl v4.2d, v1.2s
	.endif
	.if \width == 256
	.if \opcode == 0x25
	sxtl2 v5.2d, v1.4s
	.else
	uxtl2 v5.2d, v1.4s
	.endif
	.endif
	.endif
.endm

.macro vector_map2_packusdw output, left, right
	sqxtun v\output\().4h, v\left\().4s
	sqxtun2 v\output\().8h, v\right\().4s
.endm

.macro vector_map2_minpos output, input
	mov w14, #0xffff
	mov w15, wzr
	.irp lane, 0, 1, 2, 3, 4, 5, 6, 7
	umov w16, v\input\().h[\lane]
	cmp w16, w14
	mov w17, #\lane
	csel w14, w16, w14, lo
	csel w15, w17, w15, lo
	.endr
	movi v\output\().16b, #0
	mov v\output\().h[0], w14
	mov v\output\().h[1], w15
.endm

.macro vector_map2_lane opcode, width, output, left, right
	.if \opcode == 0x00
	.if \width == 64
	movi v6.16b, #0x87
	.else
	movi v6.16b, #0x8f
	.endif
	and v6.16b, v6.16b, v\right\().16b
	tbl v\output\().16b, {v\left\().16b}, v6.16b
	.elseif \opcode == 0x01
	vector_map2_horizontal 16, 0, 0, \width, \output, \left, \right
	.elseif \opcode == 0x02
	vector_map2_horizontal 32, 0, 0, \width, \output, \left, \right
	.elseif \opcode == 0x03
	vector_map2_horizontal 16, 0, 1, \width, \output, \left, \right
	.elseif \opcode == 0x04
	vector_map2_maddubs \output, \left, \right
	.elseif \opcode == 0x05
	vector_map2_horizontal 16, 1, 0, \width, \output, \left, \right
	.elseif \opcode == 0x06
	vector_map2_horizontal 32, 1, 0, \width, \output, \left, \right
	.elseif \opcode == 0x07
	vector_map2_horizontal 16, 1, 1, \width, \output, \left, \right
	.elseif \opcode == 0x08
	vector_map2_sign 8, \output, \left, \right
	.elseif \opcode == 0x09
	vector_map2_sign 16, \output, \left, \right
	.elseif \opcode == 0x0a
	vector_map2_sign 32, \output, \left, \right
	.elseif \opcode == 0x0b
	vector_map2_mulhrs \output, \left, \right
	.elseif \opcode == 0x10 || \opcode == 0x14 || \opcode == 0x15
	ldr q6, [_vcpu, #UML_X86_64_CPU_VECTORS]
	.if \opcode == 0x10
	cmlt v6.16b, v6.16b, #0
	.elseif \opcode == 0x14
	cmlt v6.4s, v6.4s, #0
	.else
	cmlt v6.2d, v6.2d, #0
	.endif
	bsl v6.16b, v\right\().16b, v\left\().16b
	mov v\output\().16b, v6.16b
	.elseif \opcode == 0x1c
	abs v\output\().16b, v\right\().16b
	.elseif \opcode == 0x1d
	abs v\output\().8h, v\right\().8h
	.elseif \opcode == 0x1e
	abs v\output\().4s, v\right\().4s
	.elseif (\opcode >= 0x20 && \opcode <= 0x25) || \
		(\opcode >= 0x30 && \opcode <= 0x35)
	vector_map2_extend \opcode, \width
	.elseif \opcode == 0x28
	uzp1 v6.4s, v\left\().4s, v\left\().4s
	uzp1 v7.4s, v\right\().4s, v\right\().4s
	smull v\output\().2d, v6.2s, v7.2s
	.elseif \opcode == 0x29
	cmeq v\output\().2d, v\left\().2d, v\right\().2d
	.elseif \opcode == 0x2b
	vector_map2_packusdw \output, \left, \right
	.elseif \opcode == 0x37
	cmgt v\output\().2d, v\left\().2d, v\right\().2d
	.elseif \opcode == 0x38
	smin v\output\().16b, v\left\().16b, v\right\().16b
	.elseif \opcode == 0x39
	smin v\output\().4s, v\left\().4s, v\right\().4s
	.elseif \opcode == 0x3a
	umin v\output\().8h, v\left\().8h, v\right\().8h
	.elseif \opcode == 0x3b
	umin v\output\().4s, v\left\().4s, v\right\().4s
	.elseif \opcode == 0x3c
	smax v\output\().16b, v\left\().16b, v\right\().16b
	.elseif \opcode == 0x3d
	smax v\output\().4s, v\left\().4s, v\right\().4s
	.elseif \opcode == 0x3e
	umax v\output\().8h, v\left\().8h, v\right\().8h
	.elseif \opcode == 0x3f
	umax v\output\().4s, v\left\().4s, v\right\().4s
	.elseif \opcode == 0x40
	mul v\output\().4s, v\left\().4s, v\right\().4s
	.else
	vector_map2_minpos \output, \right
	.endif
.endm

.macro vector_map2_input_size opcode, width
	.set Lx86_vector_map2_input_size, (\width / 8)
	.if \opcode == 0x20 || \opcode == 0x30 || \opcode == 0x23 || \opcode == 0x33
	.set Lx86_vector_map2_input_size, (\width / 16)
	.elseif \opcode == 0x21 || \opcode == 0x31 || \opcode == 0x24 || \
		\opcode == 0x34
	.set Lx86_vector_map2_input_size, (\width / 32)
	.elseif \opcode == 0x22 || \opcode == 0x32
	.set Lx86_vector_map2_input_size, (\width / 64)
	.elseif \opcode == 0x25 || \opcode == 0x35
	.set Lx86_vector_map2_input_size, (\width / 16)
	.elseif \opcode == 0x41
	.set Lx86_vector_map2_input_size, 16
	.endif
.endm

.macro vector_map2_register symbol, opcode, width
gadget vector_integer_map2_\symbol\()_\width\()_rr
	ldr x8, [_stream], #8
	mov x23, x8
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	.if \width == 64
	ldr d0, [_vcpu, x10]
	ldr d1, [_vcpu, x11]
	.else
	add x10, _vcpu, x10
	add x11, _vcpu, x11
	ldr q0, [x10]
	vector_map2_input_size \opcode, \width
	.if Lx86_vector_map2_input_size == 1
	ldr b1, [x11]
	.elseif Lx86_vector_map2_input_size == 2
	ldr h1, [x11]
	.elseif Lx86_vector_map2_input_size == 4
	ldr s1, [x11]
	.elseif Lx86_vector_map2_input_size == 8
	ldr d1, [x11]
	.else
	ldr q1, [x11]
	.endif
	.if \width == 256
	ldr q2, [x10, #16]
	.if Lx86_vector_map2_input_size == 32
	ldr q3, [x11, #16]
	.endif
	.endif
	.endif
	vector_map2_lane \opcode, \width, 4, 0, 1
	.if \width == 256 && !((\opcode >= 0x20 && \opcode <= 0x25) || \
		(\opcode >= 0x30 && \opcode <= 0x35))
	vector_map2_lane \opcode, 128, 5, 2, 3
	.endif
	vector_write \width, 23
	retire_and_next
.endm

.macro vector_map2_memory symbol, opcode, width
gadget vector_integer_map2_\symbol\()_\width\()_rm
	effective_address
	ldr x26, [_stream], #8
	mov x25, x7
	vector_map2_input_size \opcode, \width
	vector_memory_source Lx86_vector_map2_input_size, Lx86_vector_map2_cross_fault\@, scalar=1
	and x10, x26, #0xffff
	vector_read \width
	vector_map2_lane \opcode, \width, 4, 0, 1
	.if \width == 256 && !((\opcode >= 0x20 && \opcode <= 0x25) || \
		(\opcode >= 0x30 && \opcode <= 0x35))
	vector_map2_lane \opcode, 128, 5, 2, 3
	.endif
	vector_write \width, 23
	retire_and_next
	vector_memory_source_fault Lx86_vector_map2_cross_fault\@
.endm

.macro vector_map2_family opcode
	vector_map2_register \opcode, 0x\opcode, 64
	vector_map2_memory \opcode, 0x\opcode, 64
	vector_map2_register \opcode, 0x\opcode, 128
	vector_map2_memory \opcode, 0x\opcode, 128
	vector_map2_register \opcode, 0x\opcode, 256
	vector_map2_memory \opcode, 0x\opcode, 256
.endm

vector_map2_family 00
vector_map2_family 01
vector_map2_family 02
vector_map2_family 03
vector_map2_family 04
vector_map2_family 05
vector_map2_family 06
vector_map2_family 07
vector_map2_family 08
vector_map2_family 09
vector_map2_family 0a
vector_map2_family 0b
vector_map2_family 10
vector_map2_family 14
vector_map2_family 15
vector_map2_family 1c
vector_map2_family 1d
vector_map2_family 1e
vector_map2_family 20
vector_map2_family 21
vector_map2_family 22
vector_map2_family 23
vector_map2_family 24
vector_map2_family 25
vector_map2_family 28
vector_map2_family 29
vector_map2_family 2b
vector_map2_family 30
vector_map2_family 31
vector_map2_family 32
vector_map2_family 33
vector_map2_family 34
vector_map2_family 35
vector_map2_family 37
vector_map2_family 38
vector_map2_family 39
vector_map2_family 3a
vector_map2_family 3b
vector_map2_family 3c
vector_map2_family 3d
vector_map2_family 3e
vector_map2_family 3f
vector_map2_family 40
vector_map2_family 41

.purgem vector_map2_family
.purgem vector_map2_memory
.purgem vector_map2_register
.purgem vector_map2_input_size
.purgem vector_map2_lane
.purgem vector_map2_minpos
.purgem vector_map2_packusdw
.purgem vector_map2_extend
.purgem vector_map2_mulhrs
.purgem vector_map2_sign
.purgem vector_map2_maddubs
.purgem vector_map2_horizontal
