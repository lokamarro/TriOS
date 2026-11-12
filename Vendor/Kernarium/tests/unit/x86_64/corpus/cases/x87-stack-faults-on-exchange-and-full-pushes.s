/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FXCH with an empty register is a stack underflow. Masked, the empty
 * register reads as the QNaN indefinite and the exchange goes on; unmasked,
 * it changes no register and only the status word shows the fault. A push
 * onto a full stack is a stack overflow, which while masked pushes the QNaN
 * indefinite rather than the value. FXCH clears C1.
 *
 * RAX, RBX, RCX: masked FXCH of ST(1) empty, the doubles ST(0) and ST(1)
 * then hold; RSP an FXCH of two full registers, which clears C1. RDX, RSI,
 * RDI: unmasked, the status word, the tag word and the
 * double ST(0) holds. R8 and R10: the ninth FLD1's status word and the
 * double it pushed.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0x3841",
    "RBX": "0xfff8000000000000",
    "RCX": "0x3ff0000000000000",
    "RSP": "0x3000",
    "RDX": "0xb8c1",
    "RSI": "0x3fff",
    "RDI": "0x3ff0000000000000",
    "R8": "0x3a41",
    "R10": "0xfff8000000000000"
  }
}
*/
	.text
	.globl _start
_start:
	movl $0x20000000, %r9d
	fninit
	fld1
	fxch %st(1)
	fnstsw 0x80(%r9)
	fstpl (%r9)
	fstpl 8(%r9)
	movq (%r9), %rbx
	movq 8(%r9), %rcx

	fninit
	fld1
	fld1
	fnstenv 0x40(%r9)
	orw $0x0200, 0x44(%r9)
	fldenv 0x40(%r9)
	fxch %st(1)
	fnstsw 0x82(%r9)

	fninit
	fldcw unmask_invalid(%rip)
	fld1
	fxch %st(1)
	fnstsw %ax
	movzwl %ax, %edx
	fnstenv 0x40(%r9)
	movzwl 0x48(%r9), %esi
	fnclex
	fstpl (%r9)
	movq (%r9), %rdi

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
	movzwl %ax, %r8d
	fstpl (%r9)
	movq (%r9), %r10

	fninit
	movzwl 0x20000080, %eax
	movzwl 0x20000082, %esp
	hlt
unmask_invalid:
	.short 0x037e
