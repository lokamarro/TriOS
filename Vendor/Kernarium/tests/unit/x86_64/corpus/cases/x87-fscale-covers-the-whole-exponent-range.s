/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FSCALE adds ST(1), truncated, to ST(0)'s exponent and rounds once at
 * full precision, so a result in range is exact however large the scale;
 * a scale of zero leaves even a denormal alone. Unmasked, an overflow or
 * underflow rebiases by 24576 (0x6000), and a result still out of range
 * after that overflows to an infinity or becomes a zero, as on the Xeon
 * 8581C.
 *
 * Each pair of registers holds ST0's significand, then the status word
 * shifted left by 16 with ST0's sign and exponent, after FSCALE of: RAX/RBX
 * 1.5 * 2^-16382 by 20000, masked; RCX/RDX (1 + 2^-63) * 2^-16368 by -17,
 * a denormal losing a bit; RSI/RDI 1.5 * 2^16383 by 20000, overflow
 * unmasked; RBP/R8 the same by 24577, beyond the rebias; R10/R11 1.5 *
 * 2^-16382 by -24577, underflow unmasked; R12/R13 the denormal 3 *
 * 2^-16445 by zero and R14/R15 by one, underflow unmasked.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0xc000000000000000",
    "RBX": "0x30004e21",
    "RCX": "0x1000000000000000",
    "RDX": "0x30300000",
    "RSI": "0xc000000000000000",
    "RDI": "0xb0886e1e",
    "RBP": "0x8000000000000000",
    "R8": "0xb2a87fff",
    "R10": "0x0",
    "R11": "0xb0b00000",
    "R12": "0x3",
    "R13": "0x30020000",
    "R14": "0xc000000000000000",
    "R15": "0xb0925fc4"
  }
}
*/
	.text
	.globl _start
_start:
	movl $0x20000000, %r9d
	/* FNSAVE does not wait, so it stores what the instruction left beside
	 * the exception it leaves pending, and then reinitializes. */
	.macro x87case control, first, second, operation, slot
	fninit
	fldcw \control(%rip)
	fldt \second(%rip)
	fldt \first(%rip)
	\operation
	fnsave 0x40(%r9)
	movzwl 0x44(%r9), %eax
	shll $16, %eax
	movw 0x64(%r9), %ax
	movl %eax, \slot+8(%r9)
	movq 0x5c(%r9), %rcx
	movq %rcx, \slot(%r9)
	.endm
	x87case masked, tiny, twenty_thousand, "fscale", 0x100
	x87case masked, tiny_odd, minus_seventeen, "fscale", 0x110
	x87case unmask_overflow, big, twenty_thousand, "fscale", 0x120
	x87case unmask_overflow, big, rebias_plus_one, "fscale", 0x130
	x87case unmask_underflow, tiny, minus_rebias_plus_one, "fscale", 0x140
	x87case unmask_underflow, denormal_three, zero, "fscale", 0x150
	x87case unmask_underflow, denormal_three, one, "fscale", 0x160
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
big:
	.quad 0xc000000000000000
	.short 0x7ffe
	.p2align 4
big_odd:
	.quad 0x8000000000000001
	.short 0x7ff0
	.p2align 4
tiny:
	.quad 0xc000000000000000
	.short 0x0001
	.p2align 4
tiny_odd:
	.quad 0x8000000000000001
	.short 0x000f
	.p2align 4
denormal_three:
	.quad 0x3
	.short 0x0000
	.p2align 4
denormal_one:
	.quad 0x1
	.short 0x0000
	.p2align 4
largest:
	.quad 0xffffffffffffffff
	.short 0x7ffe
	.p2align 4
three:
	.quad 0xc000000000000000
	.short 0x4000
	.p2align 4
one:
	.quad 0x8000000000000000
	.short 0x3fff
	.p2align 4
twenty_thousand:
	.quad 0x9c40000000000000
	.short 0x400d
	.p2align 4
minus_seventeen:
	.quad 0x8800000000000000
	.short 0xc003
	.p2align 4
rebias_plus_one:
	.quad 0xc002000000000000
	.short 0x400d
	.p2align 4
minus_rebias_plus_one:
	.quad 0xc002000000000000
	.short 0xc00d
	.p2align 4
zero:
	.quad 0x0
	.short 0x0000
	.p2align 4
masked:
	.short 0x037f
unmask_overflow:
	.short 0x0377
unmask_underflow:
	.short 0x036f
unmask_overflow_toward_zero:
	.short 0x0f77
unmask_overflow_up:
	.short 0x0b77
unmask_overflow_single:
	.short 0x0077
