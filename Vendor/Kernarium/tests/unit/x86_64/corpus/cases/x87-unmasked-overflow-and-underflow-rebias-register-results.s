/* SPDX-License-Identifier: GPL-2.0-or-later */
/* While the control word unmasks overflow or underflow, x87 delivers an
 * out-of-range result to a register with its significand rounded as if
 * the exponent were unbounded, at the control word's precision, and the
 * exponent brought back into range by 24576 (0x6000): down for an
 * overflow, up for an underflow, which an exact tiny result raises too.
 * C1 and precision follow that rounding, as on the Xeon 8581C.
 *
 * Each pair of registers holds ST0's significand, then the status word
 * shifted left by 16 with ST0's sign and exponent, after: RAX/RBX FMUL of
 * 1.5 * 2^16383 by (1 + 2^-63) * 2^16369, overflow unmasked; RCX/RDX FADD
 * of 1.5 * 2^16383 to itself; RSI/RDI FMUL of 1.5 * 2^-16382 by
 * (1 + 2^-63) * 2^-16368, underflow unmasked; RBP/R8 FADD of the
 * denormals 3 and 1 * 2^-16445, exact; R10/R11 FMUL of the largest
 * finite value by 3 rounding toward zero; R12/R13 FMUL as RAX's at
 * single precision; R14/R15 FADD of 1 to the largest finite value
 * rounding up.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0xc000000000000002",
    "RBX": "0xb2a85fef",
    "RCX": "0xc000000000000000",
    "RDX": "0xb0881fff",
    "RSI": "0xc000000000000002",
    "RDI": "0xb2b02011",
    "RBP": "0x8000000000000000",
    "R8": "0xb0925fc4",
    "R10": "0xbfffffffffffffff",
    "R11": "0xb0a82000",
    "R12": "0xc000000000000000",
    "R13": "0xb0a85fef",
    "R14": "0x8000000000000000",
    "R15": "0xb2a81fff"
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
	x87case unmask_overflow, big, big_odd, "fmul %st(1), %st(0)", 0x100
	x87case unmask_overflow, big, big, "fadd %st(1), %st(0)", 0x110
	x87case unmask_underflow, tiny, tiny_odd, "fmul %st(1), %st(0)", 0x120
	x87case unmask_underflow, denormal_three, denormal_one, "fadd %st(1), %st(0)", 0x130
	x87case unmask_overflow_toward_zero, largest, three, "fmul %st(1), %st(0)", 0x140
	x87case unmask_overflow_single, big, big_odd, "fmul %st(1), %st(0)", 0x150
	x87case unmask_overflow_up, largest, one, "fadd %st(1), %st(0)", 0x160
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
