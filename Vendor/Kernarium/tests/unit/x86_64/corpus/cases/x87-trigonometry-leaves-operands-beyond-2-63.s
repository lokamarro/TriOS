/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FSIN, FCOS, FPTAN and FSINCOS leave an operand of magnitude 2^63 or
 * more as it is, push nothing, set C2 and clear C1, which a division
 * before each case sets, as on the Xeon 8581C.
 *
 * Each pair of registers holds ST0's significand, then the status word
 * shifted left by 16 with ST0's sign and exponent: RAX/RBX FSIN of 2^63,
 * RCX/RDX FCOS of -1.5 * 2^70, RSI/RDI FPTAN of 2^64, RBP/R8 FSINCOS of
 * -2^63, R10/R11 FPTAN of the largest finite value.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0x8000000000000000",
    "RBX": "0x3c20403e",
    "RCX": "0xc000000000000000",
    "RDX": "0x3c20c045",
    "RSI": "0x8000000000000000",
    "RDI": "0x3c20403f",
    "RBP": "0x8000000000000000",
    "R8": "0x3c20c03e",
    "R10": "0xffffffffffffffff",
    "R11": "0x3c207ffe"
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
	fld1
	fdivl three(%rip)
	fstp %st(0)
	fldt two_63(%rip)
	fsin
	capture 0, 0x100
	fninit
	fld1
	fdivl three(%rip)
	fstp %st(0)
	fldt minus_big(%rip)
	fcos
	capture 0, 0x110
	fninit
	fld1
	fdivl three(%rip)
	fstp %st(0)
	fldt two_64(%rip)
	fptan
	capture 0, 0x120
	fninit
	fld1
	fdivl three(%rip)
	fstp %st(0)
	fldt minus_two_63(%rip)
	fsincos
	capture 0, 0x130
	fninit
	fld1
	fdivl three(%rip)
	fstp %st(0)
	fldt largest(%rip)
	fptan
	capture 0, 0x140
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
	xorl %r9d, %r9d
	hlt
	.p2align 4
two_63:
	.quad 0x8000000000000000
	.short 0x403e
	.p2align 4
minus_big:
	.quad 0xc000000000000000
	.short 0xc045
	.p2align 4
two_64:
	.quad 0x8000000000000000
	.short 0x403f
	.p2align 4
minus_two_63:
	.quad 0x8000000000000000
	.short 0xc03e
	.p2align 4
largest:
	.quad 0xffffffffffffffff
	.short 0x7ffe
	.p2align 4
	.p2align 3
three:
	.double 3.0
