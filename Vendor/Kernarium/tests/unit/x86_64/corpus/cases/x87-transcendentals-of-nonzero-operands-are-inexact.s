/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A sine, cosine, tangent or arctangent of a finite nonzero operand is
 * never exact, though for a tiny operand it rounds to the operand itself
 * or to 1: x86 raises precision for it, and underflow for a denormal one,
 * with C1 clear, as on the Xeon 8581C.
 *
 * Each pair of registers holds a result's significand, then the status
 * word shifted left by 16 with the result's sign and exponent: RAX/RBX
 * FSIN and RCX/RDX FCOS of 2^-70, RSI/RDI FSIN of the denormal 3 *
 * 2^-16445, RBP/R8 FPTAN's tangent of 2^-70, R10/R11 FSINCOS's sine of
 * 2^-16000, R12/R13 FPATAN of 2^-70 over 1, R14/R15 FSIN of 2^-33.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0x8000000000000000",
    "RBX": "0x38203fb9",
    "RCX": "0x8000000000000000",
    "RDX": "0x38203fff",
    "RSI": "0x3",
    "RDI": "0x38320000",
    "RBP": "0x8000000000000000",
    "R8": "0x30203fb9",
    "R10": "0x8000000000000000",
    "R11": "0x3020017f",
    "R12": "0x8000000000000000",
    "R13": "0x38203fb9",
    "R14": "0x8000000000000000",
    "R15": "0x3a203fde"
  }
}
*/
	.text
	.globl _start
_start:
	movl $0x20000000, %r9d
	/* FNSAVE stores the status word and the registers in stack order
	 * without waiting, and reinitializes. */
	.macro capture index, slot
	fnsave 0x40(%r9)
	movzwl 0x44(%r9), %eax
	shll $16, %eax
	movw 0x40 + 28 + 10 * \index + 8(%r9), %ax
	movl %eax, \slot+8(%r9)
	movq 0x40 + 28 + 10 * \index(%r9), %rcx
	movq %rcx, \slot(%r9)
	.endm
	fninit
	fldt two_minus_70(%rip)
	fsin
	capture 0, 0x100
	fninit
	fldt two_minus_70(%rip)
	fcos
	capture 0, 0x110
	fninit
	fldt denormal_three(%rip)
	fsin
	capture 0, 0x120
	fninit
	fldt two_minus_70(%rip)
	fptan
	capture 1, 0x130
	fninit
	fldt two_minus_16000(%rip)
	fsincos
	capture 1, 0x140
	fninit
	fldt two_minus_70(%rip)
	fld1
	fpatan
	capture 0, 0x150
	fninit
	fldt two_minus_33(%rip)
	fsin
	capture 0, 0x160
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
two_minus_70:
	.quad 0x8000000000000000
	.short 0x3fb9
	.p2align 4
denormal_three:
	.quad 0x3
	.short 0x0000
	.p2align 4
two_minus_16000:
	.quad 0x8000000000000000
	.short 0x017f
	.p2align 4
two_minus_33:
	.quad 0x8000000000000000
	.short 0x3fde
	.p2align 4
