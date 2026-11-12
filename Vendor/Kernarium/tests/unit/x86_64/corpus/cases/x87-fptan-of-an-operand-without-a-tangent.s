/* SPDX-License-Identifier: GPL-2.0-or-later */
/* FPTAN and FSINCOS push a second result: 1.0 after a tangent, the cosine
 * after a sine. For an operand without a tangent or a sine, an infinity or
 * a NaN, an Intel core pushes the same NaN as the first result, the
 * indefinite for an infinity and the operand quieted for a NaN, and raises
 * invalid operation for the infinity alone. An operand of 2^63 or more is
 * out of range: C2 is set and nothing changes.
 *
 * Each operation is a pair of registers. The first holds ST(1)'s sign and
 * exponent in bits 48-63 over the tag word shifted left by 32, the status
 * word shifted left by 16 and ST(0)'s sign and exponent, the second ST(0)'s
 * significand xor ST(1)'s rotated left by one: RAX/RBX FPTAN of +infinity,
 * RCX/RDX of a QNaN with payload 5, RSI/RDI of 2^63, RBP/R8 FSINCOS of
 * +infinity, R10/R11 of the same QNaN.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0xffffafff3001ffff",
    "RBX": "0x4000000000000001",
    "RCX": "0x7fffafff30007fff",
    "RDX": "0x400000000000000e",
    "RSI": "0x3fff3c00403e",
    "RDI": "0x8000000000000000",
    "RBP": "0xffffafff3001ffff",
    "R8": "0x4000000000000001",
    "R10": "0x7fffafff30007fff",
    "R11": "0x400000000000000e"
  }
}
*/
	.text
	.globl _start
_start:
	movl $0x20000000, %r9d
	/* FNSAVE stores the environment and the registers in stack order
	 * without waiting, and reinitializes. */
	.macro capture_pair slot
	fnsave 0x40(%r9)
	movzwl 0x48(%r9), %eax
	shlq $16, %rax
	movzwl 0x44(%r9), %ecx
	orq %rcx, %rax
	shlq $16, %rax
	movzwl 0x40 + 28 + 8(%r9), %ecx
	orq %rcx, %rax
	movzwl 0x40 + 28 + 18(%r9), %ecx
	shlq $48, %rcx
	orq %rcx, %rax
	movq %rax, \slot(%r9)
	movq 0x40 + 28 + 10(%r9), %rcx
	rolq $1, %rcx
	xorq 0x40 + 28(%r9), %rcx
	movq %rcx, \slot+8(%r9)
	.endm
	fninit
	fldt plus_infinity(%rip)
	fptan
	capture_pair 0x100
	fninit
	fldt quiet_nan_five(%rip)
	fptan
	capture_pair 0x110
	fninit
	fldt two_to_the_63(%rip)
	fptan
	capture_pair 0x120
	fninit
	fldt plus_infinity(%rip)
	fsincos
	capture_pair 0x130
	fninit
	fldt quiet_nan_five(%rip)
	fsincos
	capture_pair 0x140
	movq 0x100(%r9), %rax
	movq 0x108(%r9), %rbx
	movq 0x110(%r9), %rcx
	movq 0x118(%r9), %rdx
	movq 0x120(%r9), %rsi
	movq 0x128(%r9), %rdi
	movq 0x130(%r9), %rbp
	movq 0x138(%r9), %r8
	movq 0x140(%r9), %r10
	movq 0x148(%r9), %r11
	xorl %r9d, %r9d
	hlt
	.p2align 4
plus_infinity:
	.quad 0x8000000000000000
	.short 0x7fff
	.p2align 4
quiet_nan_five:
	.quad 0xc000000000000005
	.short 0x7fff
	.p2align 4
two_to_the_63:
	.quad 0x8000000000000000
	.short 0x403e
	.p2align 4
