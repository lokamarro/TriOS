/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Legacy prefixes decode whatever their order, and the ones a form cannot
 * use are ignored: a doubled operand-size prefix on MOVD, a REX.W that
 * PINSRW, PEXTRW and CRC32 byte forms leave unused for their operation, and
 * REP/REPE, operand-size and address-size prefixes in the order that
 * assemblers do not choose. The FEX programs that held these encodings are
 * assembled without them by NASM 3, which drops a redundant prefix and
 * orders the others its own way, so this case spells each byte.
 *
 * XMM0, RBX, XMM3: MOVD with 66 66 from a GPR, to a GPR, to memory and back.
 * XMM4, R12: PINSRW and PEXTRW with REX.W. R8, R9: CRC32 r64, r/m8 from a
 * register and from memory. R10, R11, R13: F3 66 STOSW, then MOVSW. RBP: F3
 * 66 LODSW. R14, R15: F3 66 SCASW stopping at the fourth word with its
 * flags and pointer. RAX, RSI, RCX: F3 67 REX.W LODSQ through ESI and ECX,
 * and RDI with RCX: 66 67 LEA of a 32-bit address into CX.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0xfedcba9876543210",
    "RBX": "0xcafebabe",
    "RCX": "0xffffffffffffabcd",
    "RSI": "0x20000310",
    "RDI": "0x1234abcd",
    "RBP": "0x7788",
    "R8": "0x3e2fbccf",
    "R9": "0x7baedb01",
    "R10": "0x106",
    "R11": "0x206",
    "R12": "0x5556",
    "R13": "0x7788",
    "R14": "0x4",
    "R15": "0x108",
    "XMM0": ["0x0000000011223344", "0x0000000000000000"],
    "XMM3": ["0x00000000a5a5a5a5", "0x0000000000000000"],
    "XMM4": ["0x3344434445464748", "0x5152535455565758"]
  }
}
*/
	.text
	.globl _start
_start:
	movl $0x20000000, %edx
	movl $0x20000f00, %esp
	movabsq $0x0123456789abcdef, %rax
	movq %rax, 0x300(%rdx)
	movabsq $0xfedcba9876543210, %rax
	movq %rax, 0x308(%rdx)

	/* MOVD with the operand-size prefix twice. */
	movabsq $0xdeadbeef11223344, %rax
	.byte 0x66, 0x66, 0x0f, 0x6e, 0xc0		/* movd %eax, %xmm0 */
	movabsq $0xdeadbeefcafebabe, %rcx
	movq %rcx, %xmm1
	movq $-1, %rbx
	.byte 0x66, 0x66, 0x0f, 0x7e, 0xcb		/* movd %xmm1, %ebx */
	movl $0xa5a5a5a5, %esi
	movd %esi, %xmm2
	.byte 0x66, 0x66, 0x0f, 0x7e, 0x52, 0x40	/* movd %xmm2, 0x40(%rdx) */
	.byte 0x66, 0x66, 0x0f, 0x6e, 0x5a, 0x40	/* movd 0x40(%rdx), %xmm3 */

	/* PINSRW and PEXTRW with REX.W: the word moves, the width is the same. */
	movabsq $0x4142434445464748, %r10
	movabsq $0x5152535455565758, %r11
	movq %r10, %xmm4
	movq %r11, %xmm5
	punpcklqdq %xmm5, %xmm4
	.byte 0x66, 0x48, 0x0f, 0xc4, 0xe0, 0x03	/* pinsrw $3, %rax, %xmm4 */
	.byte 0x66, 0x4c, 0x0f, 0xc5, 0xe4, 0x05	/* pextrw $5, %xmm4, %r12 */

	/* CRC32 r64, r/m8 with REX.W, from CL and from memory. */
	movl $0xffffffff, %eax
	movl $0x61, %ecx
	.byte 0xf2, 0x48, 0x0f, 0x38, 0xf0, 0xc1	/* crc32b %cl, %rax */
	movq %rax, %r8
	movb $0x62, 0x10(%rdx)
	movl $0x12345678, %ecx
	.byte 0xf2, 0x48, 0x0f, 0x38, 0xf0, 0x4a, 0x10	/* crc32b 0x10(%rdx), %rcx */
	movq %rcx, %r9

	/* REP before the operand-size prefix. */
	cld
	leaq 0x100(%rdx), %rdi
	movl $3, %ecx
	movabsq $0x1122334455667788, %rax
	.byte 0xf3, 0x66, 0xab				/* rep stosw */
	movq %rdi, %r10
	subq %rdx, %r10
	leaq 0x100(%rdx), %rsi
	leaq 0x200(%rdx), %rdi
	movl $3, %ecx
	.byte 0xf3, 0x66, 0xa5				/* rep movsw */
	movq %rdi, %r11
	subq %rdx, %r11
	movzwl 0x204(%rdx), %r13d
	leaq 0x200(%rdx), %rsi
	movl $3, %ecx
	xorl %eax, %eax
	.byte 0xf3, 0x66, 0xad				/* rep lodsw */
	movq %rax, %rbp
	leaq 0x100(%rdx), %rdi
	movl $4, %ecx
	movabsq $0x1122334455667788, %rax
	.byte 0xf3, 0x66, 0xaf				/* repe scasw */
	pushfq
	popq %r14
	andl $0x8d5, %r14d
	movq %rdi, %r15
	subq %rdx, %r15

	/* REP, then the address-size prefix, then REX.W. */
	movl $0x20000300, %esi
	movl $2, %ecx
	.byte 0xf3, 0x67, 0x48, 0xad			/* rep lodsq with ESI and ECX */

	/* The operand-size prefix before the address-size prefix. */
	movq $-1, %rcx
	movl $0x1234abcd, %edi
	.byte 0x66, 0x67, 0x8d, 0x0f			/* lea (%edi), %cx */
	hlt
