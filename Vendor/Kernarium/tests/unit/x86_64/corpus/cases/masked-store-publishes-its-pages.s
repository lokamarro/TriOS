/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A masked store publishes every page it writes (audit NC1).
 *
 * VPMASKMOVD translates each selected element on its own, but publishes the
 * store once. Its base is on this page and its only selected element is the
 * first dword of the next, which holds an already translated block. The
 * next page's code stamp must move, so the second call runs the rewritten
 * immediate rather than the cached translation.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x2000"},
  "RegData": {"RAX": "0x2", "RBX": "0x1"}
}
*/
	.text
	.globl _start
_start:
	movq $0x20002000, %rsp
	call patched
	movl %eax, %ebx
	/* Select dword 2 only; its bytes b8 02 00 00 make `movl $2, %eax`. */
	vpxor %xmm0, %xmm0, %xmm0
	vpxor %xmm1, %xmm1, %xmm1
	movl $0x2b8, %ecx
	vpinsrd $2, %ecx, %xmm0, %xmm0
	movl $0x80000000, %ecx
	vpinsrd $2, %ecx, %xmm1, %xmm1
	leaq patched-8(%rip), %rax
	vpmaskmovd %xmm0, %xmm1, (%rax)
	call patched
	hlt

	.balign 4096, 0xf4
patched:
	movl $1, %eax
	ret
