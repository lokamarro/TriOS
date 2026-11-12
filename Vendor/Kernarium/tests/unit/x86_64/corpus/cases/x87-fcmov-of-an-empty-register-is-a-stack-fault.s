/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FCMOVcc reads ST(i) and ST(0) whichever way its condition goes, so an
 * empty register raises invalid operation with the stack fault flag, and
 * while that is masked leaves the QNaN indefinite in ST(0); with both
 * registers valid it moves or keeps by the condition and raises nothing.
 *
 * Each pair of registers holds ST(0)'s significand, then the tag word
 * shifted left by 32, the status word shifted left by 16 and ST(0)'s sign
 * and exponent: RAX/RBX FCMOVB with an empty ST(1) and CF set, RCX/RDX with
 * CF clear, RSI/RDI FCMOVB of two empty registers, RBP/R8 FCMOVE with a
 * freed ST(0) and ZF clear, R10/R11 FCMOVNB with valid registers and CF
 * set, which keeps ST(0), R12/R13 FCMOVB with valid registers and CF set,
 * which moves ST(1).
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0xc000000000000000",
    "RBX": "0xbfff3841ffff",
    "RCX": "0xc000000000000000",
    "RDX": "0xbfff3841ffff",
    "RSI": "0xc000000000000000",
    "RDI": "0xfffe0041ffff",
    "RBP": "0xc000000000000000",
    "R8": "0x2fff3041ffff",
    "R10": "0x8000000000000000",
    "R11": "0xfff30003fff",
    "R12": "0xb8aa3b295c17f0bc",
    "R13": "0xfff30003fff"
  }
}
*/
	.text
	.globl _start
_start:
	movl $0x20000000, %r9d
	/* FNSAVE stores the environment and the registers in stack order
	 * without waiting, and reinitializes. */
	.macro capture index, slot
	fnsave 0x40(%r9)
	movzwl 0x48(%r9), %eax
	shlq $16, %rax
	movzwl 0x44(%r9), %ecx
	orq %rcx, %rax
	shlq $16, %rax
	movzwl 0x40 + 28 + 10 * \index + 8(%r9), %ecx
	orq %rcx, %rax
	movq %rax, \slot+8(%r9)
	movq 0x40 + 28 + 10 * \index(%r9), %rcx
	movq %rcx, \slot(%r9)
	.endm
	fninit
	fld1
	stc
	fcmovb %st(1), %st(0)
	capture 0, 0x100
	fninit
	fld1
	clc
	fcmovb %st(1), %st(0)
	capture 0, 0x110
	fninit
	stc
	fcmovb %st(1), %st(0)
	capture 0, 0x120
	fninit
	fldl2e
	fld1
	ffree %st(0)
	xorl %eax, %eax
	cmpl $1, %eax
	fcmove %st(1), %st(0)
	capture 0, 0x130
	fninit
	fldl2e
	fld1
	stc
	fcmovnb %st(1), %st(0)
	capture 0, 0x140
	fninit
	fldl2e
	fld1
	stc
	fcmovb %st(1), %st(0)
	capture 0, 0x150
	movq 0x100(%r9), %rax
	movq 0x108(%r9), %rbx
	movq 0x110(%r9), %rcx
	movq 0x118(%r9), %rdx
	movq 0x120(%r9), %rsi
	movq 0x128(%r9), %rdi
	movq 0x130(%r9), %rbp
	movq 0x138(%r9), %r8
	movq 0x140(%r9), %r10
	movq 0x148(%r9), %r11
	movq 0x150(%r9), %r12
	movq 0x158(%r9), %r13
	xorl %r9d, %r9d
	hlt
