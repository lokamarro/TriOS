/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FXTRACT of a denormal gives the exponent and significand of its
 * normalized value, and a pseudo-denormal those it would have with
 * exponent 1, raising a denormal operand, as on the Xeon 8581C.
 *
 * Each triple of registers holds the significand's significand, then the
 * status word shifted left by 32 with the exponent's sign and exponent
 * shifted left by 16 and the significand's, then the exponent's
 * significand: RAX/RBX/RCX FXTRACT of the denormal 1 * 2^-16445, RDX/RSI/
 * RDI of the denormal 3 * 2^-16445, RBP/R8/R10 of the pseudo-denormal (1 +
 * 2^-63) * 2^-16382, R11/R12/R13 of -1.25 times the smallest normal.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0x8000000000000000",
    "RBX": "0x3002c00d3fff",
    "RCX": "0x807a000000000000",
    "RDX": "0xc000000000000000",
    "RSI": "0x3002c00d3fff",
    "RDI": "0x8078000000000000",
    "RBP": "0x8000000000000001",
    "R8": "0x3002c00c3fff",
    "R10": "0xfff8000000000000",
    "R11": "0xa000000000000000",
    "R12": "0x3000c00cbfff",
    "R13": "0xfff8000000000000"
  }
}
*/
	.text
	.globl _start
_start:
	movl $0x20000000, %r9d
	/* FNSAVE stores the status word and the registers in stack order
	 * without waiting, and reinitializes. */
	.macro extract operand, slot
	fninit
	fldt \operand(%rip)
	fxtract
	fnsave 0x40(%r9)
	movzwl 0x44(%r9), %eax
	shlq $32, %rax
	movzwl 0x40 + 28 + 10 + 8(%r9), %ecx
	shll $16, %ecx
	orq %rcx, %rax
	movw 0x40 + 28 + 8(%r9), %ax
	movq %rax, \slot+8(%r9)
	movq 0x40 + 28(%r9), %rcx
	movq %rcx, \slot(%r9)
	movq 0x40 + 28 + 10(%r9), %rcx
	movq %rcx, \slot+16(%r9)
	.endm
	extract denormal_one, 0x100
	extract denormal_three, 0x120
	extract pseudo_denormal, 0x140
	extract minus_small, 0x160
	movq 0x100(%r9), %rax
	movq 0x108(%r9), %rbx
	movq 0x110(%r9), %rcx
	movq 0x120(%r9), %rdx
	movq 0x128(%r9), %rsi
	movq 0x130(%r9), %rdi
	movq 0x140(%r9), %rbp
	movq 0x148(%r9), %r8
	movq 0x150(%r9), %r10
	movq 0x160(%r9), %r11
	movq 0x168(%r9), %r12
	movq 0x170(%r9), %r13
	xorl %r9d, %r9d
	hlt
	.p2align 4
denormal_one:
	.quad 0x1
	.short 0x0000
	.p2align 4
denormal_three:
	.quad 0x3
	.short 0x0000
	.p2align 4
pseudo_denormal:
	.quad 0x8000000000000001
	.short 0x0000
	.p2align 4
minus_small:
	.quad 0xa000000000000000
	.short 0x8001
