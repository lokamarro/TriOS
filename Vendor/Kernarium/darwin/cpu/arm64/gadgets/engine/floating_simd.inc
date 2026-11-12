/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Direct CpuState vector offsets fit in 16 bits. Builders use these layouts
 * whenever one instruction has multiple vector operands; gadget/link/helper
 * pointers and full-width data remain separate stream cells. */
.macro unpack_u16_2 packed, first, second
	and \first, \packed, #0xffff
	lsr \second, \packed, #16
.endm

.macro unpack_u16_3 packed, first, second, third
	and \first, \packed, #0xffff
	ubfx \second, \packed, #16, #16
	lsr \third, \packed, #32
.endm

.macro unpack_u16_4 packed, first, second, third, fourth
	and \first, \packed, #0xffff
	ubfx \second, \packed, #16, #16
	ubfx \third, \packed, #32, #16
	lsr \fourth, \packed, #48
.endm

/* The operands of an FP/SIMD gadget with two vector registers: activates the
 * guest FP environment, then reads the stream word, destination vector
 * offset in bits 4:0 and source in bits 12:8, leaving the source at x12 and
 * the destination at x13. */
.macro fp_two_vector_operands
	ensure_fp_environment
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	add x11, _vcpu, #UML_GADGET_CPU_V0
	add x12, x11, x10, lsl #4
	add x13, x11, x9, lsl #4
.endm

/* The destination the result goes into as v2: the old Vd for the FEAT_AFP
 * twin, which merges the result into it, and zero for the base gadget. */
.macro afp_merge_source afp
	.if \afp
	ldr q2, [x13]
	.else
	movi v2.16b, #0
	.endif
.endm

/* FMOV (immediate), decoded once by the builder into IEEE bits. */
gadget floating_move_immediate
	ldr x8, [_stream], #8
	ldr x9, [_stream], #8
	add x10, _vcpu, #UML_GADGET_CPU_V0
	add x10, x10, x8, lsl #4
	str x9, [x10]
	str xzr, [x10, #8]
	next_gadget

/* Operation-specific scalar FP gadgets receive a predecoded operand:
 * destination[4:0], left/source[12:8], right[20:16]. Selecting these at build
 * time removes all guest-opcode branches from the arithmetic hot path.
 */
.macro floating_move_register name, bytes
gadget \name
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	add x11, _vcpu, #UML_GADGET_CPU_V0
	add x12, x11, x10, lsl #4
	add x13, x11, x9, lsl #4
	.if \bytes == 2
	ldrh w14, [x12]
	strh w14, [x13]
	strh wzr, [x13, #2]
	str wzr, [x13, #4]
	.elseif \bytes == 4
	ldr w14, [x12]
	str w14, [x13]
	str wzr, [x13, #4]
	.else
	ldr x14, [x12]
	str x14, [x13]
	.endif
	str xzr, [x13, #8]
	next_gadget
.endm

floating_move_register floating_move_half, 2
floating_move_register floating_move_single, 4
floating_move_register floating_move_double, 8

/* An FEAT_AFP twin (afp=1) starts the destination from the old Vd, the
 * operand FPCR.NEP merges a scalar result into. */
.macro floating_unary name, source, destination, element, operation, forward=0, afp=0
.if \afp
afp_gadget \name
.else
vector_entry \name, \forward, Lvector_register0, Lvector_register8, Lscalar_none, Lscalar_none, 1
.endif
	fp_two_vector_operands
	.if \afp
	ldr q2, [x13]
	.elseif !\forward
	movi v2.16b, #0
	.endif
	vector_read \forward, 1, \source, [x12]
	.if \forward
	movi v2.16b, #0
	.endif
	\operation \destination, \source
	str q2, [x13]
	next_gadget
	.if !\afp
	vector_variants \forward, Lvector_register8, Lscalar_none, Lscalar_none, floating_unary, \name, \source, \destination, \element, \operation
	.endif
.endm

/* Scalar FABS/FNEG: one native instruction. Only FPCR.AH and NEP change it,
 * so the base entry needs no guest FP environment; its FEAT_AFP twin installs
 * one and starts the destination from the old Vd. */
.macro floating_sign_scalar name, source, destination, element, arrangement, shift, operation, forward=0, afp=0
.if \afp
afp_gadget \name
	ensure_fp_environment
.else
vector_entry \name, \forward, Lvector_register0, Lvector_register8, Lscalar_none, Lscalar_none, 1
.endif
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	add x11, _vcpu, #UML_GADGET_CPU_V0
	add x12, x11, x10, lsl #4
	add x13, x11, x9, lsl #4
	.if \afp
	ldr q2, [x13]
	.endif
	vector_read \forward, 1, \source, [x12]
	\operation \destination, \source
	str q2, [x13]
	next_gadget
	.if !\afp
	vector_variants \forward, Lvector_register8, Lscalar_none, Lscalar_none, floating_sign_scalar, \name, \source, \destination, \element, \arrangement, \shift, \operation
	.endif
.endm

floating_sign_scalar floating_absolute_half, h0, h2, h, 8h, 11, fabs
floating_sign_scalar floating_absolute_single, s0, s2, s, 4s, 24, fabs
floating_sign_scalar floating_absolute_double, d0, d2, d, 2d, 53, fabs
floating_sign_scalar floating_negate_half, h0, h2, h, 8h, 11, fneg
floating_sign_scalar floating_negate_single, s0, s2, s, 4s, 24, fneg
floating_sign_scalar floating_negate_double, d0, d2, d, 2d, 53, fneg

afp_twins_begin
floating_sign_scalar floating_absolute_half, h0, h2, h, 8h, 11, fabs, afp=1
floating_sign_scalar floating_absolute_single, s0, s2, s, 4s, 24, fabs, afp=1
floating_sign_scalar floating_absolute_double, d0, d2, d, 2d, 53, fabs, afp=1
floating_sign_scalar floating_negate_half, h0, h2, h, 8h, 11, fneg, afp=1
floating_sign_scalar floating_negate_single, s0, s2, s, 4s, 24, fneg, afp=1
floating_sign_scalar floating_negate_double, d0, d2, d, 2d, 53, fneg, afp=1
afp_twins_end

.purgem floating_sign_scalar
floating_unary floating_sqrt_half, h0, h2, h, fsqrt
floating_unary floating_sqrt_single, s0, s2, s, fsqrt
floating_unary floating_sqrt_double, d0, d2, d, fsqrt

afp_twins_begin
floating_unary floating_sqrt_half, h0, h2, h, fsqrt, afp=1
floating_unary floating_sqrt_single, s0, s2, s, fsqrt, afp=1
floating_unary floating_sqrt_double, d0, d2, d, fsqrt, afp=1
afp_twins_end

/* FRINT operation, precision, vector width, and integer bound are selected
 * during translation. Standard forms are one exact host instruction. */
.macro floating_round_scalar name, operation, source, destination, element, afp=0
gadget_or_twin floating_round_\name, \afp
	fp_two_vector_operands
	.if \afp
	ldr q1, [x13]
	.else
	movi v1.16b, #0
	.endif
	ldr \source, [x12]
	\operation \destination, \source
	str q1, [x13]
	next_gadget
.endm

.macro floating_round_scalar_shapes name, operation, afp=0
	floating_round_scalar \name\()_h, \operation, h0, h1, h, \afp
	floating_round_scalar \name\()_s, \operation, s0, s1, s, \afp
	floating_round_scalar \name\()_d, \operation, d0, d1, d, \afp
.endm

.macro floating_round_vector name, operation, arrangement
gadget simd_round_\name
	fp_two_vector_operands
	ldr q0, [x12]
	movi v1.16b, #0
	\operation v1.\arrangement, v0.\arrangement
	str q1, [x13]
	next_gadget
.endm

.macro floating_round_vector_shapes name, operation
	floating_round_vector \name\()_4h, \operation, 4h
	floating_round_vector \name\()_8h, \operation, 8h
	floating_round_vector \name\()_2s, \operation, 2s
	floating_round_vector \name\()_4s, \operation, 4s
	floating_round_vector \name\()_2d, \operation, 2d
.endm

floating_round_scalar_shapes n, frintn
floating_round_scalar_shapes p, frintp
floating_round_scalar_shapes m, frintm
floating_round_scalar_shapes z, frintz
floating_round_scalar_shapes a, frinta
floating_round_scalar_shapes x, frintx
floating_round_scalar_shapes i, frinti
floating_round_vector_shapes n, frintn
floating_round_vector_shapes p, frintp
floating_round_vector_shapes m, frintm
floating_round_vector_shapes z, frintz
floating_round_vector_shapes a, frinta
floating_round_vector_shapes x, frintx
floating_round_vector_shapes i, frinti

floating_round_scalar 32x_s, frint32x, s0, s1, s
floating_round_scalar 32x_d, frint32x, d0, d1, d
floating_round_scalar 32z_s, frint32z, s0, s1, s
floating_round_scalar 32z_d, frint32z, d0, d1, d
floating_round_scalar 64x_s, frint64x, s0, s1, s
floating_round_scalar 64x_d, frint64x, d0, d1, d
floating_round_scalar 64z_s, frint64z, s0, s1, s
floating_round_scalar 64z_d, frint64z, d0, d1, d
floating_round_vector 32x_2s, frint32x, 2s
floating_round_vector 32x_4s, frint32x, 4s
floating_round_vector 32x_2d, frint32x, 2d
floating_round_vector 32z_2s, frint32z, 2s
floating_round_vector 32z_4s, frint32z, 4s
floating_round_vector 32z_2d, frint32z, 2d
floating_round_vector 64x_2s, frint64x, 2s
floating_round_vector 64x_4s, frint64x, 4s
floating_round_vector 64x_2d, frint64x, 2d
floating_round_vector 64z_2s, frint64z, 2s
floating_round_vector 64z_4s, frint64z, 4s
floating_round_vector 64z_2d, frint64z, 2d

afp_twins_begin
floating_round_scalar_shapes n, frintn, 1
floating_round_scalar_shapes p, frintp, 1
floating_round_scalar_shapes m, frintm, 1
floating_round_scalar_shapes z, frintz, 1
floating_round_scalar_shapes a, frinta, 1
floating_round_scalar_shapes x, frintx, 1
floating_round_scalar_shapes i, frinti, 1
floating_round_scalar 32x_s, frint32x, s0, s1, s, 1
floating_round_scalar 32x_d, frint32x, d0, d1, d, 1
floating_round_scalar 32z_s, frint32z, s0, s1, s, 1
floating_round_scalar 32z_d, frint32z, d0, d1, d, 1
floating_round_scalar 64x_s, frint64x, s0, s1, s, 1
floating_round_scalar 64x_d, frint64x, d0, d1, d, 1
floating_round_scalar 64z_s, frint64z, s0, s1, s, 1
floating_round_scalar 64z_d, frint64z, d0, d1, d, 1
afp_twins_end

.purgem floating_round_vector_shapes
.purgem floating_round_vector
.purgem floating_round_scalar_shapes
.purgem floating_round_scalar

gadget floating_round_sized_software
	ensure_fp_environment
	ldr x8, [_stream], #8
	/* Merge any host IDC/other status raised while classifying operands with
	 * the explicit IOC/IXC produced by the bounded reference helper. */
	fpsr_merge_helper floating_round_sized_software
	next_gadget

/* Horizontal/pairwise FP operations are completely specialized by operation
 * and shape and execute one branch-free native instruction. */
.macro floating_horizontal_scalar_binary name, operation, source, destination
gadget floating_horizontal_\name
	ensure_fp_environment
	ldr x11, [_stream], #8
	unpack_u16_3 x11, x8, x9, x10
	ldr q0, [_vcpu, x9]
	ldr \destination, [_vcpu, x10]
	\operation \source, \source, \destination
	str q0, [_vcpu, x8]
	next_gadget
.endm

.macro floating_horizontal_scalar_binary_shapes name, operation
	floating_horizontal_scalar_binary \name\()_h, \operation, h0, h1
	floating_horizontal_scalar_binary \name\()_s, \operation, s0, s1
	floating_horizontal_scalar_binary \name\()_d, \operation, d0, d1
.endm

.macro floating_horizontal_vector name, operation, arrangement
gadget floating_horizontal_\name
	ensure_fp_environment
	ldr x11, [_stream], #8
	unpack_u16_3 x11, x8, x9, x10
	ldr q0, [_vcpu, x9]
	ldr q1, [_vcpu, x10]
	\operation v2.\arrangement, v0.\arrangement, v1.\arrangement
	str q2, [_vcpu, x8]
	next_gadget
.endm

.macro floating_horizontal_vector_shapes name, operation
	floating_horizontal_vector \name\()_4h, \operation, 4h
	floating_horizontal_vector \name\()_8h, \operation, 8h
	floating_horizontal_vector \name\()_2s, \operation, 2s
	floating_horizontal_vector \name\()_4s, \operation, 4s
	floating_horizontal_vector \name\()_2d, \operation, 2d
.endm

.macro floating_horizontal_scalar_pair name, operation, destination, arrangement
gadget floating_horizontal_\name
	ensure_fp_environment
	ldr x10, [_stream], #8
	unpack_u16_2 x10, x8, x9
	ldr q0, [_vcpu, x9]
	\operation \destination, v0.\arrangement
	str q1, [_vcpu, x8]
	next_gadget
.endm

.macro floating_horizontal_scalar_pair_shapes name, operation
	floating_horizontal_scalar_pair \name\()_h, \operation, h1, 2h
	floating_horizontal_scalar_pair \name\()_s, \operation, s1, 2s
	floating_horizontal_scalar_pair \name\()_d, \operation, d1, 2d
.endm

.macro floating_horizontal_reduce name, operation, destination, arrangement
gadget floating_horizontal_\name
	ensure_fp_environment
	ldr x10, [_stream], #8
	unpack_u16_2 x10, x8, x9
	ldr q0, [_vcpu, x9]
	\operation \destination, v0.\arrangement
	str q1, [_vcpu, x8]
	next_gadget
.endm

.macro floating_horizontal_reduce_shapes name, operation
	floating_horizontal_reduce \name\()_4h, \operation, h1, 4h
	floating_horizontal_reduce \name\()_8h, \operation, h1, 8h
	floating_horizontal_reduce \name\()_4s, \operation, s1, 4s
.endm

floating_horizontal_scalar_binary_shapes fabd_scalar, fabd
floating_horizontal_vector_shapes fabd_vector, fabd
floating_horizontal_scalar_pair_shapes faddp_scalar, faddp
floating_horizontal_vector_shapes faddp_vector, faddp

floating_horizontal_scalar_binary_shapes fmax_scalar, fmax
floating_horizontal_vector_shapes fmax_vector, fmax
floating_horizontal_scalar_binary_shapes fmaxnm_scalar, fmaxnm
floating_horizontal_vector_shapes fmaxnm_vector, fmaxnm
floating_horizontal_scalar_binary_shapes fmin_scalar, fmin
floating_horizontal_vector_shapes fmin_vector, fmin
floating_horizontal_scalar_binary_shapes fminnm_scalar, fminnm
floating_horizontal_vector_shapes fminnm_vector, fminnm

floating_horizontal_scalar_pair_shapes fmaxp_scalar, fmaxp
floating_horizontal_vector_shapes fmaxp_vector, fmaxp
floating_horizontal_scalar_pair_shapes fmaxnmp_scalar, fmaxnmp
floating_horizontal_vector_shapes fmaxnmp_vector, fmaxnmp
floating_horizontal_scalar_pair_shapes fminp_scalar, fminp
floating_horizontal_vector_shapes fminp_vector, fminp
floating_horizontal_scalar_pair_shapes fminnmp_scalar, fminnmp
floating_horizontal_vector_shapes fminnmp_vector, fminnmp

floating_horizontal_reduce_shapes fmaxv, fmaxv
floating_horizontal_reduce_shapes fmaxnmv, fmaxnmv
floating_horizontal_reduce_shapes fminv, fminv
floating_horizontal_reduce_shapes fminnmv, fminnmv

.purgem floating_horizontal_reduce_shapes
.purgem floating_horizontal_reduce
.purgem floating_horizontal_scalar_pair_shapes
.purgem floating_horizontal_scalar_pair
.purgem floating_horizontal_vector_shapes
.purgem floating_horizontal_vector
.purgem floating_horizontal_scalar_binary_shapes
.purgem floating_horizontal_scalar_binary

/* BFCVT/BFCVTN map one-for-one on BF16 hosts. The native instruction runs
 * under the guest FPCR, so it follows FPCR.AH, and scalar BFCVT starts from
 * the old destination that FPCR.NEP merges into, so these branch-free forms
 * need no control check or FEAT_AFP twin. */
gadget bfloat_convert_scalar_native
	ensure_fp_environment
	ldr x8, [_stream], #8
	uxth w9, w8
	ubfx x10, x8, #16, #16
	add x12, _vcpu, x10
	add x13, _vcpu, x9
	ldr s0, [x12]
	ldr q1, [x13]
	.inst 0x1e634001
	str q1, [x13]
	next_gadget

gadget bfloat_convert_lower_native
	ensure_fp_environment
	ldr x8, [_stream], #8
	uxth w9, w8
	ubfx x10, x8, #16, #16
	add x12, _vcpu, x10
	add x13, _vcpu, x9
	ldr q0, [x12]
	.inst 0x0ea16802
	str q2, [x13]
	next_gadget

gadget bfloat_convert_upper_native
	ensure_fp_environment
	ldr x8, [_stream], #8
	uxth w9, w8
	ubfx x10, x8, #16, #16
	add x12, _vcpu, x10
	add x13, _vcpu, x9
	ldr q0, [x12]
	ldr q2, [x13]
	.inst 0x4ea16802
	str q2, [x13]
	next_gadget

gadget bfloat_convert_software
	ensure_fp_environment
	ldr x8, [_stream], #8
	fpsr_merge_helper bfloat_conversion_software
	next_gadget

/* One native instruction \encoding on q0, q1 and the accumulator q2, whose
 * CpuState offsets the operand packs at bits 16, 32 and 0. The two macros
 * differ only in how they address CpuState. */
.macro ternary_q_native name, encoding
gadget \name
	ensure_fp_environment
	ldr x8, [_stream], #8
	uxth w9, w8
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	ldr q0, [_vcpu, x10]
	ldr q1, [_vcpu, x11]
	ldr q2, [_vcpu, x9]
	.inst \encoding
	str q2, [_vcpu, x9]
	next_gadget
.endm

.macro ternary_q_native_base name, encoding
gadget \name
	ensure_fp_environment
	ldr x8, [_stream], #8
	uxth w9, w8
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	add x13, _vcpu, x10
	add x14, _vcpu, x11
	add x15, _vcpu, x9
	ldr q0, [x13]
	ldr q1, [x14]
	ldr q2, [x15]
	.inst \encoding
	str q2, [x15]
	next_gadget
.endm

/* Native BFDOT is exact on every BF16 host: FPCR.EBF is RES0, so the guest
 * can only select the standard behavior, which ignores FPCR. Width and indexed
 * pair are fixed by translation, so these gadgets contain no instruction-shape
 * branch. */
ternary_q_native bfloat_dot_native_vector_2s, 0x2e41fc02
ternary_q_native bfloat_dot_native_vector_4s, 0x6e41fc02
ternary_q_native bfloat_dot_native_indexed_2s_0, 0x0f41f002
ternary_q_native bfloat_dot_native_indexed_2s_1, 0x0f61f002
ternary_q_native bfloat_dot_native_indexed_2s_2, 0x0f41f802
ternary_q_native bfloat_dot_native_indexed_2s_3, 0x0f61f802
ternary_q_native bfloat_dot_native_indexed_4s_0, 0x4f41f002
ternary_q_native bfloat_dot_native_indexed_4s_1, 0x4f61f002
ternary_q_native bfloat_dot_native_indexed_4s_2, 0x4f41f802
ternary_q_native bfloat_dot_native_indexed_4s_3, 0x4f61f802

gadget bfloat_dot_software
	ensure_fp_environment
	ldr x8, [_stream], #8
	software_helper bfloat_dot_software
	next_gadget

ternary_q_native_base bfloat_matrix_native, 0x6e41ec02

gadget bfloat_matrix_software
	ensure_fp_environment
	ldr x8, [_stream], #8
	software_helper bfloat_matrix_software
	next_gadget

/* Shape and CpuState offsets are resolved by the builder, leaving no runtime
 * test. */
ternary_q_native half_fused_long_native_vector_add_lower_2s, 0x0e21ec02
ternary_q_native half_fused_long_native_vector_add_lower_4s, 0x4e21ec02
ternary_q_native half_fused_long_native_vector_add_upper_2s, 0x2e21cc02
ternary_q_native half_fused_long_native_vector_add_upper_4s, 0x6e21cc02
ternary_q_native half_fused_long_native_vector_sub_lower_2s, 0x0ea1ec02
ternary_q_native half_fused_long_native_vector_sub_lower_4s, 0x4ea1ec02
ternary_q_native half_fused_long_native_vector_sub_upper_2s, 0x2ea1cc02
ternary_q_native half_fused_long_native_vector_sub_upper_4s, 0x6ea1cc02

.macro half_fused_long_indexed operation, half, shape, prefix, index, bits
	ternary_q_native \
		half_fused_long_native_indexed_\operation\()_\half\()_\shape\()_\index, \
		(\prefix | \bits)
.endm
.macro half_fused_long_indexed_shapes operation, half, shape, prefix
	half_fused_long_indexed \operation, \half, \shape, \prefix, 0, 0x000000
	half_fused_long_indexed \operation, \half, \shape, \prefix, 1, 0x100000
	half_fused_long_indexed \operation, \half, \shape, \prefix, 2, 0x200000
	half_fused_long_indexed \operation, \half, \shape, \prefix, 3, 0x300000
	half_fused_long_indexed \operation, \half, \shape, \prefix, 4, 0x000800
	half_fused_long_indexed \operation, \half, \shape, \prefix, 5, 0x100800
	half_fused_long_indexed \operation, \half, \shape, \prefix, 6, 0x200800
	half_fused_long_indexed \operation, \half, \shape, \prefix, 7, 0x300800
.endm

half_fused_long_indexed_shapes add, lower, 2s, 0x0f810002
half_fused_long_indexed_shapes add, lower, 4s, 0x4f810002
half_fused_long_indexed_shapes add, upper, 2s, 0x2f818002
half_fused_long_indexed_shapes add, upper, 4s, 0x6f818002
half_fused_long_indexed_shapes sub, lower, 2s, 0x0f814002
half_fused_long_indexed_shapes sub, lower, 4s, 0x4f814002
half_fused_long_indexed_shapes sub, upper, 2s, 0x2f81c002
half_fused_long_indexed_shapes sub, upper, 4s, 0x6f81c002

.purgem half_fused_long_indexed_shapes
.purgem half_fused_long_indexed

gadget half_fused_long_software
	ensure_fp_environment
	ldr x8, [_stream], #8
	fpsr_model_helper half_fused_long_software
	next_gadget

/* BFMLALB/BFMLALT map one-for-one to branch-free BF16 instructions. Shape,
 * half selection, and indexed lane are all selected by the builder, leaving
 * only register extraction in the hot path. */
ternary_q_native_base bfloat_multiply_long_native_vector_b, 0x2ec1fc02
ternary_q_native_base bfloat_multiply_long_native_vector_t, 0x6ec1fc02
ternary_q_native_base bfloat_multiply_long_native_indexed_b_0, 0x0fc1f002
ternary_q_native_base bfloat_multiply_long_native_indexed_b_1, 0x0fd1f002
ternary_q_native_base bfloat_multiply_long_native_indexed_b_2, 0x0fe1f002
ternary_q_native_base bfloat_multiply_long_native_indexed_b_3, 0x0ff1f002
ternary_q_native_base bfloat_multiply_long_native_indexed_b_4, 0x0fc1f802
ternary_q_native_base bfloat_multiply_long_native_indexed_b_5, 0x0fd1f802
ternary_q_native_base bfloat_multiply_long_native_indexed_b_6, 0x0fe1f802
ternary_q_native_base bfloat_multiply_long_native_indexed_b_7, 0x0ff1f802
ternary_q_native_base bfloat_multiply_long_native_indexed_t_0, 0x4fc1f002
ternary_q_native_base bfloat_multiply_long_native_indexed_t_1, 0x4fd1f002
ternary_q_native_base bfloat_multiply_long_native_indexed_t_2, 0x4fe1f002
ternary_q_native_base bfloat_multiply_long_native_indexed_t_3, 0x4ff1f002
ternary_q_native_base bfloat_multiply_long_native_indexed_t_4, 0x4fc1f802
ternary_q_native_base bfloat_multiply_long_native_indexed_t_5, 0x4fd1f802
ternary_q_native_base bfloat_multiply_long_native_indexed_t_6, 0x4fe1f802
ternary_q_native_base bfloat_multiply_long_native_indexed_t_7, 0x4ff1f802

gadget bfloat_multiply_long_software
	ensure_fp_environment
	ldr x8, [_stream], #8
	fpsr_merge_helper bfloat_multiply_long_software
	next_gadget

/* Reciprocal operation, arity, precision, and vector width are selected at
 * translation time. Each gadget is one branch-free native instruction. */
.macro floating_reciprocal_scalar_unary name, operation, source, destination, afp=0
gadget_or_twin floating_reciprocal_\name, \afp
	ensure_fp_environment
	ldr x8, [_stream], #8
	uxth w9, w8
	ubfx x10, x8, #16, #16
	add x12, _vcpu, x10
	add x13, _vcpu, x9
	.if \afp
	ldr q1, [x13]
	.endif
	ldr \source, [x12]
	\operation \destination, \source
	str q1, [x13]
	next_gadget
.endm

.macro floating_reciprocal_scalar_unary_shapes name, operation, afp=0
	floating_reciprocal_scalar_unary \name\()_h, \operation, h0, h1, \afp
	floating_reciprocal_scalar_unary \name\()_s, \operation, s0, s1, \afp
	floating_reciprocal_scalar_unary \name\()_d, \operation, d0, d1, \afp
.endm

/* FRECPS and FRSQRTS merge into the whole first source, which the FEAT_AFP
 * twin loads. */
.macro floating_reciprocal_scalar_binary name, operation, source0, source1, destination, afp=0
gadget_or_twin floating_reciprocal_\name, \afp
	ensure_fp_environment
	ldr x8, [_stream], #8
	uxth w9, w8
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	add x13, _vcpu, x10
	add x14, _vcpu, x11
	.if \afp
	ldr q0, [x13]
	.else
	ldr \source0, [x13]
	.endif
	ldr \source1, [x14]
	\operation \destination, \source0, \source1
	add x13, _vcpu, x9
	str q2, [x13]
	next_gadget
.endm

.macro floating_reciprocal_scalar_binary_shapes name, operation, afp=0
	floating_reciprocal_scalar_binary \name\()_h, \operation, h0, h1, h2, \afp
	floating_reciprocal_scalar_binary \name\()_s, \operation, s0, s1, s2, \afp
	floating_reciprocal_scalar_binary \name\()_d, \operation, d0, d1, d2, \afp
.endm

.macro floating_reciprocal_vector name, operation, arrangement, unary
gadget floating_reciprocal_\name
	ensure_fp_environment
	ldr x8, [_stream], #8
	uxth w9, w8
	ubfx x10, x8, #16, #16
	add x12, _vcpu, x10
	ldr q0, [x12]
	.if !\unary
	lsr x10, x8, #32
	add x12, _vcpu, x10
	ldr q1, [x12]
	.endif
	movi v2.16b, #0
	.if \unary
	\operation v2.\arrangement, v0.\arrangement
	.else
	\operation v2.\arrangement, v0.\arrangement, v1.\arrangement
	.endif
	add x12, _vcpu, x9
	str q2, [x12]
	next_gadget
.endm

.macro floating_reciprocal_vector_shapes name, operation, unary
	floating_reciprocal_vector \name\()_4h, \operation, 4h, \unary
	floating_reciprocal_vector \name\()_8h, \operation, 8h, \unary
	floating_reciprocal_vector \name\()_2s, \operation, 2s, \unary
	floating_reciprocal_vector \name\()_4s, \operation, 4s, \unary
	floating_reciprocal_vector \name\()_2d, \operation, 2d, \unary
.endm

floating_reciprocal_scalar_unary_shapes native_frecpe_scalar, frecpe
floating_reciprocal_vector_shapes native_frecpe_vector, frecpe, 1
floating_reciprocal_scalar_binary_shapes native_frecps_scalar, frecps
floating_reciprocal_vector_shapes native_frecps_vector, frecps, 0
floating_reciprocal_scalar_unary_shapes native_frecpx_scalar, frecpx
floating_reciprocal_scalar_unary_shapes native_frsqrte_scalar, frsqrte
floating_reciprocal_vector_shapes native_frsqrte_vector, frsqrte, 1
floating_reciprocal_scalar_binary_shapes native_frsqrts_scalar, frsqrts
floating_reciprocal_vector_shapes native_frsqrts_vector, frsqrts, 0

afp_twins_begin
floating_reciprocal_scalar_unary_shapes native_frecpe_scalar, frecpe, 1
floating_reciprocal_scalar_binary_shapes native_frecps_scalar, frecps, 1
floating_reciprocal_scalar_unary_shapes native_frecpx_scalar, frecpx, 1
floating_reciprocal_scalar_unary_shapes native_frsqrte_scalar, frsqrte, 1
floating_reciprocal_scalar_binary_shapes native_frsqrts_scalar, frsqrts, 1
afp_twins_end

.purgem floating_reciprocal_vector_shapes
.purgem floating_reciprocal_vector
.purgem floating_reciprocal_scalar_binary_shapes
.purgem floating_reciprocal_scalar_binary
.purgem floating_reciprocal_scalar_unary_shapes
.purgem floating_reciprocal_scalar_unary

/* FCADD/FCMLA rotation, shape, and indexed complex source are all selected
 * during translation. The valid hot path is the exact host complex opcode. */
.macro floating_complex_vector name, operation, arrangement, rotation, accumulate, forward=0
.set Lvector_old\@, Lscalar_none
.if \accumulate
.set Lvector_old\@, Lscalar_offset0
.endif
vector_entry floating_complex_\name, \forward, Lscalar_offset0, Lscalar_offset16, Lscalar_offset32, Lvector_old\@, 1
	ensure_fp_environment
	ldr x8, [_stream], #8
	uxth w9, w8
	ubfx x10, x8, #16, #16
	lsr x11, x8, #32
	vector_read \forward, 1, q0, [_vcpu, x10]
	vector_read \forward, 2, q1, [_vcpu, x11]
	.if \accumulate
	vector_read \forward, 4, q2, [_vcpu, x9]
	.else
	movi v2.16b, #0
	.endif
	\operation v2.\arrangement, v0.\arrangement, v1.\arrangement, #\rotation
	str q2, [_vcpu, x9]
	next_gadget
	vector_variants \forward, Lscalar_offset16, Lscalar_offset32, Lvector_old\@, floating_complex_vector, \name, \operation, \arrangement, \rotation, \accumulate
.endm

.macro floating_complex_vector_rotations name, operation, arrangement, accumulate
	floating_complex_vector \name\()_\arrangement\()_0, \operation, \arrangement, 0, \accumulate
	floating_complex_vector \name\()_\arrangement\()_90, \operation, \arrangement, 90, \accumulate
	floating_complex_vector \name\()_\arrangement\()_180, \operation, \arrangement, 180, \accumulate
	floating_complex_vector \name\()_\arrangement\()_270, \operation, \arrangement, 270, \accumulate
.endm

.macro floating_complex_element name, arrangement, element, index, rotation, forward=0
.set Lvector_old\@, Lscalar_offset0
vector_entry floating_complex_\name, \forward, Lscalar_offset0, Lscalar_offset16, Lscalar_offset32, Lvector_old\@, 1
	ensure_fp_environment
	ldr x8, [_stream], #8
	uxth w9, w8
	ubfx x10, x8, #16, #16
	lsr x11, x8, #32
	vector_read \forward, 1, q0, [_vcpu, x10]
	vector_read \forward, 2, q1, [_vcpu, x11]
	vector_read \forward, 4, q2, [_vcpu, x9]
	fcmla v2.\arrangement, v0.\arrangement, v1.\element[\index], #\rotation
	str q2, [_vcpu, x9]
	next_gadget
	vector_variants \forward, Lscalar_offset16, Lscalar_offset32, Lvector_old\@, floating_complex_element, \name, \arrangement, \element, \index, \rotation
.endm

.macro floating_complex_element_rotations name, arrangement, element, index
	floating_complex_element \name\()_\arrangement\()_\index\()_0, \arrangement, \element, \index, 0
	floating_complex_element \name\()_\arrangement\()_\index\()_90, \arrangement, \element, \index, 90
	floating_complex_element \name\()_\arrangement\()_\index\()_180, \arrangement, \element, \index, 180
	floating_complex_element \name\()_\arrangement\()_\index\()_270, \arrangement, \element, \index, 270
.endm

/* FCADD only allocates 90 and 270; unallocated rotations are not emitted. */
floating_complex_vector fcadd_4h_90, fcadd, 4h, 90, 0
floating_complex_vector fcadd_4h_270, fcadd, 4h, 270, 0
floating_complex_vector fcadd_8h_90, fcadd, 8h, 90, 0
floating_complex_vector fcadd_8h_270, fcadd, 8h, 270, 0
floating_complex_vector fcadd_2s_90, fcadd, 2s, 90, 0
floating_complex_vector fcadd_2s_270, fcadd, 2s, 270, 0
floating_complex_vector fcadd_4s_90, fcadd, 4s, 90, 0
floating_complex_vector fcadd_4s_270, fcadd, 4s, 270, 0
floating_complex_vector fcadd_2d_90, fcadd, 2d, 90, 0
floating_complex_vector fcadd_2d_270, fcadd, 2d, 270, 0

floating_complex_vector_rotations fcmla, fcmla, 4h, 1
floating_complex_vector_rotations fcmla, fcmla, 8h, 1
floating_complex_vector_rotations fcmla, fcmla, 2s, 1
floating_complex_vector_rotations fcmla, fcmla, 4s, 1
floating_complex_vector_rotations fcmla, fcmla, 2d, 1

floating_complex_element_rotations fcmla_element, 4h, h, 0
floating_complex_element_rotations fcmla_element, 4h, h, 1
floating_complex_element_rotations fcmla_element, 8h, h, 0
floating_complex_element_rotations fcmla_element, 8h, h, 1
floating_complex_element_rotations fcmla_element, 8h, h, 2
floating_complex_element_rotations fcmla_element, 8h, h, 3
floating_complex_element_rotations fcmla_element, 4s, s, 0
floating_complex_element_rotations fcmla_element, 4s, s, 1

.purgem floating_complex_element_rotations
.purgem floating_complex_element
.purgem floating_complex_vector_rotations
.purgem floating_complex_vector

/* The scalar result merges into the whole first source under FPCR.NEP, so
 * the FEAT_AFP twin loads all of it. */
.macro floating_binary name, left, right, destination, element, operation, forward=0, afp=0
.if \afp
afp_gadget \name
.else
vector_entry \name, \forward, Lvector_register0, Lvector_register8, Lvector_register16, Lscalar_none, 1
.endif
	ensure_fp_environment
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	ubfx w11, w8, #16, #5
	add x12, _vcpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	add x14, x12, x11, lsl #4
	add x15, x12, x9, lsl #4
	.if !\forward
	movi v2.16b, #0
	.endif
	.if \afp
	ldr q0, [x13]
	.else
	vector_read \forward, 1, \left, [x13]
	.endif
	vector_read \forward, 2, \right, [x14]
	.if \forward
	movi v2.16b, #0
	.endif
	\operation \destination, \left, \right
	str q2, [x15]
	next_gadget
	.if !\afp
	vector_variants \forward, Lvector_register8, Lvector_register16, Lscalar_none, floating_binary, \name, \left, \right, \destination, \element, \operation
	.endif
.endm

floating_binary floating_multiply_half, h0, h1, h2, h, fmul
floating_binary floating_multiply_single, s0, s1, s2, s, fmul
floating_binary floating_multiply_double, d0, d1, d2, d, fmul
floating_binary floating_divide_half, h0, h1, h2, h, fdiv
floating_binary floating_divide_single, s0, s1, s2, s, fdiv
floating_binary floating_divide_double, d0, d1, d2, d, fdiv
floating_binary floating_add_half, h0, h1, h2, h, fadd
floating_binary floating_add_single, s0, s1, s2, s, fadd
floating_binary floating_add_double, d0, d1, d2, d, fadd
floating_binary floating_subtract_half, h0, h1, h2, h, fsub
floating_binary floating_subtract_single, s0, s1, s2, s, fsub
floating_binary floating_subtract_double, d0, d1, d2, d, fsub
floating_binary floating_negative_multiply_half, h0, h1, h2, h, fnmul
floating_binary floating_negative_multiply_single, s0, s1, s2, s, fnmul
floating_binary floating_negative_multiply_double, d0, d1, d2, d, fnmul

afp_twins_begin
floating_binary floating_multiply_half, h0, h1, h2, h, fmul, afp=1
floating_binary floating_multiply_single, s0, s1, s2, s, fmul, afp=1
floating_binary floating_multiply_double, d0, d1, d2, d, fmul, afp=1
floating_binary floating_divide_half, h0, h1, h2, h, fdiv, afp=1
floating_binary floating_divide_single, s0, s1, s2, s, fdiv, afp=1
floating_binary floating_divide_double, d0, d1, d2, d, fdiv, afp=1
floating_binary floating_add_half, h0, h1, h2, h, fadd, afp=1
floating_binary floating_add_single, s0, s1, s2, s, fadd, afp=1
floating_binary floating_add_double, d0, d1, d2, d, fadd, afp=1
floating_binary floating_subtract_half, h0, h1, h2, h, fsub, afp=1
floating_binary floating_subtract_single, s0, s1, s2, s, fsub, afp=1
floating_binary floating_subtract_double, d0, d1, d2, d, fsub, afp=1
floating_binary floating_negative_multiply_half, h0, h1, h2, h, fnmul, afp=1
floating_binary floating_negative_multiply_single, s0, s1, s2, s, fnmul, afp=1
floating_binary floating_negative_multiply_double, d0, d1, d2, d, fnmul, afp=1
afp_twins_end

.purgem floating_binary
.purgem floating_unary

/* Every vector operation/shape is selected while translating. The valid
 * gadget path contains one exact host FP instruction and no opcode, width,
 * or arrangement branch. */
.macro simd_basic_floating_binary name, operation, arrangement, forward=0
vector_entry simd_basic_\name, \forward, Lscalar_offset0, Lscalar_offset16, Lscalar_offset32, Lscalar_none, 1
	ensure_fp_environment
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	lsr x11, x8, #32
	vector_read \forward, 1, q0, [_vcpu, x10]
	vector_read \forward, 2, q1, [_vcpu, x11]
	movi v2.16b, #0
	\operation v2.\arrangement, v0.\arrangement, v1.\arrangement
	str q2, [_vcpu, x9]
	next_gadget
	vector_variants \forward, Lscalar_offset16, Lscalar_offset32, Lscalar_none, simd_basic_floating_binary, \name, \operation, \arrangement
.endm

.macro simd_basic_floating_unary name, operation, arrangement, forward=0
vector_entry simd_basic_\name, \forward, Lscalar_offset0, Lscalar_offset16, Lscalar_none, Lscalar_none, 1
	ensure_fp_environment
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	vector_read \forward, 1, q0, [_vcpu, x10]
	movi v2.16b, #0
	\operation v2.\arrangement, v0.\arrangement
	str q2, [_vcpu, x9]
	next_gadget
	vector_variants \forward, Lscalar_offset16, Lscalar_none, Lscalar_none, simd_basic_floating_unary, \name, \operation, \arrangement
.endm

.macro simd_basic_floating_binary_shapes name, operation
	simd_basic_floating_binary \name\()_4h, \operation, 4h
	simd_basic_floating_binary \name\()_8h, \operation, 8h
	simd_basic_floating_binary \name\()_2s, \operation, 2s
	simd_basic_floating_binary \name\()_4s, \operation, 4s
	simd_basic_floating_binary \name\()_2d, \operation, 2d
.endm

simd_basic_floating_binary_shapes fmul, fmul
simd_basic_floating_binary_shapes fdiv, fdiv
simd_basic_floating_binary_shapes fadd, fadd
simd_basic_floating_binary_shapes fsub, fsub
simd_basic_floating_unary fsqrt_4h, fsqrt, 4h
simd_basic_floating_unary fsqrt_8h, fsqrt, 8h
simd_basic_floating_unary fsqrt_2s, fsqrt, 2s
simd_basic_floating_unary fsqrt_4s, fsqrt, 4s
simd_basic_floating_unary fsqrt_2d, fsqrt, 2d

.purgem simd_basic_floating_binary_shapes
.purgem simd_basic_floating_unary
.purgem simd_basic_floating_binary

/* FCMP/FCMPE and FCCMP/FCCMPE are specialized by precision, NaN signaling,
 * zero/register form, and (for conditional forms) condition at translation
 * time. Each is one native host comparison. */
.macro floating_compare_body bits, signal, zero
	ensure_fp_environment
	ubfx w9, w8, #8, #5
	add x11, _vcpu, #UML_GADGET_CPU_V0
	add x12, x11, x9, lsl #4
	.if !\zero
	ubfx w10, w8, #16, #5
	add x13, x11, x10, lsl #4
	.endif
	.if \bits == 16
	ldr h0, [x12]
	.if !\zero
	ldr h1, [x13]
	.endif
	.elseif \bits == 32
	ldr s0, [x12]
	.if !\zero
	ldr s1, [x13]
	.endif
	.else
	ldr d0, [x12]
	.if !\zero
	ldr d1, [x13]
	.endif
	.endif
	.if \bits == 16
		.if \signal
			.if \zero
			fcmpe h0, #0.0
			.else
			fcmpe h0, h1
			.endif
		.else
			.if \zero
			fcmp h0, #0.0
			.else
			fcmp h0, h1
			.endif
		.endif
	.elseif \bits == 32
		.if \signal
			.if \zero
			fcmpe s0, #0.0
			.else
			fcmpe s0, s1
			.endif
		.else
			.if \zero
			fcmp s0, #0.0
			.else
			fcmp s0, s1
			.endif
		.endif
	.else
		.if \signal
			.if \zero
			fcmpe d0, #0.0
			.else
			fcmpe d0, d1
			.endif
		.else
			.if \zero
			fcmp d0, #0.0
			.else
			fcmp d0, d1
			.endif
		.endif
	.endif
	mrs x27, nzcv
	next_gadget
.endm

.macro floating_compare_direct name, bits, signal, zero
gadget \name
	ldr x8, [_stream], #8
	floating_compare_body \bits, \signal, \zero
.endm

floating_compare_direct floating_compare_quiet_h_register, 16, 0, 0
floating_compare_direct floating_compare_quiet_h_zero, 16, 0, 1
floating_compare_direct floating_compare_signaling_h_register, 16, 1, 0
floating_compare_direct floating_compare_signaling_h_zero, 16, 1, 1
floating_compare_direct floating_compare_quiet_s_register, 32, 0, 0
floating_compare_direct floating_compare_quiet_s_zero, 32, 0, 1
floating_compare_direct floating_compare_signaling_s_register, 32, 1, 0
floating_compare_direct floating_compare_signaling_s_zero, 32, 1, 1
floating_compare_direct floating_compare_quiet_d_register, 64, 0, 0
floating_compare_direct floating_compare_quiet_d_zero, 64, 0, 1
floating_compare_direct floating_compare_signaling_d_register, 64, 1, 0
floating_compare_direct floating_compare_signaling_d_zero, 64, 1, 1

.macro floating_conditional_compare name, condition, bits, signal
gadget \name
	ldr x8, [_stream], #8
	msr nzcv, x27
	b.\condition Lfloating_condition_passed_\@
	and w9, w8, #0xf
	lsl w9, w9, #28
	mov w27, w9
	next_gadget
Lfloating_condition_passed_\@:
	floating_compare_body \bits, \signal, 0
.endm

.macro floating_conditional_compare_variants name, condition
	floating_conditional_compare floating_conditional_compare_\name\()_quiet_h, \condition, 16, 0
	floating_conditional_compare floating_conditional_compare_\name\()_signaling_h, \condition, 16, 1
	floating_conditional_compare floating_conditional_compare_\name\()_quiet_s, \condition, 32, 0
	floating_conditional_compare floating_conditional_compare_\name\()_signaling_s, \condition, 32, 1
	floating_conditional_compare floating_conditional_compare_\name\()_quiet_d, \condition, 64, 0
	floating_conditional_compare floating_conditional_compare_\name\()_signaling_d, \condition, 64, 1
.endm

floating_conditional_compare_variants eq, eq
floating_conditional_compare_variants ne, ne
floating_conditional_compare_variants hs, hs
floating_conditional_compare_variants lo, lo
floating_conditional_compare_variants mi, mi
floating_conditional_compare_variants pl, pl
floating_conditional_compare_variants vs, vs
floating_conditional_compare_variants vc, vc
floating_conditional_compare_variants hi, hi
floating_conditional_compare_variants ls, ls
floating_conditional_compare_variants ge, ge
floating_conditional_compare_variants lt, lt
floating_conditional_compare_variants gt, gt
floating_conditional_compare_variants le, le

/* Advanced SIMD floating comparisons are specialized by operation and exact
 * scalar/vector shape and execute the matching native FP16/FP32/FP64
 * instruction directly. */
/* The register forms merge a scalar result into the whole second source
 * under FPCR.NEP, so the FEAT_AFP twin loads all of it; the zero forms do not
 * merge. */
.macro simd_floating_compare_register_scalar name, operation, element, forward=0, afp=0
.if \afp
afp_gadget \name
.else
vector_entry \name, \forward, Lvector_register0, Lvector_register8, Lvector_register16, Lscalar_none, 1
.endif
	ensure_fp_environment
	ldr x8, [_stream], #8
	ubfx w9, w8, #8, #5
	ubfx w10, w8, #16, #5
	add x11, _vcpu, #UML_GADGET_CPU_V0
	add x12, x11, x9, lsl #4
	add x13, x11, x10, lsl #4
	.if !\forward
	movi v2.16b, #0
	.endif
	vector_read \forward, 1, \element\()0, [x12]
	.if \afp
	ldr q1, [x13]
	.else
	vector_read \forward, 2, \element\()1, [x13]
	.endif
	.if \forward
	movi v2.16b, #0
	.endif
	\operation \element\()2, \element\()0, \element\()1
	and w9, w8, #0x1f
	add x12, x11, x9, lsl #4
	str q2, [x12]
	next_gadget
	.if !\afp
	vector_variants \forward, Lvector_register8, Lvector_register16, Lscalar_none, simd_floating_compare_register_scalar, \name, \operation, \element
	.endif
.endm

.macro simd_floating_compare_register_vector name, operation, arrangement, forward=0
vector_entry \name, \forward, Lvector_register0, Lvector_register8, Lvector_register16, Lscalar_none, 1
	ensure_fp_environment
	ldr x8, [_stream], #8
	ubfx w9, w8, #8, #5
	ubfx w10, w8, #16, #5
	add x11, _vcpu, #UML_GADGET_CPU_V0
	add x12, x11, x9, lsl #4
	add x13, x11, x10, lsl #4
	vector_read \forward, 1, q0, [x12]
	vector_read \forward, 2, q1, [x13]
	\operation v2.\arrangement, v0.\arrangement, v1.\arrangement
	and w9, w8, #0x1f
	add x12, x11, x9, lsl #4
	str q2, [x12]
	next_gadget
	vector_variants \forward, Lvector_register8, Lvector_register16, Lscalar_none, simd_floating_compare_register_vector, \name, \operation, \arrangement
.endm

.macro simd_floating_compare_zero_scalar name, operation, element, forward=0
vector_entry \name, \forward, Lvector_register0, Lvector_register8, Lscalar_none, Lscalar_none, 1
	ensure_fp_environment
	ldr x8, [_stream], #8
	ubfx w9, w8, #8, #5
	add x11, _vcpu, #UML_GADGET_CPU_V0
	add x12, x11, x9, lsl #4
	.if !\forward
	movi v2.16b, #0
	.endif
	vector_read \forward, 1, \element\()0, [x12]
	.if \forward
	movi v2.16b, #0
	.endif
	\operation \element\()2, \element\()0, #0.0
	and w9, w8, #0x1f
	add x12, x11, x9, lsl #4
	str q2, [x12]
	next_gadget
	vector_variants \forward, Lvector_register8, Lscalar_none, Lscalar_none, simd_floating_compare_zero_scalar, \name, \operation, \element
.endm

.macro simd_floating_compare_zero_vector name, operation, arrangement, forward=0
vector_entry \name, \forward, Lvector_register0, Lvector_register8, Lscalar_none, Lscalar_none, 1
	ensure_fp_environment
	ldr x8, [_stream], #8
	ubfx w9, w8, #8, #5
	add x11, _vcpu, #UML_GADGET_CPU_V0
	add x12, x11, x9, lsl #4
	vector_read \forward, 1, q0, [x12]
	\operation v2.\arrangement, v0.\arrangement, #0.0
	and w9, w8, #0x1f
	add x12, x11, x9, lsl #4
	str q2, [x12]
	next_gadget
	vector_variants \forward, Lvector_register8, Lscalar_none, Lscalar_none, simd_floating_compare_zero_vector, \name, \operation, \arrangement
.endm

.macro simd_floating_compare_register_variants name, operation
	simd_floating_compare_register_scalar simd_fcmp_\name\()_h, \operation, h
	simd_floating_compare_register_scalar simd_fcmp_\name\()_s, \operation, s
	simd_floating_compare_register_scalar simd_fcmp_\name\()_d, \operation, d
	simd_floating_compare_register_vector simd_fcmp_\name\()_4h, \operation, 4h
	simd_floating_compare_register_vector simd_fcmp_\name\()_8h, \operation, 8h
	simd_floating_compare_register_vector simd_fcmp_\name\()_2s, \operation, 2s
	simd_floating_compare_register_vector simd_fcmp_\name\()_4s, \operation, 4s
	simd_floating_compare_register_vector simd_fcmp_\name\()_2d, \operation, 2d
.endm

.macro simd_floating_compare_zero_variants name, operation
	simd_floating_compare_zero_scalar simd_fcmp_\name\()_h, \operation, h
	simd_floating_compare_zero_scalar simd_fcmp_\name\()_s, \operation, s
	simd_floating_compare_zero_scalar simd_fcmp_\name\()_d, \operation, d
	simd_floating_compare_zero_vector simd_fcmp_\name\()_4h, \operation, 4h
	simd_floating_compare_zero_vector simd_fcmp_\name\()_8h, \operation, 8h
	simd_floating_compare_zero_vector simd_fcmp_\name\()_2s, \operation, 2s
	simd_floating_compare_zero_vector simd_fcmp_\name\()_4s, \operation, 4s
	simd_floating_compare_zero_vector simd_fcmp_\name\()_2d, \operation, 2d
.endm

simd_floating_compare_register_variants eq, fcmeq
simd_floating_compare_register_variants ge, fcmge
simd_floating_compare_register_variants gt, fcmgt
simd_floating_compare_register_variants facge, facge
simd_floating_compare_register_variants facgt, facgt
simd_floating_compare_zero_variants eq0, fcmeq
simd_floating_compare_zero_variants ge0, fcmge
simd_floating_compare_zero_variants gt0, fcmgt
simd_floating_compare_zero_variants le0, fcmle
simd_floating_compare_zero_variants lt0, fcmlt

afp_twins_begin
simd_floating_compare_register_scalar simd_fcmp_eq_h, fcmeq, h, afp=1
simd_floating_compare_register_scalar simd_fcmp_eq_s, fcmeq, s, afp=1
simd_floating_compare_register_scalar simd_fcmp_eq_d, fcmeq, d, afp=1
simd_floating_compare_register_scalar simd_fcmp_ge_h, fcmge, h, afp=1
simd_floating_compare_register_scalar simd_fcmp_ge_s, fcmge, s, afp=1
simd_floating_compare_register_scalar simd_fcmp_ge_d, fcmge, d, afp=1
simd_floating_compare_register_scalar simd_fcmp_gt_h, fcmgt, h, afp=1
simd_floating_compare_register_scalar simd_fcmp_gt_s, fcmgt, s, afp=1
simd_floating_compare_register_scalar simd_fcmp_gt_d, fcmgt, d, afp=1
simd_floating_compare_register_scalar simd_fcmp_facge_h, facge, h, afp=1
simd_floating_compare_register_scalar simd_fcmp_facge_s, facge, s, afp=1
simd_floating_compare_register_scalar simd_fcmp_facge_d, facge, d, afp=1
simd_floating_compare_register_scalar simd_fcmp_facgt_h, facgt, h, afp=1
simd_floating_compare_register_scalar simd_fcmp_facgt_s, facgt, s, afp=1
simd_floating_compare_register_scalar simd_fcmp_facgt_d, facgt, d, afp=1
afp_twins_end

/* FCVTL/FCVTL2 and FCVTN/FCVTN2 are specialized by conversion pair and
 * destination half. The normal path is exactly one native conversion; only
 * AHP for half conversions uses the integer-only helper. */
.macro simd_floating_precision_vector name, operation, destination_arrangement, source_arrangement, upper_narrow, half_involved, afp=0
gadget_or_twin \name, \afp
	fp_two_vector_operands
	ldr q0, [x12]
	.if \half_involved && !\afp
	ldr w16, [_vcpu, #UML_GADGET_CPU_FPCR]
	tbnz w16, #26, Lsimd_fcvt_vector_software_\@
	.endif
	.if \upper_narrow
	ldr d2, [x13]
	.else
	movi v2.16b, #0
	.endif
	\operation v2.\destination_arrangement, v0.\source_arrangement
	str q2, [x13]
	next_gadget
	.if \half_involved && !\afp
Lsimd_fcvt_vector_software_\@:
	fpsr_model_helper simd_floating_precision_vector_software
	next_gadget
	.endif
.endm

simd_floating_precision_vector simd_fcvtl_hs, fcvtl, 4s, 4h, 0, 1
simd_floating_precision_vector simd_fcvtl2_hs, fcvtl2, 4s, 8h, 0, 1
simd_floating_precision_vector simd_fcvtl_sd, fcvtl, 2d, 2s, 0, 0
simd_floating_precision_vector simd_fcvtl2_sd, fcvtl2, 2d, 4s, 0, 0
simd_floating_precision_vector simd_fcvtn_sh, fcvtn, 4h, 4s, 0, 1
simd_floating_precision_vector simd_fcvtn2_sh, fcvtn2, 8h, 4s, 1, 1
simd_floating_precision_vector simd_fcvtn_ds, fcvtn, 2s, 2d, 0, 0
simd_floating_precision_vector simd_fcvtn2_ds, fcvtn2, 4s, 2d, 1, 0
simd_floating_precision_vector simd_fcvtxn_vector, fcvtxn, 2s, 2d, 0, 0
simd_floating_precision_vector simd_fcvtxn2_vector, fcvtxn2, 4s, 2d, 1, 0

/* The host converts half precision under FPCR.AHP itself, which the FEAT_AFP
 * twins rely on: the integer-only helper does not model FIZ or AH. */
afp_twins_begin
simd_floating_precision_vector simd_fcvtl_hs, fcvtl, 4s, 4h, 0, 1, 1
simd_floating_precision_vector simd_fcvtl2_hs, fcvtl2, 4s, 8h, 0, 1, 1
simd_floating_precision_vector simd_fcvtn_sh, fcvtn, 4h, 4s, 0, 1, 1
simd_floating_precision_vector simd_fcvtn2_sh, fcvtn2, 8h, 4s, 1, 1, 1
afp_twins_end

.purgem simd_floating_precision_vector

.macro simd_fcvtxn_scalar afp=0
gadget_or_twin simd_fcvtxn_scalar, \afp
	fp_two_vector_operands
	.if \afp
	ldr q2, [x13]
	.endif
	ldr d0, [x12]
	fcvtxn s2, d0
	str q2, [x13]
	next_gadget
.endm

simd_fcvtxn_scalar
afp_twins_begin
simd_fcvtxn_scalar 1
afp_twins_end

.purgem simd_fcvtxn_scalar

/* FCVT is specialized to all six source/destination precision pairs. The
 * ordinary FPCR path is one native conversion. FPCR.AHP takes a rare
 * integer-only helper path for half-precision pairs, except in the FEAT_AFP
 * twins, which convert natively from the old Vd under every FPCR. */
.macro floating_convert_precision name, source_bits, destination_bits, afp=0
gadget_or_twin \name, \afp
	ensure_fp_environment
	ldr x8, [_stream], #8
	uxth w9, w8
	lsr x10, x8, #16
	add x12, _vcpu, x10
	add x13, _vcpu, x9
	.if \source_bits == 16
	ldr h0, [x12]
	.elseif \source_bits == 32
	ldr s0, [x12]
	.else
	ldr d0, [x12]
	.endif
	.if \afp
	ldr q2, [x13]
	.elseif \source_bits == 16 || \destination_bits == 16
	ldr w16, [_vcpu, #UML_GADGET_CPU_FPCR]
	tbnz w16, #26, Lfloating_convert_software_\@
	.endif
	.if \source_bits == 16 && \destination_bits == 32
	fcvt s2, h0
	.elseif \source_bits == 16 && \destination_bits == 64
	fcvt d2, h0
	.elseif \source_bits == 32 && \destination_bits == 16
	fcvt h2, s0
	.elseif \source_bits == 32 && \destination_bits == 64
	fcvt d2, s0
	.elseif \source_bits == 64 && \destination_bits == 16
	fcvt h2, d0
	.else
	fcvt s2, d0
	.endif
	str q2, [x13]
	next_gadget

	.if (\source_bits == 16 || \destination_bits == 16) && !\afp
Lfloating_convert_software_\@:
	/* x1/x2 are the threaded engine's CpuState/TLB registers. Preserve them
	 * in ABI callee-saved registers across the integer-only helper call. */
	mov x19, x1
	mov x20, x2
	mov x21, x13
	.if \source_bits == 16
	umov w0, v0.h[0]
	.elseif \source_bits == 32
	fmov w0, s0
	.else
	fmov x0, d0
	.endif
	mov w1, #\source_bits
	mov w2, #\destination_bits
	mov w3, w16
	helper_call uml_arm64_convert_floating_precision
	mov x14, x0
	mov x15, x1
	mov x1, x19
	mov x2, x20
	mov x13, x21
	cbz x15, Lfloating_convert_insert_\@
	mrs x17, fpsr
	orr x17, x17, x15
	msr fpsr, x17
Lfloating_convert_insert_\@:
	movi v2.16b, #0
	.if \destination_bits == 16
	ins v2.h[0], w14
	.elseif \destination_bits == 32
	fmov s2, w14
	.else
	fmov d2, x14
	.endif
	str q2, [x13]
	next_gadget
	.endif
.endm

floating_convert_precision floating_convert_h_to_s, 16, 32
floating_convert_precision floating_convert_h_to_d, 16, 64
floating_convert_precision floating_convert_s_to_h, 32, 16
floating_convert_precision floating_convert_s_to_d, 32, 64
floating_convert_precision floating_convert_d_to_h, 64, 16
floating_convert_precision floating_convert_d_to_s, 64, 32

afp_twins_begin
floating_convert_precision floating_convert_h_to_s, 16, 32, 1
floating_convert_precision floating_convert_h_to_d, 16, 64, 1
floating_convert_precision floating_convert_s_to_h, 32, 16, 1
floating_convert_precision floating_convert_s_to_d, 32, 64, 1
floating_convert_precision floating_convert_d_to_h, 64, 16, 1
floating_convert_precision floating_convert_d_to_s, 64, 32, 1
afp_twins_end

/* Floating-to-integer conversions are specialized by rounding operation,
 * destination kind, signedness, source precision, integer width, vector
 * shape, and discard-at-XZR. Each executes one native conversion; forms the
 * host lacks select the integer-only fallback gadget. */
gadget floating_to_integer_software_fallback
	ensure_fp_environment
	ldr x8, [_stream], #8
	fpsr_model_helper floating_to_integer_software
	next_gadget

gadget floating_javascript_convert_software_fallback
	ensure_fp_environment
	ldr x8, [_stream], #8
	fpsr_model_helper floating_javascript_convert_software
	ldr w27, [_vcpu, #UML_GADGET_CPU_NZCV]
	next_gadget

/* The software path under FPCR.FZ corrects the Z flag some hosts set for a
 * flushed subnormal input. Its model has no FIZ or AH, so the FEAT_AFP twins
 * instead correct the flag after the native instruction under every FPCR, as
 * the JIT does: a subnormal input always clears Z. */
.macro floating_javascript_convert name, discard, afp=0
gadget_or_twin \name, \afp
	ensure_fp_environment
	ldr x8, [_stream], #8
	ubfx w10, w8, #8, #5
	add x11, _vcpu, #UML_GADGET_CPU_V0
	add x12, x11, x10, lsl #4
	ldr d0, [x12]
	.if !\afp
	ldr w16, [_vcpu, #UML_GADGET_CPU_FPCR]
	tbnz w16, #24, Lfloating_javascript_convert_software_\@
	.endif
	fjcvtzs w14, d0
	mrs x27, nzcv
	.if \afp
	fmov x15, d0
	ubfx x16, x15, #52, #11
	cbnz x16, Lfloating_javascript_convert_flags_\@
	lsl x16, x15, #12
	cbz x16, Lfloating_javascript_convert_flags_\@
	mov w27, wzr
Lfloating_javascript_convert_flags_\@:
	.endif
	.if \discard == 0
	and w9, w8, #0x1f
	add x13, _vcpu, #UML_GADGET_CPU_X0
	str x14, [x13, x9, lsl #3]
	.endif
	next_gadget
	.if !\afp
Lfloating_javascript_convert_software_\@:
	fpsr_model_helper floating_javascript_convert_software
	ldr w27, [_vcpu, #UML_GADGET_CPU_NZCV]
	next_gadget
	.endif
.endm

floating_javascript_convert floating_javascript_convert, 0
floating_javascript_convert floating_javascript_convert_discard, 1

afp_twins_begin
floating_javascript_convert floating_javascript_convert, 0, 1
floating_javascript_convert floating_javascript_convert_discard, 1, 1
afp_twins_end

.purgem floating_javascript_convert

gadget integer_to_floating_software_fallback
	ensure_fp_environment
	ldr x8, [_stream], #8
	fpsr_model_helper integer_to_floating_software
	next_gadget

.macro gpr_to_floating name, operation, integer, floating, element, zero, afp=0
gadget_or_twin \name, \afp
	ensure_fp_environment
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	add x11, _vcpu, #UML_GADGET_CPU_V0
	add x13, x11, x9, lsl #4
	.if \zero
	mov x14, #0
	.else
	add x12, _vcpu, #UML_GADGET_CPU_X0
	ldr x14, [x12, x10, lsl #3]
	.endif
	afp_merge_source \afp
	\operation \floating\()2, \integer\()14
	str q2, [x13]
	next_gadget
.endm

.macro gpr_to_floating_widths name, operation, integer, zero, afp=0
	gpr_to_floating gpr_to_floating_\name\()_\integer\()_h\zero, \operation, \integer, h, h, \zero, \afp
	gpr_to_floating gpr_to_floating_\name\()_\integer\()_s\zero, \operation, \integer, s, s, \zero, \afp
	gpr_to_floating gpr_to_floating_\name\()_\integer\()_d\zero, \operation, \integer, d, d, \zero, \afp
.endm

.macro gpr_to_floating_variants name, operation, afp=0
	gpr_to_floating_widths \name, \operation, w, 0, \afp
	gpr_to_floating_widths \name, \operation, x, 0, \afp
	gpr_to_floating_widths \name, \operation, w, 1, \afp
	gpr_to_floating_widths \name, \operation, x, 1, \afp
.endm

gpr_to_floating_variants scvtf, scvtf
gpr_to_floating_variants ucvtf, ucvtf

afp_twins_begin
gpr_to_floating_variants scvtf, scvtf, 1
gpr_to_floating_variants ucvtf, ucvtf, 1
afp_twins_end

.purgem gpr_to_floating_variants
.purgem gpr_to_floating_widths
.purgem gpr_to_floating

/* The SIMD&FP conversions between integer and floating-point elements: SCVTF
 * and UCVTF, then the FCVT* group, define the same gadgets with different
 * operations. A scalar element zeroes the rest of the destination unless the
 * FEAT_AFP twin merges into the old Vd. Purged after the FCVT* group. */
.macro simd_fp_convert_scalar name, operation, element, afp=0
gadget_or_twin \name, \afp
	fp_two_vector_operands
	ldr \element\()0, [x12]
	afp_merge_source \afp
	\operation \element\()2, \element\()0
	str q2, [x13]
	next_gadget
.endm

.macro simd_fp_convert_vector name, operation, arrangement
gadget \name
	fp_two_vector_operands
	ldr q0, [x12]
	\operation v2.\arrangement, v0.\arrangement
	str q2, [x13]
	next_gadget
.endm

.macro simd_fp_convert_variants name, operation
	simd_fp_convert_scalar simd_\name\()_h, \operation, h
	simd_fp_convert_scalar simd_\name\()_s, \operation, s
	simd_fp_convert_scalar simd_\name\()_d, \operation, d
	simd_fp_convert_vector simd_\name\()_4h, \operation, 4h
	simd_fp_convert_vector simd_\name\()_8h, \operation, 8h
	simd_fp_convert_vector simd_\name\()_2s, \operation, 2s
	simd_fp_convert_vector simd_\name\()_4s, \operation, 4s
	simd_fp_convert_vector simd_\name\()_2d, \operation, 2d
.endm

simd_fp_convert_variants scvtf, scvtf
simd_fp_convert_variants ucvtf, ucvtf

afp_twins_begin
.irp element, h, s, d
simd_fp_convert_scalar simd_scvtf_\element, scvtf, \element, 1
simd_fp_convert_scalar simd_ucvtf_\element, ucvtf, \element, 1
.endr
afp_twins_end

.macro floating_to_gpr name, operation, integer, floating, discard
gadget \name
	ensure_fp_environment
	ldr x8, [_stream], #8
	ubfx w10, w8, #8, #5
	add x11, _vcpu, #UML_GADGET_CPU_V0
	add x12, x11, x10, lsl #4
	ldr \floating\()0, [x12]
	\operation \integer\()14, \floating\()0
	.if \discard == 0
	and w9, w8, #0x1f
	add x13, _vcpu, #UML_GADGET_CPU_X0
	str x14, [x13, x9, lsl #3]
	.endif
	next_gadget
.endm

.macro floating_to_gpr_widths name, operation, floating
	floating_to_gpr floating_to_gpr_\name\()_w_\floating, \operation, w, \floating, 0
	floating_to_gpr floating_to_gpr_\name\()_x_\floating, \operation, x, \floating, 0
	floating_to_gpr floating_to_gpr_\name\()_w_\floating\()_discard, \operation, w, \floating, 1
	floating_to_gpr floating_to_gpr_\name\()_x_\floating\()_discard, \operation, x, \floating, 1
.endm

.macro floating_to_gpr_variants name, operation
	floating_to_gpr_widths \name, \operation, h
	floating_to_gpr_widths \name, \operation, s
	floating_to_gpr_widths \name, \operation, d
.endm

floating_to_gpr_variants fcvtns, fcvtns
floating_to_gpr_variants fcvtnu, fcvtnu
floating_to_gpr_variants fcvtps, fcvtps
floating_to_gpr_variants fcvtpu, fcvtpu
floating_to_gpr_variants fcvtms, fcvtms
floating_to_gpr_variants fcvtmu, fcvtmu
floating_to_gpr_variants fcvtzs, fcvtzs
floating_to_gpr_variants fcvtzu, fcvtzu
floating_to_gpr_variants fcvtas, fcvtas
floating_to_gpr_variants fcvtau, fcvtau

.purgem floating_to_gpr_variants
.purgem floating_to_gpr_widths
.purgem floating_to_gpr

simd_fp_convert_variants fcvtns, fcvtns
simd_fp_convert_variants fcvtnu, fcvtnu
simd_fp_convert_variants fcvtps, fcvtps
simd_fp_convert_variants fcvtpu, fcvtpu
simd_fp_convert_variants fcvtms, fcvtms
simd_fp_convert_variants fcvtmu, fcvtmu
simd_fp_convert_variants fcvtzs, fcvtzs
simd_fp_convert_variants fcvtzu, fcvtzu
simd_fp_convert_variants fcvtas, fcvtas
simd_fp_convert_variants fcvtau, fcvtau

afp_twins_begin
.irp operation, fcvtns, fcvtnu, fcvtps, fcvtpu, fcvtms, fcvtmu, fcvtzs, fcvtzu, fcvtas, fcvtau
.irp element, h, s, d
simd_fp_convert_scalar simd_\operation\()_\element, \operation, \element, 1
.endr
.endr
afp_twins_end

.purgem simd_fp_convert_variants
.purgem simd_fp_convert_vector
.purgem simd_fp_convert_scalar

/* Fixed-point conversions have an architectural immediate scale. Emit dense,
 * equal-stride tables so the builder selects one exact native gadget at
 * translation time; the running gadget has no scale dispatch. */
.macro floating_fixed_table_begin name
	.globl CSYM(uml_arm64_gadget_\name)
	.p2align 4
CSYM(uml_arm64_gadget_\name):
.endm

.macro floating_fixed_table_end name
	.p2align 4
	.globl CSYM(uml_arm64_gadget_\name\()_end)
CSYM(uml_arm64_gadget_\name\()_end):
.endm

/* A FEAT_AFP twin table has the same stride as its base, which the unit
 * suite checks, so the builder maps an entry by its offset. */
.macro floating_fixed_twin_begin name, afp
	.if \afp
	floating_fixed_table_begin \name\()_afp
	.else
	floating_fixed_table_begin \name
	.endif
.endm

.macro floating_fixed_twin_end name, afp
	.if \afp
	floating_fixed_table_end \name\()_afp
	afp_table_row \name
	.else
	floating_fixed_table_end \name
	.endif
.endm

.macro floating_to_gpr_fixed_body operation, integer, floating, discard, fraction
	.p2align 4
	ensure_fp_environment
	ldr x8, [_stream], #8
	ubfx w10, w8, #8, #5
	add x11, _vcpu, #UML_GADGET_CPU_V0
	add x12, x11, x10, lsl #4
	ldr \floating\()0, [x12]
	\operation \integer\()14, \floating\()0, #\fraction
	.if \discard == 0
	and w9, w8, #0x1f
	add x13, _vcpu, #UML_GADGET_CPU_X0
	str x14, [x13, x9, lsl #3]
	.endif
	next_gadget
.endm

.macro floating_to_gpr_fixed_table name, operation, integer, floating, discard, count
	floating_fixed_table_begin \name
	.irp fraction, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64
		.if \fraction <= \count
			floating_to_gpr_fixed_body \operation, \integer, \floating, \discard, \fraction
		.endif
	.endr
	floating_fixed_table_end \name
.endm

.macro floating_to_gpr_fixed_widths name, operation, floating
	floating_to_gpr_fixed_table floating_to_gpr_fixed_\name\()_w_\floating, \operation, w, \floating, 0, 32
	floating_to_gpr_fixed_table floating_to_gpr_fixed_\name\()_x_\floating, \operation, x, \floating, 0, 64
	floating_to_gpr_fixed_table floating_to_gpr_fixed_\name\()_w_\floating\()_discard, \operation, w, \floating, 1, 32
	floating_to_gpr_fixed_table floating_to_gpr_fixed_\name\()_x_\floating\()_discard, \operation, x, \floating, 1, 64
.endm

.macro floating_to_gpr_fixed_variants name, operation
	floating_to_gpr_fixed_widths \name, \operation, h
	floating_to_gpr_fixed_widths \name, \operation, s
	floating_to_gpr_fixed_widths \name, \operation, d
.endm

floating_to_gpr_fixed_variants fcvtzs, fcvtzs
floating_to_gpr_fixed_variants fcvtzu, fcvtzu

.purgem floating_to_gpr_fixed_variants
.purgem floating_to_gpr_fixed_widths
.purgem floating_to_gpr_fixed_table
.purgem floating_to_gpr_fixed_body

/* The fixed-point forms of the same conversions, FCVTZS and FCVTZU first and
 * then SCVTF and UCVTF, share these tables. Purged after SCVTF and UCVTF. */
.macro simd_fp_convert_fixed_scalar_body operation, element, fraction, afp=0
	.p2align 4
	fp_two_vector_operands
	ldr \element\()0, [x12]
	afp_merge_source \afp
	\operation \element\()2, \element\()0, #\fraction
	str q2, [x13]
	next_gadget
.endm

.macro simd_fp_convert_fixed_vector_body operation, arrangement, fraction
	.p2align 4
	fp_two_vector_operands
	ldr q0, [x12]
	\operation v2.\arrangement, v0.\arrangement, #\fraction
	str q2, [x13]
	next_gadget
.endm

.macro simd_fp_convert_fixed_table name, operation, shape, scalar, count, afp=0
	floating_fixed_twin_begin \name, \afp
	.irp fraction, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64
		.if \fraction <= \count
			.if \scalar
				simd_fp_convert_fixed_scalar_body \operation, \shape, \fraction, \afp
			.else
				simd_fp_convert_fixed_vector_body \operation, \shape, \fraction
			.endif
		.endif
	.endr
	floating_fixed_twin_end \name, \afp
.endm

.macro simd_fp_convert_fixed_variants name, operation
	simd_fp_convert_fixed_table simd_fixed_\name\()_h, \operation, h, 1, 16
	simd_fp_convert_fixed_table simd_fixed_\name\()_s, \operation, s, 1, 32
	simd_fp_convert_fixed_table simd_fixed_\name\()_d, \operation, d, 1, 64
	simd_fp_convert_fixed_table simd_fixed_\name\()_4h, \operation, 4h, 0, 16
	simd_fp_convert_fixed_table simd_fixed_\name\()_8h, \operation, 8h, 0, 16
	simd_fp_convert_fixed_table simd_fixed_\name\()_2s, \operation, 2s, 0, 32
	simd_fp_convert_fixed_table simd_fixed_\name\()_4s, \operation, 4s, 0, 32
	simd_fp_convert_fixed_table simd_fixed_\name\()_2d, \operation, 2d, 0, 64
.endm

simd_fp_convert_fixed_variants fcvtzs, fcvtzs
simd_fp_convert_fixed_variants fcvtzu, fcvtzu

afp_twins_begin
.irp operation, fcvtzs, fcvtzu
simd_fp_convert_fixed_table simd_fixed_\operation\()_h, \operation, h, 1, 16, 1
simd_fp_convert_fixed_table simd_fixed_\operation\()_s, \operation, s, 1, 32, 1
simd_fp_convert_fixed_table simd_fixed_\operation\()_d, \operation, d, 1, 64, 1
.endr
afp_twins_end

.macro gpr_to_floating_fixed_body operation, integer, floating, element, zero, fraction, afp=0
	.p2align 4
	ensure_fp_environment
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	add x11, _vcpu, #UML_GADGET_CPU_V0
	add x13, x11, x9, lsl #4
	.if \zero
	mov x14, #0
	.else
	add x12, _vcpu, #UML_GADGET_CPU_X0
	ldr x14, [x12, x10, lsl #3]
	.endif
	afp_merge_source \afp
	\operation \floating\()2, \integer\()14, #\fraction
	str q2, [x13]
	next_gadget
.endm

.macro gpr_to_floating_fixed_table name, operation, integer, floating, element, zero, count, afp=0
	floating_fixed_twin_begin \name, \afp
	.irp fraction, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64
		.if \fraction <= \count
			gpr_to_floating_fixed_body \operation, \integer, \floating, \element, \zero, \fraction, \afp
		.endif
	.endr
	floating_fixed_twin_end \name, \afp
.endm

.macro gpr_to_floating_fixed_widths name, operation, integer, zero, count, afp=0
	gpr_to_floating_fixed_table gpr_to_floating_fixed_\name\()_\integer\()_h\zero, \operation, \integer, h, h, \zero, \count, \afp
	gpr_to_floating_fixed_table gpr_to_floating_fixed_\name\()_\integer\()_s\zero, \operation, \integer, s, s, \zero, \count, \afp
	gpr_to_floating_fixed_table gpr_to_floating_fixed_\name\()_\integer\()_d\zero, \operation, \integer, d, d, \zero, \count, \afp
.endm

.macro gpr_to_floating_fixed_variants name, operation, afp=0
	gpr_to_floating_fixed_widths \name, \operation, w, 0, 32, \afp
	gpr_to_floating_fixed_widths \name, \operation, x, 0, 64, \afp
	gpr_to_floating_fixed_widths \name, \operation, w, 1, 32, \afp
	gpr_to_floating_fixed_widths \name, \operation, x, 1, 64, \afp
.endm

gpr_to_floating_fixed_variants scvtf, scvtf
gpr_to_floating_fixed_variants ucvtf, ucvtf

afp_twins_begin
gpr_to_floating_fixed_variants scvtf, scvtf, 1
gpr_to_floating_fixed_variants ucvtf, ucvtf, 1
afp_twins_end

.purgem gpr_to_floating_fixed_variants
.purgem gpr_to_floating_fixed_widths
.purgem gpr_to_floating_fixed_table
.purgem gpr_to_floating_fixed_body

simd_fp_convert_fixed_variants scvtf, scvtf
simd_fp_convert_fixed_variants ucvtf, ucvtf

afp_twins_begin
.irp operation, scvtf, ucvtf
simd_fp_convert_fixed_table simd_fixed_\operation\()_h, \operation, h, 1, 16, 1
simd_fp_convert_fixed_table simd_fixed_\operation\()_s, \operation, s, 1, 32, 1
simd_fp_convert_fixed_table simd_fixed_\operation\()_d, \operation, d, 1, 64, 1
.endr
afp_twins_end

.purgem simd_fp_convert_fixed_variants
.purgem simd_fp_convert_fixed_table
.purgem simd_fp_convert_fixed_vector_body
.purgem simd_fp_convert_fixed_scalar_body
.purgem floating_fixed_twin_end
.purgem floating_fixed_twin_begin
.purgem floating_fixed_table_end
.purgem floating_fixed_table_begin

/* Translation packs four direct CpuState vector offsets in destination,
 * first multiplicand, second multiplicand, and addend order. The result
 * merges into the whole addend under FPCR.NEP, so the FEAT_AFP twin loads all
 * of it. */
.macro floating_fused name, first, second, addend, destination, element, operation, afp=0
gadget_or_twin \name, \afp
	ensure_fp_environment
	ldr x12, [_stream], #8
	unpack_u16_4 x12, x8, x9, x10, x11
	movi v3.16b, #0
	ldr \first, [_vcpu, x9]
	ldr \second, [_vcpu, x10]
	.if \afp
	ldr q2, [_vcpu, x11]
	.else
	ldr \addend, [_vcpu, x11]
	.endif
	\operation \destination, \first, \second, \addend
	str q3, [_vcpu, x8]
	next_gadget
.endm

floating_fused floating_madd_half, h0, h1, h2, h3, h, fmadd
floating_fused floating_madd_single, s0, s1, s2, s3, s, fmadd
floating_fused floating_madd_double, d0, d1, d2, d3, d, fmadd
floating_fused floating_msub_half, h0, h1, h2, h3, h, fmsub
floating_fused floating_msub_single, s0, s1, s2, s3, s, fmsub
floating_fused floating_msub_double, d0, d1, d2, d3, d, fmsub
floating_fused floating_nmadd_half, h0, h1, h2, h3, h, fnmadd
floating_fused floating_nmadd_single, s0, s1, s2, s3, s, fnmadd
floating_fused floating_nmadd_double, d0, d1, d2, d3, d, fnmadd
floating_fused floating_nmsub_half, h0, h1, h2, h3, h, fnmsub
floating_fused floating_nmsub_single, s0, s1, s2, s3, s, fnmsub
floating_fused floating_nmsub_double, d0, d1, d2, d3, d, fnmsub

afp_twins_begin
.irp operation, madd, msub, nmadd, nmsub
floating_fused floating_\operation\()_half, h0, h1, h2, h3, h, f\operation, 1
floating_fused floating_\operation\()_single, s0, s1, s2, s3, s, f\operation, 1
floating_fused floating_\operation\()_double, d0, d1, d2, d3, d, f\operation, 1
.endr
afp_twins_end

.purgem floating_fused

/* Translation-specialized vector FABS/FNEG. Valid guest programs execute with
 * no opcode, precision, or Q decode branches. */
/* Only FPCR.AH changes these, so the base entries need no guest FP
 * environment and their FEAT_AFP twins install one. */
.macro simd_floating_sign name, operation, arrangement, shift, forward=0, afp=0
.if \afp
afp_gadget simd_floating_\name
	ensure_fp_environment
.else
vector_entry simd_floating_\name, \forward, Lvector_register0, Lvector_register8, Lscalar_none, Lscalar_none, 1
.endif
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	add x11, _vcpu, #UML_GADGET_CPU_V0
	add x12, x11, x10, lsl #4
	vector_read \forward, 1, q0, [x12]
	\operation v2.\arrangement, v0.\arrangement
	add x12, x11, x9, lsl #4
	str q2, [x12]
	next_gadget
	.if !\afp
	vector_variants \forward, Lvector_register8, Lscalar_none, Lscalar_none, simd_floating_sign, \name, \operation, \arrangement, \shift
	.endif
.endm

simd_floating_sign fabs_4h, fabs, 4h, 11
simd_floating_sign fabs_8h, fabs, 8h, 11
simd_floating_sign fabs_2s, fabs, 2s, 24
simd_floating_sign fabs_4s, fabs, 4s, 24
simd_floating_sign fabs_2d, fabs, 2d, 53
simd_floating_sign fneg_4h, fneg, 4h, 11
simd_floating_sign fneg_8h, fneg, 8h, 11
simd_floating_sign fneg_2s, fneg, 2s, 24
simd_floating_sign fneg_4s, fneg, 4s, 24
simd_floating_sign fneg_2d, fneg, 2d, 53

afp_twins_begin
simd_floating_sign fabs_4h, fabs, 4h, 11, afp=1
simd_floating_sign fabs_8h, fabs, 8h, 11, afp=1
simd_floating_sign fabs_2s, fabs, 2s, 24, afp=1
simd_floating_sign fabs_4s, fabs, 4s, 24, afp=1
simd_floating_sign fabs_2d, fabs, 2d, 53, afp=1
simd_floating_sign fneg_4h, fneg, 4h, 11, afp=1
simd_floating_sign fneg_8h, fneg, 8h, 11, afp=1
simd_floating_sign fneg_2s, fneg, 2s, 24, afp=1
simd_floating_sign fneg_4s, fneg, 4s, 24, afp=1
simd_floating_sign fneg_2d, fneg, 2d, 53, afp=1
afp_twins_end

.purgem simd_floating_sign

/* Translation-specialized same-width integer AdvSIMD gadgets. The builder
 * selects the exact operation and arrangement, so a valid guest instruction
 * executes with no opcode, Q, scalar, or element-size branches here. Operand:
 * destination[4:0], left[12:8], right[20:16]. Native A64 writes also perform
 * the architecturally required zeroing above short-vector/scalar results. */
.macro simd_three_same_vector name, operation, arrangement, accumulate=0, fpstate=0, forward=0
.set Lvector_old\@, Lscalar_none
.if \accumulate
.set Lvector_old\@, Lscalar_offset0
.endif
vector_entry simd_three_same_\name, \forward, Lscalar_offset0, Lscalar_offset16, Lscalar_offset32, Lvector_old\@, 1
	.if \fpstate
	ensure_fp_environment
	.endif
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	lsr x11, x8, #32
	vector_read \forward, 1, q0, [_vcpu, x10]
	vector_read \forward, 2, q1, [_vcpu, x11]
	.if \accumulate
	vector_read \forward, 4, q2, [_vcpu, x9]
	.endif
	\operation v2.\arrangement, v0.\arrangement, v1.\arrangement
	str q2, [_vcpu, x9]
	next_gadget
	vector_variants \forward, Lscalar_offset16, Lscalar_offset32, Lvector_old\@, simd_three_same_vector, \name, \operation, \arrangement, \accumulate, \fpstate
.endm

.macro simd_three_same_scalar name, operation, arrangement, accumulate=0, fpstate=0, forward=0
.set Lvector_old\@, Lscalar_none
.if \accumulate
.set Lvector_old\@, Lscalar_offset0
.endif
vector_entry simd_three_same_\name, \forward, Lscalar_offset0, Lscalar_offset16, Lscalar_offset32, Lvector_old\@, 1
	.if \fpstate
	ensure_fp_environment
	.endif
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	lsr x11, x8, #32
	vector_read \forward, 1, q0, [_vcpu, x10]
	vector_read \forward, 2, q1, [_vcpu, x11]
	.if \accumulate
	vector_read \forward, 4, q2, [_vcpu, x9]
	.endif
	\operation \arrangement\()2, \arrangement\()0, \arrangement\()1
	str q2, [_vcpu, x9]
	next_gadget
	vector_variants \forward, Lscalar_offset16, Lscalar_offset32, Lvector_old\@, simd_three_same_scalar, \name, \operation, \arrangement, \accumulate, \fpstate
.endm

.macro simd_three_same_v6 name, operation, accumulate=0, fpstate=0
	simd_three_same_vector \name\()_8b,  \operation, 8b,  \accumulate, \fpstate
	simd_three_same_vector \name\()_4h,  \operation, 4h,  \accumulate, \fpstate
	simd_three_same_vector \name\()_2s,  \operation, 2s,  \accumulate, \fpstate
	simd_three_same_vector \name\()_16b, \operation, 16b, \accumulate, \fpstate
	simd_three_same_vector \name\()_8h,  \operation, 8h,  \accumulate, \fpstate
	simd_three_same_vector \name\()_4s,  \operation, 4s,  \accumulate, \fpstate
.endm

.macro simd_three_same_v7 name, operation, accumulate=0, fpstate=0
	simd_three_same_v6 \name, \operation, \accumulate, \fpstate
	simd_three_same_vector \name\()_2d, \operation, 2d, \accumulate, \fpstate
.endm

.macro simd_three_same_s4 name, operation, accumulate=0, fpstate=0
	simd_three_same_scalar \name\()_b, \operation, b, \accumulate, \fpstate
	simd_three_same_scalar \name\()_h, \operation, h, \accumulate, \fpstate
	simd_three_same_scalar \name\()_s, \operation, s, \accumulate, \fpstate
	simd_three_same_scalar \name\()_d, \operation, d, \accumulate, \fpstate
.endm

/* Halving arithmetic. */
simd_three_same_v6 shadd,  shadd
simd_three_same_v6 uhadd,  uhadd
simd_three_same_v6 srhadd, srhadd
simd_three_same_v6 urhadd, urhadd
simd_three_same_v6 shsub,  shsub
simd_three_same_v6 uhsub,  uhsub

/* Saturating add/subtract and shifts update cumulative FPSR.QC. */
simd_three_same_v7 sqadd,  sqadd,  0, 1
simd_three_same_s4 sqadd,  sqadd,  0, 1
simd_three_same_v7 uqadd,  uqadd,  0, 1
simd_three_same_s4 uqadd,  uqadd,  0, 1
simd_three_same_v7 sqsub,  sqsub,  0, 1
simd_three_same_s4 sqsub,  sqsub,  0, 1
simd_three_same_v7 uqsub,  uqsub,  0, 1
simd_three_same_s4 uqsub,  uqsub,  0, 1
simd_three_same_v7 sqshl,  sqshl,  0, 1
simd_three_same_s4 sqshl,  sqshl,  0, 1
simd_three_same_v7 uqshl,  uqshl,  0, 1
simd_three_same_s4 uqshl,  uqshl,  0, 1
simd_three_same_v7 sqrshl, sqrshl, 0, 1
simd_three_same_s4 sqrshl, sqrshl, 0, 1
simd_three_same_v7 uqrshl, uqrshl, 0, 1
simd_three_same_s4 uqrshl, uqrshl, 0, 1

/* Variable shifts and integer comparisons. */
simd_three_same_v7 sshl, sshl
simd_three_same_scalar sshl_d, sshl, d
simd_three_same_v7 ushl, ushl
simd_three_same_scalar ushl_d, ushl, d
simd_three_same_v7 srshl, srshl
simd_three_same_scalar srshl_d, srshl, d
simd_three_same_v7 urshl, urshl
simd_three_same_scalar urshl_d, urshl, d
simd_three_same_v7 cmgt, cmgt
simd_three_same_scalar cmgt_d, cmgt, d
simd_three_same_v7 cmhi, cmhi
simd_three_same_scalar cmhi_d, cmhi, d
simd_three_same_v7 cmge, cmge
simd_three_same_scalar cmge_d, cmge, d
simd_three_same_v7 cmhs, cmhs
simd_three_same_scalar cmhs_d, cmhs, d

/* Min/max, absolute difference, and accumulating absolute difference. */
simd_three_same_v6 smax, smax
simd_three_same_v6 umax, umax
simd_three_same_v6 smin, smin
simd_three_same_v6 umin, umin
simd_three_same_v6 sabd, sabd
simd_three_same_v6 uabd, uabd
simd_three_same_v6 saba, saba, 1
simd_three_same_v6 uaba, uaba, 1

/* Modular arithmetic, tests, and multiply/accumulate. */
simd_three_same_v7 add, add
simd_three_same_scalar add_d, add, d
simd_three_same_v7 sub, sub
simd_three_same_scalar sub_d, sub, d
simd_three_same_v7 cmtst, cmtst
simd_three_same_scalar cmtst_d, cmtst, d
simd_three_same_v7 cmeq, cmeq
simd_three_same_scalar cmeq_d, cmeq, d
simd_three_same_v6 mla, mla, 1
simd_three_same_v6 mls, mls, 1
simd_three_same_v6 mul, mul
simd_three_same_vector pmul_8b, pmul, 8b
simd_three_same_vector pmul_16b, pmul, 16b

/* Pairwise integer operations. */
simd_three_same_v6 smaxp, smaxp
simd_three_same_v6 umaxp, umaxp
simd_three_same_v6 sminp, sminp
simd_three_same_v6 uminp, uminp
simd_three_same_v7 addp, addp

/* Signed saturating doubling multiply-high (with and without rounding). */
simd_three_same_vector sqdmulh_4h, sqdmulh, 4h, 0, 1
simd_three_same_vector sqdmulh_2s, sqdmulh, 2s, 0, 1
simd_three_same_vector sqdmulh_8h, sqdmulh, 8h, 0, 1
simd_three_same_vector sqdmulh_4s, sqdmulh, 4s, 0, 1
simd_three_same_scalar sqdmulh_h, sqdmulh, h, 0, 1
simd_three_same_scalar sqdmulh_s, sqdmulh, s, 0, 1
simd_three_same_vector sqrdmulh_4h, sqrdmulh, 4h, 0, 1
simd_three_same_vector sqrdmulh_2s, sqrdmulh, 2s, 0, 1
simd_three_same_vector sqrdmulh_8h, sqrdmulh, 8h, 0, 1
simd_three_same_vector sqrdmulh_4s, sqrdmulh, 4s, 0, 1
simd_three_same_scalar sqrdmulh_h, sqrdmulh, h, 0, 1
simd_three_same_scalar sqrdmulh_s, sqrdmulh, s, 0, 1

/* FEAT_RDM three-same-extra operations use the destination as the exact
 * accumulator. They are kept in dedicated symbols because their encoding
 * class is separate from ordinary three-same. Translation packs three direct
 * CpuState offsets in one stream cell. */
.macro simd_rdm_direct name, operation, destination, left, right, load_destination, load_left, load_right
gadget simd_three_same_\name
	ensure_fp_environment
	ldr x8, [_stream], #8
	unpack_u16_3 x8, x13, x14, x15
	ldr \load_destination, [_vcpu, x13]
	ldr \load_left, [_vcpu, x14]
	ldr \load_right, [_vcpu, x15]
	\operation \destination, \left, \right
	str q0, [_vcpu, x13]
	next_gadget
.endm

.macro simd_rdm_direct_family name, operation
	simd_rdm_direct \name\()_4h, \operation, v0.4h, v1.4h, v2.4h, q0, q1, q2
	simd_rdm_direct \name\()_2s, \operation, v0.2s, v1.2s, v2.2s, q0, q1, q2
	simd_rdm_direct \name\()_8h, \operation, v0.8h, v1.8h, v2.8h, q0, q1, q2
	simd_rdm_direct \name\()_4s, \operation, v0.4s, v1.4s, v2.4s, q0, q1, q2
	simd_rdm_direct \name\()_h, \operation, h0, h1, h2, h0, h1, h2
	simd_rdm_direct \name\()_s, \operation, s0, s1, s2, s0, s1, s2
.endm

simd_rdm_direct_family sqrdmlah, sqrdmlah
simd_rdm_direct_family sqrdmlsh, sqrdmlsh

.purgem simd_rdm_direct_family
.purgem simd_rdm_direct

/* Hosts without FEAT_RDM use one cold exact helper selected at translation.
 * Synchronize cumulative native FPSR state on both sides of the call. */
gadget simd_rdm_software
	ensure_fp_environment
	ldr w8, [_stream], #8
	mrs x16, fpsr
	str w16, [_vcpu, #UML_GADGET_CPU_FPSR]
	stp x1, x2, [sp, #-16]!
	mov x0, x1
	mov w1, w8
	helper_call uml_arm64_simd_rdm_software
	ldp x1, x2, [sp], #16
	cbz w0, CSYM(uml_arm64_gadget_illegal)
	ldr w16, [_vcpu, #UML_GADGET_CPU_FPSR]
	msr fpsr, x16
	add _stream, _stream, #8
	next_gadget

.purgem simd_three_same_s4
.purgem simd_three_same_v7
.purgem simd_three_same_v6
.purgem simd_three_same_scalar
.purgem simd_three_same_vector

/* Integer dot products accumulate groups of four bytes into 32-bit lanes.
 * DotProd hosts use exact native instructions. FEAT_I8MM mixed-signedness
 * forms additionally have branch-free baseline-AdvSIMD fallbacks selected by
 * the builder when the host extension is unavailable. */
.macro simd_dot_vector name, operation, destination, sources, wide
gadget simd_dot_vector_\name
	ldr x8, [_stream], #8
	unpack_u16_3 x8, x9, x10, x11
	ldr q0, [_vcpu, x9]
	ldr q1, [_vcpu, x10]
	ldr q2, [_vcpu, x11]
	\operation v0.\destination, v1.\sources, v2.\sources
	.if \wide
	str q0, [_vcpu, x9]
	.else
	str d0, [_vcpu, x9]
	add x9, x9, #8
	str xzr, [_vcpu, x9]
	.endif
	next_gadget
.endm

simd_dot_vector sdot_2s, sdot, 2s, 8b, 0
simd_dot_vector sdot_4s, sdot, 4s, 16b, 1
simd_dot_vector udot_2s, udot, 2s, 8b, 0
simd_dot_vector udot_4s, udot, 4s, 16b, 1
simd_dot_vector usdot_2s, usdot, 2s, 8b, 0
simd_dot_vector usdot_4s, usdot, 4s, 16b, 1

.purgem simd_dot_vector

.macro simd_dot_indexed name, operation, destination, sources, index, wide
gadget simd_dot_indexed_\name\()_\destination\()_\index
	ldr x8, [_stream], #8
	unpack_u16_3 x8, x9, x10, x11
	ldr q0, [_vcpu, x9]
	ldr q1, [_vcpu, x10]
	ldr q2, [_vcpu, x11]
	\operation v0.\destination, v1.\sources, v2.4b[\index]
	.if \wide
	str q0, [_vcpu, x9]
	.else
	str d0, [_vcpu, x9]
	add x9, x9, #8
	str xzr, [_vcpu, x9]
	.endif
	next_gadget
.endm

.macro simd_dot_indexed_family name, operation
	.irp index, 0, 1, 2, 3
		simd_dot_indexed \name, \operation, 2s, 8b, \index, 0
		simd_dot_indexed \name, \operation, 4s, 16b, \index, 1
	.endr
.endm

simd_dot_indexed_family sdot, sdot
simd_dot_indexed_family udot, udot
simd_dot_indexed_family sudot, sudot
simd_dot_indexed_family usdot, usdot

.purgem simd_dot_indexed_family
.purgem simd_dot_indexed

.macro simd_dot_software_body signed_left, signed_right, wide
	.if \signed_left
	sxtl v3.8h, v1.8b
	.else
	uxtl v3.8h, v1.8b
	.endif
	.if \signed_right
	sxtl v4.8h, v2.8b
	.else
	uxtl v4.8h, v2.8b
	.endif
	mul v3.8h, v3.8h, v4.8h
	.if !\signed_left && !\signed_right
	uaddlp v3.4s, v3.8h
	.else
	saddlp v3.4s, v3.8h
	.endif
	addp v3.4s, v3.4s, v3.4s
	.if \wide
	.if \signed_left
	sxtl2 v5.8h, v1.16b
	.else
	uxtl2 v5.8h, v1.16b
	.endif
	.if \signed_right
	sxtl2 v6.8h, v2.16b
	.else
	uxtl2 v6.8h, v2.16b
	.endif
	mul v5.8h, v5.8h, v6.8h
	.if !\signed_left && !\signed_right
	uaddlp v5.4s, v5.8h
	.else
	saddlp v5.4s, v5.8h
	.endif
	addp v5.4s, v5.4s, v5.4s
	mov v3.d[1], v5.d[0]
	add v0.4s, v0.4s, v3.4s
	.else
	add v0.2s, v0.2s, v3.2s
	.endif
.endm

.macro simd_dot_vector_software name, signed_left, signed_right, wide
gadget simd_dot_vector_\name\()_soft
	ldr x8, [_stream], #8
	unpack_u16_3 x8, x9, x10, x11
	ldr q0, [_vcpu, x9]
	ldr q1, [_vcpu, x10]
	ldr q2, [_vcpu, x11]
	simd_dot_software_body \signed_left, \signed_right, \wide
	.if \wide
	str q0, [_vcpu, x9]
	.else
	str d0, [_vcpu, x9]
	add x9, x9, #8
	str xzr, [_vcpu, x9]
	.endif
	next_gadget
.endm

simd_dot_vector_software sdot_2s, 1, 1, 0
simd_dot_vector_software sdot_4s, 1, 1, 1
simd_dot_vector_software udot_2s, 0, 0, 0
simd_dot_vector_software udot_4s, 0, 0, 1
simd_dot_vector_software usdot_2s, 0, 1, 0
simd_dot_vector_software usdot_4s, 0, 1, 1

.purgem simd_dot_vector_software

.macro simd_dot_indexed_software name, signed_left, signed_right, shape, index, wide
gadget simd_dot_indexed_\name\()_\shape\()_\index\()_soft
	ldr x8, [_stream], #8
	unpack_u16_3 x8, x9, x10, x11
	ldr q0, [_vcpu, x9]
	ldr q1, [_vcpu, x10]
	ldr q2, [_vcpu, x11]
	dup v2.4s, v2.s[\index]
	simd_dot_software_body \signed_left, \signed_right, \wide
	.if \wide
	str q0, [_vcpu, x9]
	.else
	str d0, [_vcpu, x9]
	add x9, x9, #8
	str xzr, [_vcpu, x9]
	.endif
	next_gadget
.endm

.macro simd_dot_indexed_software_family name, signed_left, signed_right
	.irp index, 0, 1, 2, 3
		simd_dot_indexed_software \name, \signed_left, \signed_right, 2s, \index, 0
		simd_dot_indexed_software \name, \signed_left, \signed_right, 4s, \index, 1
	.endr
.endm

simd_dot_indexed_software_family sdot, 1, 1
simd_dot_indexed_software_family udot, 0, 0
simd_dot_indexed_software_family sudot, 1, 0
simd_dot_indexed_software_family usdot, 0, 1

.purgem simd_dot_indexed_software_family
.purgem simd_dot_indexed_software
.purgem simd_dot_software_body

.macro simd_matrix_native name, operation
gadget simd_matrix_\name
	ldr x8, [_stream], #8
	unpack_u16_3 x8, x9, x10, x11
	ldr q0, [_vcpu, x9]
	ldr q1, [_vcpu, x10]
	ldr q2, [_vcpu, x11]
	\operation v0.4s, v1.16b, v2.16b
	str q0, [_vcpu, x9]
	next_gadget
.endm

simd_matrix_native smmla, smmla
simd_matrix_native ummla, ummla
simd_matrix_native usmmla, usmmla

.purgem simd_matrix_native

/* The fallback performs the same 2x8 by 8x2 matrix product using native
 * widening and pairwise operations.  Signedness is fixed per gadget and the
 * valid path has no data-dependent or decode branches. */
.macro simd_matrix_reduce low, high, unsigned_product
	.if \unsigned_product
	uaddlp v\low\().4s, v\low\().8h
	uaddlp v\high\().4s, v\high\().8h
	.else
	saddlp v\low\().4s, v\low\().8h
	saddlp v\high\().4s, v\high\().8h
	.endif
	addp v\low\().4s, v\low\().4s, v\high\().4s
	addp v\low\().4s, v\low\().4s, v\low\().4s
.endm

.macro simd_matrix_software name, signed_left, signed_right, unsigned_product
gadget simd_matrix_\name\()_soft
	ldr x8, [_stream], #8
	unpack_u16_3 x8, x9, x10, x11
	ldr q0, [_vcpu, x10]
	ldr q1, [_vcpu, x11]
	ldr q16, [_vcpu, x9]
	.if \signed_left
	sxtl v2.8h, v0.8b
	sxtl2 v3.8h, v0.16b
	.else
	uxtl v2.8h, v0.8b
	uxtl2 v3.8h, v0.16b
	.endif
	.if \signed_right
	sxtl v4.8h, v1.8b
	sxtl2 v5.8h, v1.16b
	.else
	uxtl v4.8h, v1.8b
	uxtl2 v5.8h, v1.16b
	.endif
	mul v6.8h, v2.8h, v4.8h
	mul v7.8h, v3.8h, v4.8h
	simd_matrix_reduce 6, 7, \unsigned_product
	mov v4.16b, v6.16b
	mul v6.8h, v2.8h, v5.8h
	mul v7.8h, v3.8h, v5.8h
	simd_matrix_reduce 6, 7, \unsigned_product
	zip1 v4.4s, v4.4s, v6.4s
	add v16.4s, v16.4s, v4.4s
	str q16, [_vcpu, x9]
	next_gadget
.endm

simd_matrix_software smmla, 1, 1, 0
simd_matrix_software ummla, 0, 0, 1
simd_matrix_software usmmla, 0, 1, 0

.purgem simd_matrix_software
.purgem simd_matrix_reduce

/* Translation-specialized widening arithmetic. As with the three-same matrix
 * above, the builder resolves operation, signedness, element size, and
 * lower/upper-half form once. */
.macro simd_widening_vector name, operation, destination, left, right, accumulate=0, fpstate=0, forward=0
.set Lvector_old\@, Lscalar_none
.if \accumulate
.set Lvector_old\@, Lscalar_offset0
.endif
vector_entry simd_widening_\name, \forward, Lscalar_offset0, Lscalar_offset16, Lscalar_offset32, Lvector_old\@, 1
	.if \fpstate
	ensure_fp_environment
	.endif
	ldr x8, [_stream], #8
	unpack_u16_3 x8, x9, x10, x11
	vector_read \forward, 1, q0, [_vcpu, x10]
	vector_read \forward, 2, q1, [_vcpu, x11]
	.if \accumulate
	vector_read \forward, 4, q2, [_vcpu, x9]
	.endif
	\operation v2.\destination, v0.\left, v1.\right
	str q2, [_vcpu, x9]
	next_gadget
	vector_variants \forward, Lscalar_offset16, Lscalar_offset32, Lvector_old\@, simd_widening_vector, \name, \operation, \destination, \left, \right, \accumulate, \fpstate
.endm

.macro simd_widening_scalar name, operation, destination, source, accumulate=0, forward=0
.set Lvector_old\@, Lscalar_none
.if \accumulate
.set Lvector_old\@, Lscalar_offset0
.endif
vector_entry simd_widening_\name, \forward, Lscalar_offset0, Lscalar_offset16, Lscalar_offset32, Lvector_old\@, 1
	ensure_fp_environment
	ldr x8, [_stream], #8
	unpack_u16_3 x8, x9, x10, x11
	vector_read \forward, 1, q0, [_vcpu, x10]
	vector_read \forward, 2, q1, [_vcpu, x11]
	.if \accumulate
	vector_read \forward, 4, q2, [_vcpu, x9]
	.endif
	\operation \destination\()2, \source\()0, \source\()1
	str q2, [_vcpu, x9]
	next_gadget
	vector_variants \forward, Lscalar_offset16, Lscalar_offset32, Lvector_old\@, simd_widening_scalar, \name, \operation, \destination, \source, \accumulate
.endm

.macro simd_widening_long3 name, operation, operation2, accumulate=0, fpstate=0
	simd_widening_vector \name\()_8b,  \operation,  8h, 8b,  8b,  \accumulate, \fpstate
	simd_widening_vector \name\()_4h,  \operation,  4s, 4h,  4h,  \accumulate, \fpstate
	simd_widening_vector \name\()_2s,  \operation,  2d, 2s,  2s,  \accumulate, \fpstate
	simd_widening_vector \name\()_16b, \operation2, 8h, 16b, 16b, \accumulate, \fpstate
	simd_widening_vector \name\()_8h,  \operation2, 4s, 8h,  8h,  \accumulate, \fpstate
	simd_widening_vector \name\()_4s,  \operation2, 2d, 4s,  4s,  \accumulate, \fpstate
.endm

.macro simd_widening_wide3 name, operation, operation2
	simd_widening_vector \name\()_8b,  \operation,  8h, 8h, 8b
	simd_widening_vector \name\()_4h,  \operation,  4s, 4s, 4h
	simd_widening_vector \name\()_2s,  \operation,  2d, 2d, 2s
	simd_widening_vector \name\()_16b, \operation2, 8h, 8h, 16b
	simd_widening_vector \name\()_8h,  \operation2, 4s, 4s, 8h
	simd_widening_vector \name\()_4s,  \operation2, 2d, 2d, 4s
.endm

/* High-narrow streams carry three packed CpuState offsets. The upper-half
 * forms alone load the old destination because lower-half forms clear it. */
.macro simd_high_narrow_vector name, operation, destination, left, right, preserve=0, forward=0
.set Lvector_old\@, Lscalar_none
.if \preserve
.set Lvector_old\@, Lscalar_offset0
.endif
vector_entry simd_widening_\name, \forward, Lscalar_offset0, Lscalar_offset16, Lscalar_offset32, Lvector_old\@, 1
	ldr x8, [_stream], #8
	unpack_u16_3 x8, x9, x10, x11
	vector_read \forward, 1, q0, [_vcpu, x10]
	vector_read \forward, 2, q1, [_vcpu, x11]
	.if \preserve
	vector_read \forward, 4, q2, [_vcpu, x9]
	.endif
	\operation v2.\destination, v0.\left, v1.\right
	str q2, [_vcpu, x9]
	next_gadget
	vector_variants \forward, Lscalar_offset16, Lscalar_offset32, Lvector_old\@, simd_high_narrow_vector, \name, \operation, \destination, \left, \right, \preserve
.endm

.macro simd_high_narrow3 name, operation, operation2
	simd_high_narrow_vector \name\()_8h,  \operation,  8b, 8h, 8h
	simd_high_narrow_vector \name\()_4s,  \operation,  4h, 4s, 4s
	simd_high_narrow_vector \name\()_2d,  \operation,  2s, 2d, 2d
	simd_high_narrow_vector \name\()_8h2, \operation2, 16b, 8h, 8h, 1
	simd_high_narrow_vector \name\()_4s2, \operation2, 8h,  4s, 4s, 1
	simd_high_narrow_vector \name\()_2d2, \operation2, 4s,  2d, 2d, 1
.endm

simd_widening_long3 saddl, saddl, saddl2
simd_widening_long3 uaddl, uaddl, uaddl2
simd_widening_wide3 saddw, saddw, saddw2
simd_widening_wide3 uaddw, uaddw, uaddw2
simd_widening_long3 ssubl, ssubl, ssubl2
simd_widening_long3 usubl, usubl, usubl2
simd_widening_wide3 ssubw, ssubw, ssubw2
simd_widening_wide3 usubw, usubw, usubw2

simd_high_narrow3 addhn,  addhn,  addhn2
simd_high_narrow3 raddhn, raddhn, raddhn2
simd_high_narrow3 subhn,  subhn,  subhn2
simd_high_narrow3 rsubhn, rsubhn, rsubhn2

simd_widening_long3 sabdl, sabdl, sabdl2
simd_widening_long3 uabdl, uabdl, uabdl2
simd_widening_long3 sabal, sabal, sabal2, 1
simd_widening_long3 uabal, uabal, uabal2, 1
simd_widening_long3 smlal, smlal, smlal2, 1
simd_widening_long3 umlal, umlal, umlal2, 1
simd_widening_long3 smlsl, smlsl, smlsl2, 1
simd_widening_long3 umlsl, umlsl, umlsl2, 1
simd_widening_long3 smull, smull, smull2
simd_widening_long3 umull, umull, umull2

/* Saturating doubling long multiply is allocated only for H/S sources. */
simd_widening_vector sqdmlal_4h, sqdmlal, 4s, 4h, 4h, 1, 1
simd_widening_vector sqdmlal_2s, sqdmlal, 2d, 2s, 2s, 1, 1
simd_widening_vector sqdmlal_8h, sqdmlal2, 4s, 8h, 8h, 1, 1
simd_widening_vector sqdmlal_4s, sqdmlal2, 2d, 4s, 4s, 1, 1
simd_widening_scalar sqdmlal_h, sqdmlal, s, h, 1
simd_widening_scalar sqdmlal_s, sqdmlal, d, s, 1
simd_widening_vector sqdmlsl_4h, sqdmlsl, 4s, 4h, 4h, 1, 1
simd_widening_vector sqdmlsl_2s, sqdmlsl, 2d, 2s, 2s, 1, 1
simd_widening_vector sqdmlsl_8h, sqdmlsl2, 4s, 8h, 8h, 1, 1
simd_widening_vector sqdmlsl_4s, sqdmlsl2, 2d, 4s, 4s, 1, 1
simd_widening_scalar sqdmlsl_h, sqdmlsl, s, h, 1
simd_widening_scalar sqdmlsl_s, sqdmlsl, d, s, 1
simd_widening_vector sqdmull_4h, sqdmull, 4s, 4h, 4h, 0, 1
simd_widening_vector sqdmull_2s, sqdmull, 2d, 2s, 2s, 0, 1
simd_widening_vector sqdmull_8h, sqdmull2, 4s, 8h, 8h, 0, 1
simd_widening_vector sqdmull_4s, sqdmull2, 2d, 4s, 4s, 0, 1
simd_widening_scalar sqdmull_h, sqdmull, s, h
simd_widening_scalar sqdmull_s, sqdmull, d, s

.purgem simd_high_narrow3
.purgem simd_high_narrow_vector
.purgem simd_widening_wide3
.purgem simd_widening_long3
.purgem simd_widening_scalar
.purgem simd_widening_vector

/* Translation-specialized integer two-register operations. The stream packs
 * direct CpuState vector byte offsets; runtime performs no guest-register
 * decoding or vector-base address formation. */
.macro simd_unary_vector name, operation, destination, source, accumulate=0, fpstate=0, forward=0
.set Lvector_old\@, Lscalar_none
.if \accumulate
.set Lvector_old\@, Lscalar_offset0
.endif
vector_entry simd_unary_\name, \forward, Lscalar_offset0, Lscalar_offset16, Lvector_old\@, Lscalar_none, 1
	.if \fpstate
	ensure_fp_environment
	.endif
	ldr x8, [_stream], #8
	uxth w9, w8
	lsr w10, w8, #16
	vector_read \forward, 1, q0, [_vcpu, x10]
	.if \accumulate
	vector_read \forward, 2, q2, [_vcpu, x9]
	.endif
	\operation v2.\destination, v0.\source
	str q2, [_vcpu, x9]
	next_gadget
	vector_variants \forward, Lscalar_offset16, Lvector_old\@, Lscalar_none, simd_unary_vector, \name, \operation, \destination, \source, \accumulate, \fpstate
.endm

.macro simd_unary_scalar name, operation, destination, source, accumulate=0, fpstate=0, forward=0
.set Lvector_old\@, Lscalar_none
.if \accumulate
.set Lvector_old\@, Lscalar_offset0
.endif
vector_entry simd_unary_\name, \forward, Lscalar_offset0, Lscalar_offset16, Lvector_old\@, Lscalar_none, 1
	.if \fpstate
	ensure_fp_environment
	.endif
	ldr x8, [_stream], #8
	uxth w9, w8
	lsr w10, w8, #16
	vector_read \forward, 1, q0, [_vcpu, x10]
	.if \accumulate
	vector_read \forward, 2, q2, [_vcpu, x9]
	.endif
	\operation \destination\()2, \source\()0
	str q2, [_vcpu, x9]
	next_gadget
	vector_variants \forward, Lscalar_offset16, Lvector_old\@, Lscalar_none, simd_unary_scalar, \name, \operation, \destination, \source, \accumulate, \fpstate
.endm

.macro simd_unary_v6 name, operation, fpstate=0
	simd_unary_vector \name\()_8b,  \operation, 8b,  8b,  0, \fpstate
	simd_unary_vector \name\()_4h,  \operation, 4h,  4h,  0, \fpstate
	simd_unary_vector \name\()_2s,  \operation, 2s,  2s,  0, \fpstate
	simd_unary_vector \name\()_16b, \operation, 16b, 16b, 0, \fpstate
	simd_unary_vector \name\()_8h,  \operation, 8h,  8h,  0, \fpstate
	simd_unary_vector \name\()_4s,  \operation, 4s,  4s,  0, \fpstate
.endm

.macro simd_unary_v7 name, operation, fpstate=0
	simd_unary_v6 \name, \operation, \fpstate
	simd_unary_vector \name\()_2d, \operation, 2d, 2d, 0, \fpstate
.endm

.macro simd_unary_s4 name, operation, fpstate=0
	simd_unary_scalar \name\()_b, \operation, b, b, 0, \fpstate
	simd_unary_scalar \name\()_h, \operation, h, h, 0, \fpstate
	simd_unary_scalar \name\()_s, \operation, s, s, 0, \fpstate
	simd_unary_scalar \name\()_d, \operation, d, d, 0, \fpstate
.endm

.macro simd_unary_acc_v7 name, operation, fpstate=0
	simd_unary_vector \name\()_8b,  \operation, 8b,  8b,  1, \fpstate
	simd_unary_vector \name\()_4h,  \operation, 4h,  4h,  1, \fpstate
	simd_unary_vector \name\()_2s,  \operation, 2s,  2s,  1, \fpstate
	simd_unary_vector \name\()_16b, \operation, 16b, 16b, 1, \fpstate
	simd_unary_vector \name\()_8h,  \operation, 8h,  8h,  1, \fpstate
	simd_unary_vector \name\()_4s,  \operation, 4s,  4s,  1, \fpstate
	simd_unary_vector \name\()_2d,  \operation, 2d,  2d,  1, \fpstate
.endm

.macro simd_unary_acc_s4 name, operation, fpstate=0
	simd_unary_scalar \name\()_b, \operation, b, b, 1, \fpstate
	simd_unary_scalar \name\()_h, \operation, h, h, 1, \fpstate
	simd_unary_scalar \name\()_s, \operation, s, s, 1, \fpstate
	simd_unary_scalar \name\()_d, \operation, d, d, 1, \fpstate
.endm

.macro simd_pairwise_long3 name, operation, accumulate=0
	simd_unary_vector \name\()_8b,  \operation, 4h, 8b,  \accumulate
	simd_unary_vector \name\()_4h,  \operation, 2s, 4h,  \accumulate
	simd_unary_vector \name\()_2s,  \operation, 1d, 2s,  \accumulate
	simd_unary_vector \name\()_16b, \operation, 8h, 16b, \accumulate
	simd_unary_vector \name\()_8h,  \operation, 4s, 8h,  \accumulate
	simd_unary_vector \name\()_4s,  \operation, 2d, 4s,  \accumulate
.endm

simd_pairwise_long3 saddlp, saddlp
simd_pairwise_long3 uaddlp, uaddlp
simd_pairwise_long3 sadalp, sadalp, 1
simd_pairwise_long3 uadalp, uadalp, 1

/* Mixed-sign saturating accumulate and signed saturating unary operations. */
simd_unary_acc_v7 suqadd, suqadd, 1
simd_unary_acc_s4 suqadd, suqadd, 1
simd_unary_acc_v7 usqadd, usqadd, 1
simd_unary_acc_s4 usqadd, usqadd, 1
simd_unary_v7 sqabs, sqabs, 1
simd_unary_s4 sqabs, sqabs, 1
simd_unary_v7 sqneg, sqneg, 1
simd_unary_s4 sqneg, sqneg, 1

simd_unary_v6 cls, cls
simd_unary_v6 clz, clz
simd_unary_v7 abs, abs
simd_unary_scalar abs_d, abs, d, d
simd_unary_v7 neg, neg
simd_unary_scalar neg_d, neg, d, d

/* Byte unary operations. */
simd_unary_vector cnt_8b, cnt, 8b, 8b
simd_unary_vector cnt_16b, cnt, 16b, 16b
simd_unary_vector not_8b, not, 8b, 8b
simd_unary_vector not_16b, not, 16b, 16b
simd_unary_vector rbit_8b, rbit, 8b, 8b
simd_unary_vector rbit_16b, rbit, 16b, 16b

/* REV64/REV32/REV16 have progressively narrower allocated element sets. */
simd_unary_vector rev64_8b, rev64, 8b, 8b
simd_unary_vector rev64_4h, rev64, 4h, 4h
simd_unary_vector rev64_2s, rev64, 2s, 2s
simd_unary_vector rev64_16b, rev64, 16b, 16b
simd_unary_vector rev64_8h, rev64, 8h, 8h
simd_unary_vector rev64_4s, rev64, 4s, 4s
simd_unary_vector rev32_8b, rev32, 8b, 8b
simd_unary_vector rev32_4h, rev32, 4h, 4h
simd_unary_vector rev32_16b, rev32, 16b, 16b
simd_unary_vector rev32_8h, rev32, 8h, 8h
simd_unary_vector rev16_8b, rev16, 8b, 8b
simd_unary_vector rev16_16b, rev16, 16b, 16b

/* Integer compare against zero. */
.macro simd_compare_zero_vector name, operation, arrangement, forward=0
vector_entry simd_unary_\name, \forward, Lscalar_offset0, Lscalar_offset16, Lscalar_none, Lscalar_none, 1
	ldr x8, [_stream], #8
	uxth w9, w8
	lsr w10, w8, #16
	vector_read \forward, 1, q0, [_vcpu, x10]
	\operation v2.\arrangement, v0.\arrangement, #0
	str q2, [_vcpu, x9]
	next_gadget
	vector_variants \forward, Lscalar_offset16, Lscalar_none, Lscalar_none, simd_compare_zero_vector, \name, \operation, \arrangement
.endm

.macro simd_compare_zero_scalar name, operation, forward=0
vector_entry simd_unary_\name, \forward, Lscalar_offset0, Lscalar_offset16, Lscalar_none, Lscalar_none, 1
	ldr x8, [_stream], #8
	uxth w9, w8
	lsr w10, w8, #16
	vector_read \forward, 1, q0, [_vcpu, x10]
	\operation d2, d0, #0
	str q2, [_vcpu, x9]
	next_gadget
	vector_variants \forward, Lscalar_offset16, Lscalar_none, Lscalar_none, simd_compare_zero_scalar, \name, \operation
.endm

.macro simd_compare_zero_v7 name, operation
	simd_compare_zero_vector \name\()_8b,  \operation, 8b
	simd_compare_zero_vector \name\()_4h,  \operation, 4h
	simd_compare_zero_vector \name\()_2s,  \operation, 2s
	simd_compare_zero_vector \name\()_16b, \operation, 16b
	simd_compare_zero_vector \name\()_8h,  \operation, 8h
	simd_compare_zero_vector \name\()_4s,  \operation, 4s
	simd_compare_zero_vector \name\()_2d,  \operation, 2d
	simd_compare_zero_scalar \name\()_d, \operation
.endm

simd_compare_zero_v7 cmgt0, cmgt
simd_compare_zero_v7 cmge0, cmge
simd_compare_zero_v7 cmeq0, cmeq
simd_compare_zero_v7 cmle0, cmle
simd_compare_zero_v7 cmlt0, cmlt

/* Extract/narrow families. Upper-half forms read the old destination. */
.macro simd_narrow3 name, operation, operation2, fpstate=0
	simd_unary_vector \name\()_8h,  \operation,  8b, 8h, 0, \fpstate
	simd_unary_vector \name\()_4s,  \operation,  4h, 4s, 0, \fpstate
	simd_unary_vector \name\()_2d,  \operation,  2s, 2d, 0, \fpstate
	simd_unary_vector \name\()_8h2, \operation2, 16b, 8h, 1, \fpstate
	simd_unary_vector \name\()_4s2, \operation2, 8h,  4s, 1, \fpstate
	simd_unary_vector \name\()_2d2, \operation2, 4s,  2d, 1, \fpstate
.endm

simd_narrow3 xtn, xtn, xtn2
simd_narrow3 sqxtn, sqxtn, sqxtn2, 1
simd_narrow3 uqxtn, uqxtn, uqxtn2, 1
simd_narrow3 sqxtun, sqxtun, sqxtun2, 1
simd_unary_scalar sqxtn_b, sqxtn, b, h, 0, 1
simd_unary_scalar sqxtn_h, sqxtn, h, s, 0, 1
simd_unary_scalar sqxtn_s, sqxtn, s, d, 0, 1
simd_unary_scalar uqxtn_b, uqxtn, b, h, 0, 1
simd_unary_scalar uqxtn_h, uqxtn, h, s, 0, 1
simd_unary_scalar uqxtn_s, uqxtn, s, d, 0, 1
simd_unary_scalar sqxtun_b, sqxtun, b, h, 0, 1
simd_unary_scalar sqxtun_h, sqxtun, h, s, 0, 1
simd_unary_scalar sqxtun_s, sqxtun, s, d, 0, 1

/* SHLL shifts by the source element width. */
.macro simd_shll name, operation, destination, source, shift, forward=0
vector_entry simd_unary_\name, \forward, Lscalar_offset0, Lscalar_offset16, Lscalar_none, Lscalar_none, 1
	ldr x8, [_stream], #8
	uxth w9, w8
	lsr w10, w8, #16
	vector_read \forward, 1, q0, [_vcpu, x10]
	\operation v2.\destination, v0.\source, #\shift
	str q2, [_vcpu, x9]
	next_gadget
	vector_variants \forward, Lscalar_offset16, Lscalar_none, Lscalar_none, simd_shll, \name, \operation, \destination, \source, \shift
.endm

simd_shll shll_8b, shll, 8h, 8b, 8
simd_shll shll_4h, shll, 4s, 4h, 16
simd_shll shll_2s, shll, 2d, 2s, 32
simd_shll shll_16b, shll2, 8h, 16b, 8
simd_shll shll_8h, shll2, 4s, 8h, 16
simd_shll shll_4s, shll2, 2d, 4s, 32

/* Translation-specialized across-lane integer reductions. */
.macro simd_reduce name, operation, destination, source, forward=0
vector_entry simd_reduce_\name, \forward, Lvector_register0, Lvector_register8, Lscalar_none, Lscalar_none, 1
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	add x11, _vcpu, #UML_GADGET_CPU_V0
	add x12, x11, x10, lsl #4
	add x13, x11, x9, lsl #4
	vector_read \forward, 1, q0, [x12]
	\operation \destination\()2, v0.\source
	str q2, [x13]
	next_gadget
	vector_variants \forward, Lvector_register8, Lscalar_none, Lscalar_none, simd_reduce, \name, \operation, \destination, \source
.endm

.macro simd_reduce_long5 name, operation
	simd_reduce \name\()_8b,  \operation, h, 8b
	simd_reduce \name\()_4h,  \operation, s, 4h
	simd_reduce \name\()_16b, \operation, h, 16b
	simd_reduce \name\()_8h,  \operation, s, 8h
	simd_reduce \name\()_4s,  \operation, d, 4s
.endm

.macro simd_reduce_same5 name, operation
	simd_reduce \name\()_8b,  \operation, b, 8b
	simd_reduce \name\()_4h,  \operation, h, 4h
	simd_reduce \name\()_16b, \operation, b, 16b
	simd_reduce \name\()_8h,  \operation, h, 8h
	simd_reduce \name\()_4s,  \operation, s, 4s
.endm

simd_reduce_long5 saddlv, saddlv
simd_reduce_long5 uaddlv, uaddlv
simd_reduce_same5 smaxv, smaxv
simd_reduce_same5 umaxv, umaxv
simd_reduce_same5 sminv, sminv
simd_reduce_same5 uminv, uminv
simd_reduce_same5 addv, addv

simd_unary_vector urecpe_2s, urecpe, 2s, 2s
simd_unary_vector urecpe_4s, urecpe, 4s, 4s
simd_unary_vector ursqrte_2s, ursqrte, 2s, 2s
simd_unary_vector ursqrte_4s, ursqrte, 4s, 4s

.purgem simd_reduce_same5
.purgem simd_reduce_long5
.purgem simd_reduce

.purgem simd_shll
.purgem simd_narrow3
.purgem simd_compare_zero_v7
.purgem simd_compare_zero_scalar
.purgem simd_compare_zero_vector
.purgem simd_pairwise_long3
.purgem simd_unary_acc_s4
.purgem simd_unary_acc_v7
.purgem simd_unary_s4
.purgem simd_unary_v7
.purgem simd_unary_v6
.purgem simd_unary_scalar
.purgem simd_unary_vector

/* Translation-specialized Advanced SIMD bitwise operations. Register fields
 * become packed CpuState byte offsets in the stream, so the runtime only
 * unpacks offsets rather than decoding guest registers or forming addresses. */
.macro simd_bitwise_shape name, operation, arrangement, wide, accumulate=0, forward=0
.set Lvector_old\@, Lscalar_none
.if \accumulate
.set Lvector_old\@, Lscalar_offset0
.endif
vector_entry \name\()_\arrangement, \forward, Lscalar_offset0, Lscalar_offset16, Lscalar_offset32, Lvector_old\@, 1
	ldr x8, [_stream], #8
	unpack_u16_3 x8, x9, x10, x11
	.if \wide
	vector_read \forward, 1, q0, [_vcpu, x10]
	vector_read \forward, 2, q1, [_vcpu, x11]
	.if \accumulate
	vector_read \forward, 4, q2, [_vcpu, x9]
	.endif
	.else
	vector_read \forward, 1, d0, [_vcpu, x10]
	vector_read \forward, 2, d1, [_vcpu, x11]
	.if \accumulate
	vector_read \forward, 4, d2, [_vcpu, x9]
	.endif
	.endif
	\operation v2.\arrangement, v0.\arrangement, v1.\arrangement
	str q2, [_vcpu, x9]
	next_gadget
	vector_variants \forward, Lscalar_offset16, Lscalar_offset32, Lvector_old\@, simd_bitwise_shape, \name, \operation, \arrangement, \wide, \accumulate
.endm

.macro simd_bitwise name, operation, accumulate=0
	simd_bitwise_shape \name, \operation, 8b, 0, \accumulate
	simd_bitwise_shape \name, \operation, 16b, 1, \accumulate
.endm

simd_bitwise simd_and, and
simd_bitwise simd_bic, bic
simd_bitwise simd_orr, orr
simd_bitwise simd_orn, orn
simd_bitwise simd_eor, eor
simd_bitwise simd_bsl, bsl, 1
simd_bitwise simd_bit, bit, 1
simd_bitwise simd_bif, bif, 1

/* ORR with identical sources is MOV. Translation removes the redundant
 * source load and native operation; the 64-bit load also clears the upper
 * half required by the 8-byte arrangement. */
.macro simd_move_shape name, register, forward=0
vector_entry \name, \forward, Lscalar_offset0, Lscalar_offset16, Lscalar_none, Lscalar_none, 1
	ldr x8, [_stream], #8
	unpack_u16_2 x8, x9, x10
	vector_read \forward, 1, \register\()2, [_vcpu, x10]
	str q2, [_vcpu, x9]
	next_gadget
	vector_variants \forward, Lscalar_offset16, Lscalar_none, Lscalar_none, simd_move_shape, \name, \register
.endm

simd_move_shape simd_mov_8b, d
simd_move_shape simd_mov_16b, q

.purgem simd_move_shape
.purgem simd_bitwise
.purgem simd_bitwise_shape

/* Translation resolves both vector-register fields to CpuState offsets and
 * selects the operation and host path. Valid native blocks therefore contain
 * no opcode decode, register-index arithmetic, or conditional branch. */
.macro simd_aes_native_binary name, operation
gadget \name\()_native
	ldr x8, [_stream], #8
	unpack_u16_2 x8, x9, x10
	ldr q0, [_vcpu, x9]
	ldr q1, [_vcpu, x10]
	\operation v0.16b, v1.16b
	str q0, [_vcpu, x9]
	next_gadget
.endm

.macro simd_aes_native_unary name, operation
gadget \name\()_native
	ldr x8, [_stream], #8
	unpack_u16_2 x8, x9, x10
	ldr q1, [_vcpu, x10]
	\operation v0.16b, v1.16b
	str q0, [_vcpu, x9]
	next_gadget
.endm

/* A two-register transform in software: the helper takes CpuState and both
 * vector offsets. The SHA family below shares it. */
.macro simd_crypto_two_software name
gadget \name\()_soft
	ldr x10, [_stream], #8
	unpack_u16_2 x10, x8, x9
	stp x1, x2, [sp, #-16]!
	mov x0, x1
	mov x1, x8
	mov x2, x9
	helper_call uml_arm64_\name\()_software
	ldp x1, x2, [sp], #16
	next_gadget
.endm

simd_aes_native_binary simd_aese, aese
simd_aes_native_binary simd_aesd, aesd
simd_aes_native_unary simd_aesmc, aesmc
simd_aes_native_unary simd_aesimc, aesimc
simd_crypto_two_software simd_aese
simd_crypto_two_software simd_aesd
simd_crypto_two_software simd_aesmc
simd_crypto_two_software simd_aesimc

.purgem simd_aes_native_unary
.purgem simd_aes_native_binary

/* FEAT_SHA1 and FEAT_SHA256 map one-for-one to host crypto instructions.
 * Operation selection is performed by the builder, leaving no opcode branch
 * in a valid translated block. */
.macro simd_sha_three name, operation, destination, first, second
gadget \name
	ldr x11, [_stream], #8
	unpack_u16_3 x11, x8, x9, x10
	ldr q0, [_vcpu, x8]
	ldr q1, [_vcpu, x9]
	ldr q2, [_vcpu, x10]
	\operation \destination, \first, \second
	str q0, [_vcpu, x8]
	next_gadget
.endm

simd_sha_three simd_sha1c, sha1c, q0, s1, v2.4s
simd_sha_three simd_sha1p, sha1p, q0, s1, v2.4s
simd_sha_three simd_sha1m, sha1m, q0, s1, v2.4s
simd_sha_three simd_sha1su0, sha1su0, v0.4s, v1.4s, v2.4s
simd_sha_three simd_sha256h, sha256h, q0, q1, v2.4s
simd_sha_three simd_sha256h2, sha256h2, q0, q1, v2.4s
simd_sha_three simd_sha256su1, sha256su1, v0.4s, v1.4s, v2.4s
simd_sha_three simd_sha512h, sha512h, q0, q1, v2.2d
simd_sha_three simd_sha512h2, sha512h2, q0, q1, v2.2d
simd_sha_three simd_sha512su1, sha512su1, v0.2d, v1.2d, v2.2d

.purgem simd_sha_three

.macro simd_sha_two name, operation, destination, source
gadget \name
	ldr x10, [_stream], #8
	unpack_u16_2 x10, x8, x9
	ldr q0, [_vcpu, x8]
	ldr q1, [_vcpu, x9]
	\operation \destination, \source
	str q0, [_vcpu, x8]
	next_gadget
.endm

simd_sha_two simd_sha1h, sha1h, s0, s1
simd_sha_two simd_sha1su1, sha1su1, v0.4s, v1.4s
simd_sha_two simd_sha256su0, sha256su0, v0.4s, v1.4s
simd_sha_two simd_sha512su0, sha512su0, v0.2d, v1.2d

.purgem simd_sha_two

/* Hosts that do not implement an assigned fixed transform use a
 * translation-selected semantic leaf. Each entry still has a fixed operation
 * and direct register offsets, so neither opcode nor register fields are
 * decoded by the threaded runtime. */
.macro simd_sha_three_software name
gadget \name\()_soft
	ldr x11, [_stream], #8
	unpack_u16_3 x11, x8, x9, x10
	stp x1, x2, [sp, #-16]!
	mov x0, x1
	mov x1, x8
	mov x2, x9
	mov x3, x10
	helper_call uml_arm64_\name\()_software
	ldp x1, x2, [sp], #16
	next_gadget
.endm

simd_sha_three_software simd_sha1c
simd_sha_three_software simd_sha1p
simd_sha_three_software simd_sha1m
simd_sha_three_software simd_sha1su0
simd_sha_three_software simd_sha256h
simd_sha_three_software simd_sha256h2
simd_sha_three_software simd_sha256su1
simd_sha_three_software simd_sha512h
simd_sha_three_software simd_sha512h2
simd_sha_three_software simd_sha512su1

.purgem simd_sha_three_software

simd_crypto_two_software simd_sha1h
simd_crypto_two_software simd_sha1su1
simd_crypto_two_software simd_sha256su0
simd_crypto_two_software simd_sha512su0

.purgem simd_crypto_two_software

.macro simd_four_source_logic name, operation, native, kind
gadget \name
	ldr x12, [_stream], #8
	unpack_u16_4 x12, x8, x9, x10, x11
	ldr q1, [_vcpu, x9]
	ldr q2, [_vcpu, x10]
	ldr q3, [_vcpu, x11]
.if \native
	\operation v0.16b, v1.16b, v2.16b, v3.16b
.elseif \kind == 0
	eor v0.16b, v1.16b, v2.16b
	eor v0.16b, v0.16b, v3.16b
.else
	bic v0.16b, v2.16b, v3.16b
	eor v0.16b, v0.16b, v1.16b
.endif
	str q0, [_vcpu, x8]
	next_gadget
.endm

simd_four_source_logic simd_eor3_native, eor3, 1, 0
simd_four_source_logic simd_eor3_soft, eor3, 0, 0
simd_four_source_logic simd_bcax_native, bcax, 1, 1
simd_four_source_logic simd_bcax_soft, bcax, 0, 1
.purgem simd_four_source_logic

.macro simd_rax1 name, native
gadget \name
	ldr x11, [_stream], #8
	unpack_u16_3 x11, x8, x9, x10
	ldr q1, [_vcpu, x9]
	ldr q2, [_vcpu, x10]
.if \native
	rax1 v0.2d, v1.2d, v2.2d
.else
	shl v0.2d, v2.2d, #1
	ushr v3.2d, v2.2d, #63
	orr v0.16b, v0.16b, v3.16b
	eor v0.16b, v0.16b, v1.16b
.endif
	str q0, [_vcpu, x8]
	next_gadget
.endm

simd_rax1 simd_rax1_native, 1
simd_rax1 simd_rax1_soft, 0
.purgem simd_rax1

.macro simd_xar name, rotate, native
gadget \name
	ldr x11, [_stream], #8
	unpack_u16_3 x11, x8, x9, x10
	ldr q1, [_vcpu, x9]
	ldr q2, [_vcpu, x10]
.if \native
	xar v0.2d, v1.2d, v2.2d, #\rotate
.else
	eor v0.16b, v1.16b, v2.16b
.if \rotate != 0
	ushr v3.2d, v0.2d, #\rotate
	shl v0.2d, v0.2d, #(64-\rotate)
	orr v0.16b, v0.16b, v3.16b
.endif
.endif
	str q0, [_vcpu, x8]
	next_gadget
.endm

.macro simd_xar_pair rotate
	simd_xar simd_xar_native_\rotate, \rotate, 1
	simd_xar simd_xar_soft_\rotate, \rotate, 0
.endm

simd_xar_pair 0
simd_xar_pair 1
simd_xar_pair 2
simd_xar_pair 3
simd_xar_pair 4
simd_xar_pair 5
simd_xar_pair 6
simd_xar_pair 7
simd_xar_pair 8
simd_xar_pair 9
simd_xar_pair 10
simd_xar_pair 11
simd_xar_pair 12
simd_xar_pair 13
simd_xar_pair 14
simd_xar_pair 15
simd_xar_pair 16
simd_xar_pair 17
simd_xar_pair 18
simd_xar_pair 19
simd_xar_pair 20
simd_xar_pair 21
simd_xar_pair 22
simd_xar_pair 23
simd_xar_pair 24
simd_xar_pair 25
simd_xar_pair 26
simd_xar_pair 27
simd_xar_pair 28
simd_xar_pair 29
simd_xar_pair 30
simd_xar_pair 31
simd_xar_pair 32
simd_xar_pair 33
simd_xar_pair 34
simd_xar_pair 35
simd_xar_pair 36
simd_xar_pair 37
simd_xar_pair 38
simd_xar_pair 39
simd_xar_pair 40
simd_xar_pair 41
simd_xar_pair 42
simd_xar_pair 43
simd_xar_pair 44
simd_xar_pair 45
simd_xar_pair 46
simd_xar_pair 47
simd_xar_pair 48
simd_xar_pair 49
simd_xar_pair 50
simd_xar_pair 51
simd_xar_pair 52
simd_xar_pair 53
simd_xar_pair 54
simd_xar_pair 55
simd_xar_pair 56
simd_xar_pair 57
simd_xar_pair 58
simd_xar_pair 59
simd_xar_pair 60
simd_xar_pair 61
simd_xar_pair 62
simd_xar_pair 63

.purgem simd_xar_pair
.purgem simd_xar

.macro simd_pmull_direct name, operation, destination_arrangement, source_arrangement
gadget \name
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	ldr q1, [_vcpu, x10]
	ldr q2, [_vcpu, x11]
	\operation v0.\destination_arrangement, v1.\source_arrangement, \
		v2.\source_arrangement
	str q0, [_vcpu, x9]
	next_gadget
.endm

simd_pmull_direct simd_pmull_8b_direct, pmull, 8h, 8b
simd_pmull_direct simd_pmull_8b2_direct, pmull2, 8h, 16b
simd_pmull_direct simd_pmull_1d_native_direct, pmull, 1q, 1d
simd_pmull_direct simd_pmull_1d2_native_direct, pmull2, 1q, 2d

.purgem simd_pmull_direct

.macro simd_pmull_software_direct name, high
gadget \name
	ldr x8, [_stream], #8
	stp x1, x2, [sp, #-16]!
	mov x0, x1
	mov x1, x8
	mov w2, #\high
	helper_call uml_arm64_simd_pmull_software
	ldp x1, x2, [sp], #16
	next_gadget
.endm

simd_pmull_software_direct simd_pmull_1d_soft_direct, 0
simd_pmull_software_direct simd_pmull_1d2_soft_direct, 1

.purgem simd_pmull_software_direct

.macro simd_extract_q offset
gadget simd_extract_q\offset
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	ubfx w11, w8, #16, #5
	add x12, _vcpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	add x14, x12, x11, lsl #4
	ldr q1, [x13]
	ldr q2, [x14]
	ext v0.16b, v1.16b, v2.16b, #\offset
	add x13, x12, x9, lsl #4
	str q0, [x13]
	next_gadget
.endm

.macro simd_extract_d offset
gadget simd_extract_d\offset
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	ubfx w11, w8, #16, #5
	add x12, _vcpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	add x14, x12, x11, lsl #4
	ldr d1, [x13]
	ldr d2, [x14]
	ext v0.8b, v1.8b, v2.8b, #\offset
	add x13, x12, x9, lsl #4
	str d0, [x13]
	str xzr, [x13, #8]
	next_gadget
.endm

simd_extract_q 0
simd_extract_q 1
simd_extract_q 2
simd_extract_q 3
simd_extract_q 4
simd_extract_q 5
simd_extract_q 6
simd_extract_q 7
simd_extract_q 8
simd_extract_q 9
simd_extract_q 10
simd_extract_q 11
simd_extract_q 12
simd_extract_q 13
simd_extract_q 14
simd_extract_q 15
simd_extract_d 0
simd_extract_d 1
simd_extract_d 2
simd_extract_d 3
simd_extract_d 4
simd_extract_d 5
simd_extract_d 6
simd_extract_d 7

/* Translation-specialized native two-source permutes. */
.macro simd_permute_shape name, operation, arrangement, wide
gadget \name
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	lsr x11, x8, #32
	ldr q1, [_vcpu, x10]
	ldr q2, [_vcpu, x11]
	\operation v0.\arrangement, v1.\arrangement, v2.\arrangement
	.if \wide
	str q0, [_vcpu, x9]
	.else
	add x15, _vcpu, x9
	str d0, [x15]
	str xzr, [x15, #8]
	.endif
	next_gadget
.endm

.macro simd_permute name, operation
	simd_permute_shape simd_permute_\name\()_8b, \operation, 8b, 0
	simd_permute_shape simd_permute_\name\()_4h, \operation, 4h, 0
	simd_permute_shape simd_permute_\name\()_2s, \operation, 2s, 0
	simd_permute_shape simd_permute_\name\()_16b, \operation, 16b, 1
	simd_permute_shape simd_permute_\name\()_8h, \operation, 8h, 1
	simd_permute_shape simd_permute_\name\()_4s, \operation, 4s, 1
	simd_permute_shape simd_permute_\name\()_2d, \operation, 2d, 1
.endm

simd_permute uzp1, uzp1
simd_permute trn1, trn1
simd_permute zip1, zip1
simd_permute uzp2, uzp2
simd_permute trn2, trn2
simd_permute zip2, zip2

.purgem simd_permute
.purgem simd_permute_shape

/* Immediate shifts use a translation-selected operation and shape.  The
 * shift count remains stream data so all immediates share branch-free host
 * vector code.  Insert forms consume one additional replicated mask word. */
.macro simd_shift_right_shape name, arrangement, bytes, wide, operation, accumulate, insert
gadget \name
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	ubfx w11, w8, #16, #7
	add x12, _vcpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	ldr q1, [x13]
	.if \bytes == 8
	neg x11, x11
	dup v2.\arrangement, x11
	.else
	neg w11, w11
	dup v2.\arrangement, w11
	.endif
	\operation v0.\arrangement, v1.\arrangement, v2.\arrangement
	.if \accumulate || \insert
	add x13, x12, x9, lsl #4
	ldr q3, [x13]
	.endif
	.if \accumulate
	add v0.\arrangement, v0.\arrangement, v3.\arrangement
	.endif
	.if \insert
	ldr x14, [_stream], #8
	dup v4.2d, x14
	and v3.16b, v3.16b, v4.16b
	orr v0.16b, v0.16b, v3.16b
	.endif
	add x13, x12, x9, lsl #4
	.if \wide
	str q0, [x13]
	.else
	str d0, [x13]
	str xzr, [x13, #8]
	.endif
	next_gadget
.endm

.macro simd_shift_right_action name, operation, accumulate, insert
	simd_shift_right_shape simd_shift_right_\name\()_8b, 16b, 1, 0, \operation, \accumulate, \insert
	simd_shift_right_shape simd_shift_right_\name\()_4h, 8h, 2, 0, \operation, \accumulate, \insert
	simd_shift_right_shape simd_shift_right_\name\()_2s, 4s, 4, 0, \operation, \accumulate, \insert
	simd_shift_right_shape simd_shift_right_\name\()_16b, 16b, 1, 1, \operation, \accumulate, \insert
	simd_shift_right_shape simd_shift_right_\name\()_8h, 8h, 2, 1, \operation, \accumulate, \insert
	simd_shift_right_shape simd_shift_right_\name\()_4s, 4s, 4, 1, \operation, \accumulate, \insert
	simd_shift_right_shape simd_shift_right_\name\()_2d, 2d, 8, 1, \operation, \accumulate, \insert
	simd_shift_right_shape simd_shift_right_\name\()_d, 2d, 8, 0, \operation, \accumulate, \insert
.endm

simd_shift_right_action sshr, sshl, 0, 0
simd_shift_right_action ushr, ushl, 0, 0
simd_shift_right_action ssra, sshl, 1, 0
simd_shift_right_action usra, ushl, 1, 0
simd_shift_right_action srshr, srshl, 0, 0
simd_shift_right_action urshr, urshl, 0, 0
simd_shift_right_action srsra, srshl, 1, 0
simd_shift_right_action ursra, urshl, 1, 0
simd_shift_right_action sri, ushl, 0, 1

.purgem simd_shift_right_action
.purgem simd_shift_right_shape

.macro simd_shift_left_shape name, arrangement, bytes, wide, insert
gadget \name
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	ubfx w11, w8, #16, #7
	add x12, _vcpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	ldr q1, [x13]
	.if \bytes == 8
	dup v2.\arrangement, x11
	.else
	dup v2.\arrangement, w11
	.endif
	sshl v0.\arrangement, v1.\arrangement, v2.\arrangement
	.if \insert
	add x13, x12, x9, lsl #4
	ldr q3, [x13]
	ldr x14, [_stream], #8
	dup v4.2d, x14
	and v3.16b, v3.16b, v4.16b
	orr v0.16b, v0.16b, v3.16b
	.endif
	add x13, x12, x9, lsl #4
	.if \wide
	str q0, [x13]
	.else
	str d0, [x13]
	str xzr, [x13, #8]
	.endif
	next_gadget
.endm

.macro simd_shift_left_action name, insert
	simd_shift_left_shape simd_shift_left_\name\()_8b, 16b, 1, 0, \insert
	simd_shift_left_shape simd_shift_left_\name\()_4h, 8h, 2, 0, \insert
	simd_shift_left_shape simd_shift_left_\name\()_2s, 4s, 4, 0, \insert
	simd_shift_left_shape simd_shift_left_\name\()_16b, 16b, 1, 1, \insert
	simd_shift_left_shape simd_shift_left_\name\()_8h, 8h, 2, 1, \insert
	simd_shift_left_shape simd_shift_left_\name\()_4s, 4s, 4, 1, \insert
	simd_shift_left_shape simd_shift_left_\name\()_2d, 2d, 8, 1, \insert
	simd_shift_left_shape simd_shift_left_\name\()_d, 2d, 8, 0, \insert
.endm

simd_shift_left_action shl, 0
simd_shift_left_action sli, 1

.purgem simd_shift_left_action
.purgem simd_shift_left_shape

.macro simd_saturating_shift_left_shape name, arrangement, bytes, wide, scalar, operation
gadget \name
	ensure_fp_environment
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	ubfx w11, w8, #16, #7
	add x12, _vcpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	.if \scalar
	.if \bytes == 1
	ldr b1, [x13]
	.elseif \bytes == 2
	ldr h1, [x13]
	.elseif \bytes == 4
	ldr s1, [x13]
	.else
	ldr d1, [x13]
	.endif
	.else
	.if \wide
	ldr q1, [x13]
	.else
	movi v1.16b, #0
	ldr d1, [x13]
	.endif
	.endif
	.if \bytes == 8
	dup v2.\arrangement, x11
	.else
	dup v2.\arrangement, w11
	.endif
	\operation v0.\arrangement, v1.\arrangement, v2.\arrangement
	add x13, x12, x9, lsl #4
	.if \wide
	str q0, [x13]
	.else
	str d0, [x13]
	str xzr, [x13, #8]
	.endif
	next_gadget
.endm

.macro simd_saturating_shift_left name, operation
	simd_saturating_shift_left_shape simd_shift_left_\name\()_8b, 16b, 1, 0, 0, \operation
	simd_saturating_shift_left_shape simd_shift_left_\name\()_4h, 8h, 2, 0, 0, \operation
	simd_saturating_shift_left_shape simd_shift_left_\name\()_2s, 4s, 4, 0, 0, \operation
	simd_saturating_shift_left_shape simd_shift_left_\name\()_16b, 16b, 1, 1, 0, \operation
	simd_saturating_shift_left_shape simd_shift_left_\name\()_8h, 8h, 2, 1, 0, \operation
	simd_saturating_shift_left_shape simd_shift_left_\name\()_4s, 4s, 4, 1, 0, \operation
	simd_saturating_shift_left_shape simd_shift_left_\name\()_2d, 2d, 8, 1, 0, \operation
	simd_saturating_shift_left_shape simd_shift_left_\name\()_b, 16b, 1, 0, 1, \operation
	simd_saturating_shift_left_shape simd_shift_left_\name\()_h, 8h, 2, 0, 1, \operation
	simd_saturating_shift_left_shape simd_shift_left_\name\()_s, 4s, 4, 0, 1, \operation
	simd_saturating_shift_left_shape simd_shift_left_\name\()_d, 2d, 8, 0, 1, \operation
.endm

simd_saturating_shift_left sqshl, sqshl
simd_saturating_shift_left uqshl, uqshl

.purgem simd_saturating_shift_left
.purgem simd_saturating_shift_left_shape

/* SQSHLU only has an immediate host form.  Emit one exact gadget per shift so
 * the translated valid path uses the native operation without a dispatch
 * branch or a software saturation sequence. */
.macro simd_sqshlu_shape name, destination, source, load, wide, shift
gadget simd_shift_left_sqshlu_\name\()_\shift
	ensure_fp_environment
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	add x12, _vcpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	ldr \load, [x13]
	sqshlu \destination, \source, #\shift
	add x13, x12, x9, lsl #4
	.if \wide
	str q0, [x13]
	.else
	str d0, [x13]
	str xzr, [x13, #8]
	.endif
	next_gadget
.endm

.macro simd_sqshlu_8 name, destination, source, load, wide
	.irp shift, 0, 1, 2, 3, 4, 5, 6, 7
		simd_sqshlu_shape \name, \destination, \source, \load, \wide, \shift
	.endr
.endm

.macro simd_sqshlu_16 name, destination, source, load, wide
	.irp shift, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
		simd_sqshlu_shape \name, \destination, \source, \load, \wide, \shift
	.endr
.endm

.macro simd_sqshlu_32 name, destination, source, load, wide
	.irp shift, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
		simd_sqshlu_shape \name, \destination, \source, \load, \wide, \shift
	.endr
.endm

.macro simd_sqshlu_64 name, destination, source, load, wide
	.irp shift, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63
		simd_sqshlu_shape \name, \destination, \source, \load, \wide, \shift
	.endr
.endm

simd_sqshlu_8 8b, v0.8b, v1.8b, d1, 0
simd_sqshlu_16 4h, v0.4h, v1.4h, d1, 0
simd_sqshlu_32 2s, v0.2s, v1.2s, d1, 0
simd_sqshlu_8 16b, v0.16b, v1.16b, q1, 1
simd_sqshlu_16 8h, v0.8h, v1.8h, q1, 1
simd_sqshlu_32 4s, v0.4s, v1.4s, q1, 1
simd_sqshlu_64 2d, v0.2d, v1.2d, q1, 1
simd_sqshlu_8 b, b0, b1, b1, 0
simd_sqshlu_16 h, h0, h1, h1, 0
simd_sqshlu_32 s, s0, s1, s1, 0
simd_sqshlu_64 d, d0, d1, d1, 0

.purgem simd_sqshlu_64
.purgem simd_sqshlu_32
.purgem simd_sqshlu_16
.purgem simd_sqshlu_8
.purgem simd_sqshlu_shape

/* SSHLL/USHLL use the host's lower/upper widening operation selected while
 * translating.  A variable positive shift also handles the zero-shift
 * SXTL/UXTL aliases without a runtime special case. */
.macro simd_shift_left_long_shape name, widening, wide_arrangement, narrow_arrangement, bytes
gadget \name
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	ubfx w11, w8, #16, #6
	add x12, _vcpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	ldr q1, [x13]
	\widening v0.\wide_arrangement, v1.\narrow_arrangement, #0
	.if \bytes == 8
	dup v2.\wide_arrangement, x11
	.else
	dup v2.\wide_arrangement, w11
	.endif
	sshl v0.\wide_arrangement, v0.\wide_arrangement, v2.\wide_arrangement
	add x13, x12, x9, lsl #4
	str q0, [x13]
	next_gadget
.endm

.macro simd_shift_left_long_operation name, widening, bytes, halves, words
	simd_shift_left_long_shape simd_\name\()_8h_8b, \widening, 8h, \bytes, 2
	simd_shift_left_long_shape simd_\name\()_4s_4h, \widening, 4s, \halves, 4
	simd_shift_left_long_shape simd_\name\()_2d_2s, \widening, 2d, \words, 8
.endm

simd_shift_left_long_operation sshll, sshll, 8b, 4h, 2s
simd_shift_left_long_operation sshll2, sshll2, 16b, 8h, 4s
simd_shift_left_long_operation ushll, ushll, 8b, 4h, 2s
simd_shift_left_long_operation ushll2, ushll2, 16b, 8h, 4s

.purgem simd_shift_left_long_operation
.purgem simd_shift_left_long_shape

/* Translation-specialized TBL/TBX uses the exact host table-list form. */
.macro simd_table_lookup_shape name, operation, count, extend, wide
gadget \name
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	.if \count > 1
	lsr x12, x8, #48
	.endif
	ldr q4, [_vcpu, x10]
	ldr q0, [_vcpu, x11]
	.if \count > 1
	ldr q1, [_vcpu, x12]
	.endif
	.if \count > 2
	ldr x13, [_stream], #8
	and x14, x13, #0xffff
	ldr q2, [_vcpu, x14]
	.endif
	.if \count > 3
	ubfx x15, x13, #16, #16
	ldr q3, [_vcpu, x15]
	.endif
	.if \extend
	ldr q5, [_vcpu, x9]
	.endif
	.if \count == 1
	\operation v5.16b, {v0.16b}, v4.16b
	.elseif \count == 2
	\operation v5.16b, {v0.16b-v1.16b}, v4.16b
	.elseif \count == 3
	\operation v5.16b, {v0.16b-v2.16b}, v4.16b
	.else
	\operation v5.16b, {v0.16b-v3.16b}, v4.16b
	.endif
	.if \wide
	str q5, [_vcpu, x9]
	.else
	add x15, _vcpu, x9
	str d5, [x15]
	str xzr, [x15, #8]
	.endif
	next_gadget
.endm

simd_table_lookup_shape simd_tbl_1_8b, tbl, 1, 0, 0
simd_table_lookup_shape simd_tbl_1_16b, tbl, 1, 0, 1
simd_table_lookup_shape simd_tbl_2_8b, tbl, 2, 0, 0
simd_table_lookup_shape simd_tbl_2_16b, tbl, 2, 0, 1
simd_table_lookup_shape simd_tbl_3_8b, tbl, 3, 0, 0
simd_table_lookup_shape simd_tbl_3_16b, tbl, 3, 0, 1
simd_table_lookup_shape simd_tbl_4_8b, tbl, 4, 0, 0
simd_table_lookup_shape simd_tbl_4_16b, tbl, 4, 0, 1
simd_table_lookup_shape simd_tbx_1_8b, tbx, 1, 1, 0
simd_table_lookup_shape simd_tbx_1_16b, tbx, 1, 1, 1
simd_table_lookup_shape simd_tbx_2_8b, tbx, 2, 1, 0
simd_table_lookup_shape simd_tbx_2_16b, tbx, 2, 1, 1
simd_table_lookup_shape simd_tbx_3_8b, tbx, 3, 1, 0
simd_table_lookup_shape simd_tbx_3_16b, tbx, 3, 1, 1
simd_table_lookup_shape simd_tbx_4_8b, tbx, 4, 1, 0
simd_table_lookup_shape simd_tbx_4_16b, tbx, 4, 1, 1

.purgem simd_table_lookup_shape

/* SIMD lane moves are fully decoded while translating.  Each valid gadget
 * has a fixed element size and destination shape, leaving only register and
 * byte offsets in the stream operand. */
.macro simd_duplicate_element_shape name, arrangement, bytes, wide, scalar
gadget \name
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	ubfx w11, w8, #16, #4
	add x12, _vcpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	add x13, x13, x11
	.if \bytes == 1
	ldrb w14, [x13]
	.elseif \bytes == 2
	ldrh w14, [x13]
	.elseif \bytes == 4
	ldr w14, [x13]
	.else
	ldr x14, [x13]
	.endif
	add x13, x12, x9, lsl #4
	.if \scalar
	movi v0.16b, #0
	.if \bytes == 1
	mov v0.b[0], w14
	.elseif \bytes == 2
	mov v0.h[0], w14
	.elseif \bytes == 4
	mov v0.s[0], w14
	.else
	mov v0.d[0], x14
	.endif
	str q0, [x13]
	.else
	.if \bytes == 8
	dup v0.\arrangement, x14
	.else
	dup v0.\arrangement, w14
	.endif
	.if \wide
	str q0, [x13]
	.else
	str d0, [x13]
	str xzr, [x13, #8]
	.endif
	.endif
	next_gadget
.endm

simd_duplicate_element_shape simd_duplicate_element_8b, 16b, 1, 0, 0
simd_duplicate_element_shape simd_duplicate_element_16b, 16b, 1, 1, 0
simd_duplicate_element_shape simd_duplicate_element_4h, 8h, 2, 0, 0
simd_duplicate_element_shape simd_duplicate_element_8h, 8h, 2, 1, 0
simd_duplicate_element_shape simd_duplicate_element_2s, 4s, 4, 0, 0
simd_duplicate_element_shape simd_duplicate_element_4s, 4s, 4, 1, 0
simd_duplicate_element_shape simd_duplicate_element_2d, 2d, 8, 1, 0
simd_duplicate_element_shape simd_duplicate_element_b, 16b, 1, 1, 1
simd_duplicate_element_shape simd_duplicate_element_h, 8h, 2, 1, 1
simd_duplicate_element_shape simd_duplicate_element_s, 4s, 4, 1, 1
simd_duplicate_element_shape simd_duplicate_element_d, 2d, 8, 1, 1

.purgem simd_duplicate_element_shape

.macro simd_duplicate_general_shape name, arrangement, wide, bytes
gadget \name
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	add x11, _vcpu, #UML_GADGET_CPU_X0
	add x11, x11, x10, lsl #3
	.if \bytes == 8
	ldr x12, [x11]
	.else
	ldr w12, [x11]
	.endif
	.if \bytes == 8
	dup v0.\arrangement, x12
	.else
	dup v0.\arrangement, w12
	.endif
	add x11, _vcpu, #UML_GADGET_CPU_V0
	add x11, x11, x9, lsl #4
	.if \wide
	str q0, [x11]
	.else
	str d0, [x11]
	str xzr, [x11, #8]
	.endif
	next_gadget
.endm

simd_duplicate_general_shape simd_duplicate_general_8b, 16b, 0, 1
simd_duplicate_general_shape simd_duplicate_general_16b, 16b, 1, 1
simd_duplicate_general_shape simd_duplicate_general_4h, 8h, 0, 2
simd_duplicate_general_shape simd_duplicate_general_8h, 8h, 1, 2
simd_duplicate_general_shape simd_duplicate_general_2s, 4s, 0, 4
simd_duplicate_general_shape simd_duplicate_general_4s, 4s, 1, 4
simd_duplicate_general_shape simd_duplicate_general_2d, 2d, 1, 8

.purgem simd_duplicate_general_shape

.macro simd_duplicate_general_zero name, wide
gadget \name
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	add x10, _vcpu, #UML_GADGET_CPU_V0
	add x10, x10, x9, lsl #4
	str xzr, [x10]
	str xzr, [x10, #8]
	next_gadget
.endm

simd_duplicate_general_zero simd_duplicate_general_zero_8b, 0
simd_duplicate_general_zero simd_duplicate_general_zero_16b, 1

.purgem simd_duplicate_general_zero

/* Unsigned shift-right narrows select rounding, half, and source width while
 * translating.  Only the register indices and positive shift remain data. */
.macro simd_shift_narrow_shape name, source_arrangement, bytes, destination_arrangement, narrow, high, rounding
gadget \name
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	ubfx w11, w8, #16, #7
	add x12, _vcpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	ldr q1, [x13]
	.if \bytes == 8
	neg x11, x11
	dup v2.\source_arrangement, x11
	.else
	neg w11, w11
	dup v2.\source_arrangement, w11
	.endif
	.if \rounding
	urshl v1.\source_arrangement, v1.\source_arrangement, v2.\source_arrangement
	.else
	ushl v1.\source_arrangement, v1.\source_arrangement, v2.\source_arrangement
	.endif
	.if \high
	add x13, x12, x9, lsl #4
	ldr q0, [x13]
	\narrow\()2 v0.\destination_arrangement, v1.\source_arrangement
	.else
	movi v0.16b, #0
	\narrow v0.\destination_arrangement, v1.\source_arrangement
	.endif
	add x13, x12, x9, lsl #4
	str q0, [x13]
	next_gadget
.endm

simd_shift_narrow_shape simd_shrn_8h_to_8b, 8h, 2, 8b, xtn, 0, 0
simd_shift_narrow_shape simd_shrn2_8h_to_8b, 8h, 2, 16b, xtn, 1, 0
simd_shift_narrow_shape simd_rshrn_8h_to_8b, 8h, 2, 8b, xtn, 0, 1
simd_shift_narrow_shape simd_rshrn2_8h_to_8b, 8h, 2, 16b, xtn, 1, 1
simd_shift_narrow_shape simd_shrn_4s_to_4h, 4s, 4, 4h, xtn, 0, 0
simd_shift_narrow_shape simd_shrn2_4s_to_4h, 4s, 4, 8h, xtn, 1, 0
simd_shift_narrow_shape simd_rshrn_4s_to_4h, 4s, 4, 4h, xtn, 0, 1
simd_shift_narrow_shape simd_rshrn2_4s_to_4h, 4s, 4, 8h, xtn, 1, 1
simd_shift_narrow_shape simd_shrn_2d_to_2s, 2d, 8, 2s, xtn, 0, 0
simd_shift_narrow_shape simd_shrn2_2d_to_2s, 2d, 8, 4s, xtn, 1, 0
simd_shift_narrow_shape simd_rshrn_2d_to_2s, 2d, 8, 2s, xtn, 0, 1
simd_shift_narrow_shape simd_rshrn2_2d_to_2s, 2d, 8, 4s, xtn, 1, 1

.purgem simd_shift_narrow_shape

.macro simd_saturating_narrow_shape name, source_arrangement, bytes, destination_arrangement, shift_operation, narrow_operation, high, scalar
gadget \name
	ensure_fp_environment
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	ubfx w11, w8, #16, #7
	add x12, _vcpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	.if \scalar
	.if \bytes == 2
	ldr h1, [x13]
	.elseif \bytes == 4
	ldr s1, [x13]
	.else
	ldr d1, [x13]
	.endif
	.else
	ldr q1, [x13]
	.endif
	.if \bytes == 8
	neg x11, x11
	dup v2.\source_arrangement, x11
	.else
	neg w11, w11
	dup v2.\source_arrangement, w11
	.endif
	\shift_operation v1.\source_arrangement, v1.\source_arrangement, v2.\source_arrangement
	.if \high
	add x13, x12, x9, lsl #4
	ldr q0, [x13]
	\narrow_operation\()2 v0.\destination_arrangement, v1.\source_arrangement
	.else
	\narrow_operation v0.\destination_arrangement, v1.\source_arrangement
	.endif
	add x13, x12, x9, lsl #4
	str q0, [x13]
	next_gadget
.endm

.macro simd_saturating_narrow_operation name, shift_operation, narrow_operation
	simd_saturating_narrow_shape simd_\name\()_h_b_low, 8h, 2, 8b, \shift_operation, \narrow_operation, 0, 0
	simd_saturating_narrow_shape simd_\name\()_h_b_high, 8h, 2, 16b, \shift_operation, \narrow_operation, 1, 0
	simd_saturating_narrow_shape simd_\name\()_h_b_scalar, 8h, 2, 8b, \shift_operation, \narrow_operation, 0, 1
	simd_saturating_narrow_shape simd_\name\()_s_h_low, 4s, 4, 4h, \shift_operation, \narrow_operation, 0, 0
	simd_saturating_narrow_shape simd_\name\()_s_h_high, 4s, 4, 8h, \shift_operation, \narrow_operation, 1, 0
	simd_saturating_narrow_shape simd_\name\()_s_h_scalar, 4s, 4, 4h, \shift_operation, \narrow_operation, 0, 1
	simd_saturating_narrow_shape simd_\name\()_d_s_low, 2d, 8, 2s, \shift_operation, \narrow_operation, 0, 0
	simd_saturating_narrow_shape simd_\name\()_d_s_high, 2d, 8, 4s, \shift_operation, \narrow_operation, 1, 0
	simd_saturating_narrow_shape simd_\name\()_d_s_scalar, 2d, 8, 2s, \shift_operation, \narrow_operation, 0, 1
.endm

simd_saturating_narrow_operation sqshrn, sshl, sqxtn
simd_saturating_narrow_operation sqrshrn, srshl, sqxtn
simd_saturating_narrow_operation sqshrun, sshl, sqxtun
simd_saturating_narrow_operation sqrshrun, srshl, sqxtun
simd_saturating_narrow_operation uqshrn, ushl, uqxtn
simd_saturating_narrow_operation uqrshrn, urshl, uqxtn

.purgem simd_saturating_narrow_operation
.purgem simd_saturating_narrow_shape

/* Integer by-element operations use one native gadget for every operation,
 * arrangement, and lane index. The builder resolves every encoding choice;
 * the hot path unpacks offsets, then performs register loads, the host
 * operation, and stores. */
.macro simd_indexed_same_shape name, operation, arrangement, element, index, wide, accumulate
gadget simd_indexed_\name\()_\arrangement\()_\index
	ldr x8, [_stream], #8
	unpack_u16_3 x8, x13, x14, x15
	.if \accumulate
	ldr q0, [_vcpu, x13]
	.endif
	ldr q1, [_vcpu, x14]
	ldr q2, [_vcpu, x15]
	\operation v0.\arrangement, v1.\arrangement, v2.\element[\index]
	str q0, [_vcpu, x13]
	next_gadget
.endm

.macro simd_indexed_same_family name, operation, accumulate
	.irp index, 0, 1, 2, 3, 4, 5, 6, 7
		simd_indexed_same_shape \name, \operation, 4h, h, \index, 0, \accumulate
		simd_indexed_same_shape \name, \operation, 8h, h, \index, 1, \accumulate
	.endr
	.irp index, 0, 1, 2, 3
		simd_indexed_same_shape \name, \operation, 2s, s, \index, 0, \accumulate
		simd_indexed_same_shape \name, \operation, 4s, s, \index, 1, \accumulate
	.endr
.endm

simd_indexed_same_family mla, mla, 1
simd_indexed_same_family mls, mls, 1
simd_indexed_same_family mul, mul, 0

.purgem simd_indexed_same_family
.purgem simd_indexed_same_shape

.macro simd_indexed_long_shape name, operation, narrow, wide, element, index, high, accumulate
gadget simd_indexed_\name\()_\element\()_\high\()_\index
	ldr x8, [_stream], #8
	unpack_u16_3 x8, x13, x14, x15
	.if \accumulate
	ldr q0, [_vcpu, x13]
	.endif
	ldr q1, [_vcpu, x14]
	ldr q2, [_vcpu, x15]
	\operation v0.\wide, v1.\narrow, v2.\element[\index]
	str q0, [_vcpu, x13]
	next_gadget
.endm

.macro simd_indexed_long_family name, low_operation, high_operation, accumulate
	.irp index, 0, 1, 2, 3, 4, 5, 6, 7
		simd_indexed_long_shape \name, \low_operation, 4h, 4s, h, \index, low, \accumulate
		simd_indexed_long_shape \name, \high_operation, 8h, 4s, h, \index, high, \accumulate
	.endr
	.irp index, 0, 1, 2, 3
		simd_indexed_long_shape \name, \low_operation, 2s, 2d, s, \index, low, \accumulate
		simd_indexed_long_shape \name, \high_operation, 4s, 2d, s, \index, high, \accumulate
	.endr
.endm

simd_indexed_long_family smlal, smlal, smlal2, 1
simd_indexed_long_family smlsl, smlsl, smlsl2, 1
simd_indexed_long_family smull, smull, smull2, 0
simd_indexed_long_family umlal, umlal, umlal2, 1
simd_indexed_long_family umlsl, umlsl, umlsl2, 1
simd_indexed_long_family umull, umull, umull2, 0

.purgem simd_indexed_long_family
.purgem simd_indexed_long_shape

.macro simd_indexed_saturating_long_shape name, operation, narrow, wide, element, load_source, load_destination, index, shape, scalar, accumulate
gadget simd_indexed_\name\()_\element\()_\shape\()_\index
	ensure_fp_environment
	ldr x8, [_stream], #8
	unpack_u16_3 x8, x13, x14, x15
	.if \scalar
	movi v0.16b, #0
	movi v1.16b, #0
	.if \accumulate
	ldr \load_destination, [_vcpu, x13]
	.endif
	ldr \load_source, [_vcpu, x14]
	.else
	.if \accumulate
	ldr q0, [_vcpu, x13]
	.endif
	ldr q1, [_vcpu, x14]
	.endif
	ldr q2, [_vcpu, x15]
	\operation \wide, \narrow, v2.\element[\index]
	str q0, [_vcpu, x13]
	next_gadget
.endm

.macro simd_indexed_saturating_long_family name, low_operation, high_operation, accumulate
	.irp index, 0, 1, 2, 3, 4, 5, 6, 7
		simd_indexed_saturating_long_shape \name, \low_operation, v1.4h, v0.4s, h, h1, s0, \index, low, 0, \accumulate
		simd_indexed_saturating_long_shape \name, \high_operation, v1.8h, v0.4s, h, h1, s0, \index, high, 0, \accumulate
		simd_indexed_saturating_long_shape \name, \low_operation, h1, s0, h, h1, s0, \index, scalar, 1, \accumulate
	.endr
	.irp index, 0, 1, 2, 3
		simd_indexed_saturating_long_shape \name, \low_operation, v1.2s, v0.2d, s, s1, d0, \index, low, 0, \accumulate
		simd_indexed_saturating_long_shape \name, \high_operation, v1.4s, v0.2d, s, s1, d0, \index, high, 0, \accumulate
		simd_indexed_saturating_long_shape \name, \low_operation, s1, d0, s, s1, d0, \index, scalar, 1, \accumulate
	.endr
.endm

simd_indexed_saturating_long_family sqdmlal, sqdmlal, sqdmlal2, 1
simd_indexed_saturating_long_family sqdmlsl, sqdmlsl, sqdmlsl2, 1
simd_indexed_saturating_long_family sqdmull, sqdmull, sqdmull2, 0

.purgem simd_indexed_saturating_long_family
.purgem simd_indexed_saturating_long_shape

.macro simd_indexed_saturating_high_shape name, operation, arrangement, element, load_source, index, shape, scalar, wide
gadget simd_indexed_\name\()_\element\()_\shape\()_\index
	ensure_fp_environment
	ldr x8, [_stream], #8
	unpack_u16_3 x8, x13, x14, x15
	.if \scalar
	movi v1.16b, #0
	ldr \load_source, [_vcpu, x14]
	.else
	ldr q1, [_vcpu, x14]
	.endif
	ldr q2, [_vcpu, x15]
	\operation \arrangement, \load_source, v2.\element[\index]
	str q0, [_vcpu, x13]
	next_gadget
.endm

.macro simd_indexed_saturating_high_family name, operation
	.irp index, 0, 1, 2, 3, 4, 5, 6, 7
		simd_indexed_saturating_high_shape \name, \operation, v0.4h, h, v1.4h, \index, low, 0, 0
		simd_indexed_saturating_high_shape \name, \operation, v0.8h, h, v1.8h, \index, high, 0, 1
		simd_indexed_saturating_high_shape \name, \operation, h0, h, h1, \index, scalar, 1, 0
	.endr
	.irp index, 0, 1, 2, 3
		simd_indexed_saturating_high_shape \name, \operation, v0.2s, s, v1.2s, \index, low, 0, 0
		simd_indexed_saturating_high_shape \name, \operation, v0.4s, s, v1.4s, \index, high, 0, 1
		simd_indexed_saturating_high_shape \name, \operation, s0, s, s1, \index, scalar, 1, 0
	.endr
.endm

simd_indexed_saturating_high_family sqdmulh, sqdmulh
simd_indexed_saturating_high_family sqrdmulh, sqrdmulh

.purgem simd_indexed_saturating_high_family
.purgem simd_indexed_saturating_high_shape

/* FEAT_RDM accumulate/subtract forms round the full accumulator expression,
 * so use the exact host operations rather than composing SQ(R)DMULH + ADD. */
.macro simd_indexed_saturating_accumulate_high_shape name, operation, arrangement, element, load_source, load_destination, index, shape, scalar, wide
gadget simd_indexed_\name\()_\element\()_\shape\()_\index
	ensure_fp_environment
	ldr x8, [_stream], #8
	unpack_u16_3 x8, x13, x14, x15
	.if \scalar
	movi v0.16b, #0
	movi v1.16b, #0
	ldr \load_destination, [_vcpu, x13]
	ldr \load_source, [_vcpu, x14]
	.else
	ldr q0, [_vcpu, x13]
	ldr q1, [_vcpu, x14]
	.endif
	ldr q2, [_vcpu, x15]
	\operation \arrangement, \load_source, v2.\element[\index]
	str q0, [_vcpu, x13]
	next_gadget
.endm

.macro simd_indexed_saturating_accumulate_high_family name, operation
	.irp index, 0, 1, 2, 3, 4, 5, 6, 7
		simd_indexed_saturating_accumulate_high_shape \name, \operation, v0.4h, h, v1.4h, h0, \index, low, 0, 0
		simd_indexed_saturating_accumulate_high_shape \name, \operation, v0.8h, h, v1.8h, h0, \index, high, 0, 1
		simd_indexed_saturating_accumulate_high_shape \name, \operation, h0, h, h1, h0, \index, scalar, 1, 0
	.endr
	.irp index, 0, 1, 2, 3
		simd_indexed_saturating_accumulate_high_shape \name, \operation, v0.2s, s, v1.2s, s0, \index, low, 0, 0
		simd_indexed_saturating_accumulate_high_shape \name, \operation, v0.4s, s, v1.4s, s0, \index, high, 0, 1
		simd_indexed_saturating_accumulate_high_shape \name, \operation, s0, s, s1, s0, \index, scalar, 1, 0
	.endr
.endm

simd_indexed_saturating_accumulate_high_family sqrdmlah, sqrdmlah
simd_indexed_saturating_accumulate_high_family sqrdmlsh, sqrdmlsh

.purgem simd_indexed_saturating_accumulate_high_family
.purgem simd_indexed_saturating_accumulate_high_shape

/* Floating-point by-element operations are specialized by operation,
 * arrangement, scalar/vector form, and element index. The hot path has no
 * decode or control branches. */
.macro simd_indexed_floating_shape name, operation, shape, destination, source, element, index, accumulate, forward=0
.set Lvector_old\@, Lscalar_none
.if \accumulate
.set Lvector_old\@, Lscalar_offset0
.endif
vector_entry simd_indexed_\name\()_\shape\()_\index, \forward, Lscalar_offset0, Lscalar_offset16, Lscalar_offset32, Lvector_old\@, 1
	ensure_fp_environment
	ldr x8, [_stream], #8
	and x9, x8, #0xffff
	ubfx x10, x8, #16, #16
	lsr x11, x8, #32
	vector_read \forward, 1, q0, [_vcpu, x10]
	vector_read \forward, 2, q1, [_vcpu, x11]
	.if \accumulate
	vector_read \forward, 4, q2, [_vcpu, x9]
	.endif
	\operation \destination, \source, v1.\element[\index]
	str q2, [_vcpu, x9]
	next_gadget
	vector_variants \forward, Lscalar_offset16, Lscalar_offset32, Lvector_old\@, simd_indexed_floating_shape, \name, \operation, \shape, \destination, \source, \element, \index, \accumulate
.endm

.macro simd_indexed_floating_family name, operation, accumulate
	.irp index, 0, 1, 2, 3, 4, 5, 6, 7
		simd_indexed_floating_shape \name, \operation, h, h2, h0, h, \index, \accumulate
		simd_indexed_floating_shape \name, \operation, 4h, v2.4h, v0.4h, h, \index, \accumulate
		simd_indexed_floating_shape \name, \operation, 8h, v2.8h, v0.8h, h, \index, \accumulate
	.endr
	.irp index, 0, 1, 2, 3
		simd_indexed_floating_shape \name, \operation, s, s2, s0, s, \index, \accumulate
		simd_indexed_floating_shape \name, \operation, 2s, v2.2s, v0.2s, s, \index, \accumulate
		simd_indexed_floating_shape \name, \operation, 4s, v2.4s, v0.4s, s, \index, \accumulate
	.endr
	.irp index, 0, 1
		simd_indexed_floating_shape \name, \operation, d, d2, d0, d, \index, \accumulate
		simd_indexed_floating_shape \name, \operation, 2d, v2.2d, v0.2d, d, \index, \accumulate
	.endr
.endm

simd_indexed_floating_family fmla, fmla, 1
simd_indexed_floating_family fmls, fmls, 1
simd_indexed_floating_family fmul, fmul, 0
simd_indexed_floating_family fmulx, fmulx, 0

.purgem simd_indexed_floating_family
.purgem simd_indexed_floating_shape

.macro simd_fmulx_shape name, destination, left, right, element, scalar
gadget simd_fmulx_\name
	ensure_fp_environment
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	ubfx w11, w8, #16, #5
	add x12, _vcpu, #UML_GADGET_CPU_V0
	add x13, x12, x9, lsl #4
	add x14, x12, x10, lsl #4
	add x15, x12, x11, lsl #4
	ldr q0, [x14]
	ldr q1, [x15]
	fmulx \destination, \left, \right
	str q2, [x13]
	next_gadget
.endm

simd_fmulx_shape h, h2, h0, h1, h, 1
simd_fmulx_shape 4h, v2.4h, v0.4h, v1.4h, h, 0
simd_fmulx_shape 8h, v2.8h, v0.8h, v1.8h, h, 0
simd_fmulx_shape s, s2, s0, s1, s, 1
simd_fmulx_shape 2s, v2.2s, v0.2s, v1.2s, s, 0
simd_fmulx_shape 4s, v2.4s, v0.4s, v1.4s, s, 0
simd_fmulx_shape d, d2, d0, d1, d, 1
simd_fmulx_shape 2d, v2.2d, v0.2d, v1.2d, d, 0

.purgem simd_fmulx_shape

gadget simd_addp_scalar
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	add x11, _vcpu, #UML_GADGET_CPU_V0
	add x12, x11, x10, lsl #4
	add x13, x11, x9, lsl #4
	ldr q1, [x12]
	addp d0, v1.2d
	str d0, [x13]
	str xzr, [x13, #8]
	next_gadget

/* Builder-predecoded MOVI/MVNI/ORR/BIC/FMOV immediate.  Operation and Q are
 * selected at translation time.  Operand: destination[4:0], then imm64. */
.macro simd_immediate_replace name, wide
gadget \name
	ldr x8, [_stream], #8
	ldr x9, [_stream], #8
	and w10, w8, #0x1f
	add x11, _vcpu, #UML_GADGET_CPU_V0
	add x11, x11, x10, lsl #4
	.if \wide
	stp x9, x9, [x11]
	.else
	str x9, [x11]
	str xzr, [x11, #8]
	.endif
	next_gadget
.endm

.macro simd_immediate_combine name, operation, wide
gadget \name
	ldr x8, [_stream], #8
	ldr x9, [_stream], #8
	and w10, w8, #0x1f
	add x11, _vcpu, #UML_GADGET_CPU_V0
	add x11, x11, x10, lsl #4
	.if \wide
	ldr q0, [x11]
	dup v1.2d, x9
	\operation v0.16b, v0.16b, v1.16b
	str q0, [x11]
	.else
	ldr x12, [x11]
	\operation x12, x12, x9
	str x12, [x11]
	str xzr, [x11, #8]
	.endif
	next_gadget
.endm

simd_immediate_replace simd_immediate_replace_8b, 0
simd_immediate_replace simd_immediate_replace_16b, 1
simd_immediate_combine simd_immediate_orr_8b, orr, 0
simd_immediate_combine simd_immediate_orr_16b, orr, 1
simd_immediate_combine simd_immediate_bic_8b, bic, 0
simd_immediate_combine simd_immediate_bic_16b, bic, 1

.purgem simd_immediate_combine
.purgem simd_immediate_replace

/* Exact FMOV forms between the integer file and scalar/vector bit patterns.
 * Operand: destination[4:0], source[12:8]. */
.p2align 4
.macro simd_lane_to_gpr name, load, target
gadget \name
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	ubfx w11, w8, #16, #4
	add x12, _vcpu, #UML_GADGET_CPU_V0
	add x12, x12, x10, lsl #4
	add x12, x12, x11
	\load \target, [x12]
	add x12, _vcpu, #UML_GADGET_CPU_X0
	str x13, [x12, x9, lsl #3]
	next_gadget
.endm

simd_lane_to_gpr simd_lane_to_gpr_smov_w_b, ldrsb, w13
simd_lane_to_gpr simd_lane_to_gpr_smov_w_h, ldrsh, w13
simd_lane_to_gpr simd_lane_to_gpr_smov_x_b, ldrsb, x13
simd_lane_to_gpr simd_lane_to_gpr_smov_x_h, ldrsh, x13
simd_lane_to_gpr simd_lane_to_gpr_smov_x_s, ldrsw, x13
simd_lane_to_gpr simd_lane_to_gpr_umov_w_b, ldrb, w13
simd_lane_to_gpr simd_lane_to_gpr_umov_w_h, ldrh, w13
simd_lane_to_gpr simd_lane_to_gpr_umov_w_s, ldr, w13
simd_lane_to_gpr simd_lane_to_gpr_umov_x_d, ldr, x13

.purgem simd_lane_to_gpr

gadget simd_lane_to_gpr_discard
	ldr x8, [_stream], #8
	next_gadget

.macro simd_insert_general_shape name, bytes, zero
gadget \name
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #16, #4
	.if !\zero
	ubfx w11, w8, #8, #5
	add x12, _vcpu, #UML_GADGET_CPU_X0
	ldr x13, [x12, x11, lsl #3]
	.else
	mov x13, xzr
	.endif
	add x12, _vcpu, #UML_GADGET_CPU_V0
	add x12, x12, x9, lsl #4
	add x12, x12, x10
	.if \bytes == 1
	strb w13, [x12]
	.elseif \bytes == 2
	strh w13, [x12]
	.elseif \bytes == 4
	str w13, [x12]
	.else
	str x13, [x12]
	.endif
	next_gadget
.endm

simd_insert_general_shape simd_insert_general_b, 1, 0
simd_insert_general_shape simd_insert_general_h, 2, 0
simd_insert_general_shape simd_insert_general_s, 4, 0
simd_insert_general_shape simd_insert_general_d, 8, 0
simd_insert_general_shape simd_insert_general_zero_b, 1, 1
simd_insert_general_shape simd_insert_general_zero_h, 2, 1
simd_insert_general_shape simd_insert_general_zero_s, 4, 1
simd_insert_general_shape simd_insert_general_zero_d, 8, 1

.purgem simd_insert_general_shape

/* INS (element) loads before storing, preserving alias correctness without a
 * source/destination comparison on the valid hot path. */
.macro simd_insert_element_shape name, bytes
gadget \name
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	ubfx w11, w8, #16, #4
	ubfx w12, w8, #24, #4
	add x13, _vcpu, #UML_GADGET_CPU_V0
	add x14, x13, x10, lsl #4
	add x14, x14, x12
	.if \bytes == 1
	ldrb w15, [x14]
	.elseif \bytes == 2
	ldrh w15, [x14]
	.elseif \bytes == 4
	ldr w15, [x14]
	.else
	ldr x15, [x14]
	.endif
	add x13, x13, x9, lsl #4
	add x13, x13, x11
	.if \bytes == 1
	strb w15, [x13]
	.elseif \bytes == 2
	strh w15, [x13]
	.elseif \bytes == 4
	str w15, [x13]
	.else
	str x15, [x13]
	.endif
	next_gadget
.endm

simd_insert_element_shape simd_insert_element_b, 1
simd_insert_element_shape simd_insert_element_h, 2
simd_insert_element_shape simd_insert_element_s, 4
simd_insert_element_shape simd_insert_element_d, 8

.purgem simd_insert_element_shape

.macro vector_to_gpr name, offset, bytes
gadget \name
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	add x11, _vcpu, #UML_GADGET_CPU_V0
	add x11, x11, x10, lsl #4
	.if \bytes == 2
	ldrh w12, [x11, #\offset]
	.elseif \bytes == 4
	ldr w12, [x11, #\offset]
	.else
	ldr x12, [x11, #\offset]
	.endif
	add x13, _vcpu, #UML_GADGET_CPU_X0
	str x12, [x13, x9, lsl #3]
	next_gadget
.endm

.macro gpr_to_vector name, offset, bytes, zero
gadget \name
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	.if \zero
	mov x12, xzr
	.else
	add x11, _vcpu, #UML_GADGET_CPU_X0
	ldr x12, [x11, x10, lsl #3]
	.endif
	add x13, _vcpu, #UML_GADGET_CPU_V0
	add x13, x13, x9, lsl #4
	.if \bytes == 2
	strh w12, [x13, #\offset]
	strh wzr, [x13, #2]
	str wzr, [x13, #4]
	str xzr, [x13, #8]
	.elseif \bytes == 4
	str w12, [x13, #\offset]
	str wzr, [x13, #4]
	str xzr, [x13, #8]
	.else
	str x12, [x13, #\offset]
	.if \offset == 0
	str xzr, [x13, #8]
	.endif
	.endif
	next_gadget
.endm

vector_to_gpr vector_to_gpr_16_low, 0, 2
vector_to_gpr vector_to_gpr_64_low, 0, 8
vector_to_gpr vector_to_gpr_32_low, 0, 4
vector_to_gpr vector_to_gpr_64_high, 8, 8
gpr_to_vector gpr_to_vector_16_low, 0, 2, 0
gpr_to_vector gpr_to_vector_16_low_zero, 0, 2, 1
gpr_to_vector gpr_to_vector_32_low, 0, 4, 0
gpr_to_vector gpr_to_vector_32_low_zero, 0, 4, 1
gpr_to_vector gpr_to_vector_64_low, 0, 8, 0
gpr_to_vector gpr_to_vector_64_low_zero, 0, 8, 1
gpr_to_vector gpr_to_vector_64_high, 8, 8, 0
gpr_to_vector gpr_to_vector_64_high_zero, 8, 8, 1

/* FCSEL's condition and precision are fixed by the builder. Operand:
 * destination[4:0], true-source[12:8], false-source[20:16]. */
.macro floating_select_shape name, condition, suffix, bytes
gadget floating_select_\name\()_\suffix
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	ubfx w11, w8, #16, #5
	msr nzcv, x27
	add x12, _vcpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	add x14, x12, x11, lsl #4
	csel x13, x13, x14, \condition
	add x14, x12, x9, lsl #4
	.if \bytes == 2
	ldrh w15, [x13]
	strh w15, [x14]
	strh wzr, [x14, #2]
	str wzr, [x14, #4]
	.elseif \bytes == 4
	ldr w15, [x13]
	str w15, [x14]
	str wzr, [x14, #4]
	.else
	ldr x15, [x13]
	str x15, [x14]
	.endif
	str xzr, [x14, #8]
	next_gadget
.endm

.macro floating_select_family name, condition
	floating_select_shape \name, \condition, h, 2
	floating_select_shape \name, \condition, s, 4
	floating_select_shape \name, \condition, d, 8
.endm

floating_select_family eq, eq
floating_select_family ne, ne
floating_select_family hs, hs
floating_select_family lo, lo
floating_select_family mi, mi
floating_select_family pl, pl
floating_select_family vs, vs
floating_select_family vc, vc
floating_select_family hi, hi
floating_select_family ls, ls
floating_select_family ge, ge
floating_select_family lt, lt
floating_select_family gt, gt
floating_select_family le, le

.macro floating_select_always_shape suffix, bytes
gadget floating_select_always_\suffix
	ldr x8, [_stream], #8
	and w9, w8, #0x1f
	ubfx w10, w8, #8, #5
	add x12, _vcpu, #UML_GADGET_CPU_V0
	add x13, x12, x10, lsl #4
	add x14, x12, x9, lsl #4
	.if \bytes == 2
	ldrh w15, [x13]
	strh w15, [x14]
	strh wzr, [x14, #2]
	str wzr, [x14, #4]
	.elseif \bytes == 4
	ldr w15, [x13]
	str w15, [x14]
	str wzr, [x14, #4]
	.else
	ldr x15, [x13]
	str x15, [x14]
	.endif
	str xzr, [x14, #8]
	next_gadget
.endm

floating_select_always_shape h, 2
floating_select_always_shape s, 4
floating_select_always_shape d, 8

.purgem floating_select_always_shape
.purgem floating_select_family
.purgem floating_select_shape

.purgem afp_merge_source
.purgem fp_two_vector_operands
.purgem unpack_u16_4
.purgem unpack_u16_3
.purgem unpack_u16_2
