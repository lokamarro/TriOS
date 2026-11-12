/* SPDX-License-Identifier: GPL-2.0-or-later */
/* An x87 read of an empty register is a stack underflow, and a push onto a
 * full one a stack overflow: either raises invalid operation with the stack
 * fault flag, C1 telling overflow from underflow, and while the control word
 * masks it the operation goes on with the QNaN indefinite.
 *
 * R10: FADD of two empty registers. RBX: the ninth push. RCX and RDX: FST
 * of an empty ST(0), and the single it stores. RSI and RDI: FLD of an empty
 * ST(3), and the double it pushed. R8: FCOM against an empty ST(2).
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "R10": "0x41",
    "RBX": "0x3a41",
    "RCX": "0x41",
    "RDX": "0xffc00000",
    "RSI": "0x3841",
    "RDI": "0xfff8000000000000",
    "R8": "0x7d41"
  }
}
*/
	.text
	.globl _start
_start:
	movl $0x20000000, %r9d
	fninit
	fadd %st(1), %st(0)
	fnstsw %ax
	movzwl %ax, %r10d

	fninit
	fld1
	fld1
	fld1
	fld1
	fld1
	fld1
	fld1
	fld1
	fld1
	fnstsw %ax
	movzwl %ax, %ebx

	fninit
	fsts (%r9)
	fnstsw %ax
	movzwl %ax, %ecx
	movl (%r9), %edx

	fninit
	fld %st(3)
	fnstsw %ax
	movzwl %ax, %esi
	fstpl 8(%r9)
	movq 8(%r9), %rdi

	fninit
	fld1
	fcom %st(2)
	fnstsw %ax
	movzwl %ax, %r8d
	fninit
	hlt
