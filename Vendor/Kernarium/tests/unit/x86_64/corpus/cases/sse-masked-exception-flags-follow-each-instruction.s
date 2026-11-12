/* SPDX-License-Identifier: GPL-2.0-or-later */
/* With every exception masked, each SIMD instruction leaves the flags x86
 * defines for it in MXCSR. RCP and RSQRT raise none and drop none. CMP
 * raises invalid operation for a quiet NaN under its signalling predicates
 * alone, VEX's included, and its false predicates still compare; COMIS
 * signals and UCOMIS does not, and MIN and MAX raise invalid operation for
 * any NaN. A conversion to an integer raises precision for a fraction, and
 * invalid operation alone out of range; ROUND raises precision unless its
 * immediate suppresses it. FTZ's flush raises underflow and precision, and
 * a denormal input under DAZ raises nothing.
 *
 * Slot n of XMM10 to XMM15 (four per register, lowest first) holds:
 * 0 RCPSS; 1 RSQRTSS; 2 CMPEQSS and 3 CMPLTSS of a quiet NaN; 4 COMISS and
 * 5 UCOMISS of a quiet NaN; 6 UCOMISS of a signalling NaN; 7 MINSS of a quiet
 * NaN; 8 CVTSD2SI of 3e9 + 0.5 and 9 of 1.5; 10 CVTTSD2SI of 1.5; 11 ROUNDSD
 * of 1.5 with immediate 0 and 12 with 8; 13 VCMPSS FALSE_OS and 14 FALSE_OQ
 * of a quiet NaN, 15 FALSE_OQ of a signalling NaN; 16 MULSS of the least
 * normal by a half under FTZ; 17 ADDSS of a denormal to one under DAZ;
 * 18 CVTTPS2DQ with one lane 1.5; 19 CVTPD2DQ with one lane 3e9 + 0.5;
 * 20 VCMPPS LT_OQ and 21 EQ_OS of a quiet NaN in a YMM register; 22 RCPSS
 * after precision from a quotient; 23 MAXSD of a signalling NaN.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x2000"},
  "RegData": {
    "XMM10": ["0x00001f8000001f80", "0x00001f8100001f80"],
    "XMM11": ["0x00001f8000001f81", "0x00001f8100001f81"],
    "XMM12": ["0x00001fa000001f81", "0x00001fa000001fa0"],
    "XMM13": ["0x00001f8100001f80", "0x00001f8100001f80"],
    "XMM14": ["0x00001fc000009fb0", "0x00001f8100001fa0"],
    "XMM15": ["0x00001f8100001f80", "0x00001f8100001fa0"]
  }
}
*/
	.macro clear
	movl $0x1f80, 512(%r10)
	ldmxcsr 512(%r10)
	.endm

	.macro load value
	movl $\value, 512(%r10)
	ldmxcsr 512(%r10)
	.endm

	.macro flags slot
	stmxcsr 0x100 + 4 * \slot(%r10)
	.endm

	.text
	.globl _start
_start:
	movl $0x20001000, %r10d
	movss one_s(%rip), %xmm2
	movss quiet_s(%rip), %xmm3
	movss signalling_s(%rip), %xmm4

	clear
	rcpss three_s(%rip), %xmm0
	flags 0

	clear
	rsqrtss three_s(%rip), %xmm0
	flags 1

	clear
	movaps %xmm2, %xmm0
	cmpeqss %xmm3, %xmm0
	flags 2

	clear
	movaps %xmm2, %xmm0
	cmpltss %xmm3, %xmm0
	flags 3

	clear
	comiss %xmm3, %xmm2
	flags 4

	clear
	ucomiss %xmm3, %xmm2
	flags 5

	clear
	ucomiss %xmm4, %xmm2
	flags 6

	clear
	movaps %xmm2, %xmm0
	minss %xmm3, %xmm0
	flags 7

	clear
	cvtsd2si big_d(%rip), %eax
	flags 8

	clear
	cvtsd2si one_and_a_half_d(%rip), %eax
	flags 9

	clear
	cvttsd2si one_and_a_half_d(%rip), %eax
	flags 10

	clear
	roundsd $0, one_and_a_half_d(%rip), %xmm0
	flags 11

	clear
	roundsd $8, one_and_a_half_d(%rip), %xmm0
	flags 12

	clear
	vcmpss $0x1b, %xmm3, %xmm2, %xmm0
	flags 13

	clear
	vcmpss $0x0b, %xmm3, %xmm2, %xmm0
	flags 14

	clear
	vcmpss $0x0b, %xmm4, %xmm2, %xmm0
	flags 15

	load 0x9f80
	movss least_normal_s(%rip), %xmm0
	mulss half_s(%rip), %xmm0
	flags 16

	load 0x1fc0
	movss least_denormal_s(%rip), %xmm0
	addss %xmm2, %xmm0
	flags 17

	clear
	movups ones_and_a_half_s(%rip), %xmm1
	cvttps2dq %xmm1, %xmm0
	flags 18

	clear
	movupd one_and_big_d(%rip), %xmm1
	cvtpd2dq %xmm1, %xmm0
	flags 19

	clear
	vbroadcastss %xmm2, %ymm5
	vbroadcastss %xmm3, %ymm6
	vcmpps $0x11, %ymm6, %ymm5, %ymm0
	flags 20

	clear
	vcmpps $0x10, %ymm6, %ymm5, %ymm0
	flags 21

	clear
	movsd one_d(%rip), %xmm0
	divsd three_d(%rip), %xmm0
	rcpss three_s(%rip), %xmm0
	flags 22

	clear
	movsd one_d(%rip), %xmm0
	maxsd signalling_d(%rip), %xmm0
	flags 23

	clear
	vzeroupper
	movdqu 0x100(%r10), %xmm10
	movdqu 0x110(%r10), %xmm11
	movdqu 0x120(%r10), %xmm12
	movdqu 0x130(%r10), %xmm13
	movdqu 0x140(%r10), %xmm14
	movdqu 0x150(%r10), %xmm15
	hlt

	.balign 16
ones_and_a_half_s:
	.long 0x3f800000, 0x3fc00000, 0x3f800000, 0x3f800000
one_and_big_d:
	.quad 0x3ff0000000000000, 0x41e65a0bc0100000
big_d:
	.quad 0x41e65a0bc0100000
one_and_a_half_d:
	.quad 0x3ff8000000000000
one_d:
	.quad 0x3ff0000000000000
three_d:
	.quad 0x4008000000000000
signalling_d:
	.quad 0x7ff4000000000000
one_s:
	.long 0x3f800000
three_s:
	.long 0x40400000
half_s:
	.long 0x3f000000
quiet_s:
	.long 0x7fc00000
signalling_s:
	.long 0x7fa00000
least_normal_s:
	.long 0x00800000
least_denormal_s:
	.long 0x00000001
