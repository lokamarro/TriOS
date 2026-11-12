/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FNSAVE stores the registers in stack order, ST(0) first, and FRSTOR loads
 * them back the same way, whatever TOP is. FNSTENV and FNSAVE compute the
 * tag of each register that is not empty from what it holds: 00 valid, 01
 * zero, 10 special. An MMX instruction tags every register valid and writes
 * all ones over its register's exponent, so the image tags that register
 * special and the zeroed rest zero; FXSAVE's abridged tag calls all eight
 * valid.
 *
 * RAX: FNSAVE's tag word, RBX and RCX ST(0)'s significand and exponent, RDX
 * ST(1)'s significand, RSI ST(2)'s, RDI its status word. R8 and R10: the
 * doubles FRSTOR's ST(0) and ST(1) store. R11: FNSTENV's tag word after an
 * MMX write to MM1 beside FRSTOR's 3.0 and 5.0 and zeros, R12 FXSAVE's
 * abridged tag, R13 the tag word after EMMS.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0x13ff",
    "RBX": "0xc90fdaa22168c235",
    "RCX": "0x4000",
    "RDX": "0x0",
    "RSI": "0x8000000000000000",
    "RDI": "0x2800",
    "R8": "0x4008000000000000",
    "R10": "0x4014000000000000",
    "R11": "0x0559",
    "R12": "0xff",
    "R13": "0xffff"
  }
}
*/
	.text
	.globl _start
_start:
	movl $0x20000000, %r9d
	fninit
	fld1
	fldz
	fldpi
	fnsave (%r9)
	movzwl 8(%r9), %eax
	movq 28(%r9), %rbx
	movzwl 36(%r9), %ecx
	movq 38(%r9), %rdx
	movq 48(%r9), %rsi
	movzwl 4(%r9), %edi

	/* An image with TOP 6, ST(0) 3.0 and ST(1) 5.0. */
	leaq 0x200(%r9), %r14
	movl $0x037f, (%r14)
	movl $0x3000, 4(%r14)
	movl $0x0fff, 8(%r14)
	movq $0, 12(%r14)
	movq $0, 20(%r14)
	movabsq $0xc000000000000000, %r15
	movq %r15, 28(%r14)
	movw $0x4000, 36(%r14)
	movabsq $0xa000000000000000, %r15
	movq %r15, 38(%r14)
	movw $0x4001, 46(%r14)
	frstor (%r14)
	fstpl 0x100(%r9)
	fstpl 0x108(%r9)
	movq 0x100(%r9), %r8
	movq 0x108(%r9), %r10

	fninit
	movl $0x1234, %r15d
	movq %r15, %mm1
	fnstenv 0x300(%r9)
	movzwl 0x308(%r9), %r11d
	fxsave 0x400(%r9)
	movzbl 0x404(%r9), %r12d
	emms
	fnstenv 0x300(%r9)
	movzwl 0x308(%r9), %r13d
	fninit
	hlt
