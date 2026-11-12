/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FXTRACT of a zero raises division by zero and returns an exponent of
 * negative infinity beside the zero. FYL2X of a zero is an infinity with
 * division by zero, unless its factor is zero too, which is invalid, as is
 * the logarithm of a negative number. FRNDINT of an inexact value raises
 * precision.
 *
 * RAX, RBX, RCX: FXTRACT of -0, its status word with C1 masked off and the
 * doubles in ST(0) and ST(1) as RBX and RCX. RDI and R8: FYL2X of
 * zero by one, its status word and result. R10: FYL2X of zero by zero, its
 * result; R11 FYL2X of -1 by one. R12 and R13: FRNDINT of 2.5, its status
 * word and result.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0x3004",
    "RBX": "0x8000000000000000",
    "RCX": "0xfff0000000000000",
    "RDI": "0x3804",
    "R8": "0xfff0000000000000",
    "R10": "0xfff8000000000000",
    "R11": "0xfff8000000000000",
    "R12": "0x3820",
    "R13": "0x4000000000000000"
  }
}
*/
	.text
	.globl _start
_start:
	movl $0x20000000, %r9d

	fninit
	fldz
	fchs
	fxtract
	fnstsw %ax
	andl $0xfdff, %eax
	movl %eax, 0x80(%r9)
	fstpl (%r9)
	movq (%r9), %rbx
	fstpl (%r9)
	movq (%r9), %rcx


	fninit
	fld1
	fldz
	fyl2x
	fnstsw %ax
	andl $0xfdff, %eax
	movl %eax, %edi
	fstpl (%r9)
	movq (%r9), %r8

	fninit
	fldz
	fldz
	fyl2x
	fstpl (%r9)
	movq (%r9), %r10

	fninit
	fld1
	fld1
	fchs
	fyl2x
	fstpl (%r9)
	movq (%r9), %r11

	fninit
	fldl two_and_a_half(%rip)
	frndint
	fnstsw %ax
	andl $0xfdff, %eax
	movl %eax, %r12d
	fstpl (%r9)
	movq (%r9), %r13

	fninit
	movl 0x80(%r9), %eax
	xorl %r9d, %r9d
	hlt
	.p2align 3
two_and_a_half:
	.quad 0x4004000000000000
