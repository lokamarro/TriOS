/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FBSTP rounds its operand to an integer and stores 18 packed decimal
 * digits and a sign byte, raising precision when the rounding was inexact
 * and C1 when it rounded up; an operand of 10^18 or more, a NaN and an
 * infinity store the packed BCD indefinite instead, the ten bytes of a
 * negative QNaN, and raise invalid operation alone. F2XM1 does not compute
 * outside -1 to 1: an operand beyond it comes back unchanged with precision
 * raised, and -infinity returns -1 and raises nothing, while -1 gives -0.5
 * with precision raised, as every result F2XM1 computes does.
 *
 * Each FBSTP is a pair of registers, the digits' low eight bytes, then the
 * status word shifted left by 16 over the last two bytes: RAX/RBX 1.5,
 * RCX/RDX 10^19, RSI/RDI -0.5, RBP/R8 a QNaN. Each F2XM1 is a pair, the
 * result's significand, then the status word shifted left by 16 over its
 * sign and exponent: R10/R11 of 2.0, R12/R13 of -1.0, R14/R15 of -infinity.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0x2",
    "RBX": "0x2200000",
    "RCX": "0xc000000000000000",
    "RDX": "0x1ffff",
    "RSI": "0x0",
    "RDI": "0x208000",
    "RBP": "0xc000000000000000",
    "R8": "0x1ffff",
    "R10": "0x8000000000000000",
    "R11": "0x38204000",
    "R12": "0x8000000000000000",
    "R13": "0x3820bffe",
    "R14": "0x8000000000000000",
    "R15": "0x3800bfff"
  }
}
*/
	.text
	.globl _start
_start:
	movl $0x20000000, %r9d
	.macro bcd_case value, slot
	fninit
	fldt \value(%rip)
	fbstp 0x80(%r9)
	fnstsw %ax
	movzwl %ax, %eax
	shll $16, %eax
	movzwl 0x88(%r9), %ecx
	orl %ecx, %eax
	movl %eax, \slot+8(%r9)
	movq 0x80(%r9), %rcx
	movq %rcx, \slot(%r9)
	.endm
	.macro f2xm1_case value, slot
	fninit
	fldt \value(%rip)
	f2xm1
	fnstsw %ax
	movzwl %ax, %eax
	shll $16, %eax
	fstpt 0x80(%r9)
	movzwl 0x88(%r9), %ecx
	orl %ecx, %eax
	movl %eax, \slot+8(%r9)
	movq 0x80(%r9), %rcx
	movq %rcx, \slot(%r9)
	.endm
	bcd_case one_and_a_half, 0x100
	bcd_case ten_to_the_nineteenth, 0x110
	bcd_case minus_one_half, 0x120
	bcd_case quiet_nan, 0x130
	f2xm1_case two, 0x140
	f2xm1_case minus_one, 0x150
	f2xm1_case minus_infinity, 0x160
	movq 0x100(%r9), %rax
	movl 0x108(%r9), %ebx
	movq 0x110(%r9), %rcx
	movl 0x118(%r9), %edx
	movq 0x120(%r9), %rsi
	movl 0x128(%r9), %edi
	movq 0x130(%r9), %rbp
	movl 0x138(%r9), %r8d
	movq 0x140(%r9), %r10
	movl 0x148(%r9), %r11d
	movq 0x150(%r9), %r12
	movl 0x158(%r9), %r13d
	movq 0x160(%r9), %r14
	movl 0x168(%r9), %r15d
	xorl %r9d, %r9d
	hlt
	.p2align 4
one_and_a_half:
	.quad 0xc000000000000000
	.short 0x3fff
	.p2align 4
ten_to_the_nineteenth:
	.quad 0x8ac7230489e80000
	.short 0x403e
	.p2align 4
minus_one_half:
	.quad 0x8000000000000000
	.short 0xbffe
	.p2align 4
quiet_nan:
	.quad 0xc000000000000000
	.short 0x7fff
	.p2align 4
two:
	.quad 0x8000000000000000
	.short 0x4000
	.p2align 4
minus_one:
	.quad 0x8000000000000000
	.short 0xbfff
	.p2align 4
minus_infinity:
	.quad 0x8000000000000000
	.short 0xffff
	.p2align 4
