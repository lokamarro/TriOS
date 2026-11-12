/* SPDX-License-Identifier: GPL-2.0-or-later */
/* MIN and MAX return one of their sources unchanged, so MXCSR.FTZ leaves a
 * denormal result alone.
 */
/* CONFIG
{
  "RegData": {
    "XMM0": ["0x0000000000000001", "0x0000000000000000"],
    "XMM1": ["0x8000000300400000", "0x807fffff00000005"],
    "XMM3": ["0x000000013f800000", "0x800000003f800000"],
    "XMM4": ["0x0000000000000001", "0x000fffffffffffff",
             "0x8000000000000005", "0x4000000000000000"],
    "XMM6": ["0x8000000000000009", "0x0000000000000000"]
  }
}
*/
	.text
	.globl _start
_start:
	ldmxcsr flush(%rip)
	movss tiny_single(%rip), %xmm0
	minss one_single(%rip), %xmm0
	movups packed_left(%rip), %xmm1
	movups packed_right(%rip), %xmm2
	movaps %xmm1, %xmm3
	minps %xmm2, %xmm1
	maxps %xmm2, %xmm3
	vmovupd wide_left(%rip), %ymm5
	vmaxpd wide_right(%rip), %ymm5, %ymm4
	vmovsd wide_left+16(%rip), %xmm7
	vmovsd wide_right+16(%rip), %xmm8
	vminsd %xmm8, %xmm7, %xmm6
	hlt

	.balign 16
flush:
	.long 0x9f80
tiny_single:
	.long 0x00000001
one_single:
	.long 0x3f800000
	.balign 16
packed_left:
	.long 0x00400000, 0x80000003, 0x3f800000, 0x807fffff
packed_right:
	.long 0x3f800000, 0x00000001, 0x00000005, 0x80000000
wide_left:
	.quad 0x0000000000000001, 0xbff0000000000000
	.quad 0x8000000000000005, 0x4000000000000000
wide_right:
	.quad 0x0000000000000000, 0x000fffffffffffff
	.quad 0x8000000000000009, 0x3ff0000000000000
