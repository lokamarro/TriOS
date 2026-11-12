/* SPDX-License-Identifier: GPL-2.0-or-later */
/* An unmasked x87 exception is reported at the next waiting instruction;
 * the non-waiting FNSTSW and FNSTENV read what the faulting one left. An
 * invalid operation or denormal operand leaves the registers, the stack and
 * memory as they were and raises none of a result's exceptions, but a
 * comparison still sets its condition codes, and a stack fault C1. A
 * store that overflows or underflows stores nothing and raises no
 * precision.
 *
 * Status words, with RBX-R15's tag words after them where they differ:
 * RAX FST m32 overflowing, RBX FSTP m32 underflowing, RCX FISTP m32 out of
 * range with RDX the memory both left; RSI FCOMP of a denormal and RDI its
 * tag word; R8 FADDP of a denormal and R10 its tag word; R11 FCOMP of an
 * empty register; R12 FUCOMP of an SNaN and R13 its tag word; R14 FRNDINT
 * of an SNaN and R15 the significand it left.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0xb888",
    "RBX": "0xb890",
    "RCX": "0xb881",
    "RDX": "0x12345678",
    "RSI": "0xb182",
    "RDI": "0x2fff",
    "R8": "0xb082",
    "R10": "0x8fff",
    "R11": "0xfdc1",
    "R12": "0xf581",
    "R13": "0x8fff",
    "R14": "0xb881",
    "R15": "0xa000000000000000"
  }
}
*/
	.text
	.globl _start
_start:
	movl $0x20000000, %r9d
	movl $0x12345678, (%r9)

	fninit
	fldcw unmask_overflow(%rip)
	fldl huge(%rip)
	fsts (%r9)
	fnstsw 0x80(%r9)

	fninit
	fldcw unmask_underflow(%rip)
	fldl tiny(%rip)
	fstps (%r9)
	fnstsw %ax
	movzwl %ax, %ebx

	fninit
	fldcw unmask_invalid(%rip)
	fldl huge(%rip)
	fistpl (%r9)
	fnstsw %ax
	movzwl %ax, %ecx
	movl (%r9), %edx

	fninit
	fldcw unmask_denormal(%rip)
	fldt one(%rip)
	fldt denormal(%rip)
	fcomp %st(1)
	fnstsw %ax
	movzwl %ax, %esi
	fnstenv 0x40(%r9)
	movzwl 0x48(%r9), %edi

	fninit
	fldcw unmask_denormal(%rip)
	fldt denormal(%rip)
	fld1
	faddp
	fnstsw %ax
	movzwl %ax, %r8d
	fnstenv 0x40(%r9)
	movzwl 0x48(%r9), %r10d

	fninit
	fldcw unmask_invalid(%rip)
	fld1
	fcomp %st(2)
	fnstsw %ax
	movzwl %ax, %r11d

	fninit
	fldcw unmask_invalid(%rip)
	fldt signaling_nan(%rip)
	fld1
	fucomp %st(1)
	fnstsw %ax
	movzwl %ax, %r12d
	fnstenv 0x40(%r9)
	movzwl 0x48(%r9), %r13d

	fninit
	fldcw unmask_invalid(%rip)
	fldt signaling_nan(%rip)
	frndint
	fnstsw %ax
	movzwl %ax, %r14d
	fnclex
	fstpt 0x10(%r9)
	movq 0x10(%r9), %r15

	fninit
	movzwl 0x80(%r9), %eax
	xorl %r9d, %r9d
	hlt
	.p2align 4
one:
	.quad 0x8000000000000000
	.short 0x3fff
	.p2align 4
denormal:
	.quad 1
	.short 0
	.p2align 4
signaling_nan:
	.quad 0xa000000000000000
	.short 0x7fff
	.p2align 3
huge:
	.quad 0x7e37e43c8800759c
tiny:
	.quad 0x01a56e1fc2f8f359
unmask_invalid:
	.short 0x037e
unmask_denormal:
	.short 0x037d
unmask_overflow:
	.short 0x0377
unmask_underflow:
	.short 0x036f
