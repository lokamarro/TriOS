/* SPDX-License-Identifier: GPL-2.0-or-later */
/* At CPL 3, IRETQ loads only the flags POPF may load, plus RF (audit
 * NC-X4).
 *
 * IF, IOPL, VM, VIF and VIP keep their values and the reserved bits stay
 * fixed, however the popped image sets them. TF and RF stay clear in both
 * images: TF would trap after the next instruction, and PUSHF never stores
 * RF, so the flags it reads back cannot show whether RF was loaded.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {"RAX": "0x244ed7", "RBX": "0x202"}
}
*/
	.text
	.globl _start
_start:
	movq $0x20001000, %rsp
	movabsq $0xfffffffffffefeff, %rdi
	call iret_flags
	movq %rax, %rbx
	xorl %edi, %edi
	call iret_flags
	xchgq %rax, %rbx
	hlt

/* Returns in RAX what PUSHFQ stores after IRETQ has popped RDI as RFLAGS.
 * The popped image may set AC, so every stack access stays aligned until
 * POPFQ restores ordinary flags. */
iret_flags:
	movq %rsp, %rsi
	pushq $0x2b
	pushq %rsi
	pushq %rdi
	pushq $0x33
	leaq 1f(%rip), %rsi
	pushq %rsi
	iretq
1:	pushfq
	popq %rax
	pushq $0x202
	popfq
	ret
