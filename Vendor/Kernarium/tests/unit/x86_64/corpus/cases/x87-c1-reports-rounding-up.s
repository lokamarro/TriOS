/* SPDX-License-Identifier: GPL-2.0-or-later */
/* After an arithmetic instruction, a store or a conversion to an integer,
 * C1 says whether the result was rounded up, away from zero, and a result
 * rounded down clears it. FIST, FISTP and FISTTP raise precision for a
 * value they round. FPTAN reports the tangent's rounding, beneath the 1 it
 * pushes, and FSINCOS the cosine's. Each register holds a status word.
 *
 * RAX: FDIV 1/3. RBX: FADD of 1/3 and the 2/3 left of 1. RCX: FST m32 of
 * 1/3. RDX: FST m64 of 1/3. RSI: FISTP m32 of 2.7. RDI: FISTP m32 of 2.5.
 * R8: FISTTP m64 of 2.7. R10: FRNDINT of 2.7. R11: FSQRT of 3. R12: FADD
 * of the largest finite value to itself, a masked overflow. R13: FPTAN of
 * 1, rounding up. R14: FSINCOS of 0.1. R15: FSINCOS of 2. RBP: FPTAN of 1,
 * rounding to nearest.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0x3a20",
    "RBX": "0x3a20",
    "RCX": "0x3a20",
    "RDX": "0x3820",
    "RSI": "0x0220",
    "RDI": "0x0020",
    "R8": "0x0020",
    "R10": "0x3a20",
    "R11": "0x3a20",
    "R12": "0x3a28",
    "R13": "0x3220",
    "R14": "0x3020",
    "R15": "0x3220",
    "RBP": "0x3020"
  }
}
*/
	.text
	.globl _start
_start:
	movl $0x20000000, %r9d

	fninit
	fld1
	fdivl three(%rip)
	fnstsw %ax
	movzwl %ax, %eax
	fld1
	fdivl three(%rip)
	fld1
	fsubp
	faddp
	fnstsw 0x80(%r9)
	movzwl 0x80(%r9), %ebx

	fninit
	fld1
	fdivl three(%rip)
	fsts (%r9)
	fnstsw 0x80(%r9)
	movzwl 0x80(%r9), %ecx
	fninit
	fld1
	fdivl three(%rip)
	fstl (%r9)
	fnstsw 0x80(%r9)
	movzwl 0x80(%r9), %edx

	fninit
	fldl two_point_seven(%rip)
	fistpl (%r9)
	fnstsw 0x80(%r9)
	movzwl 0x80(%r9), %esi
	fninit
	fld1
	fdivl three(%rip)
	fstp %st(0)
	fldl two_point_five(%rip)
	fistpl (%r9)
	fnstsw 0x80(%r9)
	movzwl 0x80(%r9), %edi
	fninit
	fldl two_point_seven(%rip)
	fisttpll (%r9)
	fnstsw 0x80(%r9)
	movzwl 0x80(%r9), %r8d

	fninit
	fldl two_point_seven(%rip)
	frndint
	fnstsw 0x80(%r9)
	movzwl 0x80(%r9), %r10d
	fninit
	fldl three(%rip)
	fsqrt
	fnstsw 0x80(%r9)
	movzwl 0x80(%r9), %r11d
	fninit
	fldt largest(%rip)
	fadd %st(0), %st(0)
	fnstsw 0x80(%r9)
	movzwl 0x80(%r9), %r12d

	fninit
	fldcw round_up(%rip)
	fld1
	fptan
	fnstsw 0x80(%r9)
	movzwl 0x80(%r9), %r13d
	fninit
	fld1
	fdivl three(%rip)
	fstp %st(0)
	fldl one_tenth(%rip)
	fsincos
	fnstsw 0x80(%r9)
	movzwl 0x80(%r9), %r14d
	fninit
	fldl two(%rip)
	fsincos
	fnstsw 0x80(%r9)
	movzwl 0x80(%r9), %r15d
	fninit
	fld1
	fptan
	fnstsw 0x80(%r9)
	movzwl 0x80(%r9), %ebp

	fninit
	xorl %r9d, %r9d
	hlt
	.p2align 4
largest:
	.quad 0xffffffffffffffff
	.short 0x7ffe
	.p2align 3
three:
	.double 3.0
two:
	.double 2.0
two_point_seven:
	.double 2.7
two_point_five:
	.double 2.5
one_tenth:
	.double 0.1
round_up:
	.short 0x0b7f
