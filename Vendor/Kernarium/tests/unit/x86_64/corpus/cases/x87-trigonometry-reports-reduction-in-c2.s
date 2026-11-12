/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FSIN, FCOS, FPTAN and FSINCOS clear C2 for an operand they reduce and
 * leave C0 and C3 alone.
 *
 * Status words with C1 masked off, each after an FLDENV that sets C3, C2
 * and C0: RAX FSIN of 1, RBX FCOS of 1, RCX FPTAN of 1, RDX FSINCOS of 1.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0x7920",
    "RBX": "0x7920",
    "RCX": "0x7120",
    "RDX": "0x7120"
  }
}
*/
	.text
	.globl _start
_start:
	movl $0x20000000, %r9d

	.macro reduce operand, operation, register
	fninit
	fldl \operand(%rip)
	fnstenv 0x40(%r9)
	orw $0x4500, 0x44(%r9)
	fldenv 0x40(%r9)
	\operation
	fnstsw %ax
	andl $0xfdff, %eax
	movl %eax, \register
	.endm

	reduce one, fsin, 0x80(%r9)
	reduce one, fcos, %ebx
	reduce one, fptan, %ecx
	reduce one, fsincos, %edx
	fninit
	movl 0x80(%r9), %eax
	xorl %r9d, %r9d
	hlt
	.p2align 3
one:
	.quad 0x3ff0000000000000
