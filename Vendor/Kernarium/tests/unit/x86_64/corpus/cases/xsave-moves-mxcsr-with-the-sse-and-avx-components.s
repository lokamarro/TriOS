/* SPDX-License-Identifier: GPL-2.0-or-later */
/* XRSTOR loads MXCSR from the image whenever RFBM selects the SSE or the AVX
 * component, whatever XSTATE_BV says, and leaves it alone when RFBM selects
 * neither. XSAVE and XSAVEOPT store MXCSR and MXCSR_MASK under the same
 * condition. XSAVE keeps the XSTATE_BV bits outside RFBM and the header's
 * reserved bytes as it found them.
 *
 * MXCSR is 0x5e41 before each XRSTOR, and the image holds 0x3fa5. RAX:
 * STMXCSR after RFBM 2 with XSTATE_BV 0; RBX: after RFBM 4 with XSTATE_BV 0;
 * RCX: after RFBM 1 with XSTATE_BV 7; RDX: after RFBM 6 with XSTATE_BV 4, and
 * RSI and RDI the low half of XMM0 and the high half of YMM0 it leaves
 * (initialized, then the image's). R8: XMM0's low half after RFBM 4 with
 * XSTATE_BV 4, which leaves XMM0 alone.
 *
 * XSAVE with RFBM 4 into an image of 0x55 bytes, its MXCSR field 0xaa: R9
 * and R10 the MXCSR and
 * MXCSR_MASK it stores; R11 the MXCSR field after RFBM 1, which stores
 * none. R12: XSTATE_BV after RFBM 2 into a header holding 0xff05, with SSE
 * state in use. R13: the header quadword at 520 after that XSAVE. RBP: the
 * MXCSR field XSAVEOPT stores for RFBM 4.
 */
/* CONFIG
{
  "MemoryRegions": {"0x20000000": "0x3000"},
  "RegData": {
    "RAX": "0x3fa5",
    "RBX": "0x3fa5",
    "RCX": "0x5e41",
    "RDX": "0x3fa5",
    "RSI": "0x0",
    "RDI": "0x2222222222222222",
    "R8": "0x9999999999999999",
    "R9": "0x5e41",
    "R10": "0xffff",
    "R11": "0xaaaaaaaa",
    "R12": "0xff07",
    "R13": "0x55555555aaaaaaaa",
    "RBP": "0x5e41"
  }
}
*/
	.macro setmxcsr value
	movl $\value, (%r15)
	ldmxcsr (%r15)
	.endm

	.macro getmxcsr slot
	stmxcsr (%r15)
	movl (%r15), %eax
	movq %rax, \slot(%r15)
	.endm

	/* An image of XMM0 0x1111, YMM0's high half 0x2222, MXCSR 0x3fa5 and a
	 * clear header. */
	.macro image
	xorl %eax, %eax
	movl $0x180, %ecx
	movq %r14, %rdi
	rep stosq
	movl $0x3fa5, 24(%r14)
	movl $0xffff, 28(%r14)
	movabsq $0x1111111111111111, %rax
	movq %rax, 160(%r14)
	movabsq $0x2222222222222222, %rax
	movq %rax, 576(%r14)
	.endm

	/* The whole image as 0x55 bytes, but for its MXCSR field's 0xaa. */
	.macro fill
	movabsq $0x5555555555555555, %rax
	movl $0x180, %ecx
	movq %r14, %rdi
	rep stosq
	movabsq $0xaaaaaaaaaaaaaaaa, %rax
	movq %rax, 24(%r14)
	.endm

	/* XMM0 0x9999 and YMM0's high half 0x8888, in use. */
	.macro vectors
	movabsq $0x9999999999999999, %rax
	movq %rax, 0(%r15)
	movq %rax, 8(%r15)
	movabsq $0x8888888888888888, %rax
	movq %rax, 16(%r15)
	movq %rax, 24(%r15)
	vmovdqu (%r15), %ymm0
	.endm

	.text
	.globl _start
_start:
	movl $0x20000000, %r14d
	movl $0x20002000, %r15d

	/* RFBM 2, XSTATE_BV 0. */
	image
	setmxcsr 0x5e41
	movl $2, %eax
	xorl %edx, %edx
	xrstor (%r14)
	getmxcsr 0x100

	/* RFBM 4, XSTATE_BV 0. */
	image
	setmxcsr 0x5e41
	movl $4, %eax
	xorl %edx, %edx
	xrstor (%r14)
	getmxcsr 0x108

	/* RFBM 1, XSTATE_BV 7. */
	image
	movq $7, 512(%r14)
	setmxcsr 0x5e41
	movl $1, %eax
	xorl %edx, %edx
	xrstor (%r14)
	getmxcsr 0x110

	/* RFBM 6, XSTATE_BV 4. */
	image
	movq $4, 512(%r14)
	setmxcsr 0x5e41
	vectors
	movl $6, %eax
	xorl %edx, %edx
	xrstor (%r14)
	getmxcsr 0x118
	movq %xmm0, 0x120(%r15)
	vextractf128 $1, %ymm0, %xmm1
	movq %xmm1, 0x128(%r15)

	/* RFBM 4, XSTATE_BV 4: XMM0 stays. */
	image
	movq $4, 512(%r14)
	vectors
	movl $4, %eax
	xorl %edx, %edx
	xrstor (%r14)
	movq %xmm0, 0x130(%r15)

	/* XSAVE, RFBM 4. */
	fill
	setmxcsr 0x5e41
	vectors
	movl $4, %eax
	xorl %edx, %edx
	xsave (%r14)
	movl 24(%r14), %eax
	movq %rax, 0x138(%r15)
	movl 28(%r14), %eax
	movq %rax, 0x140(%r15)

	/* XSAVE, RFBM 1. */
	fill
	movl $1, %eax
	xorl %edx, %edx
	xsave (%r14)
	movl 24(%r14), %eax
	movq %rax, 0x148(%r15)

	/* XSAVE, RFBM 2, over a header of 0xff05 and a marked reserved
	 * quadword. */
	fill
	movq $0xff05, 512(%r14)
	movabsq $0x55555555aaaaaaaa, %rax
	movq %rax, 520(%r14)
	vectors
	movl $2, %eax
	xorl %edx, %edx
	xsave (%r14)
	movq 512(%r14), %rax
	movq %rax, 0x150(%r15)
	movq 520(%r14), %rax
	movq %rax, 0x158(%r15)

	/* XSAVEOPT, RFBM 4. */
	fill
	vectors
	movl $4, %eax
	xorl %edx, %edx
	xsaveopt (%r14)
	movl 24(%r14), %eax
	movq %rax, 0x160(%r15)

	setmxcsr 0x1f80
	movq 0x100(%r15), %rax
	movq 0x108(%r15), %rbx
	movq 0x110(%r15), %rcx
	movq 0x118(%r15), %rdx
	movq 0x120(%r15), %rsi
	movq 0x128(%r15), %rdi
	movq 0x130(%r15), %r8
	movq 0x138(%r15), %r9
	movq 0x140(%r15), %r10
	movq 0x148(%r15), %r11
	movq 0x150(%r15), %r12
	movq 0x158(%r15), %r13
	movq 0x160(%r15), %rbp
	hlt
