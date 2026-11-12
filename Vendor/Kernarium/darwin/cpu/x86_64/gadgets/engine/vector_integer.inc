/* SPDX-License-Identifier: GPL-2.0-or-later */
/* MMX/SSE/AVX packed-integer operations from the primary opcode map. */

.macro vector_integer_pack opcode, width, output, left, right
	.if \opcode == 0x63 || \opcode == 0x67
	.if \width == 64
	.if \opcode == 0x63
	sqxtn v6.8b, v\left\().8h
	sqxtn v7.8b, v\right\().8h
	.else
	sqxtun v6.8b, v\left\().8h
	sqxtun v7.8b, v\right\().8h
	.endif
	mov v6.s[1], v7.s[0]
	mov v\output\().8b, v6.8b
	.else
	.if \opcode == 0x63
	sqxtn v\output\().8b, v\left\().8h
	sqxtn2 v\output\().16b, v\right\().8h
	.else
	sqxtun v\output\().8b, v\left\().8h
	sqxtun2 v\output\().16b, v\right\().8h
	.endif
	.endif
	.else
	.if \width == 64
	sqxtn v6.4h, v\left\().4s
	sqxtn v7.4h, v\right\().4s
	mov v6.s[1], v7.s[0]
	mov v\output\().8b, v6.8b
	.else
	sqxtn v\output\().4h, v\left\().4s
	sqxtn2 v\output\().8h, v\right\().4s
	.endif
	.endif
.endm

.macro vector_integer_mul_high signed, output, left, right
	.if \signed
	smull v6.4s, v\left\().4h, v\right\().4h
	smull2 v7.4s, v\left\().8h, v\right\().8h
	.else
	umull v6.4s, v\left\().4h, v\right\().4h
	umull2 v7.4s, v\left\().8h, v\right\().8h
	.endif
	uzp2 v\output\().8h, v6.8h, v7.8h
.endm

.macro vector_integer_mul_even output, left, right
	uzp1 v6.4s, v\left\().4s, v\left\().4s
	uzp1 v7.4s, v\right\().4s, v\right\().4s
	umull v\output\().2d, v6.2s, v7.2s
.endm

.macro vector_integer_madd_words output, left, right
	smull v6.4s, v\left\().4h, v\right\().4h
	smull2 v7.4s, v\left\().8h, v\right\().8h
	addp v\output\().4s, v6.4s, v7.4s
.endm

.macro vector_integer_sad output, left, right
	movi v\output\().16b, #0
	uabdl v6.8h, v\left\().8b, v\right\().8b
	uaddlv s7, v6.8h
	mov v\output\().h[0], v7.h[0]
	uabdl2 v6.8h, v\left\().16b, v\right\().16b
	uaddlv s7, v6.8h
	mov v\output\().h[4], v7.h[0]
.endm

.macro vector_integer_lane opcode, width, output, left, right
	.if \opcode == 0x60
	.if \width == 64
	zip1 v\output\().8b, v\left\().8b, v\right\().8b
	.else
	zip1 v\output\().16b, v\left\().16b, v\right\().16b
	.endif
	.elseif \opcode == 0x61
	.if \width == 64
	zip1 v\output\().4h, v\left\().4h, v\right\().4h
	.else
	zip1 v\output\().8h, v\left\().8h, v\right\().8h
	.endif
	.elseif \opcode == 0x62
	.if \width == 64
	zip1 v\output\().2s, v\left\().2s, v\right\().2s
	.else
	zip1 v\output\().4s, v\left\().4s, v\right\().4s
	.endif
	.elseif \opcode == 0x63 || \opcode == 0x67 || \opcode == 0x6b
	vector_integer_pack \opcode, \width, \output, \left, \right
	.elseif \opcode == 0x64
	cmgt v\output\().16b, v\left\().16b, v\right\().16b
	.elseif \opcode == 0x65
	cmgt v\output\().8h, v\left\().8h, v\right\().8h
	.elseif \opcode == 0x66
	cmgt v\output\().4s, v\left\().4s, v\right\().4s
	.elseif \opcode == 0x68
	.if \width == 64
	zip2 v\output\().8b, v\left\().8b, v\right\().8b
	.else
	zip2 v\output\().16b, v\left\().16b, v\right\().16b
	.endif
	.elseif \opcode == 0x69
	.if \width == 64
	zip2 v\output\().4h, v\left\().4h, v\right\().4h
	.else
	zip2 v\output\().8h, v\left\().8h, v\right\().8h
	.endif
	.elseif \opcode == 0x6a
	.if \width == 64
	zip2 v\output\().2s, v\left\().2s, v\right\().2s
	.else
	zip2 v\output\().4s, v\left\().4s, v\right\().4s
	.endif
	.elseif \opcode == 0x6c
	zip1 v\output\().2d, v\left\().2d, v\right\().2d
	.elseif \opcode == 0x6d
	zip2 v\output\().2d, v\left\().2d, v\right\().2d
	.elseif \opcode == 0x74
	cmeq v\output\().16b, v\left\().16b, v\right\().16b
	.elseif \opcode == 0x75
	cmeq v\output\().8h, v\left\().8h, v\right\().8h
	.elseif \opcode == 0x76
	cmeq v\output\().4s, v\left\().4s, v\right\().4s
	.elseif \opcode == 0xd1 || \opcode == 0xd2 || \opcode == 0xd3 || \
		\opcode == 0xe1 || \opcode == 0xe2 || \
		\opcode == 0xf1 || \opcode == 0xf2 || \opcode == 0xf3
	mov x14, v\right\().d[0]
	.if \opcode == 0xd1 || \opcode == 0xe1 || \opcode == 0xf1
	mov x15, #16
	.elseif \opcode == 0xd2 || \opcode == 0xe2 || \opcode == 0xf2
	mov x15, #32
	.else
	mov x15, #64
	.endif
	cmp x14, x15
	csel x14, x14, x15, lo
	.if \opcode < 0xf0
	neg x14, x14
	.endif
	.if \opcode == 0xd1 || \opcode == 0xe1 || \opcode == 0xf1
	dup v6.8h, w14
	.if \opcode == 0xe1
	sshl v\output\().8h, v\left\().8h, v6.8h
	.else
	ushl v\output\().8h, v\left\().8h, v6.8h
	.endif
	.elseif \opcode == 0xd2 || \opcode == 0xe2 || \opcode == 0xf2
	dup v6.4s, w14
	.if \opcode == 0xe2
	sshl v\output\().4s, v\left\().4s, v6.4s
	.else
	ushl v\output\().4s, v\left\().4s, v6.4s
	.endif
	.else
	dup v6.2d, x14
	ushl v\output\().2d, v\left\().2d, v6.2d
	.endif
	.elseif \opcode == 0xd4
	add v\output\().2d, v\left\().2d, v\right\().2d
	.elseif \opcode == 0xd5
	mul v\output\().8h, v\left\().8h, v\right\().8h
	.elseif \opcode == 0xd8
	uqsub v\output\().16b, v\left\().16b, v\right\().16b
	.elseif \opcode == 0xd9
	uqsub v\output\().8h, v\left\().8h, v\right\().8h
	.elseif \opcode == 0xda
	umin v\output\().16b, v\left\().16b, v\right\().16b
	.elseif \opcode == 0xdb
	and v\output\().16b, v\left\().16b, v\right\().16b
	.elseif \opcode == 0xdc
	uqadd v\output\().16b, v\left\().16b, v\right\().16b
	.elseif \opcode == 0xdd
	uqadd v\output\().8h, v\left\().8h, v\right\().8h
	.elseif \opcode == 0xde
	umax v\output\().16b, v\left\().16b, v\right\().16b
	.elseif \opcode == 0xdf
	bic v\output\().16b, v\right\().16b, v\left\().16b
	.elseif \opcode == 0xe0
	urhadd v\output\().16b, v\left\().16b, v\right\().16b
	.elseif \opcode == 0xe3
	urhadd v\output\().8h, v\left\().8h, v\right\().8h
	.elseif \opcode == 0xe4
	vector_integer_mul_high 0, \output, \left, \right
	.elseif \opcode == 0xe5
	vector_integer_mul_high 1, \output, \left, \right
	.elseif \opcode == 0xe8
	sqsub v\output\().16b, v\left\().16b, v\right\().16b
	.elseif \opcode == 0xe9
	sqsub v\output\().8h, v\left\().8h, v\right\().8h
	.elseif \opcode == 0xea
	smin v\output\().8h, v\left\().8h, v\right\().8h
	.elseif \opcode == 0xeb
	orr v\output\().16b, v\left\().16b, v\right\().16b
	.elseif \opcode == 0xec
	sqadd v\output\().16b, v\left\().16b, v\right\().16b
	.elseif \opcode == 0xed
	sqadd v\output\().8h, v\left\().8h, v\right\().8h
	.elseif \opcode == 0xee
	smax v\output\().8h, v\left\().8h, v\right\().8h
	.elseif \opcode == 0xef
	eor v\output\().16b, v\left\().16b, v\right\().16b
	.elseif \opcode == 0xf4
	vector_integer_mul_even \output, \left, \right
	.elseif \opcode == 0xf5
	vector_integer_madd_words \output, \left, \right
	.elseif \opcode == 0xf6
	vector_integer_sad \output, \left, \right
	.elseif \opcode == 0xf8
	sub v\output\().16b, v\left\().16b, v\right\().16b
	.elseif \opcode == 0xf9
	sub v\output\().8h, v\left\().8h, v\right\().8h
	.elseif \opcode == 0xfa
	sub v\output\().4s, v\left\().4s, v\right\().4s
	.elseif \opcode == 0xfb
	sub v\output\().2d, v\left\().2d, v\right\().2d
	.elseif \opcode == 0xfc
	add v\output\().16b, v\left\().16b, v\right\().16b
	.elseif \opcode == 0xfd
	add v\output\().8h, v\left\().8h, v\right\().8h
	.else
	add v\output\().4s, v\left\().4s, v\right\().4s
	.endif
.endm

.macro vector_integer_index_sequence
	mov w16, #0x0100
	movk w16, #0x0302, lsl #16
	mov v6.s[0], w16
	mov w16, #0x0504
	movk w16, #0x0706, lsl #16
	mov v6.s[1], w16
	mov w16, #0x0908
	movk w16, #0x0b0a, lsl #16
	mov v6.s[2], w16
	mov w16, #0x0d0c
	movk w16, #0x0f0e, lsl #16
	mov v6.s[3], w16
.endm

.macro vector_integer_shuffle_indices_word base, first
	vector_integer_index_sequence
	.irp lane, 0, 1, 2, 3
	lsr w14, w13, #(\lane * 2)
	and w14, w14, #3
	add w14, w14, #\base
	lsl w14, w14, #1
	add w15, w14, #1
	orr w14, w14, w15, lsl #8
	mov v6.h[(\first + \lane)], w14
	.endr
.endm

.macro vector_integer_shuffle prefix, width, output, input
	.if \prefix == 1
	movi v6.16b, #0
	vector_shuffle_dword_indices
	.elseif \prefix == 2
	vector_integer_shuffle_indices_word 4, 4
	.else
	vector_integer_shuffle_indices_word 0, 0
	.endif
	tbl v\output\().16b, {v\input\().16b}, v6.16b
.endm

.macro vector_integer_body opcode, width
	.if \opcode == 0x70
	.if \width == 64
	vector_integer_shuffle 0, 64, 4, 1
	.else
	cmp w14, #1
	b.eq Lx86_vector_integer_shuffle_dword\@
	cmp w14, #2
	b.eq Lx86_vector_integer_shuffle_high\@
	vector_integer_shuffle 3, \width, 4, 1
	.if \width == 256
	vector_integer_shuffle 3, \width, 5, 3
	.endif
	b Lx86_vector_integer_shuffle_done\@
Lx86_vector_integer_shuffle_dword\@:
	vector_integer_shuffle 1, \width, 4, 1
	.if \width == 256
	vector_integer_shuffle 1, \width, 5, 3
	.endif
	b Lx86_vector_integer_shuffle_done\@
Lx86_vector_integer_shuffle_high\@:
	vector_integer_shuffle 2, \width, 4, 1
	.if \width == 256
	vector_integer_shuffle 2, \width, 5, 3
	.endif
Lx86_vector_integer_shuffle_done\@:
	.endif
	.else
	vector_integer_lane \opcode, \width, 4, 0, 1
	.if \width == 256
	.if \opcode == 0xd1 || \opcode == 0xd2 || \opcode == 0xd3 || \
		\opcode == 0xe1 || \opcode == 0xe2 || \
		\opcode == 0xf1 || \opcode == 0xf2 || \opcode == 0xf3
	vector_integer_lane \opcode, 128, 5, 2, 1
	.else
	vector_integer_lane \opcode, 128, 5, 2, 3
	.endif
	.endif
	.endif
.endm

.macro vector_integer_register opcode, width
gadget vector_integer_\opcode\()_\width\()_rr
	ldr x8, [_stream], #8
	mov x23, x8
	.if 0x\opcode == 0x70
	ldr x13, [_stream], #8
	ubfx w14, w13, #8, #8
	and w13, w13, #0xff
	.endif
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	.if \width == 64
	ldr d0, [_vcpu, x10]
	ldr d1, [_vcpu, x11]
	.else
	add x10, _vcpu, x10
	add x11, _vcpu, x11
	/* Bit 62 says the gadget before this one left the left operand in v4,
	 * and v5 with it at 256 bits: take it from there rather than reading
	 * back the state slot it has just written, whose store the load has to
	 * wait for. The builder sets the bit only when that gadget is the one
	 * immediately before in the stream, wrote exactly this register at
	 * exactly this width, and cannot have called out in between. */
	tbz x8, #62, Lx86_vector_integer_load_left\@
	mov v0.16b, v4.16b
	.if \width == 256
	mov v2.16b, v5.16b
	.endif
	b Lx86_vector_integer_left_ready\@
Lx86_vector_integer_load_left\@:
	ldr q0, [x10]
	.if \width == 256
	ldr q2, [x10, #16]
	.endif
Lx86_vector_integer_left_ready\@:
	ldr q1, [x11]
	.if \width == 256
	ldr q3, [x11, #16]
	.endif
	.endif
	vector_integer_body 0x\opcode, \width
	vector_write \width, 23
	retire_and_next
.endm

.macro vector_integer_memory opcode, width
gadget vector_integer_\opcode\()_\width\()_rm
	effective_address
	ldr x26, [_stream], #8
	.if 0x\opcode == 0x70
	ldr x27, [_stream], #8
	.endif
	mov x25, x7
	vector_memory_source (\width / 8), Lx86_vector_integer_cross_fault\@
	.if 0x\opcode == 0x70
	ubfx w14, w27, #8, #8
	and w13, w27, #0xff
	.endif
	and x10, x26, #0xffff
	vector_read \width
	vector_integer_body 0x\opcode, \width
	vector_write \width, 23
	retire_and_next
	vector_memory_source_fault Lx86_vector_integer_cross_fault\@
.endm

.macro vector_integer_family opcode
	vector_integer_register \opcode, 64
	vector_integer_memory \opcode, 64
	vector_integer_register \opcode, 128
	vector_integer_memory \opcode, 128
	vector_integer_register \opcode, 256
	vector_integer_memory \opcode, 256
.endm

vector_integer_family 60
vector_integer_family 61
vector_integer_family 62
vector_integer_family 63
vector_integer_family 64
vector_integer_family 65
vector_integer_family 66
vector_integer_family 67
vector_integer_family 68
vector_integer_family 69
vector_integer_family 6a
vector_integer_family 6b
vector_integer_family 6c
vector_integer_family 6d
vector_integer_family 70
vector_integer_family 74
vector_integer_family 75
vector_integer_family 76
vector_integer_family d1
vector_integer_family d2
vector_integer_family d3
vector_integer_family d4
vector_integer_family d5
vector_integer_family d8
vector_integer_family d9
vector_integer_family da
vector_integer_family db
vector_integer_family dc
vector_integer_family dd
vector_integer_family de
vector_integer_family df
vector_integer_family e0
vector_integer_family e1
vector_integer_family e2
vector_integer_family e3
vector_integer_family e4
vector_integer_family e5
vector_integer_family e8
vector_integer_family e9
vector_integer_family ea
vector_integer_family eb
vector_integer_family ec
vector_integer_family ed
vector_integer_family ee
vector_integer_family ef
vector_integer_family f1
vector_integer_family f2
vector_integer_family f3
vector_integer_family f4
vector_integer_family f5
vector_integer_family f6
vector_integer_family f8
vector_integer_family f9
vector_integer_family fa
vector_integer_family fb
vector_integer_family fc
vector_integer_family fd
vector_integer_family fe

/* Immediate shifts have no memory form. Translation selects the group once. */
.macro vector_integer_immediate_lane opcode, group, output, input
	.if \opcode == 0x73 && (\group == 3 || \group == 7)
	/* PSRLDQ and PSLLDQ index the lane through TBL, which answers an index
	 * past the sixteen-byte table with zero, and that is also what the
	 * architecture produces for a byte count of 16 or more. The index
	 * arithmetic is byte-wise, so a count of 241 or more would wrap back
	 * into the table and resurrect a byte the shift had discarded; clamp
	 * the count, which leaves every smaller count untouched. */
	vector_integer_index_sequence
	mov w14, #16
	cmp w13, w14
	csel w14, w13, w14, lo
	dup v7.16b, w14
	.if \group == 3
	add v6.16b, v6.16b, v7.16b
	.else
	sub v6.16b, v6.16b, v7.16b
	.endif
	tbl v\output\().16b, {v\input\().16b}, v6.16b
	.else
	.if \opcode == 0x71
	mov w14, #16
	.elseif \opcode == 0x72
	mov w14, #32
	.else
	mov w14, #64
	.endif
	cmp w13, w14
	csel w13, w13, w14, lo
	.if \group != 6
	neg w13, w13
	.endif
	.if \opcode == 0x71
	dup v6.8h, w13
	.if \group == 4
	sshl v\output\().8h, v\input\().8h, v6.8h
	.else
	ushl v\output\().8h, v\input\().8h, v6.8h
	.endif
	.elseif \opcode == 0x72
	dup v6.4s, w13
	.if \group == 4
	sshl v\output\().4s, v\input\().4s, v6.4s
	.else
	ushl v\output\().4s, v\input\().4s, v6.4s
	.endif
	.else
	sxtw x13, w13
	dup v6.2d, x13
	ushl v\output\().2d, v\input\().2d, v6.2d
	.endif
	.endif
.endm

.macro vector_integer_immediate opcode, group, width
gadget vector_integer_\opcode\()_g\group\()_\width\()_ri
	ldp x8, x13, [_stream], #16
	mov x23, x8
	mov w12, w13
	ubfx x10, x8, #16, #16
	vector_read \width
	vector_integer_immediate_lane 0x\opcode, \group, 4, 0
	.if \width == 256
	mov w13, w12
	vector_integer_immediate_lane 0x\opcode, \group, 5, 2
	.endif
	vector_write \width, 23
	retire_and_next
.endm

.macro vector_integer_immediate_widths opcode, group
	vector_integer_immediate \opcode, \group, 64
	vector_integer_immediate \opcode, \group, 128
	vector_integer_immediate \opcode, \group, 256
.endm

vector_integer_immediate_widths 71, 2
vector_integer_immediate_widths 71, 4
vector_integer_immediate_widths 71, 6
vector_integer_immediate_widths 72, 2
vector_integer_immediate_widths 72, 4
vector_integer_immediate_widths 72, 6
vector_integer_immediate_widths 73, 2
vector_integer_immediate_widths 73, 6
vector_integer_immediate 73, 3, 128
vector_integer_immediate 73, 3, 256
vector_integer_immediate 73, 7, 128
vector_integer_immediate 73, 7, 256

.purgem vector_integer_immediate_widths
.purgem vector_integer_immediate
.purgem vector_integer_immediate_lane
.purgem vector_integer_family
.purgem vector_integer_memory
.purgem vector_integer_register
.purgem vector_integer_body
.purgem vector_integer_shuffle
.purgem vector_integer_shuffle_indices_word
.purgem vector_integer_index_sequence
.purgem vector_integer_lane
.purgem vector_integer_sad
.purgem vector_integer_madd_words
.purgem vector_integer_mul_even
.purgem vector_integer_mul_high
.purgem vector_integer_pack
