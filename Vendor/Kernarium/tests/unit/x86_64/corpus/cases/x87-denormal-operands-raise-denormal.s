/* SPDX-License-Identifier: GPL-2.0-or-later */
/* An arithmetic operand that is denormal or pseudo-denormal raises the
 * denormal operand exception, and so does a single or double denormal that
 * FLD or an arithmetic instruction converts; FLD of an 80-bit denormal, a
 * store and a sign change do not. Unmasked, it stops an arithmetic
 * instruction before its result, but FLD still loads its operand.
 *
 * Status words with C1 masked off: RAX FLD m32 denormal, RBX FADD of an
 * 80-bit denormal, RCX FSQRT of one, RDX FRNDINT, RSI FSCALE, RDI FST m32
 * of one, R8 FLD m80 of one with FCHS, R10 FMUL m64 denormal, R11 FCOM m32
 * denormal. Unmasked: R13 FLD m32 denormal and R14 the
 * double it loaded; R15 FADD m32 denormal and RBP the double ST(0) keeps.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0x3802",
    "RBX": "0x3022",
    "RCX": "0x3822",
    "RDX": "0x3822",
    "RSI": "0x3002",
    "RDI": "0x3830",
    "R8": "0x3800",
    "R10": "0x3002",
    "R11": "0x3002",
    "R13": "0xb882",
    "R14": "0x3800000000000000",
    "R15": "0xb882",
    "RBP": "0x3ff0000000000000"
  }
}
*/
	.text
	.globl _start
_start:
	movl $0x20000000, %r9d

	fninit
	flds single_denormal(%rip)
	fnstsw %ax
	andl $0xfdff, %eax
	movl %eax, %ebx
	movl %ebx, 0x80(%r9)

	fninit
	fldt denormal(%rip)
	fld1
	fadd %st(1), %st(0)
	fnstsw %ax
	andl $0xfdff, %eax
	movl %eax, %ebx

	fninit
	fldt denormal(%rip)
	fsqrt
	fnstsw %ax
	andl $0xfdff, %eax
	movl %eax, %ecx

	fninit
	fldt denormal(%rip)
	frndint
	fnstsw %ax
	andl $0xfdff, %eax
	movl %eax, %edx

	fninit
	fld1
	fldt denormal(%rip)
	fscale
	fnstsw %ax
	andl $0xfdff, %eax
	movl %eax, %esi

	fninit
	fldt denormal(%rip)
	fsts (%r9)
	fnstsw %ax
	andl $0xfdff, %eax
	movl %eax, %edi

	fninit
	fldt denormal(%rip)
	fchs
	fnstsw %ax
	andl $0xfdff, %eax
	movl %eax, %r8d

	fninit
	fld1
	fmull double_denormal(%rip)
	fld1
	fnstsw %ax
	andl $0xfdff, %eax
	movl %eax, %r10d

	fninit
	fld1
	fcoms single_denormal(%rip)
	fld1
	fnstsw %ax
	andl $0xfdff, %eax
	movl %eax, %r11d


	fninit
	fldcw unmask_denormal(%rip)
	flds single_denormal(%rip)
	fnstsw %ax
	movzwl %ax, %r13d
	fnclex
	fstpl (%r9)
	movq (%r9), %r14

	fninit
	fldcw unmask_denormal(%rip)
	fld1
	fadds single_denormal(%rip)
	fnstsw %ax
	movzwl %ax, %r15d
	fnclex
	fstpl (%r9)
	movq (%r9), %rbp

	fninit
	movl 0x80(%r9), %eax
	xorl %r9d, %r9d
	hlt
	.p2align 4
denormal:
	.quad 0x0000000000000001
	.short 0
single_denormal:
	.long 0x00400000
	.p2align 3
double_denormal:
	.quad 0x0008000000000000
unmask_denormal:
	.short 0x037d
