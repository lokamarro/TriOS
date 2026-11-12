/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FCOM, FICOM, FCOMI and FTST raise invalid operation for any NaN; FUCOM
 * and FUCOMI only for a signaling one. Either reports unordered. A
 * denormal operand raises a denormal operand exception, unless a NaN makes
 * the compare invalid. Unmasked, an exception leaves the operands on the
 * stack, but the compare still sets C3, C2 and C0 or ZF, PF and CF.
 *
 * Status words: RAX FCOM QNaN, RBX FUCOM QNaN, RCX FUCOM SNaN, RDX FTST
 * QNaN, RSI FICOM QNaN, RDI FCOMI QNaN with its flags in R8, R10 FUCOMI
 * QNaN with its flags in R11, R12 FCOM of a denormal. Unmasked: R13 FCOMP
 * of a QNaN, R14 its tag word, R15 FCOMIP of a QNaN with its flags in RBP.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0x7501",
    "RBX": "0x7500",
    "RCX": "0x7501",
    "RDX": "0x7d01",
    "RSI": "0x7d01",
    "RDI": "0x3001",
    "R8": "0x247",
    "R10": "0x3000",
    "R11": "0x247",
    "R12": "0x3102",
    "R13": "0xf581",
    "R14": "0x2fff",
    "R15": "0xb081",
    "RBP": "0x247"
  }
}
*/
	.text
	.globl _start
_start:
	movl $0x20000000, %r9d
	movl $0x20000f00, %esp
	movl $1, (%r9)

	fninit
	fldt one(%rip)
	fldt quiet_nan(%rip)
	fcom %st(1)
	fnstsw 0x80(%r9)

	fninit
	fldt one(%rip)
	fldt quiet_nan(%rip)
	fucom %st(1)
	fnstsw %ax
	movzwl %ax, %ebx

	fninit
	fldt one(%rip)
	fldt signaling_nan(%rip)
	fucom %st(1)
	fnstsw %ax
	movzwl %ax, %ecx

	fninit
	fldt quiet_nan(%rip)
	ftst
	fnstsw %ax
	movzwl %ax, %edx

	fninit
	fldt quiet_nan(%rip)
	ficoml (%r9)
	fnstsw %ax
	movzwl %ax, %esi

	fninit
	fldt one(%rip)
	fldt quiet_nan(%rip)
	fcomi %st(1)
	pushfq
	popq %r8
	andl $0x8d5, %r8d
	orl $0x202, %r8d
	fnstsw %ax
	movzwl %ax, %edi

	fninit
	fldt one(%rip)
	fldt quiet_nan(%rip)
	fucomi %st(1)
	pushfq
	popq %r11
	andl $0x8d5, %r11d
	orl $0x202, %r11d
	fnstsw %ax
	movzwl %ax, %r10d

	fninit
	fldt one(%rip)
	fldt denormal(%rip)
	fcom %st(1)
	fnstsw %ax
	movzwl %ax, %r12d

	fninit
	fldcw unmask_invalid(%rip)
	fldt one(%rip)
	fldt quiet_nan(%rip)
	fcomp %st(1)
	fnstsw %ax
	movzwl %ax, %r13d
	fnstenv 0x40(%r9)
	movzwl 0x48(%r9), %r14d

	fninit
	fldcw unmask_invalid(%rip)
	fldt one(%rip)
	fldt quiet_nan(%rip)
	fcomip %st(1)
	pushfq
	popq %rbp
	andl $0x8d5, %ebp
	orl $0x202, %ebp
	fnstsw %ax
	movzwl %ax, %r15d
	fninit
	movzwl 0x80(%r9), %eax
	xorl %r9d, %r9d
	xorl %esp, %esp
	hlt
	.p2align 4
one:
	.quad 0x8000000000000000
	.short 0x3fff
	.p2align 4
quiet_nan:
	.quad 0xc000000000000000
	.short 0x7fff
	.p2align 4
signaling_nan:
	.quad 0xa000000000000000
	.short 0x7fff
	.p2align 4
denormal:
	.quad 1
	.short 0
unmask_invalid:
	.short 0x037e
