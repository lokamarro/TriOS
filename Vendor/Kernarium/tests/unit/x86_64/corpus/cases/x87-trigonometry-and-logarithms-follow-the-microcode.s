/* SPDX-License-Identifier: GPL-2.0-or-later */
/* An Intel core reduces an operand of FSIN, FCOS and FPTAN by multiples of
 * pi/2 taken to 66 bits, so the sine of the 64-bit pi is not its distance
 * from pi but the function of a remainder from a slightly wrong one; the
 * tangent of an odd quadrant is minus the cotangent of the remainder.
 * FYL2X of a power of two below one is exact in 64 bits but x86 rounds it
 * up in magnitude, and truncates it down in a directed mode, and F2XM1 and
 * FYL2XP1 keep every bit of a tiny operand's result.
 *
 * Each result is a pair of registers, its significand, then the status
 * word shifted left by 16 over its sign and exponent: RAX/RBX FSIN of the
 * 64-bit pi, RCX/RDX FCOS of it, RSI/RDI FPTAN of pi/2 rounded to 64 bits,
 * RBP/R8 FYL2X of 0.25 and 1.0, R10/R11 the same rounding toward zero,
 * R12/R13 F2XM1 of 2^-70, R14/R15 FYL2XP1 of 2^-70 and 1.0.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0x8000000000000000",
    "RBX": "0x3a20bfbf",
    "RCX": "0x8000000000000000",
    "RDX": "0x3a20bfff",
    "RSI": "0x8000000000000000",
    "RDI": "0x3020c040",
    "RBP": "0x8000000000000000",
    "R8": "0x3a20c000",
    "R10": "0xffffffffffffffff",
    "R11": "0x3820bfff",
    "R12": "0xb17217f7d1cf79ac",
    "R13": "0x3a203fb8",
    "R14": "0xb8aa3b295c17f0bc",
    "R15": "0x3a203fb9"
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
	fldpi
	fsin
	capture 0, 0x100
	fninit
	fldpi
	fcos
	capture 0, 0x110
	fninit
	fldt pi_over_2(%rip)
	fptan
	capture 1, 0x120
	fninit
	fld1
	fldt quarter(%rip)
	fyl2x
	capture 0, 0x130
	fninit
	fldcw truncate(%rip)
	fld1
	fldt quarter(%rip)
	fyl2x
	capture 0, 0x140
	fninit
	fldt two_minus_70(%rip)
	f2xm1
	capture 0, 0x150
	fninit
	fld1
	fldt two_minus_70(%rip)
	fyl2xp1
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
pi_over_2:
	.quad 0xc90fdaa22168c235
	.short 0x3fff
	.p2align 4
quarter:
	.quad 0x8000000000000000
	.short 0x3ffd
	.p2align 4
two_minus_70:
	.quad 0x8000000000000000
	.short 0x3fb9
	.p2align 4
truncate:
	.short 0x0f7f
	.p2align 4
