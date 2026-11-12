/* SPDX-License-Identifier: GPL-2.0-or-later */
/* DPPS and DPPD follow x86's NaN rules, and each result lane sums in its
 * own order, as x86 hardware does: of NaN products, DPPS lane i returns
 * p[i ^ 1] before p[i] and its own pair's before the other's, and DPPD lane
 * i returns p[i] before p[i ^ 1]. MXCSR.FTZ flushes each product or sum that
 * is tiny after rounding; MXCSR.DAZ makes denormal inputs, products and sums
 * zero. Unselected products are zero.
 */
/* CONFIG
{
  "RegData": {
    "XMM0": ["0x7fc000117fc00033", "0xffc00022ffc00022"],
    "XMM2": ["0x7ff8000000000044", "0xfff8000000000055"],
    "XMM4": ["0x0080000000800000", "0x0080000000800000"],
    "XMM6": ["0x0080000000800000", "0x0080000000800000"],
    "XMM8": ["0x0000000041200000", "0x0000000041200000",
             "0x000000007fc00033", "0x00000000ffc00022"],
    "XMM12": ["0x0010000000000000", "0x0000000000000000"]
  }
}
*/
	.text
	.globl _start
_start:
	movups nan_left(%rip), %xmm0
	movups nan_right(%rip), %xmm1
	dpps $0xff, %xmm1, %xmm0
	movupd nan_double_left(%rip), %xmm2
	movupd nan_double_right(%rip), %xmm3
	dppd $0x33, %xmm3, %xmm2
	vmovups wide_left(%rip), %ymm9
	vmovups wide_right(%rip), %ymm10
	vdpps $0xf5, %ymm10, %ymm9, %ymm8
	ldmxcsr flush_to_zero(%rip)
	movups flush_left(%rip), %xmm4
	movups flush_right(%rip), %xmm5
	dpps $0x7f, %xmm5, %xmm4
	movupd flush_double_left(%rip), %xmm12
	movupd flush_double_right(%rip), %xmm13
	dppd $0x31, %xmm13, %xmm12
	ldmxcsr denormals_are_zero(%rip)
	movups zero_left(%rip), %xmm6
	movups zero_right(%rip), %xmm7
	dpps $0x7f, %xmm7, %xmm6
	hlt

	.balign 16
flush_to_zero:
	.long 0x9f80
denormals_are_zero:
	.long 0x1fc0
	.balign 16
nan_left:
	.long 0x7fc00011, 0x7f800033, 0x3f800000, 0x3f800000
nan_right:
	.long 0x3f800000, 0x40400000, 0x3f800000, 0xffc00022
nan_double_left:
	.quad 0x7ff0000000000044, 0x4000000000000000
nan_double_right:
	.quad 0x3ff0000000000000, 0xfff8000000000055
wide_left:
	.long 0x3f800000, 0x40000000, 0x40400000, 0x40800000
	.long 0x7fc00011, 0x7f800033, 0x3f800000, 0x3f800000
wide_right:
	.long 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000
	.long 0x3f800000, 0x40400000, 0x3f800000, 0xffc00022
flush_left:
	.long 0x3f400006, 0x3f400003, 0x1c800000, 0x7f800001
flush_right:
	.long 0x00aaaaa5, 0x00aaaaa8, 0x21800000, 0x3f800000
flush_double_left:
	.quad 0x1ed0000000000000, 0x0010000000000000
flush_double_right:
	.quad 0x20b0000000000000, 0x3ff0000000000000
zero_left:
	.long 0x1c800000, 0x00800000, 0x00000001, 0x7f800001
zero_right:
	.long 0x21800000, 0x3f800000, 0x71800000, 0x3f800000
