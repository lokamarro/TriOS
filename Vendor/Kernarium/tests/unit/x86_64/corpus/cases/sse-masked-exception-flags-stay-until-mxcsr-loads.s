/* SPDX-License-Identifier: GPL-2.0-or-later */
/* With every exception masked, SIMD floating-point instructions leave their
 * exception flags in MXCSR, and the flags stay until MXCSR is loaded: across
 * instructions, a taken branch and a system call. STMXCSR, FXSAVE and XSAVE
 * show them without clearing them, LDMXCSR, FXRSTOR and XRSTOR replace them,
 * and x87 arithmetic raises none of them.
 *
 * Slot n of XMM12 to XMM15 (four per register, lowest first) holds:
 * 0 precision from a quotient, kept across a branch and time();
 * 1 division by zero; 2 invalid operation; 3 overflow and precision;
 * 4 underflow and precision; 5 precision from a sum; 6 division by zero,
 * then an exact sum, then overflow and precision; 7 the invalid operation
 * LDMXCSR loads, beside an exact sum; 8 division by zero replaced by
 * LDMXCSR; 9 FXSAVE's MXCSR after precision; 10 XSAVE's after invalid
 * operation; 11 and 12 division by zero replaced by FXRSTOR and by XRSTOR;
 * 13 x87 division, sine and stores; 14 precision from one lane of a packed
 * sum; 15 division by zero kept after FXSAVE.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x2000"},
  "RegData": {
    "XMM12": ["0x00001f8400001fa0", "0x00001fa800001f81"],
    "XMM13": ["0x00001fa000001fb0", "0x00001f8100001fac"],
    "XMM14": ["0x00001fa000001f80", "0x00001f8000001f81"],
    "XMM15": ["0x00001f8000001f80", "0x00001f8400001fa0"]
  }
}
*/
	.macro clear
	movl $0x1f80, 512(%r10)
	ldmxcsr 512(%r10)
	.endm

	.macro flags slot
	stmxcsr 0x100 + 4 * \slot(%r10)
	.endm

	.macro image slot
	movl 24(%r9), %eax
	movl %eax, 0x100 + 4 * \slot(%r10)
	.endm

	.macro divide_by_zero
	movss one_s(%rip), %xmm0
	xorps %xmm1, %xmm1
	divss %xmm1, %xmm0
	.endm

	.text
	.globl _start
_start:
	movl $0x20000000, %r9d
	movl $0x20001000, %r10d

	clear
	movsd one_d(%rip), %xmm0
	divsd three_d(%rip), %xmm0
	jmp 1f
	ud2
1:	movl $201, %eax
	xorl %edi, %edi
	syscall
	flags 0

	clear
	divide_by_zero
	flags 1

	clear
	sqrtsd minus_one_d(%rip), %xmm0
	flags 2

	clear
	movsd largest_d(%rip), %xmm0
	mulsd two_d(%rip), %xmm0
	flags 3

	clear
	movss least_normal_s(%rip), %xmm0
	mulss third_s(%rip), %xmm0
	flags 4

	clear
	movsd one_d(%rip), %xmm0
	addsd tiny_d(%rip), %xmm0
	flags 5

	clear
	divide_by_zero
	movsd one_d(%rip), %xmm0
	addsd one_d(%rip), %xmm0
	movsd largest_d(%rip), %xmm0
	mulsd two_d(%rip), %xmm0
	flags 6

	movl $0x1f81, 512(%r10)
	ldmxcsr 512(%r10)
	movss one_s(%rip), %xmm0
	addss one_s(%rip), %xmm0
	flags 7

	clear
	divide_by_zero
	clear
	flags 8

	clear
	movsd one_d(%rip), %xmm0
	divsd three_d(%rip), %xmm0
	fxsave64 (%r9)
	image 9

	clear
	sqrtss minus_one_s(%rip), %xmm0
	movl $7, %eax
	xorl %edx, %edx
	xsave64 (%r9)
	image 10

	clear
	fxsave64 (%r9)
	divide_by_zero
	fxrstor64 (%r9)
	flags 11

	clear
	movl $7, %eax
	xorl %edx, %edx
	xsave64 (%r9)
	divide_by_zero
	movl $7, %eax
	xorl %edx, %edx
	xrstor64 (%r9)
	flags 12

	clear
	fld1
	fdivl three_d(%rip)
	fsin
	fstps 520(%r10)
	fldl largest_d(%rip)
	fstps 524(%r10)
	flags 13

	clear
	movups ones_s(%rip), %xmm0
	movups ones_and_tiny_s(%rip), %xmm1
	addps %xmm1, %xmm0
	flags 14

	clear
	divide_by_zero
	fxsave64 (%r9)
	flags 15

	clear
	movdqu 0x100(%r10), %xmm12
	movdqu 0x110(%r10), %xmm13
	movdqu 0x120(%r10), %xmm14
	movdqu 0x130(%r10), %xmm15
	hlt

	.balign 16
ones_s:
	.long 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000
ones_and_tiny_s:
	.long 0x3f800000, 0x3f800000, 0x3f800000, 0x30800000
one_d:
	.quad 0x3ff0000000000000
two_d:
	.quad 0x4000000000000000
three_d:
	.quad 0x4008000000000000
minus_one_d:
	.quad 0xbff0000000000000
largest_d:
	.quad 0x7fefffffffffffff
tiny_d:
	.quad 0x3c30000000000000
one_s:
	.long 0x3f800000
minus_one_s:
	.long 0xbf800000
least_normal_s:
	.long 0x00800000
third_s:
	.long 0x3eaaaaab
