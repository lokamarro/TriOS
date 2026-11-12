/* SPDX-License-Identifier: GPL-2.0-or-later */
/* RCP and RSQRT ignore MXCSR: a denormal input counts as zero, so its
 * estimate is an infinity, RCP's estimate of 2^126 or more is a zero of the
 * input's sign, and RSQRT of a negative input is the indefinite.
 */
/* CONFIG
{
  "RegData": {
    "XMM0": ["0x0000000000000000", "0x0000000000000000"],
    "XMM2": ["0xff8000007f800000", "0x8000000000000000"],
    "XMM4": ["0xff8000007f800000", "0xff800000ffc00000"],
    "XMM5": ["0x000000007f800000", "0x0000000000000000"],
    "XMM7": ["0xff8000007f800000", "0x0000000000000000",
             "0x8000000000000000", "0xff8000007f800000"],
    "XMM8": ["0xff8000007f800000", "0xffc00000ffc00000",
             "0x7f80000000000000", "0xffc00000ff800000"]
  }
}
*/
	.text
	.globl _start
_start:
	movss largest(%rip), %xmm0
	rcpss %xmm0, %xmm0
	movups reciprocal_inputs(%rip), %xmm1
	rcpps %xmm1, %xmm2
	movups root_inputs(%rip), %xmm3
	rsqrtps %xmm3, %xmm4
	movss root_inputs(%rip), %xmm5
	rsqrtss %xmm5, %xmm5
	vmovups wide_reciprocal_inputs(%rip), %ymm6
	vrcpps %ymm6, %ymm7
	vrsqrtps wide_root_inputs(%rip), %ymm8
	hlt

	.balign 16
largest:
	.long 0x7f7fffff
	.balign 16
reciprocal_inputs:
	.long 0x00000001, 0x807fffff, 0x7f000000, 0xfec00000
root_inputs:
	.long 0x00000001, 0x80000000, 0xbf800000, 0x807fffff
wide_reciprocal_inputs:
	.long 0x00000001, 0x807fffff, 0x7f000000, 0x7f7fffff
	.long 0x7f800000, 0xff800000, 0x00000000, 0x80000000
wide_root_inputs:
	.long 0x00000001, 0x807fffff, 0xbf800000, 0xff800000
	.long 0x7f800000, 0x00000000, 0x80000000, 0xff7fffff
