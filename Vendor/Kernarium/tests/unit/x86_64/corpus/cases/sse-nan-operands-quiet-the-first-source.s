/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SSE and AVX arithmetic returns its first source quieted when that is a
 * NaN, even beside a signaling second source, else the second quieted, and
 * the negative indefinite for a NaN the operation generates.
 */
/* CONFIG
{
  "RegData": {
    "XMM0": ["0x7fc222227fc11111", "0xffc333337fc77777"],
    "XMM2": ["0x7fc222227fc11111", "0xffc333333f800000"],
    "XMM3": ["0x7ff8000000011111", "0xfff8000000000000"],
    "XMM5": ["0x7ff8000000011111", "0x0000000000000000"],
    "XMM8": ["0x7fc222227fc11111", "0xffc333337fc77777",
             "0xffc00000ffc00000", "0x7fcbbbbb7fc99999"],
    "XMM9": ["0x7ff8000000011111", "0x7ff8000000066666",
             "0xfff8000000000000", "0xfff8000000077777"]
  }
}
*/
	.text
	.globl _start
_start:
	movups singles_first(%rip), %xmm0
	movups singles_second(%rip), %xmm1
	addps %xmm1, %xmm0
	movups singles_first(%rip), %xmm2
	mulss singles_second(%rip), %xmm2
	movupd doubles_first(%rip), %xmm3
	movupd doubles_second(%rip), %xmm4
	subpd %xmm4, %xmm3
	movsd doubles_first(%rip), %xmm5
	divsd %xmm4, %xmm5
	vmovups wide_first(%rip), %ymm6
	vmovups wide_second(%rip), %ymm7
	vdivps %ymm7, %ymm6, %ymm8
	vmovupd wide_doubles_first(%rip), %ymm10
	vmulpd wide_doubles_second(%rip), %ymm10, %ymm9
	hlt

	.balign 16
singles_first:
	.long 0x7fc11111, 0x7fc22222, 0x3f800000, 0xffc33333
singles_second:
	.long 0x7f855555, 0x7f866666, 0x7f877777, 0x7fa00000
doubles_first:
	.quad 0x7ff8000000011111, 0x7ff0000000000000
doubles_second:
	.quad 0x7ff0000000055555, 0x7ff0000000000000
wide_first:
	.long 0x7fc11111, 0x7fc22222, 0x3f800000, 0xffc33333
	.long 0x00000000, 0x7f800000, 0x7f899999, 0x40000000
wide_second:
	.long 0x7f855555, 0x7f866666, 0x7f877777, 0x7fa00000
	.long 0x00000000, 0x7f800000, 0x7fcaaaaa, 0x7f8bbbbb
wide_doubles_first:
	.quad 0x7ff8000000011111, 0x3ff0000000000000
	.quad 0x7ff0000000000000, 0xfff0000000077777
wide_doubles_second:
	.quad 0x7ff0000000055555, 0x7ff0000000066666
	.quad 0x0000000000000000, 0x7ff80000000aaaaa
