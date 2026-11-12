/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FPREM1 rounds a quotient between 1/2 and 1 to 1, so its remainder takes
 * the other sign; a remainder below the smallest normal is stored as a
 * denormal, exactly. A quiet NaN operand propagates without raising
 * invalid operation, and a pseudo-denormal dividend whose quotient is zero
 * comes back normalized. A partial remainder keeps the dividend's sign.
 *
 * Each pair of registers holds a result's significand, then its status
 * word's condition codes and flags shifted left by 16 with its sign and
 * exponent: RAX/RBX FPREM1 of 0.8 by 1.5, RCX/RDX FPREM and RSI/RDI FPREM1
 * of 1.5 by 1 times 2^-16382, RBP/R8 FPREM of a quiet NaN by 3, R10/R11
 * FPREM of 5 by a quiet NaN, R12/R13 FPREM of a pseudo-denormal by 1.5,
 * R14/R15 FPREM of -2^80 by 3 (partial).
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0xb333333333333333",
    "RBX": "0x0200bffe",
    "RCX": "0x4000000000000000",
    "RDX": "0x02000000",
    "RSI": "0x4000000000000000",
    "RDI": "0x40008000",
    "RBP": "0xc000000000000001",
    "R8": "0x00007fff",
    "R10": "0xc000000000000002",
    "R11": "0x00007fff",
    "R12": "0x8000000000000001",
    "R13": "0x00020001",
    "R14": "0x8000000000000000",
    "R15": "0x0400c01f"
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
	rem fprem1, four_fifths, one_and_half, 0x100
	rem fprem, tiny_three, tiny_two, 0x110
	rem fprem1, tiny_three, tiny_two, 0x120
	rem fprem, quiet_nan, three, 0x130
	rem fprem, five, other_nan, 0x140
	rem fprem, pseudo_denormal, one_and_half, 0x150
	rem fprem, minus_two_80, three, 0x160
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
four_fifths:
	.quad 0xcccccccccccccccd
	.short 0x3ffe
	.p2align 4
one_and_half:
	.quad 0xc000000000000000
	.short 0x3fff
	.p2align 4
tiny_three:
	.quad 0xc000000000000000
	.short 0x0001
	.p2align 4
tiny_two:
	.quad 0x8000000000000000
	.short 0x0001
	.p2align 4
quiet_nan:
	.quad 0xc000000000000001
	.short 0x7fff
	.p2align 4
three:
	.quad 0xc000000000000000
	.short 0x4000
	.p2align 4
five:
	.quad 0xa000000000000000
	.short 0x4001
	.p2align 4
other_nan:
	.quad 0xc000000000000002
	.short 0x7fff
	.p2align 4
pseudo_denormal:
	.quad 0x8000000000000001
	.short 0x0000
	.p2align 4
minus_two_80:
	.quad 0x8000000000000000
	.short 0xc04f
	.p2align 4
