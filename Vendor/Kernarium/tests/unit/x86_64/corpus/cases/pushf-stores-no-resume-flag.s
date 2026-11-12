/* SPDX-License-Identifier: GPL-2.0-or-later */
/* PUSHF stores RF and VM as zero, whatever RFLAGS holds.
 *
 * IRETQ loads RF from its image at CPL 3, so the flags PUSHFQ stores right
 * after it are where a stored RF would show.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {"RAX": "0x202"}
}
*/
	.text
	.globl _start
_start:
	movq $0x20001000, %rsp
	movq %rsp, %rsi
	pushq $0x2b
	pushq %rsi
	pushq $0x10000
	pushq $0x33
	leaq 1f(%rip), %rsi
	pushq %rsi
	iretq
1:	pushfq
	popq %rax
	hlt
