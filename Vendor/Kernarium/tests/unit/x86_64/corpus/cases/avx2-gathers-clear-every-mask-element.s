/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A completed AVX2 gather clears its whole mask register, the elements it
 * did not read included: an element whose mask sign is clear keeps its
 * destination element but not its mask element.
 *
 * RAX and RCX: VPGATHERDD XMM0's destination, low and high; RBX and RDX
 * its mask, with element 0 0x7fffffff and element 3 0x00000005 unread.
 * RSI and RDI: VPGATHERQQ YMM3's elements 1 and 2, R8 and R10 its mask's
 * elements 0 and 3 (0x7fffffffffffffff and 1, unread), R11 the OR of its
 * whole mask.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x1000"},
  "RegData": {
    "RAX": "0xa0000002ffffffff",
    "RBX": "0x0",
    "RCX": "0xffffffffa0000003",
    "RDX": "0x0",
    "RSI": "0xb000000000000002",
    "RDI": "0xb000000000000003",
    "R8": "0x0",
    "R10": "0x0",
    "R11": "0x0"
  }
}
*/
	.text
	.globl _start
_start:
	movl $0x20000000, %r9d
	xorl %ecx, %ecx
1:	movl $0xa0000000, %eax
	orl %ecx, %eax
	movl %eax, (%r9,%rcx,4)
	incl %ecx
	cmpl $8, %ecx
	jb 1b
	movabsq $0xb000000000000000, %rax
	xorl %ecx, %ecx
2:	leaq (%rax,%rcx), %rdx
	movq %rdx, 0x100(%r9,%rcx,8)
	incl %ecx
	cmpl $8, %ecx
	jb 2b

	vmovdqu dd_mask(%rip), %xmm1
	vmovdqu dd_index(%rip), %xmm2
	vpcmpeqd %xmm0, %xmm0, %xmm0
	vpgatherdd %xmm1, (%r9,%xmm2,4), %xmm0
	vmovq %xmm0, %rax
	vpextrq $1, %xmm0, %rcx
	vmovq %xmm1, %rbx
	vpextrq $1, %xmm1, %rdx

	vmovdqu qq_mask(%rip), %ymm4
	vmovdqu qq_index(%rip), %ymm5
	vpcmpeqq %ymm3, %ymm3, %ymm3
	vpgatherqq %ymm4, 0x100(%r9,%ymm5,8), %ymm3
	vpextrq $1, %xmm3, %rsi
	vextracti128 $1, %ymm3, %xmm6
	vmovq %xmm6, %rdi
	vmovq %xmm4, %r8
	vextracti128 $1, %ymm4, %xmm7
	vpextrq $1, %xmm7, %r10
	vpor %xmm4, %xmm7, %xmm7
	vmovq %xmm7, %r11
	vpextrq $1, %xmm7, %r12
	orq %r12, %r11

	vzeroall
	xorl %r9d, %r9d
	xorl %r12d, %r12d
	hlt
	.p2align 5
dd_mask:
	.long 0x7fffffff, 0x80000001, 0x80000000, 0x00000005
dd_index:
	.long 1, 2, 3, 4
qq_mask:
	.quad 0x7fffffffffffffff, 0x8000000000000001, 0x8000000000000000, 1
qq_index:
	.quad 1, 2, 3, 4
