/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A masked stack underflow answers with the QNaN indefinite and writes it to
 * the instruction's destination register, which becomes valid: FST ST(i),
 * FADD, FXCH, FSQRT, FSIN, FYL2X and FPTAN on an empty stack leave it in
 * the register they would have written, and an Intel core reports invalid
 * operation with the stack fault flag and C1 clear.
 *
 * Each pair of registers holds a register's significand, then its tag word
 * shifted left by 32, the status word shifted left by 16 and its sign and
 * exponent: RAX/RBX FST ST(1) of an empty ST(0), RCX/RDX FADD ST(0), ST(1)
 * of two empty registers, RSI/RDI FXCH ST(1)'s ST(0), RBP/R8 FSQRT, R10/R11
 * FSIN, R12/R13 FYL2X's result. FPTAN's two registers: R14 holds ST(1)'s
 * sign and exponent in bits 48-63 over what the others hold for ST(0),
 * R15 ST(0)'s significand xor ST(1)'s rotated left by one.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0xc000000000000000",
    "RBX": "0xfffb0041ffff",
    "RCX": "0xc000000000000000",
    "RDX": "0xfffe0041ffff",
    "RSI": "0xc000000000000000",
    "RDI": "0xfffa0041ffff",
    "RBP": "0xc000000000000000",
    "R8": "0xfffe0041ffff",
    "R10": "0xc000000000000000",
    "R11": "0xfffe0041ffff",
    "R12": "0xc000000000000000",
    "R13": "0xfffb0841ffff",
    "R14": "0xffffbffe3841ffff",
    "R15": "0x4000000000000001"
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
	fst %st(1)
	capture 1, 0x100
	fninit
	fadd %st(1), %st(0)
	capture 0, 0x110
	fninit
	fxch %st(1)
	capture 0, 0x120
	fninit
	fsqrt
	capture 0, 0x130
	fninit
	fsin
	capture 0, 0x140
	fninit
	fyl2x
	capture 0, 0x150
	fninit
	fptan
	fnsave 0x40(%r9)
	movzwl 0x48(%r9), %eax
	shlq $16, %rax
	movzwl 0x44(%r9), %ecx
	orq %rcx, %rax
	shlq $16, %rax
	movzwl 0x40 + 28 + 8(%r9), %ecx
	orq %rcx, %rax
	movzwl 0x40 + 28 + 18(%r9), %ecx
	shlq $48, %rcx
	orq %rcx, %rax
	movq %rax, 0x168(%r9)
	movq 0x40 + 28 + 10(%r9), %rcx
	rolq $1, %rcx
	xorq 0x40 + 28(%r9), %rcx
	movq %rcx, 0x178(%r9)
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
	movq 0x168(%r9), %r14
	movq 0x178(%r9), %r15
	xorl %r9d, %r9d
	hlt
