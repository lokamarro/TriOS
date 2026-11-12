/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SSE4.2 packed-string comparison state machine. */

/* Inputs: w11 = right index, w12 = left index, w4/w5 = lengths,
 * w6 = element bytes, w7 = signed elements, w8 = aggregation,
 * x9/x10 = left/right byte arrays. Output: w14 = boolean result.
 */
.macro vector_map3_string_compare_pair
	cmp w12, w4
	cset w15, lo
	cmp w11, w5
	cset w16, lo
	cmp w8, #1
	b.hi Lx86_vector_map3_string_compare_special\@
	and w17, w15, w16
	cbz w17, Lx86_vector_map3_string_compare_false\@
	b Lx86_vector_map3_string_compare_load\@
Lx86_vector_map3_string_compare_special\@:
	cmp w8, #2
	b.ne Lx86_vector_map3_string_compare_substring\@
	eor w17, w15, w16
	cbnz w17, Lx86_vector_map3_string_compare_false\@
	cbz w15, Lx86_vector_map3_string_compare_true\@
	b Lx86_vector_map3_string_compare_load\@
Lx86_vector_map3_string_compare_substring\@:
	cbz w15, Lx86_vector_map3_string_compare_true\@
	cbz w16, Lx86_vector_map3_string_compare_false\@
Lx86_vector_map3_string_compare_load\@:
	cmp w6, #1
	b.ne Lx86_vector_map3_string_compare_words\@
	ldrb w15, [x9, w12, uxtw]
	ldrb w16, [x10, w11, uxtw]
	cbz w7, Lx86_vector_map3_string_compare_values_ready\@
	sxtb w15, w15
	sxtb w16, w16
	b Lx86_vector_map3_string_compare_values_ready\@
Lx86_vector_map3_string_compare_words\@:
	ldrh w15, [x9, w12, uxtw #1]
	ldrh w16, [x10, w11, uxtw #1]
	cbz w7, Lx86_vector_map3_string_compare_values_ready\@
	sxth w15, w15
	sxth w16, w16
Lx86_vector_map3_string_compare_values_ready\@:
	cmp w8, #1
	b.ne Lx86_vector_map3_string_compare_equal\@
	tbz w12, #0, Lx86_vector_map3_string_compare_lower\@
	cmp w16, w15
	cset w14, le
	cbnz w7, Lx86_vector_map3_string_compare_done\@
	cset w14, ls
	b Lx86_vector_map3_string_compare_done\@
Lx86_vector_map3_string_compare_lower\@:
	cmp w16, w15
	cset w14, ge
	cbnz w7, Lx86_vector_map3_string_compare_done\@
	cset w14, hs
	b Lx86_vector_map3_string_compare_done\@
Lx86_vector_map3_string_compare_equal\@:
	cmp w15, w16
	cset w14, eq
	b Lx86_vector_map3_string_compare_done\@
Lx86_vector_map3_string_compare_true\@:
	mov w14, #1
	b Lx86_vector_map3_string_compare_done\@
Lx86_vector_map3_string_compare_false\@:
	mov w14, #0
Lx86_vector_map3_string_compare_done\@:
.endm

/* q0/q1 are the left/right XMM values. w13 is the immediate, w12 selects
 * explicit lengths, and w11 selects 64-bit explicit lengths. Returns the
 * polarity-adjusted bit result in w0, lengths in w1/w2, and count in w3.
 */
.p2align 4
Lx86_vector_map3_string_compute:
	sub sp, sp, #32
	str q0, [sp]
	str q1, [sp, #16]
	mov x9, sp
	add x10, sp, #16
	and w6, w13, #1
	mov w3, #16
	lsrv w3, w3, w6
	add w6, w6, #1
	cbz w12, Lx86_vector_map3_string_implicit_lengths
	cbz w11, Lx86_vector_map3_string_explicit_32
	ldr x14, [_vcpu, #0]
	cmp x14, #0
	cneg x14, x14, mi
	cmp x14, x3
	csel x4, x14, x3, lo
	ldr x14, [_vcpu, #16]
	cmp x14, #0
	cneg x14, x14, mi
	cmp x14, x3
	csel x5, x14, x3, lo
	b Lx86_vector_map3_string_lengths_ready
Lx86_vector_map3_string_explicit_32:
	ldr w14, [_vcpu, #0]
	sxtw x14, w14
	cmp x14, #0
	cneg x14, x14, mi
	cmp x14, x3
	csel x4, x14, x3, lo
	ldr w14, [_vcpu, #16]
	sxtw x14, w14
	cmp x14, #0
	cneg x14, x14, mi
	cmp x14, x3
	csel x5, x14, x3, lo
	b Lx86_vector_map3_string_lengths_ready
Lx86_vector_map3_string_implicit_lengths:
	mov w4, w3
	mov w5, w3
	mov w14, #0
Lx86_vector_map3_string_length_loop:
	cmp w14, w3
	b.hs Lx86_vector_map3_string_lengths_ready
	cmp w6, #1
	b.ne Lx86_vector_map3_string_length_words
	ldrb w15, [x9, w14, uxtw]
	ldrb w16, [x10, w14, uxtw]
	b Lx86_vector_map3_string_length_values
Lx86_vector_map3_string_length_words:
	ldrh w15, [x9, w14, uxtw #1]
	ldrh w16, [x10, w14, uxtw #1]
Lx86_vector_map3_string_length_values:
	cmp w4, w3
	b.ne Lx86_vector_map3_string_right_length
	cbnz w15, Lx86_vector_map3_string_right_length
	mov w4, w14
Lx86_vector_map3_string_right_length:
	cmp w5, w3
	b.ne Lx86_vector_map3_string_length_next
	cbnz w16, Lx86_vector_map3_string_length_next
	mov w5, w14
Lx86_vector_map3_string_length_next:
	add w14, w14, #1
	b Lx86_vector_map3_string_length_loop
Lx86_vector_map3_string_lengths_ready:
	ubfx w8, w13, #2, #2
	ubfx w7, w13, #1, #1
	mov w0, #0
	cmp w8, #0
	b.eq Lx86_vector_map3_string_equal_any
	cmp w8, #1
	b.eq Lx86_vector_map3_string_ranges
	cmp w8, #2
	b.eq Lx86_vector_map3_string_equal_each
	b Lx86_vector_map3_string_equal_ordered

Lx86_vector_map3_string_equal_any:
	mov w11, #0
Lx86_vector_map3_string_equal_any_right:
	cmp w11, w3
	b.hs Lx86_vector_map3_string_aggregation_done
	mov w12, #0
Lx86_vector_map3_string_equal_any_left:
	cmp w12, w3
	b.hs Lx86_vector_map3_string_equal_any_next
	vector_map3_string_compare_pair
	cbnz w14, Lx86_vector_map3_string_set_right
	add w12, w12, #1
	b Lx86_vector_map3_string_equal_any_left
Lx86_vector_map3_string_equal_any_next:
	add w11, w11, #1
	b Lx86_vector_map3_string_equal_any_right

Lx86_vector_map3_string_ranges:
	mov w11, #0
Lx86_vector_map3_string_ranges_right:
	cmp w11, w3
	b.hs Lx86_vector_map3_string_aggregation_done
	mov w12, #0
Lx86_vector_map3_string_ranges_left:
	cmp w12, w3
	b.hs Lx86_vector_map3_string_ranges_next
	vector_map3_string_compare_pair
	cbz w14, Lx86_vector_map3_string_ranges_pair_next
	add w12, w12, #1
	vector_map3_string_compare_pair
	cbnz w14, Lx86_vector_map3_string_set_right
	add w12, w12, #1
	b Lx86_vector_map3_string_ranges_left
Lx86_vector_map3_string_ranges_pair_next:
	add w12, w12, #2
	b Lx86_vector_map3_string_ranges_left
Lx86_vector_map3_string_ranges_next:
	add w11, w11, #1
	b Lx86_vector_map3_string_ranges_right

Lx86_vector_map3_string_equal_each:
	mov w11, #0
Lx86_vector_map3_string_equal_each_loop:
	cmp w11, w3
	b.hs Lx86_vector_map3_string_aggregation_done
	mov w12, w11
	vector_map3_string_compare_pair
	cbz w14, Lx86_vector_map3_string_equal_each_next
	mov w15, #1
	lsl w15, w15, w11
	orr w0, w0, w15
Lx86_vector_map3_string_equal_each_next:
	add w11, w11, #1
	b Lx86_vector_map3_string_equal_each_loop

Lx86_vector_map3_string_equal_ordered:
	mov w2, #0
Lx86_vector_map3_string_ordered_start:
	cmp w2, w3
	b.hs Lx86_vector_map3_string_aggregation_done
	mov w12, #0
Lx86_vector_map3_string_ordered_left:
	add w11, w2, w12
	cmp w11, w3
	b.hs Lx86_vector_map3_string_ordered_set
	vector_map3_string_compare_pair
	cbz w14, Lx86_vector_map3_string_ordered_next
	add w12, w12, #1
	b Lx86_vector_map3_string_ordered_left
Lx86_vector_map3_string_ordered_set:
	mov w15, #1
	lsl w15, w15, w2
	orr w0, w0, w15
Lx86_vector_map3_string_ordered_next:
	add w2, w2, #1
	b Lx86_vector_map3_string_ordered_start

Lx86_vector_map3_string_set_right:
	mov w15, #1
	lsl w15, w15, w11
	orr w0, w0, w15
	cmp w8, #2
	b.eq Lx86_vector_map3_string_equal_each_next
	cmp w8, #1
	b.eq Lx86_vector_map3_string_ranges_next
	b Lx86_vector_map3_string_equal_any_next

Lx86_vector_map3_string_aggregation_done:
	mov w14, #1
	lsl w14, w14, w3
	sub w14, w14, #1
	and w0, w0, w14
	tbz w13, #4, Lx86_vector_map3_string_polarity_done
	tbnz w13, #5, Lx86_vector_map3_string_masked_negative
	mvn w0, w0
	and w0, w0, w14
	b Lx86_vector_map3_string_polarity_done
Lx86_vector_map3_string_masked_negative:
	cmp w5, w3
	b.eq Lx86_vector_map3_string_valid_mask_ready
	mov w15, #1
	lsl w15, w15, w5
	sub w15, w15, #1
	b Lx86_vector_map3_string_apply_valid_mask
Lx86_vector_map3_string_valid_mask_ready:
	mov w15, w14
Lx86_vector_map3_string_apply_valid_mask:
	eor w0, w0, w15
Lx86_vector_map3_string_polarity_done:
	mov w1, w4
	mov w2, w5
	add sp, sp, #32
	ret

.purgem vector_map3_string_compare_pair

.macro vector_map3_string_finish opcode
	mov w24, w0
	mov w25, w1
	mov w26, w2
	mov w16, w3
	ldr x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	mov x10, #0x8d5
	bic x9, x9, x10
	cmp w24, #0
	cset w11, ne
	orr x9, x9, x11
	cmp w26, w16
	cset w11, lo
	orr x9, x9, x11, lsl #6
	cmp w25, w16
	cset w11, lo
	orr x9, x9, x11, lsl #7
	and w11, w24, #1
	orr x9, x9, x11, lsl #11
	orr x9, x9, #2
	str x9, [_vcpu, #UML_X86_64_CPU_RFLAGS]
	str xzr, [_vcpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
	.if (\opcode & 1) == 0
	add x9, _vcpu, #UML_X86_64_CPU_VECTORS
	movi v4.16b, #0
	tbnz w27, #6, Lx86_vector_map3_string_expanded_mask\@
	mov v4.h[0], w24
	str q4, [x9]
	b Lx86_vector_map3_string_mask_written\@
Lx86_vector_map3_string_expanded_mask\@:
	str q4, [x9]
	mov w11, #0
	mov w14, #-1
Lx86_vector_map3_string_expand_loop\@:
	cmp w11, w16
	b.hs Lx86_vector_map3_string_mask_written\@
	lsrv w12, w24, w11
	tbz w12, #0, Lx86_vector_map3_string_expand_next\@
	tbnz w27, #0, Lx86_vector_map3_string_expand_word\@
	strb w14, [x9, w11, uxtw]
	b Lx86_vector_map3_string_expand_next\@
Lx86_vector_map3_string_expand_word\@:
	strh w14, [x9, w11, uxtw #1]
Lx86_vector_map3_string_expand_next\@:
	add w11, w11, #1
	b Lx86_vector_map3_string_expand_loop\@
Lx86_vector_map3_string_mask_written\@:
	tbz x19, #63, Lx86_vector_map3_string_result_done\@
	movi v4.16b, #0
	str q4, [x9, #16]
	b Lx86_vector_map3_string_result_done\@
	.else
	cbz w24, Lx86_vector_map3_string_index_default\@
	tbnz w27, #6, Lx86_vector_map3_string_index_high\@
	rbit w11, w24
	clz w11, w11
	b Lx86_vector_map3_string_index_ready\@
Lx86_vector_map3_string_index_high\@:
	clz w11, w24
	mov w12, #31
	sub w11, w12, w11
	b Lx86_vector_map3_string_index_ready\@
Lx86_vector_map3_string_index_default\@:
	mov w11, w16
Lx86_vector_map3_string_index_ready\@:
	str x11, [_vcpu, #8]
	.endif
Lx86_vector_map3_string_result_done\@:
	retire_and_next
.endm

.macro vector_map3_string_execute opcode, descriptor
	mov x19, x\descriptor
	/* Every arithmetic flag is defined below, and the only overrides a gadget
	 * records lie inside that same set, so a pending record has nothing left
	 * to contribute: discard it instead of materializing a value this
	 * gadget immediately overwrites. */
	str xzr, [_vcpu, #UML_X86_64_CPU_DEFERRED_OPERATION]
	and w13, w27, #0xff
	mov w12, #(\opcode <= 0x61)
	ubfx w11, w27, #8, #1
	bl Lx86_vector_map3_string_compute
	vector_map3_string_finish \opcode
.endm

.macro vector_map3_string_register symbol, opcode
gadget vector_map3_string_\symbol\()_rr
	ldp x8, x27, [_stream], #16
	mov x19, x8
	and x10, x8, #0xffff
	ubfx x11, x8, #16, #16
	ldr q0, [_vcpu, x10]
	ldr q1, [_vcpu, x11]
	vector_map3_string_execute \opcode, 19
.endm

.macro vector_map3_string_memory symbol, opcode
gadget vector_map3_string_\symbol\()_rm
	effective_address
	ldr x27, [_stream], #8
	mov x25, x7
	vector_memory_source 16, Lx86_vector_map3_string_cross_fault\@, frame=16
	and x10, x23, #0xffff
	ldr q0, [_vcpu, x10]
	vector_map3_string_execute \opcode, 23
	vector_memory_source_fault Lx86_vector_map3_string_cross_fault\@, 16
.endm

.macro vector_map3_string_family symbol, opcode
	vector_map3_string_register \symbol, \opcode
	vector_map3_string_memory \symbol, \opcode
.endm

vector_map3_string_family 60, 0x60
vector_map3_string_family 61, 0x61
vector_map3_string_family 62, 0x62
vector_map3_string_family 63, 0x63

.purgem vector_map3_string_family
.purgem vector_map3_string_memory
.purgem vector_map3_string_register
.purgem vector_map3_string_execute
.purgem vector_map3_string_finish
