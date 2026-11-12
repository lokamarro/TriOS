/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FPREM and FPREM1 compute their remainder exactly. Operands whose
 * exponents differ by less than 64 give the complete remainder, with the
 * quotient's low three bits in C0, C3 and C1; a difference of 64 or more
 * reduces the dividend by the divisor scaled to N = 32 + D mod 32 bits
 * below it and sets C2, as the Xeon 8581C does. Denormal operands count by
 * their normalized exponents.
 *
 * Each pair of registers holds a result's significand, then its status
 * word's condition codes and flags shifted left by 16 with its sign and
 * exponent: RAX/RBX FPREM of -7 by 2, RCX/RDX FPREM1 of -7 by 2, RSI/RDI
 * FPREM of 2^64 - 1 by 1 + 2^-63 (D 63), RBP/R8 the same times 2 (D 64,
 * partial), R10/R11 FPREM of 1 by the denormal 3 * 2^-16445 (partial),
 * R12/R13 FPREM and R14/R15 FPREM1 of the denormals 7 and 2 * 2^-16445.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0x8000000000000000",
    "RBX": "0x4200bfff",
    "RCX": "0x8000000000000000",
    "RDX": "0x01003fff",
    "RSI": "0xc000000000000000",
    "RDI": "0x03003fc1",
    "RBP": "0xfffffffa00000002",
    "R8": "0x0400401e",
    "R10": "0x8000000000000000",
    "R11": "0x04023fc3",
    "R12": "0x1",
    "R13": "0x42020000",
    "R14": "0x1",
    "R15": "0x01028000"
  }
}
*/
	.text
	.globl _start
_start:
	movl $0x20000000, %r9d
	.macro rem operation, dividend, divisor, slot
	fninit
	fldt \divisor(%rip)
	fldt \dividend(%rip)
	\operation
	fnstsw %ax
	andl $0x47ff, %eax
	shll $16, %eax
	fstpt 0x40(%r9)
	movw 0x48(%r9), %ax
	movl %eax, \slot+8(%r9)
	movq 0x40(%r9), %rcx
	movq %rcx, \slot(%r9)
	.endm
	rem fprem, minus_seven, two, 0x100
	rem fprem1, minus_seven, two, 0x110
	rem fprem, all_ones_63, one_plus, 0x120
	rem fprem, all_ones_64, one_plus, 0x130
	rem fprem, one, denormal_three, 0x140
	rem fprem, denormal_seven, denormal_two, 0x150
	rem fprem1, denormal_seven, denormal_two, 0x160
	fninit
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
minus_seven:
	.quad 0xe000000000000000
	.short 0xc001
	.p2align 4
two:
	.quad 0x8000000000000000
	.short 0x4000
	.p2align 4
all_ones_63:
	.quad 0xffffffffffffffff
	.short 0x403e
	.p2align 4
all_ones_64:
	.quad 0xffffffffffffffff
	.short 0x403f
	.p2align 4
one_plus:
	.quad 0x8000000000000001
	.short 0x3fff
	.p2align 4
one:
	.quad 0x8000000000000000
	.short 0x3fff
	.p2align 4
denormal_three:
	.quad 0x3
	.short 0x0000
	.p2align 4
denormal_seven:
	.quad 0x7
	.short 0x0000
	.p2align 4
denormal_two:
	.quad 0x2
	.short 0x0000
	.p2align 4
