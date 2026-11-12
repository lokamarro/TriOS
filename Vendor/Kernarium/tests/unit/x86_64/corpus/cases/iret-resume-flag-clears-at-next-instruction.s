/* SPDX-License-Identifier: GPL-2.0-or-later */
/* RF, which IRETQ loads from its image at CPL 3, is cleared as the next
 * instruction starts (audit NC-E4).
 *
 * SYSCALL saves RFLAGS in R11, which makes it the CPL 3 instruction that
 * shows RF: PUSHF stores it as zero. RBX is R11 from a SYSCALL that is the
 * first instruction after IRETQ, and RDX from one an instruction later. Both
 * calls are time(NULL), which the unit runner answers itself, so RAX, RCX and
 * R11 stay undeclared.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {"RBX": "0x202", "RDX": "0x202"}
}
*/
	.text
	.globl _start
_start:
	movq $0x20001000, %rsp
	movl $201, %eax
	movq %rsp, %rsi
	pushq $0x2b
	pushq %rsi
	pushq $0x10202
	pushq $0x33
	leaq 1f(%rip), %rsi
	pushq %rsi
	iretq
1:	syscall
	movq %r11, %rbx
	movq %rsp, %rsi
	pushq $0x2b
	pushq %rsi
	pushq $0x10202
	pushq $0x33
	leaq 2f(%rip), %rsi
	pushq %rsi
	iretq
2:	movl $201, %eax
	syscall
	movq %r11, %rdx
	hlt
