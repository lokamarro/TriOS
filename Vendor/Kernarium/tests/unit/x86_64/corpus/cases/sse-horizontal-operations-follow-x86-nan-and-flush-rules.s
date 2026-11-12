/* SPDX-License-Identifier: GPL-2.0-or-later */
/* HADD, HSUB and ADDSUB follow x86's NaN rules: of two NaN elements in one
 * pair, the lower is returned quieted; of two NaN operands, the first; an
 * invalid sum is the negative indefinite. MXCSR.DAZ makes denormal inputs
 * zero and MXCSR.FTZ flushes a denormal result.
 */
/* CONFIG
{
  "RegData": {
    "XMM0": ["0x7fc000037fc00001", "0x40a00000ffc00000"],
    "XMM2": ["0x7ff8000000000001", "0x7ff8000000000004"],
    "XMM4": ["0x0000000000000000", "0x404000007fc00006"],
    "XMM6": ["0x0000000000000000", "0x8000000000000000"],
    "XMM8": ["0x0010000000000000", "0x7ff8000000000008",
             "0x0000000000000000", "0x4008000000000000"]
  }
}
*/
	.text
	.globl _start
_start:
	movups hadd_left(%rip), %xmm0
	movups hadd_right(%rip), %xmm1
	haddps %xmm1, %xmm0
	movupd hsub_left(%rip), %xmm2
	movupd hsub_right(%rip), %xmm3
	hsubpd %xmm3, %xmm2
	ldmxcsr denormals_are_zero(%rip)
	movups addsub_left(%rip), %xmm4
	movups addsub_right(%rip), %xmm5
	addsubps %xmm5, %xmm4
	ldmxcsr flush_to_zero(%rip)
	movupd addsub_double_left(%rip), %xmm6
	movupd addsub_double_right(%rip), %xmm7
	addsubpd %xmm7, %xmm6
	ldmxcsr both(%rip)
	vmovupd wide_left(%rip), %ymm9
	vmovupd wide_right(%rip), %ymm10
	vhaddpd %ymm10, %ymm9, %ymm8
	hlt

	.balign 16
denormals_are_zero:
	.long 0x1fc0
flush_to_zero:
	.long 0x9f80
both:
	.long 0x9fc0
	.balign 16
hadd_left:
	.long 0x7fc00001, 0xff800002, 0x3f800000, 0x7f800003
hadd_right:
	.long 0x7f800000, 0xff800000, 0x40000000, 0x40400000
hsub_left:
	.quad 0x3ff0000000000000, 0x7ff0000000000001
hsub_right:
	.quad 0x7ff8000000000004, 0xfff0000000000005
addsub_left:
	.long 0x00400000, 0x00000005, 0x7fc00006, 0x3f800000
addsub_right:
	.long 0x00000000, 0x80000002, 0xff800007, 0x40000000
addsub_double_left:
	.quad 0x0010000000000000, 0x8010000000000000
addsub_double_right:
	.quad 0x0000000000000001, 0x0000000000000001
wide_left:
	.quad 0x0000000000000001, 0x0010000000000000
	.quad 0x0018000000000000, 0x8010000000000000
wide_right:
	.quad 0x7ff8000000000008, 0xfff0000000000009
	.quad 0x3ff0000000000000, 0x4000000000000000
