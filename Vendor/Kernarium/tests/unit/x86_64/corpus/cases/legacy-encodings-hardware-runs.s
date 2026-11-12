/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Encodings that x86 runs whatever a reserved field holds: SETcc and
 * 0F 1F ignore ModRM.reg, which for 0F 1F also leaves its memory operand
 * unread; F6 and F7 /1 are TEST; MOVZX and MOVSX from a word to a word
 * move it; SFENCE ignores ModRM.rm. XSAVEOPT saves what XRSTOR restores,
 * SGDT stores a zero limit and the base Linux reports, and LSL of a
 * selector user code may not use clears ZF and writes nothing.
 *
 * RAX: SETB with reg 7 into AL over all ones. RBX: SETNE with reg 3 into
 * BL. RCX: F6 /1 of CL with 0x80, its flags. RDX: F7 /1 of R14D with
 * 0x00010000, its flags. RSI: 66 0F B7 of a memory word over all ones.
 * RDI: 66 0F BF likewise. R8: 0F 1F /5 with an unmapped operand, then the
 * number it was. R10: XMM0 after XSAVEOPT, a clear and XRSTOR. R11 and
 * R12: SGDT's limit and base. RBP: LSL into EBP over all ones of the null
 * selector, then of the kernel's data selector, and R13 ZF after each.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0xffffffffffffff01",
    "RBX": "0x1",
    "RCX": "0x282",
    "RDX": "0x206",
    "RSI": "0xffffffffffff8001",
    "RDI": "0xffffffffffff8001",
    "R8": "0x1234",
    "R10": "0x1122334455667788",
    "R11": "0x0",
    "R12": "0xfffffffffffe0000",
    "RBP": "0xffffffffffffffff",
    "R13": "0x0"
  }
}
*/
	.text
	.globl _start
_start:
	movl $0x20000000, %r9d
	movl $0x20000f00, %esp

	movabsq $0x1122334455667788, %r10
	movq %r10, %xmm0
	movl $7, %eax
	xorl %edx, %edx
	.byte 0x41, 0x0f, 0xae, 0xb1, 0x00, 0x04, 0x00, 0x00	/* xsaveopt 0x400(%r9) */
	pxor %xmm0, %xmm0
	xrstor 0x400(%r9)
	movq %xmm0, %r10
	pxor %xmm0, %xmm0

	movq $-1, %rax
	stc
	.byte 0x0f, 0x92, 0xf8		/* setb %al, ModRM.reg 7 */
	movl $1, %ebx
	cmpl $2, %ebx
	movl $0, %ebx
	.byte 0x0f, 0x95, 0xdb		/* setne %bl, ModRM.reg 3 */

	movl $0x80, %ecx
	.byte 0xf6, 0xc9, 0x80		/* test $0x80, %cl as F6 /1 */
	pushfq
	popq %rcx
	andl $0x8d5, %ecx
	orl $0x202, %ecx
	movl $0x00010000, %r14d
	.byte 0x41, 0xf7, 0xce, 0x00, 0x00, 0x01, 0x00	/* test as F7 /1 */
	pushfq
	popq %rdx
	andl $0x8d5, %edx
	orl $0x202, %edx

	movw $0x8001, 0x80(%r9)
	movq $-1, %rsi
	.byte 0x66, 0x41, 0x0f, 0xb7, 0xb1, 0x80, 0x00, 0x00, 0x00	/* movzww */
	movq $-1, %rdi
	.byte 0x66, 0x41, 0x0f, 0xbf, 0xb9, 0x80, 0x00, 0x00, 0x00	/* movsww */

	movl $0x1234, %r8d
	movl $0x30000000, %r13d
	.byte 0x41, 0x0f, 0x1f, 0x6d, 0x00	/* nop with ModRM.reg 5 at (%r13) */
	.byte 0x0f, 0xae, 0xfb		/* sfence with ModRM.rm 3 */

	sgdt 0x100(%r9)
	movzwl 0x100(%r9), %r11d
	movq 0x102(%r9), %r12

	movq $-1, %rbp
	xorl %r15d, %r15d
	cmpl %r15d, %r15d
	.byte 0x41, 0x0f, 0x03, 0xef	/* lsl %r15w, %ebp */
	setz %r13b
	movzbl %r13b, %r13d
	movl $0x18, %r15d
	cmpl %r15d, %r15d
	.byte 0x41, 0x0f, 0x03, 0xef
	setz %r14b
	orb %r14b, %r13b
	xorl %r15d, %r15d

	xorl %r9d, %r9d
	xorl %r14d, %r14d
	xorl %esp, %esp
	hlt
