/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Intel SHA-extension state and message operations.
 *
 * The two SHA-256 message schedules are the host's own SHA256SU0 and
 * SHA256SU1. The round instruction is not: x86's SHA256RNDS2 performs two
 * rounds and the host's SHA256H pair performs four, so it keeps the scalar
 * sequence. The SHA-1 operations keep theirs because x86 numbers their words
 * from the high lane down, which no host operand order matches.
 */
.arch_extension crypto

.macro vector_sha_sum0 output, input
	ror w\output, w\input, #2
	ror w15, w\input, #13
	eor w\output, w\output, w15
	ror w15, w\input, #22
	eor w\output, w\output, w15
.endm

.macro vector_sha_sum1 output, input
	ror w\output, w\input, #6
	ror w15, w\input, #11
	eor w\output, w\output, w15
	ror w15, w\input, #25
	eor w\output, w\output, w15
.endm

.macro vector_sha_round constant_offset
	/* choose(e,f,g) */
	eor w9, w5, w6
	and w9, w9, w4
	eor w9, w9, w6
	vector_sha_sum1 10, 4
	add w9, w9, w10
	ldr w8, [_vcpu, #(UML_X86_64_CPU_VECTORS + \constant_offset)]
	add w9, w9, w8
	add w9, w9, w7
	/* majority(a,b,c) */
	eor w10, w0, w1
	and w10, w10, w2
	and w11, w0, w1
	eor w10, w10, w11
	vector_sha_sum0 11, 0
	add w13, w9, w10
	add w13, w13, w11
	add w14, w9, w3
	mov w7, w6
	mov w6, w5
	mov w5, w4
	mov w4, w14
	mov w3, w2
	mov w2, w1
	mov w1, w0
	mov w0, w13
.endm

.macro vector_sha_body opcode
	.if \opcode == 0xc8
	mov v4.16b, v1.16b
	mov w14, v0.s[3]
	ror w14, w14, #2
	mov w15, v1.s[3]
	add w14, w14, w15
	mov v4.s[3], w14
	.elseif \opcode == 0xc9
	mov w10, v0.s[0]
	mov w11, v0.s[1]
	mov w12, v0.s[2]
	mov w13, v0.s[3]
	mov w14, v1.s[2]
	eor w14, w14, w10
	mov v4.s[0], w14
	mov w14, v1.s[3]
	eor w14, w14, w11
	mov v4.s[1], w14
	eor w14, w10, w12
	mov v4.s[2], w14
	eor w14, w11, w13
	mov v4.s[3], w14
	.elseif \opcode == 0xca
	mov w10, v0.s[3]
	mov w11, v1.s[2]
	eor w10, w10, w11
	ror w10, w10, #31
	mov v4.s[3], w10
	mov w11, v0.s[2]
	mov w12, v1.s[1]
	eor w11, w11, w12
	ror w11, w11, #31
	mov v4.s[2], w11
	mov w12, v0.s[1]
	mov w13, v1.s[0]
	eor w12, w12, w13
	ror w12, w12, #31
	mov v4.s[1], w12
	mov w13, v0.s[0]
	eor w13, w13, w10
	ror w13, w13, #31
	mov v4.s[0], w13
	.elseif \opcode == 0xcb
	mov w0, v1.s[3]
	mov w1, v1.s[2]
	mov w2, v0.s[3]
	mov w3, v0.s[2]
	mov w4, v1.s[1]
	mov w5, v1.s[0]
	mov w6, v0.s[1]
	mov w7, v0.s[0]
	vector_sha_round 0
	vector_sha_round 4
	mov v4.s[3], w0
	mov v4.s[2], w1
	mov v4.s[1], w4
	mov v4.s[0], w5
	.elseif \opcode == 0xcc
	/* SHA256MSG1 is the host's SHA256SU0 exactly: each lane takes the guest
	 * destination's own word plus sigma0 of the next one, and the top lane
	 * takes sigma0 of the source's low word. */
	mov v4.16b, v0.16b
	sha256su0 v4.4s, v1.4s
	.else
	/* SHA256MSG2 is the host's SHA256SU1 with its third addend suppressed.
	 * SU1 adds {Vn[1], Vn[2], Vn[3], Vm[0]} on top of the destination, which
	 * x86 does not, so the second source is zero and the copy of the guest
	 * source clears the low word the host would otherwise add. The sigma1
	 * inputs and the two lanes that feed the upper two are identical. */
	movi v5.4s, #0
	mov v6.16b, v1.16b
	ins v6.s[0], wzr
	mov v4.16b, v0.16b
	sha256su1 v4.4s, v5.4s, v6.4s
	.endif
.endm

.macro vector_sha_register opcode
gadget vector_sha_\opcode\()_rr
	ldr x8, [_stream], #8
	mov x17, x8
	ubfx x10, x8, #16, #16
	ubfx x11, x8, #32, #16
	ldr q0, [_vcpu, x10]
	ldr q1, [_vcpu, x11]
	vector_sha_body 0x\opcode
	and x9, x17, #0xffff
	str q4, [_vcpu, x9]
	retire_and_next
.endm

.macro vector_sha_memory opcode
gadget vector_sha_\opcode\()_rm
	effective_address
	ldr x26, [_stream], #8
	mov x25, x7
	vector_memory_source 16, Lx86_vector_sha_cross_fault\@, frame=16
	and x10, x26, #0xffff
	ldr q0, [_vcpu, x10]
	vector_sha_body 0x\opcode
	and x9, x23, #0xffff
	str q4, [_vcpu, x9]
	retire_and_next
	vector_memory_source_fault Lx86_vector_sha_cross_fault\@, 16
.endm

.macro vector_sha_family opcode
	vector_sha_register \opcode
	vector_sha_memory \opcode
.endm

vector_sha_family c8
vector_sha_family c9
vector_sha_family ca
vector_sha_family cb
vector_sha_family cc
vector_sha_family cd

.purgem vector_sha_family
.purgem vector_sha_memory
.purgem vector_sha_register
.purgem vector_sha_body
.purgem vector_sha_round
.purgem vector_sha_sum1
.purgem vector_sha_sum0
