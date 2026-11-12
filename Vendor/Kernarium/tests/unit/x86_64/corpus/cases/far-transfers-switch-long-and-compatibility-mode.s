/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Far transfers between 64-bit code (CS 0x33) and compatibility-mode code
 * (CS 0x23), and what compatibility mode alone decodes (XC1).
 *
 * An indirect far JMP through m16:32 enters 32-bit code, which runs opcode
 * 0x82 as group 1, LES and LDS (C4 and C5 with a memory operand), 32-bit
 * stack operations and a direct far CALL to 64-bit code that returns with a
 * 32-bit far RET, and leaves by a direct far JMP. The 64-bit code then makes
 * an indirect far CALL through m16:32 to 32-bit code, which returns with a
 * 32-bit far RET, a REX.W far CALL through m16:64 that returns with LRETQ,
 * and an IRETQ to 32-bit code, which comes back by a direct far JMP. R13
 * counts the 64-bit landings.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RBX": "0x14",
    "RCX": "0x23",
    "RDX": "0x12345678",
    "RSI": "0x20001000",
    "RDI": "0x2b",
    "R8": "0x14",
    "R9": "0x12345678",
    "R10": "0x12345678",
    "R11": "0x2b002b",
    "R12": "0x20000ff8",
    "R13": "0x6",
    "R14": "0x23",
    "R15": "0x20000ff8"
  }
}
*/
	.text
	.globl _start
	.code64
_start:
	movq $0x20001000, %rsp
	xorl %r13d, %r13d
	leaq compat_entry(%rip), %rax
	movl %eax, 0x20000100
	movw $0x23, 0x20000104
	ljmp *0x20000100

	.code32
compat_entry:
	movl $0x2b, %ecx
	movl %ecx, %ds
	movl %ecx, %es
	/* 0x82 is group 1 with an 8-bit immediate. */
	movl $0x10, %eax
	.byte 0x82, 0xc0, 0x05
	.byte 0x82, 0xe8, 0x01
	movl %eax, %ebx
	/* LES and LDS of a far pointer, which 64-bit mode decodes as VEX. */
	movl $0x20000110, %esi
	movl $0x12345678, (%esi)
	movw $0x2b, 4(%esi)
	xorl %ecx, %ecx
	movl %ecx, %es
	les (%esi), %edx
	lds (%esi), %edi
	/* 32-bit stack operations. */
	pushl $0x2b
	popl %ecx
	/* A direct far CALL to 64-bit code, and a direct far JMP back. */
	lcall $0x33, $long_callee
	ljmp $0x33, $long_resume

	.code64
long_callee:
	incl %r13d
	movl %ebx, %r8d
	movl %edx, %r9d
	movl %edi, %r10d
	movl %es, %eax
	shll $16, %eax
	movl %ds, %r11d
	orl %eax, %r11d
	movq %rsp, %r12
	movzwl 4(%rsp), %r14d
	lretl

long_resume:
	incl %r13d
	/* An indirect far CALL through m16:32 to 32-bit code. */
	leaq compat_callee(%rip), %rax
	movl %eax, 0x20000120
	movw $0x23, 0x20000124
	lcall *0x20000120
	incl %r13d
	movl 0x20000130, %r15d
	/* A REX.W far CALL through m16:64, which LRETQ returns from. */
	leaq long_far(%rip), %rax
	movq %rax, 0x20000140
	movw $0x33, 0x20000148
	.byte 0x48, 0xff, 0x1c, 0x25
	.long 0x20000140
	incl %r13d
	/* IRETQ to 32-bit code. */
	movq %rsp, %rax
	pushq $0x2b
	pushq %rax
	pushfq
	pushq $0x23
	leaq compat_iret(%rip), %rax
	pushq %rax
	iretq

long_far:
	incl %r13d
	lretq

finish:
	incl %r13d
	movl %esp, %esi
	movl %ds, %edi
	xorl %eax, %eax
	movl %eax, %ds
	movl %eax, %es
	hlt

	.code32
compat_callee:
	movl %esp, 0x20000130
	lret

compat_iret:
	movl %cs, %ecx
	ljmp $0x33, $finish
