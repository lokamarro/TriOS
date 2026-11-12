/* SPDX-License-Identifier: GPL-2.0-or-later */
/* With 0x67 in 64-bit mode, ModR/M mod=00 rm=101 is EIP-relative (audit
 * NC-X3).
 *
 * The displacement counts from the end of the instruction, as it does
 * without the prefix, and the sum is a 32-bit address. It is an absolute
 * disp32 only in 32-bit mode. The second slot is reached through a sum that
 * wraps below zero, which only the truncation brings back to 0xffffxxxx.
 */
/* CONFIG
{
  "MemoryRegions": {"0xffff0000": "0x10000"},
  "RegData": {
    "RAX": "0x1122334455667788",
    "RBX": "0x99aabbccddeeff00",
    "RCX": "0x0",
    "RDX": "0x0"
  }
}
*/
	.text
	.globl _start
_start:
	movabsq $0x1122334455667788, %rsi
	movq %rsi, slot(%rip)
	/* The wrapped slot is the low 32 bits of slot - 0x20000. */
	leaq slot(%rip), %r8
	subq $0x20000, %r8
	movl %r8d, %r8d
	movabsq $0x99aabbccddeeff00, %rdi
	movq %rdi, (%r8)
	movq slot(%eip), %rax
	movq (slot - 0x20000)(%eip), %rbx
	leaq slot(%eip), %rcx
	leaq slot(%rip), %r9
	subq %r9, %rcx
	leaq (slot - 0x20000)(%eip), %rdx
	subq %r8, %rdx
	hlt

	/* HLT bytes until the case stores the value it loads back. */
	.balign 8, 0xf4
slot:
	.fill 8, 1, 0xf4
