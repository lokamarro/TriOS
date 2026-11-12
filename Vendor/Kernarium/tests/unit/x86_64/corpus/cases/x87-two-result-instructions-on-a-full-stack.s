/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FPTAN, FSINCOS and FXTRACT check their second result's slot first: on a
 * full stack, masked, they compute nothing, leave the QNaN indefinite in
 * ST0, push another and raise only the stack fault, with C1 set.
 *
 * RAX, RBX, RCX: the status words of FPTAN, FSINCOS and FXTRACT of 0.75 on
 * a stack of eight. RDX and RSI: FPTAN's ST0 and ST1 significands; RDI and
 * R8 their exponents. R10 and R11: FXTRACT's ST0 and ST1 significands.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0x3a41",
    "RBX": "0x3a41",
    "RCX": "0x3a41",
    "RDX": "0xc000000000000000",
    "RSI": "0xc000000000000000",
    "RDI": "0xffff",
    "R8": "0xffff",
    "R10": "0xc000000000000000",
    "R11": "0xc000000000000000"
  }
}
*/
	.text
	.globl _start
_start:
	movl $0x20000000, %r9d
	movl $0x20000f00, %esp

	call fill
	fptan
	fnstsw 0x80(%r9)
	movzwl 0x80(%r9), %eax
	fstpt (%r9)
	fstpt 0x10(%r9)
	movq (%r9), %rdx
	movq 0x10(%r9), %rsi
	movzwl 8(%r9), %edi
	movzwl 0x18(%r9), %r8d

	call fill
	fsincos
	fnstsw 0x80(%r9)
	movzwl 0x80(%r9), %ebx

	call fill
	fxtract
	fnstsw 0x80(%r9)
	movzwl 0x80(%r9), %ecx
	fstpt (%r9)
	fstpt 0x10(%r9)
	movq (%r9), %r10
	movq 0x10(%r9), %r11

	fninit
	xorl %r9d, %r9d
	xorl %r12d, %r12d
	xorl %esp, %esp
	hlt

fill:
	fninit
	movl $8, %r12d
1:	fldl three_quarters(%rip)
	decl %r12d
	jnz 1b
	ret
	.p2align 3
three_quarters:
	.double 0.75
