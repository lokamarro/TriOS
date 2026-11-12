/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Encodings x86 runs that both engines once refused, and what LSL and
 * SIDT give user code under Linux. The 0F 0D register forms are no-ops;
 * LFENCE, MFENCE and SFENCE ignore ModRM.rm; FNENI, FNDISI and FNSETPM are
 * no-ops; D9 D8+i is FSTP ST(i). LSL reads the flat user code and data
 * entries 4, 5 and 6, from a register or memory, and writes a word, or
 * the zero-extended doubleword, only with ZF set; an LDT selector or an
 * empty entry clears ZF and leaves the destination. SIDT stores the
 * limit and base of Linux's UMIP emulation.
 *
 * RAX: the 0F 0D forms over 0x1234. RBX: 1 after the fences. RCX: the x87
 * status word after FNENI, FNDISI and FNSETPM. RDX and RSI: the status
 * word after FSTP's alias of pi into ST(1) over 1, then ST0 as a double.
 * RDI: LSL of 0x2b into EDI over all ones. R8: LSL of 0x2b into R8W over
 * 0x1111222233334444. R10: LSL of 0x33 into R10 over all ones. R11: LSL
 * of the word 0x23 in memory into R11D. R12: ZF after LSL of 0x2b, 0x2f
 * (an LDT selector), 0x3b (empty) and 0x23 from memory, in bits 0 to 3.
 * R13: the destination 0x5555 after LSL of 0x2f. R14 and R15: SIDT's
 * limit and base.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0x1234",
    "RBX": "0x1",
    "RCX": "0x0",
    "RDX": "0x3800",
    "RSI": "0x400921fb54442d18",
    "RDI": "0xffffffff",
    "R8": "0x111122223333ffff",
    "R10": "0xffffffff",
    "R11": "0xffffffff",
    "R12": "0x9",
    "R13": "0x5555",
    "R14": "0x0",
    "R15": "0xffffffffffff0000"
  }
}
*/
	.text
	.globl _start
_start:
	movl $0x20000000, %r9d

	movl $0x1234, %eax
	.byte 0x0f, 0x0d, 0xc0
	.byte 0x0f, 0x0d, 0xc8
	.byte 0x0f, 0x0d, 0xd1
	.byte 0xf3, 0x0f, 0x0d, 0xc0
	.byte 0x66, 0x0f, 0x0d, 0xf8
	.byte 0xf2, 0x0f, 0x0d, 0xe8

	xorl %ebx, %ebx
	.byte 0x0f, 0xae, 0xe9		/* lfence, rm 1 */
	.byte 0x0f, 0xae, 0xef		/* lfence, rm 7 */
	.byte 0x0f, 0xae, 0xf1		/* mfence, rm 1 */
	.byte 0x0f, 0xae, 0xf7		/* mfence, rm 7 */
	.byte 0x0f, 0xae, 0xf9		/* sfence, rm 1 */
	movl $1, %ebx

	fninit
	.byte 0xdb, 0xe0		/* fneni */
	.byte 0xdb, 0xe1		/* fndisi */
	.byte 0xdb, 0xe4		/* fnsetpm */
	fnstsw %ax
	movzwl %ax, %ecx

	fld1
	fldpi
	.byte 0xd9, 0xd9		/* fstp %st(1), D9 D8+1 */
	fnstsw %ax
	movzwl %ax, %edx
	fstpl 0x40(%r9)
	movq 0x40(%r9), %rsi
	movl $0x1234, %eax

	xorl %r12d, %r12d
	movl $0x2b, %r15d
	movq $-1, %rdi
	.byte 0x41, 0x0f, 0x03, 0xff	/* lsl %r15w, %edi */
	setz %r12b
	movabsq $0x1111222233334444, %r8
	.byte 0x66, 0x45, 0x0f, 0x03, 0xc7	/* lsl %r15w, %r8w */
	movl $0x33, %r15d
	movq $-1, %r10
	.byte 0x4d, 0x0f, 0x03, 0xd7	/* lsl %r15w, %r10 */
	movl $0x2f, %r15d
	movl $0x5555, %r13d
	.byte 0x45, 0x0f, 0x03, 0xef	/* lsl %r15w, %r13d */
	setz %r14b
	shlb $1, %r14b
	orb %r14b, %r12b
	movl $0x3b, %r15d
	movl $0x5555, %r11d
	.byte 0x45, 0x0f, 0x03, 0xdf	/* lsl %r15w, %r11d */
	setz %r14b
	shlb $2, %r14b
	orb %r14b, %r12b
	movw $0x23, 0x80(%r9)
	movq $-1, %r11
	.byte 0x45, 0x0f, 0x03, 0x99, 0x80, 0x00, 0x00, 0x00	/* lsl 0x80(%r9), %r11d */
	setz %r14b
	shlb $3, %r14b
	orb %r14b, %r12b

	sidt 0x100(%r9)
	movzwl 0x100(%r9), %r14d
	movq 0x102(%r9), %r15

	xorl %r9d, %r9d
	hlt
