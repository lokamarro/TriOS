/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FXAM reports ST(0)'s class in C3, C2 and C0 -- 000 unsupported, 001 NaN,
 * 010 normal, 011 infinity, 100 zero, 101 empty, 110 denormal -- and its
 * sign in C1. A pseudo-denormal is a denormal; an unnormal, a pseudo-NaN
 * and a pseudo-infinity, whose integer bit is clear, are unsupported.
 *
 * Each register holds one status word, masked to C3-C0: RAX +0, RBX -0,
 * RCX +inf, RDX -inf, RSI QNaN, RDI -SNaN, R8 denormal, R10 pseudo-denormal,
 * R11 unnormal, R12 pseudo-infinity, R13 -normal, R14 empty after FLD1 and
 * FFREE, R15 pseudo-NaN.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0x4000",
    "RBX": "0x4200",
    "RCX": "0x0500",
    "RDX": "0x0700",
    "RSI": "0x0100",
    "RDI": "0x0300",
    "R8": "0x4400",
    "R10": "0x4400",
    "R11": "0x0000",
    "R12": "0x0000",
    "R13": "0x0600",
    "R14": "0x4100",
    "R15": "0x0000"
  }
}
*/
	.text
	.globl _start
_start:
	.macro examine value, register
	fninit
	fldt \value(%rip)
	fxam
	fnstsw %ax
	andl $0x4700, %eax
	movl %eax, \register
	.endm

	examine positive_zero, %ebx
	movl %ebx, %r9d
	examine negative_zero, %ebx
	movl %ebx, %r10d
	examine positive_infinity, %ecx
	examine negative_infinity, %edx
	examine quiet_nan, %esi
	examine negative_signaling_nan, %edi
	examine denormal, %r8d
	movl %r10d, %ebx
	examine pseudo_denormal, %r10d
	examine unnormal, %r11d
	examine pseudo_infinity, %r12d
	examine negative_normal, %r13d
	examine pseudo_nan, %r15d

	fninit
	fld1
	ffree %st(0)
	fxam
	fnstsw %ax
	andl $0x4700, %eax
	movl %eax, %r14d
	movl %r9d, %eax
	xorl %r9d, %r9d
	fninit
	hlt
	.p2align 4
positive_zero:
	.quad 0
	.short 0
	.p2align 4
negative_zero:
	.quad 0
	.short 0x8000
	.p2align 4
positive_infinity:
	.quad 0x8000000000000000
	.short 0x7fff
	.p2align 4
negative_infinity:
	.quad 0x8000000000000000
	.short 0xffff
	.p2align 4
quiet_nan:
	.quad 0xc000000000000000
	.short 0x7fff
	.p2align 4
negative_signaling_nan:
	.quad 0xa000000000000000
	.short 0xffff
	.p2align 4
denormal:
	.quad 1
	.short 0
	.p2align 4
pseudo_denormal:
	.quad 0x8000000000000001
	.short 0
	.p2align 4
unnormal:
	.quad 1
	.short 0x1234
	.p2align 4
pseudo_infinity:
	.quad 0
	.short 0x7fff
	.p2align 4
negative_normal:
	.quad 0x8000000000000000
	.short 0xbfff
	.p2align 4
pseudo_nan:
	.quad 0x4000000000000000
	.short 0x7fff
