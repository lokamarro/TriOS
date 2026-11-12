/* SPDX-License-Identifier: GPL-2.0-or-later */
/* 64-bit selector loads and segment inspection against the GDT Linux gives
 * every task (XC1).
 *
 * MOV, POP, LSS, LFS and LGS load data, readable code and null selectors;
 * LAR, LSL, VERR and VERW describe the user code and data segments, the
 * per-CPU segment 0x7b, whose limit is a CPU number and so goes unread, and
 * an unused entry; SLDT and STR read the LDT and
 * task registers with their 32-bit forms, which leave bits 63:32 alone.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RBX": "0x23002b",
    "RCX": "0x2b002b",
    "RDX": "0x3",
    "RSI": "0x20000ff0",
    "RDI": "0x2b",
    "R8": "0xcffb00",
    "R9": "0xcff300",
    "R10": "0xaffb00",
    "R11": "0xffffffff0040f500",
    "R12": "0xffffffffffffffff",
    "R13": "0x174",
    "R14": "0xffffffff00000040",
    "R15": "0xffffffff00000000"
  }
}
*/
	.text
	.globl _start
_start:
	movq $0x20001000, %rsp
	/* A data and a readable code selector into ES and DS. */
	movl $0x2b, %eax
	movl %eax, %es
	movl $0x23, %eax
	movl %eax, %ds
	movl %es, %ebx
	movl %ds, %ecx
	shll $16, %ecx
	orl %ecx, %ebx
	/* A null selector of any RPL loads into DS and keeps its RPL. */
	movl $3, %eax
	movl %eax, %ds
	movl %ds, %edx
	xorl %eax, %eax
	movl %eax, %ds
	movl %eax, %es
	/* MOV SS and LSS of the user data segment; LSS loads ESP. */
	movl $0x2b, %eax
	movl %eax, %ss
	movq %rsp, %rbp
	movl $0x20000ff0, 0x20000000
	movw $0x2b, 0x20000004
	lssl 0x20000000, %esp
	movl %esp, %esi
	movl %ss, %edi
	movq %rbp, %rsp
	/* LFS of a null selector and LGS of the data segment, POP FS. */
	movl $0x1234, 0x20000010
	movw $0, 0x20000014
	lfsl 0x20000010, %eax
	movl $0x5678, 0x20000020
	movw $0x2b, 0x20000024
	lgsl 0x20000020, %eax
	movl %gs, %ecx
	pushq $0x2b
	popq %fs
	movl %fs, %eax
	shll $16, %eax
	orl %eax, %ecx
	xorl %eax, %eax
	movl %eax, %gs
	pushq $0
	popq %fs
	/* LAR and LSL. */
	movl $0x23, %eax
	larl %eax, %r8d
	movl $0x2b, %eax
	larl %eax, %r9d
	movl $0x33, %eax
	larl %eax, %r10d
	movl $0x7b, %eax
	larl %eax, %r11d
	movl $0x23, %eax
	lsll %eax, %ebp
	shlq $32, %rbp
	orq %rbp, %r11
	movq $-1, %r12
	movl $0x3b, %eax
	larl %eax, %r12d
	/* VERR and VERW, one bit each, first in the highest. */
	xorl %r13d, %r13d
	movl $0x23, %eax
	call readable_writable
	movl $0x2b, %eax
	call readable_writable
	movl $0x33, %eax
	call readable_writable
	movl $0x7b, %eax
	call readable_writable
	movl $0x3b, %eax
	verr %ax
	setz %al
	movzbl %al, %eax
	shlq $1, %r13
	orq %rax, %r13
	/* SLDT and STR, 32-bit register forms. */
	movq $-1, %r14
	str %r14d
	movq $-1, %r15
	sldt %r15d
	xorl %eax, %eax
	hlt

/* Shifts VERR, then VERW, of the selector in AX into R13. */
readable_writable:
	movl %eax, %ebp
	verr %bp
	setz %al
	movzbl %al, %eax
	shlq $1, %r13
	orq %rax, %r13
	verw %bp
	setz %al
	movzbl %al, %eax
	shlq $1, %r13
	orq %rax, %r13
	ret
