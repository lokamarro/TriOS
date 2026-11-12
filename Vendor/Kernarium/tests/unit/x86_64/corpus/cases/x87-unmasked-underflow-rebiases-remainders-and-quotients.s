/* SPDX-License-Identifier: GPL-2.0-or-later */
/* x87 rebiases every register result that underflows or overflows while
 * the control word unmasks the exception: a remainder below the smallest
 * normal, which FPREM and FPREM1 compute exactly, and a quotient, as on
 * the Xeon 8581C.
 *
 * Each pair of registers holds ST0's significand, then the status word
 * shifted left by 16 with ST0's sign and exponent, after: RAX/RBX FPREM of
 * the denormals 7 and 2 * 2^-16445; RCX/RDX FPREM1 of the denormal 5 *
 * 2^-16445 by 1; RBP/R8 FDIV of 1.5 * 2^-16382 by 1.5 * 2^16383,
 * underflow unmasked; R10/R11 the other way round, overflow unmasked.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0x8000000000000000",
    "RBX": "0xf2925fc2",
    "RCX": "0xa000000000000000",
    "RDX": "0xb0925fc4",
    "RBP": "0x8000000000000000",
    "R8": "0xb0902002",
    "R10": "0x8000000000000000",
    "R11": "0xb0885ffc"
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
	x87case unmask_underflow, denormal_seven, denormal_two, "fprem", 0x100
	x87case unmask_underflow, denormal_five, one, "fprem1", 0x110
	x87case unmask_underflow, tiny, big, "fdiv %st(1), %st(0)", 0x130
	x87case unmask_overflow, big, tiny, "fdiv %st(1), %st(0)", 0x140
	movq 0x100(%r9), %rax
	movl 0x108(%r9), %ebx
	movq 0x110(%r9), %rcx
	movl 0x118(%r9), %edx
	movq 0x130(%r9), %rbp
	movl 0x138(%r9), %r8d
	movq 0x140(%r9), %r10
	movl 0x148(%r9), %r11d
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
denormal_two:
	.quad 0x2
	.short 0x0000
	.p2align 4
denormal_five:
	.quad 0x5
	.short 0x0000
	.p2align 4
denormal_seven:
	.quad 0x7
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
